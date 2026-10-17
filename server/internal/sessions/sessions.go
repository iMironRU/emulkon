// Package sessions — диспетчер сессий (ARCHITECTURE §4.2): привязка
// экземпляр 1С / рабочее место → виртуальное устройство, проверка токенов, учёт активности.
//
// Устройства создаются автоматически при первом подключении — одно на пару (экземпляр 1С,
// тип), без экземпляра — на пару (рабочее место, тип); идентификатор вида kkt-01
// (ARCHITECTURE §4.3). Реестр устройств сохраняется на диск (registry.go).
package sessions

import (
	"context"
	"crypto/rand"
	"encoding/hex"
	"errors"
	"fmt"
	"log/slog"
	"sort"
	"sync"
	"time"
	"unicode"

	"emulkon/internal/models"
	"emulkon/internal/secret"
)

var (
	ErrBadToken       = errors.New("неверный токен устройства")
	ErrBadRequest     = errors.New("некорректный запрос")
	ErrUnknownType    = errors.New("неизвестный тип оборудования")
	ErrUnknownSession = errors.New("сессия не найдена")
	ErrWrongDevice    = errors.New("сессия открыта для другого устройства")
)

// MethodGetLastError — «ПолучитьОшибку (GetLastError)», раздел 2 требований:
// возвращает код (LONG) и описание (параметр 0, STRING [OUT]) последней ошибки.
// Обслуживается ядром одинаково для всех типов оборудования.
const MethodGetLastError = "GetLastError"

// Config — параметры диспетчера.
type Config struct {
	// DeviceToken — общий токен устройств. Не задан — открытый режим (любой токен).
	DeviceToken secret.Token
	// IdleTTL — сессия без активности дольше этого удаляется. 0 — 24 часа.
	IdleTTL time.Duration
	Now     func() time.Time
	// OnPrompt — экран устройства ждёт человека (done=nil) или дождался (models.Prompter):
	// app пишет это в журнал (device.prompt, device.prompt_done).
	OnPrompt func(d *Device, p models.Prompt, done *models.PromptResult)
	// OnEvent — модель отдала событие для 1С (models.Emitter): app пишет его в журнал.
	OnEvent func(d *Device, e EventRecord, listeners int)
	// StorePath — файл реестра устройств (registry.json); пусто — ничего не сохраняется.
	StorePath string
}

// onlineWindow — устройство «на связи», если его сессия активна не дольше этого (protocol.md §2.3).
const onlineWindow = 5 * time.Minute

// OpenRequest — тело POST /api/v1/sessions (docs/protocol.md §1.1).
type OpenRequest struct {
	WorkstationID string `json:"workstation_id"`
	// WorkstationAuto — workstation_id не задан в параметрах драйвера, это имя компьютера:
	// токен рабочего места тогда сам называет место (tokens.go).
	WorkstationAuto bool   `json:"workstation_auto,omitempty"`
	DeviceToken     string `json:"device_token"`
	// EquipmentType — значение из таблицы «Тип оборудования» (KKT, POSTerminal, …
	// или русское написание), как его передала БПО в ConnectEquipment.
	EquipmentType string `json:"equipment_type"`
	DriverVersion string `json:"driver_version"`
	// DriverArch — архитектура DLL драйвера (x86_64, i386, aarch64): по ней сервер предлагает обновление.
	DriverArch string `json:"driver_arch,omitempty"`
	// DriverOS — ОС драйвера (windows, linux); пусто — windows (драйверы до 0.9.0).
	DriverOS string `json:"driver_os,omitempty"`
	// InterfaceRevision — ревизия требований, которую реализует компонента (4008 = «4.8»).
	InterfaceRevision int `json:"interface_revision"`
	// Экземпляр «Подключаемого оборудования» в 1С (ARCHITECTURE §4.3): параметры драйвера
	// InstanceID, InstanceName, Infobase. InstanceCopy — тот же InstanceID уже подключён в этом
	// процессе 1С другим экземпляром (скопировали с параметрами).
	InstanceID   string `json:"instance_id,omitempty"`
	InstanceName string `json:"instance_name,omitempty"`
	Infobase     string `json:"infobase,omitempty"`
	InstanceCopy bool   `json:"instance_copy,omitempty"`
}

// Session — подключение компоненты.
type Session struct {
	ID                string
	DeviceID          string
	WorkstationID     string
	Type              models.EquipmentType
	DriverVersion     string
	InterfaceRevision int
	CreatedAt         time.Time
	Auth              string // как допущена: AuthWorkstation, AuthServer, AuthOpen

	mu       sync.Mutex
	lastSeen time.Time
}

func (s *Session) touch(now time.Time) {
	s.mu.Lock()
	s.lastSeen = now
	s.mu.Unlock()
}

func (s *Session) LastSeen() time.Time {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.lastSeen
}

// Device — виртуальное устройство с моделью.
type Device struct {
	ID            string
	Type          models.EquipmentType
	WorkstationID string // рабочее место, где устройство появилось
	CreatedAt     time.Time
	Model         models.Model
	// InstanceID — экземпляр «Подключаемого оборудования» в 1С, часть ключа устройства;
	// пусто — устройство по рабочему месту («не настроено»).
	InstanceID string
	Copy       bool // экземпляр подключился копией уже подключённого (тот же InstanceID)

	sem chan struct{} // одна команда за раз

	mu           sync.Mutex
	instanceName string         // наименование экземпляра в 1С — с последнего подключения
	infobase     string         // база 1С — с последнего подключения
	alias, note  string         // подпись и заметка администратора
	output       *models.Output // вывод документов на рабочем месте; nil — устройство не печатает
	settings     map[string]any // сохранённые настройки (models.Field.Setting)
	lastSeen     time.Time
	commands     int64
	lastErrCode  int
	lastErrText  string
	printSeq     int64 // последний документ ленты, уже отданный из Exec

	events eventBuffer // события для компоненты (events.go)
}

// Exec выполняет метод. Команды одного устройства выполняются строго по очереди,
// как на настоящем железе; ожидание очереди прерывается ctx. Начатая команда
// доводится до конца, даже если клиент отключился: её результат нужен для
// идемпотентного повтора с тем же request_id (docs/protocol.md §1.2).
// printed — документы, которые устройство «напечатало» за время команды.
func (d *Device) Exec(ctx context.Context, method string, params []any, now time.Time) (res models.Result, printed []models.PrintedDocument, err error) {
	select {
	case d.sem <- struct{}{}:
	case <-ctx.Done():
		return models.Result{}, nil, ctx.Err()
	}
	defer func() { <-d.sem }()
	defer func() { printed = d.newPrints() }()

	if method == MethodGetLastError {
		res = d.lastError(params)
	} else {
		res = d.Model.Handle(context.WithoutCancel(ctx), method, params)
		if res.Params != nil && len(res.Params) != len(params) {
			slog.Error("модель вернула параметры другой длины — отдаём исходные",
				"device", d.ID, "method", method, "got", len(res.Params), "want", len(params))
			res.Params = nil
		}
	}
	if res.Params == nil {
		res.Params = params
	}

	d.mu.Lock()
	d.commands++
	d.lastSeen = now
	if method != MethodGetLastError {
		// Последняя ошибка — от последнего неуспешного вызова; успех её сбрасывает.
		if ok, isBool := res.Return.(bool); isBool && !ok {
			d.lastErrCode, d.lastErrText = res.ErrorCode, res.ErrorText
		} else {
			d.lastErrCode, d.lastErrText = 0, ""
		}
	}
	d.mu.Unlock()
	return res, nil, nil
}

// InjectError — ответ сценария вместо модели (ARCHITECTURE §7): команда «выполнена» с ошибкой,
// её же вернёт GetLastError. Идёт в очереди команд устройства, модель не вызывается.
func (d *Device) InjectError(ctx context.Context, params []any, code int, text string, now time.Time) (models.Result, error) {
	select {
	case d.sem <- struct{}{}:
	case <-ctx.Done():
		return models.Result{}, ctx.Err()
	}
	defer func() { <-d.sem }()
	d.mu.Lock()
	d.commands++
	d.lastSeen = now
	d.lastErrCode, d.lastErrText = code, text
	d.mu.Unlock()
	return models.Result{Return: false, Params: params, ErrorCode: code, ErrorText: text}, nil
}

// Act — действие человека на устройстве (models.Actor): напечатать этикетку на весах.
// Идёт в общей очереди команд устройства. printed — что устройство при этом напечатало.
func (d *Device) Act(ctx context.Context, name string, args map[string]any) (res any, printed []models.PrintedDocument, err error) {
	actor, ok := d.Model.(models.Actor)
	if !ok {
		return nil, nil, models.ErrUnknownAction
	}
	select {
	case d.sem <- struct{}{}:
	case <-ctx.Done():
		return nil, nil, ctx.Err()
	}
	defer func() { <-d.sem }()
	defer func() { printed = d.newPrints() }()
	res, err = actor.Act(name, args)
	return res, nil, err
}

// newPrints — документы ленты новее последнего отданного (вызывается под d.sem).
func (d *Device) newPrints() []models.PrintedDocument {
	p, ok := d.Model.(models.Printer)
	if !ok {
		return nil
	}
	docs := p.PrintsSince(d.printSeq)
	if n := len(docs); n > 0 {
		d.printSeq = docs[n-1].Seq
	}
	return docs
}

func (d *Device) lastError(params []any) models.Result {
	d.mu.Lock()
	code, text := d.lastErrCode, d.lastErrText
	d.mu.Unlock()
	out := models.CopyParams(params)
	if len(out) == 0 {
		out = []any{""}
	}
	out[0] = text
	return models.Result{Return: int64(code), Params: out, ErrorCode: code, ErrorText: text}
}

// Manager — диспетчер сессий и устройств.
type Manager struct {
	cfg Config
	reg *models.Registry

	mu           sync.Mutex
	sessions     map[string]*Session
	devices      map[string]*Device
	bindings     map[string]string // bindingKey → deviceID
	counters     map[string]int    // последний номер устройства по типу
	workstations map[string]*Workstation
	typeDefaults map[string]map[string]any // настройки классов по умолчанию
	seenSince    map[string]bool           // рабочие места, подключавшиеся после старта сервера
	dirty        bool                      // активность изменилась после записи реестра

	saveMu sync.Mutex // одна запись реестра за раз
}

func NewManager(cfg Config, reg *models.Registry) *Manager {
	if cfg.IdleTTL <= 0 {
		cfg.IdleTTL = 24 * time.Hour
	}
	if cfg.Now == nil {
		cfg.Now = time.Now
	}
	return &Manager{
		cfg: cfg, reg: reg,
		sessions:     map[string]*Session{},
		devices:      map[string]*Device{},
		bindings:     map[string]string{},
		counters:     map[string]int{},
		workstations: map[string]*Workstation{},
		typeDefaults: map[string]map[string]any{},
		seenSince:    map[string]bool{},
	}
}

// newDevice создаёт устройство с моделью и подключает хуки экрана и событий (под m.mu).
func (m *Manager) newDevice(t models.EquipmentType, id, workstation string, created time.Time) (*Device, error) {
	model, err := m.reg.New(t, id)
	if err != nil {
		return nil, err
	}
	d := &Device{
		ID: id, Type: t, WorkstationID: workstation,
		CreatedAt: created, Model: model, sem: make(chan struct{}, 1),
		output: models.DefaultOutput(t.Key),
	}
	if p, ok := model.(models.Prompter); ok && m.cfg.OnPrompt != nil {
		dev, hook := d, m.cfg.OnPrompt
		p.SetPromptHook(func(pr models.Prompt, done *models.PromptResult) { hook(dev, pr, done) })
	}
	if em, ok := model.(models.Emitter); ok {
		dev, now := d, m.cfg.Now
		em.SetEventHook(func(e models.DeviceEvent) { m.emit(dev, e, now()) })
	}
	return d, nil
}

// OpenMode — токен устройства не задан, принимается любой.
func (m *Manager) OpenMode() bool { return m.cfg.DeviceToken.Empty() }

// Open открывает сессию. newWorkstation — рабочее место подключилось впервые
// с момента старта сервера (фиксируется в журнале, ARCHITECTURE §10).
func (m *Manager) Open(req OpenRequest) (s *Session, d *Device, newWorkstation bool, err error) {
	t, err := m.reg.Resolve(req.EquipmentType)
	if err != nil {
		return nil, nil, false, fmt.Errorf("%w: %v", ErrUnknownType, err)
	}

	for name, v := range map[string]string{"instance_id": req.InstanceID, "instance_name": req.InstanceName, "infobase": req.Infobase} {
		if v != "" && validateText(v) != nil {
			return nil, nil, false, fmt.Errorf("%w: %s: %v", ErrBadRequest, name, validateText(v))
		}
	}

	now := m.cfg.Now().UTC()
	created := false
	defer func() {
		if created {
			m.persist() // новое устройство или рабочее место — сразу на диск
		}
	}()
	m.mu.Lock()
	defer m.mu.Unlock()

	ws, auth, err := m.authorize(req)
	if err != nil {
		return nil, nil, false, err
	}
	req.WorkstationID = ws

	if !m.seenSince[req.WorkstationID] {
		m.seenSince[req.WorkstationID] = true
		newWorkstation = true
	}
	if w, ok := m.workstations[req.WorkstationID]; ok {
		w.LastSeen = now
	} else {
		m.workstations[req.WorkstationID] = &Workstation{ID: req.WorkstationID, FirstSeen: now, LastSeen: now}
		created = true
	}

	key := bindingKey(req.InstanceID, req.WorkstationID, t.Key)
	if id, ok := m.bindings[key]; ok {
		d = m.devices[id]
	} else {
		m.counters[t.Key]++
		id := fmt.Sprintf("%s-%02d", t.Key, m.counters[t.Key])
		if d, err = m.newDevice(t, id, req.WorkstationID, now); err != nil {
			return nil, nil, false, fmt.Errorf("%w: %v", ErrUnknownType, err)
		}
		d.InstanceID, d.Copy = req.InstanceID, req.InstanceCopy
		// Новое устройство получает настройки своего класса (ARCHITECTURE §4.3).
		if err := d.applySettings(m.typeDefaults[t.Key]); err != nil {
			slog.Warn("настройки класса не применены к новому устройству", "device", id, "err", err)
		}
		m.devices[id] = d
		m.bindings[key] = id
		created = true
	}
	d.mu.Lock()
	if req.InstanceName != "" {
		d.instanceName = req.InstanceName
	}
	if req.Infobase != "" {
		d.infobase = req.Infobase
	}
	d.lastSeen = now
	d.mu.Unlock()
	m.dirty = true

	s = &Session{
		ID: "s_" + randomHex(16), DeviceID: d.ID, WorkstationID: req.WorkstationID,
		Type: t, DriverVersion: req.DriverVersion, InterfaceRevision: req.InterfaceRevision,
		CreatedAt: now, lastSeen: now, Auth: auth,
	}
	m.sessions[s.ID] = s
	return s, d, newWorkstation, nil
}

// Resolve находит сессию и её устройство для команды к deviceID и отмечает активность.
func (m *Manager) Resolve(sessionID, deviceID string) (*Session, *Device, error) {
	now := m.cfg.Now().UTC()
	m.mu.Lock()
	s, ok := m.sessions[sessionID]
	var d *Device
	if ok {
		d = m.devices[s.DeviceID]
		if w := m.workstations[s.WorkstationID]; w != nil {
			w.LastSeen = now
		}
		m.dirty = true // активность попадёт в реестр при Flush
	}
	m.mu.Unlock()
	if !ok || d == nil {
		return nil, nil, ErrUnknownSession
	}
	if s.DeviceID != deviceID {
		return nil, nil, ErrWrongDevice
	}
	s.touch(now)
	return s, d, nil
}

// Emit — событие устройства для компоненты (от модели или из Control API) с записью в журнал.
func (m *Manager) Emit(d *Device, e models.DeviceEvent) EventRecord {
	return m.emit(d, e, m.cfg.Now())
}

func (m *Manager) emit(d *Device, e models.DeviceEvent, now time.Time) EventRecord {
	rec, listeners := d.Emit(e, now.UTC())
	if m.cfg.OnEvent != nil {
		m.cfg.OnEvent(d, rec, listeners)
	}
	return rec
}

// Device — устройство по идентификатору.
func (m *Manager) Device(id string) (*Device, bool) {
	m.mu.Lock()
	defer m.mu.Unlock()
	d, ok := m.devices[id]
	return d, ok
}

// Session — открытая сессия по идентификатору.
func (m *Manager) Session(id string) (*Session, bool) {
	m.mu.Lock()
	defer m.mu.Unlock()
	s, ok := m.sessions[id]
	return s, ok
}

// CloseSession закрывает сессию: компонента отключила устройство (DisconnectEquipment) или
// закончила пробное подключение (DELETE сессии). false — сессии уже нет.
func (m *Manager) CloseSession(id string) bool {
	m.mu.Lock()
	defer m.mu.Unlock()
	if _, ok := m.sessions[id]; !ok {
		return false
	}
	delete(m.sessions, id)
	return true
}

// ExpireIdle удаляет сессии без активности дольше IdleTTL. Возвращает удалённые.
func (m *Manager) ExpireIdle() []*Session {
	cutoff := m.cfg.Now().UTC().Add(-m.cfg.IdleTTL)
	m.mu.Lock()
	defer m.mu.Unlock()
	var gone []*Session
	for id, s := range m.sessions {
		if s.LastSeen().Before(cutoff) {
			delete(m.sessions, id)
			gone = append(gone, s)
		}
	}
	return gone
}

// ResetDevices сбрасывает состояние всех моделей и последние ошибки (перед тестом,
// ARCHITECTURE §9.1). Устройства и сессии сохраняются — компоненты не переподключаются.
func (m *Manager) ResetDevices() int {
	m.mu.Lock()
	devs := make([]*Device, 0, len(m.devices))
	for _, d := range m.devices {
		devs = append(devs, d)
	}
	m.mu.Unlock()
	for _, d := range devs {
		// Команда может ждать человека на экране устройства — снимаем запрос, иначе
		// сброс ждал бы её до таймаута.
		if p, ok := d.Model.(models.Prompter); ok {
			p.CancelPrompt()
		}
		d.sem <- struct{}{} // не сбрасываем посреди команды
		d.resetModel()
		<-d.sem
	}
	return len(devs)
}

// resetModel — исходное состояние модели плюс сохранённые настройки устройства (под d.sem).
func (d *Device) resetModel() {
	d.Model.Reset()
	d.mu.Lock()
	d.lastErrCode, d.lastErrText = 0, ""
	settings := copyMap(d.settings)
	d.mu.Unlock()
	d.printSeq = 0 // лента модели после сброса начинается заново
	if err := d.applySettings(settings); err != nil {
		slog.Warn("настройки устройства не применены после сброса", "device", d.ID, "err", err)
	}
}

// --- представления для Control API ---

type DeviceInfo struct {
	DeviceID       string         `json:"device_id"`
	Type           string         `json:"type"`
	EquipmentType  string         `json:"equipment_type"`
	WorkstationID  string         `json:"workstation_id"`
	Name           string         `json:"name"`
	CreatedAt      time.Time      `json:"created_at"`
	LastSeen       *time.Time     `json:"last_seen,omitempty"`
	Commands       int64          `json:"commands"`
	ActiveSessions int            `json:"active_sessions"`
	EventListeners int            `json:"event_listeners"` // компоненты, слушающие события
	LastError      *ErrorInfo     `json:"last_error,omitempty"`
	State          map[string]any `json:"state"`

	// Учёт устройств (docs/protocol.md §2.3).
	InstanceID   string         `json:"instance_id,omitempty"`
	InstanceName string         `json:"instance_name,omitempty"`
	Infobase     string         `json:"infobase,omitempty"`
	KeyedBy      string         `json:"keyed_by"` // instance | workstation
	Copy         bool           `json:"copy,omitempty"`
	Alias        string         `json:"alias,omitempty"`
	Note         string         `json:"note,omitempty"`
	Settings     map[string]any `json:"settings"`
	Output       *models.Output `json:"output,omitempty"` // вывод документов на рабочем месте
	Online       bool           `json:"online"`
}

type ErrorInfo struct {
	Code int    `json:"code"`
	Text string `json:"text"`
}

type SessionInfo struct {
	SessionID         string    `json:"session_id"`
	DeviceID          string    `json:"device_id"`
	WorkstationID     string    `json:"workstation_id"`
	EquipmentType     string    `json:"equipment_type"`
	DriverVersion     string    `json:"driver_version,omitempty"`
	InterfaceRevision int       `json:"interface_revision,omitempty"`
	CreatedAt         time.Time `json:"created_at"`
	LastSeen          time.Time `json:"last_seen"`
}

// sessionStat — сессии устройства: сколько и когда последняя была активна.
type sessionStat struct {
	count int
	last  time.Time
}

func (m *Manager) sessionStats() map[string]sessionStat {
	out := map[string]sessionStat{}
	for _, s := range m.sessions {
		st := out[s.DeviceID]
		st.count++
		if ls := s.LastSeen(); ls.After(st.last) {
			st.last = ls
		}
		out[s.DeviceID] = st
	}
	return out
}

func (d *Device) info(ss sessionStat, now time.Time) DeviceInfo {
	d.mu.Lock()
	info := DeviceInfo{
		DeviceID: d.ID, Type: d.Type.Key, EquipmentType: d.Type.Name, WorkstationID: d.WorkstationID,
		Name: d.Model.Description().Name, CreatedAt: d.CreatedAt,
		Commands: d.commands, ActiveSessions: ss.count, EventListeners: d.Listeners(),
		InstanceID: d.InstanceID, InstanceName: d.instanceName, Infobase: d.infobase, KeyedBy: "workstation",
		Copy: d.Copy, Alias: d.alias, Note: d.note, Settings: copyMap(d.settings), Output: copyOutput(d.output),
		Online: ss.count > 0 && now.Sub(ss.last) <= onlineWindow,
	}
	if d.InstanceID != "" {
		info.KeyedBy = "instance"
	}
	if info.Settings == nil {
		info.Settings = map[string]any{}
	}
	if !d.lastSeen.IsZero() {
		ls := d.lastSeen
		info.LastSeen = &ls
	}
	if d.lastErrCode != 0 || d.lastErrText != "" {
		info.LastError = &ErrorInfo{Code: d.lastErrCode, Text: d.lastErrText}
	}
	d.mu.Unlock()
	info.State = d.Model.State()
	return info
}

// DeviceInfo — представление одного устройства для Control API.
func (m *Manager) DeviceInfo(id string) (DeviceInfo, bool) {
	m.mu.Lock()
	d, ok := m.devices[id]
	stats := m.sessionStats()
	m.mu.Unlock()
	if !ok {
		return DeviceInfo{}, false
	}
	return d.info(stats[id], m.cfg.Now().UTC()), true
}

func (m *Manager) Devices() []DeviceInfo {
	m.mu.Lock()
	devs := make([]*Device, 0, len(m.devices))
	for _, d := range m.devices {
		devs = append(devs, d)
	}
	stats := m.sessionStats()
	m.mu.Unlock()

	now := m.cfg.Now().UTC()
	out := make([]DeviceInfo, 0, len(devs))
	for _, d := range devs {
		out = append(out, d.info(stats[d.ID], now))
	}
	sort.Slice(out, func(a, b int) bool { return out[a].DeviceID < out[b].DeviceID })
	return out
}

func (m *Manager) Sessions() []SessionInfo {
	m.mu.Lock()
	defer m.mu.Unlock()
	out := make([]SessionInfo, 0, len(m.sessions))
	for _, s := range m.sessions {
		out = append(out, SessionInfo{
			SessionID: s.ID, DeviceID: s.DeviceID, WorkstationID: s.WorkstationID,
			EquipmentType: s.Type.Name, DriverVersion: s.DriverVersion,
			InterfaceRevision: s.InterfaceRevision,
			CreatedAt:         s.CreatedAt, LastSeen: s.LastSeen(),
		})
	}
	sort.Slice(out, func(a, b int) bool { return out[a].CreatedAt.Before(out[b].CreatedAt) })
	return out
}

// --- служебное ---

func validateID(s string) error {
	if s == "" {
		return errors.New("пусто")
	}
	if len(s) > 128 {
		return errors.New("длиннее 128 байт")
	}
	for _, r := range s {
		if !unicode.IsPrint(r) {
			return errors.New("непечатаемые символы")
		}
	}
	return nil
}

// validateText — сведения об экземпляре и подписи: до 512 байт, без управляющих символов.
func validateText(s string) error {
	if len(s) > 512 {
		return errors.New("длиннее 512 байт")
	}
	for _, r := range s {
		if unicode.IsControl(r) {
			return errors.New("управляющие символы")
		}
	}
	return nil
}

func randomHex(n int) string {
	b := make([]byte, n)
	if _, err := rand.Read(b); err != nil {
		panic(fmt.Sprintf("crypto/rand: %v", err))
	}
	return hex.EncodeToString(b)
}
