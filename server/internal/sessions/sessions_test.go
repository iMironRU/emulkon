package sessions

import (
	"context"
	"errors"
	"testing"
	"time"

	"emulkon/internal/models"
	"emulkon/internal/secret"
)

func newManager(token string, now *time.Time) *Manager {
	return NewManager(Config{DeviceToken: secret.Plain(token), IdleTTL: time.Hour, Now: func() time.Time { return *now }},
		models.DefaultRegistry("test"))
}

func TestOpenBindsDevicePerWorkstationAndType(t *testing.T) {
	now := time.Date(2026, 10, 8, 12, 0, 0, 0, time.UTC)
	m := newManager("", &now)

	s1, d1, isNew, err := m.Open(OpenRequest{WorkstationID: "KASSA-01", EquipmentType: "KKT"})
	if err != nil || !isNew || d1.ID != "kkt-01" {
		t.Fatalf("первое подключение: d=%v new=%v err=%v", d1, isNew, err)
	}
	// Русское написание типа из БПО — тот же тип и то же устройство.
	s2, d2, isNew, _ := m.Open(OpenRequest{WorkstationID: "KASSA-01", EquipmentType: "ККТ"})
	if d2 != d1 || isNew || s2.ID == s1.ID {
		t.Fatalf("переподключение должно дать то же устройство и новую сессию: %s/%s new=%v", d1.ID, d2.ID, isNew)
	}
	_, d3, _, _ := m.Open(OpenRequest{WorkstationID: "KASSA-02", EquipmentType: "kkt"})
	_, d4, _, _ := m.Open(OpenRequest{WorkstationID: "KASSA-01", EquipmentType: "WeighingScales"})
	_, d5, _, _ := m.Open(OpenRequest{WorkstationID: "KASSA-01", EquipmentType: "ЭквайринговыйТерминал"})
	if d3.ID != "kkt-02" || d4.ID != "scales-01" || d5.ID != "acquiring-01" {
		t.Fatalf("нумерация: %s, %s, %s", d3.ID, d4.ID, d5.ID)
	}
	if d5.Type.Name != "POSTerminal" {
		t.Fatalf("тип эквайринга: %+v", d5.Type)
	}
}

func TestOpenValidation(t *testing.T) {
	now := time.Now()
	m := newManager("secret", &now)
	cases := []struct {
		req  OpenRequest
		want error
	}{
		{OpenRequest{WorkstationID: "", EquipmentType: "KKT", DeviceToken: "secret"}, ErrBadRequest},
		{OpenRequest{WorkstationID: "K", EquipmentType: "Toaster", DeviceToken: "secret"}, ErrUnknownType},
		{OpenRequest{WorkstationID: "K", EquipmentType: "KKT", DeviceToken: "wrong"}, ErrBadToken},
	}
	for _, c := range cases {
		if _, _, _, err := m.Open(c.req); !errors.Is(err, c.want) {
			t.Errorf("%+v: err=%v, want %v", c.req, err, c.want)
		}
	}
	if _, _, _, err := m.Open(OpenRequest{WorkstationID: "K", EquipmentType: "KKT", DeviceToken: "secret"}); err != nil {
		t.Fatalf("верный токен: %v", err)
	}
}

func TestResolveAndExpire(t *testing.T) {
	now := time.Date(2026, 10, 8, 12, 0, 0, 0, time.UTC)
	m := newManager("", &now)
	s, d, _, _ := m.Open(OpenRequest{WorkstationID: "K", EquipmentType: "KKT"})

	if _, _, err := m.Resolve(s.ID, "scales-01"); !errors.Is(err, ErrWrongDevice) {
		t.Fatalf("чужое устройство: %v", err)
	}
	if _, _, err := m.Resolve("s_nope", d.ID); !errors.Is(err, ErrUnknownSession) {
		t.Fatalf("неизвестная сессия: %v", err)
	}

	now = now.Add(50 * time.Minute)
	if _, _, err := m.Resolve(s.ID, d.ID); err != nil { // продлевает активность
		t.Fatal(err)
	}
	now = now.Add(50 * time.Minute)
	if gone := m.ExpireIdle(); len(gone) != 0 {
		t.Fatalf("сессия активна 50 минут назад, а удалена")
	}
	now = now.Add(2 * time.Hour)
	if gone := m.ExpireIdle(); len(gone) != 1 {
		t.Fatalf("простаивающая сессия не удалена")
	}
	if len(m.Devices()) != 1 {
		t.Fatal("устройство должно пережить сессию")
	}
}

func TestExecSerializesAndHonorsContext(t *testing.T) {
	now := time.Now()
	m := newManager("", &now)
	_, d, _, _ := m.Open(OpenRequest{WorkstationID: "K", EquipmentType: "KKT"})

	d.sem <- struct{}{} // устройство «занято»
	ctx, cancel := context.WithTimeout(context.Background(), 20*time.Millisecond)
	defer cancel()
	if _, _, err := d.Exec(ctx, "M", nil, now); !errors.Is(err, context.DeadlineExceeded) {
		t.Fatalf("ожидание занятого устройства должно прерываться: %v", err)
	}
	<-d.sem

	res, _, err := d.Exec(context.Background(), "M", []any{"x"}, now)
	if err != nil || res.Return != true || len(res.Params) != 1 || res.Params[0] != "x" {
		t.Fatalf("Exec: %+v %v", res, err)
	}
	if got := m.Devices()[0].Commands; got != 1 {
		t.Fatalf("commands = %d", got)
	}
	m.ResetDevices()
	if calls := m.Devices()[0].State["calls_total"]; calls != 0 {
		t.Fatalf("после сброса calls_total = %v", calls)
	}
}

// failModel возвращает ошибку устройства на любой вызов.
type failModel struct{ *models.Stub }

func (failModel) Handle(context.Context, string, []any) models.Result {
	return models.Result{Return: false, ErrorCode: 3, ErrorText: "Нет бумаги"}
}

func TestGetLastErrorAndReset(t *testing.T) {
	now := time.Now()
	reg := models.NewRegistry()
	reg.Register("kkt", func(t models.EquipmentType, id string) models.Model {
		return failModel{models.NewStub(t, id, "test")}
	})
	m := NewManager(Config{Now: func() time.Time { return now }}, reg)
	_, d, _, _ := m.Open(OpenRequest{WorkstationID: "K", EquipmentType: "KKT"})

	d.Exec(context.Background(), "PrintXReport", []any{"h", ""}, now)
	res, _, _ := d.Exec(context.Background(), MethodGetLastError, []any{""}, now)
	if res.Return != int64(3) || res.Params[0] != "Нет бумаги" {
		t.Fatalf("GetLastError: %+v", res)
	}
	if le := m.Devices()[0].LastError; le == nil || le.Code != 3 {
		t.Fatalf("last_error в Control API: %+v", le)
	}
	m.ResetDevices()
	res, _, _ = d.Exec(context.Background(), MethodGetLastError, nil, now)
	if res.Return != int64(0) || len(res.Params) != 1 || res.Params[0] != "" {
		t.Fatalf("после сброса GetLastError: %+v", res)
	}
}
