package kkt

import (
	"encoding/base64"
	"fmt"
	"strconv"
	"strings"
	"time"
	"unicode/utf8"

	"emulkon/internal/models"
)

// Печать на «ленту»: текст шириной lineLength символов. Каждый документ помечен
// как эмуляторный (ARCHITECTURE §10) — его невозможно спутать с настоящим чеком.

const emuMark = "*** ЭМУЛЯТОР · НЕ ФИСКАЛЬНЫЙ ***"

var opNames = map[int]string{OpIncome: "ПРИХОД", OpIncomeReturn: "ВОЗВРАТ ПРИХОДА", OpExpense: "РАСХОД", OpExpenseReturn: "ВОЗВРАТ РАСХОДА"}

var taxNames = map[int]string{0: "ОСН", 1: "УСН ДОХОД", 2: "УСН ДОХОД-РАСХОД", 3: "ЕНВД", 4: "ЕСХН", 5: "ПСН"}

func money(v float64) string { return strconv.FormatFloat(Kopecks(v), 'f', 2, 64) }

func (m *Model) width() int {
	if m.st.lineLength < 20 {
		return defaultLineLength
	}
	return m.st.lineLength
}

func (m *Model) center(s string) string {
	n := utf8.RuneCountInString(s)
	if n >= m.width() {
		return s
	}
	return strings.Repeat(" ", (m.width()-n)/2) + s
}

// lr — строка «слева … справа», выровненная по ширине ленты.
func (m *Model) lr(left, right string) string {
	gap := m.width() - utf8.RuneCountInString(left) - utf8.RuneCountInString(right)
	if gap < 1 {
		return left + " " + right
	}
	return left + strings.Repeat(" ", gap) + right
}

// wrap переносит длинную строку по ширине ленты (требования: длинные строки переносятся).
func (m *Model) wrap(s string) []string {
	w := m.width()
	r := []rune(s)
	if len(r) <= w {
		return []string{s}
	}
	var out []string
	for len(r) > w {
		out = append(out, string(r[:w]))
		r = r[w:]
	}
	if len(r) > 0 {
		out = append(out, string(r))
	}
	return out
}

func (m *Model) rule() string { return strings.Repeat("-", m.width()) }

func (m *Model) header() []string {
	r := m.st.reg
	lines := []string{m.center(emuMark)}
	for _, s := range []string{r.CompanyName, r.SaleAddress, r.SaleLocation} {
		if s != "" {
			for _, l := range m.wrap(s) {
				lines = append(lines, m.center(l))
			}
		}
	}
	if r.INN != "" {
		lines = append(lines, m.center("ИНН "+r.INN))
	}
	return append(lines, m.rule())
}

func (m *Model) footer(doc int, fp string, now time.Time) []string {
	r := m.st.reg
	lines := []string{
		m.rule(),
		m.lr("РН ККТ", r.KKTNumber),
		m.lr("ЗН ККТ", r.KKTSerialNumber),
		m.lr("ФН", r.FNSerialNumber),
		m.lr("ФД", strconv.Itoa(doc)),
	}
	if fp != "" {
		lines = append(lines, m.lr("ФП", fp))
	}
	return append(lines, m.lr("ДАТА", now.Format("02.01.2006 15:04")), m.center(emuMark))
}

// fiscalReport — фискальный отчёт (открытие/закрытие смены, регистрация, расчёты).
func (m *Model) fiscalReport(title string, doc int, now time.Time, body []string) models.PrintedDocument {
	lines := append(m.header(), m.center(title))
	lines = append(lines, body...)
	lines = append(lines, m.footer(doc, m.fiscalSign(doc, now, 0), now)...)
	kinds := map[string]string{
		"ОТЧЁТ ОБ ОТКРЫТИИ СМЕНЫ": "open_shift", "ОТЧЁТ О ЗАКРЫТИИ СМЕНЫ": "close_shift",
		"ОТЧЁТ О ТЕКУЩЕМ СОСТОЯНИИ РАСЧЁТОВ": "settlements",
	}
	kind := kinds[title]
	if kind == "" {
		kind = "fn_report"
	}
	return models.PrintedDocument{Kind: kind, Number: doc, Time: now, Lines: lines}
}

// nonFiscal — нефискальный документ (X-отчёт, внесение, текст).
func (m *Model) nonFiscal(kind, title string, now time.Time, body []string) models.PrintedDocument {
	lines := []string{m.center(emuMark)}
	if title != "" {
		lines = append(lines, m.center(title))
	}
	lines = append(lines, body...)
	lines = append(lines, m.lr("ДАТА", now.Format("02.01.2006 15:04")), m.center("НЕФИСКАЛЬНЫЙ ДОКУМЕНТ"))
	return models.PrintedDocument{Kind: kind, Time: now, Lines: lines}
}

func (m *Model) shiftTotalsLines(cashier string) []string {
	st := &m.st
	lines := []string{
		"СМЕНА: " + strconv.Itoa(st.shiftNumber),
		"КАССИР: " + cashier,
		m.lr("ЧЕКОВ ЗА СМЕНУ", strconv.Itoa(st.checksInShift)),
	}
	for i, c := range st.counters {
		if c.CheckCount == 0 && c.CorrectionCheckCount == 0 {
			continue
		}
		lines = append(lines, m.lr(opNames[i+1], fmt.Sprintf("%d / %s", c.CheckCount, money(float64(c.TotalChecksAmount)))))
		if c.CorrectionCheckCount > 0 {
			lines = append(lines, m.lr("  КОРРЕКЦИЯ", fmt.Sprintf("%d / %s", c.CorrectionCheckCount, money(float64(c.TotalCorrectionChecksAmount)))))
		}
	}
	return append(lines,
		m.lr("НАЛИЧНЫХ В КАССЕ", money(st.cash)),
		m.lr("НЕПЕРЕДАННЫХ ФД", strconv.Itoa(len(st.backlog))),
	)
}

// positionLines — позиции в порядке печати; fiscal — печатать фискальные строки.
func (m *Model) positionLines(p Positions, fiscal bool) []string {
	var lines []string
	for _, it := range p.Items {
		switch {
		case it.Text != nil:
			lines = append(lines, m.wrap(it.Text.Text)...)
		case it.Barcode != nil:
			v := it.Barcode.ValueBase64
			if b, err := base64.StdEncoding.DecodeString(v); err == nil {
				v = string(b)
			}
			for _, l := range m.wrap("[" + strings.ToUpper(it.Barcode.Type) + ": " + v + "]") {
				lines = append(lines, m.center(l))
			}
		case it.Fiscal != nil && fiscal:
			f := it.Fiscal
			name := f.Name
			if f.MarkingCode != "" {
				name = "[М] " + name
			}
			lines = append(lines, m.wrap(name)...)
			lines = append(lines, m.lr(fmt.Sprintf("  %s x %s", qty(float64(f.Quantity)), money(float64(f.PriceWithDiscount))),
				"="+money(float64(f.AmountWithDiscount))))
			if f.VATRate == "none" {
				lines = append(lines, "  БЕЗ НДС")
			} else if f.VATAmount != nil {
				lines = append(lines, m.lr("  НДС "+f.VATRate+"%", money(float64(*f.VATAmount))))
			}
		}
	}
	return lines
}

func qty(v float64) string { return strconv.FormatFloat(v, 'f', -1, 64) }

// receiptLines — кассовый чек или чек коррекции; qr — строка QR-кода чека в формате ФНС.
func (m *Model) receiptLines(c *CheckPackage, correction bool, doc int, fp string, now time.Time, total, change float64) (lines []string, qr string) {
	p := c.Parameters
	title := "КАССОВЫЙ ЧЕК"
	if correction {
		title = "ЧЕК КОРРЕКЦИИ"
	}
	lines = append(m.header(),
		m.center(title),
		m.center(opNames[p.OperationType]),
		m.lr("СМЕНА "+strconv.Itoa(m.st.shiftNumber), "ЧЕК "+strconv.Itoa(m.st.checksInShift)),
	)
	if p.CashierName != "" {
		lines = append(lines, m.wrap("КАССИР: "+p.CashierName)...)
	}
	if correction && p.CorrectionData != nil {
		cd := p.CorrectionData
		kind := "САМОСТОЯТЕЛЬНО"
		if cd.Type == 1 {
			kind = "ПО ПРЕДПИСАНИЮ " + cd.Number
		}
		lines = append(lines, "КОРРЕКЦИЯ: "+kind)
		if !cd.Date.IsZero() {
			lines = append(lines, "ДАТА РАСЧЁТА: "+cd.Date.Format("02.01.2006"))
		}
	}
	lines = append(lines, m.rule())
	lines = append(lines, m.positionLines(c.Positions, true)...)
	lines = append(lines, m.rule(), m.lr("ИТОГ", "="+money(total)))
	pay := c.Payments
	for _, x := range []struct {
		name string
		v    Number
	}{{"НАЛИЧНЫМИ", pay.Cash}, {"БЕЗНАЛИЧНЫМИ", pay.ElectronicPayment}, {"ПРЕДОПЛАТА (АВАНС)", pay.PrePayment},
		{"ПОСТОПЛАТА (КРЕДИТ)", pay.PostPayment}, {"ВСТРЕЧНОЕ ПРЕДОСТАВЛЕНИЕ", pay.Barter}} {
		if x.v != 0 {
			lines = append(lines, m.lr(x.name, "="+money(float64(x.v))))
		}
	}
	if change > 0 {
		lines = append(lines, m.lr("СДАЧА", "="+money(change)))
	}
	lines = append(lines, m.lr("СНО", taxNames[p.TaxationSystem]))
	if p.CustomerEmail != "" {
		lines = append(lines, m.lr("ЭЛ. АДР. ПОКУПАТЕЛЯ", p.CustomerEmail))
	}
	lines = append(lines, m.lr("САЙТ ФНС", m.st.reg.FNSURL))
	lines = append(lines, m.footer(doc, fp, now)...)
	// QR-код чека по формату ФНС (t, s, fn, i, fp, n) — рисуется под текстом (PrintedDocument.QR):
	// его можно отсканировать телефоном и увидеть параметры чека.
	qr = fmt.Sprintf("t=%s&s=%s&fn=%s&i=%d&fp=%s&n=%d",
		now.Format("20060102T1504"), money(total), m.st.reg.FNSerialNumber, doc, fp, p.OperationType)
	return lines, qr
}
