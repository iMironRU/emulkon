// Package labelscales — модель весов с печатью этикеток в режиме OFFLINE (раздел 3.2
// требований к драйверам): docs/bpo-requirements/scales.md, поведение — docs/models/scales.md.
package labelscales

import (
	"context"
	"encoding/base64"
	"encoding/json"
	"errors"
	"fmt"
	"math"
	"sort"
	"strings"
	"sync"
	"time"
	"unicode/utf8"

	"emulkon/internal/models"
	"emulkon/internal/models/kkt"
)

// Коды ошибок (docs/models/scales.md).
const (
	ErrNoGood        = 80
	ErrPackageStatus = 81
	ErrBadParams     = 98
	ErrNotSupported  = 99
)

var errorTexts = map[int]string{
	ErrNoGood:        "Товар с таким PLU не загружен",
	ErrPackageStatus: "Неверный статус пакета",
	ErrBadParams:     "Неверные параметры вызова",
	ErrNotSupported:  "Метод не поддерживается эмулятором весов",
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

// --- XML (docs/bpo-requirements/scales.md) ---

type goodsTable struct {
	Records []goodRecord `xml:"Record"`
}

type goodRecord struct {
	PLU                kkt.Number  `xml:"PLU,attr"`
	Code               kkt.Number  `xml:"Code,attr"`
	IsWeightGoods      *kkt.Bool   `xml:"IsWeightGoods,attr"`
	Name               string      `xml:"Name,attr"`
	Price              kkt.Number  `xml:"Price,attr"`
	Description        string      `xml:"Description,attr"`
	ShelfLife          kkt.Number  `xml:"ShelfLife,attr"`
	WeightPieceOfGoods kkt.Number  `xml:"WeightPieceOfGoods,attr"`
	ButtonNumber       kkt.Number  `xml:"ButtonNumber,attr"`
	LabelFormat        kkt.Number  `xml:"LabelFormat,attr"`
	AdditionalPrices   *extraPrice `xml:"AdditionalPrices"`
}

type extraPrice struct {
	Price1 kkt.Number `xml:"Price1,attr"`
	Price2 kkt.Number `xml:"Price2,attr"`
	Price3 kkt.Number `xml:"Price3,attr"`
	Price4 kkt.Number `xml:"Price4,attr"`
}

type imagesTable struct {
	Records []struct {
		PLU     kkt.Number `xml:"PLU,attr"`
		Code    kkt.Number `xml:"Code,attr"`
		Picture string     `xml:"Picture,attr"`
	} `xml:"Record"`
}

// Good — товар в весах.
type Good struct {
	PLU         int64      `json:"plu"`
	Code        int64      `json:"code"`
	IsWeight    bool       `json:"is_weight"`
	Name        string     `json:"name"`
	Price       float64    `json:"price"`
	Prices      [4]float64 `json:"additional_prices"`
	Description string     `json:"description,omitempty"`
	ShelfLife   int64      `json:"shelf_life,omitempty"`
	PieceWeight float64    `json:"piece_weight,omitempty"`
	Button      int64      `json:"button,omitempty"`
	LabelFormat int64      `json:"label_format,omitempty"`
}

// upload — учёт выгрузки из 1С (товаров или картинок): пакеты от first до last.
type upload struct {
	Status   string    `json:"status"` // статус последнего пакета
	Packages int       `json:"packages"`
	Records  int       `json:"records"`
	At       time.Time `json:"at"`
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
	labelWidth     = 32
	tapeLimit      = 200
	maxImageBytes  = 2 << 20
	maxImagesBytes = 32 << 20
	labelMark      = "*** ЭМУЛЯТОР · ЭТИКЕТКА ***"
)

// Model — эмулятор весов с печатью этикеток.
type Model struct {
	id   string
	opts Options

	mu         sync.Mutex
	goods      map[int64]*Good
	images     map[int64]string // PLU → PNG в Base64
	imageBytes int
	prefix     string
	upload     *upload // выгрузка товаров
	imgUpload  *upload // выгрузка картинок
	labels     int
	tape       []models.PrintedDocument
	tapeSeq    int64
}

func New(deviceID string, opts Options) *Model {
	if opts.Now == nil {
		opts.Now = time.Now
	}
	m := &Model{id: deviceID, opts: opts}
	m.reset()
	return m
}

func (m *Model) reset() {
	m.goods = map[int64]*Good{}
	m.images = map[int64]string{}
	m.imageBytes = 0
	m.prefix = "21"
	m.upload, m.imgUpload = nil, nil
	m.labels = 0
	m.tape = nil
	m.tapeSeq = 0
}

func (m *Model) Description() models.Description {
	return models.Description{
		Name:        "ЭМУЛЯТОР: весы с печатью этикеток (Эмулкон)",
		Description: "Виртуальные весы с печатью этикеток Эмулкон: товары из 1С, этикетки с весовым штрихкодом.",
		Version:     m.opts.Version, IsEmulator: true,
	}
}

func (m *Model) Reset() {
	m.mu.Lock()
	m.reset()
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
			out[2] = fmt.Sprintf("Тест пройден. ЭМУЛЯТОР весов с печатью этикеток (Эмулкон): товаров загружено %d.", len(m.goods))
			m.mu.Unlock()
			out[3] = "ЭМУЛЯТОР Эмулкон"
		}
	case "UploadGoods":
		err = m.uploadGoods(a)
	case "ClearGoods":
		if err = a.Need(1); err == nil {
			m.mu.Lock()
			m.goods, m.images, m.imageBytes = map[int64]*Good{}, map[int64]string{}, 0
			m.mu.Unlock()
		}
	case "UploadProductImages":
		err = m.uploadImages(a)
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

func packageStatus(a models.Args) (string, error) {
	s, err := a.String(2)
	if err != nil {
		return "", err
	}
	s = strings.ToLower(strings.TrimSpace(s))
	if s != "first" && s != "regular" && s != "last" {
		return "", fail(ErrPackageStatus, "%q: ожидается first, regular или last", s)
	}
	return s, nil
}

// beginPackage — учёт выгрузки: first начинает новую, остальные продолжают.
func (m *Model) beginPackage(u **upload, status string, records int) {
	if status == "first" || *u == nil || (*u).Status == "last" {
		*u = &upload{}
	}
	(*u).Status = status
	(*u).Packages++
	(*u).Records += records
	(*u).At = m.opts.Now()
}

func (m *Model) uploadGoods(a models.Args) error {
	if err := a.Need(3); err != nil {
		return err
	}
	status, err := packageStatus(a)
	if err != nil {
		return err
	}
	s, err := a.String(1)
	if err != nil {
		return err
	}
	var t goodsTable
	if err := kkt.Unmarshal(s, &t); err != nil {
		return fail(ErrBadParams, "%v", err)
	}
	goods := make([]*Good, 0, len(t.Records))
	for i, r := range t.Records {
		g := &Good{
			PLU: int64(r.PLU), Code: int64(r.Code), IsWeight: r.IsWeightGoods == nil || bool(*r.IsWeightGoods),
			Name: strings.TrimSpace(r.Name), Price: kkt.Kopecks(float64(r.Price)), Description: r.Description,
			ShelfLife: int64(r.ShelfLife), PieceWeight: float64(r.WeightPieceOfGoods),
			Button: int64(r.ButtonNumber), LabelFormat: int64(r.LabelFormat),
		}
		if r.AdditionalPrices != nil {
			g.Prices = [4]float64{float64(r.AdditionalPrices.Price1), float64(r.AdditionalPrices.Price2),
				float64(r.AdditionalPrices.Price3), float64(r.AdditionalPrices.Price4)}
		}
		if g.PLU <= 0 || g.Name == "" {
			return fail(ErrBadParams, "запись %d: нужны PLU > 0 и Name", i+1)
		}
		goods = append(goods, g)
	}
	m.mu.Lock()
	defer m.mu.Unlock()
	for _, g := range goods {
		m.goods[g.PLU] = g
	}
	m.beginPackage(&m.upload, status, len(goods))
	return nil
}

func (m *Model) uploadImages(a models.Args) error {
	if err := a.Need(3); err != nil {
		return err
	}
	status, err := packageStatus(a)
	if err != nil {
		return err
	}
	s, err := a.String(1)
	if err != nil {
		return err
	}
	var t imagesTable
	if err := kkt.Unmarshal(s, &t); err != nil {
		return fail(ErrBadParams, "%v", err)
	}
	pics := make(map[int64]string, len(t.Records))
	for i, r := range t.Records {
		pic := strings.TrimSpace(r.Picture)
		if len(pic) > maxImageBytes {
			return fail(ErrBadParams, "картинка PLU %d больше %d МБ", int64(r.PLU), maxImageBytes>>20)
		}
		raw, err := base64.StdEncoding.DecodeString(pic)
		if err != nil || len(raw) < 8 || string(raw[1:4]) != "PNG" {
			return fail(ErrBadParams, "запись %d (PLU %d): Picture — не PNG в Base64", i+1, int64(r.PLU))
		}
		pics[int64(r.PLU)] = pic
	}
	m.mu.Lock()
	defer m.mu.Unlock()
	total := m.imageBytes
	for plu, pic := range pics {
		total += len(pic) - len(m.images[plu])
	}
	if total > maxImagesBytes {
		return fail(ErrBadParams, "картинок больше %d МБ — эмулятор столько не хранит", maxImagesBytes>>20)
	}
	for plu, pic := range pics {
		m.images[plu] = pic
	}
	m.imageBytes = total
	m.beginPackage(&m.imgUpload, status, len(t.Records))
	return nil
}

// --- этикетка ---

// eanCheck — контрольная цифра EAN-13 для 12 цифр.
func eanCheck(digits string) int {
	sum := 0
	for i, c := range digits {
		d := int(c - '0')
		if i%2 == 1 {
			d *= 3
		}
		sum += d
	}
	return (10 - sum%10) % 10
}

// WeightBarcode — весовой EAN-13: префикс (2) + код товара (5) + вес в граммах (5) + контрольная.
func WeightBarcode(prefix string, code int64, weightKg float64) string {
	g := int64(math.Round(weightKg * 1000))
	s := fmt.Sprintf("%s%05d%05d", prefix, code%100000, g%100000)
	return s + fmt.Sprint(eanCheck(s))
}

func center(s string) string {
	n := utf8.RuneCountInString(s)
	if n >= labelWidth {
		return s
	}
	return strings.Repeat(" ", (labelWidth-n)/2) + s
}

func lr(left, right string) string {
	gap := labelWidth - utf8.RuneCountInString(left) - utf8.RuneCountInString(right)
	if gap < 1 {
		return left + " " + right
	}
	return left + strings.Repeat(" ", gap) + right
}

func wrap(s string, maxLines int) []string {
	var out []string
	line := ""
	for _, w := range strings.Fields(s) {
		if line != "" && utf8.RuneCountInString(line)+1+utf8.RuneCountInString(w) > labelWidth {
			out = append(out, line)
			line = ""
		}
		if line != "" {
			line += " "
		}
		line += w
	}
	if line != "" {
		out = append(out, line)
	}
	if maxLines > 0 && len(out) > maxLines {
		out = out[:maxLines]
		out[maxLines-1] += "…"
	}
	return out
}

// Act — действия человека на весах (models.Actor): print_label, image.
func (m *Model) Act(name string, args map[string]any) (any, error) {
	if name != "image" && name != "print_label" {
		return nil, models.ErrUnknownAction
	}
	plu, err := intArg(args, "plu")
	if err != nil {
		return nil, err
	}
	m.mu.Lock()
	defer m.mu.Unlock()
	switch name {
	case "image":
		pic, ok := m.images[plu]
		if !ok {
			return nil, fmt.Errorf("у PLU %d нет картинки", plu)
		}
		return map[string]any{"plu": plu, "png_base64": pic}, nil
	case "print_label":
		g, ok := m.goods[plu]
		if !ok {
			return nil, &devError{code: ErrNoGood, detail: fmt.Sprint(plu)}
		}
		weight, _ := numArg(args, "weight")
		if weight <= 0 && !g.IsWeight {
			weight = g.PieceWeight
		}
		if weight <= 0 || weight >= 100 {
			return nil, fmt.Errorf("вес: больше 0 и меньше 100 кг (в штрихкоде 5 цифр граммов)")
		}
		doc := m.printLabel(g, weight)
		return doc, nil
	}
	return nil, models.ErrUnknownAction
}

func (m *Model) printLabel(g *Good, weight float64) models.PrintedDocument {
	now := m.opts.Now()
	weight = math.Round(weight*1000) / 1000
	cost := kkt.Kopecks(g.Price * weight)
	barcode := WeightBarcode(m.prefix, g.Code, weight)
	lines := []string{labelMark}
	lines = append(lines, wrap(g.Name, 2)...)
	lines = append(lines,
		lr(fmt.Sprintf("PLU %d", g.PLU), fmt.Sprintf("КОД %05d", g.Code%100000)),
		lr("ЦЕНА ЗА КГ", fmt.Sprintf("%.2f RUB", g.Price)),
		lr("ВЕС", fmt.Sprintf("%.3f КГ", weight)),
		lr("СТОИМОСТЬ", fmt.Sprintf("%.2f RUB", cost)),
		lr("УПАКОВАНО", now.Format("02.01.06 15:04")))
	if g.ShelfLife > 0 {
		lines = append(lines, lr("ГОДЕН ДО", now.AddDate(0, 0, int(g.ShelfLife)).Format("02.01.06")))
	}
	if g.Description != "" {
		lines = append(lines, wrap(g.Description, 3)...)
	}
	lines = append(lines, strings.Repeat("-", labelWidth), center("[EAN-13 "+barcode+"]"))
	m.tapeSeq++
	m.labels++
	doc := models.PrintedDocument{Seq: m.tapeSeq, Kind: "label", Time: now, Lines: lines}
	m.tape = append(m.tape, doc)
	if len(m.tape) > tapeLimit {
		m.tape = m.tape[len(m.tape)-tapeLimit:]
	}
	return doc
}

func intArg(args map[string]any, key string) (int64, error) {
	n, err := numArg(args, key)
	return int64(n), err
}

func numArg(args map[string]any, key string) (float64, error) {
	switch v := args[key].(type) {
	case float64:
		return v, nil
	case json.Number:
		return v.Float64()
	case nil:
		return 0, fmt.Errorf("нужен %s", key)
	}
	return 0, fmt.Errorf("%s: ожидается число", key)
}

// --- models.Printer ---

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

// --- состояние ---

func (m *Model) State() map[string]any {
	m.mu.Lock()
	defer m.mu.Unlock()
	goods := make([]*Good, 0, len(m.goods))
	for _, g := range m.goods {
		goods = append(goods, g)
	}
	sort.Slice(goods, func(i, k int) bool { return goods[i].PLU < goods[k].PLU })
	if len(goods) > 500 {
		goods = goods[:500] // в карточке устройства больше не нужно
	}
	images := make([]int64, 0, len(m.images))
	for plu := range m.images {
		images = append(images, plu)
	}
	sort.Slice(images, func(i, k int) bool { return images[i] < images[k] })
	return map[string]any{
		"model":          "label_scales",
		"goods_count":    len(m.goods),
		"goods":          goods,
		"images":         images,
		"upload":         m.upload,
		"images_upload":  m.imgUpload,
		"barcode_prefix": m.prefix,
		"labels":         m.labels,
	}
}

// SetState — barcode_prefix (docs/models/scales.md).
func (m *Model) SetState(patch map[string]any) error {
	m.mu.Lock()
	defer m.mu.Unlock()
	for k, v := range patch {
		switch k {
		case "barcode_prefix":
			s, _ := v.(string)
			if len(s) != 2 || s[0] != '2' || s[1] < '0' || s[1] > '9' {
				return fmt.Errorf("%s: две цифры от 20 до 29", k)
			}
		default:
			return fmt.Errorf("неизвестный ключ %q; допустимы: barcode_prefix", k)
		}
	}
	if s, ok := patch["barcode_prefix"].(string); ok {
		m.prefix = s
	}
	return nil
}
