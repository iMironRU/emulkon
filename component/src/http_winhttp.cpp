// HTTP через WinHTTP (Windows). Синхронный режим: 1С вызывает методы драйвера
// из своего потока, асинхронность (…Асинх) обеспечивает платформа.
#include "http.h"

#include <windows.h>
#include <winhttp.h>

#include "text.h"

namespace emu {

namespace {

std::string lastErrorText(const char* where) {
    DWORD code = GetLastError();
    std::string what;
    switch (code) {
        case ERROR_WINHTTP_TIMEOUT: what = "таймаут"; break;
        case ERROR_WINHTTP_CANNOT_CONNECT: what = "не удалось подключиться"; break;
        case ERROR_WINHTTP_CONNECTION_ERROR: what = "соединение разорвано"; break;
        case ERROR_WINHTTP_NAME_NOT_RESOLVED: what = "имя сервера не найдено"; break;
        case ERROR_WINHTTP_INVALID_URL: what = "неверный адрес"; break;
        case ERROR_WINHTTP_UNRECOGNIZED_SCHEME: what = "поддерживаются только http и https"; break;
        case ERROR_WINHTTP_SECURE_FAILURE: what = "ошибка TLS (сертификат)"; break;
        default: what = "код " + std::to_string(code);
    }
    return std::string(where) + ": " + what;
}

struct Handle {
    HINTERNET h = nullptr;
    explicit Handle(HINTERNET v = nullptr) : h(v) {}
    ~Handle() {
        if (h) WinHttpCloseHandle(h);
    }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
};

class WinHttpClient : public HttpClient {
public:
    explicit WinHttpClient(const std::string& userAgent) {
        std::u16string ua = utf8to16(userAgent);
        session_ = WinHttpOpen(reinterpret_cast<LPCWSTR>(ua.c_str()), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                               WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!session_) {
            // Windows до 8.1 не знает AUTOMATIC_PROXY.
            session_ = WinHttpOpen(reinterpret_cast<LPCWSTR>(ua.c_str()), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                   WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        }
    }

    ~WinHttpClient() override {
        if (session_) WinHttpCloseHandle(session_);
    }

    HttpResponse post(const std::string& url, const std::string& body, const Headers& headers, int timeoutMs) override {
        return send(L"POST", url, body, headers, timeoutMs);
    }

    HttpResponse get(const std::string& url, const Headers& headers, int timeoutMs) override {
        return send(L"GET", url, std::string(), headers, timeoutMs);
    }

    HttpResponse remove(const std::string& url, const Headers& headers, int timeoutMs) override {
        return send(L"DELETE", url, std::string(), headers, timeoutMs);
    }

    HttpResponse patch(const std::string& url, const std::string& body, const Headers& headers, int timeoutMs) override {
        return send(L"PATCH", url, body, headers, timeoutMs);
    }

    HttpResponse put(const std::string& url, const std::string& body, const Headers& headers, int timeoutMs) override {
        return send(L"PUT", url, body, headers, timeoutMs);
    }

    HttpResponse stream(const std::string& url, const Headers& headers, int idleTimeoutMs, StreamCancel& cancel,
                        const std::function<bool(const std::string&)>& onData) override {
        HttpResponse r;
        if (!session_) {
            r.error = "WinHTTP недоступен";
            return r;
        }
        std::u16string wurl = utf8to16(url);
        URL_COMPONENTS uc{};
        uc.dwStructSize = sizeof uc;
        wchar_t host[256] = {};
        wchar_t path[2048] = {};
        uc.lpszHostName = host;
        uc.dwHostNameLength = 256;
        uc.lpszUrlPath = path;
        uc.dwUrlPathLength = 2048;
        uc.dwSchemeLength = static_cast<DWORD>(-1);
        uc.dwExtraInfoLength = static_cast<DWORD>(-1);
        if (!WinHttpCrackUrl(reinterpret_cast<LPCWSTR>(wurl.c_str()), 0, 0, &uc)) {
            r.error = lastErrorText("адрес сервера") + " (" + url + ")";
            return r;
        }
        std::wstring object = std::wstring(path, uc.dwUrlPathLength);
        if (uc.lpszExtraInfo && uc.dwExtraInfoLength) object.append(uc.lpszExtraInfo, uc.dwExtraInfoLength);
        Handle conn(WinHttpConnect(session_, std::wstring(host, uc.dwHostNameLength).c_str(), uc.nPort, 0));
        if (!conn.h) {
            r.error = lastErrorText("соединение");
            return r;
        }
        HINTERNET req = WinHttpOpenRequest(conn.h, L"GET", object.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                           uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
        if (!req) {
            r.error = lastErrorText("запрос");
            return r;
        }
        // Закрыть запрос может и cancel() из другого потока — тогда его не закрываем повторно.
        if (!cancel.attach(req, [](void* h) { WinHttpCloseHandle(static_cast<HINTERNET>(h)); })) {
            WinHttpCloseHandle(req);
            r.error = "отменено";
            return r;
        }
        struct Release {
            StreamCancel& c;
            HINTERNET h;
            ~Release() {
                if (c.detach()) WinHttpCloseHandle(h);
            }
        } release{cancel, req};
        int idle = idleTimeoutMs > 0 ? idleTimeoutMs : 60000;
        WinHttpSetTimeouts(req, 10000, 10000, 10000, idle);

        std::string hdr = "Accept: text/event-stream\r\nCache-Control: no-cache\r\n";
        for (const auto& h : headers) hdr += h.first + ": " + h.second + "\r\n";
        std::u16string whdr = utf8to16(hdr);
        if (!WinHttpSendRequest(req, reinterpret_cast<LPCWSTR>(whdr.c_str()), static_cast<DWORD>(-1L), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
            !WinHttpReceiveResponse(req, nullptr)) {
            r.timeout = GetLastError() == ERROR_WINHTTP_TIMEOUT;
            r.error = cancel.cancelled() ? "отменено" : lastErrorText("поток событий");
            return r;
        }
        DWORD status = 0, size = sizeof status;
        WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size,
                            WINHTTP_NO_HEADER_INDEX);
        r.status = static_cast<int>(status);
        for (;;) {
            // Синхронный WinHTTP: сначала сколько доступно, затем читаем ровно столько.
            DWORD avail = 0;
            if (!WinHttpQueryDataAvailable(req, &avail)) {
                r.timeout = GetLastError() == ERROR_WINHTTP_TIMEOUT;
                r.error = cancel.cancelled() ? "отменено" : lastErrorText("поток событий");
                return r;
            }
            if (avail == 0) return r;  // сервер закрыл поток
            std::string chunk(avail, '\0');
            DWORD read = 0;
            if (!WinHttpReadData(req, &chunk[0], avail, &read)) {
                r.error = cancel.cancelled() ? "отменено" : lastErrorText("поток событий");
                return r;
            }
            chunk.resize(read);
            if (r.status != 200) {
                if (r.body.size() < 4096) r.body += chunk;
                continue;
            }
            if (!onData(chunk)) return r;
        }
    }

private:
    HttpResponse send(const wchar_t* verb, const std::string& url, const std::string& body, const Headers& headers,
                      int timeoutMs) {
        HttpResponse r;
        if (!session_) {
            r.error = "WinHTTP недоступен";
            return r;
        }
        std::u16string wurl = utf8to16(url);
        URL_COMPONENTS uc{};
        uc.dwStructSize = sizeof uc;
        wchar_t host[256] = {};
        wchar_t path[2048] = {};
        uc.lpszHostName = host;
        uc.dwHostNameLength = 256;
        uc.lpszUrlPath = path;
        uc.dwUrlPathLength = 2048;
        uc.dwSchemeLength = static_cast<DWORD>(-1);
        uc.dwExtraInfoLength = static_cast<DWORD>(-1);
        if (!WinHttpCrackUrl(reinterpret_cast<LPCWSTR>(wurl.c_str()), 0, 0, &uc)) {
            r.error = lastErrorText("адрес сервера") + " (" + url + ")";
            return r;
        }
        std::wstring object = std::wstring(path, uc.dwUrlPathLength);
        if (uc.lpszExtraInfo && uc.dwExtraInfoLength) object.append(uc.lpszExtraInfo, uc.dwExtraInfoLength);
        bool secure = uc.nScheme == INTERNET_SCHEME_HTTPS;

        int t = timeoutMs > 0 ? timeoutMs : 30000;
        WinHttpSetTimeouts(session_, t, t, t, t);

        Handle conn(WinHttpConnect(session_, std::wstring(host, uc.dwHostNameLength).c_str(), uc.nPort, 0));
        if (!conn.h) {
            r.error = lastErrorText("соединение");
            return r;
        }
        Handle req(WinHttpOpenRequest(conn.h, verb, object.c_str(), nullptr, WINHTTP_NO_REFERER,
                                      WINHTTP_DEFAULT_ACCEPT_TYPES, secure ? WINHTTP_FLAG_SECURE : 0));
        if (!req.h) {
            r.error = lastErrorText("запрос");
            return r;
        }
        WinHttpSetTimeouts(req.h, t, t, t, t);

        std::string hdr = body.empty() ? std::string() : "Content-Type: application/json; charset=utf-8\r\n";
        for (const auto& h : headers) hdr += h.first + ": " + h.second + "\r\n";
        std::u16string whdr = utf8to16(hdr);

        if (!WinHttpSendRequest(req.h, whdr.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : reinterpret_cast<LPCWSTR>(whdr.c_str()),
                                whdr.empty() ? 0 : static_cast<DWORD>(-1L),
                                body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(body.data()), static_cast<DWORD>(body.size()),
                                static_cast<DWORD>(body.size()), 0) ||
            !WinHttpReceiveResponse(req.h, nullptr)) {
            r.timeout = GetLastError() == ERROR_WINHTTP_TIMEOUT;
            r.error = lastErrorText("обмен с сервером");
            return r;
        }

        DWORD status = 0, size = sizeof status;
        WinHttpQueryHeaders(req.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                            &status, &size, WINHTTP_NO_HEADER_INDEX);
        r.status = static_cast<int>(status);

        for (;;) {
            DWORD avail = 0;
            if (!WinHttpQueryDataAvailable(req.h, &avail)) {
                r.timeout = GetLastError() == ERROR_WINHTTP_TIMEOUT;
                r.error = lastErrorText("чтение ответа");
                return r;
            }
            if (avail == 0) break;
            std::string chunk(avail, '\0');
            DWORD read = 0;
            if (!WinHttpReadData(req.h, &chunk[0], avail, &read)) {
                r.error = lastErrorText("чтение ответа");
                return r;
            }
            chunk.resize(read);
            r.body += chunk;
            if (r.body.size() > (16u << 20)) {
                r.error = "ответ сервера больше 16 МБ";
                return r;
            }
        }
        return r;
    }

    HINTERNET session_ = nullptr;
};

}  // namespace

std::unique_ptr<HttpClient> makeHttpClient(const std::string& userAgent) {
    return std::unique_ptr<HttpClient>(new WinHttpClient(userAgent));
}

}  // namespace emu
