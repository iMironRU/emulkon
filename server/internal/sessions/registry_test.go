package sessions

import (
	"errors"
	"os"
	"path/filepath"
	"testing"
	"time"

	"emulkon/internal/models"
	"emulkon/internal/models/scales"
)

// registryManager — диспетчер с настоящей моделью весов (у заглушек нет SetState) и реестром на диске.
func registryManager(t *testing.T, path string, now *time.Time) *Manager {
	t.Helper()
	reg := models.DefaultRegistry("test")
	reg.Register("scales", scales.Factory("test"))
	m := NewManager(Config{IdleTTL: time.Hour, Now: func() time.Time { return *now }, StorePath: path}, reg)
	if err := m.Load(); err != nil {
		t.Fatal(err)
	}
	return m
}

func TestOpenByInstance(t *testing.T) {
	now := time.Date(2026, 10, 9, 12, 0, 0, 0, time.UTC)
	m := newManager("", &now)

	// Две ККТ одной базы на одном рабочем месте — два экземпляра в 1С, два устройства.
	_, a, _, _ := m.Open(OpenRequest{WorkstationID: "PC", EquipmentType: "KKT", InstanceID: "A", Infobase: "Розница", InstanceName: "Касса 1"})
	_, b, _, _ := m.Open(OpenRequest{WorkstationID: "PC", EquipmentType: "KKT", InstanceID: "B"})
	_, c, _, _ := m.Open(OpenRequest{WorkstationID: "PC", EquipmentType: "KKT"})
	if a.ID == b.ID || a.ID == c.ID || b.ID == c.ID {
		t.Fatalf("устройства должны различаться: %s %s %s", a.ID, b.ID, c.ID)
	}
	// Экземпляр открыли на другом компьютере — то же устройство.
	_, a2, _, _ := m.Open(OpenRequest{WorkstationID: "PC-2", EquipmentType: "ККТ", InstanceID: "A"})
	if a2 != a {
		t.Fatalf("экземпляр A на другом месте: %s, ждали %s", a2.ID, a.ID)
	}
	info, _ := m.DeviceInfo(a.ID)
	if info.KeyedBy != "instance" || info.Infobase != "Розница" || info.InstanceName != "Касса 1" || !info.Online {
		t.Fatalf("сведения об экземпляре: %+v", info)
	}
	if ci, _ := m.DeviceInfo(c.ID); ci.KeyedBy != "workstation" {
		t.Fatalf("без экземпляра — по рабочему месту: %+v", ci)
	}
	_, cp, _, _ := m.Open(OpenRequest{WorkstationID: "PC", EquipmentType: "KKT", InstanceID: "A#2", InstanceCopy: true})
	if ci, _ := m.DeviceInfo(cp.ID); !ci.Copy || cp == a {
		t.Fatalf("копия экземпляра: %+v", ci)
	}
	if _, _, _, err := m.Open(OpenRequest{WorkstationID: "PC", EquipmentType: "KKT", Infobase: "a\x00b"}); !errors.Is(err, ErrBadRequest) {
		t.Fatalf("управляющие символы в базе: %v", err)
	}
	now = now.Add(time.Hour)
	if info, _ := m.DeviceInfo(a.ID); info.Online {
		t.Fatal("сессия без активности час — не на связи")
	}
}

func TestRegistryPersists(t *testing.T) {
	now := time.Date(2026, 10, 9, 12, 0, 0, 0, time.UTC)
	path := filepath.Join(t.TempDir(), "registry.json")
	m := registryManager(t, path, &now)

	// Класс весов по умолчанию: предел 6 кг. Ключ-состояние в настройки класса не берётся.
	if err := m.SetTypeDefaults("scales", map[string]any{"weight": 1.0}); !errors.Is(err, ErrBadRequest) {
		t.Fatalf("weight — не настройка: %v", err)
	}
	if err := m.SetTypeDefaults("scales", map[string]any{"max_weight": -1.0}); !errors.Is(err, ErrBadRequest) {
		t.Fatalf("неверное значение проверяет модель: %v", err)
	}
	if err := m.SetTypeDefaults("scales", map[string]any{"max_weight": 6.0}); err != nil {
		t.Fatal(err)
	}
	_, d, _, _ := m.Open(OpenRequest{WorkstationID: "PC", EquipmentType: "WeighingScales", InstanceID: "S1", Infobase: "Розница"})
	if d.Model.State()["max_weight"] != 6.0 {
		t.Fatalf("новое устройство получает настройки класса: %v", d.Model.State()["max_weight"])
	}
	// Изменение из Control API: настройка сохраняется, состояние — нет.
	patch := map[string]any{"weight_mode": "ask", "weight": 0.5}
	if err := d.Model.(models.StateSetter).SetState(map[string]any{"weight_mode": "ask", "weight": 0.5}); err != nil {
		t.Fatal(err)
	}
	m.SaveSettings(d, patch)
	alias := "Весы в отделе 2"
	if err := m.SetDeviceMeta(d.ID, Meta{Alias: &alias}); err != nil {
		t.Fatal(err)
	}
	wsAlias := "Касса на Ленина"
	if err := m.SetWorkstationMeta("PC", Meta{Alias: &wsAlias}); err != nil {
		t.Fatal(err)
	}
	m.Flush()

	// Перезапуск сервера.
	m2 := registryManager(t, path, &now)
	info, ok := m2.DeviceInfo(d.ID)
	if !ok || info.Alias != alias || info.Infobase != "Розница" || info.Settings["weight_mode"] != "ask" {
		t.Fatalf("после перезапуска: %+v", info)
	}
	if _, has := info.Settings["weight"]; has || info.State["weight"] != 0.0 || info.State["weight_mode"] != "ask" || info.State["max_weight"] != 6.0 {
		t.Fatalf("настройки применены, состояние — исходное: settings=%v state=%v", info.Settings, info.State)
	}
	_, same, _, _ := m2.Open(OpenRequest{WorkstationID: "PC", EquipmentType: "scales", InstanceID: "S1"})
	_, next, _, _ := m2.Open(OpenRequest{WorkstationID: "PC", EquipmentType: "scales", InstanceID: "S2"})
	if same.ID != d.ID || next.ID != "scales-02" {
		t.Fatalf("привязка и нумерация после перезапуска: %s, %s", same.ID, next.ID)
	}
	ws := m2.Workstations()
	if len(ws) != 1 || ws[0].Alias != wsAlias || len(ws[0].Devices) != 2 || ws[0].Infobases[0] != "Розница" {
		t.Fatalf("рабочие места: %+v", ws)
	}

	// Сброс возвращает модель к её настройкам; сброс настроек — к настройкам класса.
	m2.ResetDevices()
	if st := same.Model.State(); st["weight_mode"] != "ask" {
		t.Fatalf("после сброса настройки остаются: %v", st["weight_mode"])
	}
	if err := m2.ResetDeviceSettings(same.ID); err != nil {
		t.Fatal(err)
	}
	if st := same.Model.State(); st["weight_mode"] != "manual" || st["max_weight"] != 6.0 {
		t.Fatalf("настройки класса: %v %v", st["weight_mode"], st["max_weight"])
	}

	// Удаляет только человек; сессии устройства закрываются.
	if err := m2.DeleteDevice(next.ID); err != nil {
		t.Fatal(err)
	}
	if _, ok := m2.Device(next.ID); ok {
		t.Fatal("устройство удалено")
	}
	if err := m2.DeleteDevice(next.ID); !errors.Is(err, ErrNotFound) {
		t.Fatalf("повторное удаление: %v", err)
	}
	if _, err := os.Stat(path); err != nil {
		t.Fatalf("реестр на диске: %v", err)
	}
}

func TestRegistryBrokenFile(t *testing.T) {
	path := filepath.Join(t.TempDir(), "registry.json")
	if err := os.WriteFile(path, []byte("{не json"), 0o644); err != nil {
		t.Fatal(err)
	}
	now := time.Now()
	m := NewManager(Config{StorePath: path, Now: func() time.Time { return now }}, models.DefaultRegistry("test"))
	if err := m.Load(); err == nil {
		t.Fatal("битый реестр — ошибка запуска, а не молчаливая потеря устройств")
	}
}

func TestOutputSettings(t *testing.T) {
	now := time.Date(2026, 10, 9, 12, 0, 0, 0, time.UTC)
	path := filepath.Join(t.TempDir(), "registry.json")
	m := registryManager(t, path, &now)
	_, kkt, _, _ := m.Open(OpenRequest{WorkstationID: "PC", EquipmentType: "KKT", InstanceID: "K"})
	_, scl, _, _ := m.Open(OpenRequest{WorkstationID: "PC", EquipmentType: "scales", InstanceID: "S"})

	// По умолчанию: на экран — да, на принтер — нет.
	if d := kkt.OutputFor("check"); !d.Screen || d.Print {
		t.Fatalf("вывод по умолчанию: %+v", d)
	}
	if info, _ := m.DeviceInfo(scl.ID); info.Output != nil {
		t.Fatalf("весы ничего не печатают — вывода нет: %+v", info.Output)
	}
	bad := models.Output{Printer: true, Kinds: []string{"labels"}}
	if err := m.SetDeviceMeta(kkt.ID, Meta{Output: &bad}); !errors.Is(err, ErrBadRequest) {
		t.Fatalf("этикеток у ККТ нет: %v", err)
	}
	out := models.Output{Screen: false, Printer: true, PrinterName: "Microsoft Print to PDF", Kinds: []string{"checks", "reports", "checks"}}
	if err := m.SetDeviceMeta(kkt.ID, Meta{Output: &out}); err != nil {
		t.Fatal(err)
	}
	if d := kkt.OutputFor("close_shift"); d.Screen || !d.Print || d.Printer != "Microsoft Print to PDF" {
		t.Fatalf("отчёт — на принтер: %+v", d)
	}
	if d := kkt.OutputFor("copy"); d.Print {
		t.Fatalf("копии не выбраны: %+v", d)
	}
	if err := m.SetWorkstationPrinters("PC", []string{"HP", "Microsoft Print to PDF"}, "HP"); err != nil {
		t.Fatal(err)
	}

	m2 := registryManager(t, path, &now)
	info, _ := m2.DeviceInfo(kkt.ID)
	if info.Output == nil || info.Output.Screen || len(info.Output.Kinds) != 2 {
		t.Fatalf("вывод после перезапуска: %+v", info.Output)
	}
	if ws := m2.Workstations(); len(ws) != 1 || ws[0].Default != "HP" || len(ws[0].Printers) != 2 {
		t.Fatalf("принтеры рабочего места: %+v", ws)
	}
}
