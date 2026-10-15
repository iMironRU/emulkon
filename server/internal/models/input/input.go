// Package input — устройства ввода (раздел 3.8 требований к драйверам): сканер штрихкода и
// считыватель магнитных карт. Своих методов у них нет: данные уходят в 1С внешними
// событиями (ExternalEvent). Контракт — docs/bpo-requirements/input-devices.md, поведение —
// docs/models/input.md.
package input

import (
	"encoding/base64"
	"encoding/json"
	"errors"
	"fmt"
	"sync"
	"time"

	"emulkon/internal/models"
)

// Коды ошибок (docs/models/input.md).
const (
	ErrBadParams    = 98
	ErrNotSupported = 99
)

// Options — параметры модели.
type Options struct {
	Version string
	Now     func() time.Time
}

func (o *Options) defaults() {
	if o.Now == nil {
		o.Now = time.Now
	}
}

// Форматы событий: строкой или Base64 (BarcodeBase64, TracksDataBase64).
const (
	FormatPlain  = "plain"
	FormatBase64 = "base64"
)

// Режимы передачи кода (docs/models/input.md): событием через драйвер, как клавиатура (HID —
// трей вводит в активное окно) или и так, и так.
const (
	ModeDriver   = "driver"
	ModeKeyboard = "keyboard"
	ModeBoth     = "both"
)

// base — общее для устройств ввода: события и разбор вызовов драйвера.
type base struct {
	opts   Options
	title  string // «сканера штрихкода»
	mu     sync.Mutex
	hook   models.EventHook
	format string
	// Клавиатурный режим.
	mode, suffix string
	delayMS      int
}

func (b *base) resetInput(suffix string) {
	b.format, b.mode, b.suffix, b.delayMS = FormatPlain, ModeDriver, suffix, 1500
}

// deliver — код в 1С: событием (драйвер) и/или вводом с клавиатуры (трей). Под блокировкой.
func (b *base) deliver(e models.DeviceEvent, text string) (emit bool, kb *models.KeyboardInput) {
	if b.mode != ModeKeyboard {
		emit = true
	}
	if b.mode != ModeDriver {
		kb = &models.KeyboardInput{Text: text, Suffix: b.suffix, DelayMS: b.delayMS}
	}
	return emit, kb
}

// setInputKey — общие ключи SetState устройств ввода; ok=false — ключ не их.
func (b *base) setInputKey(k string, v any, mode, suffix *string, delay *int) (ok bool, err error) {
	switch k {
	case "input_mode":
		s, _ := v.(string)
		if s != ModeDriver && s != ModeKeyboard && s != ModeBoth {
			return true, fmt.Errorf("input_mode: driver, keyboard или both")
		}
		*mode = s
	case "keyboard_suffix":
		s, _ := v.(string)
		if s != "enter" && s != "tab" && s != "none" {
			return true, fmt.Errorf("keyboard_suffix: enter, tab или none")
		}
		*suffix = s
	case "keyboard_delay_ms":
		n, e := numArg(v)
		if e != nil || n < 0 || n > 10000 || n != float64(int(n)) {
			return true, fmt.Errorf("keyboard_delay_ms: целое от 0 до 10000")
		}
		*delay = int(n)
	default:
		return false, nil
	}
	return true, nil
}

func (b *base) inputState(st map[string]any) map[string]any {
	st["input_mode"], st["keyboard_suffix"], st["keyboard_delay_ms"] = b.mode, b.suffix, b.delayMS
	return st
}

func (b *base) SetEventHook(h models.EventHook) {
	b.mu.Lock()
	b.hook = h
	b.mu.Unlock()
}

// emit — событие в 1С; вызывается без блокировки модели.
func (b *base) emit(e models.DeviceEvent) {
	b.mu.Lock()
	h := b.hook
	b.mu.Unlock()
	if h != nil {
		h(e)
	}
}

// event — имя и данные события по формату: plain → name, base64 → name+"Base64".
func (b *base) event(name, data string) models.DeviceEvent {
	if b.format == FormatBase64 {
		return models.DeviceEvent{Name: name + "Base64", Data: base64.StdEncoding.EncodeToString([]byte(data))}
	}
	return models.DeviceEvent{Name: name, Data: data}
}

func (b *base) handle(method string, params []any) models.Result {
	a := models.Args(params)
	out := models.CopyParams(params)
	switch method {
	case "ConnectEquipment", "DisconnectEquipment":
		return models.Result{Return: true, Params: out}
	case "EquipmentTest":
		if err := a.Need(4); err != nil {
			return models.Result{Return: false, ErrorCode: ErrBadParams, ErrorText: "Неверные параметры вызова: " + err.Error()}
		}
		out[2] = fmt.Sprintf("Тест пройден. ЭМУЛЯТОР %s (Эмулкон): данные приходят событиями из панели управления.", b.title)
		out[3] = "ЭМУЛЯТОР Эмулкон"
		return models.Result{Return: true, Params: out}
	}
	return models.Result{Return: false, ErrorCode: ErrNotSupported,
		ErrorText: fmt.Sprintf("Метод не поддерживается: %s — у устройства ввода нет методов, данные приходят событиями", method)}
}

func (b *base) Description(name, desc string) models.Description {
	return models.Description{Name: name, Description: desc, Version: b.opts.Version, IsEmulator: true}
}

func setFormat(v any) (string, error) {
	s, _ := v.(string)
	if s != FormatPlain && s != FormatBase64 {
		return "", fmt.Errorf("event_format: %s или %s", FormatPlain, FormatBase64)
	}
	return s, nil
}

func strArg(args map[string]any, key string) (string, error) {
	switch v := args[key].(type) {
	case nil:
		return "", nil
	case string:
		return v, nil
	}
	return "", fmt.Errorf("%s: ожидается строка", key)
}

func numArg(v any) (float64, error) {
	switch n := v.(type) {
	case float64:
		return n, nil
	case json.Number:
		return n.Float64()
	}
	return 0, errors.New("ожидается число")
}
