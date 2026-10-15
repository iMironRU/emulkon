package sessions

// Администрирование устройств, рабочих мест и классов (docs/protocol.md §2.3).

import (
	"errors"
	"fmt"
	"sort"
	"time"

	"emulkon/internal/models"
)

var ErrNotFound = errors.New("не найдено")

// Meta — подпись и заметка; nil — не менять. Output — вывод документов (только у устройства).
type Meta struct {
	Alias  *string        `json:"alias"`
	Note   *string        `json:"note"`
	Output *models.Output `json:"output"`
}

func (mt Meta) validate() error {
	for _, v := range []*string{mt.Alias, mt.Note} {
		if v != nil {
			if err := validateText(*v); err != nil {
				return fmt.Errorf("%w: %v", ErrBadRequest, err)
			}
		}
	}
	return nil
}

// SetDeviceMeta меняет подпись и заметку устройства.
func (m *Manager) SetDeviceMeta(id string, mt Meta) error {
	if err := mt.validate(); err != nil {
		return err
	}
	d, ok := m.Device(id)
	if !ok {
		return ErrNotFound
	}
	if mt.Output != nil {
		if msg, ok := mt.Output.Normalize(d.Type.Key); !ok {
			return fmt.Errorf("%w: output: %s", ErrBadRequest, msg)
		}
	}
	d.mu.Lock()
	if mt.Output != nil {
		d.output = copyOutput(mt.Output)
	}
	if mt.Alias != nil {
		d.alias = *mt.Alias
	}
	if mt.Note != nil {
		d.note = *mt.Note
	}
	d.mu.Unlock()
	m.persist()
	return nil
}

// DeleteDevice удаляет устройство и его сессии. Компонента при следующей команде получит
// 401 session_unknown, откроет сессию заново и попадёт на новое устройство.
func (m *Manager) DeleteDevice(id string) error {
	m.mu.Lock()
	d, ok := m.devices[id]
	if !ok {
		m.mu.Unlock()
		return ErrNotFound
	}
	delete(m.devices, id)
	for k, v := range m.bindings {
		if v == id {
			delete(m.bindings, k)
		}
	}
	for sid, s := range m.sessions {
		if s.DeviceID == id {
			delete(m.sessions, sid)
		}
	}
	m.mu.Unlock()
	if p, ok := d.Model.(models.Prompter); ok {
		p.CancelPrompt() // команда, ждущая человека, не должна висеть до таймаута
	}
	m.persist()
	return nil
}

// SaveSettings запоминает настройки из изменения состояния (уже принятого моделью).
func (m *Manager) SaveSettings(d *Device, patch map[string]any) {
	s := models.SplitSettings(d.Type.Key, patch)
	if len(s) == 0 {
		return
	}
	d.mu.Lock()
	if d.settings == nil {
		d.settings = map[string]any{}
	}
	for k, v := range s {
		d.settings[k] = v
	}
	d.mu.Unlock()
	m.persist()
}

// ResetDeviceSettings возвращает устройству настройки его класса: модель — в исходное
// состояние, затем настройки класса по умолчанию.
func (m *Manager) ResetDeviceSettings(id string) error {
	d, ok := m.Device(id)
	if !ok {
		return ErrNotFound
	}
	m.mu.Lock()
	defaults := copyMap(m.typeDefaults[d.Type.Key])
	m.mu.Unlock()
	if p, ok := d.Model.(models.Prompter); ok {
		p.CancelPrompt()
	}
	d.sem <- struct{}{}
	d.mu.Lock()
	d.settings = defaults
	d.mu.Unlock()
	d.resetModel()
	<-d.sem
	m.persist()
	return nil
}

// WorkstationInfo — рабочее место для Control API.
type WorkstationInfo struct {
	WorkstationID string     `json:"workstation_id"`
	Alias         string     `json:"alias,omitempty"`
	Note          string     `json:"note,omitempty"`
	FirstSeen     time.Time  `json:"first_seen"`
	LastSeen      time.Time  `json:"last_seen"`
	Infobases     []string   `json:"infobases"`
	Devices       []string   `json:"devices"`
	Sessions      int        `json:"sessions"`
	Printers      []string   `json:"printers"` // принтеры Windows — сообщает трей
	Default       string     `json:"default_printer,omitempty"`
	Token         *TokenInfo `json:"token,omitempty"` // выдан токен рабочего места (сам токен не показывается)
}

// Workstations — рабочие места: где появлялись устройства или открывались сессии.
func (m *Manager) Workstations() []WorkstationInfo {
	m.mu.Lock()
	defer m.mu.Unlock()
	byID := map[string]*WorkstationInfo{}
	get := func(id string) *WorkstationInfo {
		if w, ok := byID[id]; ok {
			return w
		}
		w := &WorkstationInfo{WorkstationID: id, Infobases: []string{}, Devices: []string{}, Printers: []string{}}
		if rec, ok := m.workstations[id]; ok {
			w.Alias, w.Note, w.FirstSeen, w.LastSeen = rec.Alias, rec.Note, rec.FirstSeen, rec.LastSeen
			w.Printers, w.Default = append(w.Printers, rec.Printers...), rec.Default
			if rec.Token != nil {
				t := *rec.Token
				w.Token = &t
			}
		}
		byID[id] = w
		return w
	}
	for id := range m.workstations {
		get(id)
	}
	// Устройство относится к рабочему месту своих сессий, без сессий — к месту, где появилось.
	devWS := map[string]map[string]bool{}
	for _, s := range m.sessions {
		get(s.WorkstationID).Sessions++
		if devWS[s.DeviceID] == nil {
			devWS[s.DeviceID] = map[string]bool{}
		}
		devWS[s.DeviceID][s.WorkstationID] = true
	}
	for _, d := range m.devices {
		places := devWS[d.ID]
		if len(places) == 0 {
			places = map[string]bool{d.WorkstationID: true}
		}
		d.mu.Lock()
		ib := d.infobase
		d.mu.Unlock()
		for ws := range places {
			w := get(ws)
			w.Devices = append(w.Devices, d.ID)
			if ib != "" && !contains(w.Infobases, ib) {
				w.Infobases = append(w.Infobases, ib)
			}
		}
	}
	out := make([]WorkstationInfo, 0, len(byID))
	for _, w := range byID {
		sort.Strings(w.Devices)
		sort.Strings(w.Infobases)
		out = append(out, *w)
	}
	sort.Slice(out, func(a, b int) bool { return out[a].WorkstationID < out[b].WorkstationID })
	return out
}

func contains(list []string, s string) bool {
	for _, v := range list {
		if v == s {
			return true
		}
	}
	return false
}

// SetWorkstationMeta меняет подпись и заметку рабочего места. Рабочее место можно завести
// заранее — до первого подключения.
func (m *Manager) SetWorkstationMeta(id string, mt Meta) error {
	if err := validateID(id); err != nil {
		return fmt.Errorf("%w: workstation_id: %v", ErrBadRequest, err)
	}
	if err := mt.validate(); err != nil {
		return err
	}
	m.mu.Lock()
	w, ok := m.workstations[id]
	if !ok {
		w = &Workstation{ID: id}
		m.workstations[id] = w
	}
	if mt.Alias != nil {
		w.Alias = *mt.Alias
	}
	if mt.Note != nil {
		w.Note = *mt.Note
	}
	m.mu.Unlock()
	m.persist()
	return nil
}

// SetWorkstationPrinters — принтеры Windows рабочего места (их сообщает трей, docs/protocol.md §2.3).
func (m *Manager) SetWorkstationPrinters(id string, printers []string, def string) error {
	if err := validateID(id); err != nil {
		return fmt.Errorf("%w: workstation_id: %v", ErrBadRequest, err)
	}
	if len(printers) > 200 {
		printers = printers[:200]
	}
	for _, p := range append([]string{def}, printers...) {
		if err := validateText(p); err != nil {
			return fmt.Errorf("%w: принтер: %v", ErrBadRequest, err)
		}
	}
	m.mu.Lock()
	w, ok := m.workstations[id]
	if !ok {
		w = &Workstation{ID: id}
		m.workstations[id] = w
	}
	changed := w.Default != def || len(w.Printers) != len(printers)
	for i := 0; !changed && i < len(printers); i++ {
		changed = w.Printers[i] != printers[i]
	}
	w.Printers, w.Default = append([]string{}, printers...), def
	m.mu.Unlock()
	if changed {
		m.persist()
	}
	return nil
}

// TypeInfo — класс устройств для Control API.
type TypeInfo struct {
	models.EquipmentType
	OutputKinds []models.OutputKind `json:"output_kinds"` // что можно печатать; пусто — класс не печатает
	Schema      []models.Field      `json:"schema"`
	Defaults    map[string]any      `json:"defaults"`
	Devices     int                 `json:"devices"`
}

// Types — классы: схема ключей состояния, настройки по умолчанию, сколько устройств.
func (m *Manager) Types() []TypeInfo {
	m.mu.Lock()
	defer m.mu.Unlock()
	count := map[string]int{}
	for _, d := range m.devices {
		count[d.Type.Key]++
	}
	var out []TypeInfo
	for _, t := range m.reg.Types() {
		schema := models.Schema(t.Key)
		if schema == nil {
			schema = []models.Field{}
		}
		defaults := copyMap(m.typeDefaults[t.Key])
		if defaults == nil {
			defaults = map[string]any{}
		}
		kinds := models.OutputKinds(t.Key)
		if kinds == nil {
			kinds = []models.OutputKind{}
		}
		out = append(out, TypeInfo{EquipmentType: t, OutputKinds: kinds, Schema: schema, Defaults: defaults, Devices: count[t.Key]})
	}
	return out
}

// SetTypeDefaults заменяет настройки класса по умолчанию. Только ключи-настройки схемы;
// значения проверяет сама модель на пробном экземпляре. Существующие устройства не меняются.
func (m *Manager) SetTypeDefaults(key string, defaults map[string]any) error {
	t, err := m.reg.Resolve(key)
	if err != nil {
		return ErrNotFound
	}
	settings := models.SplitSettings(t.Key, defaults)
	for k := range defaults {
		if _, ok := settings[k]; !ok {
			return fmt.Errorf("%w: %s — не настройка класса %s (docs/protocol.md §2.3)", ErrBadRequest, k, t.Key)
		}
	}
	if len(settings) > 0 {
		probe, err := m.reg.New(t, t.Key+"-probe")
		if err != nil {
			return err
		}
		if setter, ok := probe.(models.StateSetter); ok {
			if err := setter.SetState(copyMap(settings)); err != nil {
				return fmt.Errorf("%w: %v", ErrBadRequest, err)
			}
		}
	}
	m.mu.Lock()
	if len(settings) == 0 {
		delete(m.typeDefaults, t.Key)
	} else {
		m.typeDefaults[t.Key] = settings
	}
	m.mu.Unlock()
	m.persist()
	return nil
}
