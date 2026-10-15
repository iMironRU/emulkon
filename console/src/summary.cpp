#include "summary.h"

#include "json.h"

namespace console {

std::wstring listenAddr(const ServerSettings& s) {
    if (!s.addr.empty()) return s.addr;
    return s.network == L"1" ? L"0.0.0.0:8737" : L"127.0.0.1:8737";
}

std::wstring localUrl(const std::wstring& listen) {
    std::wstring host, port;
    size_t colon = listen.rfind(L':');
    if (colon == std::wstring::npos) {
        host = listen;
        port = L"8737";
    } else {
        host = listen.substr(0, colon);
        port = listen.substr(colon + 1);
    }
    if (host.empty() || host == L"0.0.0.0" || host == L"[::]" || host == L"::") host = L"127.0.0.1";
    if (port.empty()) port = L"8737";
    return L"http://" + host + L":" + port;
}

bool isLocalOnly(const std::wstring& listen) {
    return listen.rfind(L"127.", 0) == 0 || listen.rfind(L"localhost", 0) == 0 || listen.rfind(L"[::1]", 0) == 0;
}

std::wstring serviceText(Service s) {
    switch (s) {
        case Service::Missing: return L"не установлена";
        case Service::Stopped: return L"остановлена";
        case Service::Starting: return L"запускается";
        case Service::Running: return L"работает";
        case Service::Stopping: return L"останавливается";
        default: return L"состояние неизвестно";
    }
}

namespace {

// failure — почему запрос не удался, по-человечески.
std::wstring failure(const Reply& r) {
    if (r.status == 0) return L"нет связи с сервером" + (r.error.empty() ? std::wstring() : L": " + widen(r.error));
    if (r.status == 401 || r.status == 403) return L"нужен токен управления: «Действие → Токен управления для консоли…»";
    return L"ошибка сервера: HTTP " + std::to_wstring(r.status);
}

// parsed — тело ответа 200 как JSON-объект; иначе nullptr и причина в why.
bool parsed(const Reply& r, emu::Json& out, std::wstring& why) {
    if (r.status != 200) {
        why = failure(r);
        return false;
    }
    std::string err;
    if (!emu::parseJson(r.body, out, err) || !out.isObject()) {
        why = L"непонятный ответ сервера";
        return false;
    }
    return true;
}

std::wstring str(const emu::Json& o, const char* key) {
    const emu::Json* v = o.get(key);
    return v && v->isString() ? widen(v->text) : std::wstring();
}

bool flag(const emu::Json& o, const char* key) {
    const emu::Json* v = o.get(key);
    return v && v->isBool() && v->b;
}

// count — длина массива key или причина, почему её нет.
std::wstring count(const Reply& r, const char* key) {
    emu::Json o;
    std::wstring why;
    if (!parsed(r, o, why)) return why;
    const emu::Json* a = o.get(key);
    if (!a || !a->isArray()) return L"непонятный ответ сервера";
    return std::to_wstring(a->items.size());
}

}  // namespace

std::vector<Row> summaryRows(const Inputs& in) {
    std::vector<Row> rows;
    const std::wstring listen = listenAddr(in.settings);
    rows.push_back({L"Служба Emulkon", serviceText(in.service)});

    emu::Json info;
    std::wstring why;
    bool haveInfo = parsed(in.info, info, why);
    rows.push_back({L"Версия сервера", haveInfo ? str(info, "version") : why});
    rows.push_back({L"Версия консоли", widen(EMULKON_VERSION)});

    std::wstring where = listen;
    if (in.settings.readable) {
        where += isLocalOnly(listen) ? L" — только этот компьютер" : L" — кассы в сети";
    } else {
        where += L" (настройки службы видны администратору)";
    }
    rows.push_back({L"Адрес", where});
    rows.push_back({L"Веб-интерфейс", localUrl(listen) + L"/"});

    if (haveInfo) {
        rows.push_back({L"Доступ к устройствам", flag(info, "device_open_mode") ? L"открытый, без токена" : L"по токену рабочего места"});
        rows.push_back({L"Доступ к управлению", flag(info, "control_open_mode") ? L"открытый, без токена" : L"по токену управления"});
    }
    rows.push_back({L"Устройств", count(in.devices, "devices")});
    rows.push_back({L"Рабочих мест", count(in.workstations, "workstations")});
    return rows;
}

std::vector<Row> serviceRows(const ServiceInfo& svc, const ServerSettings& s, bool consoleToken, const std::wstring& dataDir) {
    const std::wstring admin = L"видно администратору";
    std::vector<Row> rows;
    rows.push_back({L"Состояние", serviceText(svc.state)});
    if (svc.state != Service::Missing) {
        rows.push_back({L"Тип запуска", svc.configKnown ? svc.startType : admin});
        rows.push_back({L"Учётная запись", svc.configKnown ? svc.account : admin});
        rows.push_back({L"Команда", svc.configKnown ? svc.command : admin});
    }
    const std::wstring listen = listenAddr(s);
    if (!s.readable) {
        rows.push_back({L"Адрес", listen + L" (настройки службы видны администратору)"});
        rows.push_back({L"Токен управления", admin});
        rows.push_back({L"Токен устройств", admin});
    } else {
        rows.push_back({L"Адрес", listen});
        rows.push_back({L"Кассы в сети",
                        isLocalOnly(listen) ? L"нет — только этот компьютер"
                                            : L"да — подключения с других компьютеров, правило брандмауэра «Emulkon (TCP 8737)»"});
        rows.push_back({L"Токен управления", s.controlToken ? L"задан — веб-интерфейс, трей и консоль спрашивают токен"
                                                            : L"не задан — управлять может любой, кто достучится до сервера"});
        rows.push_back({L"Токен устройств", s.deviceToken ? L"задан — драйверу нужен он или токен рабочего места"
                                                          : L"не задан — подключается любой драйвер без токена рабочего места"});
        if (s.policy) rows.push_back({L"Групповая политика", L"часть настроек задана политикой — они важнее заданных здесь"});
    }
    rows.push_back({L"Токен в этой консоли", consoleToken ? L"введён (до закрытия консоли)" : L"нет"});
    rows.push_back({L"Данные", dataDir + L"\\data"});
    rows.push_back({L"Журнал службы", dataDir + L"\\logs\\service.log"});
    return rows;
}

}  // namespace console
