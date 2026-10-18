// Команда emulkon — сервер эмуляции подключаемого оборудования 1С (Эмулкон).
//
// Настройки (docs/local-install.md §2.1): ключи командной строки → переменные окружения → реестр Windows
// (HKLM\SOFTWARE\Policies\Emulkon, затем HKLM\SOFTWARE\Emulkon\Server) → значения по умолчанию.
//
//	-addr, EMULKON_ADDR, Addr               адрес HTTP (127.0.0.1:8737; при Network=1 — 0.0.0.0:8737)
//	-data, EMULKON_DATA_DIR, DataDir        каталог данных (служба — %ProgramData%\Emulkon\data, иначе ./data)
//	-downloads, EMULKON_DOWNLOADS_DIR,      каталоги дистрибутива для /downloads через «;» (по умолчанию —
//	  DownloadsDir                          1C\driver и setup рядом с bin\ установки или downloads рядом с программой)
//	EMULKON_DEVICE_TOKEN[_HASH],            общий токен устройств (открыто или SHA-256); пусто — открытый режим
//	  DeviceTokenHash
//	EMULKON_CONTROL_TOKEN[_HASH],           токен Control API (открыто или SHA-256); пусто — без авторизации
//	  ControlTokenHash
//	EMULKON_LOG_LEVEL, LogLevel             debug | info | warn | error (info)
//	EMULKON_UPDATE_MODE, UpdateMode         новые выпуски: off | notify (notify) — раз в сутки latest.json; только
//	                                        сообщить, установщик качает трей или консоль
//	EMULKON_UPDATE_URL, UpdateURL           свой latest.json вместо выпусков GitHub
//	EMULKON_DEFAULT_TIMEOUT_MS              рекомендуемый таймаут компоненты, мс (30000)
//	EMULKON_JOURNAL_MAX_FILE_MB             порог ротации файла журнала, МБ (32)
//	EMULKON_JOURNAL_MAX_FILES               сколько файлов журнала хранить (8)
//	EMULKON_SESSION_IDLE_TTL                время жизни простаивающей сессии (24h)
//
// -service — запуск службой Windows: лог в %ProgramData%\Emulkon\logs\service.log, старт, остановка и сбои —
// ещё и в журнал событий Windows (источник Emulkon).
package main

import (
	"context"
	"errors"
	"fmt"
	"io"
	"log/slog"
	"net"
	"net/http"
	"os"
	"os/signal"
	"path/filepath"
	"strconv"
	"strings"
	"syscall"
	"time"

	"emulkon/internal/app"
)

// version подставляется при сборке из файла VERSION: -ldflags "-X main.version=…" (build.ps1, Dockerfile).
var version = "dev"

// События журнала Windows (docs/local-install.md §2.2).
const (
	evStarted  = 1
	evStopped  = 2
	evFailed   = 3
	evOpenMode = 4
)

func main() {
	f, err := parseFlags(os.Args[1:])
	if err != nil {
		os.Exit(2)
	}
	if f.version {
		fmt.Println("emulkon", version)
		return
	}
	exe, _ := os.Executable()
	s, err := resolve(f, readRegistry(), exe)
	if err != nil {
		fail(nil, err)
	}

	if !s.Service {
		if err := setupLog(s, os.Stderr); err != nil {
			fail(nil, err)
		}
		ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
		defer stop()
		if err := serve(ctx, s, nil, func() {}); err != nil {
			fail(nil, err)
		}
		return
	}

	el := openEventLog()
	defer el.Close()
	logFile, err := openServiceLog(s.DataDir)
	if err != nil {
		el.Error(evFailed, "Эмулкон: не открыть лог службы: "+err.Error())
		logFile = nil
	}
	var w io.Writer = io.Discard
	if logFile != nil {
		w = logFile
		defer logFile.Close()
	}
	if err := setupLog(s, w); err != nil {
		el.Error(evFailed, "Эмулкон: "+err.Error())
		os.Exit(1)
	}
	err = runAsService(func(ctx context.Context, ready func()) error { return serve(ctx, s, el, ready) })
	if err != nil {
		fmt.Fprintln(os.Stderr, "emulkon:", err) // видно при ручном запуске с -service вне SCM
		slog.Error("служба остановлена с ошибкой", "err", err)
		el.Error(evFailed, "Эмулкон остановлен с ошибкой: "+err.Error())
		os.Exit(1)
	}
}

func fail(el *eventLog, err error) {
	slog.Error("сервер остановлен с ошибкой", "err", err)
	el.Error(evFailed, "Эмулкон: "+err.Error())
	os.Exit(1)
}

func setupLog(s settings, w io.Writer) error {
	var level slog.Level
	if err := level.UnmarshalText([]byte(s.LogLevel)); err != nil {
		return fmt.Errorf("уровень лога %q (%s): %w", s.LogLevel, s.Sources["log_level"], err)
	}
	slog.SetDefault(slog.New(slog.NewTextHandler(w, &slog.HandlerOptions{Level: level})))
	return nil
}

// openServiceLog — %ProgramData%\Emulkon\logs\service.log; больше 10 МБ — в service.log.1 (одна старая копия).
func openServiceLog(dataDir string) (*os.File, error) {
	dir := logsDir(dataDir)
	if err := os.MkdirAll(dir, 0o755); err != nil {
		return nil, fmt.Errorf("каталог логов: %w", err)
	}
	path := filepath.Join(dir, "service.log")
	if st, err := os.Stat(path); err == nil && st.Size() > 10<<20 {
		_ = os.Rename(path, path+".1")
	}
	return os.OpenFile(path, os.O_CREATE|os.O_APPEND|os.O_WRONLY, 0o644)
}

// serve работает до отмены ctx. ready — сервер слушает порт (служба сообщает SCM «работает»).
func serve(ctx context.Context, s settings, el *eventLog, ready func()) error {
	a, err := app.New(app.Config{
		Version:             version,
		DataDir:             s.DataDir,
		DeviceToken:         s.DeviceToken,
		ControlToken:        s.ControlToken,
		DownloadsDirs:       s.DownloadsDirs,
		DefaultTimeout:      s.DefaultTimeout,
		SessionIdleTTL:      s.SessionIdleTTL,
		JournalMaxFileBytes: s.JournalMaxFileBytes,
		JournalMaxFiles:     s.JournalMaxFiles,
		UpdateMode:          s.UpdateMode,
		UpdateURL:           s.UpdateURL,
	})
	if err != nil {
		return err
	}
	defer a.Close()

	if !isLoopback(s.Addr) {
		var open []string
		if s.DeviceToken.Empty() {
			slog.Warn("токен устройств не задан, а сервер слушает не только localhost: подключиться может любой")
			open = append(open, "токен устройств")
		}
		if s.ControlToken.Empty() {
			slog.Warn("токен Control API не задан, а сервер слушает не только localhost: управление открыто всем")
			open = append(open, "токен Control API")
		}
		if len(open) > 0 {
			el.Warning(evOpenMode, fmt.Sprintf("Эмулкон слушает %s без защиты: не задан %s.", s.Addr, strings.Join(open, " и ")))
		}
	}

	ln, err := net.Listen("tcp", s.Addr)
	if err != nil {
		return fmt.Errorf("слушать %s: %w", s.Addr, err)
	}

	// Базовый контекст запросов отменяется при остановке — так завершаются
	// долгие потоки журнала (SSE), иначе Shutdown ждал бы их до таймаута.
	baseCtx, cancelBase := context.WithCancel(context.Background())
	defer cancelBase()
	srv := &http.Server{
		Handler:           a.Handler,
		ReadHeaderTimeout: 10 * time.Second,
		BaseContext:       func(net.Listener) context.Context { return baseCtx },
	}
	runCtx, stopRun := context.WithCancel(ctx)
	defer stopRun()
	go a.Run(runCtx)

	errc := make(chan error, 1)
	go func() { errc <- srv.Serve(ln) }()
	slog.Info("Эмулкон запущен", "version", version, "addr", s.Addr, "data", s.DataDir,
		"downloads", strings.Join(s.DownloadsDirs, string(os.PathListSeparator)), "service", s.Service,
		"device_open_mode", s.DeviceToken.Empty(), "control_open_mode", s.ControlToken.Empty(),
		"sources", fmt.Sprint(s.Sources))
	el.Info(evStarted, fmt.Sprintf("Эмулкон %s запущен: %s, данные %s.", version, s.Addr, s.DataDir))
	ready()

	select {
	case err := <-errc:
		if !errors.Is(err, http.ErrServerClosed) {
			return fmt.Errorf("HTTP-сервер: %w", err)
		}
	case <-ctx.Done():
		slog.Info("остановка…")
	}
	cancelBase()
	shutdownCtx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()
	if err := srv.Shutdown(shutdownCtx); err != nil {
		return fmt.Errorf("остановить HTTP-сервер: %w", err)
	}
	el.Info(evStopped, "Эмулкон остановлен.")
	return nil
}

func envInt(key string, def int) (int, error) {
	v := os.Getenv(key)
	if v == "" {
		return def, nil
	}
	n, err := strconv.Atoi(v)
	if err != nil || n <= 0 {
		return 0, fmt.Errorf("%s: ожидается положительное целое, получено %q", key, v)
	}
	return n, nil
}

func envDuration(key string, def int, unit time.Duration) (time.Duration, error) {
	n, err := envInt(key, def)
	return time.Duration(n) * unit, err
}

func envParseDuration(key string, def time.Duration) (time.Duration, error) {
	v := os.Getenv(key)
	if v == "" {
		return def, nil
	}
	d, err := time.ParseDuration(v)
	if err != nil || d <= 0 {
		return 0, fmt.Errorf("%s: ожидается длительность вида 30m или 24h, получено %q", key, v)
	}
	return d, nil
}

func isLoopback(addr string) bool {
	host, _, err := net.SplitHostPort(addr)
	if err != nil {
		return false
	}
	if host == "localhost" {
		return true
	}
	ip := net.ParseIP(host)
	return ip != nil && ip.IsLoopback()
}
