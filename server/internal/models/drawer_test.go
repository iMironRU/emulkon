package models

import (
	"testing"
	"time"
)

func TestDrawer(t *testing.T) {
	now := time.Date(2026, 10, 9, 12, 0, 0, 0, time.UTC)
	d := NewDrawer()
	if d.IsOpen(now) {
		t.Fatal("сначала закрыт")
	}
	d.Opened(now)
	if !d.IsOpen(now.Add(9*time.Second)) || d.IsOpen(now.Add(10*time.Second)) {
		t.Fatal("закрывается сам через 10 с")
	}
	if _, err := d.Set("drawer_auto_close_s", 0.0, now); err != nil {
		t.Fatal(err)
	}
	if !d.IsOpen(now.Add(time.Hour)) {
		t.Fatal("0 — открыт, пока не закроют")
	}
	if _, err := d.Set("drawer_open", false, now); err != nil || d.IsOpen(now) {
		t.Fatal("закрыть из карточки")
	}
	if ok, err := d.Set("drawer_auto_close_s", 1000.0, now); !ok || err == nil {
		t.Fatal("больше 600 — ошибка")
	}
	if ok, _ := d.Set("paper", true, now); ok {
		t.Fatal("чужой ключ")
	}
	st := d.State(now, map[string]any{})
	if st["drawer_open"] != false || st["drawer_auto_close_s"] != 0 {
		t.Fatalf("состояние: %v", st)
	}
}
