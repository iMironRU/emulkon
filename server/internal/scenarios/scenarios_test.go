package scenarios

import (
	"errors"
	"path/filepath"
	"testing"
	"time"

	"emulkon/internal/models"
)

func errRule(name string, m Match) Rule {
	return Rule{Name: name, Enabled: true, Match: m, Action: Action{Type: ActError, ErrorCode: 3, ErrorText: "Нет бумаги"}}
}

func TestPlanMatching(t *testing.T) {
	e, _ := New("")
	third, _ := e.add(errRule("третий чек", Match{Device: "kkt-01", Method: "ProcessCheck", NthCall: 3}))
	e.add(Rule{Name: "медленно", Enabled: true, Match: Match{Type: "kkt"}, Action: Action{Type: ActDelay, DelayMS: 50}})
	e.add(Rule{Name: "выключено", Match: Match{}, Action: Action{Type: ActDrop}})
	call := Call{Device: "kkt-01", Type: "kkt", Method: "processcheck"}

	for i := 1; i <= 4; i++ {
		p := e.Plan(call)
		if p.Delay != 50*time.Millisecond || p.Drop {
			t.Fatalf("вызов %d: задержка всем, выключенное не срабатывает: %+v", i, p)
		}
		if (i == 3) != (p.Error != nil) {
			t.Fatalf("вызов %d: ошибка только на третьем: %+v", i, p)
		}
	}
	if p := e.Plan(Call{Device: "kkt-02", Type: "kkt", Method: "ProcessCheck"}); p.Error != nil {
		t.Fatal("другое устройство")
	}
	rules := e.Rules()
	if rules[0].ID != third.ID || rules[0].Seen != 4 || rules[0].Fired != 1 {
		t.Fatalf("счётчики: %+v", rules[0])
	}
	e.ResetCounters()
	if e.Rules()[0].Seen != 0 {
		t.Fatal("сброс счётчиков")
	}
}

func TestEveryLimitParamProbability(t *testing.T) {
	e, _ := New("")
	e.add(Rule{Name: "каждая вторая оплата", Enabled: true, Limit: 2,
		Match:  Match{Method: "Pay, PayWithCashWithdrawal", Every: 2},
		Action: Action{Type: ActError, ErrorCode: 61, ErrorText: "Отказ"}})
	fired := 0
	for i := 0; i < 10; i++ {
		if e.Plan(Call{Method: "Pay"}).Error != nil {
			fired++
		}
	}
	if fired != 2 {
		t.Fatalf("каждый второй, не больше двух раз: %d", fired)
	}

	e2, _ := New("")
	e2.add(Rule{Name: "сумма", Enabled: true, Match: Match{Param: &ParamMatch{Index: 1, Contains: `Amount="999`}},
		Action: Action{Type: ActDrop}})
	if !e2.Plan(Call{Params: []any{"emulkon-1", `<OperationParameters Amount="999.00"/>`}}).Drop ||
		e2.Plan(Call{Params: []any{"emulkon-1", `<OperationParameters Amount="10"/>`}}).Drop ||
		e2.Plan(Call{Params: []any{"emulkon-1"}}).Drop {
		t.Fatal("условие на параметр")
	}

	e3, _ := New("")
	e3.rnd = func() float64 { return 0.7 }
	e3.add(Rule{Name: "редко", Enabled: true, Match: Match{Probability: 0.5}, Action: Action{Type: ActDrop}})
	if e3.Plan(Call{}).Drop {
		t.Fatal("0.7 ≥ 0.5 — не срабатывает")
	}
}

func TestFirstTerminalActionWins(t *testing.T) {
	e, _ := New("")
	e.add(Rule{Name: "обрыв", Enabled: true, Action: Action{Type: ActDrop}})
	e.add(errRule("ошибка", Match{}))
	e.add(Rule{Name: "состояние", Enabled: true, Action: Action{Type: ActSetState, State: map[string]any{"paper": false}}})
	e.add(Rule{Name: "событие", Enabled: true, Action: Action{Type: ActEmitEvent, Event: &models.DeviceEvent{Name: "DriverError", Data: "x"}}})
	p := e.Plan(Call{})
	if !p.Drop || p.Error != nil || len(p.States) != 1 || len(p.Events) != 1 || len(p.Applied) != 3 {
		t.Fatalf("первое из error/drop выигрывает, остальное копится: %+v", p)
	}
}

func TestValidationAndPersistence(t *testing.T) {
	path := filepath.Join(t.TempDir(), "scenarios.json")
	e, _ := New(path)
	bad := []Rule{
		{Name: "", Action: Action{Type: ActDrop}},
		{Name: "x", Action: Action{Type: "explode"}},
		{Name: "x", Action: Action{Type: ActError, ErrorCode: 3}},
		{Name: "x", Action: Action{Type: ActDelay}},
		{Name: "x", Match: Match{NthCall: 2, Every: 2}, Action: Action{Type: ActDrop}},
		{Name: "x", Match: Match{Probability: 2}, Action: Action{Type: ActDrop}},
		{Name: "x", Action: Action{Type: ActEmitEvent, Event: &models.DeviceEvent{Name: "Придумано"}}},
	}
	for _, r := range bad {
		if _, err := e.add(r); !errors.Is(err, ErrBadRule) {
			t.Errorf("%+v: ждали ErrBadRule, получили %v", r, err)
		}
	}
	r, err := e.add(errRule("нет бумаги", Match{Type: "kkt"}))
	if err != nil {
		t.Fatal(err)
	}
	r.Enabled = false
	if _, err := e.Put(r, false); err != nil {
		t.Fatal(err)
	}
	if _, err := e.Put(Rule{ID: "nope", Name: "x", Action: Action{Type: ActDrop}}, false); !errors.Is(err, ErrNotFound) {
		t.Fatalf("замена несуществующего: %v", err)
	}
	e.add(Rule{Name: "второе", Enabled: true, Action: Action{Type: ActDrop}})

	again, err := New(path)
	if err != nil {
		t.Fatal(err)
	}
	rules := again.Rules()
	if len(rules) != 2 || rules[0].ID != r.ID || rules[0].Enabled || rules[1].Name != "второе" {
		t.Fatalf("после перезапуска: %+v", rules)
	}
	if err := again.Delete(r.ID); err != nil || len(again.Rules()) != 1 {
		t.Fatalf("удаление: %v", err)
	}
	if err := again.Delete(r.ID); !errors.Is(err, ErrNotFound) {
		t.Fatalf("повторное удаление: %v", err)
	}
}

// add — новое правило (POST /control/v1/scenarios).
func (e *Engine) add(r Rule) (Rule, error) { return e.Put(r, true) }

func TestCreateWithOwnID(t *testing.T) {
	e, _ := New("")
	if r, err := e.add(Rule{ID: "emulkon-1c-test", Name: "x", Action: Action{Type: ActDrop}}); err != nil || r.ID != "emulkon-1c-test" {
		t.Fatalf("свой id: %v %v", r, err)
	}
	if _, err := e.add(Rule{ID: "emulkon-1c-test", Name: "y", Action: Action{Type: ActDrop}}); !errors.Is(err, ErrBadRule) {
		t.Fatalf("занятый id: %v", err)
	}
	if _, err := e.add(Rule{ID: "плохой id", Name: "y", Action: Action{Type: ActDrop}}); !errors.Is(err, ErrBadRule) {
		t.Fatalf("недопустимый id: %v", err)
	}
}
