package printers

import (
	"context"
	"fmt"
	"sort"
	"strings"
	"sync"
	"time"

	"emulkon/internal/models"
	"emulkon/internal/models/kkt"
)

// Receipt — эмулятор принтера чеков (docs/models/printers.md).
type Receipt struct {
	opts Options

	mu          sync.Mutex
	dev         device
	lineLength  int
	barcodes    bool
	documents   int
	drawerOpens int
	drawerAt    *time.Time
	drawer      models.Drawer
	tape        tape
	open        *legacyCheck // старая схема: чек между ОткрытьЧек и ЗакрытьЧек (legacy.go)
}

const defaultLineLength = 42

// ReceiptFactory — фабрика принтера чеков для реестра.
func ReceiptFactory(version string) models.Factory {
	return func(_ models.EquipmentType, _ string) models.Model {
		return NewReceipt(Options{Version: version})
	}
}

func NewReceipt(opts Options) *Receipt {
	opts.defaults()
	m := &Receipt{opts: opts}
	m.reset()
	return m
}

func (m *Receipt) reset() {
	m.dev = device{paper: true}
	m.lineLength = defaultLineLength
	m.barcodes = true
	m.documents, m.drawerOpens, m.drawerAt = 0, 0, nil
	m.drawer = models.NewDrawer()
	m.tape = tape{}
	m.open = nil
}

func (m *Receipt) Description() models.Description {
	return models.Description{
		Name:        "ЭМУЛЯТОР: принтер чеков (Эмулкон)",
		Description: "Виртуальный нефискальный принтер чеков Эмулкон: текстовые документы, штрихкоды, денежный ящик.",
		Version:     m.opts.Version, IsEmulator: true,
	}
}

func (m *Receipt) Reset() {
	m.mu.Lock()
	m.reset()
	m.mu.Unlock()
}

func (m *Receipt) Handle(_ context.Context, method string, params []any) models.Result {
	a := models.Args(params)
	out := models.CopyParams(params)
	m.mu.Lock()
	defer m.mu.Unlock()
	var err error
	switch method {
	case "ConnectEquipment", "DisconnectEquipment":
	case "EquipmentTest":
		if err = a.Need(4); err == nil {
			paper := "бумага есть"
			if !m.dev.paper {
				paper = "нет бумаги"
			}
			out[2] = fmt.Sprintf("Тест пройден. ЭМУЛЯТОР принтера чеков (Эмулкон): ширина строки %d, %s.", m.lineLength, paper)
			out[3] = "ЭМУЛЯТОР Эмулкон"
		}
	case "PrintTextDocument":
		err = m.printText(a)
	case "OpenCashDrawer":
		if err = a.Need(1); err == nil {
			now := m.opts.Now()
			m.drawerOpens++
			m.drawerAt = &now
			m.drawer.Opened(now)
		}
	case "GetLineLength":
		if err = a.Need(2); err == nil {
			out[1] = int64(m.lineLength)
		}
	default:
		var legacy bool
		if legacy, err = m.legacy(method, a, out); !legacy {
			err = fail(ErrNotSupported, "%s", method)
		}
	}
	return result(err, out)
}

// PrintTextDocument(0 DeviceID, 1 DocumentPackage).
func (m *Receipt) printText(a models.Args) error {
	if err := a.Need(2); err != nil {
		return err
	}
	s, err := a.String(1)
	if err != nil {
		return err
	}
	var doc kkt.DocumentPackage
	if err := kkt.Unmarshal(s, &doc); err != nil {
		return fail(ErrBadParams, "%v", err)
	}
	for _, it := range doc.Positions.Items {
		if b := it.Barcode; b != nil {
			t := b.Type
			if t == "" {
				t = b.LegacyType
			}
			if !m.barcodes {
				return fail(ErrBarcodeType, "%q: принтер не печатает штрихкоды", t)
			}
			if !barcodeKnown(t) {
				return fail(ErrBarcodeType, "%q", t)
			}
		}
	}
	if err := m.dev.ready(); err != nil {
		return err
	}
	w := m.lineLength
	lines := []string{center(emuMark, w)}
	for _, it := range doc.Positions.Items {
		switch {
		case it.Text != nil:
			lines = append(lines, wrap(it.Text.Text, w)...)
		case it.Barcode != nil:
			b := it.Barcode
			t, v := b.Type, decodeBase64(b.ValueBase64)
			switch {
			case b.ValueBase64 != "":
			case b.Value != "":
				v = b.Value
			default:
				t, v = b.LegacyType, b.LegacyValue
			}
			for _, l := range wrap("["+strings.ToUpper(t)+": "+v+"]", w) {
				lines = append(lines, center(l, w))
			}
		}
	}
	now := m.opts.Now()
	lines = append(lines, center(now.Format("02.01.2006 15:04"), w), center(emuMark, w))
	m.tape.add(models.PrintedDocument{Kind: "text", Time: now, Lines: lines})
	m.documents++
	return nil
}

func (m *Receipt) Prints(limit int) []models.PrintedDocument {
	m.mu.Lock()
	defer m.mu.Unlock()
	return m.tape.last(limit)
}

func (m *Receipt) PrintsSince(seq int64) []models.PrintedDocument {
	m.mu.Lock()
	defer m.mu.Unlock()
	return m.tape.since(seq)
}

func (m *Receipt) State() map[string]any {
	m.mu.Lock()
	defer m.mu.Unlock()
	return map[string]any{
		"model":               "receipt_printer",
		"paper":               m.dev.paper,
		"cover_open":          m.dev.cover,
		"line_length":         m.lineLength,
		"barcodes":            m.barcodes,
		"documents":           m.documents,
		"drawer_opens":        m.drawerOpens,
		"drawer_opened_at":    m.drawerAt,
		"drawer_open":         m.drawer.IsOpen(m.opts.Now()),
		"drawer_auto_close_s": int(m.drawer.AutoClose / time.Second),
	}
}

// ReceiptStateKeys — ключи SetState принтера чеков (docs/models/printers.md).
var ReceiptStateKeys = []string{"paper", "cover_open", "line_length", "barcodes", "drawer_open", "drawer_auto_close_s"}

func (m *Receipt) SetState(patch map[string]any) error {
	m.mu.Lock()
	defer m.mu.Unlock()
	dev, lineLength, barcodes, drawer := m.dev, m.lineLength, m.barcodes, m.drawer
	now := m.opts.Now()
	keys := make([]string, 0, len(patch))
	for k := range patch {
		keys = append(keys, k)
	}
	sort.Strings(keys)
	for _, k := range keys {
		v := patch[k]
		var err error
		switch k {
		case "paper":
			dev.paper, err = asBool(k, v)
		case "cover_open":
			dev.cover, err = asBool(k, v)
		case "barcodes":
			barcodes, err = asBool(k, v)
		case "line_length":
			var n float64
			if n, err = asNumber(k, v); err == nil {
				if n < 24 || n > 80 || n != float64(int(n)) {
					err = fmt.Errorf("%s: целое от 24 до 80", k)
				}
				lineLength = int(n)
			}
		default:
			if ok, e := drawer.Set(k, v, now); ok {
				err = e
			} else {
				err = fmt.Errorf("неизвестный ключ %q; допустимы: %s", k, strings.Join(ReceiptStateKeys, ", "))
			}
		}
		if err != nil {
			return err
		}
	}
	m.dev, m.lineLength, m.barcodes, m.drawer = dev, lineLength, barcodes, drawer
	return nil
}
