package models

// DeviceEvent — внешнее событие устройства для 1С: драйвер поднимает его вызовом
// ExternalEvent(Who, What, Data) (требования 4.8, раздел 3.8; docs/protocol.md §1.4).
// Who — DeviceID драйвера, его подставляет компонента.
type DeviceEvent struct {
	Name string `json:"name"` // bstrWhat: Barcode, TracksData, Weighting, …
	Data string `json:"data"` // bstrData
}

// EventHook — куда модель отдаёт события. Вызывается без блокировок модели.
type EventHook func(e DeviceEvent)

// Emitter — модель, которая шлёт события в 1С (сканер, считыватель, весы, дисплей).
type Emitter interface {
	SetEventHook(h EventHook)
}

// EventNames — допустимые bstrWhat: раздел 3.8 требований и разбор событий в БПО
// (ПодключаемоеОборудованиеДрайверКлиент.ОбработатьСобытие). Допустимо RU и EN написание.
var EventNames = map[string]bool{
	"Штрихкод": true, "Barcode": true, "ШтрихкодBase64": true, "BarcodeBase64": true,
	"ДанныеКарты": true, "TracksData": true, "ДанныеКартыBase64": true, "TracksDataBase64": true,
	"НажатиеКлавиши": true, "KeyPress": true, "RFIDData": true, "RFID": true,
	"ОшибкаДрайвера": true, "DriverError": true,
	"Взвешивание": true, "Weighting": true, // раздел 3.1
	"Нажатие": true, "OnClick": true, // дисплей покупателя, раздел 3.3
}
