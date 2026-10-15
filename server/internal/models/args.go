package models

import (
	"encoding/json"
	"fmt"
	"strconv"
	"strings"
)

// Args — позиционные параметры вызова с типизированным доступом по номеру.
// Значения приходят как string, json.Number, bool или nil (docs/protocol.md §1.2).
type Args []any

// Need проверяет, что параметров не меньше n.
func (a Args) Need(n int) error {
	if len(a) < n {
		return fmt.Errorf("ожидается не меньше %d параметров, передано %d", n, len(a))
	}
	return nil
}

// String — STRING. null — пустая строка; число и булево приводятся к строке.
func (a Args) String(i int) (string, error) {
	if i >= len(a) {
		return "", fmt.Errorf("нет параметра %d", i)
	}
	switch v := a[i].(type) {
	case nil:
		return "", nil
	case string:
		return v, nil
	case json.Number:
		return v.String(), nil
	case bool:
		return strconv.FormatBool(v), nil
	default:
		return "", fmt.Errorf("параметр %d: неподдерживаемый тип %T", i, v)
	}
}

// Bool — BOOL. Принимает булево, строку true/false/1/0 и число 0/1; null — false.
func (a Args) Bool(i int) (bool, error) {
	if i >= len(a) {
		return false, fmt.Errorf("нет параметра %d", i)
	}
	switch v := a[i].(type) {
	case nil:
		return false, nil
	case bool:
		return v, nil
	case string:
		switch strings.ToLower(strings.TrimSpace(v)) {
		case "true", "1":
			return true, nil
		case "false", "0", "":
			return false, nil
		}
	case json.Number:
		switch v.String() {
		case "1":
			return true, nil
		case "0":
			return false, nil
		}
	}
	return false, fmt.Errorf("параметр %d: ожидается булево, получено %v", i, a[i])
}

// Int — LONG. Принимает число и строку с числом; null — 0.
func (a Args) Int(i int) (int64, error) {
	if i >= len(a) {
		return 0, fmt.Errorf("нет параметра %d", i)
	}
	var s string
	switch v := a[i].(type) {
	case nil:
		return 0, nil
	case json.Number:
		s = v.String()
	case string:
		s = strings.TrimSpace(v)
	default:
		return 0, fmt.Errorf("параметр %d: ожидается целое, получено %v", i, a[i])
	}
	n, err := strconv.ParseInt(s, 10, 64)
	if err != nil {
		return 0, fmt.Errorf("параметр %d: ожидается целое, получено %q", i, s)
	}
	return n, nil
}

// Float — DOUBLE. Принимает число и строку с числом; null — 0.
func (a Args) Float(i int) (float64, error) {
	if i >= len(a) {
		return 0, fmt.Errorf("нет параметра %d", i)
	}
	var s string
	switch v := a[i].(type) {
	case nil:
		return 0, nil
	case json.Number:
		s = v.String()
	case string:
		s = strings.Replace(strings.TrimSpace(v), ",", ".", 1)
	default:
		return 0, fmt.Errorf("параметр %d: ожидается число, получено %v", i, a[i])
	}
	f, err := strconv.ParseFloat(s, 64)
	if err != nil {
		return 0, fmt.Errorf("параметр %d: ожидается число, получено %q", i, s)
	}
	return f, nil
}
