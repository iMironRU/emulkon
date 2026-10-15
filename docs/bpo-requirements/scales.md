# Весы (раздел 3.1) и весы с печатью этикеток (раздел 3.2): методы, XML, события

Источники — как у ККТ ([kkt-methods.md](kkt-methods.md)):
- **[Т]** «Требования к разработке драйверов подключаемого оборудования» 4.8, разделы 3.1 и 3.2;
- **[Д]** БПО 3.3.4.30, `CommonModules\ПодключаемоеОборудованиеДрайверКлиент`;
- **[М]** там же, `МенеджерОборудованияКлиент.ПолучитьСобытиеОтУстройства` / `ОбработатьСобытиеОтУстройства`
  (разбор внешних событий).

Сверено 2026-10-08.

## Электронные весы (ЭлектронныеВесы, WeighingScales)

| Англ. | Рус. (вызов в [Д]) | Параметры | Возврат |
|---|---|---|---|
| ScaleOptions | ПараметрыВесов (:5127) | 0 DeviceID · 1 ScaleOptions STRING **out** (XML) | BOOL |
| Calibrate | УстановитьВесТары (:1945) | 0 DeviceID · 1 TareWeight DOUBLE in (0 — тара = текущий вес) | BOOL |
| GetData | ПолучитьДанные (в [Д] не вызывается) | 0 DeviceID · 1 Data STRING **out** (XML) | BOOL |
| GetWeight | ПолучитьВес (:1900) | 0 DeviceID · 1 Weight DOUBLE **out** | BOOL |

- `ПараметрыВесов` БПО вызывает при подключении, если ревизия ≥ 4000 ([Д]:874–880). Флаги
  сравнивает через `ВРег(…) = "TRUE"`:
  - `Calibrate` — без него `УстановитьВесТары` не вызывается («Оборудование не поддерживает
    данную операцию», [Д]:1934);
  - `WeighingEvent` — БПО подписывается на события `Взвешивание` / `Weighting`.
- `ПолучитьВес`: `Вес = Число(Параметры[1])`, отрицательный становится 0 ([Д]:1903). Вес — в
  килограммах.

**ScaleOptions** — `<ScaleOptions WeighingEvent AdvancedData Calibrate PlacementControl/>`,
все атрибуты bool и необязательны.

**Data** — `<Data Weight Stable PlacementError IntersectionZones/>`: Weight — decimal, обязателен;
IntersectionZones — «err,err-1,err-2».

**Событие «Взвешивание»** ([Т] 3.1, разбор — [Д]:1460):
- `ExternalEvent(Who, What, Data)`, What — `Взвешивание` или `Weighting`;
- Data — `<вес>;<stable|unstable>;<err,err-N,…>`, хвост необязателен: `324.56;stable`,
  `34.12;unstable;err-8`, `32.67`. Вес — `xs:decimal` (точка); БПО делает `Число()`.
- **Who** — DeviceID, который драйвер вернул из `ConnectEquipment`: БПО ищет подключение по
  `Подключение.ИДУстройства = Источник` ([М]:36, :82).
- Ошибка устройства — событие `ОшибкаДрайвера` / `DriverError` с текстом в Data.

## Весы с печатью этикеток (ВесыСПечатьюЭтикеток, LabelPrintingScale), режим OFFLINE

| Англ. | Рус. (вызов в [Д]) | Параметры | Возврат |
|---|---|---|---|
| UploadGoods | ВыгрузитьТовары (:1709) | 0 DeviceID · 1 GoodsTable STRING in (XML) · 2 PackageStatus STRING in | BOOL |
| ClearGoods | ОчиститьТовары (:1806) | 0 DeviceID | BOOL |
| UploadProductImages | ВыгрузитьИзображенияТоваров (:1855) | 0 DeviceID · 1 TableImages STRING in (XML) · 2 PackageStatus STRING in | BOOL |

**Пакеты** ([Д]:1738–1765). Если пакет один, у него статус `last`. Если пакетов несколько:
первый — `first`, затем `regular`, последний — `last`. `ВыгрузитьТоварыПакетом` шлёт один
пакет со статусом `last`. Картинки — так же, при ревизии ≥ 3009.

**GoodsTable** — `<Table><Record …><AdditionalPrices Price1…Price4/></Record></Table>`:

| Атрибут | Обязателен | Тип | |
|---|---|---|---|
| PLU | да | long | индекс товара в весах |
| Code | да | long | код весового товара (идёт в штрихкод этикетки) |
| IsWeightGoods | да | bool | `false` — штучный товар, фасуемый на весах |
| Name | да | string | |
| Price | да | decimal | цена за кг |
| Description | нет | string | состав |
| ShelfLife | нет | long | срок годности, дней |
| WeightPieceOfGoods | нет | decimal | вес единицы штучного товара |
| ButtonNumber | нет | long | кнопка на весах |
| LabelFormat | нет | long | формат этикетки |

Пример [Т] пишет цены через запятую («95,50») — разбирать и точку, и запятую.

**TableImages** — `<Table><Record PLU Code Picture/></Table>`, Picture — PNG в Base64.

Этикетки печатают сами весы: в 1С о напечатанной этикетке ничего не приходит. На кассе её
штрихкод сканируют, и 1С разбирает его по своему шаблону весового штрихкода.
