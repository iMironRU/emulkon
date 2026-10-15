// Package downloads — страница «Поддержка и загрузки» и раздача дистрибутива:
// пакет драйвера для БПО, DLL драйвера для самообновления, установщик Эмулкона.
//
// Файлы лежат в каталоге загрузок (EMULKON_DOWNLOADS_DIR), версия — в имени файла;
// раскладывает их build.ps1 в корне репозитория или установщик. Всё здесь открыто
// без токенов: файлы не секретны, а кассе нужно скачать драйвер до любых настроек.
package downloads

import (
	"bytes"
	"crypto/sha256"
	"encoding/hex"
	"fmt"
	"io"
	"log/slog"
	"net/http"
	"os"
	"path/filepath"
	"regexp"
	"sort"
	"strconv"
	"strings"
	"sync"
	"time"

	"emulkon/internal/httpx"
	"emulkon/internal/journal"
	"emulkon/internal/models"
)

// Имена файлов в каталоге загрузок.
var (
	rePackage = regexp.MustCompile(`^EmulkonDriver_(\d+\.\d+\.\d+)\.zip$`)
	// DLL драйвера для самообновления: Windows — _win64.dll, _win32.dll; Linux — _lin64.so, _lin32.so.
	reDLL   = regexp.MustCompile(`^EmulkonDriver_(\d+\.\d+\.\d+)_(?:win(32|64)\.dll|lin(32|64)\.so)$`)
	reSetup = regexp.MustCompile(`^EmulkonSetup_(\d+\.\d+\.\d+)\.exe$`)
)

// Архитектуры DLL драйвера — как в MANIFEST.XML пакета (component/build.ps1).
const (
	ArchX64 = "x86_64"
	ArchX86 = "i386"
)

// ОС драйвера: в имени файла (_win…dll, _lin…so) и в запросе обновления. Без ОС — Windows: так
// спрашивают драйверы до 0.9.0.
const (
	OSWindows = "windows"
	OSLinux   = "linux"
)

// File — файл дистрибутива.
type File struct {
	Name     string    `json:"name"`
	URL      string    `json:"url"` // путь на этом сервере: /downloads/<name>
	Version  string    `json:"version"`
	Arch     string    `json:"arch,omitempty"` // для DLL драйвера: x86_64 | i386
	OS       string    `json:"os,omitempty"`   // для DLL драйвера: windows | linux
	Size     int64     `json:"size"`
	SHA256   string    `json:"sha256"`
	Modified time.Time `json:"modified"`
}

// Support — что эмулятор умеет по типу оборудования.
type Support struct {
	models.EquipmentType
	Model    string   `json:"model"`  // full — настоящая модель, stub — заглушка (любой метод — успех)
	Driver   bool     `json:"driver"` // тип объявлен в пакете драйвера для БПО (INFO.XML)
	Features []string `json:"features,omitempty"`
	// Package — пакет драйвера только с этим типом (собирается из общего, typepkg.go).
	Package *File `json:"package,omitempty"`
}

// Catalog — ответ GET /api/v1/downloads и данные страницы /downloads.
type Catalog struct {
	Product           string    `json:"product"`
	ServerVersion     string    `json:"server_version"`
	InterfaceRevision string    `json:"interface_revision"` // ревизия требований к драйверам БПО
	Published         bool      `json:"published"`          // в каталоге загрузок есть файлы
	DriverPackage     *File     `json:"driver_package,omitempty"`
	DriverDLLs        []File    `json:"driver_dlls"`
	Setup             *File     `json:"setup,omitempty"`
	Equipment         []Support `json:"equipment"`
}

// UpdateResponse — ответ GET /api/v1/driver/update.
type UpdateResponse struct {
	Current         string `json:"current"`
	UpdateAvailable bool   `json:"update_available"`
	Latest          *File  `json:"latest,omitempty"` // самая новая DLL для этой архитектуры
}

// driverTypes — типы оборудования в INFO.XML пакета драйвера (component/build.ps1).
var driverTypes = map[string]bool{"kkt": true, "acquiring": true, "scales": true, "label_scales": true,
	"receipt_printer": true, "label_printer": true, "scanner": true, "card_reader": true,
	"display": true, "tsd": true, "rfid": true, "recognizer": true}

// features — что умеет настоящая модель (docs/models/<тип>.md).
var features = map[string][]string{
	"kkt": {
		"регистрация, изменение параметров и закрытие ФН",
		"смены с контролем 24 часов",
		"чеки и чеки коррекции с проверками настоящей ККТ",
		"внесение и выплата, X-отчёт, отчёт о расчётах, копия чека, текстовый документ",
		"маркировка: запрос КМ, ответ ОИСМ, подтверждение",
		"очередь ОФД, сбои: бумага, крышка, ФН, печать после фискализации",
		"лента печати в веб-интерфейсе и всплывающие чеки в трее",
	},
	"acquiring": {
		"оплата, возврат, отмена (и частичная), оплата с выдачей наличных",
		"преавторизация: блокировка, списание, отмена",
		"экран терминала: карту «прикладывают» в трее или веб-интерфейсе — одобрение, отказ банка, неверный ПИН, отмена",
		"тестовые карты МИР, VISA, Mastercard, UnionPay и оплата по QR СБП",
		"слипы для печати на ККТ, сверка итогов, аварийная отмена, статус операции",
		"режимы для автотестов: одобрять или отказывать сразу",
	},
	"scales": {
		"вес кладут на экране весов в веб-интерфейсе или вводят в трее на каждое взвешивание",
		"тара: текущий вес или заданная",
		"сбои: перегрузка, вес не стабилизирован, товар задевает край платформы",
	},
	"label_scales": {
		"выгрузка товаров и картинок из 1С пакетами, очистка",
		"таблица товаров в веб-интерфейсе",
		"этикетки с весовым штрихкодом EAN-13: печать с весов, лента в веб-интерфейсе и трее",
	},
	"receipt_printer": {
		"текстовые документы со штрихкодами и QR, ширина строки",
		"денежный ящик",
		"сбои: нет бумаги, открыта крышка, штрихкоды не печатаются",
	},
	"label_printer": {
		"этикетки по шаблону из 1С: пакеты, копии, статичные поля, штрихкоды, картинки",
		"вид этикетки по разметке в веб-интерфейсе, текст — во всплывающем окне трея",
		"сбои: кончились этикетки, открыта крышка",
	},
	"scanner": {
		"штрихкод «сканируют» в панели управления: свой или из генератора",
		"генератор: EAN-13, EAN-8, весовой, код маркировки DataMatrix с разделителем GS, Code128, QR",
		"событие строкой или в Base64",
	},
	"card_reader": {
		"карту «проводят» в панели управления: дисконтная, карта кассира или свои дорожки",
		"дорожки с префиксом и суффиксом, по умолчанию — как в настройке БПО (дорожка 2, суффикс CR)",
	},
	"display": {
		"строки, QR-код, HTML с медиафайлами — экран дисплея в панели управления",
		"индикатор статуса операции и рабочего места, звуковой сигнал",
		"сенсорный экран: нажатие на элемент — событие в 1С",
	},
	"tsd": {
		"выгрузка товаров из 1С пакетами, полная и частичная",
		"документ «собирают» в панели управления: из таблицы товаров или по штрихкоду",
		"загрузка собранного документа в 1С",
	},
	"rfid": {
		"метки кладут в поле антенны в панели управления: свои или сгенерированные",
		"сессия чтения, пакеты меток и событие RFID, запись в банки EPC и USER",
	},
	"recognizer": {
		"что лежит на весах, задают в панели управления: коды товаров и вероятности",
		"распознавание с обучением и подтверждением, снимок с камеры",
	},
}

// order — порядок карточек на странице загрузок: сначала то, что есть на каждой кассе.
var order = []string{"kkt", "acquiring", "receipt_printer", "scanner", "scales", "display", "card_reader",
	"label_scales", "label_printer", "tsd", "rfid", "recognizer"}

func displayOrder(key string) int {
	for i, k := range order {
		if k == key {
			return i
		}
	}
	return len(order)
}

// Config — параметры раздела загрузок.
type Config struct {
	// Dirs — каталоги файлов (установка: 1Cdriver и setup, docs/local-install.md §2); пусто — раздавать нечего.
	// Файл с одним именем берётся из первого каталога, где он есть.
	Dirs       []string
	Version    string // версия сервера
	Types      []models.EquipmentType
	FullModels map[string]bool // ключи типов с настоящей моделью
	Page       http.Handler    // страница /downloads (web)
}

// Handler — раздел загрузок.
type Handler struct {
	cfg Config
	j   *journal.Journal

	mu       sync.Mutex
	hashes   map[string]hashEntry   // имя → хеш при данном размере и времени изменения
	typePkgs map[string]typePackage // хеш общего пакета + тип → пакет под тип
}

type hashEntry struct {
	size    int64
	modTime time.Time
	sum     string
}

func New(cfg Config, j *journal.Journal) *Handler {
	return &Handler{cfg: cfg, j: j, hashes: map[string]hashEntry{}, typePkgs: map[string]typePackage{}}
}

// Register регистрирует маршруты: страницу, каталог, файлы и проверку обновления драйвера.
func (h *Handler) Register(mux *http.ServeMux) {
	if h.cfg.Page != nil {
		mux.Handle("GET /downloads", h.cfg.Page)
	}
	mux.HandleFunc("GET /downloads/{$}", func(w http.ResponseWriter, r *http.Request) {
		http.Redirect(w, r, "/downloads", http.StatusMovedPermanently)
	})
	mux.HandleFunc("GET /downloads/{name}", h.file)
	mux.HandleFunc("GET /api/v1/downloads", h.catalog)
	mux.HandleFunc("GET /api/v1/driver/update", h.update)
}

// Catalog собирает каталог: файлы с хешами и поддержку оборудования.
func (h *Handler) Catalog() (Catalog, error) {
	c := Catalog{
		Product: "Эмулкон", ServerVersion: h.cfg.Version, InterfaceRevision: "4.8",
		DriverDLLs: []File{},
	}
	for _, t := range h.cfg.Types {
		s := Support{EquipmentType: t, Model: "stub", Driver: driverTypes[t.Key]}
		if h.cfg.FullModels[t.Key] {
			s.Model = "full"
			s.Features = features[t.Key]
		}
		c.Equipment = append(c.Equipment, s)
	}
	// Сначала то, что эмулируется по-настоящему; внутри — от кассы к складу (displayOrder).
	sort.SliceStable(c.Equipment, func(i, k int) bool {
		a, b := c.Equipment[i], c.Equipment[k]
		if (a.Model == "full") != (b.Model == "full") {
			return a.Model == "full"
		}
		return displayOrder(a.Key) < displayOrder(b.Key)
	})
	files, err := h.scan()
	if err != nil {
		return c, err
	}
	dlls := map[string]File{} // ОС и архитектура → самая новая
	for _, f := range files {
		switch {
		case rePackage.MatchString(f.Name):
			if c.DriverPackage == nil || Compare(f.Version, c.DriverPackage.Version) > 0 {
				c.DriverPackage = &f
			}
		case reDLL.MatchString(f.Name):
			if cur, ok := dlls[f.OS+"/"+f.Arch]; !ok || Compare(f.Version, cur.Version) > 0 {
				dlls[f.OS+"/"+f.Arch] = f
			}
		case reSetup.MatchString(f.Name):
			if c.Setup == nil || Compare(f.Version, c.Setup.Version) > 0 {
				c.Setup = &f
			}
		}
	}
	for _, o := range []string{OSWindows, OSLinux} {
		for _, a := range []string{ArchX64, ArchX86} {
			if f, ok := dlls[o+"/"+a]; ok {
				c.DriverDLLs = append(c.DriverDLLs, f)
			}
		}
	}
	c.Published = len(files) > 0
	if g := c.DriverPackage; g != nil {
		for i := range c.Equipment {
			s := &c.Equipment[i]
			if !s.Driver {
				continue
			}
			p, err := h.typePackage(*g, s.Key)
			if err != nil {
				slog.Warn("пакет драйвера под тип", "type", s.Key, "err", err)
				continue // битый общий пакет — без пакетов под тип; сам он раздаётся как есть
			}
			name := TypePackageName(g.Version, s.Key)
			s.Package = &File{Name: name, URL: "/downloads/" + name, Version: g.Version,
				Size: int64(len(p.data)), SHA256: p.sum, Modified: g.Modified}
		}
	}
	return c, nil
}

// path — файл в первом каталоге загрузок, где он есть; нет нигде — путь в первом (дальше Open вернёт ошибку).
func (h *Handler) path(name string) string {
	for _, d := range h.cfg.Dirs {
		if p := filepath.Join(d, name); fileExists(p) {
			return p
		}
	}
	if len(h.cfg.Dirs) == 0 {
		return ""
	}
	return filepath.Join(h.cfg.Dirs[0], name)
}

func fileExists(p string) bool {
	st, err := os.Stat(p)
	return err == nil && !st.IsDir()
}

// LatestDriver — самая новая DLL драйвера для ОС (пусто — Windows) и архитектуры; ok=false — нет.
func (h *Handler) LatestDriver(osName, arch string) (File, bool) {
	if osName == "" {
		osName = OSWindows
	}
	c, err := h.Catalog()
	if err != nil {
		return File{}, false
	}
	for _, f := range c.DriverDLLs {
		if f.OS == osName && f.Arch == arch {
			return f, true
		}
	}
	return File{}, false
}

// scan читает каталог загрузок. Хеш файла считается один раз на пару (размер, время).
func (h *Handler) scan() ([]File, error) {
	var out []File
	seen := map[string]bool{}
	for _, dir := range h.cfg.Dirs {
		entries, err := os.ReadDir(dir)
		if os.IsNotExist(err) {
			continue
		}
		if err != nil {
			return nil, fmt.Errorf("каталог загрузок %s: %w", dir, err)
		}
		for _, e := range entries {
			if e.IsDir() || seen[e.Name()] {
				continue
			}
			seen[e.Name()] = true
			f := File{Name: e.Name(), URL: "/downloads/" + e.Name()}
			switch {
			case rePackage.MatchString(f.Name):
				f.Version = rePackage.FindStringSubmatch(f.Name)[1]
			case reDLL.MatchString(f.Name):
				m := reDLL.FindStringSubmatch(f.Name)
				f.Version, f.OS, f.Arch = m[1], OSWindows, ArchX86
				bits := m[2]
				if m[3] != "" {
					f.OS, bits = OSLinux, m[3]
				}
				if bits == "64" {
					f.Arch = ArchX64
				}
			case reSetup.MatchString(f.Name):
				f.Version = reSetup.FindStringSubmatch(f.Name)[1]
			default:
				continue // посторонние файлы не раздаём
			}
			info, err := e.Info()
			if err != nil {
				continue
			}
			f.Size, f.Modified = info.Size(), info.ModTime().UTC()
			if f.SHA256, err = h.hash(f.Name, info); err != nil {
				return nil, err
			}
			out = append(out, f)
		}
	}
	sort.Slice(out, func(i, k int) bool { return out[i].Name < out[k].Name })
	return out, nil
}

func (h *Handler) hash(name string, info os.FileInfo) (string, error) {
	h.mu.Lock()
	e, ok := h.hashes[name]
	h.mu.Unlock()
	if ok && e.size == info.Size() && e.modTime.Equal(info.ModTime()) {
		return e.sum, nil
	}
	fh, err := os.Open(h.path(name))
	if err != nil {
		return "", fmt.Errorf("прочитать %s: %w", name, err)
	}
	defer fh.Close()
	sum := sha256.New()
	if _, err := io.Copy(sum, fh); err != nil {
		return "", fmt.Errorf("прочитать %s: %w", name, err)
	}
	s := hex.EncodeToString(sum.Sum(nil))
	h.mu.Lock()
	h.hashes[name] = hashEntry{size: info.Size(), modTime: info.ModTime(), sum: s}
	h.mu.Unlock()
	return s, nil
}

func (h *Handler) catalog(w http.ResponseWriter, _ *http.Request) {
	c, err := h.Catalog()
	if err != nil {
		httpx.WriteError(w, http.StatusInternalServerError, httpx.CodeInternal, err.Error())
		return
	}
	httpx.WriteJSON(w, http.StatusOK, c)
}

// update отвечает драйверу, есть ли DLL новее: ?version=0.2.0&arch=x86_64[&os=linux].
func (h *Handler) update(w http.ResponseWriter, r *http.Request) {
	cur := r.URL.Query().Get("version")
	arch := r.URL.Query().Get("arch")
	osName := r.URL.Query().Get("os")
	if osName != "" && osName != OSWindows && osName != OSLinux {
		httpx.WriteError(w, http.StatusBadRequest, httpx.CodeBadRequest, "os: ожидается windows или linux")
		return
	}
	if _, err := Parse(cur); err != nil {
		httpx.WriteError(w, http.StatusBadRequest, httpx.CodeBadRequest, "version: "+err.Error())
		return
	}
	if arch != ArchX64 && arch != ArchX86 {
		httpx.WriteError(w, http.StatusBadRequest, httpx.CodeBadRequest, "arch: ожидается x86_64 или i386")
		return
	}
	resp := UpdateResponse{Current: cur}
	if f, ok := h.LatestDriver(osName, arch); ok {
		resp.Latest = &f
		resp.UpdateAvailable = Compare(f.Version, cur) > 0
	}
	httpx.WriteJSON(w, http.StatusOK, resp)
}

// file отдаёт файл дистрибутива и пишет скачивание в журнал.
func (h *Handler) file(w http.ResponseWriter, r *http.Request) {
	name := r.PathValue("name")
	if len(h.cfg.Dirs) == 0 {
		httpx.NotFound(w, r)
		return
	}
	if m := reTypePackage.FindStringSubmatch(name); m != nil {
		h.typeFile(w, r, name, m[1], m[2])
		return
	}
	if !(rePackage.MatchString(name) || reDLL.MatchString(name) || reSetup.MatchString(name)) {
		httpx.NotFound(w, r)
		return
	}
	fh, err := os.Open(h.path(name))
	if err != nil {
		httpx.NotFound(w, r)
		return
	}
	defer fh.Close()
	info, err := fh.Stat()
	if err != nil || info.IsDir() {
		httpx.NotFound(w, r)
		return
	}
	h.serve(w, r, name, info.Size(), info.ModTime(), fh)
}

// typeFile — пакет драйвера под тип: собирается из общего пакета версии version.
func (h *Handler) typeFile(w http.ResponseWriter, r *http.Request, name, version, key string) {
	general := "EmulkonDriver_" + version + ".zip"
	info, err := os.Stat(h.path(general))
	if err != nil || info.IsDir() {
		httpx.NotFound(w, r)
		return
	}
	sum, err := h.hash(general, info)
	if err != nil {
		httpx.WriteError(w, http.StatusInternalServerError, httpx.CodeInternal, err.Error())
		return
	}
	p, err := h.typePackage(File{Name: general, Version: version, SHA256: sum}, key)
	if err != nil {
		httpx.NotFound(w, r)
		return
	}
	h.serve(w, r, name, int64(len(p.data)), info.ModTime(), bytes.NewReader(p.data))
}

func (h *Handler) serve(w http.ResponseWriter, r *http.Request, name string, size int64, mod time.Time, content io.ReadSeeker) {
	if h.j != nil && r.Header.Get("Range") == "" {
		payload, _ := journal.Marshal(map[string]any{
			"file": name, "size": size, "remote_addr": r.RemoteAddr, "user_agent": r.UserAgent(),
		})
		_, _ = h.j.Append(journal.Entry{
			WorkstationID: r.Header.Get("X-Emulkon-Workstation"),
			Direction:     journal.DirSystem, Method: "download", Payload: payload,
		})
	}
	ct := "application/octet-stream"
	if strings.HasSuffix(name, ".zip") {
		ct = "application/zip"
	}
	w.Header().Set("Content-Type", ct)
	w.Header().Set("Content-Disposition", `attachment; filename="`+name+`"`)
	w.Header().Set("Cache-Control", "no-cache")
	http.ServeContent(w, r, name, mod, content)
}

// --- версии x.y.z ---

// Parse разбирает версию вида 1.2.3 (суффикс после «-» не учитывается).
func Parse(v string) ([3]int, error) {
	var out [3]int
	if i := strings.IndexByte(v, '-'); i >= 0 {
		v = v[:i]
	}
	parts := strings.Split(v, ".")
	if len(parts) != 3 {
		return out, fmt.Errorf("ожидается версия вида 1.2.3, получено %q", v)
	}
	for i, p := range parts {
		n, err := strconv.Atoi(p)
		if err != nil || n < 0 {
			return out, fmt.Errorf("ожидается версия вида 1.2.3, получено %q", v)
		}
		out[i] = n
	}
	return out, nil
}

// Compare сравнивает версии: -1, 0, 1. Неразборчивая версия считается самой старой.
func Compare(a, b string) int {
	va, ea := Parse(a)
	vb, eb := Parse(b)
	switch {
	case ea != nil && eb != nil:
		return 0
	case ea != nil:
		return -1
	case eb != nil:
		return 1
	}
	for i := range va {
		if va[i] != vb[i] {
			if va[i] < vb[i] {
				return -1
			}
			return 1
		}
	}
	return 0
}
