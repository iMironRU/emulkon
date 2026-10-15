// Package rfid — считыватель RFID (раздел 3.10 требований к драйверам):
// docs/bpo-requirements/rfid.md, поведение — docs/models/rfid.md.
package rfid

import (
	"context"
	"encoding/hex"
	"encoding/xml"
	"errors"
	"fmt"
	"math/rand/v2"
	"strings"
	"sync"
	"time"

	"emulkon/internal/models"
	"emulkon/internal/models/kkt"
)

// Коды ошибок (docs/models/rfid.md).
const (
	ErrNoSession    = 75
	ErrNoTag        = 76
	ErrNoPackage    = 77
	ErrBadParams    = 98
	ErrNotSupported = 99
)

var errorTexts = map[int]string{
	ErrNoSession:    "Сессия RFID не открыта",
	ErrNoTag:        "Метки нет в поле считывателя",
	ErrNoPackage:    "Пакета с данными меток нет",
	ErrBadParams:    "Неверные параметры вызова",
	ErrNotSupported: "Метод не поддерживается эмулятором RFID",
}

type devError struct {
	code   int
	detail string
}

func (e *devError) Error() string {
	if e.detail == "" {
		return errorTexts[e.code]
	}
	return errorTexts[e.code] + ": " + e.detail
}

func fail(code int, format string, args ...any) *devError {
	return &devError{code: code, detail: fmt.Sprintf(format, args...)}
}

// Tag — метка в поле антенны: банки памяти в HEX.
type Tag struct {
	TID  string `json:"tid" xml:"TID,attr"`
	EPC  string `json:"epc" xml:"EPC,attr"`
	USER string `json:"user" xml:"USER,attr"`
}

type tagsTable struct {
	XMLName xml.Name `xml:"Table"`
	Tags    []Tag    `xml:"Tag"`
}

type pkg struct {
	ID        string    `json:"id"`
	Tags      []Tag     `json:"tags"`
	Delivered bool      `json:"delivered"`
	At        time.Time `json:"at"`
}

// Options — параметры модели.
type Options struct {
	Version string
	Now     func() time.Time
}

// Factory — фабрика для реестра.
func Factory(version string) models.Factory {
	return func(_ models.EquipmentType, _ string) models.Model { return New(Options{Version: version}) }
}

const maxPackages = 50

// Model — эмулятор считывателя RFID: метки кладут в поле антенны в панели управления.
type Model struct {
	opts Options
	rnd  *rand.Rand

	mu       sync.Mutex
	hook     models.EventHook
	field    []Tag           // метки в зоне антенны
	reported map[string]bool // TID, о которых сообщено в текущей сессии
	session  bool
	pkgSeq   int
	packages []*pkg
	writes   int
}

func New(opts Options) *Model {
	if opts.Now == nil {
		opts.Now = time.Now
	}
	m := &Model{opts: opts, rnd: rand.New(rand.NewPCG(uint64(time.Now().UnixNano()), 11))}
	m.reset()
	return m
}

func (m *Model) reset() {
	m.field, m.reported, m.session = nil, map[string]bool{}, false
	m.pkgSeq, m.packages, m.writes = 0, nil, 0
}

func (m *Model) Reset() {
	m.mu.Lock()
	m.reset()
	m.mu.Unlock()
}

func (m *Model) SetEventHook(h models.EventHook) {
	m.mu.Lock()
	m.hook = h
	m.mu.Unlock()
}

func (m *Model) Description() models.Description {
	return models.Description{
		Name:        "ЭМУЛЯТОР: считыватель RFID (Эмулкон)",
		Description: "Виртуальный RFID-считыватель Эмулкон: метки кладут в поле антенны в панели управления.",
		Version:     m.opts.Version, IsEmulator: true,
	}
}

func (m *Model) Handle(_ context.Context, method string, params []any) models.Result {
	a := models.Args(params)
	out := models.CopyParams(params)
	var err error
	var events []models.DeviceEvent
	m.mu.Lock()
	switch method {
	case "ConnectEquipment":
	case "DisconnectEquipment":
		m.session = false
	case "EquipmentTest":
		if err = a.Need(4); err == nil {
			out[2] = fmt.Sprintf("Тест пройден. ЭМУЛЯТОР считывателя RFID (Эмулкон): меток в поле %d.", len(m.field))
			out[3] = "ЭМУЛЯТОР Эмулкон"
		}
	case "OpenSessionRFID":
		if err = a.Need(1); err == nil {
			m.session = true
			m.reported = map[string]bool{}
			events = m.report() // метки, уже лежащие в поле, — первым пакетом
		}
	case "CloseSessionRFID":
		if err = a.Need(1); err == nil {
			m.session = false
		}
	case "GetDataTagsRFID":
		err = m.getData(a, out)
	case "SaveDataTagRFID":
		err = m.save(a, out)
	default:
		err = fail(ErrNotSupported, "%s", method)
	}
	h := m.hook
	m.mu.Unlock()
	if h != nil {
		for _, e := range events {
			h(e)
		}
	}
	if err != nil {
		var de *devError
		if !errors.As(err, &de) {
			de = fail(ErrBadParams, "%v", err)
		}
		return models.Result{Return: false, ErrorCode: de.code, ErrorText: de.Error()}
	}
	return models.Result{Return: true, Params: out}
}

// report — пакет из меток поля, о которых в сессии ещё не сообщали (под замком).
func (m *Model) report() []models.DeviceEvent {
	if !m.session {
		return nil
	}
	var fresh []Tag
	for _, t := range m.field {
		if !m.reported[t.TID] {
			m.reported[t.TID] = true
			fresh = append(fresh, t)
		}
	}
	if len(fresh) == 0 {
		return nil
	}
	m.pkgSeq++
	p := &pkg{ID: fmt.Sprintf("pkg-%d", m.pkgSeq), Tags: fresh, At: m.opts.Now()}
	m.packages = append(m.packages, p)
	if len(m.packages) > maxPackages {
		m.packages = m.packages[len(m.packages)-maxPackages:]
	}
	return []models.DeviceEvent{{Name: "RFID", Data: p.ID}}
}

// GetDataTagsRFID(0 DeviceID, 1 PackageID, 2 TagsTable out). Вызов — подтверждение доставки.
func (m *Model) getData(a models.Args, out []any) error {
	if err := a.Need(3); err != nil {
		return err
	}
	id, err := a.String(1)
	if err != nil {
		return err
	}
	for _, p := range m.packages {
		if p.ID == id {
			s, err := kkt.Marshal(tagsTable{Tags: p.Tags})
			if err != nil {
				return err
			}
			out[2] = s
			p.Delivered = true
			return nil
		}
	}
	return fail(ErrNoPackage, "%q", id)
}

func cleanHex(s string) (string, error) {
	s = strings.ToUpper(strings.TrimSpace(s))
	if _, err := hex.DecodeString(s); err != nil || len(s)%2 != 0 {
		return "", fmt.Errorf("ожидается HEX, получено %q", s)
	}
	return s, nil
}

// SaveDataTagRFID(0 DeviceID, 1 TID inout, 2 EPC inout, 3 Data, 4 MemoryBank 1|2, 5 Timeout).
func (m *Model) save(a models.Args, out []any) error {
	if err := a.Need(6); err != nil {
		return err
	}
	tid, err := a.String(1)
	if err != nil {
		return err
	}
	epc, err := a.String(2)
	if err != nil {
		return err
	}
	data, err := a.String(3)
	if err != nil {
		return err
	}
	bank, err := a.Int(4)
	if err != nil {
		return err
	}
	if bank != 1 && bank != 2 {
		return fail(ErrBadParams, "банк памяти: 1 — EPC, 2 — USER")
	}
	if data, err = cleanHex(data); err != nil {
		return fail(ErrBadParams, "данные: %v", err)
	}
	if !m.session {
		return &devError{code: ErrNoSession}
	}
	tid, epc = strings.ToUpper(strings.TrimSpace(tid)), strings.ToUpper(strings.TrimSpace(epc))
	for i := range m.field {
		t := &m.field[i]
		if t.TID != tid || (epc != "" && t.EPC != epc) {
			continue
		}
		if bank == 1 {
			t.EPC = data
		} else {
			t.USER = data
		}
		out[1], out[2] = t.TID, t.EPC
		m.writes++
		return nil
	}
	return fail(ErrNoTag, "TID %s, EPC %s", tid, epc)
}

func (m *Model) hexN(n int) string {
	b := make([]byte, n)
	for i := range b {
		b[i] = byte(m.rnd.IntN(256))
	}
	return strings.ToUpper(hex.EncodeToString(b))
}

// Act — add_tag: {"tid", "epc", "user"} (пустые — сгенерировать); remove_tag: {"tid"}; clear_field.
// Новая метка в поле при открытой сессии — пакет и событие RFID.
func (m *Model) Act(name string, args map[string]any) (any, error) {
	str := func(k string) string { s, _ := args[k].(string); return s }
	var res any
	var events []models.DeviceEvent
	m.mu.Lock()
	switch name {
	case "add_tag":
		t := Tag{TID: str("tid"), EPC: str("epc"), USER: str("user")}
		if t.TID == "" {
			t.TID = "E280" + m.hexN(10) // TID: класс E2, 96 бит
		}
		if t.EPC == "" {
			t.EPC = "30" + m.hexN(11) // EPC SGTIN-96
		}
		var err error
		for _, f := range []*string{&t.TID, &t.EPC, &t.USER} {
			if *f == "" {
				continue
			}
			if *f, err = cleanHex(*f); err != nil {
				m.mu.Unlock()
				return nil, err
			}
		}
		for _, x := range m.field {
			if x.TID == t.TID {
				m.mu.Unlock()
				return nil, fmt.Errorf("метка %s уже в поле", t.TID)
			}
		}
		m.field = append(m.field, t)
		events = m.report()
		res = t
	case "remove_tag":
		tid := strings.ToUpper(str("tid"))
		found := false
		for i, x := range m.field {
			if x.TID == tid {
				m.field = append(m.field[:i], m.field[i+1:]...)
				delete(m.reported, tid) // вернётся в поле — сообщим снова
				found = true
				break
			}
		}
		if !found {
			m.mu.Unlock()
			return nil, fmt.Errorf("метки %s нет в поле", tid)
		}
		res = map[string]any{"removed": tid}
	case "clear_field":
		m.field, m.reported = nil, map[string]bool{}
		res = map[string]any{"cleared": true}
	default:
		m.mu.Unlock()
		return nil, models.ErrUnknownAction
	}
	h := m.hook
	m.mu.Unlock()
	if h != nil {
		for _, e := range events {
			h(e)
		}
	}
	return res, nil
}

func (m *Model) State() map[string]any {
	m.mu.Lock()
	defer m.mu.Unlock()
	pk := make([]pkg, 0, len(m.packages))
	for i := len(m.packages) - 1; i >= 0 && len(pk) < 10; i-- {
		pk = append(pk, *m.packages[i])
	}
	return map[string]any{
		"model": "rfid", "session": m.session, "field": append([]Tag{}, m.field...),
		"packages": pk, "writes": m.writes,
	}
}
