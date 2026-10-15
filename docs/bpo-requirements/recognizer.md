# Устройство распознавания (раздел 3.11)

Источники: **[Т]** требования 4.8, раздел 3.11; **[Д]** БПО 3.3.4.30,
`ПодключаемоеОборудованиеДрайверКлиент` (:1618–1690). Сверено 2026-10-08.

| Англ. | Рус. | Параметры | Возврат |
|---|---|---|---|
| RecognizerOptions | ПараметрыУстройстваРаспознавания | 0 DeviceID · 1 RecognizerParamters STRING **out** (XML) | BOOL |
| GetImage | ПолучитьИзображение | 0 DeviceID · 1 Image STRING **out** (Base64) | BOOL |
| Recognize | Распознать ([Д]:1638) | 0 DeviceID · 1 Training BOOL · 2 Variants LONG · 3 GoodsRecognizeTable STRING **out** (XML) | BOOL |
| ConfirmRecognize | ПодтвердитьРаспознавание ([Д]:1673) | 0 DeviceID · 1 Code LONG | BOOL |

- БПО 3.3.4.30 вызывает только `Распознать` и `ПодтвердитьРаспознавание`.
- `Training=true` — устройство ждёт подтверждения с последующим дообучением.
- **RecognizerParamters** (так, с опечаткой, в [Т]) — `<RecognizerParamters GetImage
  HardwareRecognizer/>`, оба bool и необязательны.
- **GoodsRecognizeTable** — `<Table><Record Code RecognizePercent/></Table>`: `Code` — код товара
  (long), `RecognizePercent` — вероятность, 1 = 100%.
