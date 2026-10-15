package models

// Field — ключ PUT /control/v1/devices/{id}/state в виде поля формы (docs/protocol.md §2.3).
// Ключи и допустимые значения — из таблиц docs/models/*.md; проверяет их сама модель (SetState).
type Field struct {
	Key     string   `json:"key"`
	Title   string   `json:"title"`
	Kind    string   `json:"kind"` // bool, number, int, enum, string, json
	Options []Option `json:"options,omitempty"`
	Min     *float64 `json:"min,omitempty"`
	Max     *float64 `json:"max,omitempty"`
	Hint    string   `json:"hint,omitempty"`
	// Path — где текущее значение лежит в State() («faults.paper»); пусто — по Key. «-» — значения
	// в состоянии нет (действие вроде «сдвинуть смену»).
	Path string `json:"path,omitempty"`
	// Setting — настройка: сохраняется у устройства и задаётся классу по умолчанию (ARCHITECTURE §4.3).
	// Иначе — состояние и сбои: меняется, но не сохраняется.
	Setting bool `json:"setting"`
}

// Option — значение перечисления.
type Option struct {
	Value string `json:"value"`
	Title string `json:"title"`
}

func rng(min, max float64) (*float64, *float64) { return &min, &max }

func boolField(key, title string, setting bool, hint string) Field {
	return Field{Key: key, Title: title, Kind: "bool", Setting: setting, Hint: hint}
}

func intField(key, title string, min, max float64, setting bool, hint string) Field {
	lo, hi := rng(min, max)
	return Field{Key: key, Title: title, Kind: "int", Min: lo, Max: hi, Setting: setting, Hint: hint}
}

func enumField(key, title string, setting bool, hint string, opts ...string) Field {
	f := Field{Key: key, Title: title, Kind: "enum", Setting: setting, Hint: hint}
	for i := 0; i+1 < len(opts); i += 2 {
		f.Options = append(f.Options, Option{Value: opts[i], Title: opts[i+1]})
	}
	return f
}

func at(f Field, path string) Field {
	f.Path = path
	return f
}

// drawerFields — денежный ящик ККТ и принтера чеков (models.Drawer).
var drawerFields = []Field{
	boolField("drawer_open", "Денежный ящик открыт", false, "открывает OpenCashDrawer; закрыть — здесь"),
	intField("drawer_auto_close_s", "Ящик закрывается сам через, с", 0, 600, true, "0 — открыт, пока не закроют в карточке"),
}

// printerFields — общее у принтеров чеков и этикеток (docs/models/printers.md).
func printerFields(paper string) []Field {
	return []Field{
		boolField("paper", paper, false, ""),
		boolField("cover_open", "Крышка открыта", false, ""),
	}
}

// keyboardFields — клавиатурный режим сканера и считывателя (docs/models/input.md).
var keyboardFields = []Field{
	enumField("input_mode", "Как передавать код", true, "клавиатура — как сканер в режиме HID: трей рабочего места вводит код в активное окно",
		"driver", "через драйвер — событие в 1С", "keyboard", "как клавиатура — ввод в активное окно", "both", "и так, и так"),
	enumField("keyboard_suffix", "После кода нажать", true, "", "enter", "Enter", "tab", "Tab", "none", "ничего"),
	intField("keyboard_delay_ms", "Пауза перед вводом, мс", 0, 10000, true, "успеть переключиться в окно 1С"),
}

var cash = func() Field {
	lo := 0.0
	return Field{Key: "cash_balance", Title: "Наличные в кассе", Kind: "number", Min: &lo}
}()

// schemas — ключи состояния по классам (ключ EquipmentType.Key).
var schemas = map[string][]Field{
	"kkt": {
		intField("line_length", "Ширина ленты, символов", 20, 80, true, "GetLineLength"),
		{Key: "taxation_systems", Title: "Системы налогообложения", Kind: "string", Setting: true, Path: "-",
			Hint: "коды СНО через запятую; пусто — как при регистрации"},
		at(enumField("km_result", "Ответ ОИСМ по коду маркировки", true, "", "valid", "КМ корректен",
			"invalid", "КМ некорректен", "suspended", "Оборот приостановлен", "unavailable", "ОИСМ недоступна"), "km.result"),
		{Key: "km_rules", Title: "Правила ответа ОИСМ по коду", Kind: "json", Setting: true, Path: "km.rules",
			Hint: `первое совпадение по подстроке кода, иначе ответ выше: [{"title":"Просроченное молоко","contains":"0104600000000000","result":"invalid"}]`},
		at(intField("km_delay_ms", "Задержка ответа ОИСМ, мс", 0, 600000, true, ""), "km.delay_ms"),
		at(boolField("marking_strict", "Требовать подтверждённый КМ в чеке", true, ""), "km.strict"),
		boolField("piot", "ФН работает с ТС ПИоТ", true, "SupportFNPiot в параметрах ККТ и методы Piot* (требования 5.0); криптография тестовая"),
		at(boolField("paper", "Бумага есть", false, ""), "faults.paper"),
		at(boolField("cover_open", "Крышка открыта", false, ""), "faults.cover_open"),
		at(boolField("ofd_online", "Связь с ОФД", false, "включение очищает очередь непереданных"), "ofd.online"),
		at(boolField("fn_error", "Срочная замена ФН", false, "только признак FNError"), "faults.fn_error"),
		at(boolField("fn_overflow", "Память ФН переполнена", false, "фискальные операции — ошибка 14"), "faults.fn_overflow"),
		at(boolField("fn_fail", "Ресурс ФН исчерпан", false, "фискальные операции — ошибка 13"), "faults.fn_fail"),
		at(boolField("print_error_after_fiscal", "Сбой печати после фискализации", false, "PrintError"), "faults.print_error_after_fiscal"),
		boolField("fiscal", "Зарегистрирована", false, "выключение снимает регистрацию и закрывает смену"),
		cash,
		{Key: "shift_opened_hours_ago", Title: "Смена открыта часов назад", Kind: "number", Path: "-",
			Hint: "25 — смена истекла"},
		drawerFields[0], drawerFields[1],
	},
	"acquiring": {
		enumField("card_mode", "Как прикладывается карта", true, "", "interactive", "на экране терминала",
			"approve", "сразу одобрено", "decline", "сразу отказ", "timeout", "истекло время"),
		enumField("card", "Карта", true, "для «сразу одобрено» и по умолчанию на экране", "mir", "МИР",
			"visa", "VISA", "mastercard", "Mastercard", "unionpay", "UnionPay", "sbp", "СБП"),
		intField("card_timeout_s", "Ожидание карты, с", 5, 240, true, ""),
		boolField("print_slip_on_terminal", "Слип печатает терминал", true, "PrintSlipOnTerminal"),
		boolField("short_slip", "Короткий слип", true, "ShortSlip"),
		boolField("cash_withdrawal", "Выдача наличных", true, ""),
		boolField("partial_cancellation", "Частичная отмена", true, ""),
		boolField("list_card_transactions", "Список операций", true, ""),
		boolField("kopeck_rules", "Исход по копейкам суммы", true,
			".61 отказ банка, .62 недостаточно средств, .63 неверный ПИН, .64 отмена, .65 карту не приложили — сразу, без экрана"),
		{Key: "decline_above", Title: "Отказ, если сумма больше", Kind: "number", Min: func() *float64 { v := 0.0; return &v }(),
			Setting: true, Hint: "отказ банка (61) без экрана; 0 — выключено"},
		boolField("bank_online", "Связь с банком", false, ""),
	},
	"scales": {
		{Key: "max_weight", Title: "Предел весов, кг", Kind: "number", Min: func() *float64 { v := 0.001; return &v }(), Setting: true},
		enumField("weight_mode", "Откуда вес", true, "", "manual", "кладут на экране весов", "ask", "спрашивать на каждое взвешивание"),
		intField("prompt_timeout_s", "Ожидание веса, с", 5, 240, true, ""),
		{Key: "weight", Title: "Брутто на платформе, кг", Kind: "number", Min: func() *float64 { v := 0.0; return &v }()},
		{Key: "tare", Title: "Тара, кг", Kind: "number", Min: func() *float64 { v := 0.0; return &v }()},
		boolField("stable", "Вес стабилен", false, ""),
		boolField("placement_error", "Товар задевает край", false, ""),
		{Key: "zones", Title: "Зоны пересечения", Kind: "string", Hint: "через запятую: err-1,err-2"},
	},
	"label_scales": {
		{Key: "barcode_prefix", Title: "Префикс весового штрихкода", Kind: "string", Setting: true, Hint: "две цифры, 20–29"},
	},
	"receipt_printer": append([]Field{
		intField("line_length", "Ширина строки, символов", 24, 80, true, ""),
		boolField("barcodes", "Печатает штрихкоды", true, "выключено — любой штрихкод даёт ошибку 92"),
		drawerFields[0], drawerFields[1],
	}, printerFields("Бумага есть")...),
	"label_printer": printerFields("Этикетки есть"),
	"scanner": append([]Field{
		enumField("event_format", "Формат события", true, "", "plain", "Barcode", "base64", "BarcodeBase64"),
		{Key: "library", Title: "Библиотека штрихкодов", Kind: "json", Setting: true,
			Hint: `свои штрихкоды и коды маркировки — кнопки в карточке: [{"title":"Молоко","barcode":"4601234567893"}]`},
	}, keyboardFields...),
	"card_reader": append([]Field{
		enumField("event_format", "Формат события", true, "", "plain", "TracksData", "base64", "TracksDataBase64"),
		{Key: "tracks", Title: "Дорожки", Kind: "json", Setting: true,
			Hint: `[{"use":false,"prefix":0,"suffix":0},{"use":true,"prefix":0,"suffix":13},{"use":false,"prefix":0,"suffix":0}]`},
		{Key: "library", Title: "Библиотека карт", Kind: "json", Setting: true,
			Hint: `свои карты — кнопки в карточке: [{"title":"Бонусная","track2":"7770000000024=2912"}]`},
	}, keyboardFields...),
	"display": {
		intField("columns", "Символов в строке", 8, 80, true, ""),
		intField("rows", "Строк", 1, 20, true, "изменение очищает экран"),
		boolField("text", "Умеет текст", true, ""),
		boolField("graphics", "Умеет графику", true, ""),
		boolField("qr", "Умеет QR-код", true, ""),
		boolField("html", "Умеет HTML", true, ""),
		boolField("status", "Умеет статусы операций", true, ""),
		boolField("workplace_status", "Умеет статус рабочего места", true, ""),
		boolField("interactive", "Сенсорный (нажатия)", true, ""),
		boolField("broken", "Не отвечает", false, ""),
	},
	"recognizer": {
		boolField("get_image", "Отдаёт изображение", true, ""),
		boolField("hardware", "Аппаратное распознавание", true, ""),
		{Key: "candidates", Title: "Товары на весах", Kind: "json", Setting: true,
			Hint: `[{"code":1003,"percent":0.95}]`},
	},
}

// Schema — ключи состояния класса для формы; nil — у класса нет настраиваемых ключей.
func Schema(typeKey string) []Field { return schemas[typeKey] }

// SplitSettings делит изменение состояния на настройки класса и прочее (ARCHITECTURE §4.3).
func SplitSettings(typeKey string, patch map[string]any) (settings map[string]any) {
	for _, f := range schemas[typeKey] {
		if v, ok := patch[f.Key]; ok && f.Setting {
			if settings == nil {
				settings = map[string]any{}
			}
			settings[f.Key] = v
		}
	}
	return settings
}
