package acquiring

import (
	"context"
	"encoding/json"
	"encoding/xml"
	"fmt"
	"strings"
	"sync"
	"testing"
	"time"

	"emulkon/internal/models"
)

type harness struct {
	t   *testing.T
	m   *Model
	mu  sync.Mutex
	evs []string // события экрана: open:<операция>, done:<действие>
}

func newHarness(t *testing.T, mode string) *harness {
	t.Helper()
	now := time.Date(2026, 10, 8, 15, 39, 41, 0, time.Local)
	h := &harness{t: t, m: New("acquiring-01", Options{Version: "test", Now: func() time.Time { return now }})}
	h.m.SetPromptHook(func(p models.Prompt, done *models.PromptResult) {
		h.mu.Lock()
		defer h.mu.Unlock()
		if done == nil {
			h.evs = append(h.evs, "open:"+p.Operation)
		} else {
			h.evs = append(h.evs, "done:"+done.Action)
		}
	})
	h.set(map[string]any{"card_mode": mode})
	return h
}

func (h *harness) set(patch map[string]any) {
	h.t.Helper()
	if err := h.m.SetState(patch); err != nil {
		h.t.Fatal(err)
	}
}

func (h *harness) call(method string, params ...any) models.Result {
	return h.m.Handle(context.Background(), method, params)
}

func opXML(attrs string) string {
	return `<?xml version="1.0" encoding="UTF-8"?><OperationParameters MerchantNumber="0" UseBiometrics="0" AuthorizationType="0" AmountCash="0" AmountOriginalTransaction="0" ElectronicCertificateAmount="0" ElectronicCertificateDiscount="0" OwnFundsAmount="0" ` + attrs + `/>`
}

// op — операция с XML-параметрами: возвращает итог, разобранный OperationOut и слип.
func (h *harness) op(method, attrs string) (models.Result, OperationOut, string) {
	h.t.Helper()
	r := h.call(method, "emulkon-1", opXML(attrs), "", "")
	var out OperationOut
	if b, _ := r.Return.(bool); b {
		s, _ := r.Params[2].(string)
		if err := xml.Unmarshal([]byte(s), &out); err != nil {
			h.t.Fatalf("%s: выходной XML: %v\n%s", method, err, s)
		}
	}
	slip, _ := r.Params[3].(string)
	return r, out, slip
}

func ok(r models.Result) bool { b, _ := r.Return.(bool); return b }

func TestTerminalParameters(t *testing.T) {
	h := newHarness(t, modeApprove)
	r := h.call("TerminalParamters", "emulkon-1", "")
	s, _ := r.Params[1].(string)
	for _, want := range []string{`TerminalID="EMU`, `PrintSlipOnTerminal="false"`, `CashWithdrawal="true"`,
		`PartialCancellation="true"`, `ListCardTransactions="true"`, `ElectronicCertificates="false"`, `OperationStatus="true"`} {
		if !strings.Contains(s, want) {
			t.Errorf("TerminalParameters без %s:\n%s", want, s)
		}
	}
}

func TestPayApproveMode(t *testing.T) {
	h := newHarness(t, modeApprove)
	r, out, slip := h.op("Pay", `RequestID="0000ABCD" Amount="111"`)
	if !ok(r) {
		t.Fatalf("оплата: %v %s", r.ErrorCode, r.ErrorText)
	}
	if out.RRNCode == "" || len(out.RRNCode) != 12 || len(out.AuthorizationCode) != 6 || out.CardNumber != "220070******4417" ||
		out.PaymentSystemIdentifier != "22" || float64(out.Amount) != 111 || out.ReceiptNumber != "0001" || out.RequestID != "0000ABCD" {
		t.Fatalf("выходные параметры: %+v", out)
	}
	if !strings.Contains(slip, slipMark) || strings.Count(slip, "[отрезка]") != 1 || !strings.Contains(slip, "ОДОБРЕНО") ||
		!strings.Contains(slip, out.RRNCode) {
		t.Fatalf("слип:\n%s", slip)
	}
	if len(h.evs) != 0 {
		t.Fatalf("в режиме approve экран не открывается: %v", h.evs)
	}
	ops := h.m.State()["operations"].([]map[string]any)
	if len(ops) != 1 || ops[0]["rrn"] != out.RRNCode {
		t.Fatalf("пакет дня: %v", ops)
	}
}

// waitPrompt ждёт, пока экран откроет запрос.
func waitPrompt(t *testing.T, m *Model) models.Prompt {
	t.Helper()
	for i := 0; i < 200; i++ {
		if p, ok := m.State()["prompt"].(models.Prompt); ok {
			return p
		}
		time.Sleep(5 * time.Millisecond)
	}
	t.Fatal("экран не открыл запрос")
	return models.Prompt{}
}

func TestPayInteractiveCard(t *testing.T) {
	h := newHarness(t, modeInteractive)
	done := make(chan models.Result, 1)
	go func() { done <- h.call("Pay", "emulkon-1", opXML(`Amount="250.5"`), "", "") }()

	p := waitPrompt(t, h.m)
	if p.Operation != "Pay" || p.Title != "ОПЛАТА" || p.Amount != 250.5 || len(p.Actions) != 5 || p.DefaultCard != "mir" {
		t.Fatalf("запрос экрана: %+v", p)
	}
	if err := h.m.Respond("чужой", models.PromptAnswer{Action: actCard, Card: ""}); err != models.ErrPromptGone {
		t.Fatalf("чужой запрос: %v", err)
	}
	if err := h.m.Respond(p.ID, models.PromptAnswer{Action: "dance", Card: ""}); err == nil {
		t.Fatal("неизвестное действие принято")
	}
	if err := h.m.Respond(p.ID, models.PromptAnswer{Action: actCard, Card: "visa"}); err != nil {
		t.Fatal(err)
	}
	if err := h.m.Respond(p.ID, models.PromptAnswer{Action: actCard, Card: "visa"}); err != models.ErrPromptGone {
		t.Fatalf("второй ответ: %v", err)
	}
	r := <-done
	if !ok(r) || !strings.Contains(r.Params[2].(string), `PaymentSystemIdentifier="4"`) {
		t.Fatalf("оплата картой VISA: %v %v %s", r.Return, r.ErrorText, r.Params[2])
	}
	h.mu.Lock()
	defer h.mu.Unlock()
	if strings.Join(h.evs, ",") != "open:Pay,done:card" {
		t.Fatalf("события экрана: %v", h.evs)
	}
	if h.m.State()["prompt"] != nil {
		t.Fatal("запрос не снят после ответа")
	}
}

func TestPayInteractiveDeclines(t *testing.T) {
	for _, c := range []struct {
		action string
		code   int
		text   string
	}{
		{actInsufficient, ErrInsufficient, "недостаточно средств"},
		{actDecline, ErrDeclined, "отклонена банком"},
		{actWrongPIN, ErrWrongPIN, "Неверный ПИН"},
		{actCancel, ErrCancelled, "ПРЕРВАНА"},
	} {
		h := newHarness(t, modeInteractive)
		done := make(chan models.Result, 1)
		go func() { done <- h.call("Pay", "emulkon-1", opXML(`Amount="10"`), "", "") }()
		p := waitPrompt(t, h.m)
		if err := h.m.Respond(p.ID, models.PromptAnswer{Action: c.action, Card: ""}); err != nil {
			t.Fatal(err)
		}
		r := <-done
		slip, _ := r.Params[3].(string)
		if ok(r) || r.ErrorCode != c.code || !strings.Contains(slip+r.ErrorText, c.text) {
			t.Errorf("%s: %v %d %q\n%s", c.action, r.Return, r.ErrorCode, r.ErrorText, slip)
		}
		if n := len(h.m.State()["operations"].([]map[string]any)); n != 0 {
			t.Errorf("%s: отказ попал в пакет дня", c.action)
		}
	}
}

func TestPayInteractiveTimeoutAndReset(t *testing.T) {
	h := newHarness(t, modeInteractive)
	h.m.mu.Lock()
	h.m.st.cardTimeout = 30 * time.Millisecond
	h.m.mu.Unlock()
	r, _, _ := h.op("Pay", `Amount="10"`)
	if ok(r) || r.ErrorCode != ErrTimeout {
		t.Fatalf("таймаут: %v %d", r.Return, r.ErrorCode)
	}

	h = newHarness(t, modeInteractive)
	done := make(chan models.Result, 1)
	go func() { done <- h.call("Pay", "emulkon-1", opXML(`Amount="10"`), "", "") }()
	waitPrompt(t, h.m)
	h.m.Reset() // сброс посреди ожидания: команда завершается, а не висит до таймаута
	select {
	case r := <-done:
		if ok(r) || r.ErrorCode != ErrCancelled {
			t.Fatalf("сброс: %v %d", r.Return, r.ErrorCode)
		}
	case <-time.After(2 * time.Second):
		t.Fatal("команда не завершилась после сброса")
	}
}

func TestCancelAndReturn(t *testing.T) {
	h := newHarness(t, modeApprove)
	_, pay, _ := h.op("Pay", `Amount="300"`)

	if r, _, _ := h.op("CancelPayment", `Amount="10" RRNCode="000000000000"`); ok(r) || r.ErrorCode != ErrNotFound {
		t.Fatalf("отмена неизвестной: %d", r.ErrorCode)
	}
	if r, _, _ := h.op("ReturnPayment", fmt.Sprintf(`Amount="301" RRNCode="%s"`, pay.RRNCode)); ok(r) || r.ErrorCode != ErrAmountExceeded {
		t.Fatalf("возврат больше оплаты: %d", r.ErrorCode)
	}
	r, ret, _ := h.op("ReturnPayment", fmt.Sprintf(`Amount="100" RRNCode="%s"`, pay.RRNCode))
	if !ok(r) || ret.RRNCode == pay.RRNCode {
		t.Fatalf("возврат: %v %s", r.ErrorText, ret.RRNCode)
	}
	// частичная отмена остатка 50 из 200
	r, cancel, _ := h.op("CancelPayment", fmt.Sprintf(`Amount="50" AmountOriginalTransaction="300" RRNCode="%s" AuthorizationCode="%s"`,
		pay.RRNCode, pay.AuthorizationCode))
	if !ok(r) || cancel.RRNCode != pay.RRNCode || cancel.AuthorizationCode != pay.AuthorizationCode {
		t.Fatalf("частичная отмена: %v %+v", r.ErrorText, cancel)
	}
	h.set(map[string]any{"partial_cancellation": false})
	if r, _, _ := h.op("CancelPayment", fmt.Sprintf(`Amount="10" RRNCode="%s"`, pay.RRNCode)); ok(r) || r.ErrorCode != ErrNotSupported {
		t.Fatalf("частичная отмена выключена: %d", r.ErrorCode)
	}
	// полная отмена остатка, затем повторная
	if r, _, _ := h.op("CancelPayment", fmt.Sprintf(`Amount="150" RRNCode="%s"`, pay.RRNCode)); !ok(r) {
		t.Fatalf("отмена остатка: %s", r.ErrorText)
	}
	if r, _, _ := h.op("CancelPayment", fmt.Sprintf(`Amount="150" RRNCode="%s"`, pay.RRNCode)); ok(r) || r.ErrorCode != ErrAlreadyVoided {
		t.Fatalf("повторная отмена: %d", r.ErrorCode)
	}
	if r, _, _ := h.op("ReturnPayment", fmt.Sprintf(`Amount="1" RRNCode="%s"`, pay.RRNCode)); ok(r) || r.ErrorCode != ErrAlreadyVoided {
		t.Fatalf("возврат по отменённой: %d", r.ErrorCode)
	}
	// возврат по чужому дню (RRN не в пакете) проходит
	if r, _, _ := h.op("ReturnPayment", `Amount="5" RRNCode="512345678901"`); !ok(r) {
		t.Fatalf("возврат по операции другого дня: %s", r.ErrorText)
	}
}

func TestPreauthorisation(t *testing.T) {
	h := newHarness(t, modeApprove)
	_, auth, _ := h.op("Authorisation", `Amount="500"`)
	if r, _, _ := h.op("AuthConfirmation", fmt.Sprintf(`Amount="600" RRNCode="%s"`, auth.RRNCode)); ok(r) || r.ErrorCode != ErrAmountExceeded {
		t.Fatalf("списание больше блокировки: %d", r.ErrorCode)
	}
	if r, done, _ := h.op("AuthConfirmation", fmt.Sprintf(`Amount="450" RRNCode="%s"`, auth.RRNCode)); !ok(r) || float64(done.Amount) != 450 {
		t.Fatalf("завершение: %s %+v", r.ErrorText, done)
	}
	if r, _, _ := h.op("CancelAuthorisation", fmt.Sprintf(`Amount="0" RRNCode="%s"`, auth.RRNCode)); ok(r) || r.ErrorCode != ErrAlreadyVoided {
		t.Fatalf("отмена завершённой: %d", r.ErrorCode)
	}
	_, auth2, _ := h.op("Authorisation", `Amount="70"`)
	if r, _, _ := h.op("CancelAuthorisation", fmt.Sprintf(`Amount="0" RRNCode="%s"`, auth2.RRNCode)); !ok(r) {
		t.Fatalf("отмена преавторизации: %s", r.ErrorText)
	}
}

func TestReversalStatusTableSettlement(t *testing.T) {
	h := newHarness(t, modeApprove)
	if r := h.call("EmergencyReversal", "emulkon-1"); ok(r) || r.ErrorCode != ErrNotFound {
		t.Fatalf("аварийная отмена без операций: %d", r.ErrorCode)
	}
	h.op("Pay", `RequestID="00000001" Amount="100"`)
	_, second, _ := h.op("Pay", `RequestID="00000002" Amount="40"`)
	if r := h.call("EmergencyReversal", "emulkon-1"); !ok(r) {
		t.Fatalf("аварийная отмена: %s", r.ErrorText)
	}
	if r, _, _ := h.op("CancelPayment", fmt.Sprintf(`Amount="40" RRNCode="%s"`, second.RRNCode)); r.ErrorCode != ErrAlreadyVoided {
		t.Fatalf("аварийно отменённую отменяем ещё раз: %d", r.ErrorCode)
	}

	r := h.call("OperationStatus", "emulkon-1", "00000001", "", int64(0))
	if !ok(r) || r.Params[3] != int64(1) || !strings.Contains(r.Params[2].(string), `Amount="100"`) {
		t.Fatalf("статус операции: %v", r.Params)
	}
	if r := h.call("OperationStatus", "emulkon-1", "FFFFFFFF", "", int64(0)); !ok(r) || r.Params[3] != int64(0) {
		t.Fatalf("статус неизвестной: %v", r.Params)
	}

	r = h.call("GetOperationByCards", "emulkon-1", "")
	var table TableOperations
	if err := xml.Unmarshal([]byte(r.Params[1].(string)), &table); err != nil || len(table.Records) != 3 ||
		table.Records[2].TypeOperation != "CancelPayment" {
		t.Fatalf("операции по картам: %v %+v", err, table)
	}

	r = h.call("Settlement", "emulkon-1", "")
	slip, _ := r.Params[1].(string)
	if !ok(r) || !strings.Contains(slip, "СВЕРКА ИТОГОВ") || !strings.Contains(slip, "100.00 RUB") {
		t.Fatalf("сверка: %v\n%s", r.ErrorText, slip)
	}
	st := h.m.State()
	if len(st["operations"].([]map[string]any)) != 0 || st["batch"] != 2 {
		t.Fatalf("после сверки пакет не закрыт: %v", st["batch"])
	}
}

func TestBankOfflineAndUnsupported(t *testing.T) {
	h := newHarness(t, modeInteractive)
	h.set(map[string]any{"bank_online": false})
	r, _, slip := h.op("Pay", `Amount="10"`)
	if ok(r) || r.ErrorCode != ErrBankOffline || !strings.Contains(slip, "Нет связи с банком") || len(h.evs) != 0 {
		t.Fatalf("без связи с банком: %d %v\n%s", r.ErrorCode, h.evs, slip)
	}
	for _, m := range []string{"PurchaseWithEnrollment", "PayCertificate", "ReturnCertificate"} {
		if r, _, _ := h.op(m, `Amount="10"`); ok(r) || r.ErrorCode != ErrNotSupported {
			t.Errorf("%s: %d", m, r.ErrorCode)
		}
	}
	if r, _, _ := h.op("Pay", `Amount="0"`); ok(r) || r.ErrorCode != ErrBadParams {
		t.Errorf("нулевая сумма: %d", r.ErrorCode)
	}
}

func TestCardParametersAndTape(t *testing.T) {
	h := newHarness(t, modeApprove)
	if r := h.call("GetCardParametrs", "emulkon-1", "", true, "", "", "", "", int64(0)); ok(r) || r.ErrorCode != ErrNotFound {
		t.Fatalf("параметры последней карты без операций: %d", r.ErrorCode)
	}
	h.set(map[string]any{"card": "mastercard", "print_slip_on_terminal": true})
	r := h.call("GetCardParametrs", "emulkon-1", "", false, "", "", "", "", int64(0))
	if !ok(r) || r.Params[3] != "555555******4444" || r.Params[6] != "MASTERCARD" || r.Params[7] != int64(1) ||
		len(r.Params[4].(string)) != 64 || len(r.Params[5].(string)) != 29 {
		t.Fatalf("параметры карты: %v", r.Params)
	}
	h.op("Pay", `Amount="12"`)
	if tape := h.m.Prints(10); len(tape) != 1 || tape[0].Kind != "slip" {
		t.Fatalf("слип на ленте терминала: %+v", tape)
	}
}

func TestSetStateTransactional(t *testing.T) {
	h := newHarness(t, modeApprove)
	if err := h.m.SetState(map[string]any{"bank_online": false, "card": "amex"}); err == nil {
		t.Fatal("неизвестная карта принята")
	}
	if !h.m.State()["bank_online"].(bool) {
		t.Fatal("ошибка в одном ключе, а другой применился")
	}
	if err := h.m.SetState(map[string]any{"nonsense": 1}); err == nil || !strings.Contains(err.Error(), "card_mode") {
		t.Fatalf("неизвестный ключ: %v", err)
	}
	if err := h.m.SetState(map[string]any{"card_timeout_s": float64(2)}); err == nil {
		t.Fatal("таймаут меньше 5 с принят")
	}
}

// Исход по сумме и тестовой карте — без экрана, даже в интерактивном режиме (docs/models/acquiring.md).
func TestAmountAndCardRules(t *testing.T) {
	h := newHarness(t, modeInteractive)
	// Правило копеек выключено по умолчанию: .62 не трогает операцию — она ждёт карту на экране.
	h.set(map[string]any{"card_mode": modeApprove})
	if r, _, _ := h.op("Pay", `Amount="100.62"`); !ok(r) {
		t.Fatalf("без правила копеек оплата проходит: %+v", r)
	}
	h.set(map[string]any{"card_mode": modeInteractive, "kopeck_rules": true, "decline_above": 1000.0})
	cases := []struct {
		attrs string
		code  int
	}{
		{`Amount="100.61"`, ErrDeclined},
		{`Amount="100.62"`, ErrInsufficient},
		{`Amount="100.63"`, ErrWrongPIN},
		{`Amount="100.64"`, ErrCancelled},
		{`Amount="100.65"`, ErrTimeout},
		{`Amount="1000.01"`, ErrDeclined},
		{`Amount="50" CardNumber="4000 0000 0000 9995"`, ErrInsufficient},
	}
	for _, c := range cases {
		r, _, _ := h.op("Pay", c.attrs)
		if ok(r) || r.ErrorCode != c.code {
			t.Errorf("%s: ждали отказ %d, получили %+v", c.attrs, c.code, r)
		}
	}
	// Обычная сумма в интерактивном режиме по-прежнему ждёт человека.
	done := make(chan models.Result, 1)
	go func() { done <- h.call("Pay", "emulkon-1", opXML(`Amount="100.50"`), "", "") }()
	p := waitPrompt(t, h.m)
	if err := h.m.Respond(p.ID, models.PromptAnswer{Action: actCard, Card: "visa"}); err != nil {
		t.Fatal(err)
	}
	if r := <-done; !ok(r) {
		t.Fatalf("обычная сумма — одобрено картой с экрана: %+v", r)
	}
	if err := h.m.SetState(map[string]any{"decline_above": -1.0}); err == nil {
		t.Fatal("отрицательный порог — ошибка")
	}
}

// Старая схема (БПО УТ 10.3): позиционные параметры, операции 4.3+ внутри.
func TestLegacyOperations(t *testing.T) {
	h := newHarness(t, "approve")
	r := h.call("AuthorizeSales", "emulkon-1", "", json.Number("111.5"), "", "", "", "")
	if !ok(r) {
		t.Fatalf("оплата: %d %s", r.ErrorCode, r.ErrorText)
	}
	card, rrn, auth, slip := r.Params[1].(string), r.Params[4].(string), r.Params[5].(string), r.Params[6].(string)
	if card == "" || rrn == "" || auth == "" || !strings.Contains(slip, "ЭМУЛЯТОР") {
		t.Fatalf("выход оплаты: карта %q, RRN %q, код %q", card, rrn, auth)
	}
	// Отмена по RRN исходной операции — тем же путём, что CancelPayment.
	if r := h.call("AuthorizeVoid", "emulkon-1", card, json.Number("111.5"), "", rrn, auth, ""); !ok(r) {
		t.Fatalf("отмена: %d %s", r.ErrorCode, r.ErrorText)
	}
	if r := h.call("AuthorizeVoid", "emulkon-1", card, json.Number("111.5"), "", rrn, auth, ""); ok(r) || r.ErrorCode != ErrAlreadyVoided {
		t.Fatalf("повторная отмена: %v %d", r.Return, r.ErrorCode)
	}
	// Отказ банка — слип отказа в позиции 6, ошибка как у Pay.
	h.set(map[string]any{"decline_above": 100.0})
	r = h.call("AuthorizeSales", "emulkon-1", "", json.Number("150"), "", "", "", "")
	if ok(r) || r.Params[6] == "" {
		t.Fatalf("отказ: %v, слип %q", r.Return, r.Params[6])
	}
	if r := h.call("AuthorizeSales", "emulkon-1", "", 1.0); ok(r) || r.ErrorCode != ErrBadParams {
		t.Fatalf("мало параметров: %v %d", r.Return, r.ErrorCode)
	}
}
