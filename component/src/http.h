// HTTP-транспорт компоненты. Windows — WinHTTP (системная библиотека, без зависимостей).
// Linux — TODO (libcurl, ARCHITECTURE §4.1).
#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace emu {

struct HttpResponse {
    int status = 0;     // 0 — ответа нет (см. error)
    std::string body;   // тело ответа (UTF-8)
    std::string error;  // непусто — сбой транспорта: нет связи, таймаут, неверный адрес
    bool timeout = false;
};

using Headers = std::vector<std::pair<std::string, std::string>>;

// StreamCancel — отмена длинного запроса (поток событий) из другого потока: закрывает
// соединение, и заблокированное чтение сразу возвращается.
class StreamCancel {
public:
    void cancel() {
        std::lock_guard<std::mutex> lock(mu_);
        cancelled_ = true;
        if (handle_ && close_) close_(handle_);
        handle_ = nullptr;
    }
    bool cancelled() {
        std::lock_guard<std::mutex> lock(mu_);
        return cancelled_;
    }
    // Для транспорта: привязать открытый запрос. false — отмена уже была, запрос не нужен.
    bool attach(void* handle, void (*close)(void*)) {
        std::lock_guard<std::mutex> lock(mu_);
        if (cancelled_) return false;
        handle_ = handle;
        close_ = close;
        return true;
    }
    // Отвязать запрос. true — он всё ещё открыт и закрыть его должен сам транспорт.
    bool detach() {
        std::lock_guard<std::mutex> lock(mu_);
        bool mine = handle_ != nullptr;
        handle_ = nullptr;
        return mine;
    }

private:
    std::mutex mu_;
    void* handle_ = nullptr;
    void (*close_)(void*) = nullptr;
    bool cancelled_ = false;
};

class HttpClient {
public:
    virtual ~HttpClient() = default;
    // stream — длинный GET (SSE): тело отдаётся кусками в onData, пока сервер не закроет поток,
    // onData не вернёт false, не будет отмены или паузы в данных дольше idleTimeoutMs.
    // status ответа — в HttpResponse; тело при статусе не 200 — в body.
    virtual HttpResponse stream(const std::string& url, const Headers& headers, int idleTimeoutMs, StreamCancel& cancel,
                                const std::function<bool(const std::string&)>& onData) = 0;
    // POST JSON. timeoutMs — общий предел на соединение, отправку и ожидание ответа.
    virtual HttpResponse post(const std::string& url, const std::string& body, const Headers& headers, int timeoutMs) = 0;
    // GET — проверка и скачивание обновления драйвера. Тело ответа — как есть (может быть двоичным).
    virtual HttpResponse get(const std::string& url, const Headers& headers, int timeoutMs) = 0;
    // DELETE — закрыть сессию на сервере. По умолчанию ничего не делает (подмены в тестах).
    virtual HttpResponse remove(const std::string&, const Headers&, int) { return {}; }
    // PATCH и PUT JSON — консоль управления (подписи, сценарии). По умолчанию ничего не делают.
    virtual HttpResponse patch(const std::string&, const std::string&, const Headers&, int) { return {}; }
    virtual HttpResponse put(const std::string&, const std::string&, const Headers&, int) { return {}; }
};

std::unique_ptr<HttpClient> makeHttpClient(const std::string& userAgent);

}  // namespace emu
