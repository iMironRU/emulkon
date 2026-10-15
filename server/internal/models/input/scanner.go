package input

import (
	"context"
	"fmt"
	"math/rand/v2"
	"strings"
	"time"

	"emulkon/internal/models"
)

// GS — разделитель групп GS1 (ASCII 29) в коде маркировки DataMatrix.
const GS = "\x1d"

// Виды штрихкодов генератора (docs/models/input.md).
var ScanKinds = []string{"ean13", "ean8", "weight", "datamatrix", "code128", "qr"}

type scanRecord struct {
	Barcode string    `json:"barcode"`
	Kind    string    `json:"kind,omitempty"`
	Event   string    `json:"event,omitempty"` // пусто — только клавиатура
	At      time.Time `json:"at"`
	// Keyboard — ввести с клавиатуры на рабочем месте (режим keyboard или both).
	Keyboard *models.KeyboardInput `json:"keyboard,omitempty"`
}

func (r scanRecord) KeyboardInput() *models.KeyboardInput { return r.Keyboard }

// Scanner — эмулятор сканера штрихкода: штрихкоды «сканируют» в панели управления.
type Scanner struct {
	base
	rnd   *rand.Rand
	scans int
	last  []scanRecord // последние, новые первыми
	lib   []Barcode    // библиотека: свои штрихкоды и коды маркировки
}

// ScannerFactory — фабрика для реестра.
func ScannerFactory(version string) models.Factory {
	return func(_ models.EquipmentType, _ string) models.Model { return NewScanner(Options{Version: version}) }
}

func NewScanner(opts Options) *Scanner {
	opts.defaults()
	m := &Scanner{base: base{opts: opts, title: "сканера штрихкода"}, rnd: rand.New(rand.NewPCG(uint64(time.Now().UnixNano()), 7))}
	m.reset()
	return m
}

func (m *Scanner) reset() {
	m.resetInput("enter")
	m.lib = []Barcode{}
	m.scans = 0
	m.last = nil
}

func (m *Scanner) Reset() {
	m.mu.Lock()
	m.reset()
	m.mu.Unlock()
}

func (m *Scanner) Description() models.Description {
	return m.base.Description("ЭМУЛЯТОР: сканер штрихкода (Эмулкон)",
		"Виртуальный сканер Эмулкон: штрихкоды и коды маркировки сканируют в панели управления.")
}

func (m *Scanner) Handle(_ context.Context, method string, params []any) models.Result {
	return m.handle(method, params)
}

// Act — scan: {"barcode": "…"} или {"kind": "ean13"} (сгенерировать).
func (m *Scanner) Act(name string, args map[string]any) (any, error) {
	if name != "scan" {
		return nil, models.ErrUnknownAction
	}
	code, err := strArg(args, "barcode")
	if err != nil {
		return nil, err
	}
	kind, err := strArg(args, "kind")
	if err != nil {
		return nil, err
	}
	m.mu.Lock()
	if code == "" {
		if code, err = m.generate(kind); err != nil {
			m.mu.Unlock()
			return nil, err
		}
	} else {
		kind = ""
	}
	if strings.TrimSpace(code) == "" || len(code) > 4096 {
		m.mu.Unlock()
		return nil, fmt.Errorf("barcode: непустая строка до 4096 символов")
	}
	e := m.event("Barcode", code)
	emit, kb := m.deliver(e, code)
	rec := scanRecord{Barcode: code, Kind: kind, At: m.opts.Now(), Keyboard: kb}
	if emit {
		rec.Event = e.Name
	}
	m.scans++
	m.last = append([]scanRecord{rec}, m.last...)
	if len(m.last) > 20 {
		m.last = m.last[:20]
	}
	m.mu.Unlock()
	if emit {
		m.emit(e)
	}
	return rec, nil
}

func (m *Scanner) digits(n int) string {
	var b strings.Builder
	for i := 0; i < n; i++ {
		b.WriteByte(byte('0' + m.rnd.IntN(10)))
	}
	return b.String()
}

func (m *Scanner) generate(kind string) (string, error) {
	switch kind {
	case "ean13":
		s := "46" + m.digits(10)
		return s + checkDigit(s), nil
	case "ean8":
		s := "46" + m.digits(5)
		return s + checkDigit(s), nil
	case "weight": // весовой: префикс 21, код товара 5, вес в граммах 5
		s := "21" + m.digits(5) + fmt.Sprintf("%05d", 100+m.rnd.IntN(4900))
		return s + checkDigit(s), nil
	case "datamatrix": // код маркировки: 01 GTIN-14, 21 серийный номер, GS, 93 код проверки
		g := "46" + m.digits(10)
		const alnum = "ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz0123456789"
		var serial, crypto strings.Builder
		for i := 0; i < 13; i++ {
			serial.WriteByte(alnum[m.rnd.IntN(len(alnum))])
		}
		for i := 0; i < 4; i++ {
			crypto.WriteByte(alnum[m.rnd.IntN(len(alnum))])
		}
		return "010" + g + checkDigit(g) + "21" + serial.String() + GS + "93" + crypto.String(), nil
	case "code128":
		return "EMUL-" + m.digits(6), nil
	case "qr":
		return "https://imiron.ru/emulkon/q/" + m.digits(8), nil
	}
	return "", fmt.Errorf("kind: одно из %s или barcode", strings.Join(ScanKinds, ", "))
}

// checkDigit — контрольная цифра EAN/GTIN: веса 3 и 1 справа налево.
func checkDigit(s string) string {
	sum := 0
	for i := len(s) - 1; i >= 0; i-- {
		d := int(s[i] - '0')
		if (len(s)-1-i)%2 == 0 {
			d *= 3
		}
		sum += d
	}
	return fmt.Sprint((10 - sum%10) % 10)
}

func (m *Scanner) State() map[string]any {
	m.mu.Lock()
	defer m.mu.Unlock()
	last := append([]scanRecord{}, m.last...)
	return m.inputState(map[string]any{"model": "scanner", "event_format": m.format, "scans": m.scans, "last": last,
		"library": append([]Barcode{}, m.lib...)})
}

// SetState — event_format, input_mode, keyboard_suffix, keyboard_delay_ms (docs/models/input.md).
func (m *Scanner) SetState(patch map[string]any) error {
	m.mu.Lock()
	defer m.mu.Unlock()
	format, mode, suffix, delay, lib := m.format, m.mode, m.suffix, m.delayMS, m.lib
	for k, v := range patch {
		var err error
		if ok, e := m.setInputKey(k, v, &mode, &suffix, &delay); ok {
			err = e
		} else if k == "event_format" {
			format, err = setFormat(v)
		} else if k == "library" {
			lib, err = parseLibrary(v, func(b Barcode) string { return b.Title }, func(b Barcode) error {
				if strings.TrimSpace(b.Barcode) == "" || len(b.Barcode) > 4096 {
					return fmt.Errorf("barcode: непустая строка до 4096 символов")
				}
				return nil
			})
		} else {
			err = fmt.Errorf("неизвестный ключ %q; допустимы: event_format, input_mode, keyboard_suffix, keyboard_delay_ms, library", k)
		}
		if err != nil {
			return err
		}
	}
	m.format, m.mode, m.suffix, m.delayMS, m.lib = format, mode, suffix, delay, lib
	return nil
}
