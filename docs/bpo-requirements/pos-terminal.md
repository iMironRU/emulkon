# Эквайринговый терминал (раздел 3.9): методы и XML

Источники — как у ККТ ([kkt-methods.md](kkt-methods.md)):
- **[Т]** «Требования к разработке драйверов подключаемого оборудования» 4.8, раздел 3.9;
- **[Д]** БПО 3.3.4.30, `CommonModules\ПодключаемоеОборудованиеДрайверКлиент` (вызовы драйвера);
- **[П]** там же, `ОборудованиеПлатежныеСистемыКлиентСервер.ПолучитьXMLПакетДляОперации` (входной XML)
  и `ОборудованиеПлатежныеСистемы.ОбработатьДанныеОперации` (что читается из ответа).

Сверено 2026-10-08.

## Какие методы вызывает БПО

Ветку выбирает ревизия интерфейса ([Д]:96–120). **Эмулятор объявляет 4008**, поэтому БПО идёт
по XML-ветке (ревизия > 4002): у операции четыре параметра — DeviceID, входной XML, выходной
XML, текст слипа. Позиционные методы `ОплатитьПлатежнойКартой`, `ВернутьПлатежПоПлатежнойКарте`
и т.п. (ревизии 3005–4002) эмулятор не реализует.

| Англ. | Рус. (вызов в [Д]) | Параметры | Команда БПО |
|---|---|---|---|
| TerminalParamters | ПараметрыТерминала (:4258) | 0 DeviceID · 1 TerminalParameters STRING **out** (XML) | при подключении ([Д]:860) |
| Pay | Оплатить (:4685) | 0 DeviceID · 1 OperationParameters in (XML) · 2 OperationParameters **out** (XML) · 3 Slip **out** | AuthorizeSales |
| ReturnPayment | ВернутьПлатеж (:4691) | то же | AuthorizeRefund |
| CancelPayment | ОтменитьПлатеж (:4703) | то же | AuthorizeVoid |
| PayWithCashWithdrawal | Оплата**C**ВыдачейНаличных (:4711) | то же | PayByPaymentCardWithCashWithdrawal |
| PurchaseWithEnrollment | ПокупкаСЗачислением (:4724) | то же | PurchaseWithEnrollment |
| Authorisation | Преавторизация (:4736) | то же | AuthorizePreSales |
| AuthConfirmation | ЗавершитьПреавторизацию (:4742) | то же | AuthorizeCompletion |
| CancelAuthorisation | ОтменитьПреавторизацию (:4748) | то же | AuthorizeVoidPreSales |
| PayCertificate | ОплатитьСертификатом (:5049) | то же | PayElectronicCertificate |
| ReturnCertificate | ВернутьСертификатом (:5053) | то же | ReturnElectronicCertificate |
| OperationStatus | СтатусОперации (:4801) | 0 DeviceID · 1 RequestID in · 2 OperationParameters **out** · 3 Status LONG **out** (0 — не найдена, 1 — выполнена) | OperationStatus |
| EmergencyReversal | АварийнаяОтменаОперации (:4967) | 0 DeviceID | EmergencyVoid |
| GetOperationByCards | ПолучитьОперацииПоКартам (:4414) | 0 DeviceID · 1 TableOperations STRING **out** (XML) | GetOperationByCards |
| Settlement | ИтогиДняПоКартам (:4991) | 0 DeviceID · 1 Slip STRING **out** | Settlement |
| GetCardParametrs | ПолучитьПараметрыКарты (:4333) | 0 DeviceID · 1 ConsumerPresentedQR in · 2 FromLastOperation BOOL in · 3 CardNumber **out** · 4 CardNumberHash **out** · 5 PaymentAccountReference **out** · 6 CardType **out** · 7 IsOwnCard LONG **out** | GetCardParametrs |

Внимание к именам:
- в `ОплатаCВыдачейНаличных` буква «C» — **латинская** (U+0043) и в [Т], и в [Д];
- английские имена — как в [Т], с опечатками: `TerminalParamters`, `GetCardParametrs`.

Возврат — BOOL. При `False` БПО вызывает `GetLastError` и показывает текст ([Д]:4770).
Слип БПО берёт из параметра 3 **и при отказе** ([Д]:4773, [Т]: «возвращается текст квитанции
отказа»).

**Когда БПО метод не вызывает**, а сама отвечает «Терминал не поддерживает данную операцию».
Это зависит от флагов `TerminalParameters`:
- `CashWithdrawal` — для оплаты с выдачей наличных;
- `PurchaseWithEnrollment` — для покупки с зачислением;
- `PartialCancellation` — для отмены с `AmountOriginalTransaction > 0`;
- `ListCardTransactions` — для `GetOperationByCards` и `OperationStatus`.

`OperationStatus` вызывается только при ревизии ≥ 4008 и `ListCardTransactions`.

## TerminalParameters (out)

Корень `<TerminalParameters …/>`, атрибуты. БПО читает их через `ПрочитатьКорневойЭлементXML`,
флаги сравнивает через `ВРег(…) = "TRUE"` ([Д]:4264–4280).

| Атрибут | [Т] обязателен | БПО читает |
|---|---|---|
| TerminalID | да | ● (в параметры подключения) |
| MerchantID | да | — |
| AcquiringBankIdentifier | нет | ● |
| PrintSlipOnTerminal | да | ● `false` — слип печатает ККТ (`PrintText`) |
| ShortSlip | да | ● |
| CashWithdrawal | да | ● |
| ElectronicCertificates | да | ● |
| ElectronicCertificateDiscount | нет | ● |
| PartialCancellation | да | ● |
| ConsumerPresentedQR | да | ● |
| ListCardTransactions | да | ● |
| PurchaseWithEnrollment | нет | ● |
| OperationStatus | нет | ● |
| ReturnElectronicCertificateByBasketID | нет | — |

## OperationParameters (in) — что передаёт БПО ([П])

Корень `<OperationParameters …/>`, всегда:
- `MerchantNumber`, `UseBiometrics` (`0`/`1`), `AuthorizationType` (0 — карта, 1 — СБП QR,
  2 — Плати QR, 3 — цифровой рубль);
- `Amount`, `AmountCash`, `AmountOriginalTransaction`;
- `ElectronicCertificateAmount`, `ElectronicCertificateDiscount`, `OwnFundsAmount`.

Числа — `XMLСтрока`, с точкой.

Только если заполнены: `RequestID`, `ConsumerPresentedQR`, `CardNumber`, `CardNumberHash`,
`BasketID`, `ReceiptNumber`, `RRNCode`, `AuthorizationCode`. Для отмены и возврата БПО передаёт
`RRNCode` и `AuthorizationCode` исходной операции.

`RequestID` по [Т] — 4 байта, HEX-строка из 8 символов. По нему работает `OperationStatus`.

## OperationParameters (out) — что читает БПО ([П]:201–240)

Корень любой (читается корневой элемент), эмулятор отдаёт `<OperationParameters …/>`.

| Атрибут | Как читает БПО |
|---|---|
| RRNCode | ● ссылочный номер — нужен для отмены и возврата |
| AuthorizationCode | ● |
| CardNumber | ● маскированный номер |
| CardNumberHash | ● SHA-256 PAN |
| ReceiptNumber | ● номер чека терминала |
| AuthorizationType | ● вид оплаты |
| OperationStatus | ● **`Число()`** — не оставлять пустым |
| ResultCode | ● **`Число()`** — не оставлять пустым |
| PaymentSystemIdentifier | ● 4 Visa, 5 MasterCard, 6 UnionPay, 22 Мир |
| UUID | ● идентификатор банковской транзакции |
| QRTransactionIdentifier | ● |
| Date | ● дата и время операции |

[Т] перечисляет ещё `IssuerBankIdentifier`, `BIKAcquiring`, `BIKPay`, `Amount`, `MerchantNumber`.
Эмулятор их тоже возвращает.

## TableOperations (GetOperationByCards)

`<Table><Record MerchantNumber CardNumber CardNumberHash Amount AmountCash TypeOperation
AuthorizationCode RRNCode DateTime/></Table>`. `TypeOperation` — английское имя метода (`Pay`,
`ReturnPayment`, `CancelPayment`, `Authorisation`, …).

## Слип

Текст квитанции, сформированный «эквайринговым ПО». Несколько документов разделяются строкой
`[отрезка]` или `[cut]`. Управляющие символы 1–8, 11–12, 14–31 запрещены ([Т]).

Если `PrintSlipOnTerminal = false`, БПО печатает слип на ККТ командой `PrintText` ([П]:244–258):
целиком или «по умолчанию» — короткий текст из реквизитов операции.
