// Package kkt — модель ККТ с передачей в ОФД (раздел 3.5 требований к драйверам).
//
// Этот файл и xml.go — XML-структуры обмена с БПО (docs/bpo-requirements/kkt-xml.md).
// Правила: вход разбирается снисходительно (неизвестные атрибуты игнорируются,
// обязательность не проверяется — БПО шлёт не всё), выход — полный и без пустых
// числовых атрибутов (БПО делает Число() без проверки).
package kkt

import (
	"encoding/xml"
	"fmt"
	"math"
	"strconv"
	"strings"
	"time"
)

// Bool — булево в XML БПО. Читается без учёта регистра (true/True/TRUE, 1/0),
// пустое — false. Пишется как true/false: БПО сравнивает через ВРег(…) = "TRUE".
type Bool bool

func (b Bool) MarshalXMLAttr(name xml.Name) (xml.Attr, error) {
	return xml.Attr{Name: name, Value: strconv.FormatBool(bool(b))}, nil
}

func (b *Bool) UnmarshalXMLAttr(attr xml.Attr) error {
	switch strings.ToLower(strings.TrimSpace(attr.Value)) {
	case "true", "1":
		*b = true
	case "false", "0", "":
		*b = false
	default:
		return fmt.Errorf("атрибут %s: ожидается булево, получено %q", attr.Name.Local, attr.Value)
	}
	return nil
}

// BoolPtr — указатель на Bool для необязательных атрибутов.
func BoolPtr(v bool) *Bool { b := Bool(v); return &b }

// DateTimeLayout — xs:dateTime без часового пояса, как пишет и читает БПО
// (XMLСтрока / XMLЗначение(Тип("Дата"), …)).
const DateTimeLayout = "2006-01-02T15:04:05"

// DateTime — дата и время в XML БПО. Нулевое значение не пишется (атрибут отсутствует).
type DateTime struct{ time.Time }

func (d DateTime) MarshalXMLAttr(name xml.Name) (xml.Attr, error) {
	if d.IsZero() {
		return xml.Attr{}, nil // пустое имя — атрибут не пишется
	}
	return xml.Attr{Name: name, Value: d.Format(DateTimeLayout)}, nil
}

func (d *DateTime) UnmarshalXMLAttr(attr xml.Attr) error {
	s := strings.TrimSpace(attr.Value)
	if s == "" {
		d.Time = time.Time{}
		return nil
	}
	for _, layout := range []string{DateTimeLayout, time.RFC3339Nano, "2006-01-02T15:04:05.999999999", "2006-01-02"} {
		if t, err := time.ParseInLocation(layout, s, time.Local); err == nil {
			d.Time = t
			return nil
		}
	}
	return fmt.Errorf("атрибут %s: ожидается дата и время, получено %q", attr.Name.Local, attr.Value)
}

// Number — число (double/decimal) в XML БПО. Пишется десятичной записью без
// экспоненты: 1000000, а не 1e+06 — иначе Число() в 1С не разберёт.
type Number float64

func (n Number) MarshalXMLAttr(name xml.Name) (xml.Attr, error) {
	return xml.Attr{Name: name, Value: strconv.FormatFloat(float64(n), 'f', -1, 64)}, nil
}

func (n *Number) UnmarshalXMLAttr(attr xml.Attr) error {
	s := strings.TrimSpace(attr.Value)
	if s == "" {
		*n = 0
		return nil
	}
	// 1С пишет через точку; запятую принимаем на случай ручных пакетов.
	f, err := strconv.ParseFloat(strings.Replace(s, ",", ".", 1), 64)
	if err != nil {
		return fmt.Errorf("атрибут %s: ожидается число, получено %q", attr.Name.Local, attr.Value)
	}
	*n = Number(f)
	return nil
}

// NumberPtr — указатель на Number для необязательных атрибутов.
func NumberPtr(v float64) *Number { n := Number(v); return &n }

// IntPtr — указатель на int для необязательных атрибутов.
func IntPtr(v int) *int { return &v }

// Kopecks округляет сумму до копеек.
func Kopecks(v float64) float64 { return math.Round(v*100) / 100 }
