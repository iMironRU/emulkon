#include "stream.h"

#include "sse.h"

namespace tray {

namespace {

struct Url {
    std::wstring host, path;
    INTERNET_PORT port = 0;
    bool secure = false;
};

bool crack(const std::wstring& url, Url& out) {
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof uc;
    wchar_t host[256] = {}, path[4096] = {};
    uc.lpszHostName = host;
    uc.dwHostNameLength = 256;
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = 4096;
    uc.dwSchemeLength = static_cast<DWORD>(-1);
    uc.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc)) return false;
    out.host.assign(host, uc.dwHostNameLength);
    out.path.assign(path, uc.dwUrlPathLength);
    if (uc.lpszExtraInfo && uc.dwExtraInfoLength) out.path.append(uc.lpszExtraInfo, uc.dwExtraInfoLength);
    out.port = uc.nPort;
    out.secure = uc.nScheme == INTERNET_SCHEME_HTTPS;
    return true;
}

std::string winhttpError(const char* where) {
    DWORD e = GetLastError();
    switch (e) {
        case ERROR_WINHTTP_CANNOT_CONNECT: return std::string(where) + ": сервер не отвечает";
        case ERROR_WINHTTP_TIMEOUT: return std::string(where) + ": таймаут";
        case ERROR_WINHTTP_NAME_NOT_RESOLVED: return std::string(where) + ": имя сервера не найдено";
        case ERROR_WINHTTP_CONNECTION_ERROR: return std::string(where) + ": соединение разорвано";
        case ERROR_WINHTTP_OPERATION_CANCELLED: return std::string(where) + ": отменено";
        default: return std::string(where) + ": код " + std::to_string(e);
    }
}

struct Handles {
    HINTERNET session = nullptr, connect = nullptr, request = nullptr;
    ~Handles() {
        if (request) WinHttpCloseHandle(request);
        if (connect) WinHttpCloseHandle(connect);
        if (session) WinHttpCloseHandle(session);
    }
};

// openRequest открывает запрос и ждёт заголовки ответа. body — для POST (JSON).
bool openRequest(const wchar_t* verb, const std::wstring& url, const std::wstring& token, int connectMs, int receiveMs,
                 Handles& h, int& status, std::string& error, const wchar_t* accept = nullptr,
                 const std::string* body = nullptr) {
    Url u;
    if (!crack(url, u)) {
        error = "неверный адрес сервера";
        return false;
    }
    h.session = WinHttpOpen(L"Emulkon-Tray/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                            WINHTTP_NO_PROXY_BYPASS, 0);
    if (!h.session) h.session = WinHttpOpen(L"Emulkon-Tray/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                                            WINHTTP_NO_PROXY_BYPASS, 0);
    if (!h.session) {
        error = winhttpError("WinHTTP");
        return false;
    }
    WinHttpSetTimeouts(h.session, connectMs, connectMs, connectMs, receiveMs);
    h.connect = WinHttpConnect(h.session, u.host.c_str(), u.port, 0);
    if (!h.connect) {
        error = winhttpError("соединение");
        return false;
    }
    h.request = WinHttpOpenRequest(h.connect, verb, u.path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                   WINHTTP_DEFAULT_ACCEPT_TYPES, u.secure ? WINHTTP_FLAG_SECURE : 0);
    if (!h.request) {
        error = winhttpError("запрос");
        return false;
    }
    std::wstring headers;
    if (!token.empty()) headers += L"Authorization: Bearer " + token + L"\r\n";
    if (accept) headers += std::wstring(L"Accept: ") + accept + L"\r\n";
    if (body) headers += L"Content-Type: application/json; charset=utf-8\r\n";
    DWORD len = body ? static_cast<DWORD>(body->size()) : 0;
    if (!WinHttpSendRequest(h.request, headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
                            static_cast<DWORD>(-1L), body ? const_cast<char*>(body->data()) : WINHTTP_NO_REQUEST_DATA,
                            len, len, 0) ||
        !WinHttpReceiveResponse(h.request, nullptr)) {
        error = winhttpError("обмен с сервером");
        return false;
    }
    DWORD code = 0, size = sizeof code;
    WinHttpQueryHeaders(h.request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                        &code, &size, WINHTTP_NO_HEADER_INDEX);
    status = static_cast<int>(code);
    return true;
}

std::wstring trimSlash(std::wstring s) {
    while (!s.empty() && s.back() == L'/') s.pop_back();
    return s;
}

}  // namespace

std::wstring urlEncode(const std::wstring& s) {
    std::string u = narrow(s);
    std::wstring out;
    const char* hex = "0123456789ABCDEF";
    for (unsigned char c : u) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
            c == '.' || c == '~') {
            out += static_cast<wchar_t>(c);
        } else {
            out += L'%';
            out += static_cast<wchar_t>(hex[c >> 4]);
            out += static_cast<wchar_t>(hex[c & 15]);
        }
    }
    return out;
}

static void readAll(Handles& h, HttpResult& r) {
    for (;;) {
        DWORD avail = 0, read = 0;
        if (!WinHttpQueryDataAvailable(h.request, &avail) || avail == 0) break;
        std::string chunk(avail, '\0');
        if (!WinHttpReadData(h.request, &chunk[0], avail, &read)) break;
        r.body.append(chunk.data(), read);
    }
}

HttpResult httpPost(const std::wstring& url, const std::wstring& token, const std::string& json, int timeoutMs) {
    HttpResult r;
    Handles h;
    if (!openRequest(L"POST", url, token, timeoutMs, timeoutMs, h, r.status, r.error, nullptr, &json)) return r;
    readAll(h, r);
    return r;
}

HttpResult httpPut(const std::wstring& url, const std::wstring& token, const std::string& json, int timeoutMs) {
    HttpResult r;
    Handles h;
    if (!openRequest(L"PUT", url, token, timeoutMs, timeoutMs, h, r.status, r.error, nullptr, &json)) return r;
    readAll(h, r);
    return r;
}

HttpResult httpGet(const std::wstring& url, const std::wstring& token, int timeoutMs) {
    HttpResult r;
    Handles h;
    if (!openRequest(L"GET", url, token, timeoutMs, timeoutMs, h, r.status, r.error)) return r;
    for (;;) {
        DWORD avail = 0, read = 0;
        if (!WinHttpQueryDataAvailable(h.request, &avail) || avail == 0) break;
        std::string chunk(avail, '\0');
        if (!WinHttpReadData(h.request, &chunk[0], avail, &read)) break;
        r.body.append(chunk.data(), read);
    }
    return r;
}

void JournalStream::start(const std::wstring& serverUrl, const std::wstring& token, const std::wstring& workstation,
                          OnDoc onDoc, OnStatus onStatus, OnPrompt onPrompt, OnKeyboard onKeyboard) {
    stop();
    server_ = trimSlash(serverUrl);
    token_ = token;
    workstation_ = workstation;
    onDoc_ = std::move(onDoc);
    onStatus_ = std::move(onStatus);
    onPrompt_ = std::move(onPrompt);
    onKeyboard_ = std::move(onKeyboard);
    lastSeq_ = 0;
    stop_ = false;
    wake_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    thread_ = std::thread([this] { run(); });
}

void JournalStream::stop() {
    if (!thread_.joinable()) return;
    stop_ = true;
    if (wake_) SetEvent(wake_);
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (request_) WinHttpCloseHandle(request_);  // прерывает WinHttpReadData
        request_ = nullptr;
    }
    thread_.join();
    if (wake_) CloseHandle(wake_);
    wake_ = nullptr;
}

void JournalStream::run() {
    DWORD backoff = 1000;
    while (!stop_) {
        std::wstring error;
        bool hadReady = session(error);
        if (stop_) break;
        onStatus_(false, error);
        if (hadReady) backoff = 1000;
        WaitForSingleObject(wake_, backoff);
        backoff = backoff < 15000 ? backoff * 2 : 15000;
    }
}

bool JournalStream::session(std::wstring& error) {
    std::wstring url = server_ + L"/control/v1/journal/stream?method=device.print,device.reprint,device.keyboard,device.prompt,device.prompt_done";
    if (!workstation_.empty() && workstation_ != L"*") url += L"&workstation_id=" + urlEncode(workstation_);
    // Первый раз — последний документ (чтобы было что показать по клику), потом — с курсора.
    url += lastSeq_ > 0 ? L"&after_seq=" + std::to_wstring(lastSeq_) : std::wstring(L"&tail=1");

    Handles h;
    int status = 0;
    std::string err;
    // Сервер шлёт ping каждые 15 с: 45 с тишины — соединение мёртвое.
    if (!openRequest(L"GET", url, token_, 5000, 45000, h, status, err, L"text/event-stream")) {
        error = widen(err);
        return false;
    }
    if (status == 401) {
        error = L"сервер требует токен Control API (Настройки…)";
        return false;
    }
    if (status != 200) {
        error = L"сервер ответил HTTP " + std::to_wstring(status);
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (stop_) return false;
        request_ = h.request;
    }

    SseParser parser;
    bool ready = false;
    PromptEvent pending;  // открытый запрос экрана из дочитки
    bool hasPending = false;
    std::string buf;
    while (!stop_) {
        // Синхронный WinHttpReadData ждёт, пока наберётся весь буфер, — для потока
        // событий так нельзя. QueryDataAvailable ждёт любых данных, читаем ровно их.
        DWORD avail = 0, read = 0;
        if (!WinHttpQueryDataAvailable(h.request, &avail)) {
            error = widen(winhttpError("поток журнала"));
            break;
        }
        if (avail == 0) {
            error = L"сервер закрыл поток";
            break;
        }
        buf.resize(avail);
        if (!WinHttpReadData(h.request, &buf[0], avail, &read)) {
            error = widen(winhttpError("поток журнала"));
            break;
        }
        if (read == 0) {
            error = L"сервер закрыл поток";
            break;
        }
        for (const SseEvent& ev : parser.feed(buf.substr(0, read))) {
            if (ev.event == "ready") {
                ready = true;
                onStatus_(true, L"");
                // Трей запустился, пока терминал ждёт карту, — экран всё равно нужен.
                if (hasPending && onPrompt_ && secondsUntil(pending.deadline) > 0) onPrompt_(pending);
                hasPending = false;
            } else if (ev.event == "entry") {
                PrintedDoc doc;
                PromptEvent pe;
                KeyboardEvent kb;
                if (parseKeyboardEntry(ev.data, kb)) {
                    if (kb.journalSeq > lastSeq_) lastSeq_ = kb.journalSeq;
                    if (ready && onKeyboard_) onKeyboard_(kb);  // старое из дочитки не вводим
                } else if (parseJournalEntry(ev.data, doc)) {
                    if (doc.journalSeq > lastSeq_) lastSeq_ = doc.journalSeq;
                    onDoc_(doc, ready);
                } else if (parsePromptEntry(ev.data, pe)) {
                    if (pe.journalSeq > lastSeq_) lastSeq_ = pe.journalSeq;
                    if (ready) {
                        if (onPrompt_) onPrompt_(pe);
                    } else if (!pe.done) {
                        pending = pe;
                        hasPending = true;
                    } else if (hasPending && pending.promptId == pe.promptId) {
                        hasPending = false;
                    }
                }
            } else if (ev.event == "lagged") {
                error = L"отставание потока — переподключение";
            }
        }
    }
    std::lock_guard<std::mutex> lock(mu_);
    if (request_ == h.request) request_ = nullptr;
    else h.request = nullptr;  // уже закрыт из stop()
    return ready;
}

}  // namespace tray
