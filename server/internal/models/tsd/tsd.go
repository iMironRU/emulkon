// Package tsd — терминал сбора данных (раздел 3.4 требований к драйверам):
// docs/bpo-requirements/tsd.md, поведение — docs/models/tsd.md.
package tsd

import (
	"context"
	"encoding/base64"
	"encoding/json"
	"encoding/xml"
	"errors"
	"fmt"
	"math"
	"strings"
	"sync"
	"time"

	"emulkon/internal/models"
	"emulkon/internal/models/kkt"
)

// Коды ошибок (docs/models/tsd.md).
const (
	ErrPackageStatus = 87
	ErrBadParams     = 98
	ErrNotSupported  = 99
)

var errorTexts = map[int]string{
	ErrPackageStatus: "Неверный статус пакета",
	ErrBadParams:     "Неверные параметры вызова",
	ErrNotSupported:  "Метод не поддерживается эмулятором ТСД",
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

// --- XML (docs/bpo-requirements/tsd.md) ---

type goodsTable struct {
	XMLName  xml.Name  `xml:"Table"`
	FullLoad *kkt.Bool `xml:"FullLoad,attr"`
	Records  []goodXML `xml:"Record"`
}

type goodXML struct {
	BarCodeBase64                string      `xml:"BarCodeBase64,attr"`
	Name                         string      `xml:"Name,attr"`
	Article                      string      `xml:"Article,attr"`
	UnitOfMeasurement            string      `xml:"UnitOfMeasurement,attr"`
	CharacteristicOfNomenclature string      `xml:"CharacteristicOfNomenclature,attr"`
	SeriesOfNomenclature         string      `xml:"SeriesOfNomenclature,attr"`
	Quality                      string      `xml:"Quality,attr"`
	Price                        kkt.Number  `xml:"Price,attr"`
	Quantity                     kkt.Number  `xml:"Quantity,attr"`
	ContainerBarCodeBase64       string      `xml:"ContainerBarCodeBase64,attr"`
	MarkedGoodTypeCode           *kkt.Number `xml:"MarkedGoodTypeCode,attr"`
	Alcohol                      kkt.Bool    `xml:"Alcohol,attr"`
}

// downloadTable — ТаблицаЗагрузки (DownloadTable, out).
type downloadTable struct {
	XMLName xml.Name       `xml:"Table"`
	Records []downloadItem `xml:"Record"`
}

type downloadItem struct {
	BarCodeBase64          string     `xml:"BarCodeBase64,attr"`
	Quantity               kkt.Number `xml:"Quantity,attr"`
	ContainerBarCodeBase64 string     `xml:"ContainerBarCodeBase64,attr,omitempty"`
}

// Good — товар, выгруженный в ТСД.
type Good struct {
	Barcode        string  `json:"barcode"`
	Name           string  `json:"name"`
	Article        string  `json:"article,omitempty"`
	Unit           string  `json:"unit,omitempty"`
	Characteristic string  `json:"characteristic,omitempty"`
	Series         string  `json:"series,omitempty"`
	Quality        string  `json:"quality,omitempty"`
	Price          float64 `json:"price"`
	Quantity       float64 `json:"quantity"`
	Container      string  `json:"container,omitempty"`
	Marked         bool    `json:"marked,omitempty"`
	Alcohol        bool    `json:"alcohol,omitempty"`
}

func (g Good) key() string { return g.Barcode + "\x00" + g.Characteristic + "\x00" + g.Series }

// Item — строка собранного документа.
type Item struct {
	Barcode   string  `json:"barcode"`
	Name      string  `json:"name,omitempty"` // из таблицы товаров, если штрихкод там есть
	Quantity  float64 `json:"quantity"`
	Container string  `json:"container,omitempty"`
}

type upload struct {
	Status   string    `json:"status"`
	Packages int       `json:"packages"`
	Records  int       `json:"records"`
	FullLoad bool      `json:"full_load"`
	At       time.Time `json:"at"`
}

// Options — параметры модели.
type Options struct {
	Version string
	Now     func() time.Time
}

// Factory — фабрика для реестра.
func Factory(version string) models.Factory {
	return func(_ models.EquipmentType, _ string) models.Model { return New(Options{Version: version}) }
}

// Model — эмулятор ТСД: товары выгружают из 1С, документ «собирают» в панели управления.
type Model struct {
	opts Options

	mu        sync.Mutex
	goods     []Good
	upload    *upload
	doc       []Item
	downloads int
}

func New(opts Options) *Model {
	if opts.Now == nil {
		opts.Now = time.Now
	}
	return &Model{opts: opts}
}

func (m *Model) Reset() {
	m.mu.Lock()
	m.goods, m.upload, m.doc, m.downloads = nil, nil, nil, 0
	m.mu.Unlock()
}

func (m *Model) Description() models.Description {
	return models.Description{
		Name:        "ЭМУЛЯТОР: терминал сбора данных (Эмулкон)",
		Description: "Виртуальный ТСД Эмулкон: товары из 1С, документ собирают в панели управления.",
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
			out[2] = fmt.Sprintf("Тест пройден. ЭМУЛЯТОР терминала сбора данных (Эмулкон): товаров %d, в документе строк %d.", len(m.goods), len(m.doc))
			out[3] = "ЭМУЛЯТОР Эмулкон"
		}
	case "UploadTable":
		err = m.uploadTable(a)
	case "DownloadTable":
		if err = a.Need(2); err == nil {
			t := downloadTable{Records: []downloadItem{}}
			for _, it := range m.doc {
				t.Records = append(t.Records, downloadItem{
					BarCodeBase64: base64.StdEncoding.EncodeToString([]byte(it.Barcode)),
					Quantity:      kkt.Number(it.Quantity), ContainerBarCodeBase64: b64(it.Container),
				})
			}
			var s string
			if s, err = kkt.Marshal(t); err == nil {
				out[1] = s
				m.downloads++
			}
		}
	case "ClearTable":
		if err = a.Need(1); err == nil {
			m.goods, m.upload = nil, nil
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

func b64(s string) string {
	if s == "" {
		return ""
	}
	return base64.StdEncoding.EncodeToString([]byte(s))
}

func unb64(s string) (string, error) {
	if s == "" {
		return "", nil
	}
	b, err := base64.StdEncoding.DecodeString(strings.TrimSpace(s))
	return string(b), err
}

// UploadTable(0 DeviceID, 1 GoodsTable, 2 PackageStatus).
func (m *Model) uploadTable(a models.Args) error {
	if err := a.Need(3); err != nil {
		return err
	}
	status, err := a.String(2)
	if err != nil {
		return err
	}
	status = strings.ToLower(strings.TrimSpace(status))
	if status != "first" && status != "regular" && status != "last" {
		return fail(ErrPackageStatus, "%q: ожидается first, regular или last", status)
	}
	s, err := a.String(1)
	if err != nil {
		return err
	}
	var t goodsTable
	if err := kkt.Unmarshal(s, &t); err != nil {
		return fail(ErrBadParams, "%v", err)
	}
	goods := make([]Good, 0, len(t.Records))
	for i, r := range t.Records {
		code, err := unb64(r.BarCodeBase64)
		if err != nil || code == "" {
			return fail(ErrBadParams, "запись %d: BarCodeBase64 — штрихкод в Base64", i+1)
		}
		cont, err := unb64(r.ContainerBarCodeBase64)
		if err != nil {
			return fail(ErrBadParams, "запись %d: ContainerBarCodeBase64 — не Base64", i+1)
		}
		goods = append(goods, Good{
			Barcode: code, Name: r.Name, Article: r.Article, Unit: r.UnitOfMeasurement,
			Characteristic: r.CharacteristicOfNomenclature, Series: r.SeriesOfNomenclature, Quality: r.Quality,
			Price: float64(r.Price), Quantity: float64(r.Quantity), Container: cont,
			Marked: r.MarkedGoodTypeCode != nil, Alcohol: bool(r.Alcohol),
		})
	}
	// Новая выгрузка — с first или одним пакетом last; полная очищает товары в терминале.
	if status == "first" || m.upload == nil || m.upload.Status == "last" {
		full := t.FullLoad != nil && bool(*t.FullLoad)
		m.upload = &upload{FullLoad: full}
		if full {
			m.goods = nil
		}
	}
	idx := map[string]int{}
	for i, g := range m.goods {
		idx[g.key()] = i
	}
	for _, g := range goods {
		if i, ok := idx[g.key()]; ok {
			m.goods[i] = g
			continue
		}
		idx[g.key()] = len(m.goods)
		m.goods = append(m.goods, g)
	}
	m.upload.Status = status
	m.upload.Packages++
	m.upload.Records += len(goods)
	m.upload.At = m.opts.Now()
	return nil
}

func (m *Model) nameOf(code string) string {
	for _, g := range m.goods {
		if g.Barcode == code {
			return g.Name
		}
	}
	return ""
}

// Act — collect: {"barcode", "quantity"} — отобрать товар (количество добавляется);
// remove: {"barcode"}; clear_document — очистить собранный документ.
func (m *Model) Act(name string, args map[string]any) (any, error) {
	m.mu.Lock()
	defer m.mu.Unlock()
	code, _ := args["barcode"].(string)
	code = strings.TrimSpace(code)
	switch name {
	case "collect":
		if code == "" {
			return nil, errors.New("barcode: штрихкод товара")
		}
		q := 1.0
		switch v := args["quantity"].(type) {
		case float64:
			q = v
		case json.Number:
			q, _ = v.Float64()
		}
		if q <= 0 || q > 1e6 {
			return nil, errors.New("quantity: больше 0")
		}
		q = math.Round(q*1000) / 1000
		for i := range m.doc {
			if m.doc[i].Barcode == code {
				m.doc[i].Quantity = math.Round((m.doc[i].Quantity+q)*1000) / 1000
				return m.doc[i], nil
			}
		}
		it := Item{Barcode: code, Name: m.nameOf(code), Quantity: q}
		m.doc = append(m.doc, it)
		return it, nil
	case "remove":
		for i := range m.doc {
			if m.doc[i].Barcode == code {
				m.doc = append(m.doc[:i], m.doc[i+1:]...)
				return map[string]any{"removed": code}, nil
			}
		}
		return nil, fmt.Errorf("в документе нет штрихкода %q", code)
	case "clear_document":
		m.doc = nil
		return map[string]any{"cleared": true}, nil
	}
	return nil, models.ErrUnknownAction
}

func (m *Model) State() map[string]any {
	m.mu.Lock()
	defer m.mu.Unlock()
	goods := m.goods
	if len(goods) > 500 {
		goods = goods[:500]
	}
	return map[string]any{
		"model": "tsd", "goods_count": len(m.goods), "goods": append([]Good{}, goods...),
		"upload": m.upload, "document": append([]Item{}, m.doc...), "downloads": m.downloads,
	}
}
