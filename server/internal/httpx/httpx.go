// Package httpx — общие HTTP-помощники для Device API и Control API.
package httpx

import (
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"log/slog"
	"net/http"
	"strings"

	"emulkon/internal/secret"
)

// MaxBodyBytes — предел тела запроса.
const MaxBodyBytes = 1 << 20

// MaxCommandBytes — предел тела команды драйвера: картинки товаров для весов (Base64)
// приходят пакетами по несколько мегабайт.
const MaxCommandBytes = 32 << 20

// Коды транспортных ошибок (docs/protocol.md, «Ошибки транспорта»).
const (
	CodeBadRequest      = "bad_request"
	CodeUnauthorized    = "unauthorized"
	CodeBadToken        = "bad_token"
	CodeSessionUnknown  = "session_unknown"
	CodeWrongDevice     = "wrong_device"
	CodeUnknownType     = "unknown_equipment_type"
	CodeNotFound        = "not_found"
	CodeRequestConflict = "request_id_conflict"
	CodeInternal        = "internal"
	CodeUnavailable     = "unavailable"
	CodePromptGone      = "prompt_gone" // запрос экрана устройства уже отвечен или истёк
)

// ErrorBody — тело транспортной ошибки.
type ErrorBody struct {
	Error ErrorInfo `json:"error"`
}

type ErrorInfo struct {
	Code    string `json:"code"`
	Message string `json:"message"`
}

// WriteJSON пишет v как JSON с кодом status.
func WriteJSON(w http.ResponseWriter, status int, v any) {
	w.Header().Set("Content-Type", "application/json; charset=utf-8")
	w.Header().Set("Cache-Control", "no-store")
	w.WriteHeader(status)
	enc := json.NewEncoder(w)
	enc.SetEscapeHTML(false)
	if err := enc.Encode(v); err != nil {
		slog.Debug("запись ответа", "err", err)
	}
}

// WriteError пишет транспортную ошибку.
func WriteError(w http.ResponseWriter, status int, code, msg string) {
	WriteJSON(w, status, ErrorBody{Error: ErrorInfo{Code: code, Message: msg}})
}

// DecodeJSON читает тело запроса в v: ограничение размера, запрет лишних данных.
func DecodeJSON(w http.ResponseWriter, r *http.Request, v any) error {
	return DecodeJSONLimit(w, r, v, MaxBodyBytes)
}

// DecodeJSONLimit — DecodeJSON со своим пределом тела.
func DecodeJSONLimit(w http.ResponseWriter, r *http.Request, v any, limit int64) error {
	body := http.MaxBytesReader(w, r.Body, limit)
	dec := json.NewDecoder(body)
	if err := dec.Decode(v); err != nil {
		var mbe *http.MaxBytesError
		if errors.As(err, &mbe) {
			return fmt.Errorf("тело запроса больше %d байт", limit)
		}
		return fmt.Errorf("разобрать JSON: %w", err)
	}
	if _, err := dec.Token(); !errors.Is(err, io.EOF) {
		return errors.New("после JSON-объекта есть лишние данные")
	}
	return nil
}

// BearerToken извлекает токен из заголовка Authorization.
func BearerToken(r *http.Request) string {
	h := r.Header.Get("Authorization")
	if len(h) > 7 && strings.EqualFold(h[:7], "bearer ") {
		return strings.TrimSpace(h[7:])
	}
	return ""
}

// RequireToken — middleware: если токен задан, требует Authorization: Bearer <токен>.
func RequireToken(token secret.Token, next http.Handler) http.Handler {
	if token.Empty() {
		return next
	}
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if !token.Match(BearerToken(r)) {
			w.Header().Set("WWW-Authenticate", `Bearer realm="emulkon-control"`)
			WriteError(w, http.StatusUnauthorized, CodeUnauthorized, "нужен токен Control API (Authorization: Bearer …)")
			return
		}
		next.ServeHTTP(w, r)
	})
}

// Recover — middleware: паника в обработчике превращается в 500, сервер живёт.
func Recover(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		defer func() {
			if v := recover(); v != nil {
				if v == http.ErrAbortHandler {
					panic(v)
				}
				slog.Error("паника в обработчике", "path", r.URL.Path, "panic", v)
				WriteError(w, http.StatusInternalServerError, CodeInternal, "внутренняя ошибка сервера")
			}
		}()
		next.ServeHTTP(w, r)
	})
}

// NotFound — JSON-ответ 404 для неизвестных путей API.
func NotFound(w http.ResponseWriter, r *http.Request) {
	WriteError(w, http.StatusNotFound, CodeNotFound, "нет такого метода API: "+r.Method+" "+r.URL.Path)
}
