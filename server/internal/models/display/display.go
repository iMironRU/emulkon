// Package display — дисплей покупателя (раздел 3.3 требований к драйверам):
// docs/bpo-requirements/display.md, поведение — docs/models/display.md.
package display

import (
	"context"
	"encoding/base64"
	"encoding/json"
	"encoding/xml"
	"errors"
	"fmt"
	"sort"
	"strings"
	"sync"
	"time"
	"unicode/utf8"

	"emulkon/internal/models"
	"emulkon/internal/models/kkt"
)

// Коды ошибок (docs/models/display.md).
const (
	ErrNoCapability = 85
	ErrNoResponse   = 86
	ErrBadParams    = 98
	ErrNotSupported = 99
)

var errorTexts = map[int]string{
	ErrNoCapability: "Дисплей не поддерживает этот вывод",
	ErrNoResponse:   "Дисплей покупателя не отвечает",
	ErrBadParams:    "Неверные параметры вызова",
	ErrNotSupported: "Метод не поддерживается эмулятором дисплея",
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

// Options — ПараметрыДисплеяПокупателя (CustomerDisplayOptions, out).
type Options struct {
	XMLName                xml.Name   `xml:"CustomerDisplayOptions"`
	Columns                kkt.Number `xml:"Columns,attr"`
	Rows                   kkt.Number `xml:"Rows,attr"`
	DisplayText            kkt.Bool   `xml:"DisplayText,attr"`
	DisplayGraphics        kkt.Bool   `xml:"DisplayGraphics,attr"`
	DisplayQRCode          kkt.Bool   `xml:"DisplayQRCode,attr"`
	DisplayHTML            kkt.Bool   `xml:"DisplayHTML,attr"`
	DisplayStatus          kkt.Bool   `xml:"DisplayStatus,attr"`
	DisplayWorkplaceStatus kkt.Bool   `xml:"DisplayWorkplaceStatus,attr"`
	DisplayInteractiveMode kkt.Bool   `xml:"DisplayInteractiveMode,attr"`
}

// ModelOptions — параметры модели.
type ModelOptions struct {
	Version string
	Now     func() time.Time
}

// Factory — фабрика для реестра.
func Factory(version string) models.Factory {
	return func(_ models.EquipmentType, _ string) models.Model { return New(ModelOptions{Version: version}) }
}

const (
	maxHTML       = 4 << 20
	maxPicture    = 2 << 20
	maxMediaTotal = 32 << 20
)

type caps struct {
	Columns, Rows                                      int
	Text, Graphics, QR, HTML, Status, Workplace, Touch bool
	Broken                                             bool // дисплей не отвечает
}

func defaultCaps() caps {
	return caps{Columns: 20, Rows: 2, Text: true, QR: true, HTML: true, Status: true, Workplace: true}
}

type qrView struct {
	Value   string `json:"value"`
	Picture string `json:"picture,omitempty"` // PNG в Base64
}

type mediaFile struct {
	Name string    `json:"name"`
	Size int       `json:"size"` // байт в Base64
	At   time.Time `json:"at"`
	data string
}

// Model — эмулятор дисплея покупателя.
type Model struct {
	opts ModelOptions

	mu        sync.Mutex
	hook      models.EventHook
	caps      caps
	lines     []string
	qr        *qrView
	html      string
	shown     string // что на экране сейчас: text, qr, html, clear
	opStatus  int
	workplace int
	beeps     int
	media     map[string]*mediaFile
	mediaSize int
	outputs   int
	clicks    int
	updated   time.Time
}

func New(opts ModelOptions) *Model {
	if opts.Now == nil {
		opts.Now = time.Now
	}
	m := &Model{opts: opts}
	m.reset()
	return m
}

func (m *Model) reset() {
	m.caps = defaultCaps()
	m.clear()
	m.opStatus, m.workplace, m.beeps = 0, 0, 0
	m.media, m.mediaSize = map[string]*mediaFile{}, 0
	m.outputs, m.clicks = 0, 0
}

func (m *Model) clear() {
	m.lines = make([]string, m.caps.Rows)
	m.qr, m.html, m.shown = nil, "", "clear"
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
		Name:        "ЭМУЛЯТОР: дисплей покупателя (Эмулкон)",
		Description: "Виртуальный дисплей покупателя Эмулкон: строки, QR, HTML, статусы — в панели управления.",
		Version:     m.opts.Version, IsEmulator: true,
	}
}

func (m *Model) Handle(_ context.Context, method string, params []any) models.Result {
	a := models.Args(params)
	out := models.CopyParams(params)
	m.mu.Lock()
	defer m.mu.Unlock()
	var err error
	switch method {
	case "ConnectEquipment", "DisconnectEquipment":
	case "EquipmentTest":
		if err = a.Need(4); err == nil {
			out[2] = fmt.Sprintf("Тест пройден. ЭМУЛЯТОР дисплея покупателя (Эмулкон): %d×%d символов.", m.caps.Columns, m.caps.Rows)
			out[3] = "ЭМУЛЯТОР Эмулкон"
		}
	case "GetCustomerDisplayOptions":
		if err = a.Need(2); err == nil {
			c := m.caps
			var s string
			s, err = kkt.Marshal(Options{
				Columns: kkt.Number(c.Columns), Rows: kkt.Number(c.Rows), DisplayText: kkt.Bool(c.Text),
				DisplayGraphics: kkt.Bool(c.Graphics), DisplayQRCode: kkt.Bool(c.QR), DisplayHTML: kkt.Bool(c.HTML),
				DisplayStatus: kkt.Bool(c.Status), DisplayWorkplaceStatus: kkt.Bool(c.Workplace),
				DisplayInteractiveMode: kkt.Bool(c.Touch),
			})
			out[1] = s
		}
	case "GetOutputParameters":
		// Старая схема (БПО УТ 10.3): ПолучитьПараметрыВывода(0 ИД, 1 Столбцов out, 2 Строк out).
		if err = a.Need(3); err == nil {
			out[1], out[2] = int64(m.caps.Columns), int64(m.caps.Rows)
		}
	case "StringOutputOnCustomerDisplay":
		err = m.strings(a)
	case "ClearCustomerDisplay":
		if err = a.Need(1); err == nil {
			if err = m.alive(); err == nil {
				m.clear()
				m.touch()
			}
		}
	case "QRCodeOutputOnCustomerDisplay":
		err = m.qrCode(a)
	case "OutputHTML":
		err = m.outputHTML(a)
	case "AddMediaFile":
		err = m.addMedia(a)
	case "DisplayOperationStatus":
		err = m.operationStatus(a)
	case "DisplayWorkplaceStatus":
		err = m.workplaceStatus(a)
	default:
		err = fail(ErrNotSupported, "%s", method)
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

func (m *Model) alive() error {
	if m.caps.Broken {
		return &devError{code: ErrNoResponse}
	}
	return nil
}

func (m *Model) need(ok bool, what string) error {
	if err := m.alive(); err != nil {
		return err
	}
	if !ok {
		return fail(ErrNoCapability, "%s выключен в параметрах дисплея", what)
	}
	return nil
}

func (m *Model) touch() {
	m.outputs++
	m.updated = m.opts.Now()
}

// StringOutputOnCustomerDisplay(0 DeviceID, 1 Strings): строка — строка дисплея; пустая не меняет.
func (m *Model) strings(a models.Args) error {
	if err := a.Need(2); err != nil {
		return err
	}
	s, err := a.String(1)
	if err != nil {
		return err
	}
	if err := m.need(m.caps.Text, "вывод текста"); err != nil {
		return err
	}
	for i, l := range strings.Split(strings.ReplaceAll(s, "\r\n", "\n"), "\n") {
		if i >= m.caps.Rows {
			break
		}
		if l == "" {
			continue
		}
		if utf8.RuneCountInString(l) > m.caps.Columns {
			l = string([]rune(l)[:m.caps.Columns])
		}
		m.lines[i] = l
	}
	if m.shown != "qr" {
		m.shown = "text" // строки рядом с QR не скрывают его
	}
	m.html = ""
	m.touch()
	return nil
}

func isPNG(b64 string) bool {
	raw, err := base64.StdEncoding.DecodeString(strings.TrimSpace(b64))
	return err == nil && len(raw) > 8 && string(raw[1:4]) == "PNG"
}

// QRCodeOutputOnCustomerDisplay(0 DeviceID, 1 QRCodeValue, 2 QRCodePicture — PNG в Base64).
func (m *Model) qrCode(a models.Args) error {
	if err := a.Need(3); err != nil {
		return err
	}
	value, err := a.String(1)
	if err != nil {
		return err
	}
	pic, err := a.String(2)
	if err != nil {
		return err
	}
	if err := m.need(m.caps.QR, "вывод QR-кода"); err != nil {
		return err
	}
	if value == "" {
		return fail(ErrBadParams, "пустое значение QR-кода")
	}
	if pic != "" && (len(pic) > maxPicture || !isPNG(pic)) {
		return fail(ErrBadParams, "картинка QR-кода — PNG в Base64 до %d МБ", maxPicture>>20)
	}
	m.qr = &qrView{Value: value, Picture: pic}
	m.html, m.shown = "", "qr"
	m.touch()
	return nil
}

// OutputHTML(0 DeviceID, 1 HTML).
func (m *Model) outputHTML(a models.Args) error {
	if err := a.Need(2); err != nil {
		return err
	}
	s, err := a.String(1)
	if err != nil {
		return err
	}
	if err := m.need(m.caps.HTML, "вывод HTML"); err != nil {
		return err
	}
	if len(s) > maxHTML {
		return fail(ErrBadParams, "HTML больше %d МБ", maxHTML>>20)
	}
	m.html, m.shown = s, "html"
	m.touch()
	return nil
}

// AddMediaFile(0 DeviceID, 1 FileName, 2 FileData — Base64): в хранилище дисплея для HTML.
func (m *Model) addMedia(a models.Args) error {
	if err := a.Need(3); err != nil {
		return err
	}
	name, err := a.String(1)
	if err != nil {
		return err
	}
	data, err := a.String(2)
	if err != nil {
		return err
	}
	if err := m.need(m.caps.HTML, "вывод HTML"); err != nil {
		return err
	}
	name = strings.TrimSpace(name)
	if name == "" || strings.ContainsAny(name, `/\:`) {
		return fail(ErrBadParams, "имя файла %q: без пути", name)
	}
	if _, err := base64.StdEncoding.DecodeString(strings.TrimSpace(data)); err != nil || data == "" {
		return fail(ErrBadParams, "данные файла %q — не Base64", name)
	}
	total := m.mediaSize + len(data)
	if old, ok := m.media[name]; ok {
		total -= old.Size
	}
	if total > maxMediaTotal {
		return fail(ErrBadParams, "хранилище дисплея — не больше %d МБ", maxMediaTotal>>20)
	}
	m.media[name] = &mediaFile{Name: name, Size: len(data), At: m.opts.Now(), data: data}
	m.mediaSize = total
	return nil
}

func intIn(a models.Args, i int, lo, hi int64, what string) (int, error) {
	n, err := a.Int(i)
	if err != nil {
		f, ferr := a.Float(i)
		if ferr != nil || f != float64(int64(f)) {
			return 0, err
		}
		n = int64(f)
	}
	if n < lo || n > hi {
		return 0, fail(ErrBadParams, "%s: от %d до %d", what, lo, hi)
	}
	return int(n), nil
}

// DisplayOperationStatus(0 DeviceID, 1 OperationStatus 0–2).
func (m *Model) operationStatus(a models.Args) error {
	if err := a.Need(2); err != nil {
		return err
	}
	n, err := intIn(a, 1, 0, 2, "статус операции")
	if err != nil {
		return err
	}
	if err := m.need(m.caps.Status, "индикатор статуса"); err != nil {
		return err
	}
	m.opStatus = n
	m.touch()
	return nil
}

// DisplayWorkplaceStatus(0 DeviceID, 1 WorkplaceStatus 0–4, 2 SoundSignal 0/1).
func (m *Model) workplaceStatus(a models.Args) error {
	if err := a.Need(3); err != nil {
		return err
	}
	n, err := intIn(a, 1, 0, 4, "статус рабочего места")
	if err != nil {
		return err
	}
	sound, err := intIn(a, 2, 0, 1, "звуковой сигнал")
	if err != nil {
		return err
	}
	if err := m.need(m.caps.Workplace, "статус рабочего места"); err != nil {
		return err
	}
	m.workplace = n
	m.beeps += sound
	m.touch()
	return nil
}

// Act — click: {"element": "…"} — нажатие на сенсорный экран (событие OnClick);
// media: {"name": "…"} — файл из хранилища дисплея (для показа HTML).
func (m *Model) Act(name string, args map[string]any) (any, error) {
	switch name {
	case "click":
		el, _ := args["element"].(string)
		el = strings.TrimSpace(el)
		m.mu.Lock()
		if !m.caps.Touch {
			m.mu.Unlock()
			return nil, errors.New("дисплей не отрабатывает нажатия: включите interactive")
		}
		if el == "" {
			m.mu.Unlock()
			return nil, errors.New("element: имя элемента, на который нажали")
		}
		m.clicks++
		h := m.hook
		m.mu.Unlock()
		if h != nil {
			h(models.DeviceEvent{Name: "OnClick", Data: el})
		}
		return map[string]any{"element": el}, nil
	case "media":
		n, _ := args["name"].(string)
		m.mu.Lock()
		defer m.mu.Unlock()
		f, ok := m.media[n]
		if !ok {
			return nil, fmt.Errorf("файла %q нет в хранилище дисплея", n)
		}
		return map[string]any{"name": f.Name, "data_base64": f.data}, nil
	}
	return nil, models.ErrUnknownAction
}

func (m *Model) State() map[string]any {
	m.mu.Lock()
	defer m.mu.Unlock()
	media := make([]mediaFile, 0, len(m.media))
	for _, f := range m.media {
		media = append(media, *f)
	}
	sort.Slice(media, func(i, k int) bool { return media[i].Name < media[k].Name })
	var updated any
	if !m.updated.IsZero() {
		updated = m.updated
	}
	c := m.caps
	return map[string]any{
		"model": "display", "columns": c.Columns, "rows": c.Rows,
		"text": c.Text, "graphics": c.Graphics, "qr": c.QR, "html": c.HTML, "status": c.Status,
		"workplace_status": c.Workplace, "interactive": c.Touch, "broken": c.Broken,
		"lines": append([]string{}, m.lines...), "qr_view": m.qr, "html_view": m.html, "shown": m.shown,
		"operation_status": m.opStatus, "workplace": m.workplace, "beeps": m.beeps,
		"media": media, "outputs": m.outputs, "clicks": m.clicks, "updated_at": updated,
	}
}

// StateKeys — ключи SetState (docs/models/display.md).
var StateKeys = []string{"columns", "rows", "text", "graphics", "qr", "html", "status", "workplace_status", "interactive", "broken"}

func (m *Model) SetState(patch map[string]any) error {
	m.mu.Lock()
	defer m.mu.Unlock()
	c := m.caps
	for k, v := range patch {
		var err error
		switch k {
		case "columns", "rows":
			lo, hi := 8, 80
			if k == "rows" {
				lo, hi = 1, 20
			}
			var n float64
			switch x := v.(type) {
			case float64:
				n = x
			case json.Number:
				n, err = x.Float64()
			default:
				err = fmt.Errorf("%s: число", k)
			}
			if err == nil && (n < float64(lo) || n > float64(hi) || n != float64(int(n))) {
				err = fmt.Errorf("%s: целое от %d до %d", k, lo, hi)
			}
			if k == "columns" {
				c.Columns = int(n)
			} else {
				c.Rows = int(n)
			}
		case "text", "graphics", "qr", "html", "status", "workplace_status", "interactive", "broken":
			b, ok := v.(bool)
			if !ok {
				err = fmt.Errorf("%s: true или false", k)
			}
			switch k {
			case "text":
				c.Text = b
			case "graphics":
				c.Graphics = b
			case "qr":
				c.QR = b
			case "html":
				c.HTML = b
			case "status":
				c.Status = b
			case "workplace_status":
				c.Workplace = b
			case "interactive":
				c.Touch = b
			case "broken":
				c.Broken = b
			}
		default:
			err = fmt.Errorf("неизвестный ключ %q; допустимы: %s", k, strings.Join(StateKeys, ", "))
		}
		if err != nil {
			return err
		}
	}
	if c.Rows != m.caps.Rows || c.Columns != m.caps.Columns {
		m.caps = c
		m.clear() // другой размер — экран пустой
		return nil
	}
	m.caps = c
	return nil
}
