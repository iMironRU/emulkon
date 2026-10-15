package sessions

// Реестр устройств на диске (ARCHITECTURE §4.3, docs/protocol.md §2.3): устройства, рабочие места,
// подписи, настройки устройств и классов переживают перезапуск. Полное состояние моделей (смена,
// ФН, пакет терминала) не сохраняется — оно живёт в памяти.

import (
	"encoding/json"
	"errors"
	"fmt"
	"log/slog"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"time"

	"emulkon/internal/models"
)

const registryVersion = 1

type registryFile struct {
	Version      int                       `json:"version"`
	Devices      []deviceRecord            `json:"devices"`
	Workstations []workstationRecord       `json:"workstations"`
	TypeDefaults map[string]map[string]any `json:"type_defaults,omitempty"`
}

type deviceRecord struct {
	ID            string         `json:"id"`
	Type          string         `json:"type"`
	WorkstationID string         `json:"workstation_id"`
	InstanceID    string         `json:"instance_id,omitempty"`
	InstanceName  string         `json:"instance_name,omitempty"`
	Infobase      string         `json:"infobase,omitempty"`
	Copy          bool           `json:"copy,omitempty"`
	Alias         string         `json:"alias,omitempty"`
	Note          string         `json:"note,omitempty"`
	CreatedAt     time.Time      `json:"created_at"`
	LastSeen      *time.Time     `json:"last_seen,omitempty"`
	Settings      map[string]any `json:"settings,omitempty"`
	Output        *models.Output `json:"output,omitempty"`
}

type workstationRecord struct {
	ID        string     `json:"id"`
	Alias     string     `json:"alias,omitempty"`
	Note      string     `json:"note,omitempty"`
	FirstSeen time.Time  `json:"first_seen"`
	LastSeen  time.Time  `json:"last_seen"`
	Printers  []string   `json:"printers,omitempty"`
	Default   string     `json:"default_printer,omitempty"`
	TokenHash string     `json:"token_sha256,omitempty"` // SHA-256 токена рабочего места; сам токен не хранится
	Token     *TokenInfo `json:"token,omitempty"`
}

// Workstation — рабочее место в реестре.
type Workstation struct {
	ID        string
	Alias     string
	Note      string
	FirstSeen time.Time
	LastSeen  time.Time
	Printers  []string   // принтеры Windows — сообщает трей
	Default   string     // принтер по умолчанию
	TokenHash string     // SHA-256 токена рабочего места (tokens.go); пусто — токена нет
	Token     *TokenInfo // когда выдан и последние символы
}

// bindingKey — ключ устройства: экземпляр 1С и тип или, без экземпляра, рабочее место и тип.
func bindingKey(instanceID, workstationID, typeKey string) string {
	if instanceID != "" {
		return "i\x00" + instanceID + "\x00" + typeKey
	}
	return "w\x00" + workstationID + "\x00" + typeKey
}

// Load читает реестр из Config.StorePath. Файла нет — пустой реестр. Устройство неизвестного
// или незарегистрированного типа пропускается с предупреждением.
func (m *Manager) Load() error {
	if m.cfg.StorePath == "" {
		return nil
	}
	b, err := os.ReadFile(m.cfg.StorePath)
	if errors.Is(err, os.ErrNotExist) {
		return nil
	}
	if err != nil {
		return fmt.Errorf("прочитать реестр устройств: %w", err)
	}
	var f registryFile
	if err := json.Unmarshal(b, &f); err != nil {
		return fmt.Errorf("разобрать реестр устройств %s: %w", m.cfg.StorePath, err)
	}

	m.mu.Lock()
	defer m.mu.Unlock()
	for k, v := range f.TypeDefaults {
		m.typeDefaults[k] = v
	}
	for _, w := range f.Workstations {
		m.workstations[w.ID] = &Workstation{ID: w.ID, Alias: w.Alias, Note: w.Note, FirstSeen: w.FirstSeen, LastSeen: w.LastSeen,
			Printers: w.Printers, Default: w.Default, TokenHash: w.TokenHash, Token: w.Token}
	}
	for _, r := range f.Devices {
		t, err := m.reg.Resolve(r.Type)
		if err != nil {
			slog.Warn("реестр: устройство пропущено", "device", r.ID, "err", err)
			continue
		}
		d, err := m.newDevice(t, r.ID, r.WorkstationID, r.CreatedAt)
		if err != nil {
			slog.Warn("реестр: устройство пропущено", "device", r.ID, "err", err)
			continue
		}
		d.InstanceID, d.instanceName, d.infobase, d.Copy = r.InstanceID, r.InstanceName, r.Infobase, r.Copy
		d.alias, d.note = r.Alias, r.Note
		if r.Output != nil && d.output != nil {
			d.output = r.Output
		}
		if r.LastSeen != nil {
			d.lastSeen = *r.LastSeen
		}
		if err := d.applySettings(r.Settings); err != nil {
			slog.Warn("реестр: настройки устройства не применены", "device", r.ID, "err", err)
		}
		m.devices[d.ID] = d
		m.bindings[bindingKey(d.InstanceID, d.WorkstationID, t.Key)] = d.ID
		if n := deviceNumber(d.ID, t.Key); n > m.counters[t.Key] {
			m.counters[t.Key] = n
		}
	}
	return nil
}

// deviceNumber — номер из идентификатора kkt-07 → 7.
func deviceNumber(id, typeKey string) int {
	n, err := strconv.Atoi(strings.TrimPrefix(id, typeKey+"-"))
	if err != nil {
		return 0
	}
	return n
}

// snapshot — реестр для записи (под m.mu).
func (m *Manager) snapshot() registryFile {
	f := registryFile{Version: registryVersion, TypeDefaults: map[string]map[string]any{}}
	for k, v := range m.typeDefaults {
		f.TypeDefaults[k] = v
	}
	for _, w := range m.workstations {
		f.Workstations = append(f.Workstations, workstationRecord{ID: w.ID, Alias: w.Alias, Note: w.Note,
			FirstSeen: w.FirstSeen, LastSeen: w.LastSeen, Printers: w.Printers, Default: w.Default,
			TokenHash: w.TokenHash, Token: w.Token})
	}
	for _, d := range m.devices {
		d.mu.Lock()
		r := deviceRecord{ID: d.ID, Type: d.Type.Key, WorkstationID: d.WorkstationID,
			InstanceID: d.InstanceID, InstanceName: d.instanceName, Infobase: d.infobase, Copy: d.Copy,
			Alias: d.alias, Note: d.note, CreatedAt: d.CreatedAt, Settings: copyMap(d.settings), Output: copyOutput(d.output)}
		if !d.lastSeen.IsZero() {
			ls := d.lastSeen
			r.LastSeen = &ls
		}
		d.mu.Unlock()
		f.Devices = append(f.Devices, r)
	}
	return f
}

// persist записывает реестр сразу (изменения администратора, новое устройство). Ошибка записи
// не ломает работу эмуляции — пишется в лог.
func (m *Manager) persist() {
	if m.cfg.StorePath == "" {
		return
	}
	m.mu.Lock()
	f := m.snapshot()
	m.dirty = false
	m.mu.Unlock()
	if err := m.write(f); err != nil {
		slog.Error("запись реестра устройств", "err", err)
	}
}

// Flush записывает реестр, если с прошлой записи менялась только активность (last_seen).
// Вызывается периодически и при остановке.
func (m *Manager) Flush() {
	m.mu.Lock()
	dirty := m.dirty
	m.mu.Unlock()
	if dirty {
		m.persist()
	}
}

func (m *Manager) write(f registryFile) error {
	m.saveMu.Lock()
	defer m.saveMu.Unlock()
	b, err := json.MarshalIndent(f, "", "  ")
	if err != nil {
		return err
	}
	if err := os.MkdirAll(filepath.Dir(m.cfg.StorePath), 0o755); err != nil {
		return err
	}
	tmp := m.cfg.StorePath + ".tmp"
	if err := os.WriteFile(tmp, b, 0o644); err != nil {
		return err
	}
	return os.Rename(tmp, m.cfg.StorePath)
}

// applySettings применяет настройки к модели и запоминает их у устройства.
func (d *Device) applySettings(settings map[string]any) error {
	d.mu.Lock()
	d.settings = copyMap(settings)
	d.mu.Unlock()
	if len(settings) == 0 {
		return nil
	}
	setter, ok := d.Model.(models.StateSetter)
	if !ok {
		return nil
	}
	return setter.SetState(copyMap(settings))
}

func copyOutput(o *models.Output) *models.Output {
	if o == nil {
		return nil
	}
	c := *o
	c.Kinds = append([]string{}, o.Kinds...)
	return &c
}

// OutputFor — что сделать с документом вида kind на рабочем месте (payload device.print → output).
func (d *Device) OutputFor(kind string) models.OutputDecision {
	d.mu.Lock()
	defer d.mu.Unlock()
	return d.output.Decide(kind)
}

func copyMap(m map[string]any) map[string]any {
	if len(m) == 0 {
		return nil
	}
	out := make(map[string]any, len(m))
	for k, v := range m {
		out[k] = v
	}
	return out
}
