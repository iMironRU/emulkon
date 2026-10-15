# Терминал сбора данных (раздел 3.4): методы, GoodsTable, DownloadTable

Источники: **[Т]** требования 4.8, раздел 3.4; **[Д]** БПО 3.3.4.30,
`ПодключаемоеОборудованиеДрайверКлиент` (:5160–5252). Сверено 2026-10-08.

| Англ. | Рус. (вызов в [Д]) | Параметры | Возврат |
|---|---|---|---|
| UploadTable | ВыгрузитьТаблицу (:5228) | 0 DeviceID · 1 GoodsTable STRING (XML) · 2 PackageStatus STRING | BOOL |
| DownloadTable | ЗагрузитьТаблицу (:5192) | 0 DeviceID · 1 DownloadTable STRING **out** (XML) | BOOL |
| ClearTable | ОчиститьТаблицу (:5168) | 0 DeviceID | BOOL |

- Выгрузка — пакетами (`first`, `regular`, `last`; один пакет — `last`). Здесь БПО считает
  статусы правильно ([Д]:5222, 5242), в отличие от принтера этикеток.
- Загрузка — одним вызовом. ClearTable — «очищает загруженную ранее таблицу товаров» [Т].

**GoodsTable** — `<Table FullLoad="true|false"><Record …/></Table>`. `FullLoad` — полная выгрузка:
товары в терминале очищаются. Record: `BarCodeBase64` (обязателен, штрихкод в Base64), `Name`,
`Article`, `UnitOfMeasurement`, `CharacteristicOfNomenclature`, `SeriesOfNomenclature`, `Quality`,
`Price`, `Quantity`, `ContainerBarCodeBase64`, `MarkedGoodTypeCode`, `Alcohol` и алкогольные
реквизиты. Цены в примере [Т] — через запятую.

**DownloadTable** — `<Table><Record BarCodeBase64 Quantity [ContainerBarCodeBase64]
[AlcoholExciseStampBase64] …/></Table>`: `BarCodeBase64` и `Quantity` обязательны.
