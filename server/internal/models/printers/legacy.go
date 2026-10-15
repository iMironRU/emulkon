package printers

// Принтер чеков старой схемы (требования до 4.3, БПО УТ 10.3 — docs/models/printers.md, «Старая схема»):
// документ собирается по строкам между ОткрытьЧек и ЗакрытьЧек и печатается так же, как
// PrintTextDocument. ОтменитьЧек выбрасывает собранное.

import (
	"fmt"
	"strings"

	"emulkon/internal/models"
)

// legacyCheck — открытый чек: строки до ЗакрытьЧек.
type legacyCheck struct {
	lines  []string
	refund bool
}

// legacy — методы старой схемы; ok=false — метод не из неё.
func (m *Receipt) legacy(method string, a models.Args, out []any) (ok bool, err error) {
	switch method {
	case "OpenCheck":
		return true, m.openCheck(a, out)
	case "PrintFiscalString":
		return true, m.addLine(a, 7, func() ([]string, error) {
			name, _ := a.String(1)
			qty, _ := a.Float(2)
			price, _ := a.Float(3)
			sum, _ := a.Float(4)
			return append(wrap(name, m.lineLength), fmt.Sprintf("%10.3f x %.2f = %.2f", qty, price, sum)), nil
		})
	case "PrintNonFiscalString":
		return true, m.addLine(a, 2, func() ([]string, error) {
			text, _ := a.String(1)
			return wrap(text, m.lineLength), nil
		})
	case "PrintBarCode":
		return true, m.addLine(a, 3, func() ([]string, error) {
			t, _ := a.String(1)
			v, _ := a.String(2)
			if !m.barcodes {
				return nil, fail(ErrBarcodeType, "%q: принтер не печатает штрихкоды", t)
			}
			if !barcodeKnown(t) {
				return nil, fail(ErrBarcodeType, "%q", t)
			}
			var lines []string
			for _, l := range wrap("["+strings.ToUpper(t)+": "+v+"]", m.lineLength) {
				lines = append(lines, center(l, m.lineLength))
			}
			return lines, nil
		})
	case "CloseCheck":
		return true, m.closeCheck(a)
	case "CancelCheck":
		if err := a.Need(1); err != nil {
			return true, err
		}
		m.open = nil
		return true, nil
	}
	return false, nil
}

// ОткрытьЧек(0 ИД, 1 Фискальный, 2 Возврат, 3 —, 4 НомерЧека out, 5 НомерСмены out).
func (m *Receipt) openCheck(a models.Args, out []any) error {
	if err := a.Need(6); err != nil {
		return err
	}
	if m.open != nil {
		return fail(ErrCheckState, "чек уже открыт")
	}
	if err := m.dev.ready(); err != nil {
		return err
	}
	refund, _ := a.Bool(2)
	m.open = &legacyCheck{refund: refund}
	out[4] = int64(m.documents + 1)
	out[5] = int64(0) // смен у нефискального принтера нет
	return nil
}

func (m *Receipt) addLine(a models.Args, n int, lines func() ([]string, error)) error {
	if err := a.Need(n); err != nil {
		return err
	}
	if m.open == nil {
		return fail(ErrCheckState, "чек не открыт")
	}
	l, err := lines()
	if err != nil {
		return err
	}
	m.open.lines = append(m.open.lines, l...)
	return nil
}

// ЗакрытьЧек(0 ИД, 1 Наличные, 2–4 Безналичные 1–3).
func (m *Receipt) closeCheck(a models.Args) error {
	if err := a.Need(5); err != nil {
		return err
	}
	if m.open == nil {
		return fail(ErrCheckState, "чек не открыт")
	}
	if err := m.dev.ready(); err != nil {
		return err
	}
	w := m.lineLength
	lines := []string{center(emuMark, w)}
	if m.open.refund {
		lines = append(lines, center("ВОЗВРАТ", w))
	}
	lines = append(lines, m.open.lines...)
	for i, name := range []string{"НАЛИЧНЫМИ", "БЕЗНАЛИЧНЫМИ 1", "БЕЗНАЛИЧНЫМИ 2", "БЕЗНАЛИЧНЫМИ 3"} {
		if v, _ := a.Float(i + 1); v != 0 {
			lines = append(lines, fmt.Sprintf("%s %*.2f", name, w-len([]rune(name))-1, v))
		}
	}
	now := m.opts.Now()
	lines = append(lines, center(now.Format("02.01.2006 15:04"), w), center(emuMark, w))
	m.tape.add(models.PrintedDocument{Kind: "text", Time: now, Lines: lines})
	m.documents++
	m.open = nil
	return nil
}
