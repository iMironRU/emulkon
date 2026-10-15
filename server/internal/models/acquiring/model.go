package acquiring

import (
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"hash/fnv"
	"math"
	"sort"
	"strings"
	"sync"
	"time"

	"emulkon/internal/models"
	"emulkon/internal/models/kkt"
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
	bankName           = "ТЕСТ-БАНК (ЭМУЛЯТОР)"
	bankID             = "00000000001"
	bankBIK            = "044525000"
	merchantName       = "ООО «ТЕСТ ЭМУЛКОН»"
	tapeLimit          = 200
	opsLimit           = 1000 // операций в пакете дня; больше — сверка итогов давно не делалась
	defaultCardTimeout = 60 * time.Second
)

// Режимы «прикладывания» карты (card_mode).
const (
	modeInteractive = "interactive"
	modeApprove     = "approve"
	modeDecline     = "decline"
	modeTimeout     = "timeout"
)

// Действия на экране терминала (models.Prompt.Actions).
const (
	actCard         = "card"
	actDecline      = "decline"
	actInsufficient = "insufficient"
	actWrongPIN     = "wrong_pin"
	actCancel       = "cancel"
	actTimeout      = models.PromptTimeout // никто не ответил
	actReset        = models.PromptReset   // запрос снят сбросом состояния
)

// cardInfo — тестовая карта. Номера — только тестовые и сразу маскированные.
type cardInfo struct {
	Key      string
	Title    string // на кнопке экрана
	Type     string // ТипКарты и строка слипа
	Masked   string
	PAN      string // тестовый PAN — только для хеша CardNumberHash
	PSID     string // PaymentSystemIdentifier: 4 Visa, 5 MasterCard, 6 UnionPay, 22 Мир
	AuthType int    // AuthorizationType: 0 — карта, 1 — СБП QR
}

var cardList = []cardInfo{
	{"mir", "МИР", "МИР", "220070******4417", "2200700000004417", "22", 0},
	{"visa", "VISA", "VISA", "411111******1111", "4111111111111111", "4", 0},
	{"mastercard", "Mastercard", "MASTERCARD", "555555******4444", "5555555555554444", "5", 0},
	{"unionpay", "UnionPay", "UNIONPAY", "620000******0000", "6200000000000000", "6", 0},
	{"sbp", "СБП (QR)", "СБП", "+7 9** ***-12-34", "", "", 1},
}

func findCard(key string) (cardInfo, bool) {
	for _, c := range cardList {
		if c.Key == key {
			return c, true
		}
	}
	return cardInfo{}, false
}

func (c cardInfo) hash() string {
	if c.PAN == "" {
		return ""
	}
	s := sha256.Sum256([]byte(c.PAN))
	return strings.ToUpper(hex.EncodeToString(s[:]))
}

// operation — операция в пакете дня (до сверки итогов).
type operation struct {
	Type      string // имя метода: Pay, ReturnPayment, CancelPayment, …
	RequestID string
	RRN       string
	AuthCode  string
	Receipt   string
	UUID      string
	Amount    float64
	Cash      float64
	Card      cardInfo
	At        time.Time
	Merchant  int
	Ref       string  // RRN исходной операции (отмена, возврат, завершение преавторизации)
	Voided    bool    // отменена: оплата, возврат, преавторизация
	Completed bool    // преавторизация завершена
	Refunded  float64 // возвраты и частичные отмены по оплате
}

type lastResult struct {
	Operation string    `json:"operation"`
	Title     string    `json:"title"`
	Amount    float64   `json:"amount"`
	Approved  bool      `json:"approved"`
	Text      string    `json:"text"`
	Card      string    `json:"card,omitempty"`
	RRN       string    `json:"rrn,omitempty"`
	At        time.Time `json:"at"`
}

// state — настройки и пакет дня. Копируется при SetState (транзакционно).
type state struct {
	cardMode             string
	card                 string
	cardTimeout          time.Duration
	bankOnline           bool
	printSlipOnTerminal  bool
	shortSlip            bool
	cashWithdrawal       bool
	partialCancellation  bool
	listCardTransactions bool
	// Исход без человека — прямо из 1С (docs/models/acquiring.md, «Исход по сумме и карте»).
	kopeckRules  bool    // копейки .61–.65 — свой отказ
	declineAbove float64 // сумма больше — отказ банка; 0 — выключено

	batch      int
	receiptSeq int
	rrnSeq     int
	ops        []*operation
	lastCard   *cardInfo
	lastResult *lastResult
	lastSlip   []string
}

// Model — эмулятор эквайрингового терминала (раздел 3.9 требований, ревизия 4008).
type Model struct {
	id         string
	opts       Options
	terminalID string
	merchantID string
	rrnBase    int

	models.Screen // экран терминала: Respond, CancelPrompt, SetPromptHook

	mu      sync.Mutex
	st      state
	tape    []models.PrintedDocument
	tapeSeq int64
}

// New создаёт терминал: связь с банком есть, карта прикладывается на экране.
func New(deviceID string, opts Options) *Model {
	if opts.Now == nil {
		opts.Now = time.Now
	}
	h := fnv.New64a()
	h.Write([]byte("acquiring" + deviceID))
	n := h.Sum64()
	m := &Model{
		id: deviceID, opts: opts, Screen: models.Screen{DeviceID: deviceID},
		terminalID: fmt.Sprintf("EMU%05d", n%100000),
		merchantID: fmt.Sprintf("77%010d", (n/100000)%10000000000),
		rrnBase:    int((n / 7) % 900000),
	}
	m.st = defaultState()
	return m
}

func defaultState() state {
	return state{
		cardMode: modeInteractive, card: "mir", cardTimeout: defaultCardTimeout,
		bankOnline: true, cashWithdrawal: true, partialCancellation: true, listCardTransactions: true,
		batch: 1,
	}
}

// --- models.Model ---

func (m *Model) Description() models.Description {
	return models.Description{
		Name:        "ЭМУЛЯТОР: эквайринговый терминал (Эмулкон)",
		Description: "Виртуальный эквайринговый терминал Эмулкон. Деньги не списываются, карты только тестовые.",
		Version:     m.opts.Version,
		IsEmulator:  true,
	}
}

func (m *Model) Reset() {
	m.CancelPrompt()
	m.mu.Lock()
	defer m.mu.Unlock()
	m.st = defaultState()
	m.tape = nil
	m.tapeSeq = 0
}

// Handle — вызов метода драйвера (docs/bpo-requirements/pos-terminal.md). Операции с картой
// ждут человека без блокировки модели: экран, ответ и State доступны во время ожидания.
func (m *Model) Handle(ctx context.Context, method string, params []any) models.Result {
	if _, ok := legacyMethods[method]; ok {
		return m.legacyOperation(ctx, method, params) // старая схема (legacy.go)
	}
	a := models.Args(params)
	out := models.CopyParams(params)
	var err error
	switch method {
	case "ConnectEquipment", "DisconnectEquipment":
	case "EquipmentTest":
		err = m.equipmentTest(a, out)
	case "TerminalParamters":
		err = m.terminalParameters(a, out)
	case "Pay", "ReturnPayment", "PayWithCashWithdrawal", "Authorisation":
		err = m.cardOperation(method, a, out)
	case "CancelPayment", "AuthConfirmation", "CancelAuthorisation":
		err = m.refOperation(method, a, out)
	case "EmergencyReversal":
		err = m.emergencyReversal(a)
	case "OperationStatus":
		err = m.operationStatus(a, out)
	case "GetOperationByCards":
		err = m.operationsByCards(a, out)
	case "Settlement":
		err = m.settlement(a, out)
	case "GetCardParametrs":
		err = m.cardParameters(a, out)
	default: // PurchaseWithEnrollment, PayCertificate, ReturnCertificate и всё неизвестное
		err = fail(ErrNotSupported, "%s", method)
	}
	if err != nil {
		var te *termError
		if !errors.As(err, &te) {
			te = fail(ErrBadParams, "%v", err) // ошибки разбора параметров
		}
		// Params и при отказе: БПО забирает слип отказа (позиция 3).
		return models.Result{Return: false, Params: out, ErrorCode: te.code, ErrorText: te.Error()}
	}
	return models.Result{Return: true, Params: out}
}

func modeTitle(mode string) string {
	switch mode {
	case modeApprove:
		return "одобрять сразу"
	case modeDecline:
		return "отказывать сразу"
	case modeTimeout:
		return "карту не прикладывают"
	}
	return "карту прикладывают на экране терминала"
}

func (m *Model) equipmentTest(a models.Args, out []any) error {
	if err := a.Need(4); err != nil {
		return err
	}
	m.mu.Lock()
	defer m.mu.Unlock()
	out[2] = fmt.Sprintf("Тест пройден. ЭМУЛЯТОР эквайрингового терминала (Эмулкон): %s, терминал %s, режим — %s.",
		bankName, m.terminalID, modeTitle(m.st.cardMode))
	out[3] = "ЭМУЛЯТОР Эмулкон"
	return nil
}

func (m *Model) terminalParameters(a models.Args, out []any) error {
	if err := a.Need(2); err != nil {
		return err
	}
	m.mu.Lock()
	tp := TerminalParameters{
		TerminalID: m.terminalID, MerchantID: m.merchantID, AcquiringBankIdentifier: bankID,
		PrintSlipOnTerminal: kkt.Bool(m.st.printSlipOnTerminal), ShortSlip: kkt.Bool(m.st.shortSlip),
		CashWithdrawal: kkt.Bool(m.st.cashWithdrawal), PartialCancellation: kkt.Bool(m.st.partialCancellation),
		ListCardTransactions: kkt.Bool(m.st.listCardTransactions), OperationStatus: kkt.Bool(m.st.listCardTransactions),
	}
	m.mu.Unlock()
	s, err := kkt.Marshal(tp)
	if err != nil {
		return err
	}
	out[1] = s
	return nil
}

// --- операции ---

var titles = map[string]string{
	"Pay":                   "ОПЛАТА",
	"ReturnPayment":         "ВОЗВРАТ",
	"CancelPayment":         "ОТМЕНА",
	"PayWithCashWithdrawal": "ОПЛАТА С ВЫДАЧЕЙ НАЛИЧНЫХ",
	"Authorisation":         "ПРЕАВТОРИЗАЦИЯ",
	"AuthConfirmation":      "ЗАВЕРШЕНИЕ ПРЕАВТОРИЗАЦИИ",
	"CancelAuthorisation":   "ОТМЕНА ПРЕАВТОРИЗАЦИИ",
	"EmergencyReversal":     "АВАРИЙНАЯ ОТМЕНА",
	"GetCardParametrs":      "ЧТЕНИЕ КАРТЫ",
}

func parseIn(a models.Args) (OperationIn, error) {
	var in OperationIn
	s, err := a.String(1)
	if err != nil {
		return in, err
	}
	if err := kkt.Unmarshal(s, &in); err != nil {
		return in, fail(ErrBadParams, "%v", err)
	}
	return in, nil
}

// cardOperation — оплата, возврат, оплата с выдачей наличных, преавторизация: нужна карта.
func (m *Model) cardOperation(method string, a models.Args, out []any) error {
	if err := a.Need(4); err != nil {
		return err
	}
	in, err := parseIn(a)
	if err != nil {
		return err
	}
	amount, cash := kkt.Kopecks(float64(in.Amount)), kkt.Kopecks(float64(in.AmountCash))
	if amount <= 0 {
		return fail(ErrBadParams, "сумма операции (Amount) должна быть больше 0, получено %v", amount)
	}
	switch {
	case method != "PayWithCashWithdrawal":
		cash = 0 // БПО передаёт AmountCash всегда, наличные выдаются только в этой операции
	case cash < 0:
		return fail(ErrBadParams, "сумма наличных (AmountCash) не может быть отрицательной: %v", cash)
	}

	// Проверки до карты: без связи с банком и без исходной операции экран не открываем.
	m.mu.Lock()
	if method == "PayWithCashWithdrawal" && !m.st.cashWithdrawal {
		m.mu.Unlock()
		return fail(ErrNotSupported, "выдача наличных выключена (cash_withdrawal)")
	}
	if !m.st.bankOnline {
		out[3] = m.declineSlip(method, amount, nil, ErrBankOffline)
		m.mu.Unlock()
		return failCode(ErrBankOffline)
	}
	if method == "ReturnPayment" && in.RRNCode != "" {
		if orig := m.find(in.RRNCode, refundable...); orig != nil {
			if err := checkRefund(orig, amount); err != nil {
				out[3] = m.declineSlip(method, amount, nil, err.code)
				m.mu.Unlock()
				return err
			}
		}
	}
	m.mu.Unlock()

	actions := payActions
	if method == "ReturnPayment" {
		actions = returnActions
	}
	res := m.awaitCard(method, amount, cash, in.CardNumber, actions)

	m.mu.Lock()
	defer m.mu.Unlock()
	now := m.opts.Now()
	if res.Action != actCard {
		code := declineCode(res.Action)
		var card *cardInfo
		if c, ok := findCard(res.Card); ok {
			card = &c
		}
		out[3] = m.declineSlip(method, amount, card, code)
		m.setLast(method, amount, false, errorTexts[code], "", "", now)
		return failCode(code)
	}
	card, _ := findCard(res.Card)
	var orig *operation
	if method == "ReturnPayment" && in.RRNCode != "" {
		// Пока ждали карту, исходную могли отменить — проверяем ещё раз.
		if orig = m.find(in.RRNCode, refundable...); orig != nil {
			if err := checkRefund(orig, amount); err != nil {
				out[3] = m.declineSlip(method, amount, &card, err.code)
				return err
			}
		}
	}
	op := m.newOperation(method, in, amount, cash, card, now)
	if orig != nil {
		orig.Refunded = kkt.Kopecks(orig.Refunded + amount)
		op.Ref = orig.RRN
	}
	return m.complete(op, out)
}

// refOperation — отмена оплаты, завершение и отмена преавторизации: по RRN, без карты.
func (m *Model) refOperation(method string, a models.Args, out []any) error {
	if err := a.Need(4); err != nil {
		return err
	}
	in, err := parseIn(a)
	if err != nil {
		return err
	}
	amount := kkt.Kopecks(float64(in.Amount))
	m.mu.Lock()
	defer m.mu.Unlock()
	now := m.opts.Now()
	if !m.st.bankOnline {
		out[3] = m.declineSlip(method, amount, nil, ErrBankOffline)
		return failCode(ErrBankOffline)
	}
	if in.RRNCode == "" {
		return fail(ErrBadParams, "не передан RRNCode исходной операции")
	}
	types := voidable
	if method != "CancelPayment" {
		types = []string{"Authorisation"}
	}
	orig := m.find(in.RRNCode, types...)
	if orig == nil {
		out[3] = m.declineSlip(method, amount, nil, ErrNotFound)
		return fail(ErrNotFound, "RRN %s нет в пакете дня терминала", in.RRNCode)
	}
	if orig.Voided || orig.Completed {
		out[3] = m.declineSlip(method, amount, &orig.Card, ErrAlreadyVoided)
		return failCode(ErrAlreadyVoided)
	}
	switch method {
	case "CancelPayment":
		rest := kkt.Kopecks(orig.Amount - orig.Refunded)
		if amount <= 0 {
			amount = rest
		}
		if amount > rest {
			out[3] = m.declineSlip(method, amount, &orig.Card, ErrAmountExceeded)
			return fail(ErrAmountExceeded, "отмена %.2f, доступно %.2f", amount, rest)
		}
		if amount < rest {
			if !m.st.partialCancellation {
				return fail(ErrNotSupported, "частичная отмена выключена (partial_cancellation)")
			}
			orig.Refunded = kkt.Kopecks(orig.Refunded + amount)
		} else {
			orig.Voided = true
		}
	case "AuthConfirmation":
		if amount <= 0 {
			amount = orig.Amount
		}
		if amount > orig.Amount {
			out[3] = m.declineSlip(method, amount, &orig.Card, ErrAmountExceeded)
			return fail(ErrAmountExceeded, "списание %.2f, заблокировано %.2f", amount, orig.Amount)
		}
		orig.Completed = true
	case "CancelAuthorisation":
		amount = orig.Amount
		orig.Voided = true
	}
	op := m.newOperation(method, in, amount, 0, orig.Card, now)
	// По [Т] RRN и код авторизации у этих операций — входные: остаются от исходной.
	op.RRN, op.AuthCode, op.Ref = orig.RRN, orig.AuthCode, orig.RRN
	return m.complete(op, out)
}

// complete — операция одобрена: в пакет дня, выходные параметры, слип.
func (m *Model) complete(op *operation, out []any) error {
	if len(m.st.ops) >= opsLimit {
		m.st.ops = m.st.ops[1:]
	}
	m.st.ops = append(m.st.ops, op)
	c := op.Card
	m.st.lastCard = &c
	s, err := kkt.Marshal(outParams(op))
	if err != nil {
		return err
	}
	out[2] = s
	out[3] = m.approvedSlip(op)
	m.setLast(op.Type, op.Amount, true, "ОДОБРЕНО", op.Card.Type+" "+op.Card.Masked, op.RRN, op.At)
	return nil
}

func (m *Model) emergencyReversal(a models.Args) error {
	if err := a.Need(1); err != nil {
		return err
	}
	m.mu.Lock()
	defer m.mu.Unlock()
	if !m.st.bankOnline {
		return failCode(ErrBankOffline)
	}
	for i := len(m.st.ops) - 1; i >= 0; i-- {
		orig := m.st.ops[i]
		if !contains(voidable, orig.Type) && orig.Type != "Authorisation" {
			continue
		}
		if orig.Voided {
			break // последняя операция уже отменена — отменять нечего
		}
		orig.Voided = true
		now := m.opts.Now()
		op := m.newOperation("EmergencyReversal", OperationIn{}, orig.Amount, orig.Cash, orig.Card, now)
		op.RRN, op.AuthCode, op.Ref = orig.RRN, orig.AuthCode, orig.RRN
		m.st.ops = append(m.st.ops, op)
		m.approvedSlip(op) // на ленту, если терминал печатает сам
		m.setLast(op.Type, op.Amount, true, "ОТМЕНЕНО", op.Card.Type+" "+op.Card.Masked, op.RRN, now)
		return nil
	}
	return fail(ErrNotFound, "нет операции для аварийной отмены")
}

func (m *Model) operationStatus(a models.Args, out []any) error {
	if err := a.Need(4); err != nil {
		return err
	}
	id, err := a.String(1)
	if err != nil {
		return err
	}
	m.mu.Lock()
	defer m.mu.Unlock()
	for i := len(m.st.ops) - 1; i >= 0; i-- {
		if op := m.st.ops[i]; id != "" && op.RequestID == id {
			s, err := kkt.Marshal(outParams(op))
			if err != nil {
				return err
			}
			out[2], out[3] = s, int64(1)
			return nil
		}
	}
	out[2], out[3] = "", int64(0)
	return nil
}

func (m *Model) operationsByCards(a models.Args, out []any) error {
	if err := a.Need(2); err != nil {
		return err
	}
	m.mu.Lock()
	t := TableOperations{Records: []OperationRecord{}}
	for _, op := range m.st.ops {
		typ := op.Type
		if typ == "EmergencyReversal" {
			typ = "CancelPayment" // в TypeOperation по [Т] аварийной отмены нет
		}
		t.Records = append(t.Records, OperationRecord{
			MerchantNumber: op.Merchant, CardNumber: op.Card.Masked, CardNumberHash: op.Card.hash(),
			Amount: kkt.Number(op.Amount), AmountCash: kkt.Number(op.Cash), TypeOperation: typ,
			AuthorizationCode: op.AuthCode, RRNCode: op.RRN, DateTime: kkt.DateTime{Time: op.At},
		})
	}
	m.mu.Unlock()
	s, err := kkt.Marshal(t)
	if err != nil {
		return err
	}
	out[1] = s
	return nil
}

func (m *Model) settlement(a models.Args, out []any) error {
	if err := a.Need(2); err != nil {
		return err
	}
	m.mu.Lock()
	defer m.mu.Unlock()
	if !m.st.bankOnline {
		return failCode(ErrBankOffline)
	}
	now := m.opts.Now()
	out[1] = m.settlementSlip(now)
	m.setLast("Settlement", 0, true, fmt.Sprintf("СВЕРКА ИТОГОВ: ПАКЕТ %d ЗАКРЫТ", m.st.batch), "", "", now)
	m.st.ops = nil
	m.st.batch++
	m.st.receiptSeq = 0
	return nil
}

// cardParameters — GetCardParametrs: 0 DeviceID · 1 QR · 2 FromLastOperation · 3 CardNumber ·
// 4 CardNumberHash · 5 PaymentAccountReference · 6 CardType · 7 IsOwnCard.
func (m *Model) cardParameters(a models.Args, out []any) error {
	if err := a.Need(8); err != nil {
		return err
	}
	fromLast, err := a.Bool(2)
	if err != nil {
		return err
	}
	var card cardInfo
	if fromLast {
		m.mu.Lock()
		last := m.st.lastCard
		m.mu.Unlock()
		if last == nil {
			return fail(ErrNotFound, "операций по картам ещё не было")
		}
		card = *last
	} else {
		res := m.awaitCard("GetCardParametrs", 0, 0, "", readActions)
		if res.Action != actCard {
			return failCode(declineCode(res.Action))
		}
		card, _ = findCard(res.Card)
		m.mu.Lock()
		m.st.lastCard = &card
		m.setLast("GetCardParametrs", 0, true, "КАРТА ПРОЧИТАНА", card.Type+" "+card.Masked, "", m.opts.Now())
		m.mu.Unlock()
	}
	out[3], out[4], out[6], out[7] = card.Masked, card.hash(), card.Type, int64(1)
	if h := card.hash(); h != "" {
		out[5] = "EMU" + h[:26] // PAR — 29 символов
	} else {
		out[5] = ""
	}
	return nil
}

// --- ожидание карты на экране ---

var (
	payActions = []models.PromptOption{
		{ID: actCard, Title: "Приложить карту", Kind: "primary"},
		{ID: actDecline, Title: "Отказ банка", Kind: "danger"},
		{ID: actInsufficient, Title: "Недостаточно средств", Kind: "danger"},
		{ID: actWrongPIN, Title: "Неверный ПИН", Kind: "danger"},
		{ID: actCancel, Title: "Отмена"},
	}
	returnActions = []models.PromptOption{
		{ID: actCard, Title: "Приложить карту", Kind: "primary"},
		{ID: actDecline, Title: "Отказ банка", Kind: "danger"},
		{ID: actCancel, Title: "Отмена"},
	}
	readActions = []models.PromptOption{
		{ID: actCard, Title: "Приложить карту", Kind: "primary"},
		{ID: actCancel, Title: "Отмена"},
	}
)

func cardOptions() []models.PromptOption {
	out := make([]models.PromptOption, 0, len(cardList))
	for _, c := range cardList {
		out = append(out, models.PromptOption{ID: c.Key, Title: c.Title})
	}
	return out
}

func declineCode(action string) int {
	switch action {
	case actDecline:
		return ErrDeclined
	case actInsufficient:
		return ErrInsufficient
	case actWrongPIN:
		return ErrWrongPIN
	case actTimeout:
		return ErrTimeout
	}
	return ErrCancelled // cancel, reset
}

// awaitCard — получить карту: по правилам суммы и тестовой карты, сразу (card_mode ≠ interactive)
// или с экрана терминала. Вызывается без блокировки модели.
func (m *Model) awaitCard(method string, amount, cash float64, pan string, actions []models.PromptOption) models.PromptResult {
	m.mu.Lock()
	if action, ok := ruleOutcome(m.st, amount, pan); ok {
		m.mu.Unlock()
		return models.PromptResult{Action: action}
	}
	mode, card, timeout := m.st.cardMode, m.st.card, m.st.cardTimeout
	switch mode {
	case modeApprove:
		m.mu.Unlock()
		return models.PromptResult{Action: actCard, Card: card}
	case modeDecline:
		m.mu.Unlock()
		return models.PromptResult{Action: actDecline}
	case modeTimeout:
		m.mu.Unlock()
		return models.PromptResult{Action: actTimeout}
	}
	m.mu.Unlock()
	text := "Приложите, вставьте или проведите карту"
	if method == "ReturnPayment" {
		text = "Приложите карту, на которую возвращаются деньги"
	}
	p := models.Prompt{
		Operation: method, Title: titles[method], Text: text, Amount: amount, AmountCash: cash,
		Actions: actions, Cards: cardOptions(), DefaultCard: card,
	}
	return m.Ask(p, m.opts.Now(), timeout, func(r models.PromptResult) string {
		if r.Action == actCard {
			return "ОДОБРЕНО"
		}
		return "ОТКАЗ: " + errorTexts[declineCode(r.Action)]
	})
}

// Исход по сумме и тестовой карте: тестировщик управляет терминалом прямо из 1С — суммой
// или номером карты, без экрана и Control API (как «магические» суммы платёжных песочниц).
var kopeckOutcomes = map[int]string{61: actDecline, 62: actInsufficient, 63: actWrongPIN, 64: actCancel, 65: actTimeout}

// testCards — тестовые номера карт с отказом, если 1С передала CardNumber (docs/models/acquiring.md).
var testCards = map[string]string{
	"4000000000000002": actDecline,
	"4000000000009995": actInsufficient,
	"4000000000000127": actWrongPIN,
}

func ruleOutcome(st state, amount float64, pan string) (string, bool) {
	if a, ok := testCards[strings.ReplaceAll(pan, " ", "")]; ok {
		return a, true
	}
	if amount <= 0 {
		return "", false
	}
	if st.kopeckRules {
		if a, ok := kopeckOutcomes[int(math.Round(amount*100))%100]; ok {
			return a, true
		}
	}
	if st.declineAbove > 0 && amount > st.declineAbove {
		return actDecline, true
	}
	return "", false
}

// --- пакет дня ---

// voidable — что можно отменить (CancelPayment) и аварийно отменить.
var voidable = []string{"Pay", "PayWithCashWithdrawal", "ReturnPayment", "AuthConfirmation"}

// refundable — по чему делается возврат.
var refundable = []string{"Pay", "PayWithCashWithdrawal", "AuthConfirmation"}

func contains(list []string, s string) bool {
	for _, v := range list {
		if v == s {
			return true
		}
	}
	return false
}

// find — последняя операция пакета дня с этим RRN и одним из типов.
func (m *Model) find(rrn string, types ...string) *operation {
	for i := len(m.st.ops) - 1; i >= 0; i-- {
		if op := m.st.ops[i]; op.RRN == rrn && contains(types, op.Type) {
			return op
		}
	}
	return nil
}

func checkRefund(orig *operation, amount float64) *termError {
	if orig.Voided {
		return fail(ErrAlreadyVoided, "исходная операция RRN %s отменена", orig.RRN)
	}
	if rest := kkt.Kopecks(orig.Amount - orig.Refunded); amount > rest {
		return fail(ErrAmountExceeded, "возврат %.2f, доступно %.2f", amount, rest)
	}
	return nil
}

func (m *Model) newOperation(typ string, in OperationIn, amount, cash float64, card cardInfo, now time.Time) *operation {
	m.st.rrnSeq++
	m.st.receiptSeq++
	// RRN — 12 цифр по образцу YDDDHH + номер: год, день года, час, порядковый номер.
	rrn := fmt.Sprintf("%d%03d%02d%06d", now.Year()%10, now.YearDay(), now.Hour(), (m.rrnBase+m.st.rrnSeq)%1000000)
	sum := sha256.Sum256([]byte(m.id + "|" + rrn + "|" + now.String()))
	h := strings.ToUpper(hex.EncodeToString(sum[:]))
	return &operation{
		Type: typ, RequestID: in.RequestID, RRN: rrn, AuthCode: h[:6],
		Receipt: fmt.Sprintf("%04d", m.st.receiptSeq),
		UUID:    strings.ToLower(h[6:14] + "-" + h[14:18] + "-4" + h[19:22] + "-a" + h[23:26] + "-" + h[26:38]),
		Amount:  amount, Cash: cash, Card: card, At: now, Merchant: int(in.MerchantNumber),
	}
}

func outParams(op *operation) OperationOut {
	o := OperationOut{
		RequestID: op.RequestID, MerchantNumber: op.Merchant,
		Amount: kkt.Number(op.Amount), AmountCash: kkt.Number(op.Cash),
		AuthorizationType: op.Card.AuthType, CardNumber: op.Card.Masked, CardNumberHash: op.Card.hash(),
		ReceiptNumber: op.Receipt, RRNCode: op.RRN, AuthorizationCode: op.AuthCode,
		Date: kkt.DateTime{Time: op.At}, PaymentSystemIdentifier: op.Card.PSID,
		IssuerBankIdentifier: bankID, BIKAcquiring: bankBIK, BIKPay: bankBIK, UUID: op.UUID,
	}
	if op.Card.AuthType == 1 {
		o.QRTransactionIdentifier = "EMUQR" + op.RRN
	}
	return o
}

func (m *Model) setLast(op string, amount float64, approved bool, text, card, rrn string, at time.Time) {
	title := titles[op]
	if title == "" && op == "Settlement" {
		title = "СВЕРКА ИТОГОВ"
	}
	m.st.lastResult = &lastResult{Operation: op, Title: title, Amount: amount, Approved: approved, Text: text,
		Card: card, RRN: rrn, At: at}
}

// --- лента (models.Printer): только если терминал печатает слипы сам ---

func (m *Model) addTape(kind string, lines []string, at time.Time) {
	m.st.lastSlip = lines
	if !m.st.printSlipOnTerminal {
		return
	}
	m.tapeSeq++
	m.tape = append(m.tape, models.PrintedDocument{Seq: m.tapeSeq, Kind: kind, Time: at, Lines: lines})
	if len(m.tape) > tapeLimit {
		m.tape = m.tape[len(m.tape)-tapeLimit:]
	}
}

func (m *Model) Prints(limit int) []models.PrintedDocument {
	m.mu.Lock()
	defer m.mu.Unlock()
	out := []models.PrintedDocument{}
	for i := len(m.tape) - 1; i >= 0 && len(out) < limit; i-- {
		out = append(out, m.tape[i])
	}
	return out
}

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
	ops := make([]map[string]any, 0, len(m.st.ops))
	for i := len(m.st.ops) - 1; i >= 0 && len(ops) < 50; i-- {
		op := m.st.ops[i]
		status := "одобрена"
		switch {
		case op.Voided:
			status = "отменена"
		case op.Completed:
			status = "завершена"
		case op.Refunded > 0:
			status = fmt.Sprintf("возвращено %.2f", op.Refunded)
		}
		ops = append(ops, map[string]any{
			"type": op.Type, "title": titles[op.Type], "amount": op.Amount, "amount_cash": op.Cash,
			"card": op.Card.Type + " " + op.Card.Masked, "rrn": op.RRN, "auth_code": op.AuthCode,
			"receipt": op.Receipt, "request_id": op.RequestID, "at": op.At, "status": status,
		})
	}
	var prompt any
	if p := m.Current(); p != nil {
		prompt = *p
	}
	return map[string]any{
		"model":                  "acquiring",
		"bank":                   bankName,
		"terminal_id":            m.terminalID,
		"merchant_id":            m.merchantID,
		"card_mode":              m.st.cardMode,
		"kopeck_rules":           m.st.kopeckRules,
		"decline_above":          m.st.declineAbove,
		"card":                   m.st.card,
		"cards":                  cardOptions(),
		"card_timeout_s":         int(m.st.cardTimeout / time.Second),
		"bank_online":            m.st.bankOnline,
		"print_slip_on_terminal": m.st.printSlipOnTerminal,
		"short_slip":             m.st.shortSlip,
		"cash_withdrawal":        m.st.cashWithdrawal,
		"partial_cancellation":   m.st.partialCancellation,
		"list_card_transactions": m.st.listCardTransactions,
		"batch":                  m.st.batch,
		"prompt":                 prompt,
		"last_result":            m.st.lastResult,
		"last_slip":              m.st.lastSlip,
		"operations":             ops,
	}
}

// StateKeys — ключи SetState (docs/models/acquiring.md).
var StateKeys = []string{
	"card_mode", "card", "card_timeout_s", "bank_online", "print_slip_on_terminal", "short_slip",
	"cash_withdrawal", "partial_cancellation", "list_card_transactions", "kopeck_rules", "decline_above",
}

// SetState меняет состояние из Control API. Все ключи применяются вместе или ни один.
func (m *Model) SetState(patch map[string]any) error {
	m.mu.Lock()
	defer m.mu.Unlock()
	st := m.st
	keys := make([]string, 0, len(patch))
	for k := range patch {
		keys = append(keys, k)
	}
	sort.Strings(keys)
	for _, k := range keys {
		v := patch[k]
		var err error
		switch k {
		case "card_mode":
			s, _ := v.(string)
			if s != modeInteractive && s != modeApprove && s != modeDecline && s != modeTimeout {
				err = fmt.Errorf("%s: одно из %s, %s, %s, %s", k, modeInteractive, modeApprove, modeDecline, modeTimeout)
			}
			st.cardMode = s
		case "card":
			s, _ := v.(string)
			if _, ok := findCard(s); !ok {
				err = fmt.Errorf("%s: одна из mir, visa, mastercard, unionpay, sbp", k)
			}
			st.card = s
		case "card_timeout_s":
			var n float64
			if n, err = asNumber(k, v); err == nil {
				if n < 5 || n > 240 {
					err = fmt.Errorf("%s: от 5 до 240", k)
				}
				st.cardTimeout = time.Duration(n) * time.Second
			}
		case "bank_online":
			st.bankOnline, err = asBool(k, v)
		case "print_slip_on_terminal":
			st.printSlipOnTerminal, err = asBool(k, v)
		case "short_slip":
			st.shortSlip, err = asBool(k, v)
		case "cash_withdrawal":
			st.cashWithdrawal, err = asBool(k, v)
		case "partial_cancellation":
			st.partialCancellation, err = asBool(k, v)
		case "list_card_transactions":
			st.listCardTransactions, err = asBool(k, v)
		case "kopeck_rules":
			st.kopeckRules, err = asBool(k, v)
		case "decline_above":
			var n float64
			if n, err = asNumber(k, v); err == nil && n < 0 {
				err = fmt.Errorf("%s: 0 (выключено) или больше", k)
			}
			st.declineAbove = n
		default:
			err = fmt.Errorf("неизвестный ключ %q; допустимы: %s", k, strings.Join(StateKeys, ", "))
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
	case json.Number:
		return n.Float64()
	}
	return 0, fmt.Errorf("%s: ожидается число", key)
}
