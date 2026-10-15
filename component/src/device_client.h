// Клиент Device API (docs/protocol.md §1): сессия и вызов методов драйвера на сервере.
// Ничего не знает о ККТ и других устройствах — только «метод, параметры, ответ» (ARCHITECTURE §4.1).
#pragma once

#include <mutex>
#include <string>

#include "http.h"
#include "json.h"
#include "update.h"

namespace emu {

// Endpoint — куда и с какими правами подключаться (из ConnectionParameters).
struct Endpoint {
    std::string serverUrl;    // http://host:8737
    std::string deviceToken;  // EMULKON_DEVICE_TOKEN сервера; пусто — открытый режим
    std::string workstation;  // идентификатор рабочего места
    bool workstationAuto = false;  // workstation — имя компьютера: токен рабочего места назовёт место сам
    int timeoutMs = 30000;
    bool autoUpdate = true;   // скачивать новую версию драйвера, если сервер её предлагает
    // Экземпляр в 1С (ARCHITECTURE §4.3): ключ устройства на сервере и сведения для UI.
    std::string instanceId;
    std::string instanceName;
    std::string infobase;
    bool instanceCopy = false;  // тот же instanceId уже подключён в этом процессе
};

// CallResult — итог вызова метода. transportOk=false — до устройства не достучались
// (нет связи, сессия не открывается, ошибка протокола): message объясняет почему.
struct CallResult {
    bool transportOk = false;
    Json result;        // возвращаемое значение метода
    Json params;        // массив параметров после вызова
    int errorCode = 0;  // последняя ошибка устройства
    std::string errorText;
    std::string message;  // описание сбоя транспорта
};

class DeviceClient {
public:
    DeviceClient(HttpClient& http, Endpoint ep, std::string equipmentType, std::string driverVersion, int interfaceRevision);

    // open открывает сессию (POST /api/v1/sessions). false — message заполнено.
    bool open(std::string& message);

    // reopen — новая сессия вместо stale (сервер её не знает). Если её уже заменил другой поток
    // (команда 1С или поток событий), ничего не делает.
    bool reopen(const std::string& stale, std::string& message);

    // call выполняет метод на виртуальном устройстве. Повторяет с тем же request_id при
    // обрыве/таймауте и 503; при 401 session_unknown переоткрывает сессию.
    // timeoutMs > 0 — свой предел ожидания ответа (операции с человеком), иначе из Endpoint.
    CallResult call(const std::string& method, const Json& params, int timeoutMs = 0);

    // close закрывает сессию на сервере (DELETE /api/v1/sessions/{id}) — после «Теста устройства».
    // Ответ не важен: незакрытую сессию сервер сам удалит после простоя.
    void close();

    // Сессию читают и 1С (команды), и поток событий (events.h) — поэтому копии под замком.
    std::string deviceId() const {
        std::lock_guard<std::mutex> lock(mu_);
        return deviceId_;
    }
    std::string sessionId() const {
        std::lock_guard<std::mutex> lock(mu_);
        return sessionId_;
    }
    const Endpoint& endpoint() const { return ep_; }
    // updateOffer — DLL драйвера новее этой, если сервер предложил её при открытии сессии.
    const UpdateOffer& updateOffer() const { return offer_; }

private:
    HttpClient& http_;
    Endpoint ep_;
    std::string equipmentType_;
    std::string driverVersion_;
    int interfaceRevision_;
    mutable std::mutex mu_;  // sessionId_, deviceId_
    std::mutex openMu_;      // одно открытие сессии за раз
    std::string sessionId_;
    std::string deviceId_;
    UpdateOffer offer_;
};

// newRequestId — UUID v4 для request_id.
std::string newRequestId();

// nowRfc3339 — текущее время UTC для client_ts.
std::string nowRfc3339();

}  // namespace emu
