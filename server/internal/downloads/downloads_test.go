package downloads

import (
	"archive/zip"
	"bytes"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"io"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"emulkon/internal/journal"
	"emulkon/internal/models"
)

func TestCompare(t *testing.T) {
	cases := []struct {
		a, b string
		want int
	}{
		{"0.2.0", "0.2.0", 0},
		{"0.2.1", "0.2.0", 1},
		{"0.10.0", "0.9.9", 1},
		{"1.0.0", "0.99.99", 1},
		{"0.2.0", "0.2.0-dev", 0},
		{"dev", "0.0.1", -1},
		{"0.0.1", "", 1},
	}
	for _, c := range cases {
		if got := Compare(c.a, c.b); got != c.want {
			t.Errorf("Compare(%q, %q) = %d, ждали %d", c.a, c.b, got, c.want)
		}
	}
}

func writeFile(t *testing.T, dir, name, body string) string {
	t.Helper()
	if err := os.WriteFile(filepath.Join(dir, name), []byte(body), 0o644); err != nil {
		t.Fatal(err)
	}
	s := sha256.Sum256([]byte(body))
	return hex.EncodeToString(s[:])
}

func newHandler(t *testing.T, dir string) (*Handler, *journal.Journal, *httptest.Server) {
	t.Helper()
	j, err := journal.Open(journal.Options{Dir: t.TempDir()})
	if err != nil {
		t.Fatal(err)
	}
	h := New(Config{
		Dirs: []string{dir}, Version: "0.3.0", Types: models.EquipmentTypes,
		FullModels: map[string]bool{"kkt": true},
	}, j)
	mux := http.NewServeMux()
	h.Register(mux)
	srv := httptest.NewServer(mux)
	t.Cleanup(func() { srv.Close(); j.Close() })
	return h, j, srv
}

func getJSON(t *testing.T, url string, out any) int {
	t.Helper()
	res, err := http.Get(url)
	if err != nil {
		t.Fatal(err)
	}
	defer res.Body.Close()
	if err := json.NewDecoder(res.Body).Decode(out); err != nil && res.StatusCode == 200 {
		t.Fatal(err)
	}
	return res.StatusCode
}

// Файл, который не прочитать (пропал во время обхода, забран антивирусом), не роняет каталог (#20):
// висячая ссылка — то же самое для теста.
func TestCatalogSkipsUnreadable(t *testing.T) {
	dir := t.TempDir()
	writeFile(t, dir, "EmulkonDriver_0.3.0.zip", "zip")
	if err := os.Symlink(filepath.Join(dir, "нет-такого"), filepath.Join(dir, "EmulkonSetup_0.3.0.exe")); err != nil {
		t.Skipf("символическая ссылка недоступна: %v", err)
	}
	_, _, srv := newHandler(t, dir)
	var c Catalog
	if code := getJSON(t, srv.URL+"/api/v1/downloads", &c); code != 200 {
		t.Fatalf("каталог: %d", code)
	}
	if c.DriverPackage == nil || c.Setup != nil {
		t.Fatalf("пакет есть, нечитаемого установщика нет: %+v, %+v", c.DriverPackage, c.Setup)
	}
}

func TestCatalogPicksNewestPerKind(t *testing.T) {
	dir := t.TempDir()
	writeFile(t, dir, "EmulkonDriver_0.2.0.zip", "old zip")
	writeFile(t, dir, "EmulkonDriver_0.10.0.zip", "new zip")
	writeFile(t, dir, "EmulkonDriver_0.2.0_win64.dll", "old 64")
	sum64 := writeFile(t, dir, "EmulkonDriver_0.3.0_win64.dll", "new 64")
	writeFile(t, dir, "EmulkonDriver_0.3.0_win32.dll", "new 32")
	writeFile(t, dir, "EmulkonSetup_0.3.0.exe", "setup")
	writeFile(t, dir, "secret.txt", "не раздаётся")
	_, _, srv := newHandler(t, dir)

	var c Catalog
	if code := getJSON(t, srv.URL+"/api/v1/downloads", &c); code != 200 {
		t.Fatalf("каталог: %d", code)
	}
	if !c.Published || c.DriverPackage == nil || c.DriverPackage.Version != "0.10.0" {
		t.Fatalf("пакет драйвера: %+v", c.DriverPackage)
	}
	if len(c.DriverDLLs) != 2 || c.DriverDLLs[0].Arch != ArchX64 || c.DriverDLLs[0].Version != "0.3.0" ||
		c.DriverDLLs[0].SHA256 != sum64 || c.DriverDLLs[1].Arch != ArchX86 {
		t.Fatalf("DLL: %+v", c.DriverDLLs)
	}
	if c.Setup == nil || c.Setup.URL != "/downloads/EmulkonSetup_0.3.0.exe" {
		t.Fatalf("установщик: %+v", c.Setup)
	}
	kkt := c.Equipment[0] // настоящие модели — первыми
	if kkt.Key != "kkt" || kkt.Model != "full" || !kkt.Driver || len(kkt.Features) == 0 {
		t.Fatalf("ККТ в перечне поддержки: %+v", kkt)
	}
	if len(c.Equipment) != len(models.EquipmentTypes) || c.Equipment[1].Model != "stub" || c.Equipment[1].Key != "acquiring" {
		t.Fatalf("остальные — заглушки, от кассы к складу (displayOrder): %+v", c.Equipment[1])
	}
}

func TestDriverUpdate(t *testing.T) {
	dir := t.TempDir()
	writeFile(t, dir, "EmulkonDriver_0.3.0_win64.dll", "new 64")
	_, _, srv := newHandler(t, dir)

	var u UpdateResponse
	getJSON(t, srv.URL+"/api/v1/driver/update?version=0.2.0&arch=x86_64", &u)
	if !u.UpdateAvailable || u.Latest == nil || u.Latest.Version != "0.3.0" {
		t.Fatalf("обновление x64: %+v", u)
	}
	u = UpdateResponse{}
	getJSON(t, srv.URL+"/api/v1/driver/update?version=0.3.0&arch=x86_64", &u)
	if u.UpdateAvailable {
		t.Fatalf("та же версия — не обновление: %+v", u)
	}
	u = UpdateResponse{}
	getJSON(t, srv.URL+"/api/v1/driver/update?version=0.2.0&arch=i386", &u)
	if u.UpdateAvailable || u.Latest != nil {
		t.Fatalf("x86 не опубликована: %+v", u)
	}
	var e map[string]any
	if code := getJSON(t, srv.URL+"/api/v1/driver/update?version=x&arch=x86_64", &e); code != 400 {
		t.Fatalf("неверная версия: %d", code)
	}
	if code := getJSON(t, srv.URL+"/api/v1/driver/update?version=0.2.0&arch=x86_64&os=macos", &e); code != 400 {
		t.Fatalf("неизвестная ОС: %d", code)
	}
}

// Linux-драйвер (.so) — отдельно от Windows: драйвер под Linux не получает DLL и наоборот.
func TestDriverUpdateLinux(t *testing.T) {
	dir := t.TempDir()
	writeFile(t, dir, "EmulkonDriver_0.3.0_win64.dll", "win 64")
	sumLin := writeFile(t, dir, "EmulkonDriver_0.4.0_lin64.so", "lin 64")
	writeFile(t, dir, "EmulkonDriver_0.4.0_lin64.dll", "не тот суффикс — не раздаётся")
	h, _, srv := newHandler(t, dir)

	var u UpdateResponse
	getJSON(t, srv.URL+"/api/v1/driver/update?version=0.2.0&arch=x86_64&os=linux", &u)
	if !u.UpdateAvailable || u.Latest == nil || u.Latest.Version != "0.4.0" || u.Latest.OS != OSLinux ||
		u.Latest.SHA256 != sumLin || u.Latest.Name != "EmulkonDriver_0.4.0_lin64.so" {
		t.Fatalf("обновление Linux x64: %+v", u)
	}
	u = UpdateResponse{}
	getJSON(t, srv.URL+"/api/v1/driver/update?version=0.2.0&arch=x86_64", &u)
	if u.Latest == nil || u.Latest.Version != "0.3.0" || u.Latest.OS != OSWindows {
		t.Fatalf("без os — Windows, как у драйверов до 0.9.0: %+v", u)
	}
	if f, ok := h.LatestDriver(OSLinux, ArchX86); ok {
		t.Fatalf("Linux x86 не опубликован: %+v", f)
	}
	// ARM64 (issue #12): запрос принимается, но библиотеку x86_64 драйверу ARM не предлагаем.
	u = UpdateResponse{}
	getJSON(t, srv.URL+"/api/v1/driver/update?version=0.2.0&arch=aarch64&os=linux", &u)
	if u.UpdateAvailable || u.Latest != nil {
		t.Fatalf("Linux ARM64 — файлов нет, обновления нет: %+v", u)
	}
	var c Catalog
	getJSON(t, srv.URL+"/api/v1/downloads", &c)
	if len(c.DriverDLLs) != 2 || c.DriverDLLs[0].OS != OSWindows || c.DriverDLLs[1].OS != OSLinux {
		t.Fatalf("каталог: сначала Windows, потом Linux: %+v", c.DriverDLLs)
	}
}

func TestFileDownloadIsJournaled(t *testing.T) {
	dir := t.TempDir()
	writeFile(t, dir, "EmulkonDriver_0.3.0.zip", "zip body")
	writeFile(t, dir, "secret.txt", "нельзя")
	_, j, srv := newHandler(t, dir)

	req, _ := http.NewRequest("GET", srv.URL+"/downloads/EmulkonDriver_0.3.0.zip", nil)
	req.Header.Set("X-Emulkon-Workstation", "KASSA-01")
	res, err := http.DefaultClient.Do(req)
	if err != nil {
		t.Fatal(err)
	}
	body, _ := io.ReadAll(res.Body)
	res.Body.Close()
	if res.StatusCode != 200 || string(body) != "zip body" || res.Header.Get("Content-Type") != "application/zip" {
		t.Fatalf("файл: %d %q %s", res.StatusCode, body, res.Header.Get("Content-Type"))
	}
	entries, err := j.Tail(journal.Filter{Method: "download"}, 10)
	if err != nil || len(entries) != 1 || entries[0].WorkstationID != "KASSA-01" {
		t.Fatalf("журнал скачиваний: %v %+v", err, entries)
	}

	for _, p := range []string{"/downloads/secret.txt", "/downloads/..%2fsecret.txt", "/downloads/EmulkonDriver_9.9.9.zip"} {
		res, err := http.Get(srv.URL + p)
		if err != nil {
			t.Fatal(err)
		}
		res.Body.Close()
		if res.StatusCode != 404 {
			t.Errorf("%s: %d, ждали 404", p, res.StatusCode)
		}
	}
}

func TestNoDirMeansUnpublished(t *testing.T) {
	_, _, srv := newHandler(t, "")
	var c Catalog
	getJSON(t, srv.URL+"/api/v1/downloads", &c)
	if c.Published || c.DriverPackage != nil || len(c.Equipment) == 0 {
		t.Fatalf("без каталога: %+v", c)
	}
}

func TestTypePackage(t *testing.T) {
	dir := t.TempDir()
	var buf bytes.Buffer
	zw := zip.NewWriter(&buf)
	for name, body := range map[string]string{
		"INFO.XML":                `<?xml version="1.0" encoding="UTF-8"?><drivers><component progid="AddIn.EmulkonDriver" type="ККТ,ЭквайринговыйТерминал,ПринтерЧеков" name="ЭМУЛЯТОР Эмулкон 0.5.0 (imiron.ru)" version="0.5.0"/></drivers>`,
		"MANIFEST.XML":            `<bundle/>`,
		"EmulkonDriver_win64.dll": "MZ…",
	} {
		w, _ := zw.Create(name)
		w.Write([]byte(body))
	}
	zw.Close()
	if err := os.WriteFile(filepath.Join(dir, "EmulkonDriver_0.5.0.zip"), buf.Bytes(), 0o644); err != nil {
		t.Fatal(err)
	}
	h, _, srv := newHandler(t, dir)
	c, err := h.Catalog()
	if err != nil {
		t.Fatal(err)
	}
	var kkt *Support
	for i := range c.Equipment {
		if c.Equipment[i].Key == "kkt" {
			kkt = &c.Equipment[i]
		}
		if c.Equipment[i].Key == "scanner" && c.Equipment[i].Package != nil && !driverTypes["scanner"] {
			t.Error("пакет под тип, которого нет в драйвере")
		}
	}
	if kkt == nil || kkt.Package == nil || kkt.Package.Name != "EmulkonDriver_0.5.0_kkt.zip" || kkt.Package.SHA256 == "" {
		t.Fatalf("пакет ККТ: %+v", kkt)
	}

	res, err := http.Get(srv.URL + "/downloads/EmulkonDriver_0.5.0_kkt.zip")
	if err != nil {
		t.Fatal(err)
	}
	data, _ := io.ReadAll(res.Body)
	res.Body.Close()
	sum := sha256.Sum256(data)
	if res.StatusCode != 200 || hex.EncodeToString(sum[:]) != kkt.Package.SHA256 {
		t.Fatalf("скачивание: %d, хеш не совпал с каталогом", res.StatusCode)
	}
	zr, err := zip.NewReader(bytes.NewReader(data), int64(len(data)))
	if err != nil {
		t.Fatal(err)
	}
	files := map[string]string{}
	for _, f := range zr.File {
		rc, _ := f.Open()
		b, _ := io.ReadAll(rc)
		rc.Close()
		files[f.Name] = string(b)
	}
	if !strings.Contains(files["INFO.XML"], `type="ККТ"`) || strings.Contains(files["INFO.XML"], "Эквайринг") ||
		files["EmulkonDriver_win64.dll"] != "MZ…" || len(files) != 3 {
		t.Fatalf("содержимое пакета под тип: %v", files)
	}
	for _, bad := range []string{"EmulkonDriver_0.5.0_nosuch.zip", "EmulkonDriver_0.4.0_kkt.zip"} {
		if res, _ := http.Get(srv.URL + "/downloads/" + bad); res.StatusCode != 404 {
			t.Errorf("%s: %d", bad, res.StatusCode)
		}
	}
	if got := (models.EquipmentType{Key: "card_reader", NameRU: "СчитывательКарт"}).BPOName(); got != "СчитывательМагнитныхКарт" {
		t.Errorf("имя считывателя карт в БПО: %s", got)
	}
}

// Раскладка установки (docs/local-install.md §2): драйвер в 1C\driver, установщик в setup — оба на /downloads.
func TestSeveralDirs(t *testing.T) {
	driver, setup := t.TempDir(), t.TempDir()
	writeFile(t, driver, "EmulkonDriver_0.3.0.zip", "zip")
	writeFile(t, driver, "EmulkonDriver_0.3.0_win64.dll", "dll")
	writeFile(t, setup, "EmulkonSetup_0.3.0.exe", "exe")
	writeFile(t, setup, "EmulkonDriver_0.3.0.zip", "дубль во втором каталоге не раздаётся")
	j, err := journal.Open(journal.Options{Dir: t.TempDir()})
	if err != nil {
		t.Fatal(err)
	}
	defer j.Close()
	h := New(Config{Dirs: []string{driver, filepath.Join(driver, "нет-такого"), setup}, Version: "0.3.0", Types: models.EquipmentTypes}, j)
	mux := http.NewServeMux()
	h.Register(mux)
	srv := httptest.NewServer(mux)
	defer srv.Close()

	var c Catalog
	if code := getJSON(t, srv.URL+"/api/v1/downloads", &c); code != 200 {
		t.Fatalf("каталог: %d", code)
	}
	if c.DriverPackage == nil || c.Setup == nil || len(c.DriverDLLs) != 1 || c.DriverPackage.Size != 3 {
		t.Fatalf("каталог из двух каталогов: %+v", c)
	}
	res, err := http.Get(srv.URL + "/downloads/EmulkonSetup_0.3.0.exe")
	if err != nil {
		t.Fatal(err)
	}
	res.Body.Close()
	if res.StatusCode != 200 {
		t.Fatalf("установщик из второго каталога: %d", res.StatusCode)
	}
}
