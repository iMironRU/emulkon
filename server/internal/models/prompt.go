package models

import (
	"errors"
	"time"
)

// Prompt — запрос на экране устройства, который ждёт человека: эквайринговый терминал
// «приложите карту» (docs/models/acquiring.md). Команда драйвера в это время ждёт ответа.
// Экран показывают веб-интерфейс и трей; отвечают POST /control/v1/devices/{id}/prompt.
type Prompt struct {
	ID          string         `json:"prompt_id"`
	Operation   string         `json:"operation"` // метод драйвера: Pay, ReturnPayment, …
	Title       string         `json:"title"`     // «ОПЛАТА»
	Text        string         `json:"text"`      // «Приложите, вставьте или проведите карту»
	Amount      float64        `json:"amount"`
	AmountCash  float64        `json:"amount_cash,omitempty"`
	Actions     []PromptOption `json:"actions"` // кнопки экрана
	Cards       []PromptOption `json:"cards,omitempty"`
	DefaultCard string         `json:"default_card,omitempty"`
	Input       *PromptInput   `json:"input,omitempty"` // поле ввода: вес на весах
	StartedAt   time.Time      `json:"started_at"`
	Deadline    time.Time      `json:"deadline"`
}

// PromptOption — вариант на экране: кнопка действия или карта.
type PromptOption struct {
	ID    string `json:"id"`
	Title string `json:"title"`
	Kind  string `json:"kind,omitempty"` // primary | danger — для оформления
}

// PromptInput — числовое поле на экране устройства («Вес, кг»).
type PromptInput struct {
	Label string  `json:"label"`
	Unit  string  `json:"unit"`
	Value float64 `json:"value"` // значение по умолчанию
	Min   float64 `json:"min"`
	Max   float64 `json:"max"`
	Step  float64 `json:"step"`
}

// PromptAnswer — ответ человека: кнопка, карта (терминал), значение поля (весы).
type PromptAnswer struct {
	Action string   `json:"action"`
	Card   string   `json:"card,omitempty"`
	Value  *float64 `json:"value,omitempty"`
}

// PromptResult — чем закончился запрос (запись device.prompt_done).
type PromptResult struct {
	Action string   `json:"action"`          // card, decline, weigh, …, timeout, reset
	Card   string   `json:"card,omitempty"`  // для action=card
	Value  *float64 `json:"value,omitempty"` // значение поля ввода
	Text   string   `json:"text"`            // итог для экрана: «ОДОБРЕНО», «ОТКАЗ: …»
}

// PromptHook — куда модель сообщает об открытии (done=nil) и закрытии запроса.
// Вызывается без блокировок модели.
type PromptHook func(p Prompt, done *PromptResult)

// Prompter — модель с экраном, который ждёт человека.
type Prompter interface {
	// Respond — ответ на запрос promptID. ErrPromptGone — запроса уже нет (отвечен, истёк, сброшен).
	Respond(promptID string, a PromptAnswer) error
	// CancelPrompt снимает открытый запрос (сброс состояния): команда завершится отказом.
	CancelPrompt()
	SetPromptHook(h PromptHook)
}

// ErrPromptGone — на запрос уже ответили или он истёк.
var ErrPromptGone = errors.New("запроса уже нет: он отвечен, истёк или снят")
