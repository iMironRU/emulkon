// Package journal — журнал обменов (ARCHITECTURE §8).
//
// Append-only поток записей в файлах JSONL с ротацией по размеру, чтением
// с фильтрами и живой подпиской. Каждая запись получает монотонный номер seq,
// который переживает перезапуск сервера и служит курсором для потока (SSE).
package journal

import (
	"bytes"
	"encoding/json"
	"fmt"
	"strings"
	"time"
	"unicode/utf8"
)

// Направления записей журнала.
const (
	DirIn     = "in"     // команда от компоненты
	DirOut    = "out"    // ответ компоненте
	DirEvent  = "event"  // событие сервер → компонента
	DirSystem = "system" // служебное: сессии, сброс, управление
)

// Entry — запись журнала. Поля из ARCHITECTURE §8 плюс служебные
// seq, session_id и replay (повтор запроса с тем же request_id).
type Entry struct {
	Seq           int64           `json:"seq"`
	TS            time.Time       `json:"ts"`
	CorrelationID string          `json:"correlation_id,omitempty"`
	SessionID     string          `json:"session_id,omitempty"`
	WorkstationID string          `json:"workstation_id,omitempty"`
	DeviceID      string          `json:"device_id,omitempty"`
	Direction     string          `json:"direction"`
	Method        string          `json:"method,omitempty"`
	Payload       json.RawMessage `json:"payload,omitempty"`
	LatencyMS     *int64          `json:"latency_ms,omitempty"`
	AppliedRules  []string        `json:"applied_rules,omitempty"`
	Replay        bool            `json:"replay,omitempty"`
}

// Marshal — JSON без HTML-экранирования: XML-пакеты драйверов остаются читаемыми
// в журнале («<CheckPackage …>», а не «<CheckPackage …»), и по ним работает поиск.
func Marshal(v any) ([]byte, error) {
	var buf bytes.Buffer
	enc := json.NewEncoder(&buf)
	enc.SetEscapeHTML(false)
	if err := enc.Encode(v); err != nil {
		return nil, err
	}
	return bytes.TrimRight(buf.Bytes(), "\n"), nil
}

// MaxParamBytes — сколько байт строкового параметра команды попадает в журнал. Длиннее —
// картинки товаров для весов (Base64): журнал хранит начало и размер.
const MaxParamBytes = 128 << 10

// ClipParams — копия params для журнала: длинные строки обрезаны до MaxParamBytes.
func ClipParams(params []any) []any {
	var out []any
	for i, p := range params {
		s, ok := p.(string)
		if !ok || len(s) <= MaxParamBytes {
			continue
		}
		if out == nil {
			out = append([]any(nil), params...)
		}
		cut := MaxParamBytes
		for cut > 0 && !utf8.RuneStart(s[cut]) {
			cut--
		}
		out[i] = s[:cut] + fmt.Sprintf("…[в журнале обрезано: %d байт]", len(s))
	}
	if out == nil {
		return params
	}
	return out
}

// Filter — условия выборки. Пустое поле не ограничивает.
type Filter struct {
	DeviceID      string
	WorkstationID string
	SessionID     string
	Direction     string
	Method        string // одно имя или несколько через запятую: device.print,device.prompt
	CorrelationID string
	Since         time.Time // ts >= Since
	Until         time.Time // ts < Until
	AfterSeq      int64     // seq > AfterSeq
	BeforeSeq     int64     // seq < BeforeSeq (0 — без ограничения)
	Text          string    // подстрока в сыром JSON записи, без учёта регистра
}

// Match проверяет запись по всем условиям, кроме Text (он проверяется по сырой строке).
func (f *Filter) Match(e *Entry) bool {
	switch {
	case f.DeviceID != "" && e.DeviceID != f.DeviceID,
		f.WorkstationID != "" && e.WorkstationID != f.WorkstationID,
		f.SessionID != "" && e.SessionID != f.SessionID,
		f.Direction != "" && e.Direction != f.Direction,
		f.Method != "" && !matchMethod(f.Method, e.Method),
		f.CorrelationID != "" && e.CorrelationID != f.CorrelationID,
		!f.Since.IsZero() && e.TS.Before(f.Since),
		!f.Until.IsZero() && !e.TS.Before(f.Until),
		e.Seq <= f.AfterSeq,
		f.BeforeSeq > 0 && e.Seq >= f.BeforeSeq:
		return false
	}
	return true
}

// matchMethod — метод записи есть в списке через запятую.
func matchMethod(list, method string) bool {
	if list == method {
		return true
	}
	for _, m := range strings.Split(list, ",") {
		if strings.TrimSpace(m) == method {
			return true
		}
	}
	return false
}

// matchText проверяет подстроку в сырой строке записи.
func (f *Filter) matchText(raw []byte) bool {
	if f.Text == "" {
		return true
	}
	return bytes.Contains(bytes.ToLower(raw), []byte(strings.ToLower(f.Text)))
}

// MatchEntry — полная проверка записи, включая Text (для живого потока).
func (f *Filter) MatchEntry(e *Entry) bool {
	if !f.Match(e) {
		return false
	}
	if f.Text == "" {
		return true
	}
	raw, err := Marshal(e)
	return err == nil && f.matchText(raw)
}
