// Package printers — модели принтера чеков (раздел 3.6 требований к драйверам) и принтера
// этикеток (3.7): docs/bpo-requirements/printers.md, поведение — docs/models/printers.md.
package printers

import (
	"encoding/base64"
	"encoding/json"
	"errors"
	"fmt"
	"strings"
	"time"
	"unicode/utf8"

	"emulkon/internal/models"
)

// Коды ошибок (docs/models/printers.md).
const (
	ErrNoPaper       = 90
	ErrCoverOpen     = 91
	ErrBarcodeType   = 92
	ErrPackageStatus = 93
	ErrNoLayout      = 94
	ErrCheckState    = 95 // старая схема: чек не открыт или уже открыт
	ErrBadParams     = 98
	ErrNotSupported  = 99
)

var errorTexts = map[int]string{
	ErrNoPaper:       "Нет бумаги",
	ErrCoverOpen:     "Крышка принтера открыта",
	ErrBarcodeType:   "Тип штрихкода не поддерживается",
	ErrPackageStatus: "Неверный статус пакета",
	ErrNoLayout:      "Нет разметки этикетки: задание печати не начато",
	ErrCheckState:    "Неверное состояние чека",
	ErrBadParams:     "Неверные параметры вызова",
	ErrNotSupported:  "Метод не поддерживается эмулятором принтера",
}

type devError struct {
	code   int
	detail string
}

func (e *devError) Error() string {
	if e.detail == "" {
		return errorTexts[e.code]
	}
	return errorTexts[e.code] + ": " + e.detail
}

func fail(code int, format string, args ...any) *devError {
	return &devError{code: code, detail: fmt.Sprintf(format, args...)}
}

// result — ответ метода: ошибка устройства или успех с параметрами out.
func result(err error, out []any) models.Result {
	if err != nil {
		var de *devError
		if !errors.As(err, &de) {
			de = fail(ErrBadParams, "%v", err)
		}
		return models.Result{Return: false, ErrorCode: de.code, ErrorText: de.Error()}
	}
	return models.Result{Return: true, Params: out}
}

// Options — параметры модели.
type Options struct {
	Version string
	Now     func() time.Time
}

func (o *Options) defaults() {
	if o.Now == nil {
		o.Now = time.Now
	}
}

// Типы штрихкода разделов 3.6 и 3.7 (QR и QRCode — одно и то же), без учёта регистра.
var barcodeTypes = map[string]bool{
	"EAN8": true, "EAN13": true, "EAN128": true, "CODE39": true, "CODE128": true, "CODE16K": true,
	"CODE93": true, "PDF417": true, "DATAMATRIX": true, "QR": true, "QRCODE": true, "ITF14": true,
	"EAN13ADDON2": true, "EAN13ADDON5": true, "GS1DATABAREXPANDEDSTACKED": true,
}

func barcodeKnown(t string) bool { return barcodeTypes[strings.ToUpper(strings.TrimSpace(t))] }

// decodeBase64 — значение штрихкода из Base64; не Base64 — как есть.
func decodeBase64(s string) string {
	if b, err := base64.StdEncoding.DecodeString(strings.TrimSpace(s)); err == nil {
		return string(b)
	}
	return s
}

const emuMark = "*** ЭМУЛЯТОР · НЕ ФИСКАЛЬНЫЙ ***"

func center(s string, w int) string {
	n := utf8.RuneCountInString(s)
	if n >= w {
		return s
	}
	return strings.Repeat(" ", (w-n)/2) + s
}

// wrap переносит строку по ширине w символов.
func wrap(s string, w int) []string {
	r := []rune(s)
	if len(r) <= w {
		return []string{s}
	}
	var out []string
	for len(r) > w {
		out = append(out, string(r[:w]))
		r = r[w:]
	}
	if len(r) > 0 {
		out = append(out, string(r))
	}
	return out
}

// tape — лента печати принтера.
type tape struct {
	docs []models.PrintedDocument
	seq  int64
}

const tapeLimit = 200

func (t *tape) add(d models.PrintedDocument) models.PrintedDocument {
	t.seq++
	d.Seq = t.seq
	t.docs = append(t.docs, d)
	if len(t.docs) > tapeLimit {
		t.docs = t.docs[len(t.docs)-tapeLimit:]
	}
	return d
}

func (t *tape) last(limit int) []models.PrintedDocument {
	out := []models.PrintedDocument{}
	for i := len(t.docs) - 1; i >= 0 && len(out) < limit; i-- {
		out = append(out, t.docs[i])
	}
	return out
}

func (t *tape) since(seq int64) []models.PrintedDocument {
	var out []models.PrintedDocument
	for _, d := range t.docs {
		if d.Seq > seq {
			out = append(out, d)
		}
	}
	return out
}

// device — бумага и крышка: общее для принтеров.
type device struct {
	paper bool
	cover bool // крышка открыта
}

func (d device) ready() error {
	switch {
	case !d.paper:
		return &devError{code: ErrNoPaper}
	case d.cover:
		return &devError{code: ErrCoverOpen}
	}
	return nil
}

func asBool(key string, v any) (bool, error) {
	b, ok := v.(bool)
	if !ok {
		return false, fmt.Errorf("%s: ожидается true или false", key)
	}
	return b, nil
}

func asNumber(key string, v any) (float64, error) {
	switch n := v.(type) {
	case float64:
		return n, nil
	case json.Number:
		return n.Float64()
	}
	return 0, fmt.Errorf("%s: ожидается число", key)
}
