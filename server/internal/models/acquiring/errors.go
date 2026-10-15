package acquiring

import "fmt"

// Коды ошибок эмулятора терминала (docs/models/acquiring.md). Механизм — как у ККТ:
// GetLastError отдаёт код и текст последней неуспешной операции.
const (
	ErrDeclined       = 61 // отказ банка-эмитента
	ErrInsufficient   = 62 // недостаточно средств
	ErrWrongPIN       = 63 // неверный ПИН-код
	ErrCancelled      = 64 // отмена на терминале
	ErrTimeout        = 65 // карту не приложили
	ErrBankOffline    = 66 // нет связи с банком
	ErrNotFound       = 67 // операция не найдена
	ErrAmountExceeded = 68 // сумма больше исходной
	ErrAlreadyVoided  = 69 // операция уже отменена
	ErrBadParams      = 98 // неверные параметры вызова
	ErrNotSupported   = 99 // не поддерживается
)

var errorTexts = map[int]string{
	ErrDeclined:       "Операция отклонена банком-эмитентом",
	ErrInsufficient:   "Операция отклонена: недостаточно средств",
	ErrWrongPIN:       "Неверный ПИН-код",
	ErrCancelled:      "Операция отменена на терминале",
	ErrTimeout:        "Время ожидания карты истекло",
	ErrBankOffline:    "Нет связи с банком",
	ErrNotFound:       "Операция не найдена",
	ErrAmountExceeded: "Сумма превышает сумму исходной операции",
	ErrAlreadyVoided:  "Операция уже отменена",
	ErrBadParams:      "Неверные параметры вызова",
	ErrNotSupported:   "Операция не поддерживается эмулятором терминала",
}

// termError — ошибка устройства: код и текст для GetLastError.
type termError struct {
	code   int
	detail string
}

func (e *termError) Error() string {
	if e.detail == "" {
		return errorTexts[e.code]
	}
	return errorTexts[e.code] + ": " + e.detail
}

func fail(code int, format string, args ...any) *termError {
	return &termError{code: code, detail: fmt.Sprintf(format, args...)}
}

func failCode(code int) *termError { return &termError{code: code} }
