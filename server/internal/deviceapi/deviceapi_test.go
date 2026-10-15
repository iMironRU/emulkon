package deviceapi

import (
	"bytes"
	"context"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"sync"
	"sync/atomic"
	"testing"
	"time"

	"emulkon/internal/journal"
	"emulkon/internal/models"
	"emulkon/internal/secret"
	"emulkon/internal/sessions"
)

// testModel — модель для проверки протокола: пишет номер вызова в последний
// (выходной) параметр, умеет «падать» на методе Fail и держать команду до release.
type testModel struct {
	calls   atomic.Int32
	started chan struct{}
	release chan struct{}
}

func (m *testModel) Handle(_ context.Context, method string, params []any) models.Result {
	n := m.calls.Add(1)
	if m.started != nil {
		m.started <- struct{}{}
	}
	if m.release != nil {
		<-m.release
	}
	if method == "Fail" {
		return models.Result{Return: false, ErrorCode: 51, ErrorText: "Недостаточно средств"}
	}
	out := models.CopyParams(params)
	if len(out) > 0 {
		out[len(out)-1] = int64(n)
	}
	return models.Result{Return: true, Params: out}
}
func (m *testModel) State() map[string]any { return map[string]any{"calls": m.calls.Load()} }
func (m *testModel) Reset()                { m.calls.Store(0) }
func (m *testModel) Description() models.Description {
	return models.Description{Name: "test", IsEmulator: true}
}

type env struct {
	t     *testing.T
	srv   *httptest.Server
	j     *journal.Journal
	model *testModel
}

func newEnv(t *testing.T, deviceToken string, model *testModel) *env {
	t.Helper()
	j, err := journal.Open(journal.Options{Dir: t.TempDir()})
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { j.Close() })
	reg := models.NewRegistry()
	reg.Register("kkt", func(models.EquipmentType, string) models.Model { return model })
	sess := sessions.NewManager(sessions.Config{DeviceToken: secret.Plain(deviceToken)}, reg)
	api := New(Config{Version: "test"}, sess, j)
	mux := http.NewServeMux()
	api.Register(mux)
	srv := httptest.NewServer(mux)
	t.Cleanup(srv.Close)
	return &env{t: t, srv: srv, j: j, model: model}
}

func (e *env) do(method, path, session string, body any) (*http.Response, map[string]any) {
	e.t.Helper()
	var b []byte
	switch v := body.(type) {
	case nil:
	case string:
		b = []byte(v)
	default:
		b, _ = json.Marshal(v)
	}
	req, _ := http.NewRequest(method, e.srv.URL+path, bytes.NewReader(b))
	if session != "" {
		req.Header.Set(SessionHeader, session)
	}
	res, err := http.DefaultClient.Do(req)
	if err != nil {
		e.t.Fatal(err)
	}
	defer res.Body.Close()
	var out map[string]any
	json.NewDecoder(res.Body).Decode(&out)
	return res, out
}

func (e *env) open(token string) (sessionID, deviceID string) {
	e.t.Helper()
	res, out := e.do("POST", "/api/v1/sessions", "", map[string]any{
		"workstation_id": "KASSA-01", "device_token": token, "equipment_type": "ККТ",
		"driver_version": "0.1.0", "interface_revision": 4008,
	})
	if res.StatusCode != 200 {
		e.t.Fatalf("open session: %d %v", res.StatusCode, out)
	}
	return out["session_id"].(string), out["device_id"].(string)
}

func (e *env) cmd(sid, dev string, body any) (*http.Response, map[string]any) {
	e.t.Helper()
	return e.do("POST", "/api/v1/devices/"+dev+"/commands", sid, body)
}

func errCode(out map[string]any) string {
	if e, ok := out["error"].(map[string]any); ok {
		return e["code"].(string)
	}
	return ""
}

func TestPositionalRoundTripAndJournal(t *testing.T) {
	e := newEnv(t, "", &testModel{})
	sid, dev := e.open("")
	if dev != "kkt-01" {
		t.Fatalf("device_id = %s", dev)
	}
	res, out := e.cmd(sid, dev, map[string]any{
		"request_id": "r-1", "method": "ProcessCheck",
		"params": []any{"dev-handle", false, "<CheckPackage/>", ""},
	})
	if res.StatusCode != 200 || out["result"] != true || out["request_id"] != "r-1" {
		t.Fatalf("команда: %d %v", res.StatusCode, out)
	}
	params := out["params"].([]any)
	if len(params) != 4 || params[0] != "dev-handle" || params[1] != false || params[2] != "<CheckPackage/>" || params[3] != float64(1) {
		t.Fatalf("параметры после вызова: %v", params)
	}

	entries, _ := e.j.Tail(journal.Filter{CorrelationID: "r-1"}, 10)
	if len(entries) != 2 || entries[0].Direction != "in" || entries[1].Direction != "out" || entries[1].LatencyMS == nil {
		t.Fatalf("журнал команды: %+v", entries)
	}
	if entries[0].WorkstationID != "KASSA-01" || entries[0].DeviceID != dev || entries[0].SessionID != sid {
		t.Fatalf("атрибуты записи: %+v", entries[0])
	}
	// XML в журнале — как есть, без <: по нему работает поиск.
	if !bytes.Contains(entries[0].Payload, []byte(`"params":["dev-handle",false,"<CheckPackage/>",""]`)) {
		t.Fatalf("параметры в журнале: %s", entries[0].Payload)
	}
	if found, _ := e.j.Tail(journal.Filter{Text: "<checkpackage"}, 10); len(found) != 2 {
		t.Fatalf("поиск XML по журналу: найдено %d", len(found))
	}
}

func TestParamsValidation(t *testing.T) {
	e := newEnv(t, "", &testModel{})
	sid, dev := e.open("")

	// Без params — метод без параметров.
	if res, out := e.cmd(sid, dev, map[string]any{"request_id": "a", "method": "GetInterfaceRevision"}); res.StatusCode != 200 || len(out["params"].([]any)) != 0 {
		t.Fatalf("без params: %d %v", res.StatusCode, out)
	}
	// Именованные параметры — ошибка протокола.
	if res, _ := e.cmd(sid, dev, map[string]any{"request_id": "b", "method": "M", "params": map[string]any{"a": 1}}); res.StatusCode != 400 {
		t.Fatalf("объект вместо массива: %d", res.StatusCode)
	}
	// Вложенные структуры — не значения Native API.
	if res, _ := e.cmd(sid, dev, map[string]any{"request_id": "c", "method": "M", "params": []any{[]any{1}}}); res.StatusCode != 400 {
		t.Fatalf("вложенный массив: %d", res.StatusCode)
	}
	// Большие целые не теряют точность (числа идут как json.Number, без float64).
	e.cmd(sid, dev, `{"request_id":"d","method":"M","params":[9007199254740993, null]}`)
	for _, dir := range []string{"in", "out"} {
		entries, _ := e.j.Tail(journal.Filter{CorrelationID: "d", Direction: dir}, 1)
		if !bytes.Contains(entries[0].Payload, []byte("9007199254740993")) {
			t.Fatalf("%s: точность числа потеряна: %s", dir, entries[0].Payload)
		}
	}
}

func TestGetLastErrorAfterFailure(t *testing.T) {
	e := newEnv(t, "", &testModel{})
	sid, dev := e.open("")

	_, out := e.cmd(sid, dev, map[string]any{"request_id": "p1", "method": "Fail", "params": []any{"h", ""}})
	if out["result"] != false || out["error_code"] != float64(51) {
		t.Fatalf("ошибка устройства: %v", out)
	}
	_, out = e.cmd(sid, dev, map[string]any{"request_id": "e1", "method": "GetLastError", "params": []any{""}})
	if out["result"] != float64(51) || out["params"].([]any)[0] != "Недостаточно средств" {
		t.Fatalf("GetLastError: %v", out)
	}
	if e.model.calls.Load() != 1 {
		t.Fatalf("GetLastError не должен доходить до модели")
	}

	// Успех сбрасывает последнюю ошибку.
	e.cmd(sid, dev, map[string]any{"request_id": "ok1", "method": "M", "params": []any{""}})
	_, out = e.cmd(sid, dev, map[string]any{"request_id": "e2", "method": "GetLastError", "params": []any{""}})
	if out["result"] != float64(0) || out["params"].([]any)[0] != "" {
		t.Fatalf("после успеха GetLastError: %v", out)
	}
}

func TestIdempotentReplayAndConflict(t *testing.T) {
	e := newEnv(t, "", &testModel{})
	sid, dev := e.open("")
	cmd := map[string]any{"request_id": "r-1", "method": "Pay", "params": []any{"h", "<Op Amount=\"100\"/>", ""}}
	_, first := e.cmd(sid, dev, cmd)
	_, second := e.cmd(sid, dev, cmd)
	if e.model.calls.Load() != 1 {
		t.Fatalf("метод выполнен %d раз, want 1", e.model.calls.Load())
	}
	if first["params"].([]any)[2] != second["params"].([]any)[2] {
		t.Fatalf("повтор вернул другой ответ: %v / %v", first, second)
	}
	replays, _ := e.j.Tail(journal.Filter{CorrelationID: "r-1"}, 10)
	if len(replays) != 4 || !replays[2].Replay || !replays[3].Replay {
		t.Fatalf("повтор не помечен в журнале: %+v", replays)
	}

	cmd["params"] = []any{"h", "<Op Amount=\"200\"/>", ""}
	res, out := e.cmd(sid, dev, cmd)
	if res.StatusCode != http.StatusConflict || errCode(out) != "request_id_conflict" {
		t.Fatalf("другие параметры с тем же request_id: %d %v", res.StatusCode, out)
	}
}

// Повтор, пришедший пока первый запрос ещё выполняется (клиент не дождался
// ответа и повторил), должен дождаться и получить тот же результат без второго выполнения.
func TestReplayWhileInFlight(t *testing.T) {
	m := &testModel{started: make(chan struct{}, 1), release: make(chan struct{})}
	e := newEnv(t, "", m)
	sid, dev := e.open("")
	cmd := map[string]any{"request_id": "r-slow", "method": "Pay", "params": []any{"h", ""}}

	var wg sync.WaitGroup
	results := make([]map[string]any, 2)
	wg.Add(1)
	go func() { defer wg.Done(); _, results[0] = e.cmd(sid, dev, cmd) }()
	<-m.started
	wg.Add(1)
	go func() { defer wg.Done(); _, results[1] = e.cmd(sid, dev, cmd) }()
	time.Sleep(50 * time.Millisecond) // повтор успевает встать в ожидание
	close(m.release)
	wg.Wait()

	if m.calls.Load() != 1 {
		t.Fatalf("выполнено %d раз, want 1", m.calls.Load())
	}
	for i, r := range results {
		if r["result"] != true || r["request_id"] != "r-slow" || r["params"].([]any)[1] != float64(1) {
			t.Fatalf("ответ %d: %v", i, r)
		}
	}
}

func TestSessionErrors(t *testing.T) {
	e := newEnv(t, "secret", &testModel{})

	res, out := e.do("POST", "/api/v1/sessions", "", map[string]any{"workstation_id": "K", "device_token": "bad", "equipment_type": "KKT"})
	if res.StatusCode != 401 || errCode(out) != "bad_token" {
		t.Fatalf("неверный токен: %d %v", res.StatusCode, out)
	}
	res, out = e.do("POST", "/api/v1/sessions", "", map[string]any{"workstation_id": "K", "device_token": "secret", "equipment_type": "Toaster"})
	if res.StatusCode != 400 || errCode(out) != "unknown_equipment_type" {
		t.Fatalf("неизвестный тип: %d %v", res.StatusCode, out)
	}
	// Тип из таблицы требований, но модель не зарегистрирована.
	res, out = e.do("POST", "/api/v1/sessions", "", map[string]any{"workstation_id": "K", "device_token": "secret", "equipment_type": "POSTerminal"})
	if res.StatusCode != 400 || errCode(out) != "unknown_equipment_type" {
		t.Fatalf("незарегистрированный тип: %d %v", res.StatusCode, out)
	}
	rejected, _ := e.j.Tail(journal.Filter{Method: "session.open"}, 10)
	if len(rejected) != 3 {
		t.Fatalf("отказы не попали в журнал: %+v", rejected)
	}
	if bytes.Contains(rejected[0].Payload, []byte("bad")) {
		t.Fatalf("токен не должен попадать в журнал: %s", rejected[0].Payload)
	}

	sid, dev := e.open("secret")
	cmd := map[string]any{"request_id": "r", "method": "M"}
	if res, out = e.cmd("", dev, cmd); res.StatusCode != 401 || errCode(out) != "session_unknown" {
		t.Fatalf("без сессии: %d %v", res.StatusCode, out)
	}
	if res, out = e.cmd("s_unknown", dev, cmd); res.StatusCode != 401 || errCode(out) != "session_unknown" {
		t.Fatalf("чужая сессия: %d %v", res.StatusCode, out)
	}
	if res, out = e.cmd(sid, "scales-01", cmd); res.StatusCode != 403 || errCode(out) != "wrong_device" {
		t.Fatalf("другое устройство: %d %v", res.StatusCode, out)
	}
	if res, out = e.cmd(sid, dev, map[string]any{"method": "M"}); res.StatusCode != 400 {
		t.Fatalf("без request_id: %d %v", res.StatusCode, out)
	}
	if res, _ = e.do("GET", "/api/v1/devices/"+dev+"/description", sid, nil); res.StatusCode != 404 {
		t.Fatalf("описание драйвера отдаёт компонента, не сервер: %d", res.StatusCode)
	}
}
