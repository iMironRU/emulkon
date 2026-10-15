package kkt

import (
	"context"
	"errors"
	"fmt"
	"hash/fnv"
	"sort"
	"strconv"
	"strings"
	"sync"
	"time"

	"emulkon/internal/models"
)

// Options — параметры модели.
type Options struct {
	Version string
	Now     func() time.Time
}

// Factory — фабрика модели для реестра (models.Registry).
func Factory(version string) models.Factory {
	return func(_ models.EquipmentType, deviceID string) models.Model {
		return New(deviceID, Options{Version: version})
	}
}

const (
	tapeLimit          = 200
	defaultLineLength  = 42
	shiftMaxDuration   = 24 * time.Hour
	defaultFNSURL      = "www.nalog.gov.ru"
	fnValidityDuration = 15 * 30 * 24 * time.Hour // ~15 месяцев
)

// Результат проверки КМ в ОИСМ, задаётся через Control API (km_result).
const (
	kmValid       = "valid"
	kmInvalid     = "invalid"
	kmUnavailable = "unavailable"
	kmSuspended   = "suspended" // оборот товара приостановлен (StatusInfo 3, ФФД табл. 106)
)

type kmRequest struct {
	GUID          string
	MarkingCode   string // Base64, как пришёл
	PlannedStatus int
	NotSend       bool
	Result        string // ответ ОИСМ по правилу km_rules; пусто — текущий km_result на момент опроса
	RequestedAt   time.Time
	Confirmed     bool // ConfirmKM с типом 0
}

type backlogDoc struct {
	Number int
	At     time.Time
}

// state — состояние ККТ. Копируется целиком при SetState (транзакционно).
type state struct {
	reg        Registration
	fnClosed   bool
	fnValidity time.Time

	shiftOpen     bool
	shiftNumber   int
	shiftOpenedAt time.Time
	checksInShift int // номер чека за смену
	docsInShift   int // чеки и чеки коррекции за смену
	docNumber     int // номер последнего ФД
	counters      [4]OperationCounters
	cash          float64
	mtNumber      int

	ofdOnline bool
	backlog   []backlogDoc

	paper                 bool
	coverOpen             bool
	fnError               bool
	fnOverflow            bool
	fnFail                bool
	printErrorAfterFiscal bool

	kmSession     bool
	kmRequests    map[string]*kmRequest
	kmCurrent     string
	kmResult      string
	kmRules       []KMRule // ответ ОИСМ по коду: первое совпадение, иначе kmResult
	kmDelay       time.Duration
	markingStrict bool
	piot          bool // ФН работает с ТС ПИоТ: SupportFNPiot в GetDataKKT, методы Piot* (piot.go)

	lineLength   int
	drawerOpened int
	drawer       models.Drawer
	connects     int
}

// Model — эмулятор ККТ с передачей в ОФД (раздел 3.5 требований, ревизия 4008).
type Model struct {
	id   string
	opts Options

	mu      sync.Mutex
	st      state
	tape    []models.PrintedDocument
	tapeSeq int64
}

// New создаёт ККТ в состоянии «зарегистрирована тестовыми данными, смена закрыта».
func New(deviceID string, opts Options) *Model {
	if opts.Now == nil {
		opts.Now = time.Now
	}
	m := &Model{id: deviceID, opts: opts}
	m.st = m.defaultState()
	return m
}

// serial — детерминированный «заводской» номер из идентификатора устройства.
func serial(id string, prefix string, digits int) string {
	h := fnv.New64a()
	h.Write([]byte(prefix + id))
	n := h.Sum64()
	s := strconv.FormatUint(n, 10)
	for len(s) < digits {
		s = "0" + s
	}
	return prefix + s[len(s)-digits:]
}

func (m *Model) defaultState() state {
	now := m.opts.Now()
	st := state{
		fnValidity:    now.Add(fnValidityDuration),
		ofdOnline:     true,
		paper:         true,
		kmRequests:    map[string]*kmRequest{},
		kmResult:      kmValid,
		markingStrict: true,
		lineLength:    defaultLineLength,
		drawer:        models.NewDrawer(),
	}
	st.reg = Registration{
		KKTSerialNumber: serial(m.id, "EMU", 10),
		KKTModelID:      "EMULKON",
		FirmwareVersion: m.opts.Version,
		FNSerialNumber:  serial(m.id, "9999", 12),
	}
	m.registerDefaults(&st, now)
	return st
}

// registerDefaults — регистрация тестовыми данными (ФД №1 — отчёт о регистрации).
func (m *Model) registerDefaults(st *state, now time.Time) {
	r := &st.reg
	r.Fiscal = true
	r.KKTNumber = serial(m.id, "", 16)
	r.FFDVersionFN, r.FFDVersionKKT = "1.2", "1.2"
	r.CompanyName = "ООО «ТЕСТ ЭМУЛКОН» (ЭМУЛЯТОР)"
	r.INN = "7700000000"
	r.SaleAddress = "г. Москва, ул. Тестовая, д. 1"
	r.SaleLocation = "Эмулятор ККТ Эмулкон"
	r.TaxationSystems = "0,1,2,4,5"
	r.IsMarking = true
	r.OFDCompany = "ООО «ТЕСТОВЫЙ ОФД»"
	r.OFDCompanyINN = "7700000001"
	r.FNSURL = defaultFNSURL
	st.fnClosed = false
	if st.docNumber == 0 {
		st.docNumber = 1
	}
	r.DocumentNumber = strconv.Itoa(st.docNumber)
	r.DateTime = DateTime{now}
}

// --- models.Model ---

func (m *Model) Description() models.Description {
	return models.Description{
		Name:        "ЭМУЛЯТОР: ККТ с передачей в ОФД (Эмулкон)",
		Description: "Виртуальная ККТ Эмулкон. НЕ ИСПОЛЬЗОВАТЬ В РАБОЧЕЙ БАЗЕ: чеки не фискальные, в ОФД не передаются.",
		Version:     m.opts.Version,
		IsEmulator:  true,
	}
}

func (m *Model) Reset() {
	m.mu.Lock()
	defer m.mu.Unlock()
	m.st = m.defaultState()
	m.tape = nil
	m.tapeSeq = 0
}

// Handle — вызов метода драйвера. Позиции параметров — docs/bpo-requirements/kkt-methods.md.
func (m *Model) Handle(_ context.Context, method string, params []any) models.Result {
	m.mu.Lock()
	defer m.mu.Unlock()

	a := models.Args(params)
	out := models.CopyParams(params)
	now := m.opts.Now()

	var ret any = true
	var err error
	switch method {
	// раздел 2: подключение (остальные методы раздела 2 компонента обслуживает сама)
	case "ConnectEquipment":
		m.st.connects++
	case "DisconnectEquipment":
	case "EquipmentTest":
		err = m.equipmentTest(a, out)

	// раздел 3.5
	case "GetDataKKT":
		err = m.getDataKKT(a, out)
	case "OperationFN":
		err = m.operationFN(a, now)
	case "OpenShift":
		err = m.openShift(a, out, now)
	case "CloseShift":
		err = m.closeShift(a, out, now)
	case "ProcessCheck":
		err = m.processCheck(a, out, now, false)
	case "ProcessCorrectionCheck":
		err = m.processCheck(a, out, now, true)
	case "PrintTextDocument":
		err = m.printTextDocument(a, now)
	case "CashInOutcome":
		err = m.cashInOutcome(a, now)
	case "PrintXReport":
		err = m.printXReport(a, now)
	case "PrintCheckCopy":
		err = m.printCheckCopy(a, now)
	case "GetCurrentStatus":
		err = m.getCurrentStatus(a, out, now)
	case "ReportCurrentStatusOfSettlements":
		err = m.reportSettlements(a, out, now)
	case "OpenCashDrawer":
		if err = a.Need(1); err == nil {
			m.st.drawerOpened++
			m.st.drawer.Opened(now)
		}
	case "GetLineLength":
		if err = a.Need(2); err == nil {
			out[1] = int64(m.st.lineLength)
		}
	case "OpenSessionRegistrationKM":
		if err = a.Need(1); err == nil {
			m.openKMSession()
		}
	case "CloseSessionRegistrationKM":
		if err = a.Need(1); err == nil {
			m.closeKMSession()
		}
	case "RequestKM":
		err = m.requestKM(a, out, now)
	case "GetProcessingKMResult":
		err = m.getProcessingKMResult(a, out, now)
	case "ConfirmKM":
		err = m.confirmKM(a)
	case "PiotSetChallenge":
		err = m.piotSetChallenge(a, out)
	case "PiotCheckChallenge":
		err = m.piotCheckChallenge(a, out)
	case "PiotSetDKMsg":
		err = m.piotSetDKMsg(a, out)
	case "PiotGetDKMsg":
		err = m.piotGetDKMsg(a, out)
	default:
		err = fail(ErrNotSupported, "%s", method)
	}

	if err != nil {
		var ke *kktError
		if !errors.As(err, &ke) {
			ke = fail(ErrBadParams, "%v", err) // ошибки разбора параметров
		}
		return models.Result{Return: false, ErrorCode: ke.code, ErrorText: ke.Error()}
	}
	return models.Result{Return: ret, Params: out}
}

// --- проверки состояния ---

func (m *Model) shiftState(now time.Time) int {
	switch {
	case !m.st.shiftOpen:
		return ShiftClosed
	case now.Sub(m.st.shiftOpenedAt) > shiftMaxDuration:
		return ShiftExpired
	default:
		return ShiftOpen
	}
}

// requireFiscal — ККТ зарегистрирована, ФН не закрыт и не исчерпан.
func (m *Model) requireFiscal() error {
	switch {
	case !bool(m.st.reg.Fiscal):
		return failCode(ErrNotRegistered)
	case m.st.fnClosed:
		return failCode(ErrFNClosed)
	case m.st.fnFail:
		return failCode(ErrFNFail)
	case m.st.fnOverflow:
		return failCode(ErrFNOverflow)
	}
	return nil
}

func (m *Model) requireOpenShift(now time.Time) error {
	switch m.shiftState(now) {
	case ShiftClosed:
		return failCode(ErrShiftClosed)
	case ShiftExpired:
		return fail(ErrShiftExpired, "смена №%d открыта %s", m.st.shiftNumber, m.st.shiftOpenedAt.Format("02.01.2006 15:04"))
	}
	return nil
}

// requirePrinter — бумага есть и крышка закрыта (если документ печатается).
func (m *Model) requirePrinter(print bool) error {
	if !print {
		return nil
	}
	if !m.st.paper {
		return failCode(ErrPaperOut)
	}
	if m.st.coverOpen {
		return failCode(ErrCoverOpen)
	}
	return nil
}

// nextDoc — следующий фискальный документ. Без связи с ОФД он копится в очереди.
func (m *Model) nextDoc(now time.Time) int {
	m.st.docNumber++
	if !m.st.ofdOnline {
		m.st.backlog = append(m.st.backlog, backlogDoc{Number: m.st.docNumber, At: now})
	}
	return m.st.docNumber
}

// fiscalSign — детерминированный «фискальный признак» (32 бита, как у ФН).
func (m *Model) fiscalSign(doc int, now time.Time, total float64) string {
	h := fnv.New32a()
	fmt.Fprintf(h, "%s|%d|%s|%.2f", m.st.reg.FNSerialNumber, doc, now.Format(DateTimeLayout), total)
	return strconv.FormatUint(uint64(h.Sum32()), 10)
}

func (m *Model) taxationAllowed(code int) bool {
	for _, s := range strings.Split(m.st.reg.TaxationSystems, ",") {
		if strings.TrimSpace(s) == strconv.Itoa(code) {
			return true
		}
	}
	return false
}

// statusParams — состояние смены для OutputParameters. Пишем все поля, которые
// БПО читает в одном разборе (ОЧУ-КС:2125): лишние для метода атрибуты ей не мешают.
func (m *Model) statusParams(now time.Time) ShiftStatus {
	st := &m.st
	s := ShiftStatus{
		ShiftNumber:             IntPtr(st.shiftNumber),
		CheckNumber:             IntPtr(st.docNumber),
		ShiftClosingCheckNumber: IntPtr(st.checksInShift),
		ShiftState:              m.shiftState(now),
		CashBalance:             NumberPtr(Kopecks(st.cash)),
		BacklogDocumentsCounter: IntPtr(len(st.backlog)),
		FNError:                 BoolPtr(st.fnError),
		FNOverflow:              BoolPtr(st.fnOverflow),
		FNFail:                  BoolPtr(st.fnFail),
		FNValidityDate:          DateTime{st.fnValidity},
	}
	if st.shiftOpen {
		s.DateTime = DateTime{st.shiftOpenedAt}
	}
	if len(st.backlog) > 0 {
		s.BacklogDocumentFirstNumber = IntPtr(st.backlog[0].Number)
		s.BacklogDocumentFirstDateTime = DateTime{st.backlog[0].At}
	}
	return s
}

// setOut пишет выходной параметр (XML-структуру) в позицию pos.
func setOut(a models.Args, out []any, pos int, v any) error {
	if err := a.Need(pos + 1); err != nil {
		return err
	}
	s, err := Marshal(v)
	if err != nil {
		return err
	}
	out[pos] = s
	return nil
}

// parseXML разбирает входную XML-структуру из позиции pos.
func parseXML(a models.Args, pos int, v any) error {
	s, err := a.String(pos)
	if err != nil {
		return err
	}
	if err := Unmarshal(s, v); err != nil {
		return fail(ErrBadData, "%v", err)
	}
	return nil
}

// inputParams — InputParameters из позиции pos; пустая строка допустима.
func inputParams(a models.Args, pos int) (OperationParams, error) {
	var in InputParameters
	s, err := a.String(pos)
	if err != nil {
		return in.Parameters, err
	}
	if strings.TrimSpace(s) == "" {
		return in.Parameters, nil
	}
	if err := Unmarshal(s, &in); err != nil {
		return in.Parameters, fail(ErrBadData, "InputParameters: %v", err)
	}
	return in.Parameters, nil
}

func printRequired(p OperationParams) bool {
	return p.PrintRequired == nil || bool(*p.PrintRequired)
}

// --- лента печати ---

func (m *Model) print(doc models.PrintedDocument) {
	m.tapeSeq++
	doc.Seq = m.tapeSeq
	m.tape = append(m.tape, doc)
	if len(m.tape) > tapeLimit {
		m.tape = append([]models.PrintedDocument(nil), m.tape[len(m.tape)-tapeLimit:]...)
	}
}

// Prints — последние документы, новые первыми.
func (m *Model) Prints(limit int) []models.PrintedDocument {
	m.mu.Lock()
	defer m.mu.Unlock()
	if limit <= 0 || limit > len(m.tape) {
		limit = len(m.tape)
	}
	out := make([]models.PrintedDocument, 0, limit)
	for i := len(m.tape) - 1; i >= 0 && len(out) < limit; i-- {
		out = append(out, m.tape[i])
	}
	return out
}

// PrintsSince — документы ленты с Seq > seq, по порядку печати.
func (m *Model) PrintsSince(seq int64) []models.PrintedDocument {
	m.mu.Lock()
	defer m.mu.Unlock()
	var out []models.PrintedDocument
	for _, d := range m.tape {
		if d.Seq > seq {
			out = append(out, d)
		}
	}
	return out
}

// --- состояние для Control API ---

func (m *Model) State() map[string]any {
	m.mu.Lock()
	defer m.mu.Unlock()
	now := m.opts.Now()
	st := &m.st
	shift := map[string]any{"state": m.shiftState(now), "number": st.shiftNumber, "checks": st.checksInShift}
	if st.shiftOpen {
		shift["opened_at"] = st.shiftOpenedAt
	}
	pending := make([]string, 0, len(st.kmRequests))
	for g := range st.kmRequests {
		pending = append(pending, g)
	}
	sort.Strings(pending)
	return map[string]any{
		"model":        "kkt",
		"fiscal":       bool(st.reg.Fiscal),
		"fn_closed":    st.fnClosed,
		"fn_serial":    st.reg.FNSerialNumber,
		"kkt_number":   st.reg.KKTNumber,
		"shift":        shift,
		"doc_number":   st.docNumber,
		"cash_balance": Kopecks(st.cash),
		"ofd":          map[string]any{"online": st.ofdOnline, "backlog": len(st.backlog)},
		"faults": map[string]any{
			"paper": st.paper, "cover_open": st.coverOpen,
			"fn_error": st.fnError, "fn_overflow": st.fnOverflow, "fn_fail": st.fnFail,
			"print_error_after_fiscal": st.printErrorAfterFiscal,
		},
		"km": map[string]any{
			"session": st.kmSession, "requests": pending, "result": st.kmResult, "rules": append([]KMRule{}, st.kmRules...),
			"delay_ms": st.kmDelay.Milliseconds(), "strict": st.markingStrict,
		},
		"piot":                st.piot,
		"line_length":         st.lineLength,
		"drawer_opened":       st.drawerOpened,
		"drawer_open":         st.drawer.IsOpen(m.opts.Now()),
		"drawer_auto_close_s": int(st.drawer.AutoClose / time.Second),
	}
}

// StateKeys — ключи SetState (docs/models/kkt.md).
var StateKeys = []string{
	"paper", "cover_open", "ofd_online", "fn_error", "fn_overflow", "fn_fail",
	"print_error_after_fiscal", "fiscal", "cash_balance", "shift_opened_hours_ago",
	"line_length", "taxation_systems", "km_result", "km_rules", "km_delay_ms", "marking_strict",
	"drawer_open", "drawer_auto_close_s", "piot",
}

// SetState меняет состояние из Control API. Все ключи применяются вместе или ни один.
func (m *Model) SetState(patch map[string]any) error {
	m.mu.Lock()
	defer m.mu.Unlock()
	now := m.opts.Now()
	st := m.st // копия: применяем к ней и сохраняем, только если ошибок нет

	keys := make([]string, 0, len(patch))
	for k := range patch {
		keys = append(keys, k)
	}
	sort.Strings(keys) // fiscal раньше shift_* — детерминированный порядок
	for _, k := range keys {
		v := patch[k]
		var err error
		switch k {
		case "paper":
			st.paper, err = asBool(k, v)
		case "cover_open":
			st.coverOpen, err = asBool(k, v)
		case "ofd_online":
			if st.ofdOnline, err = asBool(k, v); err == nil && st.ofdOnline {
				st.backlog = nil // связь восстановилась — очередь передана
			}
		case "fn_error":
			st.fnError, err = asBool(k, v)
		case "fn_overflow":
			st.fnOverflow, err = asBool(k, v)
		case "fn_fail":
			st.fnFail, err = asBool(k, v)
		case "print_error_after_fiscal":
			st.printErrorAfterFiscal, err = asBool(k, v)
		case "marking_strict":
			st.markingStrict, err = asBool(k, v)
		case "piot":
			st.piot, err = asBool(k, v)
		case "fiscal":
			var on bool
			if on, err = asBool(k, v); err == nil {
				if on && !bool(st.reg.Fiscal) {
					m.registerDefaults(&st, now)
				} else if !on {
					st.reg.Fiscal = false
					st.shiftOpen = false
				}
			}
		case "cash_balance":
			var f float64
			if f, err = asNumber(k, v); err == nil {
				if f < 0 {
					err = fmt.Errorf("%s: не может быть отрицательным", k)
				}
				st.cash = Kopecks(f)
			}
		case "shift_opened_hours_ago":
			var h float64
			if h, err = asNumber(k, v); err == nil {
				if !st.shiftOpen {
					err = fmt.Errorf("%s: смена не открыта", k)
				}
				st.shiftOpenedAt = now.Add(-time.Duration(h * float64(time.Hour)))
			}
		case "line_length":
			var n float64
			if n, err = asNumber(k, v); err == nil {
				if n < 20 || n > 80 {
					err = fmt.Errorf("%s: от 20 до 80", k)
				}
				st.lineLength = int(n)
			}
		case "taxation_systems":
			s, ok := v.(string)
			if !ok {
				err = fmt.Errorf("%s: ожидается строка кодов через запятую", k)
			}
			st.reg.TaxationSystems = s
		case "km_result":
			s, _ := v.(string)
			if !validKMResult(s) {
				err = fmt.Errorf("%s: одно из %s, %s, %s, %s", k, kmValid, kmInvalid, kmSuspended, kmUnavailable)
			}
			st.kmResult = s
		case "km_rules":
			st.kmRules, err = parseKMRules(v)
		case "km_delay_ms":
			var n float64
			if n, err = asNumber(k, v); err == nil {
				st.kmDelay = time.Duration(n) * time.Millisecond
			}
		default:
			if ok, e := st.drawer.Set(k, v, now); ok {
				err = e
			} else {
				err = fmt.Errorf("неизвестный ключ %q; допустимы: %s", k, strings.Join(StateKeys, ", "))
			}
		}
		if err != nil {
			return err
		}
	}
	m.st = st
	return nil
}

func asBool(key string, v any) (bool, error) {
	b, ok := v.(bool)
	if !ok {
		return false, fmt.Errorf("%s: ожидается true или false", key)
	}
	return b, nil
}

func asNumber(key string, v any) (float64, error) {
	switch n := v.(type) {
	case float64:
		return n, nil
	case int:
		return float64(n), nil
	case interface{ Float64() (float64, error) }: // json.Number
		return n.Float64()
	}
	return 0, fmt.Errorf("%s: ожидается число", key)
}
