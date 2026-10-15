package scenarios

import (
	"crypto/rand"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"math/big"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"time"

	"emulkon/internal/models"
)

// Действия правила (ARCHITECTURE §7).
const (
	ActError     = "error"      // ответить ошибкой устройства, модель не вызывается
	ActDelay     = "delay"      // подождать, затем выполнить как обычно
	ActDrop      = "drop"       // выполнить и оборвать соединение — ответ теряется, повтор получает сохранённый
	ActLate      = "late"       // выполнить и ответить с задержкой (после таймаута компоненты)
	ActSetState  = "set_state"  // изменить состояние модели, затем выполнить
	ActEmitEvent = "emit_event" // отправить событие устройства в 1С, затем выполнить
)

var ErrBadRule = errors.New("неверное правило")
var ErrNotFound = errors.New("правило не найдено")

// Rule — правило сценария.
type Rule struct {
	ID      string `json:"id"`
	Name    string `json:"name"`
	Enabled bool   `json:"enabled"`
	Match   Match  `json:"match"`
	Action  Action `json:"action"`
	// Limit — сработать не больше раз (0 — без ограничения). Счётчики сбрасывает «Сброс».
	Limit int `json:"limit,omitempty"`
	// Fired, Seen — сколько раз сработало и сколько подходящих вызовов было (только чтение).
	Fired int `json:"fired"`
	Seen  int `json:"seen"`
}

// Match — условия. Пустое поле — любое значение.
type Match struct {
	Device      string      `json:"device,omitempty"`      // kkt-01
	Type        string      `json:"type,omitempty"`        // класс: kkt, acquiring…
	Workstation string      `json:"workstation,omitempty"` // рабочее место сессии
	Method      string      `json:"method,omitempty"`      // метод драйвера; несколько — через запятую
	Param       *ParamMatch `json:"param,omitempty"`
	NthCall     int         `json:"nth_call,omitempty"`    // только на N-м подходящем вызове
	Every       int         `json:"every,omitempty"`       // на каждом N-м
	Probability float64     `json:"probability,omitempty"` // 0 < p ≤ 1; 0 — всегда
}

// ParamMatch — условие на параметр вызова по номеру (позиционные параметры, protocol.md §1.2).
type ParamMatch struct {
	Index    int    `json:"index"`
	Equals   string `json:"equals,omitempty"`
	Contains string `json:"contains,omitempty"`
}

// Action — что сделать.
type Action struct {
	Type      string              `json:"type"`
	ErrorCode int                 `json:"error_code,omitempty"`
	ErrorText string              `json:"error_text,omitempty"`
	DelayMS   int                 `json:"delay_ms,omitempty"`
	State     map[string]any      `json:"state,omitempty"`
	Event     *models.DeviceEvent `json:"event,omitempty"`
}

// Call — вызов, к которому применяются правила.
type Call struct {
	Device, Type, Workstation, Method string
	Params                            []any
}

// Plan — что сделать с вызовом по сработавшим правилам. Error, Drop, Late — от первого
// сработавшего правила с таким действием; задержки складываются; состояния и события — все.
type Plan struct {
	Applied []string // «имя (id)» сработавших правил — в журнал
	Delay   time.Duration
	Late    time.Duration
	Drop    bool
	Error   *Action
	States  []map[string]any
	Events  []models.DeviceEvent
}

// Engine — правила и их счётчики.
type Engine struct {
	path  string
	mu    sync.Mutex
	rules []*Rule
	rnd   func() float64
}

// New — движок с файлом правил path (пусто — без сохранения). Файла нет — правил нет.
func New(path string) (*Engine, error) {
	e := &Engine{path: path, rnd: random}
	if path == "" {
		return e, nil
	}
	b, err := os.ReadFile(path)
	if errors.Is(err, os.ErrNotExist) {
		return e, nil
	}
	if err != nil {
		return nil, fmt.Errorf("прочитать сценарии: %w", err)
	}
	var f struct {
		Rules []*Rule `json:"rules"`
	}
	if err := json.Unmarshal(b, &f); err != nil {
		return nil, fmt.Errorf("разобрать сценарии %s: %w", path, err)
	}
	for _, r := range f.Rules {
		if err := r.validate(); err != nil {
			return nil, fmt.Errorf("сценарии %s: правило %q: %w", path, r.Name, err)
		}
		r.Fired, r.Seen = 0, 0
	}
	e.rules = f.Rules
	return e, nil
}

func random() float64 {
	n, err := rand.Int(rand.Reader, big.NewInt(1<<53))
	if err != nil {
		return 0
	}
	return float64(n.Int64()) / float64(1<<53)
}

func (r *Rule) validate() error {
	r.Name = strings.TrimSpace(r.Name)
	if r.Name == "" || len(r.Name) > 200 {
		return fmt.Errorf("%w: name — от 1 до 200 символов", ErrBadRule)
	}
	m := r.Match
	if m.NthCall < 0 || m.Every < 0 || r.Limit < 0 {
		return fmt.Errorf("%w: nth_call, every, limit — не меньше 0", ErrBadRule)
	}
	if m.NthCall > 0 && m.Every > 0 {
		return fmt.Errorf("%w: nth_call и every вместе не задаются", ErrBadRule)
	}
	if m.Probability < 0 || m.Probability > 1 {
		return fmt.Errorf("%w: probability — от 0 до 1", ErrBadRule)
	}
	if m.Param != nil && (m.Param.Index < 0 || m.Param.Index > 64) {
		return fmt.Errorf("%w: param.index — от 0 до 64", ErrBadRule)
	}
	a := r.Action
	if a.DelayMS < 0 || a.DelayMS > 600000 {
		return fmt.Errorf("%w: delay_ms — от 0 до 600000", ErrBadRule)
	}
	switch a.Type {
	case ActError:
		if a.ErrorCode <= 0 || a.ErrorCode > 9999 {
			return fmt.Errorf("%w: error_code — от 1 до 9999", ErrBadRule)
		}
		if strings.TrimSpace(a.ErrorText) == "" {
			return fmt.Errorf("%w: error_text обязателен", ErrBadRule)
		}
	case ActDelay, ActLate:
		if a.DelayMS == 0 {
			return fmt.Errorf("%w: delay_ms обязателен", ErrBadRule)
		}
	case ActDrop:
	case ActSetState:
		if len(a.State) == 0 {
			return fmt.Errorf("%w: state обязателен", ErrBadRule)
		}
	case ActEmitEvent:
		if a.Event == nil || !models.EventNames[a.Event.Name] {
			return fmt.Errorf("%w: event.name — из требований (Barcode, TracksData, DriverError…)", ErrBadRule)
		}
	default:
		return fmt.Errorf("%w: action.type — error, delay, drop, late, set_state или emit_event", ErrBadRule)
	}
	return nil
}

// matches — базовые условия (без номера вызова и вероятности).
func (m Match) matches(c Call) bool {
	if m.Device != "" && m.Device != c.Device || m.Type != "" && m.Type != c.Type ||
		m.Workstation != "" && m.Workstation != c.Workstation {
		return false
	}
	if m.Method != "" {
		ok := false
		for _, name := range strings.Split(m.Method, ",") {
			ok = ok || strings.EqualFold(strings.TrimSpace(name), c.Method)
		}
		if !ok {
			return false
		}
	}
	if p := m.Param; p != nil {
		if p.Index >= len(c.Params) {
			return false
		}
		v := fmt.Sprint(c.Params[p.Index])
		if c.Params[p.Index] == nil {
			v = ""
		}
		if p.Equals != "" && v != p.Equals || p.Contains != "" && !strings.Contains(v, p.Contains) {
			return false
		}
	}
	return true
}

// Plan — какие правила срабатывают на вызов; счётчики увеличиваются.
func (e *Engine) Plan(c Call) Plan {
	e.mu.Lock()
	defer e.mu.Unlock()
	var p Plan
	for _, r := range e.rules {
		if !r.Enabled || !r.Match.matches(c) {
			continue
		}
		r.Seen++
		if r.Match.NthCall > 0 && r.Seen != r.Match.NthCall ||
			r.Match.Every > 0 && r.Seen%r.Match.Every != 0 ||
			r.Limit > 0 && r.Fired >= r.Limit ||
			r.Match.Probability > 0 && e.rnd() >= r.Match.Probability {
			continue
		}
		a := r.Action
		switch a.Type {
		case ActError:
			if p.Error != nil || p.Drop {
				continue // ответ уже определён правилом выше
			}
			act := a
			p.Error = &act
		case ActDrop:
			if p.Error != nil || p.Drop {
				continue
			}
			p.Drop = true
		case ActLate:
			p.Late += time.Duration(a.DelayMS) * time.Millisecond
		case ActSetState:
			p.States = append(p.States, a.State)
		case ActEmitEvent:
			p.Events = append(p.Events, *a.Event)
		}
		if a.Type != ActLate {
			p.Delay += time.Duration(a.DelayMS) * time.Millisecond // и у error: «ошибка через 3 с»
		}
		r.Fired++
		p.Applied = append(p.Applied, r.Name+" ("+r.ID+")")
	}
	return p
}

// Rules — копии правил по порядку.
func (e *Engine) Rules() []Rule {
	e.mu.Lock()
	defer e.mu.Unlock()
	out := make([]Rule, len(e.rules))
	for i, r := range e.rules {
		out[i] = *r
	}
	return out
}

// Put добавляет правило (create) или заменяет правило с этим id. Новое правило может прийти
// со своим id (автотест потом удалит его по нему); занятый id — ошибка. Счётчики заменённого
// правила обнуляются.
func (e *Engine) Put(r Rule, create bool) (Rule, error) {
	if err := r.validate(); err != nil {
		return Rule{}, err
	}
	if create && r.ID != "" && !validID(r.ID) {
		return Rule{}, fmt.Errorf("%w: id — латиница, цифры, «-» и «_», до 64 символов", ErrBadRule)
	}
	r.Fired, r.Seen = 0, 0
	e.mu.Lock()
	if create {
		if r.ID == "" {
			r.ID = newID()
		}
		for _, old := range e.rules {
			if old.ID == r.ID {
				e.mu.Unlock()
				return Rule{}, fmt.Errorf("%w: правило %s уже есть", ErrBadRule, r.ID)
			}
		}
		e.rules = append(e.rules, &r)
	} else {
		found := false
		for i, old := range e.rules {
			if old.ID == r.ID {
				e.rules[i], found = &r, true
			}
		}
		if !found {
			e.mu.Unlock()
			return Rule{}, ErrNotFound
		}
	}
	e.mu.Unlock()
	return r, e.save()
}

// Delete удаляет правило.
func (e *Engine) Delete(id string) error {
	e.mu.Lock()
	n := len(e.rules)
	for i, r := range e.rules {
		if r.ID == id {
			e.rules = append(e.rules[:i], e.rules[i+1:]...)
			break
		}
	}
	gone := len(e.rules) < n
	e.mu.Unlock()
	if !gone {
		return ErrNotFound
	}
	return e.save()
}

// ResetCounters обнуляет счётчики всех правил (перед тестом, вместе с POST /control/v1/reset).
func (e *Engine) ResetCounters() {
	e.mu.Lock()
	defer e.mu.Unlock()
	for _, r := range e.rules {
		r.Fired, r.Seen = 0, 0
	}
}

func (e *Engine) save() error {
	if e.path == "" {
		return nil
	}
	e.mu.Lock()
	rules := make([]Rule, len(e.rules))
	for i, r := range e.rules {
		rules[i] = *r
		rules[i].Fired, rules[i].Seen = 0, 0 // счётчики — не конфигурация
	}
	e.mu.Unlock()
	b, err := json.MarshalIndent(map[string]any{"rules": rules}, "", "  ")
	if err != nil {
		return err
	}
	if err := os.MkdirAll(filepath.Dir(e.path), 0o755); err != nil {
		return fmt.Errorf("записать сценарии: %w", err)
	}
	tmp := e.path + ".tmp"
	if err := os.WriteFile(tmp, b, 0o644); err != nil {
		return fmt.Errorf("записать сценарии: %w", err)
	}
	return os.Rename(tmp, e.path)
}

func validID(id string) bool {
	if len(id) > 64 {
		return false
	}
	for _, c := range id {
		if !(c >= 'a' && c <= 'z' || c >= 'A' && c <= 'Z' || c >= '0' && c <= '9' || c == '-' || c == '_') {
			return false
		}
	}
	return true
}

func newID() string {
	b := make([]byte, 4)
	_, _ = rand.Read(b)
	return "r" + hex.EncodeToString(b)
}
