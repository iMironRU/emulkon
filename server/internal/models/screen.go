package models

import (
	"fmt"
	"strconv"
	"strings"
	"sync"
	"time"
)

// Screen — экран устройства, который ждёт человека: один запрос за раз (команды устройства
// и так идут по очереди). Реализует Respond, CancelPrompt и SetPromptHook для Prompter —
// модель встраивает Screen и вызывает Ask внутри команды без своих блокировок.
type Screen struct {
	DeviceID string

	mu   sync.Mutex
	cur  *pendingPrompt
	seq  int64
	hook PromptHook
}

type pendingPrompt struct {
	p  Prompt
	ch chan PromptResult // ёмкость 1: ответ не блокирует отвечающего
}

// Действия, которые Screen подставляет сам.
const (
	PromptTimeout = "timeout" // никто не ответил
	PromptReset   = "reset"   // запрос снят сбросом состояния
)

// Ask открывает запрос p и ждёт ответа не дольше timeout. ID, StartedAt и Deadline Screen
// ставит сам. text — итог для экрана (запись device.prompt_done) по ответу.
func (s *Screen) Ask(p Prompt, now time.Time, timeout time.Duration, text func(PromptResult) string) PromptResult {
	s.mu.Lock()
	s.seq++
	p.ID = fmt.Sprintf("%s-%s-%d", s.DeviceID, strconv.FormatInt(now.UnixMilli(), 36), s.seq)
	p.StartedAt, p.Deadline = now, now.Add(timeout)
	pend := &pendingPrompt{p: p, ch: make(chan PromptResult, 1)}
	if s.cur != nil { // не бывает: команды устройства идут по очереди — но не зависнем
		s.cur.ch <- PromptResult{Action: PromptReset}
	}
	s.cur = pend
	hook := s.hook
	s.mu.Unlock()
	if hook != nil {
		hook(p, nil)
	}

	timer := time.NewTimer(timeout)
	defer timer.Stop()
	var res PromptResult
	select {
	case res = <-pend.ch:
	case <-timer.C:
		res = PromptResult{Action: PromptTimeout}
	}
	s.mu.Lock()
	if s.cur == pend {
		s.cur = nil
	}
	if res.Action == PromptTimeout { // ответ мог успеть одновременно с таймаутом — он главнее
		select {
		case r := <-pend.ch:
			res = r
		default:
		}
	}
	s.mu.Unlock()
	if text != nil {
		res.Text = text(res)
	}
	if hook != nil {
		hook(p, &res)
	}
	return res
}

// Current — открытый запрос или nil (для State модели).
func (s *Screen) Current() *Prompt {
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.cur == nil {
		return nil
	}
	p := s.cur.p
	return &p
}

// Respond — ответ человека. Проверяет кнопку, карту (если на экране есть выбор карт)
// и значение поля (если есть поле; кнопка cancel значения не требует).
func (s *Screen) Respond(promptID string, a PromptAnswer) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	pend := s.cur
	if pend == nil || pend.p.ID != promptID {
		return ErrPromptGone
	}
	p := pend.p
	ok := false
	ids := make([]string, 0, len(p.Actions))
	for _, opt := range p.Actions {
		ids = append(ids, opt.ID)
		ok = ok || opt.ID == a.Action
	}
	if !ok {
		return fmt.Errorf("действие %q: ожидается одно из %s", a.Action, strings.Join(ids, ", "))
	}
	res := PromptResult{Action: a.Action}
	if a.Action == "card" && len(p.Cards) > 0 {
		card := a.Card
		if card == "" {
			card = p.DefaultCard
		}
		found := false
		keys := make([]string, 0, len(p.Cards))
		for _, c := range p.Cards {
			keys = append(keys, c.ID)
			found = found || c.ID == card
		}
		if !found {
			return fmt.Errorf("карта %q: ожидается одна из %s", card, strings.Join(keys, ", "))
		}
		res.Card = card
	}
	if p.Input != nil && a.Action != "cancel" {
		if a.Value == nil {
			return fmt.Errorf("нужно значение: %s", p.Input.Label)
		}
		v := *a.Value
		if v < p.Input.Min || (p.Input.Max > 0 && v > p.Input.Max) {
			return fmt.Errorf("%s: от %g до %g %s", p.Input.Label, p.Input.Min, p.Input.Max, p.Input.Unit)
		}
		res.Value = &v
	}
	s.cur = nil
	pend.ch <- res
	return nil
}

// CancelPrompt снимает открытый запрос: Ask вернёт PromptReset.
func (s *Screen) CancelPrompt() {
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.cur != nil {
		s.cur.ch <- PromptResult{Action: PromptReset}
		s.cur = nil
	}
}

func (s *Screen) SetPromptHook(h PromptHook) {
	s.mu.Lock()
	s.hook = h
	s.mu.Unlock()
}
