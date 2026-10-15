package acquiring

import (
	"fmt"
	"strings"
	"time"
	"unicode/utf8"

	"emulkon/internal/models/kkt"
)

// Слип — квитанция терминала (docs/models/acquiring.md). Ширина 32 символа: её без
// переносов печатает ККТ с лентой 42 и терминал с узким принтером.
const slipWidth = 32

// slipMark — шапка каждого слипа: эмулятор узнаваем (ARCHITECTURE §10).
const slipMark = "*** ЭМУЛЯТОР · НЕ БАНКОВСКИЙ ***"

// cutLine — разделитель документов в тексте слипа ([Т] раздел 3.9).
const cutLine = "[отрезка]"

func center(s string) string {
	n := utf8.RuneCountInString(s)
	if n >= slipWidth {
		return s
	}
	return strings.Repeat(" ", (slipWidth-n)/2) + s
}

func lr(left, right string) string {
	gap := slipWidth - utf8.RuneCountInString(left) - utf8.RuneCountInString(right)
	if gap < 1 {
		return left + " " + right
	}
	return left + strings.Repeat(" ", gap) + right
}

func money(v float64) string { return fmt.Sprintf("%.2f", kkt.Kopecks(v)) }

func (m *Model) header(at time.Time, receipt string) []string {
	lines := []string{
		slipMark,
		center(bankName),
		center(merchantName),
		lr("ТЕРМИНАЛ", m.terminalID),
		lr("МЕРЧАНТ", m.merchantID),
	}
	when := at.Format("02.01.06 15:04:05")
	if receipt != "" {
		lines = append(lines, lr("ЧЕК "+receipt, when))
	} else {
		lines = append(lines, lr("", when))
	}
	return append(lines, strings.Repeat("-", slipWidth))
}

func cardLine(c *cardInfo) string {
	if c == nil {
		return ""
	}
	return lr("КАРТА", c.Type+" "+c.Masked)
}

// approvedSlip — слип одобренной операции. Полный — две копии через [отрезка],
// короткий (ShortSlip) — одна, для печати в теле фискального чека.
func (m *Model) approvedSlip(op *operation) string {
	body := []string{center(titles[op.Type]), lr("СУММА", money(op.Amount)+" RUB")}
	if op.Cash > 0 {
		body = append(body, lr("ВЫДАЧА НАЛИЧНЫХ", money(op.Cash)+" RUB"))
	}
	body = append(body, cardLine(&op.Card),
		lr("КОД АВТОРИЗАЦИИ", op.AuthCode),
		lr("RRN", op.RRN))
	if op.Ref != "" && op.Ref != op.RRN {
		body = append(body, lr("ИСХОДНАЯ RRN", op.Ref))
	}
	body = append(body, strings.Repeat("-", slipWidth), center("ОДОБРЕНО"))

	var lines []string
	if m.st.shortSlip {
		lines = append([]string{slipMark, lr("ТЕРМИНАЛ", m.terminalID), lr("ЧЕК "+op.Receipt, op.At.Format("02.01.06 15:04"))}, body...)
	} else {
		head := m.header(op.At, op.Receipt)
		client := append(append(append([]string{}, head...), body...),
			center("ПОДПИСЬ КЛИЕНТА НЕ ТРЕБУЕТСЯ"), center("ЭКЗЕМПЛЯР КЛИЕНТА"))
		cashier := append(append(append([]string{}, head...), body...), center("ЭКЗЕМПЛЯР КАССИРА"))
		lines = append(append(client, cutLine), cashier...)
	}
	m.addTape("slip", tapeLines(lines), op.At)
	return strings.Join(lines, "\n")
}

// declineSlip — слип отказа: БПО забирает его и при False ([Т]: «текст квитанции отказа»).
func (m *Model) declineSlip(method string, amount float64, card *cardInfo, code int) string {
	now := m.opts.Now()
	lines := append(m.header(now, ""), center(titles[method]), lr("СУММА", money(amount)+" RUB"))
	if card != nil {
		lines = append(lines, cardLine(card))
	}
	verdict := "ОТКАЗ"
	if code == ErrCancelled || code == ErrTimeout {
		verdict = "ОПЕРАЦИЯ ПРЕРВАНА"
	}
	lines = append(lines, strings.Repeat("-", slipWidth), center(verdict))
	for _, w := range wrapWords(errorTexts[code]) {
		lines = append(lines, center(w))
	}
	m.addTape("slip", lines, now)
	return strings.Join(lines, "\n")
}

// settlementSlip — сверка итогов по пакету дня.
func (m *Model) settlementSlip(now time.Time) string {
	type total struct {
		n   int
		sum float64
	}
	totals := map[string]*total{}
	order := []string{"Pay", "PayWithCashWithdrawal", "ReturnPayment", "CancelPayment", "AuthConfirmation", "EmergencyReversal"}
	for _, k := range order {
		totals[k] = &total{}
	}
	net := 0.0
	for _, op := range m.st.ops {
		t, ok := totals[op.Type]
		if !ok {
			continue // преавторизации и их отмены деньги не двигают
		}
		t.n++
		t.sum += op.Amount
		switch op.Type {
		case "Pay", "PayWithCashWithdrawal", "AuthConfirmation":
			net += op.Amount
		default:
			net -= op.Amount
		}
	}
	lines := append(m.header(now, ""), center("СВЕРКА ИТОГОВ"), lr("ПАКЕТ", fmt.Sprint(m.st.batch)),
		strings.Repeat("-", slipWidth))
	for _, k := range order {
		t := totals[k]
		lines = append(lines, lr(titles[k], fmt.Sprintf("%d / %s", t.n, money(t.sum))))
	}
	lines = append(lines, strings.Repeat("-", slipWidth), lr("ИТОГО", money(net)+" RUB"), center("ИТОГИ СОВПАЛИ"))
	m.addTape("settlement", lines, now)
	return strings.Join(lines, "\n")
}

// tapeLines — для ленты разделитель документов показываем линией отреза.
func tapeLines(lines []string) []string {
	out := make([]string, 0, len(lines))
	for _, l := range lines {
		if l == cutLine {
			l = "✂ " + strings.Repeat("- ", (slipWidth-2)/2)
		}
		out = append(out, l)
	}
	return out
}

// wrapWords переносит текст по словам в ширину слипа.
func wrapWords(s string) []string {
	var out []string
	line := ""
	for _, w := range strings.Fields(s) {
		if line != "" && utf8.RuneCountInString(line)+1+utf8.RuneCountInString(w) > slipWidth {
			out = append(out, line)
			line = ""
		}
		if line != "" {
			line += " "
		}
		line += w
	}
	if line != "" {
		out = append(out, line)
	}
	return out
}
