package main

import (
	"os"
	"path/filepath"
	"strings"
	"testing"

	"emulkon/internal/secret"
)

func clearEnv(t *testing.T) {
	for _, k := range []string{"EMULKON_ADDR", "EMULKON_DATA_DIR", "EMULKON_DOWNLOADS_DIR", "EMULKON_LOG_LEVEL",
		"EMULKON_CONTROL_TOKEN", "EMULKON_CONTROL_TOKEN_HASH", "EMULKON_DEVICE_TOKEN", "EMULKON_DEVICE_TOKEN_HASH",
		"EMULKON_UPDATE_MODE", "EMULKON_UPDATE_URL"} {
		t.Setenv(k, "")
	}
}

func TestResolvePrecedence(t *testing.T) {
	clearEnv(t)
	reg := registryValues{regAddr: "0.0.0.0:9000", regDataDir: `C:\reg\data`, regLogLevel: "debug",
		regControlHash: secret.Hash("ctl"), regDeviceHash: secret.Hash("dev")}

	f, _ := parseFlags(nil)
	s, err := resolve(f, reg, "")
	if err != nil {
		t.Fatal(err)
	}
	if s.Addr != "0.0.0.0:9000" || s.DataDir != `C:\reg\data` || s.LogLevel != "debug" || s.Sources["addr"] != "registry" {
		t.Fatalf("реестр: %+v", s)
	}
	if !s.ControlToken.Match("ctl") || !s.DeviceToken.Match("dev") || s.Sources["control_token"] != "registry" {
		t.Fatal("токены из реестра (хеши)")
	}

	t.Setenv("EMULKON_ADDR", "127.0.0.1:8800")
	t.Setenv("EMULKON_CONTROL_TOKEN", "env-ctl")
	s, _ = resolve(f, reg, "")
	if s.Addr != "127.0.0.1:8800" || s.Sources["addr"] != "env" || !s.ControlToken.Match("env-ctl") || s.ControlToken.Match("ctl") {
		t.Fatalf("окружение перекрывает реестр: %+v", s)
	}

	f, _ = parseFlags([]string{"-addr", "127.0.0.1:7000"})
	s, _ = resolve(f, reg, "")
	if s.Addr != "127.0.0.1:7000" || s.Sources["addr"] != "flag" {
		t.Fatalf("ключ перекрывает всё: %+v", s)
	}
}

func TestResolveDefaults(t *testing.T) {
	clearEnv(t)
	f, _ := parseFlags(nil)
	s, err := resolve(f, registryValues{}, "")
	if err != nil || s.Addr != defaultAddr || s.DataDir != "data" || !s.ControlToken.Empty() || !s.DeviceToken.Empty() {
		t.Fatalf("по умолчанию: %+v %v", s, err)
	}
	s, _ = resolve(f, registryValues{regNetwork: "1"}, "")
	if s.Addr != defaultNetworkAddr {
		t.Fatalf("Network=1: %s", s.Addr)
	}
	t.Setenv("ProgramData", `C:\PD`)
	f, _ = parseFlags([]string{"-service"})
	s, _ = resolve(f, registryValues{}, "")
	if s.DataDir != filepath.Join(`C:\PD`, "Emulkon", "data") || !s.Service {
		t.Fatalf("служба: %+v", s)
	}
	if _, err := resolve(f, registryValues{regControlHash: "не-хеш"}, ""); err == nil {
		t.Fatal("неверный хеш — ошибка")
	}
}

func TestDefaultDownloadsDirs(t *testing.T) {
	root := t.TempDir()
	for _, d := range []string{"bin", filepath.Join("1C", "driver"), "setup"} {
		if err := os.MkdirAll(filepath.Join(root, d), 0o755); err != nil {
			t.Fatal(err)
		}
	}
	got := defaultDownloadsDirs(filepath.Join(root, "bin", "emulkon.exe"))
	want := []string{filepath.Join(root, "1C", "driver"), filepath.Join(root, "setup")}
	if len(got) != 2 || got[0] != want[0] || got[1] != want[1] {
		t.Fatalf("раскладка установки: %v", got)
	}

	old := t.TempDir()
	os.MkdirAll(filepath.Join(old, "downloads"), 0o755)
	if got := defaultDownloadsDirs(filepath.Join(old, "emulkon.exe")); len(got) != 1 || got[0] != filepath.Join(old, "downloads") {
		t.Fatalf("прежняя раскладка: %v", got)
	}
	if got := splitList("a" + string(os.PathListSeparator) + " " + string(os.PathListSeparator) + "b"); len(got) != 2 {
		t.Fatalf("список: %v", got)
	}
}

// Обновления (issue #8): по умолчанию notify; режим из реестра или окружения; неверный режим — ошибка запуска.
func TestResolveUpdates(t *testing.T) {
	clearEnv(t)
	f, _ := parseFlags(nil)
	s, err := resolve(f, registryValues{}, "")
	if err != nil || s.UpdateMode != "notify" || s.UpdateURL != "" || s.Sources["update_mode"] != "default" {
		t.Fatalf("по умолчанию: %+v, %v", s, err)
	}
	s, err = resolve(f, registryValues{regUpdateMode: "OFF", regUpdateURL: "https://example.org/latest.json"}, "")
	if err != nil || s.UpdateMode != "off" || s.UpdateURL != "https://example.org/latest.json" {
		t.Fatalf("реестр: %+v, %v", s, err)
	}
	t.Setenv("EMULKON_UPDATE_MODE", "auto")
	if _, err := resolve(f, registryValues{}, ""); err == nil || !strings.Contains(err.Error(), "off или notify") {
		t.Fatalf("неверный режим: %v", err)
	}
}
