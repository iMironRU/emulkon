package scales

import (
	"context"
	"encoding/json"
	"strings"
	"testing"
	"time"

	"emulkon/internal/models"
)

func newModel(t *testing.T) *Model {
	t.Helper()
	now := time.Date(2026, 10, 8, 15, 39, 41, 0, time.Local)
	return New("scales-01", Options{Version: "test", Now: func() time.Time { return now }})
}

func set(t *testing.T, m *Model, patch map[string]any) {
	t.Helper()
	if err := m.SetState(patch); err != nil {
		t.Fatal(err)
	}
}

func call(m *Model, method string, params ...any) models.Result {
	return m.Handle(context.Background(), method, params)
}

func ok(r models.Result) bool { b, _ := r.Return.(bool); return b }

func weigh(t *testing.T, m *Model) (float64, models.Result) {
	t.Helper()
	r := call(m, "GetWeight", "emulkon-1", 0.0)
	if !ok(r) {
		return 0, r
	}
	w, isNum := r.Params[1].(float64)
	if !isNum {
		t.Fatalf("GetWeight: вес %T, ожидается float64", r.Params[1])
	}
	return w, r
}

func TestScaleOptions(t *testing.T) {
	m := newModel(t)
	r := call(m, "ScaleOptions", "emulkon-1", "")
	s, _ := r.Params[1].(string)
	for _, want := range []string{`<ScaleOptions `, `WeighingEvent="false"`, `AdvancedData="true"`, `Calibrate="true"`, `PlacementControl="false"`} {
		if !strings.Contains(s, want) {
			t.Errorf("ScaleOptions без %s: %s", want, s)
		}
	}
}

func TestManualWeightAndTare(t *testing.T) {
	m := newModel(t)
	if w, r := weigh(t, m); !ok(r) || w != 0 {
		t.Fatalf("пустые весы: %v %+v", w, r)
	}
	set(t, m, map[string]any{"weight": 0.25})
	if r := call(m, "Calibrate", "emulkon-1", json.Number("0")); !ok(r) {
		t.Fatalf("Calibrate: %+v", r)
	}
	if w, _ := weigh(t, m); w != 0 {
		t.Fatalf("после тары 0.25 на весах 0.25 → нетто %v", w)
	}
	set(t, m, map[string]any{"weight": 1.7824})
	if w, _ := weigh(t, m); w != 1.532 {
		t.Fatalf("нетто: %v, ожидается 1.532", w)
	}
	if r := call(m, "Calibrate", "emulkon-1", json.Number("0.1")); !ok(r) {
		t.Fatalf("Calibrate 0.1: %+v", r)
	}
	if w, _ := weigh(t, m); w != 1.682 {
		t.Fatalf("нетто с тарой 0.1: %v", w)
	}
	r := call(m, "Calibrate", "emulkon-1", json.Number("16"))
	if ok(r) || r.ErrorCode != ErrOverload {
		t.Fatalf("тара больше предела: %+v", r)
	}
	st := m.State()
	if st["weighings"] != 4 || st["tare"] != 0.1 {
		t.Fatalf("состояние: %+v", st)
	}
}

func TestWeightErrors(t *testing.T) {
	cases := []struct {
		patch map[string]any
		code  int
	}{
		{map[string]any{"weight": 15.001}, ErrOverload},
		{map[string]any{"weight": 1.0, "max_weight": 0.5}, ErrOverload},
		{map[string]any{"weight": 1.0, "stable": false}, ErrUnstable},
		{map[string]any{"weight": 1.0, "placement_error": true}, ErrPlacement},
	}
	for _, c := range cases {
		m := newModel(t)
		set(t, m, c.patch)
		if _, r := weigh(t, m); ok(r) || r.ErrorCode != c.code {
			t.Errorf("%v: %+v, ожидается %d", c.patch, r, c.code)
		}
	}
}

func TestGetData(t *testing.T) {
	m := newModel(t)
	set(t, m, map[string]any{"weight": 2.5, "tare": 0.5, "stable": false, "placement_error": true, "zones": "err-1,err-2"})
	r := call(m, "GetData", "emulkon-1", "")
	s, _ := r.Params[1].(string)
	for _, want := range []string{`<Data `, `Weight="2"`, `Stable="false"`, `PlacementError="true"`, `IntersectionZones="err-1,err-2"`} {
		if !strings.Contains(s, want) {
			t.Errorf("Data без %s: %s", want, s)
		}
	}
	set(t, m, map[string]any{"placement_error": false})
	r = call(m, "GetData", "emulkon-1", "")
	if s, _ := r.Params[1].(string); strings.Contains(s, "IntersectionZones") {
		t.Errorf("зоны без ошибки размещения: %s", s)
	}
}

// answer ждёт запрос на экране весов и отвечает на него.
func answer(t *testing.T, m *Model, a models.PromptAnswer) models.Prompt {
	t.Helper()
	for i := 0; i < 200; i++ {
		if p := m.Current(); p != nil {
			if err := m.Respond(p.ID, a); err != nil {
				t.Errorf("Respond %+v: %v", a, err)
			}
			return *p
		}
		time.Sleep(5 * time.Millisecond)
	}
	t.Fatal("запрос на экране весов не появился")
	return models.Prompt{}
}

func TestAskMode(t *testing.T) {
	m := newModel(t)
	set(t, m, map[string]any{"weight_mode": "ask", "tare": 0.05})
	var events []string
	m.SetPromptHook(func(p models.Prompt, done *models.PromptResult) {
		if done == nil {
			events = append(events, "open")
		} else {
			events = append(events, "done:"+done.Action+":"+done.Text)
		}
	})

	v := 0.532
	got := make(chan models.Prompt, 1)
	go func() { got <- answer(t, m, models.PromptAnswer{Action: "weigh", Value: &v}) }()
	w, r := weigh(t, m)
	p := <-got
	if !ok(r) || w != 0.532 {
		t.Fatalf("вес с экрана: %v %+v", w, r)
	}
	if p.Input == nil || p.Input.Unit != "кг" || p.Input.Max != 15 || p.Operation != "GetWeight" {
		t.Fatalf("запрос: %+v", p)
	}
	if st := m.State(); st["weight"] != 0.582 || st["prompt"] != nil {
		t.Fatalf("на платформе брутто = вес + тара, запрос снят: %+v", st)
	}
	if len(events) != 2 || events[1] != "done:weigh:ВЕС 0.532 КГ" {
		t.Fatalf("события экрана: %v", events)
	}

	// без значения и за пределом — ошибка ответа, запрос остаётся
	go func() {
		for m.Current() == nil {
			time.Sleep(5 * time.Millisecond)
		}
		id := m.Current().ID
		big := 20.0
		if err := m.Respond(id, models.PromptAnswer{Action: "weigh"}); err == nil {
			t.Error("ответ без значения принят")
		}
		if err := m.Respond(id, models.PromptAnswer{Action: "weigh", Value: &big}); err == nil {
			t.Error("20 кг при пределе 15 приняты")
		}
		if err := m.Respond(id, models.PromptAnswer{Action: "cancel"}); err != nil {
			t.Error(err)
		}
	}()
	if _, r := weigh(t, m); ok(r) || r.ErrorCode != ErrCancelled {
		t.Fatalf("отмена: %+v", r)
	}

	set(t, m, map[string]any{"prompt_timeout_s": 5.0})
	m.SetPromptHook(nil) // события уже проверены
	go func() {
		for m.Current() == nil {
			time.Sleep(5 * time.Millisecond)
		}
		m.CancelPrompt() // сброс
	}()
	if _, r := weigh(t, m); ok(r) || r.ErrorCode != ErrCancelled {
		t.Fatalf("сброс во время запроса: %+v", r)
	}
}

func TestAskTimeout(t *testing.T) {
	if testing.Short() {
		t.Skip("ждёт 5 секунд")
	}
	m := newModel(t)
	set(t, m, map[string]any{"weight_mode": "ask", "prompt_timeout_s": 5.0})
	if _, r := weigh(t, m); ok(r) || r.ErrorCode != ErrTimeout {
		t.Fatalf("таймаут: %+v", r)
	}
}

func TestSetStateValidation(t *testing.T) {
	m := newModel(t)
	for _, bad := range []map[string]any{
		{"weight": -1.0}, {"max_weight": 0.0}, {"stable": "да"}, {"weight_mode": "auto"},
		{"prompt_timeout_s": 1.0}, {"zones": 5.0}, {"color": "red"},
		{"weight": 1.0, "weight_mode": "auto"}, // отклоняется целиком
	} {
		if err := m.SetState(bad); err == nil {
			t.Errorf("%v принят", bad)
		}
	}
	if st := m.State(); st["weight"] != 0.0 || st["weight_mode"] != "manual" {
		t.Fatalf("неудачный SetState изменил состояние: %+v", st)
	}
	set(t, m, map[string]any{"weight": 3.0, "tare": 1.0, "weight_mode": "ask"})
	m.Reset()
	if st := m.State(); st["weight"] != 0.0 || st["tare"] != 0.0 || st["weight_mode"] != "manual" {
		t.Fatalf("Reset: %+v", st)
	}
}

func TestNotSupported(t *testing.T) {
	m := newModel(t)
	if r := call(m, "UploadGoods", "emulkon-1", "", "last"); ok(r) || r.ErrorCode != ErrNotSupported {
		t.Fatalf("%+v", r)
	}
}
