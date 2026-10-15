// Package web — встроенный веб-UI (ARCHITECTURE §9.2): статика без сборки,
// работает поверх Control API.
package web

import (
	"embed"
	"io/fs"
	"net/http"
)

//go:embed static
var static embed.FS

// Handler отдаёт статику UI.
func Handler() http.Handler {
	fileServer := http.FileServer(http.FS(sub()))
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Cache-Control", "no-cache")
		w.Header().Set("X-Content-Type-Options", "nosniff")
		fileServer.ServeHTTP(w, r)
	})
}

// Page отдаёт одну HTML-страницу статики по любому пути (например, /downloads).
func Page(name string) http.Handler {
	body, err := fs.ReadFile(sub(), name)
	if err != nil {
		panic(err)
	}
	return http.HandlerFunc(func(w http.ResponseWriter, _ *http.Request) {
		w.Header().Set("Content-Type", "text/html; charset=utf-8")
		w.Header().Set("Cache-Control", "no-cache")
		w.Header().Set("X-Content-Type-Options", "nosniff")
		_, _ = w.Write(body)
	})
}

func sub() fs.FS {
	s, err := fs.Sub(static, "static")
	if err != nil {
		panic(err)
	}
	return s
}
