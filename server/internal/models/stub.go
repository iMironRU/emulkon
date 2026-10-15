package models

import (
	"context"
	"sync"
	"time"
)

// Stub — модель-заглушка этапа 1: на любой метод возвращает True, параметры
// не меняет и считает вызовы. Ничего не знает о контракте БПО.
type Stub struct {
	t                 EquipmentType
	deviceID, version string

	mu       sync.Mutex
	calls    map[string]int
	total    int
	lastCall time.Time
}

func NewStub(t EquipmentType, deviceID, version string) *Stub {
	return &Stub{t: t, deviceID: deviceID, version: version, calls: map[string]int{}}
}

func (s *Stub) Handle(_ context.Context, method string, _ []any) Result {
	s.mu.Lock()
	defer s.mu.Unlock()
	s.calls[method]++
	s.total++
	s.lastCall = time.Now().UTC()
	return Result{Return: true}
}

func (s *Stub) State() map[string]any {
	s.mu.Lock()
	defer s.mu.Unlock()
	calls := make(map[string]int, len(s.calls))
	for k, v := range s.calls {
		calls[k] = v
	}
	st := map[string]any{"model": "stub", "calls_total": s.total, "calls": calls}
	if !s.lastCall.IsZero() {
		st["last_call"] = s.lastCall
	}
	return st
}

func (s *Stub) Reset() {
	s.mu.Lock()
	defer s.mu.Unlock()
	s.calls = map[string]int{}
	s.total = 0
	s.lastCall = time.Time{}
}

func (s *Stub) Description() Description {
	return Description{
		Name:        "ЭМУЛЯТОР: " + s.t.Title + " (Эмулкон)",
		Description: "Виртуальное устройство Эмулкон. НЕ ИСПОЛЬЗОВАТЬ В РАБОЧЕЙ БАЗЕ: операции не выполняются на реальном оборудовании.",
		Version:     s.version,
		IsEmulator:  true,
	}
}
