// Package updates — новые выпуски Эмулкона (docs/local-install.md §2.3, issue #8).
//
// Сервер раз в сутки читает описание последнего выпуска (latest.json релиза GitHub) и только сообщает. Установщик
// качает трей или консоль по «Обновить» — в «Загрузки» пользователя с меткой «из Интернета», со сверкой размера и
// SHA-256 (#20: exe, тихо скачанный службой в общий каталог, антивирус считает подозрительным); ставит человек
// (UAC). Сервер ничего не качает, не запускает и не ставит. О новой версии — запись update.available в журнале (её ловит
// трей) и GET /api/v1/updates (веб-интерфейс, консоль). Проверка вручную — POST /control/v1/updates/check.
package updates

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"log/slog"
	"net/http"
	"os"
	"regexp"
	"strconv"
	"strings"
	"sync"
	"time"

	"emulkon/internal/httpx"
	"emulkon/internal/journal"
	"emulkon/internal/secret"
)

// DefaultFeed — latest.json последнего обычного (не предварительного) выпуска.
const DefaultFeed = "https://github.com/iMironRU/emulkon/releases/latest/download/latest.json"

// Режимы (UpdateMode): off — не проверять; notify — проверять и сообщать.
const (
	ModeOff    = "off"
	ModeNotify = "notify"
)

// Методы журнала.
const (
	MethodCheck     = "update.check"     // каждая проверка: что нашли или почему не вышло
	MethodAvailable = "update.available" // новая версия — один раз на версию (трей показывает уведомление)
)

// Release — описание выпуска (latest.json, .github/workflows/release.yml).
type Release struct {
	Version   string `json:"version"`
	Published string `json:"published,omitempty"`
	URL       string `json:"url"`
	SHA256    string `json:"sha256"`
	Size      int64  `json:"size,omitempty"`
	Notes     string `json:"notes,omitempty"`
}

// State — что известно о выпусках; отдаётся в GET /api/v1/updates и хранится в data/updates.json.
type State struct {
	Current   string    `json:"current"`
	Mode      string    `json:"mode"`
	CheckedAt time.Time `json:"checked_at,omitzero"`
	Error     string    `json:"error,omitempty"`
	Latest    *Release  `json:"latest,omitempty"`
	Available bool      `json:"available"`          // Latest новее текущей версии
	Page      string    `json:"page,omitempty"`     // страница выпуска: что нового, файлы для других ОС
	Notified  string    `json:"notified,omitempty"` // версия, о которой уже написали в журнал
}

// Config — настройки проверки.
type Config struct {
	Version    string        // текущая версия сервера
	Mode       string        // off | notify (пусто — notify)
	FeedURL    string        // latest.json; пусто — DefaultFeed
	AllowHTTP  bool          // адрес установщика может быть http (своё описание выпуска — проверки, стенды)
	Interval   time.Duration // между проверками; 0 — сутки
	FirstDelay time.Duration // первая проверка после запуска; 0 — минута
	StatePath  string        // файл состояния; пусто — только в памяти
	Token      secret.Token  // токен Control API — для POST /control/v1/updates/check
	Client     *http.Client
}

// Checker — проверка выпусков.
type Checker struct {
	cfg Config
	j   *journal.Journal
	mu  sync.Mutex // st
	st  State
	run sync.Mutex // одна проверка за раз
}

func New(cfg Config, j *journal.Journal) *Checker {
	if cfg.Mode == "" {
		cfg.Mode = ModeNotify
	}
	if cfg.FeedURL == "" {
		cfg.FeedURL = DefaultFeed
	}
	if cfg.Interval <= 0 {
		cfg.Interval = 24 * time.Hour
	}
	if cfg.FirstDelay <= 0 {
		cfg.FirstDelay = time.Minute
	}
	if cfg.Client == nil {
		cfg.Client = &http.Client{Timeout: 5 * time.Minute}
	}
	c := &Checker{cfg: cfg, j: j}
	c.st = c.load()
	c.st.Current, c.st.Mode = cfg.Version, cfg.Mode
	c.st.Available = c.st.Latest != nil && Compare(c.st.Latest.Version, cfg.Version) > 0
	return c
}

// ValidMode — допустимое значение UpdateMode.
func ValidMode(m string) bool { return m == ModeOff || m == ModeNotify }

// State — копия состояния.
func (c *Checker) State() State {
	c.mu.Lock()
	defer c.mu.Unlock()
	st := c.st
	if st.Latest != nil {
		l := *st.Latest
		st.Latest = &l
	}
	return st
}

// Run проверяет через FirstDelay и затем раз в Interval, пока не отменят ctx. Режим off — ничего.
func (c *Checker) Run(ctx context.Context) {
	if c.cfg.Mode == ModeOff {
		return
	}
	t := time.NewTimer(c.cfg.FirstDelay)
	defer t.Stop()
	for {
		select {
		case <-ctx.Done():
			return
		case <-t.C:
			if _, err := c.Check(ctx); err != nil {
				slog.Warn("проверка обновлений", "err", err)
			}
			t.Reset(c.cfg.Interval)
		}
	}
}

// Check — проверить сейчас: описание выпуска, журнал, файл состояния. Установщик сервер не качает (#20).
func (c *Checker) Check(ctx context.Context) (State, error) {
	c.run.Lock()
	defer c.run.Unlock()
	rel, err := c.fetch(ctx)
	st := c.State()
	st.CheckedAt = time.Now().UTC()
	if err != nil {
		st.Error = err.Error()
		c.save(st)
		c.journal(MethodCheck, map[string]any{"error": st.Error})
		return st, err
	}
	st.Error = ""
	st.Latest = &rel
	st.Page = releasePage(rel.URL, rel.Version)
	st.Available = Compare(rel.Version, c.cfg.Version) > 0
	c.journal(MethodCheck, map[string]any{"latest": rel.Version, "available": st.Available})
	if st.Available && st.Notified != rel.Version {
		c.journal(MethodAvailable, map[string]any{"version": rel.Version, "published": rel.Published, "page": st.Page,
			"url": rel.URL, "sha256": rel.SHA256, "notes": rel.Notes})
		st.Notified = rel.Version
	}
	c.save(st)
	return st, nil
}

// Register — GET /api/v1/updates (открыто: в нём нет секретов) и POST /control/v1/updates/check (токен управления).
func (c *Checker) Register(mux *http.ServeMux) {
	mux.HandleFunc("GET /api/v1/updates", func(w http.ResponseWriter, _ *http.Request) {
		httpx.WriteJSON(w, http.StatusOK, c.State())
	})
	mux.Handle("POST /control/v1/updates/check", httpx.RequireToken(c.cfg.Token, http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		st, _ := c.Check(r.Context()) // ошибка — в st.Error
		httpx.WriteJSON(w, http.StatusOK, st)
	})))
}

var (
	reVersion = regexp.MustCompile(`^(\d+)\.(\d+)\.(\d+)(?:-([0-9A-Za-z.]+))?$`)
	reSHA256  = regexp.MustCompile(`^[0-9a-fA-F]{64}$`)
)

func (c *Checker) fetch(ctx context.Context) (Release, error) {
	var rel Release
	req, err := http.NewRequestWithContext(ctx, http.MethodGet, c.cfg.FeedURL, nil)
	if err != nil {
		return rel, fmt.Errorf("адрес описания выпуска: %w", err)
	}
	req.Header.Set("User-Agent", "Emulkon/"+c.cfg.Version)
	resp, err := c.cfg.Client.Do(req)
	if err != nil {
		return rel, fmt.Errorf("нет связи с %s: %w", c.cfg.FeedURL, err)
	}
	defer resp.Body.Close()
	if resp.StatusCode == http.StatusNotFound {
		return rel, fmt.Errorf("описания выпуска нет (%s)", c.cfg.FeedURL)
	}
	if resp.StatusCode != http.StatusOK {
		return rel, fmt.Errorf("HTTP %d от %s", resp.StatusCode, c.cfg.FeedURL)
	}
	if err := json.NewDecoder(io.LimitReader(resp.Body, 1<<20)).Decode(&rel); err != nil {
		return rel, fmt.Errorf("описание выпуска — не JSON: %w", err)
	}
	return rel, validate(rel, c.cfg.AllowHTTP)
}

// validate — та же проверка, что у консоли (console/src/maintenance.cpp): версия X.Y.Z, установщик
// EmulkonSetup_<версия>.exe только по https, SHA-256 — 64 шестнадцатеричных символа.
func validate(r Release, allowHTTP bool) error {
	if !reVersion.MatchString(r.Version) {
		return fmt.Errorf("версия выпуска не по образцу X.Y.Z: %q", r.Version)
	}
	if !strings.HasPrefix(r.URL, "https://") && !(allowHTTP && strings.HasPrefix(r.URL, "http://")) {
		return fmt.Errorf("адрес установщика не https: %s", r.URL)
	}
	if !strings.HasSuffix(r.URL, "/EmulkonSetup_"+r.Version+".exe") {
		return fmt.Errorf("адрес установщика не ведёт на EmulkonSetup_%s.exe", r.Version)
	}
	if !reSHA256.MatchString(r.SHA256) {
		return errors.New("sha256 в описании выпуска — не 64 шестнадцатеричных символа")
	}
	return nil
}

// releasePage — страница выпуска по адресу установщика .../releases/download/vX/EmulkonSetup_X.exe.
func releasePage(url, version string) string {
	i := strings.Index(url, "/releases/download/")
	if i < 0 {
		return ""
	}
	return url[:i] + "/releases/tag/v" + version
}

func (c *Checker) journal(method string, payload map[string]any) {
	if c.j == nil {
		return
	}
	data, _ := journal.Marshal(payload)
	if _, err := c.j.Append(journal.Entry{Direction: journal.DirSystem, Method: method, Payload: data}); err != nil {
		slog.Error("запись в журнал", "method", method, "err", err)
	}
}

func (c *Checker) load() State {
	var st State
	if c.cfg.StatePath == "" {
		return st
	}
	data, err := os.ReadFile(c.cfg.StatePath)
	if err != nil {
		return st
	}
	if err := json.Unmarshal(data, &st); err != nil {
		slog.Warn("файл состояния обновлений не прочитан", "path", c.cfg.StatePath, "err", err)
		return State{}
	}
	return st
}

func (c *Checker) save(st State) {
	c.mu.Lock()
	c.st = st
	c.mu.Unlock()
	if c.cfg.StatePath == "" {
		return
	}
	data, _ := json.MarshalIndent(st, "", "  ")
	tmp := c.cfg.StatePath + ".tmp"
	if err := os.WriteFile(tmp, data, 0o644); err == nil {
		err = os.Rename(tmp, c.cfg.StatePath)
		if err != nil {
			slog.Warn("файл состояния обновлений", "err", err)
		}
	}
}

// Compare — SemVer без сборки: <0, 0, >0. Предварительная (0.9.0-rc1) младше выпуска 0.9.0; неразборчивая — младше.
func Compare(a, b string) int {
	ma, mb := reVersion.FindStringSubmatch(a), reVersion.FindStringSubmatch(b)
	switch {
	case ma == nil && mb == nil:
		return strings.Compare(a, b)
	case ma == nil:
		return -1
	case mb == nil:
		return 1
	}
	for i := 1; i <= 3; i++ {
		x, _ := strconv.Atoi(ma[i])
		y, _ := strconv.Atoi(mb[i])
		if x != y {
			if x < y {
				return -1
			}
			return 1
		}
	}
	switch {
	case ma[4] == mb[4]:
		return 0
	case ma[4] == "":
		return 1
	case mb[4] == "":
		return -1
	}
	return strings.Compare(ma[4], mb[4])
}
