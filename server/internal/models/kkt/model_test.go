package kkt

import (
	"context"
	"encoding/base64"
	"encoding/json"
	"fmt"
	"strings"
	"testing"
	"time"

	"emulkon/internal/models"
)

type harness struct {
	t   *testing.T
	m   *Model
	now time.Time
}

func newHarness(t *testing.T) *harness {
	h := &harness{t: t, now: time.Date(2026, 10, 8, 9, 0, 0, 0, time.Local)}
	h.m = New("kkt-01", Options{Version: "test", Now: func() time.Time { return h.now }})
	return h
}

func (h *harness) call(method string, params ...any) models.Result {
	h.t.Helper()
	return h.m.Handle(context.Background(), method, params)
}

func (h *harness) ok(method string, params ...any) models.Result {
	h.t.Helper()
	r := h.call(method, params...)
	if r.Return == false {
		h.t.Fatalf("%s: ожидался успех, ошибка %d %s", method, r.ErrorCode, r.ErrorText)
	}
	return r
}

func (h *harness) fails(code int, method string, params ...any) models.Result {
	h.t.Helper()
	r := h.call(method, params...)
	if r.Return != false || r.ErrorCode != code {
		h.t.Fatalf("%s: ожидалась ошибка %d, получено return=%v code=%d %s", method, code, r.Return, r.ErrorCode, r.ErrorText)
	}
	return r
}

func (h *harness) set(patch map[string]any) {
	h.t.Helper()
	if err := h.m.SetState(patch); err != nil {
		h.t.Fatal(err)
	}
}

func num(v any) json.Number { return json.Number(fmt.Sprint(v)) }

const input = `<?xml version="1.0" encoding="UTF-8"?><InputParameters><Parameters CashierName="Иванов И.И."/></InputParameters>`

func (h *harness) openShift() OutputParameters {
	h.t.Helper()
	r := h.ok("OpenShift", "h", input, "")
	var out OutputParameters
	if err := Unmarshal(r.Params[2].(string), &out); err != nil {
		h.t.Fatal(err)
	}
	return out
}

func (h *harness) status() ShiftStatus {
	h.t.Helper()
	r := h.ok("GetCurrentStatus", "h", input, "")
	var out OutputParameters
	if err := Unmarshal(r.Params[2].(string), &out); err != nil {
		h.t.Fatal(err)
	}
	return out.Parameters
}

// checkXML — пакет чека: позиции и оплаты как в БПО.
func checkXML(op, tax int, positions, payments string) string {
	return fmt.Sprintf(`<?xml version="1.0" encoding="UTF-8"?><CheckPackage><Parameters CashierName="Иванов И.И." OperationType="%d" TaxationSystem="%d"><AgentData/><VendorData/></Parameters><Positions>%s</Positions>%s</CheckPackage>`,
		op, tax, positions, payments)
}

const milk = `<FiscalString Name="Молоко" Quantity="2" PriceWithDiscount="89.9" AmountWithDiscount="179.8" VATRate="10" VATAmount="16.35" PaymentMethod="4" CalculationSubject="1"/>`
const bag = `<FiscalString Name="Пакет" Quantity="1" PriceWithDiscount="5" AmountWithDiscount="5" VATRate="none" PaymentMethod="4" CalculationSubject="1"/>`

func pay(cash, card float64) string {
	return fmt.Sprintf(`<Payments Cash="%v" ElectronicPayment="%v" PrePayment="0" PostPayment="0" Barter="0"/>`, cash, card)
}

func (h *harness) check(pkg string) DocumentResult {
	h.t.Helper()
	r := h.ok("ProcessCheck", "h", false, pkg, "")
	var out DocumentOutputParameters
	if err := Unmarshal(r.Params[3].(string), &out); err != nil {
		h.t.Fatal(err)
	}
	return out.Parameters
}

func TestDefaultRegistration(t *testing.T) {
	h := newHarness(t)
	r := h.ok("GetDataKKT", "h", "")
	var tp TableParametersKKT
	if err := Unmarshal(r.Params[1].(string), &tp); err != nil {
		t.Fatal(err)
	}
	if !bool(tp.Fiscal) || len(tp.KKTNumber) != 16 || tp.FFDVersionFN != "1.2" || !strings.HasPrefix(tp.KKTSerialNumber, "EMU") ||
		!strings.Contains(tp.TaxationSystems, "0") || tp.KKTModelID != "EMULKON" || !bool(tp.IsMarking) {
		t.Fatalf("регистрация по умолчанию: %+v", tp.Registration)
	}
	// Заводские номера детерминированы идентификатором устройства.
	other := New("kkt-02", Options{Now: func() time.Time { return h.now }})
	if other.st.reg.FNSerialNumber == h.m.st.reg.FNSerialNumber {
		t.Fatal("у разных устройств должны быть разные номера ФН")
	}
	if New("kkt-01", Options{}).st.reg.FNSerialNumber != h.m.st.reg.FNSerialNumber {
		t.Fatal("номер ФН должен зависеть только от идентификатора")
	}
}

func TestShiftLifecycle(t *testing.T) {
	h := newHarness(t)
	h.fails(ErrShiftClosed, "ProcessCheck", "h", false, checkXML(1, 0, milk, pay(200, 0)), "")
	h.fails(ErrShiftAlreadyClose, "CloseShift", "h", input, "")

	out := h.openShift()
	p := out.Parameters
	if p.ShiftState != ShiftOpen || *p.ShiftNumber != 1 || !p.DateTime.Equal(h.now) || bool(*p.FNError) {
		t.Fatalf("открытие смены: %+v", p)
	}
	h.fails(ErrShiftAlreadyOpen, "OpenShift", "h", input, "")

	h.check(checkXML(1, 0, milk+bag, pay(200, 0)))
	h.now = h.now.Add(8 * time.Hour)
	r := h.ok("CloseShift", "h", input, "")
	var closed OutputParameters
	Unmarshal(r.Params[2].(string), &closed)
	c := closed.Parameters
	if c.ShiftState != ShiftClosed || *c.ShiftNumber != 1 || *c.DocumentsCounter != 1 ||
		c.CountersOperationType1 == nil || c.CountersOperationType1.CheckCount != 1 || c.CountersOperationType1.TotalChecksAmount != 184.8 {
		t.Fatalf("закрытие смены: %+v", c)
	}
	if !strings.Contains(r.Params[2].(string), `<CountersOperationType1 CheckCount="1"`) {
		t.Fatalf("счётчики операций: %s", r.Params[2])
	}

	// Новая смена — номер 2, счётчики с нуля.
	if out := h.openShift(); *out.Parameters.ShiftNumber != 2 || *out.Parameters.ShiftClosingCheckNumber != 0 {
		t.Fatalf("вторая смена: %+v", out.Parameters)
	}
}

func TestCheckFiscalization(t *testing.T) {
	h := newHarness(t)
	h.openShift() // ФД 2 (ФД 1 — регистрация)
	res := h.check(checkXML(1, 0, `<TextString Text="Спасибо!"/>`+milk+bag+`<Barcode Type="QR" ValueBase64="dGVzdA=="/>`, pay(200, 0)))
	if res.CheckNumber != 3 || res.ShiftNumber != 1 || res.ShiftClosingCheckNumber != 1 || res.FiscalSign == "" ||
		res.AddressSiteInspections != "www.nalog.gov.ru" || !res.DateTime.Equal(h.now) || res.MTNumber != nil || res.PrintError != nil {
		t.Fatalf("результат чека: %+v", res)
	}
	// Фискальный признак детерминирован.
	if res.FiscalSign != h.m.fiscalSign(3, h.now, 184.8) {
		t.Fatal("ФП не детерминирован")
	}

	st := h.status()
	if *st.CheckNumber != 3 || *st.CashBalance != 184.8 || *st.ShiftClosingCheckNumber != 1 || st.ShiftState != ShiftOpen {
		t.Fatalf("состояние после чека: %+v", st)
	}

	tape := h.m.Prints(1)
	if len(tape) != 1 || tape[0].Kind != "check" || tape[0].Number != 3 {
		t.Fatalf("лента: %+v", tape)
	}
	text := strings.Join(tape[0].Lines, "\n")
	for _, want := range []string{emuMark, "КАССОВЫЙ ЧЕК", "ПРИХОД", "Спасибо!", "Молоко", "=179.80", "НДС 10%", "БЕЗ НДС",
		"ИТОГ", "=184.80", "НАЛИЧНЫМИ", "=200.00", "СДАЧА", "=15.20", "[QR: test]"} {
		if !strings.Contains(text, want) {
			t.Errorf("в чеке нет %q:\n%s", want, text)
		}
	}
	// QR-код чека ФНС — отдельной картинкой под текстом (PrintedDocument.QR), а не строкой.
	q := tape[0].QR
	if q == nil || len(q.Rows) != q.Size || q.Size < 21 {
		t.Fatalf("QR-код чека: %+v", q)
	}
	for _, want := range []string{"t=", "&s=184.80&", "fn=" + h.m.st.reg.FNSerialNumber, "&i=3&", "&fp=", "&n=1"} {
		if !strings.Contains(q.Data, want) {
			t.Errorf("в данных QR нет %q: %s", want, q.Data)
		}
	}
	if strings.Contains(text, "[QR: t=") {
		t.Error("строка QR больше не печатается текстом")
	}
	for _, l := range tape[0].Lines {
		if n := len([]rune(l)); n > defaultLineLength {
			t.Errorf("строка длиннее ленты (%d): %q", n, l)
		}
	}

	// Возврат прихода — наличные выдаются из кассы.
	h.check(checkXML(OpIncomeReturn, 0, bag, pay(5, 0)))
	if st := h.status(); *st.CashBalance != 179.8 {
		t.Fatalf("наличные после возврата: %v", *st.CashBalance)
	}
}

func TestCheckValidation(t *testing.T) {
	h := newHarness(t)
	h.openShift()
	before := h.m.st.docNumber
	cases := []struct {
		code int
		pkg  string
	}{
		{ErrPaymentLess, checkXML(1, 0, milk, pay(100, 0))},
		{ErrCashlessOverpay, checkXML(1, 0, milk, pay(0, 200))},
		{ErrTaxationSystem, checkXML(1, 3, milk, pay(200, 0))},
		{ErrVAT, checkXML(1, 0, `<FiscalString Name="Без суммы НДС" Quantity="1" PriceWithDiscount="10" AmountWithDiscount="10" VATRate="20"/>`, pay(10, 0))},
		{ErrVAT, checkXML(1, 0, `<FiscalString Name="Ставка" Quantity="1" PriceWithDiscount="10" AmountWithDiscount="10" VATRate="13" VATAmount="1"/>`, pay(10, 0))},
		{ErrNoPositions, checkXML(1, 0, `<TextString Text="только текст"/>`, pay(0, 0))},
		{ErrBadData, checkXML(9, 0, milk, pay(200, 0))},
		{ErrBadData, `<CheckPackage><Parameters`},
		{ErrNotEnoughCash, checkXML(OpIncomeReturn, 0, milk, pay(179.8, 0))},
		{ErrBarcodeType, checkXML(1, 0, milk+`<Barcode Type="PDF417" ValueBase64="eA=="/>`, pay(200, 0))},
	}
	for _, c := range cases {
		h.fails(c.code, "ProcessCheck", "h", false, c.pkg, "")
	}
	if h.m.st.docNumber != before || h.m.st.cash != 0 {
		t.Fatalf("отклонённые чеки не должны фискализироваться: ФД %d → %d, наличные %v", before, h.m.st.docNumber, h.m.st.cash)
	}
	// Смешанная оплата: безнал до суммы чека, сдача только с наличных.
	h.check(checkXML(1, 0, milk, pay(50, 179.8)))
	if h.m.st.cash != 0 {
		t.Fatalf("при сдаче со всей наличной оплаты в кассе 0, получено %v", h.m.st.cash)
	}
}

func TestShiftExpiry(t *testing.T) {
	h := newHarness(t)
	h.openShift()
	h.now = h.now.Add(25 * time.Hour)
	r := h.fails(ErrShiftExpired, "ProcessCheck", "h", false, checkXML(1, 0, milk, pay(200, 0)), "")
	if !strings.HasPrefix(r.ErrorText, "Смена превысила 24 часа") {
		t.Fatalf("текст должен узнаваться БПО: %q", r.ErrorText)
	}
	if st := h.status(); st.ShiftState != ShiftExpired {
		t.Fatalf("ShiftState = %d, want 3", st.ShiftState)
	}
	h.fails(ErrShiftExpired, "OpenShift", "h", input, "")
	h.ok("CloseShift", "h", input, "") // истёкшую смену закрыть можно
	h.openShift()

	// То же через Control API: «смена открыта 25 часов назад».
	h.set(map[string]any{"shift_opened_hours_ago": 25.0})
	h.fails(ErrShiftExpired, "CashInOutcome", "h", input, num(100))
}

func TestPrinterFaults(t *testing.T) {
	h := newHarness(t)
	h.openShift()
	pkg := checkXML(1, 0, milk, pay(200, 0))

	h.set(map[string]any{"paper": false})
	r := h.fails(ErrPaperOut, "ProcessCheck", "h", false, pkg, "")
	if !strings.HasPrefix(r.ErrorText, "Нет бумаги") {
		t.Fatalf("текст: %q", r.ErrorText)
	}
	// Электронный чек печать не требует.
	r = h.ok("ProcessCheck", "h", true, pkg, "")
	if d := h.m.Prints(1)[0]; !d.Electronic {
		t.Fatalf("электронный чек не помечен: %+v", d)
	}
	h.fails(ErrPaperOut, "PrintXReport", "h", input)

	h.set(map[string]any{"paper": true, "cover_open": true})
	h.fails(ErrCoverOpen, "ProcessCheck", "h", false, pkg, "")

	// Сбой печати после фискализации: True + PrintError, чек учтён.
	h.set(map[string]any{"cover_open": false, "print_error_after_fiscal": true})
	res := h.check(pkg)
	if res.PrintError == nil || !bool(*res.PrintError) {
		t.Fatalf("PrintError не выставлен: %+v", res)
	}
	if h.m.st.checksInShift != 2 {
		t.Fatalf("чек со сбоем печати должен быть фискализирован: чеков %d", h.m.st.checksInShift)
	}
}

func TestMarking(t *testing.T) {
	h := newHarness(t)
	h.openShift()
	km := base64.StdEncoding.EncodeToString([]byte("010460742879395621xxxxxxx\x1d91EE10\x1d92abcdefg"))
	marked := fmt.Sprintf(`<FiscalString Name="Обувь" Quantity="1" PriceWithDiscount="1000" AmountWithDiscount="1000" VATRate="20" VATAmount="166.67" CalculationSubject="33" MarkingCode="%s"/>`, km)
	pkg := checkXML(1, 0, marked, pay(1000, 0))
	request := fmt.Sprintf(`<RequestKM GUID="g-1" WaitForResult="True" NotSendToServer="False" MarkingCode="%s" PlannedStatus="1"/>`, km)

	r := h.fails(ErrKMNotChecked, "ProcessCheck", "h", false, pkg, "")
	if !strings.HasPrefix(r.ErrorText, "Контрольная марка не проверена") {
		t.Fatalf("текст: %q", r.ErrorText)
	}
	h.fails(ErrKMNoRequest, "GetProcessingKMResult", "h", "", num(0))

	r = h.ok("RequestKM", "h", request, "")
	if r.Params[2] != xmlHeader+`<RequestKMResult Checking="true" CheckingResult="true"></RequestKMResult>` {
		t.Fatalf("RequestKMResult: %v", r.Params[2])
	}
	if !h.m.st.kmSession {
		t.Fatal("сессия КМ должна открыться автоматически")
	}
	r = h.ok("GetProcessingKMResult", "h", "", num(1))
	var pr ProcessingKMResult
	Unmarshal(r.Params[1].(string), &pr)
	if r.Params[2] != int64(KMResultReady) || pr.GUID != "g-1" || !bool(pr.Result) || pr.ResultCode != 15 || *pr.StatusInfo != 1 {
		t.Fatalf("результат ОИСМ: %+v status=%v", pr, r.Params[2])
	}
	h.fails(ErrKMNotChecked, "ProcessCheck", "h", false, pkg, "") // проверен, но не подтверждён
	h.fails(ErrKMRequestUnknown, "ConfirmKM", "h", "g-x", num(0))
	h.ok("ConfirmKM", "h", "g-1", num(0))
	res := h.check(pkg)
	if res.MTNumber == nil || *res.MTNumber != 1 {
		t.Fatalf("номер уведомления о реализации МТ: %+v", res.MTNumber)
	}
	if h.m.st.kmSession || len(h.m.st.kmRequests) != 0 {
		t.Fatal("сессия КМ закрывается при фискализации чека")
	}
	if !strings.Contains(strings.Join(h.m.Prints(1)[0].Lines, "\n"), "[М] Обувь") {
		t.Fatal("маркированная позиция не помечена в чеке")
	}

	// Отрицательный результат проверки.
	h.set(map[string]any{"km_result": "invalid"})
	r = h.ok("RequestKM", "h", strings.Replace(request, "g-1", "g-2", 1), "")
	if !strings.Contains(r.Params[2].(string), `CheckingResult="false"`) {
		t.Fatalf("локальная проверка: %v", r.Params[2])
	}
	r = h.ok("GetProcessingKMResult", "h", "", num(1))
	if !strings.Contains(r.Params[1].(string), `Result="false" ResultCode="5"`) {
		t.Fatalf("ответ ОИСМ при отказе: %v", r.Params[1])
	}

	// Задержка ответа ОИСМ и недоступность.
	h.set(map[string]any{"km_result": "valid", "km_delay_ms": 3000.0})
	h.ok("RequestKM", "h", strings.Replace(request, "g-1", "g-3", 1), "")
	if r = h.ok("GetProcessingKMResult", "h", "", num(1)); r.Params[2] != int64(KMResultPending) || r.Params[1] != "" {
		t.Fatalf("ожидание ответа ОИСМ: %v", r.Params)
	}
	h.now = h.now.Add(4 * time.Second)
	if r = h.ok("GetProcessingKMResult", "h", "", num(1)); r.Params[2] != int64(KMResultReady) {
		t.Fatalf("после задержки: %v", r.Params)
	}
	h.set(map[string]any{"km_result": "unavailable"})
	if r = h.ok("GetProcessingKMResult", "h", "", num(1)); r.Params[2] != int64(KMResultUnavailable) {
		t.Fatalf("ОИСМ недоступна: %v", r.Params)
	}

	// Нестрогий режим: КМ без проверки допускается.
	h.set(map[string]any{"km_result": "valid", "marking_strict": false})
	h.check(pkg)
}

func TestOFDBacklog(t *testing.T) {
	h := newHarness(t)
	h.set(map[string]any{"ofd_online": false})
	h.openShift()
	h.check(checkXML(1, 0, milk, pay(200, 0)))
	st := h.status()
	if *st.BacklogDocumentsCounter != 2 || *st.BacklogDocumentFirstNumber != 2 || st.BacklogDocumentFirstDateTime.IsZero() {
		t.Fatalf("очередь ОФД: %+v", st)
	}
	r := h.ok("ReportCurrentStatusOfSettlements", "h", input, "")
	if !strings.Contains(r.Params[2].(string), `BacklogDocumentsCounter="3"`) {
		t.Fatalf("отчёт о расчётах: %s", r.Params[2])
	}
	h.set(map[string]any{"ofd_online": true})
	if st := h.status(); *st.BacklogDocumentsCounter != 0 || st.BacklogDocumentFirstNumber != nil {
		t.Fatalf("после восстановления связи: %+v", st)
	}
}

func TestRegistrationAndFNClose(t *testing.T) {
	h := newHarness(t)
	h.set(map[string]any{"fiscal": false})
	r := h.fails(ErrNotRegistered, "OpenShift", "h", input, "")
	if !strings.Contains(r.ErrorText, "не зарегистрирована") {
		t.Fatalf("текст: %q", r.ErrorText)
	}
	data := h.ok("GetDataKKT", "h", "").Params[1].(string)
	if !strings.Contains(data, `Fiscal="false"`) || strings.Contains(data, "CompanyName") {
		t.Fatalf("незарегистрированная ККТ: %s", data)
	}

	pf := `<ParametersFiscal CashierName="Администратор" CashierINN="" KKTNumber="0000000002000002" CompanyName="ООО &quot;Ромашка&quot;"
		INN="7711111111" SaleAddress="г. Тверь" SaleLocation="Магазин" TaxationSystems="1" IsMarking="true" IsOnlineOnly="false"
		OFDCompany="ОФД" OFDCompanyINN="7722222222"/>`
	h.ok("OperationFN", "h", num(1), pf)
	data = h.ok("GetDataKKT", "h", "").Params[1].(string)
	if !strings.Contains(data, `CompanyName="ООО &#34;Ромашка&#34;"`) || !strings.Contains(data, `TaxationSystems="1"`) ||
		!strings.Contains(data, `KKTSerialNumber="EMU`) || !strings.Contains(data, `FFDVersionFN="1.2"`) {
		t.Fatalf("после регистрации: %s", data)
	}
	h.fails(ErrAlreadyRegistered, "OperationFN", "h", num(1), pf)

	h.openShift()
	h.fails(ErrShiftMustBeClosed, "OperationFN", "h", num(2), pf)
	h.fails(ErrTaxationSystem, "ProcessCheck", "h", false, checkXML(1, 0, milk, pay(200, 0)), "") // СНО теперь только УСН
	h.ok("CloseShift", "h", input, "")

	h.ok("OperationFN", "h", num(3), pf)
	h.fails(ErrFNClosed, "OpenShift", "h", input, "")
	h.fails(ErrFNClosed, "OperationFN", "h", num(1), pf)
	if d := h.m.Prints(1)[0]; d.Kind != "fn_report" || !strings.Contains(strings.Join(d.Lines, "\n"), "ОТЧЁТ О ЗАКРЫТИИ ФН") {
		t.Fatalf("отчёт о закрытии ФН: %+v", d)
	}
}

func TestCashInOutcomeAndCopy(t *testing.T) {
	h := newHarness(t)
	h.openShift()
	h.fails(ErrNotEnoughCash, "CashInOutcome", "h", input, num(-100))
	h.ok("CashInOutcome", "h", input, num(1000))
	h.ok("CashInOutcome", "h", input, num("-300.5"))
	if h.m.st.cash != 699.5 {
		t.Fatalf("наличные: %v", h.m.st.cash)
	}
	if d := h.m.Prints(1)[0]; d.Kind != "cash_out" || d.Number != 0 {
		t.Fatalf("выплата на ленте: %+v", d)
	}

	res := h.check(checkXML(1, 0, bag, pay(5, 0)))
	h.ok("PrintCheckCopy", "h", fmt.Sprint(res.CheckNumber))
	if d := h.m.Prints(1)[0]; d.Kind != "copy" || d.Lines[0] != h.m.center("*** КОПИЯ ***") {
		t.Fatalf("копия чека: %+v", d)
	}
	h.fails(ErrDocNotFound, "PrintCheckCopy", "h", "999")

	r := h.ok("GetLineLength", "h", nil)
	if r.Params[1] != int64(42) {
		t.Fatalf("ширина ленты: %v", r.Params[1])
	}
	h.fails(ErrNotSupported, "Foo", "h")
	h.fails(ErrBadParams, "CashInOutcome", "h", input) // не хватает параметра
}

func TestCorrectionCheck(t *testing.T) {
	h := newHarness(t)
	h.openShift()
	plain := checkXML(1, 0, bag, pay(5, 0))
	h.fails(ErrCorrectionData, "ProcessCorrectionCheck", "h", false, plain, "")

	corr := strings.Replace(plain, "<AgentData/>", `<CorrectionData Type="1" Date="2026-10-01T00:00:00" Number="ПР-1" Description="Пробит не тот чек"/><AgentData/>`, 1)
	r := h.ok("ProcessCorrectionCheck", "h", false, corr, "")
	if !strings.Contains(r.Params[3].(string), "<DocumentOutputParameters>") || r.Params[2] != corr {
		t.Fatalf("чек коррекции: %v", r.Params)
	}
	if c := h.m.st.counters[0]; c.CorrectionCheckCount != 1 || c.CheckCount != 0 {
		t.Fatalf("счётчики коррекции: %+v", c)
	}
	// Ревизия < 4004: три параметра без Electronically.
	r = h.ok("ProcessCorrectionCheck", "h", corr, "")
	if !strings.Contains(r.Params[2].(string), "<DocumentOutputParameters>") {
		t.Fatalf("старая сигнатура: %v", r.Params)
	}
	if !strings.Contains(strings.Join(h.m.Prints(1)[0].Lines, "\n"), "ПО ПРЕДПИСАНИЮ ПР-1") {
		t.Fatal("данные коррекции не напечатаны")
	}
}

func TestEquipmentTestMentionsEmulator(t *testing.T) {
	h := newHarness(t)
	r := h.ok("EquipmentTest", "KKT", "<Parameters/>", "", "")
	if !strings.Contains(r.Params[2].(string), "ЭМУЛЯТОР") || !strings.Contains(r.Params[3].(string), "ЭМУЛЯТОР") {
		t.Fatalf("тест устройства должен явно говорить об эмуляторе: %v", r.Params)
	}
	h.ok("ConnectEquipment", "", "KKT", "<Parameters/>")
	h.ok("DisconnectEquipment", "h")
}

func TestSetStateAndReset(t *testing.T) {
	h := newHarness(t)
	err := h.m.SetState(map[string]any{"paper": false, "nonsense": 1})
	if err == nil || !strings.Contains(err.Error(), "допустимы") {
		t.Fatalf("неизвестный ключ: %v", err)
	}
	if !h.m.st.paper {
		t.Fatal("при ошибке ни один ключ не применяется")
	}
	for _, bad := range []map[string]any{
		{"paper": "нет"}, {"cash_balance": -1.0}, {"line_length": 5.0}, {"km_result": "maybe"}, {"shift_opened_hours_ago": 1.0},
	} {
		if err := h.m.SetState(bad); err == nil {
			t.Errorf("%v: ожидалась ошибка", bad)
		}
	}
	h.set(map[string]any{"cash_balance": 500.0, "line_length": 32.0, "fn_error": true})
	h.openShift()
	if st := h.status(); *st.CashBalance != 500 || !bool(*st.FNError) {
		t.Fatalf("состояние: %+v", st)
	}
	state := h.m.State()
	if state["model"] != "kkt" || state["cash_balance"] != 500.0 || state["shift"].(map[string]any)["state"] != ShiftOpen {
		t.Fatalf("State(): %v", state)
	}

	h.m.Reset()
	if h.m.st.docNumber != 1 || h.m.st.shiftOpen || h.m.st.cash != 0 || len(h.m.Prints(0)) != 0 || h.m.st.lineLength != 42 {
		t.Fatalf("после сброса: %+v", h.m.st)
	}
}

const xmlHeader = `<?xml version="1.0" encoding="UTF-8"?>` + "\n"

// Ответ ОИСМ по правилам кода (km_rules): разные товары одного чека — разные ответы.
func TestMarkingRules(t *testing.T) {
	h := newHarness(t)
	h.openShift()
	if err := h.m.SetState(map[string]any{"km_rules": []any{
		map[string]any{"title": "Сигареты — выбыли", "contains": "04600000000011", "result": "invalid"},
		map[string]any{"title": "Обувь — оборот приостановлен", "contains": "04600000000028", "result": "suspended"},
		map[string]any{"contains": "04600000000035", "result": "unavailable"},
	}}); err != nil {
		t.Fatal(err)
	}
	ask := func(guid, gtin string) (ProcessingKMResult, int64, string) {
		km := base64.StdEncoding.EncodeToString([]byte("01" + gtin + "21SERIAL\x1d93abcd"))
		r := h.ok("RequestKM", "h", fmt.Sprintf(`<RequestKM GUID="%s" WaitForResult="True" NotSendToServer="False" MarkingCode="%s" PlannedStatus="1"/>`, guid, km), "")
		local := r.Params[2].(string)
		r = h.ok("GetProcessingKMResult", "h", "", num(1))
		var pr ProcessingKMResult
		if s, _ := r.Params[1].(string); s != "" {
			Unmarshal(s, &pr)
		}
		return pr, r.Params[2].(int64), local
	}
	if pr, st, local := ask("a", "04600000000011"); st != KMResultReady || bool(pr.Result) || pr.ResultCode != 5 || *pr.StatusInfo != 2 ||
		!strings.Contains(local, `CheckingResult="false"`) {
		t.Fatalf("некорректный код: %+v %d %s", pr, st, local)
	}
	if pr, _, local := ask("b", "04600000000028"); bool(pr.Result) || pr.ResultCode != 15 || *pr.StatusInfo != 3 ||
		!strings.Contains(local, `CheckingResult="true"`) {
		t.Fatalf("оборот приостановлен: %+v %s", pr, local)
	}
	if _, st, _ := ask("c", "04600000000035"); st != KMResultUnavailable {
		t.Fatalf("ОИСМ недоступна для этого кода: %d", st)
	}
	if pr, _, _ := ask("d", "04600000000042"); !bool(pr.Result) || *pr.StatusInfo != 1 {
		t.Fatalf("остальные — по km_result (valid): %+v", pr)
	}
	for _, bad := range []any{
		[]any{map[string]any{"contains": "", "result": "valid"}},
		[]any{map[string]any{"contains": "1", "result": "maybe"}},
		"не массив",
	} {
		if err := h.m.SetState(map[string]any{"km_rules": bad}); err == nil {
			t.Errorf("%v — ошибка", bad)
		}
	}
	if got := h.m.State()["km"].(map[string]any)["rules"].([]KMRule); len(got) != 3 || got[0].Title != "Сигареты — выбыли" {
		t.Fatalf("правила в состоянии: %+v", got)
	}
}

// ТС ПИоТ (требования 5.0): без признака piot — ошибка 60, с ним — тестовые данные нужной формы.
func TestPiot(t *testing.T) {
	h := newHarness(t)
	b64 := func(n int) string { return base64.StdEncoding.EncodeToString(make([]byte, n)) }
	h.fails(ErrPiotNotSupported, "PiotSetChallenge", "kkt-01", b64(16), "")
	if !strings.Contains(h.ok("GetDataKKT", "kkt-01", "").Params[1].(string), `SupportFNPiot="false"`) {
		t.Fatal("SupportFNPiot по умолчанию false")
	}
	if err := h.m.SetState(map[string]any{"piot": true}); err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(h.ok("GetDataKKT", "kkt-01", "").Params[1].(string), `SupportFNPiot="true"`) {
		t.Fatal("SupportFNPiot после piot: true")
	}
	out := h.ok("PiotSetChallenge", "kkt-01", b64(16), "").Params[2].(string)
	raw, _ := base64.StdEncoding.DecodeString(out)
	if len(raw) != piotHeaderLen+32 || !strings.HasPrefix(string(raw), "EMULKON PIOT TEST") {
		t.Fatalf("Req_chk_chlg: %d байт %q", len(raw), raw[:17])
	}
	h.fails(ErrPiotData, "PiotSetChallenge", "kkt-01", b64(8), "")
	h.fails(ErrPiotData, "PiotSetChallenge", "kkt-01", "не base64!", "")
	if sid := h.ok("PiotCheckChallenge", "kkt-01", b64(40), "").Params[2].(string); !strings.HasPrefix(sid, "EMULKON-FN-SID-") || len(sid) > piotMaxSID {
		t.Fatalf("X-FN-SID: %q", sid)
	}
	// Контейнер: что положили в PiotSetDKMsg, то PiotGetDKMsg и вернёт.
	part := make([]byte, piotRequestLen)
	part[0], part[268] = 7, 9
	hash := make([]byte, piotHashLen)
	hash[31] = 5
	ctrl := h.ok("PiotSetDKMsg", "kkt-01", base64.StdEncoding.EncodeToString(part), base64.StdEncoding.EncodeToString(hash), "").Params[3].(string)
	r := h.ok("PiotGetDKMsg", "kkt-01", ctrl, "", "", "")
	if r.Params[2] != base64.StdEncoding.EncodeToString(part) || r.Params[4] != base64.StdEncoding.EncodeToString(hash) {
		t.Fatal("PiotGetDKMsg не вернул dk_msg1 и хэш из контейнера")
	}
	if n, _ := base64.StdEncoding.DecodeString(r.Params[3].(string)); len(n) != 16 {
		t.Fatalf("rand_ar: %d байт", len(n))
	}
	h.fails(ErrPiotData, "PiotSetDKMsg", "kkt-01", b64(100), base64.StdEncoding.EncodeToString(hash), "")
	// Незарегистрированная ККТ — сначала регистрация.
	if err := h.m.SetState(map[string]any{"fiscal": false}); err != nil {
		t.Fatal(err)
	}
	h.fails(ErrNotRegistered, "PiotSetChallenge", "kkt-01", b64(16), "")
}
