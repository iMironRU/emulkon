// Package recognizer — устройство распознавания товаров (раздел 3.11 требований к драйверам):
// docs/bpo-requirements/recognizer.md, поведение — docs/models/recognizer.md.
package recognizer

import (
	"bytes"
	"context"
	"encoding/base64"
	"encoding/json"
	"encoding/xml"
	"errors"
	"fmt"
	"image"
	"image/color"
	"image/png"
	"sort"
	"strings"
	"sync"
	"time"

	"emulkon/internal/models"
	"emulkon/internal/models/kkt"
)

// Коды ошибок (docs/models/recognizer.md).
const (
	ErrNoImage      = 82
	ErrBadParams    = 98
	ErrNotSupported = 99
)

var errorTexts = map[int]string{
	ErrNoImage:      "Устройство не отдаёт изображение",
	ErrBadParams:    "Неверные параметры вызова",
	ErrNotSupported: "Метод не поддерживается эмулятором устройства распознавания",
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

// Options — ПараметрыУстройстваРаспознавания (RecognizerParamters — так в требованиях, out).
type recognizerOptions struct {
	XMLName            xml.Name `xml:"RecognizerParamters"`
	GetImage           kkt.Bool `xml:"GetImage,attr"`
	HardwareRecognizer kkt.Bool `xml:"HardwareRecognizer,attr"`
}

type goodsTable struct {
	XMLName xml.Name `xml:"Table"`
	Records []record `xml:"Record"`
}

type record struct {
	Code             int64      `xml:"Code,attr"`
	RecognizePercent kkt.Number `xml:"RecognizePercent,attr"`
}

// Candidate — что «лежит на весах»: код товара и вероятность (1 = 100%).
type Candidate struct {
	Code    int64   `json:"code"`
	Percent float64 `json:"percent"`
}

type recognition struct {
	Training bool        `json:"training"`
	Variants int         `json:"variants"`
	Result   []Candidate `json:"result"`
	At       time.Time   `json:"at"`
}

type confirmation struct {
	Code int64     `json:"code"`
	At   time.Time `json:"at"`
}

// ModelOptions — параметры модели.
type ModelOptions struct {
	Version string
	Now     func() time.Time
}

// Factory — фабрика для реестра.
func Factory(version string) models.Factory {
	return func(_ models.EquipmentType, _ string) models.Model { return New(ModelOptions{Version: version}) }
}

// Model — эмулятор устройства распознавания: что «видит» камера, задают в панели управления.
type Model struct {
	opts ModelOptions

	mu         sync.Mutex
	candidates []Candidate
	getImage   bool
	hardware   bool
	last       *recognition
	awaiting   bool // распознавание с обучением ждёт подтверждения
	confirms   []confirmation
	count      int
}

func New(opts ModelOptions) *Model {
	if opts.Now == nil {
		opts.Now = time.Now
	}
	m := &Model{opts: opts}
	m.reset()
	return m
}

func (m *Model) reset() {
	m.candidates = []Candidate{{Code: 1003, Percent: 0.95}, {Code: 1023, Percent: 0.15}}
	m.getImage, m.hardware = true, true
	m.last, m.awaiting, m.confirms, m.count = nil, false, nil, 0
}

func (m *Model) Reset() {
	m.mu.Lock()
	m.reset()
	m.mu.Unlock()
}

func (m *Model) Description() models.Description {
	return models.Description{
		Name:        "ЭМУЛЯТОР: устройство распознавания (Эмулкон)",
		Description: "Виртуальное устройство распознавания Эмулкон: что лежит на весах, задают в панели управления.",
		Version:     m.opts.Version, IsEmulator: true,
	}
}

func (m *Model) Handle(_ context.Context, method string, params []any) models.Result {
	a := models.Args(params)
	out := models.CopyParams(params)
	m.mu.Lock()
	defer m.mu.Unlock()
	var err error
	switch method {
	case "ConnectEquipment", "DisconnectEquipment":
	case "EquipmentTest":
		if err = a.Need(4); err == nil {
			out[2] = fmt.Sprintf("Тест пройден. ЭМУЛЯТОР устройства распознавания (Эмулкон): кандидатов %d.", len(m.candidates))
			out[3] = "ЭМУЛЯТОР Эмулкон"
		}
	case "RecognizerOptions":
		if err = a.Need(2); err == nil {
			var s string
			s, err = kkt.Marshal(recognizerOptions{GetImage: kkt.Bool(m.getImage), HardwareRecognizer: kkt.Bool(m.hardware)})
			out[1] = s
		}
	case "GetImage":
		if err = a.Need(2); err == nil {
			if !m.getImage {
				err = &devError{code: ErrNoImage}
			} else {
				out[1], err = m.image()
			}
		}
	case "Recognize":
		err = m.recognize(a, out)
	case "ConfirmRecognize":
		if err = a.Need(2); err == nil {
			var code int64
			if code, err = a.Int(1); err == nil {
				m.confirms = append(m.confirms, confirmation{Code: code, At: m.opts.Now()})
				if len(m.confirms) > 20 {
					m.confirms = m.confirms[len(m.confirms)-20:]
				}
				m.awaiting = false
			}
		}
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

// Recognize(0 DeviceID, 1 Training BOOL, 2 Variants LONG, 3 GoodsRecognizeTable out).
func (m *Model) recognize(a models.Args, out []any) error {
	if err := a.Need(4); err != nil {
		return err
	}
	training, err := a.Bool(1)
	if err != nil {
		return err
	}
	variants, err := a.Int(2)
	if err != nil {
		return err
	}
	if variants < 1 || variants > 100 {
		return fail(ErrBadParams, "Variants: от 1 до 100")
	}
	res := append([]Candidate{}, m.candidates...)
	sort.SliceStable(res, func(i, k int) bool { return res[i].Percent > res[k].Percent })
	if len(res) > int(variants) {
		res = res[:variants]
	}
	t := goodsTable{Records: []record{}}
	for _, c := range res {
		t.Records = append(t.Records, record{Code: c.Code, RecognizePercent: kkt.Number(c.Percent)})
	}
	s, err := kkt.Marshal(t)
	if err != nil {
		return err
	}
	out[3] = s
	m.count++
	m.awaiting = training
	m.last = &recognition{Training: training, Variants: int(variants), Result: res, At: m.opts.Now()}
	return nil
}

// image — «снимок» камеры: PNG 160×120 с полосами по кодам кандидатов и меткой эмулятора.
func (m *Model) image() (string, error) {
	img := image.NewRGBA(image.Rect(0, 0, 160, 120))
	for y := 0; y < 120; y++ {
		for x := 0; x < 160; x++ {
			img.Set(x, y, color.RGBA{uint8(230 - y/2), uint8(220 - x/4), 200, 255})
		}
	}
	for i, c := range m.candidates {
		h := 10 + int(c.Percent*90)
		col := color.RGBA{uint8(40 + (c.Code*37)%180), uint8(60 + (c.Code*53)%160), 90, 255}
		for y := 110 - h; y < 110; y++ {
			for x := 12 + i*28; x < 32+i*28 && x < 150; x++ {
				img.Set(x, y, col)
			}
		}
	}
	for x := 0; x < 160; x++ { // красная рамка: снимок эмулятора
		img.Set(x, 0, color.RGBA{200, 40, 30, 255})
		img.Set(x, 119, color.RGBA{200, 40, 30, 255})
	}
	var buf bytes.Buffer
	if err := png.Encode(&buf, img); err != nil {
		return "", err
	}
	return base64.StdEncoding.EncodeToString(buf.Bytes()), nil
}

// Act — image: снимок камеры для панели управления (PNG в Base64).
func (m *Model) Act(name string, _ map[string]any) (any, error) {
	if name != "image" {
		return nil, models.ErrUnknownAction
	}
	m.mu.Lock()
	defer m.mu.Unlock()
	img, err := m.image()
	if err != nil {
		return nil, err
	}
	return map[string]any{"png_base64": img}, nil
}

func (m *Model) State() map[string]any {
	m.mu.Lock()
	defer m.mu.Unlock()
	return map[string]any{
		"model": "recognizer", "candidates": append([]Candidate{}, m.candidates...),
		"get_image": m.getImage, "hardware": m.hardware, "last": m.last, "awaiting_confirm": m.awaiting,
		"confirmations": append([]confirmation{}, m.confirms...), "recognitions": m.count,
	}
}

// StateKeys — ключи SetState (docs/models/recognizer.md).
var StateKeys = []string{"candidates", "get_image", "hardware"}

func (m *Model) SetState(patch map[string]any) error {
	m.mu.Lock()
	defer m.mu.Unlock()
	cands, getImage, hardware := m.candidates, m.getImage, m.hardware
	for k, v := range patch {
		var err error
		switch k {
		case "candidates":
			cands, err = parseCandidates(v)
		case "get_image", "hardware":
			b, ok := v.(bool)
			if !ok {
				err = fmt.Errorf("%s: true или false", k)
			}
			if k == "get_image" {
				getImage = b
			} else {
				hardware = b
			}
		default:
			err = fmt.Errorf("неизвестный ключ %q; допустимы: %s", k, strings.Join(StateKeys, ", "))
		}
		if err != nil {
			return err
		}
	}
	m.candidates, m.getImage, m.hardware = cands, getImage, hardware
	return nil
}

func parseCandidates(v any) ([]Candidate, error) {
	list, ok := v.([]any)
	if !ok || len(list) > 100 {
		return nil, errors.New("candidates: массив {code, percent}, не больше 100")
	}
	out := make([]Candidate, 0, len(list))
	for i, x := range list {
		o, ok := x.(map[string]any)
		if !ok {
			return nil, fmt.Errorf("candidates[%d]: объект {code, percent}", i)
		}
		num := func(k string) (float64, bool) {
			switch n := o[k].(type) {
			case float64:
				return n, true
			case json.Number:
				f, err := n.Float64()
				return f, err == nil
			}
			return 0, false
		}
		code, ok1 := num("code")
		pct, ok2 := num("percent")
		if !ok1 || !ok2 || code < 0 || code != float64(int64(code)) || pct < 0 || pct > 1 {
			return nil, fmt.Errorf("candidates[%d]: code — целое ≥ 0, percent — от 0 до 1", i)
		}
		out = append(out, Candidate{Code: int64(code), Percent: pct})
	}
	return out, nil
}
