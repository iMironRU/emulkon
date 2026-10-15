package models

import "errors"

// Actor — модель, с которой человек что-то делает на самом устройстве, помимо команд 1С:
// весы печатают этикетку, принтер подаёт бумагу. POST /control/v1/devices/{id}/actions/{name}.
type Actor interface {
	// Act выполняет действие name с аргументами args. Результат уходит в ответ Control API.
	// ErrUnknownAction — такого действия у модели нет.
	Act(name string, args map[string]any) (any, error)
}

// ErrUnknownAction — у модели нет такого действия.
var ErrUnknownAction = errors.New("у устройства нет такого действия")

// KeyboardInput — что ввести с клавиатуры на рабочем месте: сканер или считыватель в режиме
// клавиатуры (HID). Сервер пишет запись device.keyboard, трей вводит текст в активное окно
// (docs/models/input.md, docs/protocol.md §2.3).
type KeyboardInput struct {
	Text    string `json:"text"`
	Suffix  string `json:"suffix"`   // enter, tab, none — что нажать после текста
	DelayMS int    `json:"delay_ms"` // пауза перед вводом: успеть переключиться в окно 1С
}

// Keyboarder — результат действия, который надо ввести с клавиатуры (nil — не надо).
type Keyboarder interface {
	KeyboardInput() *KeyboardInput
}
