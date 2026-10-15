// Package models — модели виртуальных устройств (ARCHITECTURE §6).
//
// Модель — конечный автомат устройства. Ядро сервера знает только этот
// интерфейс; конкретные типы (ККТ, эквайринг, весы, …) регистрируются в Registry.
// Имена методов и параметры берутся из «Требований к разработке драйверов
// подключаемого оборудования» (docs/bpo-requirements), а не придумываются:
// пока модель типа не реализована, работает Stub.
package models

import (
	"context"
	"fmt"
	"sort"
	"strings"
	"time"

	"emulkon/internal/qr"
)

// Result — результат вызова метода драйвера (тело ответа Device API, docs/protocol.md §1.2).
type Result struct {
	// Return — возвращаемое значение метода: bool (BOOL), int64 (LONG) или nil (нет значения).
	Return any
	// Params — значения всех позиционных параметров после вызова (выходные заполнены).
	// nil — параметры не менялись. Иначе длина обязана совпадать с переданными.
	Params []any
	// ErrorCode, ErrorText — последняя ошибка устройства; её же вернёт GetLastError.
	ErrorCode int
	ErrorText string
}

// Description — сведения о модели для UI. Описание драйвера для БПО (GetDescription)
// отдаёт сама компонента — его запрашивают до подключения к серверу.
type Description struct {
	Name        string `json:"name"`
	Description string `json:"description"`
	Version     string `json:"version"`
	IsEmulator  bool   `json:"is_emulator"`
}

// Model — виртуальное устройство. Вызовы Handle для одного устройства
// сериализуются вызывающей стороной (устройство обрабатывает одну команду за раз).
//
// params — позиционные параметры как их передала 1С: string, json.Number, bool или nil.
type Model interface {
	Handle(ctx context.Context, method string, params []any) Result
	State() map[string]any
	Reset()
	Description() Description
}

// StateSetter — модель, состояние которой можно менять из Control API
// (PUT /control/v1/devices/{id}/state): бумага, крышка, ФН, связь с ОФД и т.п.
// Неизвестный ключ — ошибка с перечнем допустимых.
type StateSetter interface {
	SetState(patch map[string]any) error
}

// PrintedDocument — документ, «напечатанный» устройством (ARCHITECTURE §6.1, §9.2 п.4).
type PrintedDocument struct {
	Seq        int64     `json:"seq"`                   // порядковый номер на ленте
	Kind       string    `json:"kind"`                  // check, correction, open_shift, close_shift, x_report, text, …
	Number     int       `json:"number,omitempty"`      // номер ФД, если документ фискальный
	Time       time.Time `json:"time"`                  // время формирования
	Electronic bool      `json:"electronic,omitempty"`  // только в электронном виде, не печатался
	PrintError bool      `json:"print_error,omitempty"` // сбой печати бумажной формы
	Lines      []string  `json:"lines"`
	// Layout — разметка этикеток для просмотра в веб-интерфейсе (принтер этикеток).
	Layout *LabelLayout `json:"layout,omitempty"`
	// QR — QR-код под текстом документа (чек ФНС): данные и готовая матрица — веб-интерфейс и
	// трей рисуют её как есть, без своего кодировщика.
	QR *QRImage `json:"qr,omitempty"`
}

// QRImage — QR-код документа: Rows — строки «1010…», 1 — тёмный модуль, без поля.
type QRImage struct {
	Data string   `json:"data"`
	Size int      `json:"size"`
	Rows []string `json:"rows"`
}

// NewQR кодирует данные в QR-код (internal/qr); не помещается — nil.
func NewQR(data string) *QRImage {
	c, err := qr.Encode([]byte(data))
	if err != nil {
		return nil
	}
	return &QRImage{Data: data, Size: c.Size, Rows: c.Rows()}
}

// LabelLayout — шаблон этикетки и значения полей (docs/models/printers.md). Координаты — мм.
type LabelLayout struct {
	WidthMM  float64       `json:"width_mm"`
	HeightMM float64       `json:"height_mm"`
	Fields   []LabelField  `json:"fields"`
	Labels   []LabelValues `json:"labels"` // первые этикетки пакета
	Total    int           `json:"total"`  // этикеток в пакете
}

// LabelField — поле шаблона: text, barcode, image.
type LabelField struct {
	Type        string  `json:"type"`
	Name        string  `json:"name"`
	Left        float64 `json:"left"`
	Top         float64 `json:"top"`
	Width       float64 `json:"width"`
	Height      float64 `json:"height"`
	Orientation int     `json:"orientation,omitempty"`
	FontSize    float64 `json:"font_size,omitempty"`
	FontStyle   string  `json:"font_style,omitempty"` // Bold Italic Underline StrikeOut
	Align       string  `json:"align,omitempty"`
	VAlign      string  `json:"valign,omitempty"`
	Multiline   bool    `json:"multiline,omitempty"`
	Border      string  `json:"border,omitempty"`
	BarcodeType string  `json:"barcode_type,omitempty"`
	HRI         bool    `json:"hri,omitempty"` // подпись под штрихкодом
}

// LabelValues — значения полей одной этикетки (картинка — Base64).
type LabelValues struct {
	Quantity int               `json:"quantity"`
	Values   map[string]string `json:"values"`
}

// Printer — модель с лентой печати (GET /control/v1/devices/{id}/prints).
type Printer interface {
	// Prints — последние limit документов, новые первыми.
	Prints(limit int) []PrintedDocument
	// PrintsSince — документы с Seq > seq по порядку печати (для журнала device.print).
	PrintsSince(seq int64) []PrintedDocument
}

// CopyParams — копия параметров, в которую модель пишет выходные значения.
func CopyParams(params []any) []any {
	return append([]any(nil), params...)
}

// EquipmentType — тип оборудования из таблицы «Тип оборудования» требований
// к драйверам (раздел 2). Key — наш короткий ключ: префикс device_id и ключ модели.
type EquipmentType struct {
	Key    string `json:"key"`
	Name   string `json:"equipment_type"`
	NameRU string `json:"equipment_type_ru"`
	Title  string `json:"title"`
}

// EquipmentTypes — все типы оборудования требований версии 4.8.
var EquipmentTypes = []EquipmentType{
	{"scanner", "BarcodeScanner", "СканерШтрихкода", "Сканер штрихкода"},
	{"card_reader", "CardReader", "СчитывательКарт", "Считыватель карт"},
	{"kkt", "KKT", "ККТ", "Контрольно-кассовая техника"},
	{"receipt_printer", "ReceiptPrinter", "ПринтерЧеков", "Принтер чеков"},
	{"display", "CustomerDisplay", "ДисплейПокупателя", "Дисплей покупателя"},
	{"tsd", "DataCollectionTerminal", "ТерминалСбораДанных", "Терминал сбора данных"},
	{"acquiring", "POSTerminal", "ЭквайринговыйТерминал", "Эквайринговый терминал"},
	{"scales", "WeighingScales", "ЭлектронныеВесы", "Электронные весы"},
	{"label_scales", "LabelPrintingScale", "ВесыСПечатьюЭтикеток", "Весы с печатью этикеток"},
	{"label_printer", "LabelPrinter", "ПринтерЭтикеток", "Принтер этикеток"},
	{"rfid", "RFIDReader", "СчитывательRFID", "Считыватель RFID"},
	{"recognizer", "Recognizer", "УстройствоРаспознавания", "Устройство распознавания"},
}

// BPOName — имя значения перечисления БПО ТипыПодключаемогоОборудования. Так тип пишется в
// INFO.XML пакета драйвера и приходит в ConnectEquipment (XMLСтрока). Расходится с таблицей
// требований только у считывателя карт (docs/bpo-requirements/README.md).
func (t EquipmentType) BPOName() string {
	if t.Key == "card_reader" {
		return "СчитывательМагнитныхКарт"
	}
	return t.NameRU
}

// LookupEquipmentType находит тип по значению EquipmentType из БПО (англ. или рус.,
// без учёта регистра) или по нашему ключу.
func LookupEquipmentType(s string) (EquipmentType, bool) {
	s = strings.TrimSpace(s)
	for _, t := range EquipmentTypes {
		if strings.EqualFold(s, t.Name) || strings.EqualFold(s, t.NameRU) || strings.EqualFold(s, t.Key) ||
			strings.EqualFold(s, t.BPOName()) {
			return t, true
		}
	}
	return EquipmentType{}, false
}

// Factory создаёт модель для устройства deviceID.
type Factory func(t EquipmentType, deviceID string) Model

// Registry — реестр моделей по типам оборудования.
type Registry struct {
	factories map[string]Factory // по EquipmentType.Key
}

func NewRegistry() *Registry { return &Registry{factories: map[string]Factory{}} }

// Register связывает тип оборудования (по ключу) с фабрикой модели.
func (r *Registry) Register(key string, f Factory) {
	r.factories[key] = f
}

// Resolve находит зарегистрированный тип по значению EquipmentType из БПО.
func (r *Registry) Resolve(equipmentType string) (EquipmentType, error) {
	t, ok := LookupEquipmentType(equipmentType)
	if !ok {
		return t, fmt.Errorf("%q нет в таблице «Тип оборудования» требований к драйверам", equipmentType)
	}
	if _, ok := r.factories[t.Key]; !ok {
		return t, fmt.Errorf("%s: модель не зарегистрирована на сервере", t.Name)
	}
	return t, nil
}

// New создаёт модель указанного типа.
func (r *Registry) New(t EquipmentType, deviceID string) (Model, error) {
	f, ok := r.factories[t.Key]
	if !ok {
		return nil, fmt.Errorf("тип оборудования %s не поддерживается сервером", t.Name)
	}
	return f(t, deviceID), nil
}

// Types — зарегистрированные типы в порядке таблицы требований.
func (r *Registry) Types() []EquipmentType {
	var out []EquipmentType
	for _, t := range EquipmentTypes {
		if _, ok := r.factories[t.Key]; ok {
			out = append(out, t)
		}
	}
	// Типы вне таблицы (плагины) — в конце по алфавиту.
	var extra []string
	for k := range r.factories {
		if _, ok := LookupEquipmentType(k); !ok {
			extra = append(extra, k)
		}
	}
	sort.Strings(extra)
	for _, k := range extra {
		out = append(out, EquipmentType{Key: k, Name: k, Title: k})
	}
	return out
}

// DefaultRegistry — все типы оборудования на заглушке.
// По мере реализации моделей (этапы 2–4) тип перерегистрируется на настоящую.
func DefaultRegistry(version string) *Registry {
	r := NewRegistry()
	for _, t := range EquipmentTypes {
		r.Register(t.Key, func(t EquipmentType, deviceID string) Model { return NewStub(t, deviceID, version) })
	}
	return r
}
