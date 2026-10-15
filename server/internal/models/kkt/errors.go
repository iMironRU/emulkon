package kkt

import "fmt"

// Коды ошибок эмулятора ККТ. Требования к драйверам задают только механизм
// (GetLastError: код LONG + текст), коды у каждого драйвера свои — эти стабильны
// и описаны в docs/models/kkt.md. Тексты начинаются с фраз, на которые реагирует
// БПО (МенеджерОборудованияКлиент.ЗаполнитьЛокальнуОбработкуОшибок).
const (
	ErrPaperOut          = 1  // Нет бумаги
	ErrCoverOpen         = 2  // Открыта крышка
	ErrNotRegistered     = 10 // ККТ не зарегистрирована
	ErrAlreadyRegistered = 11 // ККТ уже зарегистрирована
	ErrFNClosed          = 12 // ФН закрыт
	ErrFNFail            = 13 // ресурс ФН исчерпан / срочная замена
	ErrFNOverflow        = 14 // память ФН переполнена
	ErrShiftClosed       = 20 // смена не открыта
	ErrShiftAlreadyOpen  = 21 // смена уже открыта
	ErrShiftExpired      = 22 // смена превысила 24 часа
	ErrShiftMustBeClosed = 23 // операция требует закрытой смены
	ErrShiftAlreadyClose = 24 // смена уже закрыта
	ErrBadData           = 30 // неверные данные документа
	ErrTaxationSystem    = 31 // СНО не зарегистрирована
	ErrPaymentLess       = 32 // сумма оплаты меньше суммы чека
	ErrCashlessOverpay   = 33 // безналичная оплата больше суммы чека
	ErrNotEnoughCash     = 34 // недостаточно наличных
	ErrNoPositions       = 35 // чек без позиций
	ErrVAT               = 36 // ошибка ставки или суммы НДС
	ErrCorrectionData    = 37 // нет данных коррекции
	ErrKMNotChecked      = 40 // КМ не проверен / не подтверждён
	ErrKMRequestUnknown  = 41 // запрос КМ не найден
	ErrKMNoRequest       = 42 // нет активного запроса КМ
	ErrDocNotFound       = 50 // документ не найден
	ErrBarcodeType       = 51 // тип штрихкода не поддерживается
	ErrPiotNotSupported  = 60 // ФН не работает с ТС ПИоТ (piot: false)
	ErrPiotData          = 61 // неверные данные ТС ПИоТ
	ErrBadParams         = 98 // неверные параметры вызова
	ErrNotSupported      = 99 // метод не поддерживается
)

// errorTexts — тексты ошибок. Начало строки — фраза, которую узнаёт БПО, если такая есть.
var errorTexts = map[int]string{
	ErrPaperOut:          "Нет бумаги",
	ErrCoverOpen:         "Открыта крышка",
	ErrNotRegistered:     "ККТ не зарегистрирована (ФН не фискализирован)",
	ErrAlreadyRegistered: "ККТ уже зарегистрирована",
	ErrFNClosed:          "Фискальный накопитель закрыт (архив ФН)",
	ErrFNFail:            "Исчерпал ресурс КС ФН: требуется замена фискального накопителя",
	ErrFNOverflow:        "Ресурс хранения ФД исчерпан: память ФН переполнена",
	ErrShiftClosed:       "Кассовая смена не открыта или истекла",
	ErrShiftAlreadyOpen:  "Кассовая смена уже открыта",
	ErrShiftExpired:      "Смена превысила 24 часа",
	ErrShiftMustBeClosed: "Смена открыта: операция возможна только при закрытой смене",
	ErrShiftAlreadyClose: "Кассовая смена уже закрыта",
	ErrBadData:           "Неверные данные документа",
	ErrTaxationSystem:    "Система налогообложения не зарегистрирована в ККТ",
	ErrPaymentLess:       "Сумма оплаты меньше суммы чека",
	ErrCashlessOverpay:   "Сумма безналичной оплаты больше суммы чека",
	ErrNotEnoughCash:     "Недостаточно наличных в денежном ящике",
	ErrNoPositions:       "Чек не содержит фискальных позиций",
	ErrVAT:               "Неверная ставка или сумма НДС",
	ErrCorrectionData:    "Не заданы данные коррекции",
	ErrPiotNotSupported:  "ФН не поддерживает работу с ТС ПИоТ",
	ErrPiotData:          "Неверные данные ТС ПИоТ",
	ErrKMNotChecked:      "Контрольная марка не проверена",
	ErrKMRequestUnknown:  "Запрос КМ не найден",
	ErrKMNoRequest:       "Нет активного запроса КМ",
	ErrDocNotFound:       "Документ не найден",
	ErrBarcodeType:       "Тип штрихкода не поддерживается",
	ErrBadParams:         "Неверные параметры вызова",
	ErrNotSupported:      "Метод не поддерживается эмулятором ККТ",
}

// kktError — ошибка устройства: код и текст для GetLastError.
type kktError struct {
	code   int
	detail string
}

func (e *kktError) Error() string {
	if e.detail == "" {
		return errorTexts[e.code]
	}
	return errorTexts[e.code] + ": " + e.detail
}

func fail(code int, format string, args ...any) *kktError {
	return &kktError{code: code, detail: fmt.Sprintf(format, args...)}
}

func failCode(code int) *kktError { return &kktError{code: code} }
