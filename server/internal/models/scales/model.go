// Package scales — модель электронных весов (раздел 3.1 требований к драйверам):
// docs/bpo-requirements/scales.md, поведение — docs/models/scales.md.
package scales

import (
	"context"
	"encoding/json"
	"encoding/xml"
	"errors"
	"fmt"
	"math"
	"sort"
	"strings"
	"sync"
	"time"

	"emulkon/internal/models"
	"emulkon/internal/models/kkt"
)

// Коды ошибок (docs/models/scales.md).
const (
	ErrOverload     = 70
	ErrUnstable     = 71
	ErrPlacement    = 72
	ErrCancelled    = 73
	ErrTimeout      = 74
	ErrBadParams    = 98
	ErrNotSupported = 99
)

var errorTexts = map[int]string{
	ErrOverload:     "Перегрузка весов",
	ErrUnstable:     "Вес не стабилизирован",
	ErrPlacement:    "Ошибка размещения товара на весах",
	ErrCancelled:    "Взвешивание отменено",
	ErrTimeout:      "Время ожидания веса истекло",
	ErrBadParams:    "Неверные параметры вызова",
	ErrNotSupported: "Метод не поддерживается эмулятором весов",
}

type devError struct {
	code   int
	detail string
}

func (e *devError) Error() string {
	if e.detail == "" {
		return errorTexts[e.code]
	}
	return errorTexts[e.code] + ": " + e.detail
}

func fail(code int, format string, args ...any) *devError {
	return &devError{code: code, detail: fmt.Sprintf(format, args...)}
}

// ScaleOptions — ПараметрыВесов (out).
type ScaleOptions struct {
	XMLName          xml.Name `xml:"ScaleOptions"`
	WeighingEvent    kkt.Bool `xml:"WeighingEvent,attr"`
	AdvancedData     kkt.Bool `xml:"AdvancedData,attr"`
	Calibrate        kkt.Bool `xml:"Calibrate,attr"`
	PlacementControl kkt.Bool `xml:"PlacementControl,attr"`
}

// Data — ПолучитьДанные (out).
type Data struct {
	XMLName           xml.Name   `xml:"Data"`
	Weight            kkt.Number `xml:"Weight,attr"`
	Stable            kkt.Bool   `xml:"Stable,attr"`
	PlacementError    kkt.Bool   `xml:"PlacementError,attr"`
	IntersectionZones string     `xml:"IntersectionZones,attr,omitempty"`
}

// Options — параметры модели.
type Options struct {
	Version string
	Now     func() time.Time
}

// Factory — фабрика модели для реестра.
func Factory(version string) models.Factory {
	return func(_ models.EquipmentType, deviceID string) models.Model {
		return New(deviceID, Options{Version: version})
	}
}

const (
	modeManual = "manual"
	modeAsk    = "ask"
)

type weighing struct {
	Net float64   `json:"net"`
	At  time.Time `json:"at"`
}

type state struct {
	weight         float64 // брутто на платформе, кг
	tare           float64
	maxWeight      float64
	stable         bool
	placementError bool
	zones          string
	mode           string
	promptTimeout  time.Duration
	last           *weighing
	weighings      int
}

func defaultState() state {
	return state{maxWeight: 15, stable: true, mode: modeManual, promptTimeout: 60 * time.Second}
}

// Model — эмулятор электронных весов.
type Model struct {
	id            string
	opts          Options
	models.Screen // экран весов: вес в режиме ask

	mu sync.Mutex
	st state
}

func New(deviceID string, opts Options) *Model {
	if opts.Now == nil {
		opts.Now = time.Now
	}
	return &Model{id: deviceID, opts: opts, Screen: models.Screen{DeviceID: deviceID}, st: defaultState()}
}

func grams(v float64) float64 { return math.Round(v*1000) / 1000 }

func (m *Model) Description() models.Description {
	return models.Description{
		Name:        "ЭМУЛЯТОР: электронные весы (Эмулкон)",
		Description: "Виртуальные весы Эмулкон: вес кладут на экране весов.",
		Version:     m.opts.Version, IsEmulator: true,
	}
}

func (m *Model) Reset() {
	m.CancelPrompt()
	m.mu.Lock()
	m.st = defaultState()
	m.mu.Unlock()
}

func (m *Model) Handle(_ context.Context, method string, params []any) models.Result {
	a := models.Args(params)
	out := models.CopyParams(params)
	var err error
	switch method {
	case "ConnectEquipment", "DisconnectEquipment":
	case "EquipmentTest":
		if err = a.Need(4); err == nil {
			m.mu.Lock()
			out[2] = fmt.Sprintf("Тест пройден. ЭМУЛЯТОР электронных весов (Эмулкон): предел %g кг, на весах %.3f кг.",
				m.st.maxWeight, m.st.weight)
			m.mu.Unlock()
			out[3] = "ЭМУЛЯТОР Эмулкон"
		}
	case "ScaleOptions":
		if err = a.Need(2); err == nil {
			var s string
			s, err = kkt.Marshal(ScaleOptions{AdvancedData: true, Calibrate: true})
			out[1] = s
		}
	case "Calibrate":
		err = m.calibrate(a)
	case "GetData":
		err = m.getData(a, out)
	case "GetWeight":
		err = m.getWeight(a, out)
	default:
		err = fail(ErrNotSupported, "%s", method)
	}
	if err != nil {
		var de *devError
		if !errors.As(err, &de) {
			de = fail(ErrBadParams, "%v", err)
		}
		return models.Result{Return: false, ErrorCode: de.code, ErrorText: de.Error()}
	}
	return models.Result{Return: true, Params: out}
}

func (m *Model) calibrate(a models.Args) error {
	if err := a.Need(2); err != nil {
		return err
	}
	tare, err := a.Float(1)
	if err != nil {
		return err
	}
	m.mu.Lock()
	defer m.mu.Unlock()
	if tare < 0 {
		return fail(ErrBadParams, "вес тары не может быть отрицательным")
	}
	if tare == 0 {
		tare = m.st.weight // текущий вес на платформе — это и есть тара
	}
	if tare > m.st.maxWeight {
		return fail(ErrOverload, "тара %.3f кг больше предела %g кг", tare, m.st.maxWeight)
	}
	m.st.tare = grams(tare)
	return nil
}

func (m *Model) getData(a models.Args, out []any) error {
	if err := a.Need(2); err != nil {
		return err
	}
	m.mu.Lock()
	d := Data{Weight: kkt.Number(grams(m.st.weight - m.st.tare)), Stable: kkt.Bool(m.st.stable),
		PlacementError: kkt.Bool(m.st.placementError)}
	if m.st.placementError {
		d.IntersectionZones = m.st.zones
		if d.IntersectionZones == "" {
			d.IntersectionZones = "err"
		}
	}
	m.mu.Unlock()
	s, err := kkt.Marshal(d)
	out[1] = s
	return err
}

func (m *Model) getWeight(a models.Args, out []any) error {
	if err := a.Need(2); err != nil {
		return err
	}
	m.mu.Lock()
	mode, timeout, cur, max, tare := m.st.mode, m.st.promptTimeout, m.st.weight-m.st.tare, m.st.maxWeight, m.st.tare
	m.mu.Unlock()

	if mode == modeAsk {
		def := grams(cur)
		if def <= 0 {
			def = 0.5
		}
		p := models.Prompt{
			Operation: "GetWeight", Title: "ВЗВЕШИВАНИЕ", Text: "Положите товар на весы",
			Input:   &models.PromptInput{Label: "Вес", Unit: "кг", Value: def, Min: 0, Max: max, Step: 0.001},
			Actions: []models.PromptOption{{ID: "weigh", Title: "Взвесить", Kind: "primary"}, {ID: "cancel", Title: "Отмена"}},
		}
		res := m.Ask(p, m.opts.Now(), timeout, func(r models.PromptResult) string {
			if r.Action == "weigh" && r.Value != nil {
				return fmt.Sprintf("ВЕС %.3f КГ", *r.Value)
			}
			if r.Action == models.PromptTimeout {
				return errorTexts[ErrTimeout]
			}
			return errorTexts[ErrCancelled]
		})
		switch {
		case res.Action == models.PromptTimeout:
			return &devError{code: ErrTimeout}
		case res.Action != "weigh" || res.Value == nil:
			return &devError{code: ErrCancelled}
		}
		m.mu.Lock()
		m.st.weight = grams(*res.Value + tare) // на экране вводят вес товара, тара остаётся
		m.mu.Unlock()
	}

	m.mu.Lock()
	defer m.mu.Unlock()
	switch {
	case m.st.weight > m.st.maxWeight:
		return fail(ErrOverload, "%.3f кг при пределе %g кг", m.st.weight, m.st.maxWeight)
	case !m.st.stable:
		return &devError{code: ErrUnstable}
	case m.st.placementError:
		return &devError{code: ErrPlacement}
	}
	net := grams(m.st.weight - m.st.tare)
	out[1] = net
	m.st.weighings++
	m.st.last = &weighing{Net: net, At: m.opts.Now()}
	return nil
}

func (m *Model) State() map[string]any {
	m.mu.Lock()
	defer m.mu.Unlock()
	var prompt any
	if p := m.Current(); p != nil {
		prompt = *p
	}
	return map[string]any{
		"model":            "scales",
		"weight":           m.st.weight,
		"tare":             m.st.tare,
		"net":              grams(m.st.weight - m.st.tare),
		"max_weight":       m.st.maxWeight,
		"stable":           m.st.stable,
		"placement_error":  m.st.placementError,
		"zones":            m.st.zones,
		"weight_mode":      m.st.mode,
		"prompt_timeout_s": int(m.st.promptTimeout / time.Second),
		"prompt":           prompt,
		"last_weighing":    m.st.last,
		"weighings":        m.st.weighings,
	}
}

// StateKeys — ключи SetState (docs/models/scales.md).
var StateKeys = []string{"weight", "tare", "stable", "placement_error", "zones", "max_weight", "weight_mode", "prompt_timeout_s"}

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
		var n float64
		switch k {
		case "weight", "tare", "max_weight":
			if n, err = asNumber(k, v); err == nil {
				if n < 0 || (k == "max_weight" && n == 0) {
					err = fmt.Errorf("%s: должен быть больше 0", k)
				}
				switch k {
				case "weight":
					st.weight = grams(n)
				case "tare":
					st.tare = grams(n)
				default:
					st.maxWeight = n
				}
			}
		case "stable":
			st.stable, err = asBool(k, v)
		case "placement_error":
			st.placementError, err = asBool(k, v)
		case "zones":
			s, ok := v.(string)
			if !ok {
				err = fmt.Errorf("%s: строка вида err-1,err-2", k)
			}
			st.zones = strings.TrimSpace(s)
		case "weight_mode":
			s, _ := v.(string)
			if s != modeManual && s != modeAsk {
				err = fmt.Errorf("%s: %s или %s", k, modeManual, modeAsk)
			}
			st.mode = s
		case "prompt_timeout_s":
			if n, err = asNumber(k, v); err == nil {
				if n < 5 || n > 240 {
					err = fmt.Errorf("%s: от 5 до 240", k)
				}
				st.promptTimeout = time.Duration(n) * time.Second
			}
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
