package printers

import (
	"context"
	"encoding/xml"
	"fmt"
	"math"
	"sort"
	"strconv"
	"strings"
	"sync"
	"time"

	"emulkon/internal/models"
	"emulkon/internal/models/kkt"
)

// --- XML (docs/bpo-requirements/printers.md) ---

type labelsTable struct {
	XMLName    xml.Name    `xml:"Data"`
	Formatting *formatting `xml:"Formatting"`
	Labels     *struct {
		Items []labelXML `xml:"Label"`
	} `xml:"Labels"`
}

type formatting struct {
	Width  kkt.Number `xml:"Width,attr"`
	Height kkt.Number `xml:"Height,attr"`
	Fields []fieldXML `xml:",any"`
}

// fieldXML — Text, Barcode, Image или UserData шаблона.
type fieldXML struct {
	XMLName     xml.Name
	FieldName   string     `xml:"FieldName,attr"`
	Left        kkt.Number `xml:"Left,attr"`
	Top         kkt.Number `xml:"Top,attr"`
	Width       kkt.Number `xml:"Width,attr"`
	Height      kkt.Number `xml:"Height,attr"`
	Orientation kkt.Number `xml:"Orientation,attr"`
	FontSize    kkt.Number `xml:"FontSize,attr"`
	FontStyle   string     `xml:"FontStyle,attr"`
	Border      string     `xml:"Border,attr"`
	Align       string     `xml:"Align,attr"`
	VAlign      string     `xml:"VAlign,attr"`
	Multiline   kkt.Bool   `xml:"Multiline,attr"`
	Static      kkt.Bool   `xml:"Static,attr"`
	Value       string     `xml:"Value,attr"`
	ValueBase64 string     `xml:"ValueBase64,attr"`
	Type        string     `xml:"Type,attr"`
	PrintHRI    *kkt.Bool  `xml:"PrintHRI,attr"`
}

type labelXML struct {
	Quantity *kkt.Number `xml:"Quantity,attr"`
	Records  []struct {
		FieldName   string `xml:"FieldName,attr"`
		Value       string `xml:"Value,attr"`
		ValueBase64 string `xml:"ValueBase64,attr"`
	} `xml:"Record"`
}

// field — поле шаблона с значением по умолчанию.
type field struct {
	models.LabelField
	static bool
	value  string // значение шаблона (штрихкод — уже из Base64)
}

type layout struct {
	width, height float64
	fields        []field // порядок шаблона
	byName        map[string]int
}

// job — задание печати: пакеты от Formatting до last.
type job struct {
	Status    string    `json:"status"` // статус последнего пакета
	Open      bool      `json:"open"`
	Packages  int       `json:"packages"`
	Labels    int       `json:"labels"`
	Copies    int       `json:"copies"`
	WidthMM   float64   `json:"width_mm"`
	HeightMM  float64   `json:"height_mm"`
	StartedAt time.Time `json:"started_at"`
	UpdatedAt time.Time `json:"updated_at"`
}

const (
	layoutLabels   = 20       // этикеток пакета в layout для веб-интерфейса
	layoutImageMax = 64 << 10 // картинка больше — в layout не попадает
	labelTextWidth = 40
)

// Label — эмулятор принтера этикеток (docs/models/printers.md).
type Label struct {
	opts Options

	mu            sync.Mutex
	dev           device
	initialized   bool
	inits         int
	layout        *layout // разметка открытого задания
	job           *job
	jobs          int
	labelsPrinted int
	tape          tape
}

// LabelFactory — фабрика принтера этикеток для реестра.
func LabelFactory(version string) models.Factory {
	return func(_ models.EquipmentType, _ string) models.Model {
		return NewLabel(Options{Version: version})
	}
}

func NewLabel(opts Options) *Label {
	opts.defaults()
	m := &Label{opts: opts}
	m.reset()
	return m
}

func (m *Label) reset() {
	m.dev = device{paper: true}
	m.initialized, m.inits = false, 0
	m.layout, m.job, m.jobs, m.labelsPrinted = nil, nil, 0, 0
	m.tape = tape{}
}

func (m *Label) Description() models.Description {
	return models.Description{
		Name:        "ЭМУЛЯТОР: принтер этикеток (Эмулкон)",
		Description: "Виртуальный принтер этикеток Эмулкон: этикетки по шаблону из 1С, просмотр в веб-интерфейсе.",
		Version:     m.opts.Version, IsEmulator: true,
	}
}

func (m *Label) Reset() {
	m.mu.Lock()
	m.reset()
	m.mu.Unlock()
}

func (m *Label) Handle(_ context.Context, method string, params []any) models.Result {
	a := models.Args(params)
	out := models.CopyParams(params)
	m.mu.Lock()
	defer m.mu.Unlock()
	var err error
	switch method {
	case "ConnectEquipment", "DisconnectEquipment":
	case "EquipmentTest":
		if err = a.Need(4); err == nil {
			paper := "этикетки есть"
			if !m.dev.paper {
				paper = "этикетки кончились"
			}
			out[2] = fmt.Sprintf("Тест пройден. ЭМУЛЯТОР принтера этикеток (Эмулкон): %s.", paper)
			out[3] = "ЭМУЛЯТОР Эмулкон"
		}
	case "InitializePrinter":
		if err = a.Need(1); err == nil {
			m.initialized = true
			m.inits++
		}
	case "PrintLabels":
		err = m.printLabels(a)
	default:
		err = fail(ErrNotSupported, "%s", method)
	}
	return result(err, out)
}

func parseLayout(f *formatting) (*layout, error) {
	l := &layout{width: float64(f.Width), height: float64(f.Height), byName: map[string]int{}}
	if l.width <= 0 || l.height <= 0 {
		return nil, fail(ErrBadParams, "Formatting: Width и Height больше 0")
	}
	for i, x := range f.Fields {
		typ := strings.ToLower(x.XMLName.Local)
		switch typ {
		case "text", "barcode", "image", "userdata":
		default:
			continue // неизвестный элемент шаблона — пропускаем
		}
		if strings.TrimSpace(x.FieldName) == "" {
			return nil, fail(ErrBadParams, "Formatting: у поля %d (%s) нет FieldName", i+1, x.XMLName.Local)
		}
		fd := field{static: bool(x.Static), value: x.Value}
		fd.LabelField = models.LabelField{
			Type: typ, Name: x.FieldName,
			Left: float64(x.Left), Top: float64(x.Top), Width: float64(x.Width), Height: float64(x.Height),
			Orientation: int(x.Orientation), FontSize: float64(x.FontSize), FontStyle: x.FontStyle,
			Align: x.Align, VAlign: x.VAlign, Multiline: bool(x.Multiline), Border: x.Border,
		}
		if typ == "barcode" {
			if !barcodeKnown(x.Type) {
				return nil, fail(ErrBarcodeType, "%q в поле %q", x.Type, x.FieldName)
			}
			fd.BarcodeType = x.Type
			fd.HRI = x.PrintHRI == nil || bool(*x.PrintHRI)
			if x.ValueBase64 != "" {
				fd.value = decodeBase64(x.ValueBase64)
			}
		}
		l.byName[x.FieldName] = len(l.fields)
		l.fields = append(l.fields, fd)
	}
	return l, nil
}

// printedLabel — значения одной этикетки по порядку полей шаблона.
type printedLabel struct {
	quantity int
	values   []string
}

func (l *layout) fill(x labelXML, n int) (printedLabel, error) {
	p := printedLabel{quantity: 1, values: make([]string, len(l.fields))}
	if x.Quantity != nil {
		q := float64(*x.Quantity)
		if q < 0 || q != math.Trunc(q) {
			return p, fail(ErrBadParams, "этикетка %d: Quantity — целое ≥ 0", n)
		}
		p.quantity = int(q)
	}
	for i, f := range l.fields {
		p.values[i] = f.value
	}
	for _, r := range x.Records {
		i, ok := l.byName[r.FieldName]
		if !ok {
			return p, fail(ErrBadParams, "этикетка %d: поля %q нет в разметке", n, r.FieldName)
		}
		if l.fields[i].static {
			continue // общее значение шаблона
		}
		v := r.Value
		if r.ValueBase64 != "" {
			v = r.ValueBase64
			if l.fields[i].Type == "barcode" {
				v = decodeBase64(v)
			}
		}
		p.values[i] = v
	}
	return p, nil
}

func trimNum(v float64) string { return strconv.FormatFloat(v, 'f', -1, 64) }

// PrintLabels(0 DeviceID, 1 LabelsTable, 2 PackageStatus).
func (m *Label) printLabels(a models.Args) error {
	if err := a.Need(3); err != nil {
		return err
	}
	status, err := a.String(2)
	if err != nil {
		return err
	}
	status = strings.ToLower(strings.TrimSpace(status))
	if status != "first" && status != "regular" && status != "last" {
		return fail(ErrPackageStatus, "%q: ожидается first, regular или last", status)
	}
	s, err := a.String(1)
	if err != nil {
		return err
	}
	var t labelsTable
	if err := kkt.Unmarshal(s, &t); err != nil {
		return fail(ErrBadParams, "%v", err)
	}
	if t.Labels == nil {
		return fail(ErrBadParams, "нет секции Labels")
	}
	l := m.layout
	if t.Formatting != nil {
		if l, err = parseLayout(t.Formatting); err != nil {
			return err
		}
	} else if l == nil || m.job == nil || !m.job.Open {
		return &devError{code: ErrNoLayout}
	}
	labels := make([]printedLabel, 0, len(t.Labels.Items))
	copies := 0
	for i, x := range t.Labels.Items {
		p, err := l.fill(x, i+1)
		if err != nil {
			return err
		}
		labels = append(labels, p)
		copies += p.quantity
	}
	if err := m.dev.ready(); err != nil {
		return err
	}

	now := m.opts.Now()
	if t.Formatting != nil {
		m.layout = l
		m.job = &job{Open: true, WidthMM: l.width, HeightMM: l.height, StartedAt: now}
		m.jobs++
	}
	j := m.job
	j.Status = status
	j.Packages++
	j.Labels += len(labels)
	j.Copies += copies
	j.UpdatedAt = now
	if status == "last" {
		j.Open = false
		m.layout = nil
	}
	m.labelsPrinted += copies
	m.tape.add(models.PrintedDocument{Kind: "label", Time: now, Lines: l.text(labels, status, j.Packages, copies, now),
		Layout: l.view(labels)})
	return nil
}

// text — пакет этикеток текстом для ленты и трея.
func (l *layout) text(labels []printedLabel, status string, pkg, copies int, now time.Time) []string {
	w := labelTextWidth
	rule := strings.Repeat("-", w)
	lines := []string{
		center("*** ЭМУЛЯТОР · ЭТИКЕТКИ ***", w),
		fmt.Sprintf("ЭТИКЕТКА %s×%s ММ", trimNum(l.width), trimNum(l.height)),
		fmt.Sprintf("ПАКЕТ %d (%s): ЭТИКЕТОК %d, КОПИЙ %d", pkg, status, len(labels), copies),
		now.Format("02.01.2006 15:04"),
	}
	order := make([]int, 0, len(l.fields))
	for i, f := range l.fields {
		if f.Type != "userdata" {
			order = append(order, i)
		}
	}
	sort.SliceStable(order, func(a, b int) bool {
		fa, fb := l.fields[order[a]], l.fields[order[b]]
		if fa.Top != fb.Top {
			return fa.Top < fb.Top
		}
		return fa.Left < fb.Left
	})
	for n, p := range labels {
		lines = append(lines, rule, fmt.Sprintf("[%d] × %d", n+1, p.quantity))
		for _, i := range order {
			f, v := l.fields[i], p.values[i]
			switch f.Type {
			case "barcode":
				v = "[" + strings.ToUpper(f.BarcodeType) + ": " + v + "]"
			case "image":
				if v == "" {
					v = "[картинки нет]"
				} else {
					v = fmt.Sprintf("[картинка, %d КБ]", (len(v)*3/4+1023)/1024)
				}
			}
			lines = append(lines, wrap(f.Name+": "+v, w)...)
		}
	}
	return append(lines, rule, center("*** ЭМУЛЯТОР · ЭТИКЕТКИ ***", w))
}

// view — разметка и значения первых этикеток для веб-интерфейса.
func (l *layout) view(labels []printedLabel) *models.LabelLayout {
	v := &models.LabelLayout{WidthMM: l.width, HeightMM: l.height, Total: len(labels)}
	for _, f := range l.fields {
		if f.Type != "userdata" {
			v.Fields = append(v.Fields, f.LabelField)
		}
	}
	for n, p := range labels {
		if n == layoutLabels {
			break
		}
		vals := map[string]string{}
		for i, f := range l.fields {
			if f.Type == "userdata" || (f.Type == "image" && len(p.values[i]) > layoutImageMax) {
				continue
			}
			vals[f.Name] = p.values[i]
		}
		v.Labels = append(v.Labels, models.LabelValues{Quantity: p.quantity, Values: vals})
	}
	return v
}

func (m *Label) Prints(limit int) []models.PrintedDocument {
	m.mu.Lock()
	defer m.mu.Unlock()
	return m.tape.last(limit)
}

func (m *Label) PrintsSince(seq int64) []models.PrintedDocument {
	m.mu.Lock()
	defer m.mu.Unlock()
	return m.tape.since(seq)
}

func (m *Label) State() map[string]any {
	m.mu.Lock()
	defer m.mu.Unlock()
	var j any
	if m.job != nil {
		c := *m.job
		j = c
	}
	return map[string]any{
		"model":          "label_printer",
		"paper":          m.dev.paper,
		"cover_open":     m.dev.cover,
		"initialized":    m.initialized,
		"inits":          m.inits,
		"job":            j,
		"jobs":           m.jobs,
		"labels_printed": m.labelsPrinted,
	}
}

// LabelStateKeys — ключи SetState принтера этикеток (docs/models/printers.md).
var LabelStateKeys = []string{"paper", "cover_open"}

func (m *Label) SetState(patch map[string]any) error {
	m.mu.Lock()
	defer m.mu.Unlock()
	dev := m.dev
	for k, v := range patch {
		var err error
		switch k {
		case "paper":
			dev.paper, err = asBool(k, v)
		case "cover_open":
			dev.cover, err = asBool(k, v)
		default:
			err = fmt.Errorf("неизвестный ключ %q; допустимы: %s", k, strings.Join(LabelStateKeys, ", "))
		}
		if err != nil {
			return err
		}
	}
	m.dev = dev
	return nil
}
