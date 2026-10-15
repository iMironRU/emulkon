# Модель устройства распознавания (эмулятор)

Код — `server/internal/models/recognizer`. Контракт — [recognizer.md](../bpo-requirements/recognizer.md).

**Исходное состояние:** «на весах» товары 1003 (0.95) и 1023 (0.15) — как в примере требований;
изображение и аппаратное распознавание есть.

| Метод | Что делает |
|---|---|
| RecognizerOptions | `<RecognizerParamters GetImage HardwareRecognizer/>` |
| GetImage | «снимок» PNG 160×120 в Base64: столбики по кандидатам, красная рамка эмулятора |
| Recognize | кандидаты по убыванию вероятности, не больше `Variants` (1–100). Ничего на весах — пустая таблица, не ошибка. `Training=true` — устройство ждёт подтверждения |
| ConfirmRecognize | запоминает подтверждённый код (любой, не только из результата) |

| Код | Текст | Когда |
|---|---|---|
| 82 | Устройство не отдаёт изображение | `get_image: false` |
| 98, 99 | | `Variants` вне 1–100 |

| Ключ `PUT …/state` | Значение |
|---|---|
| `candidates` | `[{code, percent}]`, code — целое ≥ 0, percent — 0…1, до 100 |
| `get_image`, `hardware` | bool |

Действие `image` — снимок для панели управления. В состоянии: `candidates`, `last`
(последнее распознавание), `awaiting_confirm`, `confirmations`, `recognitions`.
