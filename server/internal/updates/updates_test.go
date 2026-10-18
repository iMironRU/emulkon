package updates

import (
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"emulkon/internal/journal"
)

type feed struct {
	srv       *httptest.Server
	release   Release
	installer []byte
	hits      map[string]int
}

func newFeed(t *testing.T, version string) *feed {
	t.Helper()
	f := &feed{installer: []byte("MZ installer " + version), hits: map[string]int{}}
	f.srv = httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		f.hits[r.URL.Path]++
		switch {
		case r.URL.Path == "/latest.json":
			json.NewEncoder(w).Encode(f.release)
		case strings.HasSuffix(r.URL.Path, ".exe"):
			w.Write(f.installer)
		default:
			http.NotFound(w, r)
		}
	}))
	t.Cleanup(f.srv.Close)
	sum := sha256.Sum256(f.installer)
	f.release = Release{Version: version, Published: "2026-11-01",
		URL:    f.srv.URL + "/releases/download/v" + version + "/EmulkonSetup_" + version + ".exe",
		SHA256: hex.EncodeToString(sum[:]), Size: int64(len(f.installer)), Notes: "### Добавлено\n- обновления"}
	return f
}

func openJournal(t *testing.T) *journal.Journal {
	t.Helper()
	j, err := journal.Open(journal.Options{Dir: filepath.Join(t.TempDir(), "journal")})
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { j.Close() })
	return j
}

func entries(t *testing.T, j *journal.Journal, method string) []journal.Entry {
	t.Helper()
	var all []journal.Entry
	if err := j.Scan(journal.Filter{Method: method}, func(e *journal.Entry, _ []byte) bool {
		all = append(all, *e)
		return true
	}); err != nil {
		t.Fatal(err)
	}
	return all
}

func TestCompare(t *testing.T) {
	cases := []struct {
		a, b string
		want int
	}{
		{"0.10.0", "0.9.9", 1}, {"0.9.0", "0.9.0", 0}, {"0.9.0-rc1", "0.9.0", -1}, {"1.0.0", "0.99.99", 1},
		{"мусор", "0.0.1", -1}, {"0.9.1", "0.9.0-dev", 1},
	}
	for _, c := range cases {
		if got := Compare(c.a, c.b); got != c.want {
			t.Errorf("Compare(%q, %q) = %d, ждали %d", c.a, c.b, got, c.want)
		}
	}
}

func TestCheckNotifiesOnceAndDownloadsNothing(t *testing.T) {
	f := newFeed(t, "0.9.1")
	dir := t.TempDir()
	j := openJournal(t)
	cfg := Config{Version: "0.9.0", FeedURL: f.srv.URL + "/latest.json", AllowHTTP: true,
		StatePath: filepath.Join(dir, "updates.json")}
	c := New(cfg, j)
	st, err := c.Check(context.Background())
	if err != nil {
		t.Fatal(err)
	}
	if !st.Available || st.Latest.Version != "0.9.1" || st.Latest.SHA256 != f.release.SHA256 || st.Notified != "0.9.1" {
		t.Fatalf("состояние: %+v", st)
	}
	if !strings.HasSuffix(st.Page, "/releases/tag/v0.9.1") {
		t.Fatalf("страница выпуска: %q", st.Page)
	}
	// Вторая проверка: уведомление — только раз на версию; установщик сервер не качает никогда (#20).
	if _, err := c.Check(context.Background()); err != nil {
		t.Fatal(err)
	}
	if n := f.hits["/releases/download/v0.9.1/EmulkonSetup_0.9.1.exe"]; n != 0 {
		t.Fatalf("установщик скачивали %d раз", n)
	}
	if files, _ := os.ReadDir(dir); len(files) != 1 {
		t.Fatalf("кроме файла состояния, ничего: %v", files)
	}
	e := entries(t, j, MethodAvailable)
	if len(e) != 1 || !strings.Contains(string(e[0].Payload), f.release.URL) {
		t.Fatalf("update.available: %v", e)
	}
	if n := len(entries(t, j, MethodCheck)); n != 2 {
		t.Fatalf("update.check: %d записей", n)
	}
	// Состояние переживает перезапуск; после обновления до 0.9.1 предложения нет.
	if st := New(cfg, j).State(); !st.Available {
		t.Fatalf("после перезапуска: %+v", st)
	}
	cfg.Version = "0.9.1"
	if st := New(cfg, j).State(); st.Available {
		t.Fatalf("после обновления: %+v", st)
	}
}

func TestRejectsBadRelease(t *testing.T) {
	f := newFeed(t, "0.9.1")
	ok := f.release
	bad := []func(r *Release){
		func(r *Release) { r.URL = strings.Replace(r.URL, "http://", "ftp://", 1) },
		func(r *Release) { r.URL = strings.Replace(r.URL, "EmulkonSetup_0.9.1.exe", "evil.exe", 1) },
		func(r *Release) { r.SHA256 = "abc" },
		func(r *Release) { r.Version = "0.9" },
	}
	for i, mutate := range bad {
		f.release = ok
		mutate(&f.release)
		st, err := New(Config{Version: "0.9.0", FeedURL: f.srv.URL + "/latest.json", AllowHTTP: true}, nil).Check(context.Background())
		if err == nil || st.Available {
			t.Errorf("случай %d: принято %+v", i, f.release)
		}
	}
	// https обязателен, если описание выпуска не своё.
	f.release = ok
	if _, err := New(Config{Version: "0.9.0", FeedURL: f.srv.URL + "/latest.json"}, nil).Check(context.Background()); err == nil ||
		!strings.Contains(err.Error(), "не https") {
		t.Fatalf("http без разрешения: %v", err)
	}
}

func TestNoReleaseYet(t *testing.T) {
	srv := httptest.NewServer(http.NotFoundHandler())
	defer srv.Close()
	st, err := New(Config{Version: "0.9.0", FeedURL: srv.URL + "/latest.json"}, nil).Check(context.Background())
	if err == nil || !strings.Contains(st.Error, "описания выпуска нет") || st.Available {
		t.Fatalf("нет описания: %+v, %v", st, err)
	}
}

func TestModeOffDoesNothing(t *testing.T) {
	f := newFeed(t, "0.9.1")
	c := New(Config{Version: "0.9.0", Mode: ModeOff, FeedURL: f.srv.URL + "/latest.json", AllowHTTP: true, FirstDelay: time.Millisecond}, nil)
	ctx, cancel := context.WithTimeout(context.Background(), 50*time.Millisecond)
	defer cancel()
	c.Run(ctx)
	if len(f.hits) != 0 {
		t.Fatalf("режим off — запросы: %v", f.hits)
	}
}

func TestAPI(t *testing.T) {
	f := newFeed(t, "0.9.1")
	c := New(Config{Version: "0.9.0", FeedURL: f.srv.URL + "/latest.json", AllowHTTP: true}, nil)
	mux := http.NewServeMux()
	c.Register(mux)
	srv := httptest.NewServer(mux)
	defer srv.Close()
	resp, err := http.Post(srv.URL+"/control/v1/updates/check", "application/json", nil)
	if err != nil || resp.StatusCode != 200 {
		t.Fatalf("проверка вручную: %v %v", resp, err)
	}
	resp.Body.Close()
	resp, err = http.Get(srv.URL + "/api/v1/updates")
	if err != nil {
		t.Fatal(err)
	}
	defer resp.Body.Close()
	var st State
	json.NewDecoder(resp.Body).Decode(&st)
	if !st.Available || st.Current != "0.9.0" || st.Mode != ModeNotify || st.Latest == nil {
		t.Fatalf("GET /api/v1/updates: %+v", st)
	}
}
