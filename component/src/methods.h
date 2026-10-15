// Таблица методов драйвера: имена (англ./рус.), число параметров и кто обслуживает вызов.
// Источник — «Требования к разработке драйверов подключаемого оборудования» 4.8,
// docs/bpo-requirements/kkt-methods.md, pos-terminal.md. Порядок не менять: индекс — номер метода для 1С.
#pragma once

#include <string>

namespace emu {

enum class Handling {
    Local,       // компонента отвечает сама: БПО вызывает до подключения (нет адреса сервера)
    Connect,     // ConnectEquipment: открыть сессию и передать вызов
    Disconnect,  // DisconnectEquipment: передать вызов и забыть сессию
    Test,        // EquipmentTest: пробная сессия по переданным параметрам
    Forward,     // всё остальное: на сервер по DeviceID из параметра 0
};

enum class Local {
    None,
    InterfaceRevision,
    Description,
    LastError,
    EquipmentParameters,
    AutoSetup,
    ApplicationInformation,
    AdditionalActions,
    DoAdditionalAction,
    LocalizationPattern,
    SetLocalization,
    // Старая схема подключения (требования до 4.3, БПО УТ 10.3): параметры по одному, затем Подключить.
    LegacySetParameter,
    LegacyOpen,
    LegacyClose,
    LegacyDeviceTest,
    LegacyParameters,
    LegacyPrintSlipOnTerminal,
};

struct MethodDef {
    const char16_t* en;
    const char16_t* ru;
    int params;
    Handling handling;
    Local local;
    // waitsForPerson — операция ждёт человека на экране устройства (карту на терминале):
    // компонента ждёт ответа сервера дольше обычного таймаута (kPersonWaitMs).
    bool waitsForPerson = false;
};

// Сколько компонента ждёт ответа сервера на операцию с человеком: больше предела
// ожидания карты на сервере (card_timeout_s ≤ 240 с, docs/models/acquiring.md).
constexpr int kPersonWaitMs = 300000;

const MethodDef* methods();
int methodCount();

// findMethod — номер метода по имени (любой язык, без учёта регистра) или -1.
int findMethod(const std::u16string& name);

}  // namespace emu
