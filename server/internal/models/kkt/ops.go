package kkt

import (
	"encoding/base64"
	"strconv"
	"strings"
	"time"

	"emulkon/internal/models"
)

// EquipmentTest(0 EquipmentType, 1 ConnectionParameters, 2 Description out, 3 DemoModeIsActivated out).
// Описание и «демо-режим» явно говорят, что это эмулятор (ARCHITECTURE §10).
func (m *Model) equipmentTest(a models.Args, out []any) error {
	if err := a.Need(4); err != nil {
		return err
	}
	st := &m.st
	fiscal := "не зарегистрирована"
	if st.reg.Fiscal {
		fiscal = "зарегистрирована, РН " + st.reg.KKTNumber
	}
	out[2] = "Тест пройден. ЭМУЛЯТОР ККТ (Эмулкон): " + fiscal + ", ФН " + st.reg.FNSerialNumber + "."
	out[3] = "ЭМУЛЯТОР: чеки не фискальные и в ОФД не передаются. Не использовать в рабочей базе."
	return nil
}

// GetDataKKT(0 DeviceID, 1 TableParametersKKT out).
func (m *Model) getDataKKT(a models.Args, out []any) error {
	reg := m.st.reg
	if !reg.Fiscal {
		reg = Registration{
			KKTSerialNumber: reg.KKTSerialNumber, KKTModelID: reg.KKTModelID,
			FirmwareVersion: reg.FirmwareVersion, FNSerialNumber: reg.FNSerialNumber,
		}
	}
	reg.SupportFNPiot = Bool(m.st.piot)
	return setOut(a, out, 1, TableParametersKKT{Registration: reg})
}

// OperationFN(0 DeviceID, 1 OperationType LONG, 2 ParametersFiscal): 1 — регистрация,
// 2 — изменение параметров регистрации, 3 — закрытие ФН. Только при закрытой смене.
func (m *Model) operationFN(a models.Args, now time.Time) error {
	if err := a.Need(3); err != nil {
		return err
	}
	op, err := a.Int(1)
	if err != nil {
		return err
	}
	st := &m.st
	if st.fnClosed {
		return failCode(ErrFNClosed)
	}
	if st.shiftOpen {
		return failCode(ErrShiftMustBeClosed)
	}
	var pf ParametersFiscal
	if err := parseXML(a, 2, &pf); err != nil {
		return err
	}

	var title string
	switch op {
	case 1:
		if st.reg.Fiscal {
			return failCode(ErrAlreadyRegistered)
		}
		title = "ОТЧЁТ О РЕГИСТРАЦИИ"
	case 2:
		if !st.reg.Fiscal {
			return failCode(ErrNotRegistered)
		}
		title = "ОТЧЁТ ОБ ИЗМЕНЕНИИ ПАРАМЕТРОВ РЕГИСТРАЦИИ"
	case 3:
		if !st.reg.Fiscal {
			return failCode(ErrNotRegistered)
		}
		title = "ОТЧЁТ О ЗАКРЫТИИ ФН"
	default:
		return fail(ErrBadParams, "тип операции ФН %d (1 — регистрация, 2 — изменение, 3 — закрытие)", op)
	}

	doc := m.nextDoc(now)
	if op == 3 {
		st.fnClosed = true
	} else {
		applyRegistration(&st.reg, pf)
		st.reg.DocumentNumber = strconv.Itoa(doc)
		st.reg.DateTime = DateTime{now}
	}
	m.print(m.fiscalReport(title, doc, now, []string{
		"КАССИР: " + pf.CashierName,
		"РН ККТ: " + st.reg.KKTNumber,
		"ИНН: " + st.reg.INN,
		"СНО: " + st.reg.TaxationSystems,
		"ФФД: " + st.reg.FFDVersionKKT,
	}))
	return nil
}

// applyRegistration переносит параметры регистрации от 1С. Заводские номера,
// модель и прошивка — свойства «железа», их 1С не меняет.
func applyRegistration(reg *Registration, pf ParametersFiscal) {
	keep := *reg
	*reg = pf.Registration
	reg.KKTSerialNumber, reg.KKTModelID = keep.KKTSerialNumber, keep.KKTModelID
	reg.FirmwareVersion, reg.FNSerialNumber = keep.FirmwareVersion, keep.FNSerialNumber
	if reg.KKTNumber == "" {
		reg.KKTNumber = keep.KKTNumber
	}
	reg.FFDVersionFN, reg.FFDVersionKKT = keep.FFDVersionFN, keep.FFDVersionKKT
	if pf.FFDVersion != "" {
		reg.FFDVersionFN, reg.FFDVersionKKT = pf.FFDVersion, pf.FFDVersion
	}
	if reg.FFDVersionFN == "" {
		reg.FFDVersionFN, reg.FFDVersionKKT = "1.2", "1.2"
	}
	if reg.FNSURL == "" {
		reg.FNSURL = defaultFNSURL
	}
	reg.Fiscal = true
}

// OpenShift(0 DeviceID, 1 InputParameters, 2 OutputParameters out).
func (m *Model) openShift(a models.Args, out []any, now time.Time) error {
	if err := a.Need(3); err != nil {
		return err
	}
	if err := m.requireFiscal(); err != nil {
		return err
	}
	switch m.shiftState(now) {
	case ShiftOpen:
		return fail(ErrShiftAlreadyOpen, "смена №%d", m.st.shiftNumber)
	case ShiftExpired:
		return failCode(ErrShiftExpired)
	}
	in, err := inputParams(a, 1)
	if err != nil {
		return err
	}
	if err := m.requirePrinter(printRequired(in)); err != nil {
		return err
	}

	st := &m.st
	st.shiftNumber++
	st.shiftOpen, st.shiftOpenedAt = true, now
	st.checksInShift, st.docsInShift = 0, 0
	st.counters = [4]OperationCounters{}
	doc := m.nextDoc(now)

	m.print(m.fiscalReport("ОТЧЁТ ОБ ОТКРЫТИИ СМЕНЫ", doc, now, []string{
		"СМЕНА: " + strconv.Itoa(st.shiftNumber),
		"КАССИР: " + in.CashierName,
	}))
	return setOut(a, out, 2, OutputParameters{Parameters: m.statusParams(now)})
}

// CloseShift(0 DeviceID, 1 InputParameters, 2 OutputParameters out).
// Закрыть можно и истёкшую смену — это и есть выход из «смена превысила 24 часа».
func (m *Model) closeShift(a models.Args, out []any, now time.Time) error {
	if err := a.Need(3); err != nil {
		return err
	}
	st := &m.st
	if !st.reg.Fiscal {
		return failCode(ErrNotRegistered)
	}
	if st.fnClosed {
		return failCode(ErrFNClosed)
	}
	if !st.shiftOpen {
		return failCode(ErrShiftAlreadyClose)
	}
	in, err := inputParams(a, 1)
	if err != nil {
		return err
	}
	if err := m.requirePrinter(printRequired(in)); err != nil {
		return err
	}

	doc := m.nextDoc(now)
	status := m.statusParams(now)
	status.DocumentsCounter = IntPtr(st.docsInShift)
	counters := st.counters
	status.CountersOperationType1 = &counters[0]
	status.CountersOperationType2 = &counters[1]
	status.CountersOperationType3 = &counters[2]
	status.CountersOperationType4 = &counters[3]
	status.ShiftState = ShiftClosed
	status.DateTime = DateTime{now}
	status.CheckNumber = IntPtr(doc)

	m.print(m.fiscalReport("ОТЧЁТ О ЗАКРЫТИИ СМЕНЫ", doc, now, m.shiftTotalsLines(in.CashierName)))
	st.shiftOpen = false
	return setOut(a, out, 2, OutputParameters{Parameters: status})
}

// GetCurrentStatus(0 DeviceID, 1 InputParameters, 2 OutputParameters out).
func (m *Model) getCurrentStatus(a models.Args, out []any, now time.Time) error {
	if err := a.Need(3); err != nil {
		return err
	}
	return setOut(a, out, 2, OutputParameters{Parameters: m.statusParams(now)})
}

// ReportCurrentStatusOfSettlements(0 DeviceID, 1 InputParameters, 2 OutputParameters out) — ФД.
func (m *Model) reportSettlements(a models.Args, out []any, now time.Time) error {
	if err := a.Need(3); err != nil {
		return err
	}
	if err := m.requireFiscal(); err != nil {
		return err
	}
	in, err := inputParams(a, 1)
	if err != nil {
		return err
	}
	if err := m.requirePrinter(printRequired(in)); err != nil {
		return err
	}
	doc := m.nextDoc(now)
	m.print(m.fiscalReport("ОТЧЁТ О ТЕКУЩЕМ СОСТОЯНИИ РАСЧЁТОВ", doc, now, []string{
		"НЕПЕРЕДАННЫХ ФД: " + strconv.Itoa(len(m.st.backlog)),
	}))
	return setOut(a, out, 2, OutputParameters{Parameters: m.statusParams(now)})
}

// PrintXReport(0 DeviceID, 1 InputParameters) — нефискальный отчёт без гашения.
func (m *Model) printXReport(a models.Args, now time.Time) error {
	if err := a.Need(2); err != nil {
		return err
	}
	if err := m.requireFiscal(); err != nil {
		return err
	}
	in, err := inputParams(a, 1)
	if err != nil {
		return err
	}
	if err := m.requirePrinter(true); err != nil {
		return err
	}
	m.print(m.nonFiscal("x_report", "ОТЧЁТ БЕЗ ГАШЕНИЯ (X-ОТЧЁТ)", now, m.shiftTotalsLines(in.CashierName)))
	return nil
}

// CashInOutcome(0 DeviceID, 1 InputParameters, 2 Amount DOUBLE): > 0 — внесение, < 0 — выплата
// (знак ставит БПО, ПодключаемоеОборудованиеДрайверКлиент:2757).
func (m *Model) cashInOutcome(a models.Args, now time.Time) error {
	if err := a.Need(3); err != nil {
		return err
	}
	amount, err := a.Float(2)
	if err != nil {
		return err
	}
	amount = Kopecks(amount)
	if err := m.requireFiscal(); err != nil {
		return err
	}
	if err := m.requireOpenShift(now); err != nil {
		return err
	}
	in, err := inputParams(a, 1)
	if err != nil {
		return err
	}
	if amount == 0 {
		return fail(ErrBadParams, "сумма внесения/выплаты равна нулю")
	}
	if amount < 0 && m.st.cash < -amount {
		return fail(ErrNotEnoughCash, "в кассе %s, выплата %s", money(m.st.cash), money(-amount))
	}
	if err := m.requirePrinter(printRequired(in)); err != nil {
		return err
	}
	m.st.cash = Kopecks(m.st.cash + amount)
	title, kind := "ВНЕСЕНИЕ", "cash_in"
	if amount < 0 {
		title, kind = "ВЫПЛАТА", "cash_out"
	}
	m.print(m.nonFiscal(kind, title, now, []string{
		"КАССИР: " + in.CashierName,
		m.lr("СУММА", money(abs(amount))),
		m.lr("НАЛИЧНЫХ В КАССЕ", money(m.st.cash)),
	}))
	return nil
}

// PrintTextDocument(0 DeviceID, 1 DocumentPackage).
func (m *Model) printTextDocument(a models.Args, now time.Time) error {
	if err := a.Need(2); err != nil {
		return err
	}
	var doc DocumentPackage
	if err := parseXML(a, 1, &doc); err != nil {
		return err
	}
	if err := checkBarcodes(doc.Positions); err != nil {
		return err
	}
	if err := m.requirePrinter(true); err != nil {
		return err
	}
	lines := m.positionLines(doc.Positions, false)
	m.print(m.nonFiscal("text", "", now, lines))
	return nil
}

// PrintCheckCopy(0 DeviceID, 1 CheckNumber STRING) — копия ранее напечатанного чека.
func (m *Model) printCheckCopy(a models.Args, now time.Time) error {
	if err := a.Need(2); err != nil {
		return err
	}
	s, err := a.String(1)
	if err != nil {
		return err
	}
	n, err := strconv.Atoi(strings.TrimSpace(s))
	if err != nil {
		return fail(ErrBadParams, "номер чека %q", s)
	}
	if err := m.requirePrinter(true); err != nil {
		return err
	}
	for i := len(m.tape) - 1; i >= 0; i-- {
		d := m.tape[i]
		if d.Number == n && (d.Kind == "check" || d.Kind == "correction") {
			lines := append([]string{m.center("*** КОПИЯ ***")}, d.Lines...)
			m.print(models.PrintedDocument{Kind: "copy", Number: n, Time: now, Lines: lines, QR: d.QR})
			return nil
		}
	}
	return fail(ErrDocNotFound, "чек ФД №%d", n)
}

// --- маркировка ---

func (m *Model) openKMSession() {
	m.st.kmSession = true
	m.st.kmRequests = map[string]*kmRequest{}
	m.st.kmCurrent = ""
}

func (m *Model) closeKMSession() {
	m.st.kmSession = false
	m.st.kmRequests = map[string]*kmRequest{}
	m.st.kmCurrent = ""
}

// RequestKM(0 DeviceID, 1 RequestKM, 2 RequestKMResult out). Сессию регистрации КМ
// открываем сами, если 1С не открыла её явно (снисходительно, см. docs/models/kkt.md).
func (m *Model) requestKM(a models.Args, out []any, now time.Time) error {
	if err := a.Need(3); err != nil {
		return err
	}
	if err := m.requireFiscal(); err != nil {
		return err
	}
	var rq RequestKM
	if err := parseXML(a, 1, &rq); err != nil {
		return err
	}
	if rq.GUID == "" {
		return fail(ErrBadData, "RequestKM: пустой GUID")
	}
	code, err := base64.StdEncoding.DecodeString(rq.MarkingCode)
	if err != nil || len(code) == 0 {
		return fail(ErrBadData, "RequestKM: MarkingCode должен быть непустым Base64")
	}
	if !m.st.kmSession {
		m.openKMSession()
	}
	req := &kmRequest{
		GUID: rq.GUID, MarkingCode: rq.MarkingCode, PlannedStatus: rq.PlannedStatus,
		NotSend: bool(rq.NotSendToServer), Result: m.st.kmRuleFor(string(code)), RequestedAt: now,
	}
	m.st.kmRequests[rq.GUID] = req
	m.st.kmCurrent = rq.GUID
	result := m.st.kmResultOf(req)
	// Локальная проверка ФН — криптохвост кода: отрицательна только у «некорректного» кода.
	return setOut(a, out, 2, RequestKMResult{Checking: true, CheckingResult: Bool(result != kmInvalid)})
}

// GetProcessingKMResult(0 DeviceID, 1 ProcessingKMResult out, 2 RequestStatus LONG out).
// ResultCode — тег 2106 (битовая маска): 15 — всё проверено и корректно,
// 5 — проверено, результат отрицательный.
func (m *Model) getProcessingKMResult(a models.Args, out []any, now time.Time) error {
	if err := a.Need(3); err != nil {
		return err
	}
	req := m.st.kmRequests[m.st.kmCurrent]
	if req == nil {
		return failCode(ErrKMNoRequest)
	}
	switch {
	case m.st.kmResultOf(req) == kmUnavailable && !req.NotSend:
		out[1], out[2] = "", int64(KMResultUnavailable)
		return nil
	case !req.NotSend && now.Sub(req.RequestedAt) < m.st.kmDelay:
		out[1], out[2] = "", int64(KMResultPending)
		return nil
	}
	res := ProcessingKMResult{GUID: req.GUID, Result: true, ResultCode: 15, StatusInfo: IntPtr(1)}
	switch m.st.kmResultOf(req) {
	case kmInvalid:
		res.Result, res.ResultCode, res.StatusInfo = false, 5, IntPtr(2)
	case kmSuspended: // код проверен, но оборот товара приостановлен
		res.Result, res.ResultCode, res.StatusInfo = false, 15, IntPtr(3)
	}
	out[2] = int64(KMResultReady)
	return setOut(a, out, 1, res)
}

// ConfirmKM(0 DeviceID, 1 GUID, 2 ConfirmationType LONG): 0 — КМ войдёт в чек, 1 — отказ.
func (m *Model) confirmKM(a models.Args) error {
	if err := a.Need(3); err != nil {
		return err
	}
	guid, err := a.String(1)
	if err != nil {
		return err
	}
	typ, err := a.Int(2)
	if err != nil {
		return err
	}
	req := m.st.kmRequests[guid]
	if req == nil {
		return fail(ErrKMRequestUnknown, "%s", guid)
	}
	switch typ {
	case 0:
		req.Confirmed = true
	case 1:
		delete(m.st.kmRequests, guid)
		if m.st.kmCurrent == guid {
			m.st.kmCurrent = ""
		}
	default:
		return fail(ErrBadParams, "тип подтверждения %d (0 или 1)", typ)
	}
	return nil
}

// --- чек ---

// vatRates — допустимые ставки НДС (требования 4.8).
var vatRates = map[string]bool{
	"none": true, "0": true, "5": true, "7": true, "10": true, "18": true, "20": true, "22": true,
	"5/105": true, "7/107": true, "10/110": true, "18/118": true, "20/120": true, "22/122": true,
}

func checkBarcodes(p Positions) error {
	for _, it := range p.Items {
		if it.Barcode == nil {
			continue
		}
		ok := false
		for _, t := range BarcodeTypes {
			if strings.EqualFold(it.Barcode.Type, t) {
				ok = true
			}
		}
		if !ok {
			return fail(ErrBarcodeType, "%q", it.Barcode.Type)
		}
	}
	return nil
}

// ProcessCheck / ProcessCorrectionCheck(0 DeviceID, 1 Electronically BOOL, 2 CheckPackage,
// 3 DocumentOutputParameters out). Чек коррекции при ревизии < 4004 — без Electronically:
// (0 DeviceID, 1 CheckPackage, 2 DocumentOutputParameters out).
func (m *Model) processCheck(a models.Args, out []any, now time.Time, correction bool) error {
	pkgPos, outPos, electronic := 2, 3, false
	if correction && len(a) == 3 {
		pkgPos, outPos = 1, 2
	} else {
		if err := a.Need(4); err != nil {
			return err
		}
		var err error
		if electronic, err = a.Bool(1); err != nil {
			return err
		}
	}

	if err := m.requireFiscal(); err != nil {
		return err
	}
	if err := m.requireOpenShift(now); err != nil {
		return err
	}
	var c CheckPackage
	if err := parseXML(a, pkgPos, &c); err != nil {
		return err
	}
	p := c.Parameters
	if correction && p.CorrectionData == nil {
		return failCode(ErrCorrectionData)
	}
	if p.OperationType < OpIncome || p.OperationType > OpExpenseReturn {
		return fail(ErrBadData, "OperationType %d", p.OperationType)
	}
	if !m.taxationAllowed(p.TaxationSystem) {
		return fail(ErrTaxationSystem, "код %d, зарегистрированы: %s", p.TaxationSystem, m.st.reg.TaxationSystems)
	}
	fiscal := c.FiscalStrings()
	if len(fiscal) == 0 {
		return failCode(ErrNoPositions)
	}
	for i, f := range fiscal {
		if !vatRates[f.VATRate] {
			return fail(ErrVAT, "позиция %d «%s»: ставка %q", i+1, f.Name, f.VATRate)
		}
		if f.VATRate != "none" && f.VATAmount == nil {
			return fail(ErrVAT, "позиция %d «%s»: нет суммы НДС", i+1, f.Name)
		}
	}
	if err := checkBarcodes(c.Positions); err != nil {
		return err
	}

	// Оплата: всего не меньше суммы чека, безналом не больше, сдача — только из наличных.
	total := c.Total()
	pay := c.Payments
	paid := pay.Total()
	if paid < total-0.005 {
		return fail(ErrPaymentLess, "чек %s, оплачено %s", money(total), money(paid))
	}
	cashless := Kopecks(float64(pay.ElectronicPayment + pay.PrePayment + pay.PostPayment + pay.Barter))
	if cashless > total+0.005 {
		return fail(ErrCashlessOverpay, "чек %s, безналом %s", money(total), money(cashless))
	}
	change := Kopecks(paid - total)
	cashNet := Kopecks(float64(pay.Cash) - change)
	newCash := m.st.cash
	switch p.OperationType {
	case OpIncome, OpExpenseReturn:
		newCash += cashNet
	case OpIncomeReturn, OpExpense:
		if m.st.cash < cashNet-0.005 {
			return fail(ErrNotEnoughCash, "в кассе %s, к выдаче %s", money(m.st.cash), money(cashNet))
		}
		newCash -= cashNet
	}

	// Маркировка: КМ в чеке должен пройти RequestKM и ConfirmKM (тип 0).
	marked := false
	for i, f := range fiscal {
		if f.MarkingCode == "" {
			continue
		}
		marked = true
		if m.st.markingStrict && !m.kmConfirmed(f.MarkingCode) {
			return fail(ErrKMNotChecked, "позиция %d «%s»", i+1, f.Name)
		}
	}

	if err := m.requirePrinter(!electronic); err != nil {
		return err
	}

	// Фискализация.
	st := &m.st
	doc := m.nextDoc(now)
	st.checksInShift++
	st.docsInShift++
	st.cash = Kopecks(newCash)
	cnt := &st.counters[p.OperationType-1]
	if correction {
		cnt.CorrectionCheckCount++
		cnt.TotalCorrectionChecksAmount = Number(Kopecks(float64(cnt.TotalCorrectionChecksAmount) + total))
	} else {
		cnt.CheckCount++
		cnt.TotalChecksAmount = Number(Kopecks(float64(cnt.TotalChecksAmount) + total))
	}
	res := DocumentResult{
		ShiftNumber: st.shiftNumber, CheckNumber: doc, ShiftClosingCheckNumber: st.checksInShift,
		AddressSiteInspections: st.reg.FNSURL, FiscalSign: m.fiscalSign(doc, now, total),
		DateTime: DateTime{now},
	}
	if res.AddressSiteInspections == "" {
		res.AddressSiteInspections = defaultFNSURL
	}
	if marked {
		st.mtNumber++
		res.MTNumber = IntPtr(st.mtNumber)
	}
	printErr := !electronic && st.printErrorAfterFiscal
	if printErr {
		res.PrintError = BoolPtr(true)
	}
	m.closeKMSession() // сессия проверки КМ закрывается при фискализации чека

	kind := "check"
	if correction {
		kind = "correction"
	}
	lines, qr := m.receiptLines(&c, correction, doc, res.FiscalSign, now, total, change)
	m.print(models.PrintedDocument{
		Kind: kind, Number: doc, Time: now, Electronic: electronic, PrintError: printErr,
		Lines: lines, QR: models.NewQR(qr),
	})
	return setOut(a, out, outPos, DocumentOutputParameters{Parameters: res})
}

func (m *Model) kmConfirmed(code string) bool {
	for _, r := range m.st.kmRequests {
		if r.Confirmed && r.MarkingCode == code {
			return true
		}
	}
	return false
}

func abs(v float64) float64 {
	if v < 0 {
		return -v
	}
	return v
}
