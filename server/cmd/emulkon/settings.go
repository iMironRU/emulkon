package main

// Настройки сервера (docs/local-install.md §2.1): ключи командной строки → переменные окружения → реестр
// Windows (HKLM\SOFTWARE\Policies\Emulkon перекрывает HKLM\SOFTWARE\Emulkon\Server) → значения по умолчанию.
// Сервер кроссплатформенный: в Docker и CI реестра нет, работают ключи и окружение.

import (
	"flag"
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"time"

	"emulkon/internal/secret"
)

// Значения реестра (имена — как в документе).
const (
	regAddr         = "Addr"
	regNetwork      = "Network" // DWORD 1 — сервер для касс в сети: адрес по умолчанию 0.0.0.0:8737
	regDataDir      = "DataDir"
	regDownloadsDir = "DownloadsDir" // каталоги через «;»
	regLogLevel     = "LogLevel"
	regControlHash  = "ControlTokenHash"
	regDeviceHash   = "DeviceTokenHash"
)

const (
	defaultAddr        = "127.0.0.1:8737"
	defaultNetworkAddr = "0.0.0.0:8737"
)

// registryValues — строки и DWORD (как десятичная строка) из реестра; на других ОС — пусто.
type registryValues map[string]string

// settings — итоговые настройки и откуда взялось каждое значение (в лог при старте).
type settings struct {
	Addr          string
	DataDir       string
	DownloadsDirs []string
	LogLevel      string
	ControlToken  secret.Token
	DeviceToken   secret.Token
	Service       bool
	Sources       map[string]string // имя настройки → flag | env | registry | default

	DefaultTimeout      time.Duration
	SessionIdleTTL      time.Duration
	JournalMaxFileBytes int64
	JournalMaxFiles     int
}

type flags struct {
	set       map[string]bool
	addr      string
	data      string
	downloads string
	service   bool
	version   bool
}

func parseFlags(args []string) (flags, error) {
	var f flags
	fs := flag.NewFlagSet("emulkon", flag.ContinueOnError)
	fs.StringVar(&f.addr, "addr", "", "адрес HTTP (по умолчанию "+defaultAddr+")")
	fs.StringVar(&f.data, "data", "", "каталог данных")
	fs.StringVar(&f.downloads, "downloads", "", "каталоги дистрибутива для /downloads через «"+string(os.PathListSeparator)+"»")
	fs.BoolVar(&f.service, "service", false, "запуск службой Windows (так регистрирует установщик)")
	fs.BoolVar(&f.version, "version", false, "показать версию и выйти")
	if err := fs.Parse(args); err != nil {
		return f, err
	}
	f.set = map[string]bool{}
	fs.Visit(func(fl *flag.Flag) { f.set[fl.Name] = true })
	return f, nil
}

// resolve собирает настройки. reg — значения реестра (пусто вне Windows); exe — путь к программе.
func resolve(f flags, reg registryValues, exe string) (settings, error) {
	s := settings{Service: f.service, Sources: map[string]string{}}
	pick := func(name, flagName, flagVal, envKey, regName, def string) string {
		switch {
		case flagName != "" && f.set[flagName]:
			s.Sources[name] = "flag"
			return flagVal
		case envKey != "" && os.Getenv(envKey) != "":
			s.Sources[name] = "env"
			return os.Getenv(envKey)
		case regName != "" && reg[regName] != "":
			s.Sources[name] = "registry"
			return reg[regName]
		}
		s.Sources[name] = "default"
		return def
	}

	addrDef := defaultAddr
	if reg[regNetwork] == "1" {
		addrDef = defaultNetworkAddr
	}
	s.Addr = pick("addr", "addr", f.addr, "EMULKON_ADDR", regAddr, addrDef)
	s.DataDir = pick("data", "data", f.data, "EMULKON_DATA_DIR", regDataDir, defaultDataDir(f.service))
	if dl := pick("downloads", "downloads", f.downloads, "EMULKON_DOWNLOADS_DIR", regDownloadsDir, ""); dl != "" {
		s.DownloadsDirs = splitList(dl)
	} else {
		s.DownloadsDirs = defaultDownloadsDirs(exe)
	}
	s.LogLevel = pick("log_level", "", "", "EMULKON_LOG_LEVEL", regLogLevel, "info")

	var err error
	if s.ControlToken, err = token("control_token", "EMULKON_CONTROL_TOKEN", reg[regControlHash], &s); err != nil {
		return s, err
	}
	if s.DeviceToken, err = token("device_token", "EMULKON_DEVICE_TOKEN", reg[regDeviceHash], &s); err != nil {
		return s, err
	}

	if s.DefaultTimeout, err = envDuration("EMULKON_DEFAULT_TIMEOUT_MS", 30000, time.Millisecond); err != nil {
		return s, err
	}
	if s.SessionIdleTTL, err = envParseDuration("EMULKON_SESSION_IDLE_TTL", 24*time.Hour); err != nil {
		return s, err
	}
	mb, err := envInt("EMULKON_JOURNAL_MAX_FILE_MB", 32)
	if err != nil {
		return s, err
	}
	s.JournalMaxFileBytes = int64(mb) << 20
	if s.JournalMaxFiles, err = envInt("EMULKON_JOURNAL_MAX_FILES", 8); err != nil {
		return s, err
	}
	return s, nil
}

// token — открытый токен из окружения (Docker) или его хеш (<KEY>_HASH, реестр).
func token(name, envKey, regHash string, s *settings) (secret.Token, error) {
	if v := os.Getenv(envKey); v != "" {
		s.Sources[name] = "env"
		return secret.Plain(v), nil
	}
	hash, src := os.Getenv(envKey+"_HASH"), "env"
	if hash == "" {
		hash, src = regHash, "registry"
	}
	if hash == "" {
		s.Sources[name] = "default"
		return secret.Token{}, nil
	}
	t, err := secret.FromHash(hash)
	if err != nil {
		return t, fmt.Errorf("%s (%s): %w", name, src, err)
	}
	s.Sources[name] = src
	return t, nil
}

// defaultDataDir — у службы %ProgramData%\Emulkon\data, иначе ./data (как раньше).
func defaultDataDir(service bool) string {
	if service {
		if pd := os.Getenv("ProgramData"); pd != "" {
			return filepath.Join(pd, "Emulkon", "data")
		}
	}
	return "data"
}

// logsDir — каталог лога службы: %ProgramData%\Emulkon\logs (рядом с data по умолчанию).
func logsDir(dataDir string) string {
	if pd := os.Getenv("ProgramData"); pd != "" {
		return filepath.Join(pd, "Emulkon", "logs")
	}
	return filepath.Join(filepath.Dir(dataDir), "logs")
}

// defaultDownloadsDirs — раскладка установки (docs/local-install.md §2): программа в bin\, рядом 1C\driver и
// setup; прежняя раскладка — каталог downloads рядом с программой.
func defaultDownloadsDirs(exe string) []string {
	if exe == "" {
		return nil
	}
	dir := filepath.Dir(exe)
	if strings.EqualFold(filepath.Base(dir), "bin") {
		root := filepath.Dir(dir)
		var dirs []string
		for _, d := range []string{filepath.Join(root, "1C", "driver"), filepath.Join(root, "setup")} {
			if isDir(d) {
				dirs = append(dirs, d)
			}
		}
		if len(dirs) > 0 {
			return dirs
		}
	}
	if d := filepath.Join(dir, "downloads"); isDir(d) {
		return []string{d}
	}
	return nil
}

func splitList(v string) []string {
	var out []string
	for _, p := range strings.Split(v, string(os.PathListSeparator)) {
		if p = strings.TrimSpace(p); p != "" {
			out = append(out, p)
		}
	}
	return out
}

func isDir(p string) bool {
	st, err := os.Stat(p)
	return err == nil && st.IsDir()
}
