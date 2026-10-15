package rfid

import (
	"context"
	"encoding/json"
	"strings"
	"testing"

	"emulkon/internal/models"
)

func call(m *Model, method string, params ...any) models.Result {
	return m.Handle(context.Background(), method, params)
}

func ok(r models.Result) bool { b, _ := r.Return.(bool); return b }

func TestSessionPackagesAndWrite(t *testing.T) {
	m := New(Options{Version: "test"})
	var events []models.DeviceEvent
	m.SetEventHook(func(e models.DeviceEvent) { events = append(events, e) })

	// метка в поле до сессии: событий нет, при открытии сессии — первым пакетом
	m.Act("add_tag", map[string]any{"tid": "E28011002000589616B408A1", "epc": "3074367C34001E880A6D6168"})
	if len(events) != 0 {
		t.Fatal("событие без сессии")
	}
	if r := call(m, "OpenSessionRFID", "emulkon-1"); !ok(r) || len(events) != 1 || events[0] != (models.DeviceEvent{Name: "RFID", Data: "pkg-1"}) {
		t.Fatalf("открытие сессии: %+v %v", r, events)
	}
	res, _ := m.Act("add_tag", nil) // сгенерированная
	tag := res.(Tag)
	if len(tag.TID) != 24 || !strings.HasPrefix(tag.EPC, "30") || len(events) != 2 || events[1].Data != "pkg-2" {
		t.Fatalf("новая метка: %+v %v", tag, events)
	}
	r := call(m, "GetDataTagsRFID", "emulkon-1", "pkg-1", "")
	s, _ := r.Params[2].(string)
	if !ok(r) || !strings.Contains(s, `<Tag TID="E28011002000589616B408A1" EPC="3074367C34001E880A6D6168" USER=""></Tag>`) {
		t.Fatalf("данные пакета: %s", s)
	}
	if r := call(m, "GetDataTagsRFID", "emulkon-1", "pkg-9", ""); ok(r) || r.ErrorCode != ErrNoPackage {
		t.Fatalf("чужой пакет: %+v", r)
	}

	// запись в USER, затем в EPC
	r = call(m, "SaveDataTagRFID", "emulkon-1", "E28011002000589616B408A1", "3074367C34001E880A6D6168", "cafe", json.Number("2"), json.Number("1000"))
	if !ok(r) || m.State()["field"].([]Tag)[0].USER != "CAFE" {
		t.Fatalf("запись USER: %+v", r)
	}
	r = call(m, "SaveDataTagRFID", "emulkon-1", "E28011002000589616B408A1", "", "3000AAAA", json.Number("1"), json.Number("1000"))
	if !ok(r) || r.Params[2] != "3000AAAA" {
		t.Fatalf("запись EPC: %+v", r)
	}
	if r := call(m, "SaveDataTagRFID", "emulkon-1", "E2800000", "", "00", json.Number("2"), json.Number("1000")); ok(r) || r.ErrorCode != ErrNoTag {
		t.Fatalf("нет метки: %+v", r)
	}
	if r := call(m, "SaveDataTagRFID", "emulkon-1", "E28011002000589616B408A1", "", "zz", json.Number("2"), json.Number("1000")); ok(r) {
		t.Fatal("не HEX принят")
	}
	call(m, "CloseSessionRFID", "emulkon-1")
	if r := call(m, "SaveDataTagRFID", "emulkon-1", "E28011002000589616B408A1", "", "00", json.Number("2"), json.Number("1000")); ok(r) || r.ErrorCode != ErrNoSession {
		t.Fatalf("без сессии: %+v", r)
	}
	m.Act("add_tag", nil)
	if len(events) != 2 {
		t.Fatal("событие после закрытия сессии")
	}
	if _, err := m.Act("add_tag", map[string]any{"tid": "E28011002000589616B408A1"}); err == nil {
		t.Fatal("дубликат метки")
	}
	if st := m.State(); st["writes"] != 2 || len(st["packages"].([]pkg)) != 2 || !st["packages"].([]pkg)[1].Delivered {
		t.Fatalf("состояние: %+v", st)
	}
}
