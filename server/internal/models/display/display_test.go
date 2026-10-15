package display

import (
	"context"
	"encoding/base64"
	"encoding/json"
	"strings"
	"testing"

	"emulkon/internal/models"
)

func call(m *Model, method string, params ...any) models.Result {
	return m.Handle(context.Background(), method, params)
}

func ok(r models.Result) bool { b, _ := r.Return.(bool); return b }

var png = base64.StdEncoding.EncodeToString([]byte("\x89PNG\r\n\x1a\nIHDR"))

func TestDisplayOptionsAndStrings(t *testing.T) {
	m := New(ModelOptions{Version: "test"})
	r := call(m, "GetCustomerDisplayOptions", "emulkon-1", "")
	s, _ := r.Params[1].(string)
	for _, want := range []string{`Columns="20"`, `Rows="2"`, `DisplayText="true"`, `DisplayQRCode="true"`, `DisplayHTML="true"`,
		`DisplayInteractiveMode="false"`, `DisplayWorkplaceStatus="true"`} {
		if !strings.Contains(s, want) {
			t.Errorf("параметры без %s: %s", want, s)
		}
	}
	call(m, "StringOutputOnCustomerDisplay", "emulkon-1", "Молоко 3,2% 1 л очень длинное\r\nИтого: 89.90")
	call(m, "StringOutputOnCustomerDisplay", "emulkon-1", "\nК оплате: 89.90\nлишняя строка")
	lines := m.State()["lines"].([]string)
	if lines[0] != "Молоко 3,2% 1 л очен" || lines[1] != "К оплате: 89.90" {
		t.Fatalf("строки: %q", lines)
	}
	if r := call(m, "ClearCustomerDisplay", "emulkon-1"); !ok(r) || m.State()["lines"].([]string)[0] != "" {
		t.Fatalf("очистка: %+v", r)
	}
}

func TestDisplayQRHTMLStatuses(t *testing.T) {
	m := New(ModelOptions{Version: "test"})
	if r := call(m, "QRCodeOutputOnCustomerDisplay", "emulkon-1", "https://qr.nspk.ru/AS1000", png); !ok(r) {
		t.Fatalf("QR: %+v", r)
	}
	if st := m.State(); st["shown"] != "qr" || st["qr_view"].(*qrView).Value != "https://qr.nspk.ru/AS1000" {
		t.Fatalf("QR на экране: %+v", st)
	}
	if r := call(m, "QRCodeOutputOnCustomerDisplay", "emulkon-1", "x", "bm90IHBuZw=="); ok(r) || r.ErrorCode != ErrBadParams {
		t.Fatalf("не PNG: %+v", r)
	}
	if r := call(m, "AddMediaFile", "emulkon-1", "logo.png", png); !ok(r) {
		t.Fatalf("медиафайл: %+v", r)
	}
	if r := call(m, "AddMediaFile", "emulkon-1", "../x.png", png); ok(r) {
		t.Fatal("путь в имени файла принят")
	}
	if r := call(m, "OutputHTML", "emulkon-1", `<html><body><img src="logo.png">Спасибо!</body></html>`); !ok(r) || m.State()["shown"] != "html" {
		t.Fatalf("HTML: %+v", r)
	}
	if res, err := m.Act("media", map[string]any{"name": "logo.png"}); err != nil || res.(map[string]any)["data_base64"] != png {
		t.Fatalf("медиафайл для показа: %v %v", res, err)
	}
	if r := call(m, "DisplayOperationStatus", "emulkon-1", json.Number("1")); !ok(r) || m.State()["operation_status"] != 1 {
		t.Fatalf("статус операции: %+v", r)
	}
	if r := call(m, "DisplayOperationStatus", "emulkon-1", json.Number("5")); ok(r) {
		t.Fatal("статус 5 принят")
	}
	if r := call(m, "DisplayWorkplaceStatus", "emulkon-1", json.Number("3"), json.Number("1")); !ok(r) || m.State()["beeps"] != 1 {
		t.Fatalf("статус рабочего места: %+v", r)
	}
	// выключенная возможность — 85, дисплей не отвечает — 86
	if err := m.SetState(map[string]any{"html": false}); err != nil {
		t.Fatal(err)
	}
	if r := call(m, "OutputHTML", "emulkon-1", "<p>x</p>"); ok(r) || r.ErrorCode != ErrNoCapability {
		t.Fatalf("HTML выключен: %+v", r)
	}
	m.SetState(map[string]any{"broken": true})
	if r := call(m, "StringOutputOnCustomerDisplay", "emulkon-1", "x"); ok(r) || r.ErrorCode != ErrNoResponse {
		t.Fatalf("не отвечает: %+v", r)
	}
}

func TestDisplayClick(t *testing.T) {
	m := New(ModelOptions{Version: "test"})
	var got []models.DeviceEvent
	m.SetEventHook(func(e models.DeviceEvent) { got = append(got, e) })
	if _, err := m.Act("click", map[string]any{"element": "ОплатаСБП"}); err == nil {
		t.Fatal("нажатие без interactive")
	}
	m.SetState(map[string]any{"interactive": true, "columns": 40.0, "rows": 4.0})
	if _, err := m.Act("click", map[string]any{"element": "ОплатаСБП"}); err != nil || len(got) != 1 || got[0] != (models.DeviceEvent{Name: "OnClick", Data: "ОплатаСБП"}) {
		t.Fatalf("нажатие: %v %v", got, err)
	}
	if st := m.State(); len(st["lines"].([]string)) != 4 || st["columns"] != 40 {
		t.Fatalf("размер: %+v", st)
	}
	for _, bad := range []map[string]any{{"rows": 0.0}, {"columns": 100.0}, {"text": "да"}, {"x": true}} {
		if err := m.SetState(bad); err == nil {
			t.Errorf("%v принят", bad)
		}
	}
}
