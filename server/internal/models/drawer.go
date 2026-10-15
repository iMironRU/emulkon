package models

import (
	"fmt"
	"time"
)

// Drawer — денежный ящик ККТ или принтера чеков (docs/models/kkt.md, printers.md): открыт с
// OpenCashDrawer до закрытия в карточке или автозакрытия — как у виртуальных устройств кассы.
type Drawer struct {
	Open      bool
	OpenedAt  time.Time
	AutoClose time.Duration // 0 — открыт, пока не закроют в карточке
}

// NewDrawer — ящик закрыт, сам закрывается через 10 с.
func NewDrawer() Drawer { return Drawer{AutoClose: 10 * time.Second} }

// Opened — команда OpenCashDrawer.
func (d *Drawer) Opened(now time.Time) {
	d.Open, d.OpenedAt = true, now
}

// IsOpen — открыт ли ящик сейчас.
func (d Drawer) IsOpen(now time.Time) bool {
	return d.Open && (d.AutoClose == 0 || now.Before(d.OpenedAt.Add(d.AutoClose)))
}

// Set — ключи drawer_open, drawer_auto_close_s; ok=false — ключ не ящика.
func (d *Drawer) Set(key string, v any, now time.Time) (ok bool, err error) {
	switch key {
	case "drawer_open":
		b, isBool := v.(bool)
		if !isBool {
			return true, fmt.Errorf("drawer_open: ожидается true или false")
		}
		d.Open = b
		if b {
			d.OpenedAt = now
		}
	case "drawer_auto_close_s":
		n, isNum := v.(float64)
		if !isNum || n < 0 || n > 600 || n != float64(int(n)) {
			return true, fmt.Errorf("drawer_auto_close_s: целое от 0 до 600")
		}
		d.AutoClose = time.Duration(n) * time.Second
	default:
		return false, nil
	}
	return true, nil
}

// State — ключи ящика в состоянии модели.
func (d Drawer) State(now time.Time, st map[string]any) map[string]any {
	st["drawer_open"] = d.IsOpen(now)
	st["drawer_auto_close_s"] = int(d.AutoClose / time.Second)
	return st
}
