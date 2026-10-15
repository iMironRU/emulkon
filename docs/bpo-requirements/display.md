# Дисплей покупателя (раздел 3.3): методы, CustomerDisplayOptions, нажатия

Источники: **[Т]** требования 4.8, раздел 3.3; **[Д]** БПО 3.3.4.30,
`ПодключаемоеОборудованиеДрайверКлиент` (:1964–2383). Сверено 2026-10-08.

| Англ. | Рус. (вызов в [Д]) | Параметры | Возврат |
|---|---|---|---|
| GetCustomerDisplayOptions | ПолучитьПараметрыДисплеяПокупателя (:1981) | 0 DeviceID · 1 CustomerDisplayOptions STRING **out** (XML) | BOOL |
| StringOutputOnCustomerDisplay | ВывестиСтрокуНаДисплейПокупателя (:2064) | 0 DeviceID · 1 Strings STRING in | BOOL |
| ClearCustomerDisplay | ОчиститьДисплейПокупателя (:2317) | 0 DeviceID | BOOL |
| AddMediaFile | ДобавитьМедиафайл (:2363) | 0 DeviceID · 1 FileName STRING · 2 FileData STRING (Base64) | BOOL |
| OutputHTML | ВывестиHTML (:2155) | 0 DeviceID · 1 HTML STRING | BOOL |
| QRCodeOutputOnCustomerDisplay | ВывестиQRКодНаДисплейПокупателя (:2118) | 0 DeviceID · 1 QRCodeValue STRING · 2 QRCodePicture STRING (PNG в Base64) | BOOL |
| DisplayOperationStatus | ОтобразитьСтатусОперации (:2238) | 0 DeviceID · 1 OperationStatus LONG: 0 — нет, 1 — успешно, 2 — не выполнена | BOOL |
| DisplayWorkplaceStatus | ОтобразитьСтатусРабочегоМеста (:2286) | 0 DeviceID · 1 WorkplaceStatus LONG 0–4 · 2 SoundSignal LONG 0/1 | BOOL |

- **Строки**: одна строка параметра — одна строка дисплея; пустая строка не меняет строку на
  дисплее [Т].
- **CustomerDisplayOptions** — `<CustomerDisplayOptions Columns Rows DisplayText DisplayGraphics
  DisplayQRCode DisplayHTML DisplayStatus DisplayWorkplaceStatus DisplayInteractiveMode/>`. БПО
  читает флаги через `ВРег(…) = "TRUE"` ([Д]:1992–1998) и по ним решает, что выводить
  (`ВывестиИнформациюНаДисплейПокупателя`, [Д]:2183): при `DisplayHTML` — только HTML; иначе
  строки, затем QR, затем статус операции (если код 1 или 2); статус рабочего места — если
  `DisplayWorkplaceStatus`.
- Ревизии: статус операции — с 3009, статус рабочего места — с 4001, медиафайлы — с 4008 ([Д]).
  Медиафайлы БПО отправляет перед HTML, если включено их кэширование (`КешироватьМедиаФайлы`).
- Статус рабочего места: 0 — не установлен, 1 — сервисный режим, 2 — свободно, 3 — работает,
  4 — ошибка. Тот же статус без звука БПО повторно не шлёт ([Д]:2277).
- **Нажатия** (`DisplayInteractiveMode`): дисплей с сенсорным экраном шлёт `ExternalEvent` с
  bstrWhat `OnClick` (в [Д] ещё `Нажатие`) и именем элемента в bstrData; БПО подписывается, только
  если флаг включён ([Д]:868). Ошибка устройства — `ОшибкаДрайвера` / `DriverError`.
