#include "device_client.h"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <random>
#include <thread>

namespace emu {

namespace {

const char* kSessionHeader = "X-Emulkon-Session";

std::string trimSlash(std::string s) {
    while (!s.empty() && s.back() == '/') s.pop_back();
    return s;
}

// errorCodeOf — код транспортной ошибки из тела {"error":{"code":…,"message":…}}.
std::string errorCodeOf(const std::string& body, std::string* message = nullptr) {
    Json j;
    std::string err;
    if (!parseJson(body, j, err)) return {};
    const Json* e = j.get("error");
    if (!e) return {};
    if (message) {
        if (const Json* m = e->get("message")) *message = m->text;
    }
    const Json* c = e->get("code");
    return c ? c->text : std::string();
}

}  // namespace

std::string newRequestId() {
    static thread_local std::mt19937_64 rng{std::random_device{}() ^
                                            static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count())};
    uint64_t a = rng(), b = rng();
    a = (a & 0xFFFFFFFFFFFF0FFFULL) | 0x0000000000004000ULL;  // версия 4
    b = (b & 0x3FFFFFFFFFFFFFFFULL) | 0x8000000000000000ULL;  // вариант RFC 4122
    char buf[40];
    std::snprintf(buf, sizeof buf, "%08x-%04x-%04x-%04x-%012llx", static_cast<unsigned>(a >> 32),
                  static_cast<unsigned>((a >> 16) & 0xFFFF), static_cast<unsigned>(a & 0xFFFF),
                  static_cast<unsigned>(b >> 48), static_cast<unsigned long long>(b & 0xFFFFFFFFFFFFULL));
    return buf;
}

std::string nowRfc3339() {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

DeviceClient::DeviceClient(HttpClient& http, Endpoint ep, std::string equipmentType, std::string driverVersion,
                           int interfaceRevision)
    : http_(http),
      ep_(std::move(ep)),
      equipmentType_(std::move(equipmentType)),
      driverVersion_(std::move(driverVersion)),
      interfaceRevision_(interfaceRevision) {
    ep_.serverUrl = trimSlash(ep_.serverUrl);
}

bool DeviceClient::reopen(const std::string& stale, std::string& message) {
    std::lock_guard<std::mutex> lock(openMu_);
    if (sessionId() != stale && !sessionId().empty()) return true;  // уже открыта другим потоком
    return open(message);
}

void DeviceClient::close() {
    std::string sid;
    {
        std::lock_guard<std::mutex> lock(mu_);
        sid.swap(sessionId_);
    }
    if (!sid.empty()) http_.remove(ep_.serverUrl + "/api/v1/sessions/" + sid, {}, ep_.timeoutMs < 5000 ? ep_.timeoutMs : 5000);
}

bool DeviceClient::open(std::string& message) {
    Json req = Json::object();
    req.set("workstation_id", Json::string(ep_.workstation));
    if (ep_.workstationAuto) req.set("workstation_auto", Json::boolean(true));
    req.set("device_token", Json::string(ep_.deviceToken));
    req.set("equipment_type", Json::string(equipmentType_));
    req.set("driver_version", Json::string(driverVersion_));
    req.set("driver_arch", Json::string(driverArch()));
    req.set("interface_revision", Json::number(static_cast<int64_t>(interfaceRevision_)));
    if (!ep_.instanceId.empty()) req.set("instance_id", Json::string(ep_.instanceId));
    if (!ep_.instanceName.empty()) req.set("instance_name", Json::string(ep_.instanceName));
    if (!ep_.infobase.empty()) req.set("infobase", Json::string(ep_.infobase));
    if (ep_.instanceCopy) req.set("instance_copy", Json::boolean(true));

    HttpResponse r = http_.post(ep_.serverUrl + "/api/v1/sessions", req.dump(), {}, ep_.timeoutMs);
    if (!r.error.empty()) {
        message = "Нет связи с сервером эмуляции " + ep_.serverUrl + ": " + r.error;
        return false;
    }
    if (r.status != 200) {
        std::string detail;
        std::string code = errorCodeOf(r.body, &detail);
        if (code == "bad_token") {
            message = "Сервер эмуляции отклонил токен устройства (проверьте параметр «Токен устройства»)";
            if (!detail.empty()) message += ": " + detail;
        } else {
            message = "Сервер эмуляции отказал в подключении (HTTP " + std::to_string(r.status) + "): " +
                      (detail.empty() ? r.body.substr(0, 200) : detail);
        }
        return false;
    }
    Json resp;
    std::string err;
    if (!parseJson(r.body, resp, err) || !resp.get("session_id") || !resp.get("device_id")) {
        message = "Неверный ответ сервера эмуляции при открытии сессии: " + err;
        return false;
    }
    std::lock_guard<std::mutex> lock(mu_);
    sessionId_ = resp.get("session_id")->text;
    deviceId_ = resp.get("device_id")->text;
    offer_ = parseUpdateOffer(resp.get("driver_update"));
    return true;
}

CallResult DeviceClient::call(const std::string& method, const Json& params, int timeoutMs) {
    CallResult out;
    const int timeout = timeoutMs > 0 ? timeoutMs : ep_.timeoutMs;
    Json req = Json::object();
    req.set("request_id", Json::string(newRequestId()));
    req.set("method", Json::string(method));
    req.set("params", params);
    req.set("client_ts", Json::string(nowRfc3339()));
    const std::string body = req.dump();

    // Повторы — с тем же request_id: сервер вернёт результат первого выполнения,
    // а не выполнит метод ещё раз (docs/protocol.md §1.2).
    const int maxAttempts = 3;
    bool reopened = false;
    std::string lastError;
    for (int attempt = 0; attempt < maxAttempts; ++attempt) {
        if (attempt > 0) std::this_thread::sleep_for(std::chrono::milliseconds(300 * attempt));
        std::string session = sessionId();
        if (session.empty()) {
            if (!reopen(session, lastError)) continue;
            session = sessionId();
        }
        HttpResponse r = http_.post(ep_.serverUrl + "/api/v1/devices/" + deviceId() + "/commands", body,
                                    {{kSessionHeader, session}}, timeout);
        if (!r.error.empty()) {
            lastError = "Нет связи с сервером эмуляции " + ep_.serverUrl + ": " + r.error;
            continue;
        }
        if (r.status == 401 && errorCodeOf(r.body) == "session_unknown" && !reopened) {
            // Сервер перезапущен или сессия просрочена — молча открываем новую.
            reopened = true;
            if (!reopen(session, lastError)) continue;
            --attempt;
            continue;
        }
        if (r.status == 503) {
            lastError = "Сервер эмуляции временно недоступен";
            continue;
        }
        if (r.status != 200) {
            std::string detail;
            std::string code = errorCodeOf(r.body, &detail);
            out.message = "Ошибка протокола сервера эмуляции (HTTP " + std::to_string(r.status) +
                          (code.empty() ? "" : ", " + code) + "): " + (detail.empty() ? r.body.substr(0, 200) : detail);
            return out;
        }
        Json resp;
        std::string err;
        if (!parseJson(r.body, resp, err)) {
            out.message = "Неверный ответ сервера эмуляции: " + err;
            return out;
        }
        out.transportOk = true;
        if (const Json* v = resp.get("result")) out.result = *v;
        if (const Json* v = resp.get("params")) out.params = *v;
        if (const Json* v = resp.get("error_code")) out.errorCode = static_cast<int>(v->asInt());
        if (const Json* v = resp.get("error_text")) out.errorText = v->text;
        return out;
    }
    out.message = lastError.empty() ? "Нет связи с сервером эмуляции" : lastError;
    return out;
}

}  // namespace emu
