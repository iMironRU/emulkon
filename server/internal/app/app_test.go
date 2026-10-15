package app

import (
	"bufio"
	"bytes"
	"context"
	"encoding/base64"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"emulkon/internal/secret"
)

func newServer(t *testing.T, controlToken string) *httptest.Server {
	t.Helper()
	a, err := New(Config{Version: "test", DataDir: t.TempDir(), ControlToken: secret.Plain(controlToken)})
	if err != nil {
		t.Fatal(err)
	}
	srv := httptest.NewServer(a.Handler)
	t.Cleanup(func() { srv.Close(); a.Close() })
	return srv
}

func call(t *testing.T, srv *httptest.Server, method, path string, hdr map[string]string, body any) (*http.Response, map[string]any) {
	t.Helper()
	var b []byte
	if body != nil {
		b, _ = json.Marshal(body)
	}
	req, _ := http.NewRequest(method, srv.URL+path, bytes.NewReader(b))
	for k, v := range hdr {
		req.Header.Set(k, v)
	}
	res, err := http.DefaultClient.Do(req)
	if err != nil {
		t.Fatal(err)
	}
	defer res.Body.Close()
	var out map[string]any
	json.NewDecoder(res.Body).Decode(&out)
	return res, out
}

// Сквозной сценарий этапа 1: компонента открывает сессию и выполняет метод,
// оператор видит устройство и обмен через Control API.
func TestEndToEndSkeleton(t *testing.T) {
	srv := newServer(t, "ctl")
	auth := map[string]string{"Authorization": "Bearer ctl"}

	if res, out := call(t, srv, "GET", "/api/v1/health", nil, nil); res.StatusCode != 200 || out["status"] != "ok" {
		t.Fatalf("health: %d %v", res.StatusCode, out)
	}
	if res, _ := call(t, srv, "GET", "/control/v1/devices", nil, nil); res.StatusCode != 401 {
		t.Fatalf("Control API без токена: %d", res.StatusCode)
	}

	_, sess := call(t, srv, "POST", "/api/v1/sessions", nil, map[string]any{"workstation_id": "KASSA-01", "equipment_type": "KKT", "interface_revision": 4008})
	sid, dev := sess["session_id"].(string), sess["device_id"].(string)
	if dev != "kkt-01" || sess["emulator"] != true {
		t.Fatalf("сессия: %v", sess)
	}
	_, res := call(t, srv, "POST", "/api/v1/devices/"+dev+"/commands", map[string]string{"X-Emulkon-Session": sid},
		map[string]any{"request_id": "req-1", "method": "EquipmentTest", "params": []any{"KKT", "<Parameters/>", "", ""}})
	if res["result"] != true {
		t.Fatalf("команда: %v", res)
	}

	_, devs := call(t, srv, "GET", "/control/v1/devices", auth, nil)
	list := devs["devices"].([]any)
	if len(list) != 1 || list[0].(map[string]any)["commands"].(float64) != 1 {
		t.Fatalf("устройства: %v", devs)
	}

	_, j := call(t, srv, "GET", "/control/v1/journal?correlation_id=req-1", auth, nil)
	if n := len(j["entries"].([]any)); n != 2 {
		t.Fatalf("журнал по корреляции: %d записей", n)
	}
	_, j = call(t, srv, "GET", "/control/v1/journal?limit=1", auth, nil)
	if j["has_more"] != true || len(j["entries"].([]any)) != 1 {
		t.Fatalf("limit/has_more: %v", j)
	}

	if res, out := call(t, srv, "POST", "/control/v1/reset", auth, nil); res.StatusCode != 200 || out["reset_devices"].(float64) != 1 {
		t.Fatalf("сброс: %d %v", res.StatusCode, out)
	}

	// UI отдаётся без токена.
	resp, err := http.Get(srv.URL + "/")
	if err != nil || resp.StatusCode != 200 || !strings.Contains(resp.Header.Get("Content-Type"), "text/html") {
		t.Fatalf("UI: %v %v", resp, err)
	}
	resp.Body.Close()
}

// Сквозной сценарий этапа 2: модель ККТ через Device API и Control API.
func TestKKTEndToEnd(t *testing.T) {
	srv := newServer(t, "")
	_, sess := call(t, srv, "POST", "/api/v1/sessions", nil, map[string]any{"workstation_id": "KASSA-01", "equipment_type": "ККТ", "interface_revision": 4008})
	sid, dev := sess["session_id"].(string), sess["device_id"].(string)
	hdr := map[string]string{"X-Emulkon-Session": sid}
	n := 0
	cmd := func(method string, params ...any) map[string]any {
		t.Helper()
		n++
		_, out := call(t, srv, "POST", "/api/v1/devices/"+dev+"/commands", hdr,
			map[string]any{"request_id": fmt.Sprintf("r-%d", n), "method": method, "params": params})
		return out
	}
	const in = `<InputParameters><Parameters CashierName="Иванов"/></InputParameters>`
	check := `<CheckPackage><Parameters CashierName="Иванов" OperationType="1" TaxationSystem="0"/><Positions>` +
		`<FiscalString Name="Хлеб" Quantity="1" PriceWithDiscount="50" AmountWithDiscount="50" VATRate="10" VATAmount="4.55"/>` +
		`</Positions><Payments Cash="100" ElectronicPayment="0" PrePayment="0" PostPayment="0" Barter="0"/></CheckPackage>`

	// Чек без открытой смены — ошибка устройства, текст через GetLastError.
	if out := cmd("ProcessCheck", "h", false, check, ""); out["result"] != false {
		t.Fatalf("чек без смены: %v", out)
	}
	if out := cmd("GetLastError", ""); out["result"] != float64(20) || !strings.HasPrefix(out["params"].([]any)[0].(string), "Кассовая смена не открыта") {
		t.Fatalf("GetLastError: %v", out)
	}

	if out := cmd("OpenShift", "h", in, ""); out["result"] != true || !strings.Contains(out["params"].([]any)[2].(string), `ShiftState="2"`) {
		t.Fatalf("открытие смены: %v", out)
	}
	out := cmd("ProcessCheck", "h", false, check, "")
	if out["result"] != true || !strings.Contains(out["params"].([]any)[3].(string), `<DocumentOutputParameters><Parameters ShiftNumber="1" CheckNumber="3"`) {
		t.Fatalf("чек: %v", out)
	}

	// Оператор «забирает бумагу» — следующий чек отклоняется с узнаваемым БПО текстом.
	res, info := call(t, srv, "PUT", "/control/v1/devices/"+dev+"/state", nil, map[string]any{"paper": false})
	if res.StatusCode != 200 || info["state"].(map[string]any)["faults"].(map[string]any)["paper"] != false {
		t.Fatalf("задание состояния: %d %v", res.StatusCode, info)
	}
	if out := cmd("ProcessCheck", "h", false, check, ""); out["error_text"] != "Нет бумаги" {
		t.Fatalf("без бумаги: %v", out)
	}
	if res, out := call(t, srv, "PUT", "/control/v1/devices/"+dev+"/state", nil, map[string]any{"bogus": 1}); res.StatusCode != 400 || !strings.Contains(fmt.Sprint(out), "допустимы") {
		t.Fatalf("неизвестный ключ: %d %v", res.StatusCode, out)
	}
	if res, _ := call(t, srv, "PUT", "/control/v1/devices/nope-01/state", nil, map[string]any{"paper": true}); res.StatusCode != 404 {
		t.Fatalf("нет устройства: %d", res.StatusCode)
	}

	_, prints := call(t, srv, "GET", "/control/v1/devices/"+dev+"/prints?limit=5", nil, nil)
	list := prints["prints"].([]any)
	if len(list) != 2 || list[0].(map[string]any)["kind"] != "check" || list[1].(map[string]any)["kind"] != "open_shift" {
		t.Fatalf("лента печати: %v", prints)
	}

	_, j := call(t, srv, "GET", "/control/v1/journal?method=control.set_state", nil, nil)
	if len(j["entries"].([]any)) != 1 {
		t.Fatalf("изменение состояния должно попасть в журнал: %v", j)
	}

	// Каждый «напечатанный» документ — запись device.print с полным текстом (для трея).
	_, j = call(t, srv, "GET", "/control/v1/journal?method=device.print&workstation_id=KASSA-01", nil, nil)
	printedEntries := j["entries"].([]any)
	if len(printedEntries) != 2 {
		t.Fatalf("device.print: открытие смены и чек, получено %d", len(printedEntries))
	}
	last := printedEntries[1].(map[string]any)
	doc := last["payload"].(map[string]any)
	if last["direction"] != "system" || last["device_id"] != dev || doc["kind"] != "check" ||
		!strings.Contains(fmt.Sprint(doc["lines"]), "КАССОВЫЙ ЧЕК") || last["correlation_id"] == "" {
		t.Fatalf("запись печати чека: %v", last)
	}
	// Показать ли документ и напечатать ли его, решает сервер по настройке устройства (protocol.md §2.3).
	if out, _ := doc["output"].(map[string]any); out["screen"] != true || out["print"] != false {
		t.Fatalf("решение по умолчанию: показать, не печатать: %v", doc["output"])
	}
	res, _ = call(t, srv, "PATCH", "/control/v1/devices/"+dev, nil, map[string]any{
		"output": map[string]any{"screen": false, "printer": true, "printer_name": "PDF", "kinds": []string{"labels"}}})
	if res.StatusCode != http.StatusBadRequest {
		t.Fatalf("этикеток у ККТ нет — 400, получено %d", res.StatusCode)
	}
	res, _ = call(t, srv, "PATCH", "/control/v1/devices/"+dev, nil, map[string]any{
		"output": map[string]any{"screen": false, "printer": true, "printer_name": "PDF", "kinds": []string{"checks"}}})
	if res.StatusCode != http.StatusOK {
		t.Fatalf("вывод устройства: %d", res.StatusCode)
	}
	res, rp := call(t, srv, "POST", "/control/v1/devices/"+dev+"/reprint", nil, nil)
	if res.StatusCode != http.StatusOK || rp["kind"] != "check" {
		t.Fatalf("повторная печать: %d %v", res.StatusCode, rp)
	}
	_, j = call(t, srv, "GET", "/control/v1/journal?method=device.reprint&workstation_id=KASSA-01", nil, nil)
	again := j["entries"].([]any)
	if len(again) != 1 {
		t.Fatalf("device.reprint: %v", j)
	}
	if out := again[0].(map[string]any)["payload"].(map[string]any)["output"].(map[string]any); out["print"] != true || out["screen"] != false || out["printer"] != "PDF" {
		t.Fatalf("повторная печать — на принтер устройства: %v", out)
	}

	// Сброс возвращает ККТ в исходное состояние.
	call(t, srv, "POST", "/control/v1/reset", nil, nil)
	_, prints = call(t, srv, "GET", "/control/v1/devices/"+dev+"/prints", nil, nil)
	if len(prints["prints"].([]any)) != 0 {
		t.Fatalf("после сброса лента пуста: %v", prints)
	}
}

func TestJournalStream(t *testing.T) {
	srv := newServer(t, "")
	_, sess := call(t, srv, "POST", "/api/v1/sessions", nil, map[string]any{"workstation_id": "K", "equipment_type": "WeighingScales"})
	sid, dev := sess["session_id"].(string), sess["device_id"].(string)

	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	req, _ := http.NewRequestWithContext(ctx, "GET", srv.URL+"/control/v1/journal/stream?tail=10&direction=out", nil)
	resp, err := http.DefaultClient.Do(req)
	if err != nil {
		t.Fatal(err)
	}
	defer resp.Body.Close()
	if ct := resp.Header.Get("Content-Type"); !strings.HasPrefix(ct, "text/event-stream") {
		t.Fatalf("Content-Type: %s", ct)
	}
	events := make(chan string, 16)
	go func() {
		sc := bufio.NewScanner(resp.Body)
		var ev, data string
		for sc.Scan() {
			line := sc.Text()
			switch {
			case strings.HasPrefix(line, "event: "):
				ev = line[7:]
			case strings.HasPrefix(line, "data: "):
				data = line[6:]
			case line == "":
				if ev != "" {
					events <- ev + " " + data
				}
				ev, data = "", ""
			}
		}
		close(events)
	}()

	next := func() string {
		select {
		case s := <-events:
			return s
		case <-ctx.Done():
			t.Fatal("нет события от потока")
			return ""
		}
	}
	if s := next(); !strings.HasPrefix(s, "ready ") {
		t.Fatalf("первым должно прийти ready (в журнале нет out-записей): %s", s)
	}

	call(t, srv, "POST", "/api/v1/devices/"+dev+"/commands", map[string]string{"X-Emulkon-Session": sid},
		map[string]any{"request_id": "live-1", "method": "GetWeight", "params": []any{"h", nil}})
	s := next()
	if !strings.HasPrefix(s, "entry ") || !strings.Contains(s, `"correlation_id":"live-1"`) || !strings.Contains(s, `"direction":"out"`) {
		t.Fatalf("живая запись (только out по фильтру): %s", s)
	}
}

// Регрессия: подходящая запись, за которой сразу идёт неподходящая (device.print,
// затем out), должна дойти сразу, а не ждать ping через 15 с.
func TestJournalStreamDeliversWhenFilteredEntryFollows(t *testing.T) {
	srv := newServer(t, "")
	_, sess := call(t, srv, "POST", "/api/v1/sessions", nil, map[string]any{"workstation_id": "TRAY", "equipment_type": "KKT"})
	sid, dev := sess["session_id"].(string), sess["device_id"].(string)

	ctx, cancel := context.WithTimeout(context.Background(), 3*time.Second)
	defer cancel()
	req, _ := http.NewRequestWithContext(ctx, "GET", srv.URL+"/control/v1/journal/stream?method=device.print&workstation_id=TRAY", nil)
	resp, err := http.DefaultClient.Do(req)
	if err != nil {
		t.Fatal(err)
	}
	defer resp.Body.Close()
	got := make(chan string, 4)
	go func() {
		sc := bufio.NewScanner(resp.Body)
		sc.Buffer(make([]byte, 0, 64<<10), 1<<20)
		for sc.Scan() {
			if strings.HasPrefix(sc.Text(), "data: ") && strings.Contains(sc.Text(), "device.print") {
				got <- sc.Text()
			}
		}
	}()
	time.Sleep(100 * time.Millisecond) // подписка установлена

	call(t, srv, "POST", "/api/v1/devices/"+dev+"/commands", map[string]string{"X-Emulkon-Session": sid},
		map[string]any{"request_id": "o1", "method": "OpenShift", "params": []any{"h", "", ""}})
	select {
	case s := <-got:
		if !strings.Contains(s, `"kind":"open_shift"`) {
			t.Fatalf("ожидался отчёт об открытии смены: %s", s)
		}
	case <-ctx.Done():
		t.Fatal("device.print не дошёл до клиента: поток не сброшен после неподходящей записи")
	}
}

// Загрузки: страница и каталог открыты без токена Control API; сессия драйвера
// старой версии получает предложение обновиться DLL своей архитектуры.
func TestDownloadsAndDriverUpdateOffer(t *testing.T) {
	dir := t.TempDir()
	for name, body := range map[string]string{
		"EmulkonDriver_0.3.0.zip":       "zip",
		"EmulkonDriver_0.3.0_win64.dll": "dll64",
	} {
		if err := os.WriteFile(filepath.Join(dir, name), []byte(body), 0o644); err != nil {
			t.Fatal(err)
		}
	}
	a, err := New(Config{Version: "0.3.0", DataDir: t.TempDir(), ControlToken: secret.Plain("ctl"), DownloadsDirs: []string{dir}})
	if err != nil {
		t.Fatal(err)
	}
	srv := httptest.NewServer(a.Handler)
	t.Cleanup(func() { srv.Close(); a.Close() })

	res, err := http.Get(srv.URL + "/downloads")
	if err != nil {
		t.Fatal(err)
	}
	page, _ := io.ReadAll(res.Body)
	res.Body.Close()
	if res.StatusCode != 200 || !strings.Contains(string(page), "Что поддерживается") {
		t.Fatalf("страница загрузок: %d", res.StatusCode)
	}
	if res, out := call(t, srv, "GET", "/api/v1/downloads", nil, nil); res.StatusCode != 200 || out["published"] != true {
		t.Fatalf("каталог без токена: %d %v", res.StatusCode, out)
	}

	_, sess := call(t, srv, "POST", "/api/v1/sessions", nil, map[string]any{
		"workstation_id": "KASSA-01", "equipment_type": "KKT", "interface_revision": 4008,
		"driver_version": "0.2.0", "driver_arch": "x86_64",
	})
	upd, _ := sess["driver_update"].(map[string]any)
	if upd == nil || upd["version"] != "0.3.0" || upd["url"] != "/downloads/EmulkonDriver_0.3.0_win64.dll" {
		t.Fatalf("предложение обновления: %v", sess)
	}
	_, sess = call(t, srv, "POST", "/api/v1/sessions", nil, map[string]any{
		"workstation_id": "KASSA-02", "equipment_type": "KKT", "interface_revision": 4008,
		"driver_version": "0.3.0", "driver_arch": "x86_64",
	})
	if _, ok := sess["driver_update"]; ok {
		t.Fatalf("актуальной версии обновление не предлагается: %v", sess)
	}
}

// Эквайринг: оплата ждёт человека на экране терминала; ответ приходит через Control API,
// команда драйвера возвращает одобрение, в журнале — device.prompt и device.prompt_done.
func TestAcquiringInteractivePayment(t *testing.T) {
	srv := newServer(t, "")
	_, sess := call(t, srv, "POST", "/api/v1/sessions", nil, map[string]any{
		"workstation_id": "KASSA-01", "equipment_type": "ЭквайринговыйТерминал", "interface_revision": 4008,
	})
	sid, dev := sess["session_id"].(string), sess["device_id"].(string)
	if dev != "acquiring-01" {
		t.Fatalf("устройство: %v", sess)
	}
	hdr := map[string]string{"X-Emulkon-Session": sid}
	in := `<?xml version="1.0" encoding="UTF-8"?><OperationParameters RequestID="0000ABCD" MerchantNumber="0" UseBiometrics="0" AuthorizationType="0" Amount="111" AmountCash="0" AmountOriginalTransaction="0"/>`
	done := make(chan map[string]any, 1)
	go func() {
		_, res := call(t, srv, "POST", "/api/v1/devices/"+dev+"/commands", hdr,
			map[string]any{"request_id": "pay-1", "method": "Pay", "params": []any{"emulkon-1", in, "", ""}})
		done <- res
	}()

	var promptID string
	for i := 0; i < 200 && promptID == ""; i++ {
		_, devs := call(t, srv, "GET", "/control/v1/devices", nil, nil)
		for _, d := range devs["devices"].([]any) {
			st := d.(map[string]any)["state"].(map[string]any)
			if p, ok := st["prompt"].(map[string]any); ok {
				promptID, _ = p["prompt_id"].(string)
			}
		}
		if promptID == "" {
			time.Sleep(10 * time.Millisecond)
		}
	}
	if promptID == "" {
		t.Fatal("экран терминала не открыл запрос")
	}
	if res, out := call(t, srv, "POST", "/control/v1/devices/"+dev+"/prompt", nil,
		map[string]any{"prompt_id": "нет-такого", "action": "card"}); res.StatusCode != 409 {
		t.Fatalf("чужой запрос: %d %v", res.StatusCode, out)
	}
	if res, out := call(t, srv, "POST", "/control/v1/devices/"+dev+"/prompt", nil,
		map[string]any{"prompt_id": promptID, "action": "card", "card": "visa"}); res.StatusCode != 200 {
		t.Fatalf("ответ с экрана: %d %v", res.StatusCode, out)
	}
	var res map[string]any
	select {
	case res = <-done:
	case <-time.After(5 * time.Second):
		t.Fatal("команда оплаты не завершилась")
	}
	params, _ := res["params"].([]any)
	if res["result"] != true || len(params) != 4 || !strings.Contains(params[2].(string), `PaymentSystemIdentifier="4"`) ||
		!strings.Contains(params[3].(string), "ОДОБРЕНО") {
		t.Fatalf("оплата: %v", res)
	}
	_, j := call(t, srv, "GET", "/control/v1/journal?method=device.prompt,device.prompt_done", nil, nil)
	if n := len(j["entries"].([]any)); n != 2 {
		t.Fatalf("журнал экрана: %d записей", n)
	}
}

// waitPrompt ждёт открытый запрос на экране устройства dev.
func waitPrompt(t *testing.T, srv *httptest.Server, dev string) map[string]any {
	t.Helper()
	for i := 0; i < 200; i++ {
		_, devs := call(t, srv, "GET", "/control/v1/devices", nil, nil)
		for _, d := range devs["devices"].([]any) {
			dm := d.(map[string]any)
			if p, ok := dm["state"].(map[string]any)["prompt"].(map[string]any); ok && dm["device_id"] == dev {
				return p
			}
		}
		time.Sleep(10 * time.Millisecond)
	}
	t.Fatal("экран устройства не открыл запрос")
	return nil
}

func TestScalesWeightFromScreen(t *testing.T) {
	srv := newServer(t, "")
	_, sess := call(t, srv, "POST", "/api/v1/sessions", nil, map[string]any{"workstation_id": "KASSA-01", "equipment_type": "ЭлектронныеВесы"})
	sid, dev := sess["session_id"].(string), sess["device_id"].(string)
	hdr := map[string]string{"X-Emulkon-Session": sid}
	if res, out := call(t, srv, "PUT", "/control/v1/devices/"+dev+"/state", nil, map[string]any{"weight_mode": "ask"}); res.StatusCode != 200 {
		t.Fatalf("режим ask: %d %v", res.StatusCode, out)
	}
	done := make(chan map[string]any, 1)
	go func() {
		_, res := call(t, srv, "POST", "/api/v1/devices/"+dev+"/commands", hdr,
			map[string]any{"request_id": "w-1", "method": "GetWeight", "params": []any{"emulkon-1", 0}})
		done <- res
	}()
	p := waitPrompt(t, srv, dev)
	if in, _ := p["input"].(map[string]any); in["unit"] != "кг" {
		t.Fatalf("поле веса: %v", p)
	}
	if res, out := call(t, srv, "POST", "/control/v1/devices/"+dev+"/prompt", nil,
		map[string]any{"prompt_id": p["prompt_id"], "action": "weigh"}); res.StatusCode != 400 {
		t.Fatalf("без веса: %d %v", res.StatusCode, out)
	}
	if res, out := call(t, srv, "POST", "/control/v1/devices/"+dev+"/prompt", nil,
		map[string]any{"prompt_id": p["prompt_id"], "action": "weigh", "value": 1.234}); res.StatusCode != 200 {
		t.Fatalf("вес с экрана: %d %v", res.StatusCode, out)
	}
	var res map[string]any
	select {
	case res = <-done:
	case <-time.After(5 * time.Second):
		t.Fatal("GetWeight не завершился")
	}
	if params, _ := res["params"].([]any); res["result"] != true || len(params) != 2 || params[1] != 1.234 {
		t.Fatalf("GetWeight: %v", res)
	}
}

func TestLabelScalesUploadAndPrint(t *testing.T) {
	srv := newServer(t, "")
	_, sess := call(t, srv, "POST", "/api/v1/sessions", nil, map[string]any{"workstation_id": "SKLAD", "equipment_type": "LabelPrintingScale"})
	sid, dev := sess["session_id"].(string), sess["device_id"].(string)
	if dev != "label_scales-01" {
		t.Fatalf("устройство: %v", sess)
	}
	hdr := map[string]string{"X-Emulkon-Session": sid}
	cmd := func(id, method string, params ...any) map[string]any {
		t.Helper()
		res, out := call(t, srv, "POST", "/api/v1/devices/"+dev+"/commands", hdr, map[string]any{"request_id": id, "method": method, "params": params})
		if res.StatusCode != 200 || out["result"] != true {
			t.Fatalf("%s: %d %v", method, res.StatusCode, out)
		}
		return out
	}
	cmd("g-1", "UploadGoods", "emulkon-1", `<Table><Record PLU="1" Code="42" IsWeightGoods="true" Name="Сыр" Price="500"/></Table>`, "last")

	// картинка больше 1 МБ проходит, а в журнал попадает обрезанной
	pic := append([]byte{0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'}, bytes.Repeat([]byte{7}, 1200<<10)...)
	cmd("i-1", "UploadProductImages", "emulkon-1", `<Table><Record PLU="1" Code="42" Picture="`+base64.StdEncoding.EncodeToString(pic)+`"/></Table>`, "last")
	_, j := call(t, srv, "GET", "/control/v1/journal?method=UploadProductImages", nil, nil)
	for _, e := range j["entries"].([]any) {
		if b, _ := json.Marshal(e); len(b) > 200<<10 || !strings.Contains(string(b), "в журнале обрезано") {
			t.Fatalf("запись журнала с картинкой: %d байт", len(b))
		}
	}

	if res, out := call(t, srv, "POST", "/control/v1/devices/"+dev+"/actions/print_label", nil, map[string]any{"plu": 7, "weight": 1}); res.StatusCode != 400 {
		t.Fatalf("PLU 7: %d %v", res.StatusCode, out)
	}
	if res, out := call(t, srv, "POST", "/control/v1/devices/"+dev+"/actions/fly", nil, nil); res.StatusCode != 404 {
		t.Fatalf("нет действия: %d %v", res.StatusCode, out)
	}
	res, out := call(t, srv, "POST", "/control/v1/devices/"+dev+"/actions/print_label", nil, map[string]any{"plu": 1, "weight": 0.25})
	doc, _ := out["result"].(map[string]any)
	if res.StatusCode != 200 || doc["kind"] != "label" || !strings.Contains(fmt.Sprint(doc["lines"]), "2100042002500") {
		t.Fatalf("этикетка: %d %v", res.StatusCode, out)
	}
	_, j = call(t, srv, "GET", "/control/v1/journal?method=device.print&workstation_id=SKLAD", nil, nil)
	if n := len(j["entries"].([]any)); n != 1 {
		t.Fatalf("этикетка в журнале device.print: %d", n)
	}
	_, prints := call(t, srv, "GET", "/control/v1/devices/"+dev+"/prints", nil, nil)
	if n := len(prints["prints"].([]any)); n != 1 {
		t.Fatalf("лента весов: %v", prints)
	}
	// новая команда драйвера не повторяет этикетку в журнале
	cmd("c-1", "ClearGoods", "emulkon-1")
	_, j = call(t, srv, "GET", "/control/v1/journal?method=device.print", nil, nil)
	if n := len(j["entries"].([]any)); n != 1 {
		t.Fatalf("этикетка задвоилась: %d", n)
	}
}

func TestPrintersEndToEnd(t *testing.T) {
	srv := newServer(t, "")
	open := func(typ string) (string, map[string]string) {
		_, s := call(t, srv, "POST", "/api/v1/sessions", nil, map[string]any{"workstation_id": "KASSA-02", "equipment_type": typ})
		return s["device_id"].(string), map[string]string{"X-Emulkon-Session": s["session_id"].(string)}
	}
	cmd := func(dev string, hdr map[string]string, id, method string, params ...any) map[string]any {
		t.Helper()
		res, out := call(t, srv, "POST", "/api/v1/devices/"+dev+"/commands", hdr, map[string]any{"request_id": id, "method": method, "params": params})
		if res.StatusCode != 200 {
			t.Fatalf("%s: HTTP %d %v", method, res.StatusCode, out)
		}
		return out
	}

	rp, rh := open("ПринтерЧеков")
	if rp != "receipt_printer-01" {
		t.Fatalf("устройство: %s", rp)
	}
	doc := `<Document><Positions><TextString Text="Товарный чек №5"/><Barcode Type="EAN13" ValueBase64="` +
		base64.StdEncoding.EncodeToString([]byte("2000021262157")) + `"/></Positions></Document>`
	if out := cmd(rp, rh, "p-1", "PrintTextDocument", "emulkon-1", doc); out["result"] != true {
		t.Fatalf("печать: %v", out)
	}
	if out := cmd(rp, rh, "w-1", "GetLineLength", "emulkon-1", 0); out["params"].([]any)[1] != float64(42) {
		t.Fatalf("ширина: %v", out)
	}
	call(t, srv, "PUT", "/control/v1/devices/"+rp+"/state", nil, map[string]any{"paper": false})
	if out := cmd(rp, rh, "p-2", "PrintTextDocument", "emulkon-1", doc); out["result"] != false || out["error_code"] != float64(90) {
		t.Fatalf("без бумаги: %v", out)
	}

	lp, lh := open("LabelPrinter")
	labels := `<Data><Formatting Width="58" Height="30"><Text FieldName="Имя" Left="0" Top="0" Width="58" Height="8" FontSize="9"/>` +
		`<Barcode FieldName="ШК" Type="EAN13" Left="0" Top="10" Height="15" PrintHRI="true" FontSize="8"/></Formatting>` +
		`<Labels><Label Quantity="2"><Record FieldName="Имя" Value="Ручка шариковая"/><Record FieldName="ШК" Value="4600000000008"/></Label></Labels></Data>`
	if out := cmd(lp, lh, "l-1", "PrintLabels", "emulkon-1", labels, "last"); out["result"] != true {
		t.Fatalf("этикетки: %v", out)
	}
	_, j := call(t, srv, "GET", "/control/v1/journal?method=device.print&workstation_id=KASSA-02", nil, nil)
	entries := j["entries"].([]any)
	if len(entries) != 2 {
		t.Fatalf("документов в журнале: %d", len(entries))
	}
	last := entries[1].(map[string]any)
	p := last["payload"].(map[string]any)
	layout, _ := p["layout"].(map[string]any)
	if last["device_id"] != lp || p["kind"] != "label" || layout == nil || layout["width_mm"] != float64(58) ||
		!strings.Contains(fmt.Sprint(p["lines"]), "ШК: [EAN13: 4600000000008]") {
		t.Fatalf("этикетка в журнале: %v", last)
	}
}

func TestDeviceEventsStream(t *testing.T) {
	srv := newServer(t, "")
	_, sess := call(t, srv, "POST", "/api/v1/sessions", nil, map[string]any{"workstation_id": "KASSA-03", "equipment_type": "СканерШтрихкода"})
	sid, dev := sess["session_id"].(string), sess["device_id"].(string)
	if dev != "scanner-01" {
		t.Fatalf("устройство: %v", sess)
	}
	if res, _ := call(t, srv, "GET", "/api/v1/devices/"+dev+"/events", nil, nil); res.StatusCode != 401 {
		t.Fatalf("без сессии: %d", res.StatusCode)
	}
	// событие до подписки: без курсора поток его не отдаёт
	call(t, srv, "POST", "/control/v1/devices/"+dev+"/actions/scan", nil, map[string]any{"barcode": "0000"})

	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	open := func(cursor string) (chan [2]string, func()) {
		req, _ := http.NewRequestWithContext(ctx, "GET", srv.URL+"/api/v1/devices/"+dev+"/events", nil)
		req.Header.Set("X-Emulkon-Session", sid)
		if cursor != "" {
			req.Header.Set("Last-Event-ID", cursor)
		}
		resp, err := http.DefaultClient.Do(req)
		if err != nil {
			t.Fatal(err)
		}
		out := make(chan [2]string, 16)
		go func() {
			sc := bufio.NewScanner(resp.Body)
			var id, ev string
			for sc.Scan() {
				line := sc.Text()
				switch {
				case strings.HasPrefix(line, "id: "):
					id = line[4:]
				case strings.HasPrefix(line, "event: "):
					ev = line[7:]
				case strings.HasPrefix(line, "data: "):
					if ev == "device" {
						out <- [2]string{id, line[6:]}
					} else if ev == "ready" {
						out <- [2]string{"ready", ""}
					}
				}
			}
			close(out)
		}()
		return out, func() { resp.Body.Close() }
	}
	next := func(ch chan [2]string) [2]string {
		t.Helper()
		select {
		case v := <-ch:
			return v
		case <-ctx.Done():
			t.Fatal("нет события")
		}
		return [2]string{}
	}

	ch, stop := open("")
	if v := next(ch); v[0] != "ready" {
		t.Fatalf("начало потока: %v", v)
	}
	_, devs := call(t, srv, "GET", "/control/v1/devices", nil, nil)
	if d := devs["devices"].([]any)[0].(map[string]any); d["event_listeners"] != float64(1) {
		t.Fatalf("слушатели: %v", d["event_listeners"])
	}
	call(t, srv, "POST", "/control/v1/devices/"+dev+"/actions/scan", nil, map[string]any{"barcode": "4600000000008"})
	first := next(ch)
	var e map[string]any
	json.Unmarshal([]byte(first[1]), &e)
	if e["name"] != "Barcode" || e["data"] != "4600000000008" || e["event_id"] != first[0] {
		t.Fatalf("событие: %v", first)
	}
	stop()

	// пропущенное, пока компонента переподключалась, — по курсору
	call(t, srv, "POST", "/control/v1/devices/"+dev+"/events", nil, map[string]any{"name": "DriverError", "data": "Нет связи со сканером"})
	ch, stop = open(first[0])
	defer stop()
	missed := next(ch)
	if !strings.Contains(missed[1], `"DriverError"`) {
		t.Fatalf("пропущенное событие: %v", missed)
	}
	if res, out := call(t, srv, "POST", "/control/v1/devices/"+dev+"/events", nil, map[string]any{"name": "Hello", "data": "x"}); res.StatusCode != 400 {
		t.Fatalf("событие не из требований: %d %v", res.StatusCode, out)
	}
	_, j := call(t, srv, "GET", "/control/v1/journal?direction=event&device_id="+dev, nil, nil)
	if n := len(j["entries"].([]any)); n != 3 {
		t.Fatalf("событий в журнале: %d", n)
	}
}

// Сценарии сбоев через Device API (ARCHITECTURE §7): ошибка вместо модели и GetLastError,
// обрыв после выполнения и повтор с тем же request_id, отметка в журнале, сброс счётчиков.
func TestScenariosEndToEnd(t *testing.T) {
	srv := newServer(t, "")
	_, sess := call(t, srv, "POST", "/api/v1/sessions", nil, map[string]any{"workstation_id": "KASSA-S", "equipment_type": "KKT", "interface_revision": 4008})
	sid, dev := sess["session_id"].(string), sess["device_id"].(string)
	hdr := map[string]string{"X-Emulkon-Session": sid}
	cmd := func(id, method string, params ...any) (*http.Response, map[string]any) {
		return call(t, srv, "POST", "/api/v1/devices/"+dev+"/commands", hdr, map[string]any{"request_id": id, "method": method, "params": params})
	}

	res, rule := call(t, srv, "POST", "/control/v1/scenarios", nil, map[string]any{
		"name": "нет бумаги на втором тесте", "enabled": true,
		"match":  map[string]any{"device": dev, "method": "EquipmentTest", "nth_call": 2},
		"action": map[string]any{"type": "error", "error_code": 3, "error_text": "Нет бумаги (сценарий)"}})
	if res.StatusCode != 200 || rule["id"] == "" {
		t.Fatalf("правило: %d %v", res.StatusCode, rule)
	}
	if res, _ := call(t, srv, "POST", "/control/v1/scenarios", nil, map[string]any{"name": "плохое", "action": map[string]any{"type": "boom"}}); res.StatusCode != 400 {
		t.Fatalf("неверное правило — 400: %d", res.StatusCode)
	}
	test := []any{"KKT", "<Parameters/>", "", ""}
	if _, r := cmd("t-1", "EquipmentTest", test...); r["result"] != true {
		t.Fatalf("первый вызов — как обычно: %v", r)
	}
	if _, r := cmd("t-2", "EquipmentTest", test...); r["result"] != false || r["error_code"].(float64) != 3 {
		t.Fatalf("второй — ошибка сценария: %v", r)
	}
	if _, r := cmd("t-3", "GetLastError", ""); r["params"].([]any)[0] != "Нет бумаги (сценарий)" {
		t.Fatalf("GetLastError после сценария: %v", r)
	}
	_, j := call(t, srv, "GET", "/control/v1/journal?correlation_id=t-2", nil, nil)
	for _, e := range j["entries"].([]any) {
		if ar, _ := e.(map[string]any)["applied_rules"].([]any); len(ar) != 1 {
			t.Fatalf("сработавшее правило — в журнале: %v", e)
		}
	}

	// Обрыв: команда выполнена, ответ потерян; повтор с тем же request_id получает сохранённый.
	call(t, srv, "PUT", "/control/v1/scenarios/"+rule["id"].(string), nil, map[string]any{
		"name": "обрыв", "enabled": true, "limit": 1,
		"match": map[string]any{"method": "EquipmentTest"}, "action": map[string]any{"type": "drop"}})
	body, _ := json.Marshal(map[string]any{"request_id": "t-4", "method": "EquipmentTest", "params": test})
	req, _ := http.NewRequest("POST", srv.URL+"/api/v1/devices/"+dev+"/commands", bytes.NewReader(body))
	req.Header.Set("X-Emulkon-Session", sid)
	if resp, err := http.DefaultClient.Do(req); err == nil {
		resp.Body.Close()
		t.Fatalf("соединение должно оборваться, получен ответ %d", resp.StatusCode)
	}
	if _, r := cmd("t-4", "EquipmentTest", test...); r["result"] != true {
		t.Fatalf("повтор после обрыва — сохранённый результат: %v", r)
	}
	_, devs := call(t, srv, "GET", "/control/v1/devices", nil, nil)
	if n := devs["devices"].([]any)[0].(map[string]any)["commands"].(float64); n != 4 {
		t.Fatalf("команда после обрыва выполнена один раз (t-1, t-2, t-3, t-4): %v", n)
	}
	_, list := call(t, srv, "GET", "/control/v1/scenarios", nil, nil)
	if r := list["rules"].([]any)[0].(map[string]any); r["fired"].(float64) != 1 {
		t.Fatalf("счётчик срабатываний: %v", r)
	}
	call(t, srv, "POST", "/control/v1/reset", nil, nil)
	_, list = call(t, srv, "GET", "/control/v1/scenarios", nil, nil)
	if r := list["rules"].([]any)[0].(map[string]any); r["fired"].(float64) != 0 {
		t.Fatalf("сброс обнуляет счётчики: %v", r)
	}
	if res, _ := call(t, srv, "DELETE", "/control/v1/scenarios/"+rule["id"].(string), nil, nil); res.StatusCode != 200 {
		t.Fatalf("удаление: %d", res.StatusCode)
	}
}

// Клавиатурный режим сканера: действие scan пишет device.keyboard для трея рабочего места.
func TestScannerKeyboardJournal(t *testing.T) {
	srv := newServer(t, "")
	_, sess := call(t, srv, "POST", "/api/v1/sessions", nil, map[string]any{"workstation_id": "KASSA-K", "equipment_type": "BarcodeScanner", "interface_revision": 4008})
	dev := sess["device_id"].(string)
	if res, _ := call(t, srv, "PUT", "/control/v1/devices/"+dev+"/state", nil, map[string]any{"input_mode": "keyboard", "keyboard_delay_ms": 500}); res.StatusCode != 200 {
		t.Fatalf("режим клавиатуры: %d", res.StatusCode)
	}
	call(t, srv, "POST", "/control/v1/devices/"+dev+"/actions/scan", nil, map[string]any{"barcode": "4601234567893"})
	_, j := call(t, srv, "GET", "/control/v1/journal?method=device.keyboard&workstation_id=KASSA-K", nil, nil)
	entries := j["entries"].([]any)
	if len(entries) != 1 {
		t.Fatalf("device.keyboard: %v", j)
	}
	p := entries[0].(map[string]any)["payload"].(map[string]any)
	if p["text"] != "4601234567893" || p["suffix"] != "enter" || p["delay_ms"].(float64) != 500 {
		t.Fatalf("что вводить: %v", p)
	}
	_, j = call(t, srv, "GET", "/control/v1/journal?direction=event&workstation_id=KASSA-K", nil, nil)
	if n := len(j["entries"].([]any)); n != 0 {
		t.Fatalf("только клавиатура — событий в 1С нет: %d", n)
	}
}

// Токен рабочего места (docs/protocol.md §1.1): выдача в Control API, подключение по токену без
// рабочего места в параметрах, отказ без токена; сам токен в журнал не попадает.
func TestWorkstationTokenEndToEnd(t *testing.T) {
	srv := newServer(t, "")
	res, tok := call(t, srv, "POST", "/control/v1/workstations/KASSA-3/token", nil, nil)
	token, _ := tok["token"].(string)
	if res.StatusCode != 200 || token == "" {
		t.Fatalf("выдача: %d %v", res.StatusCode, tok)
	}
	_, sess := call(t, srv, "POST", "/api/v1/sessions", nil, map[string]any{"workstation_id": "DESKTOP-X",
		"workstation_auto": true, "device_token": token, "equipment_type": "KKT"})
	if sess["device_id"] != "kkt-01" {
		t.Fatalf("сессия по токену: %v", sess)
	}
	if res, out := call(t, srv, "POST", "/api/v1/sessions", nil, map[string]any{"workstation_id": "KASSA-3", "equipment_type": "KKT"}); res.StatusCode != 401 || out["error"] == nil {
		t.Fatalf("без токена: %d %v", res.StatusCode, out)
	}
	_, ws := call(t, srv, "GET", "/control/v1/workstations", nil, nil)
	w := ws["workstations"].([]any)
	if len(w) != 1 || w[0].(map[string]any)["workstation_id"] != "KASSA-3" || w[0].(map[string]any)["token"] == nil {
		t.Fatalf("рабочие места: %v", ws)
	}
	_, info := call(t, srv, "GET", "/control/v1/info", nil, nil)
	if info["workstation_tokens"] != 1.0 {
		t.Fatalf("info: %v", info["workstation_tokens"])
	}
	_, j := call(t, srv, "GET", "/control/v1/journal", nil, nil)
	raw, _ := json.Marshal(j)
	if strings.Contains(string(raw), token) || !strings.Contains(string(raw), `"auth":"workstation"`) {
		t.Fatalf("журнал: токен не пишется, способ допуска — пишется: %s", raw)
	}
	if res, _ := call(t, srv, "DELETE", "/control/v1/workstations/KASSA-3/token", nil, nil); res.StatusCode != 200 {
		t.Fatalf("отзыв: %d", res.StatusCode)
	}
	if res, _ := call(t, srv, "POST", "/api/v1/sessions", nil, map[string]any{"workstation_id": "KASSA-3", "equipment_type": "KKT"}); res.StatusCode != 200 {
		t.Fatalf("после отзыва: %d", res.StatusCode)
	}
}

// Сессия закрывается после DisconnectEquipment и по DELETE (docs/protocol.md §1.1): в «Сессиях»
// не висят отключённые устройства, команда в закрытую сессию — session_unknown.
func TestSessionClosedOnDisconnect(t *testing.T) {
	srv := newServer(t, "")
	open := func() (string, string) {
		_, s := call(t, srv, "POST", "/api/v1/sessions", nil, map[string]any{"workstation_id": "KASSA-D", "equipment_type": "KKT"})
		return s["session_id"].(string), s["device_id"].(string)
	}
	sessionsOpen := func() int {
		_, out := call(t, srv, "GET", "/control/v1/sessions", nil, nil)
		return len(out["sessions"].([]any))
	}
	sid, dev := open()
	hdr := map[string]string{"X-Emulkon-Session": sid}
	_, r := call(t, srv, "POST", "/api/v1/devices/"+dev+"/commands", hdr, map[string]any{"request_id": "d-1", "method": "DisconnectEquipment", "params": []any{"emulkon-1"}})
	if r["result"] != true || sessionsOpen() != 0 {
		t.Fatalf("отключение закрывает сессию: %v, открыто %d", r, sessionsOpen())
	}
	if res, _ := call(t, srv, "POST", "/api/v1/devices/"+dev+"/commands", hdr, map[string]any{"request_id": "d-2", "method": "GetLastError", "params": []any{""}}); res.StatusCode != 401 {
		t.Fatalf("команда в закрытую сессию: %d", res.StatusCode)
	}
	// Повтор отключения из новой сессии (ответ потерялся) — сохранённый результат, без второго выполнения.
	sid2, _ := open()
	_, r = call(t, srv, "POST", "/api/v1/devices/"+dev+"/commands", map[string]string{"X-Emulkon-Session": sid2},
		map[string]any{"request_id": "d-1", "method": "DisconnectEquipment", "params": []any{"emulkon-1"}})
	if r["result"] != true {
		t.Fatalf("повтор отключения: %v", r)
	}
	sid3, _ := open()
	if res, _ := call(t, srv, "DELETE", "/api/v1/sessions/"+sid3, nil, nil); res.StatusCode != 200 || sessionsOpen() != 0 {
		t.Fatalf("DELETE сессии: %d, открыто %d", res.StatusCode, sessionsOpen())
	}
	if res, _ := call(t, srv, "DELETE", "/api/v1/sessions/s_nope", nil, nil); res.StatusCode != 200 {
		t.Fatalf("DELETE неизвестной сессии: %d", res.StatusCode)
	}
	_, j := call(t, srv, "GET", "/control/v1/journal?method=session.closed", nil, nil)
	if n := len(j["entries"].([]any)); n != 3 {
		t.Fatalf("session.closed в журнале: %d", n)
	}
}
