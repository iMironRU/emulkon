// HTTP на POSIX-сокетах (Linux): без внешних зависимостей — на кассе не нужен libcurl нужной версии.
// Только http:// — сервер эмуляции в локальной сети отдаёт HTTP; https — через обратный прокси с
// http-адресом для касс. HTTP/1.1, «Connection: close»: одно соединение на запрос, тело — по
// Content-Length, chunked или до закрытия. Синхронно, как WinHTTP-клиент: асинхронность — у платформы.
#include "http.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace emu {

namespace {

using Clock = std::chrono::steady_clock;

struct Url {
    std::string host;
    std::string port = "80";
    std::string target = "/";  // путь и запрос
    std::string hostHeader;
};

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
    return s;
}

// parseUrl — http://host[:port][/path][?query]; IPv6 — в квадратных скобках.
bool parseUrl(const std::string& url, Url& u, std::string& err) {
    const std::string scheme = lower(url.substr(0, url.find("://")));
    if (url.find("://") == std::string::npos) {
        err = "неверный адрес (" + url + ")";
        return false;
    }
    if (scheme == "https") {
        err = "https на Linux не поддерживается: укажите http-адрес сервера эмуляции (" + url + ")";
        return false;
    }
    if (scheme != "http") {
        err = "поддерживается только http (" + url + ")";
        return false;
    }
    std::string rest = url.substr(url.find("://") + 3);
    size_t slash = rest.find_first_of("/?");
    std::string authority = slash == std::string::npos ? rest : rest.substr(0, slash);
    if (slash != std::string::npos) u.target = rest[slash] == '/' ? rest.substr(slash) : "/" + rest.substr(slash);
    if (authority.empty()) {
        err = "неверный адрес (" + url + ")";
        return false;
    }
    u.hostHeader = authority;
    if (authority[0] == '[') {
        size_t close = authority.find(']');
        if (close == std::string::npos) {
            err = "неверный адрес (" + url + ")";
            return false;
        }
        u.host = authority.substr(1, close - 1);
        if (close + 1 < authority.size() && authority[close + 1] == ':') u.port = authority.substr(close + 2);
    } else {
        size_t colon = authority.rfind(':');
        u.host = colon == std::string::npos ? authority : authority.substr(0, colon);
        if (colon != std::string::npos) u.port = authority.substr(colon + 1);
    }
    if (u.host.empty() || u.port.empty()) {
        err = "неверный адрес (" + url + ")";
        return false;
    }
    return true;
}

int msLeft(Clock::time_point deadline) {
    auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
    return left > 0 ? static_cast<int>(left) : 0;
}

// waitFd — сокет готов к чтению или записи; false — таймаут (timeout=true) или ошибка.
bool waitFd(int fd, short events, int timeoutMs, bool& timeout) {
    pollfd p{fd, events, 0};
    for (;;) {
        int n = poll(&p, 1, timeoutMs);
        if (n < 0 && errno == EINTR) continue;
        if (n == 0) timeout = true;
        return n > 0;
    }
}

// connectTo — TCP-соединение с таймаутом по всем адресам имени.
int connectTo(const Url& u, int timeoutMs, HttpResponse& r) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* list = nullptr;
    int rc = getaddrinfo(u.host.c_str(), u.port.c_str(), &hints, &list);
    if (rc != 0) {
        r.error = "соединение: имя сервера не найдено (" + u.host + ")";
        return -1;
    }
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeoutMs);
    int fd = -1;
    bool timedOut = false;
    for (addrinfo* a = list; a && fd < 0; a = a->ai_next) {
        int s = socket(a->ai_family, a->ai_socktype | SOCK_CLOEXEC, a->ai_protocol);
        if (s < 0) continue;
        fcntl(s, F_SETFL, fcntl(s, F_GETFL) | O_NONBLOCK);
        if (connect(s, a->ai_addr, a->ai_addrlen) == 0) {
            fd = s;
            break;
        }
        if (errno == EINPROGRESS) {
            bool to = false;
            if (waitFd(s, POLLOUT, msLeft(deadline), to)) {
                int soErr = 0;
                socklen_t len = sizeof soErr;
                if (getsockopt(s, SOL_SOCKET, SO_ERROR, &soErr, &len) == 0 && soErr == 0) {
                    fd = s;
                    break;
                }
            }
            timedOut = timedOut || to;
        }
        close(s);
    }
    freeaddrinfo(list);
    if (fd < 0) {
        r.timeout = timedOut;
        r.error = timedOut ? "соединение: таймаут" : "соединение: не удалось подключиться к " + u.hostHeader;
        return -1;
    }
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    return fd;
}

bool sendAll(int fd, const std::string& data, Clock::time_point deadline, HttpResponse& r) {
    size_t done = 0;
    while (done < data.size()) {
        ssize_t n = send(fd, data.data() + done, data.size() - done, MSG_NOSIGNAL);
        if (n > 0) {
            done += static_cast<size_t>(n);
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            bool to = false;
            if (waitFd(fd, POLLOUT, msLeft(deadline), to)) continue;
            r.timeout = to;
            r.error = to ? "обмен с сервером: таймаут" : "обмен с сервером: соединение разорвано";
            return false;
        }
        r.error = "обмен с сервером: соединение разорвано";
        return false;
    }
    return true;
}

// Ответ разбирается по мере поступления: заголовки, затем тело (Content-Length, chunked или до закрытия).
class ResponseParser {
public:
    // feed — очередные байты; тело отдаётся в onBody. false — ответ не по протоколу (err).
    bool feed(const char* p, size_t n, const std::function<bool(const std::string&)>& onBody, std::string& err) {
        buf_.append(p, n);
        if (!headersDone_) {
            size_t end = buf_.find("\r\n\r\n");
            if (end == std::string::npos) {
                if (buf_.size() > 65536) {
                    err = "заголовки ответа больше 64 КБ";
                    return false;
                }
                return true;
            }
            if (!parseHeaders(buf_.substr(0, end), err)) return false;
            buf_.erase(0, end + 4);
            headersDone_ = true;
        }
        return body(onBody, err);
    }

    // finish — соединение закрыто: ответ полон?
    bool finish(std::string& err) const {
        if (!headersDone_) {
            err = "сервер закрыл соединение, не ответив";
            return false;
        }
        if (chunked_ ? !chunkedDone_ : (length_ >= 0 && received_ < length_)) {
            err = "ответ оборван";
            return false;
        }
        return true;
    }

    bool complete() const { return headersDone_ && (chunked_ ? chunkedDone_ : (length_ >= 0 && received_ >= length_)); }
    int status() const { return status_; }
    bool stopped() const { return stopped_; }

private:
    bool parseHeaders(const std::string& head, std::string& err) {
        size_t lineEnd = head.find("\r\n");
        std::string line = head.substr(0, lineEnd);
        if (line.compare(0, 5, "HTTP/") != 0 || line.size() < 12) {
            err = "ответ сервера — не HTTP";
            return false;
        }
        status_ = std::atoi(line.c_str() + line.find(' ') + 1);
        size_t pos = lineEnd == std::string::npos ? head.size() : lineEnd + 2;
        while (pos < head.size()) {
            size_t e = head.find("\r\n", pos);
            if (e == std::string::npos) e = head.size();
            std::string h = head.substr(pos, e - pos);
            size_t colon = h.find(':');
            if (colon != std::string::npos) {
                std::string name = lower(h.substr(0, colon));
                std::string value = h.substr(colon + 1);
                while (!value.empty() && (value[0] == ' ' || value[0] == '\t')) value.erase(0, 1);
                if (name == "content-length") length_ = std::atoll(value.c_str());
                if (name == "transfer-encoding" && lower(value).find("chunked") != std::string::npos) chunked_ = true;
            }
            pos = e + 2;
        }
        if (status_ == 204 || status_ == 304) length_ = 0;
        return true;
    }

    bool deliver(std::string data, const std::function<bool(const std::string&)>& onBody) {
        received_ += static_cast<int64_t>(data.size());
        if (!data.empty() && !stopped_ && !onBody(data)) stopped_ = true;
        return true;
    }

    bool body(const std::function<bool(const std::string&)>& onBody, std::string& err) {
        if (!chunked_) {
            if (length_ >= 0 && received_ + static_cast<int64_t>(buf_.size()) > length_) buf_.resize(static_cast<size_t>(length_ - received_));
            std::string data;
            data.swap(buf_);
            return deliver(std::move(data), onBody);
        }
        while (!chunkedDone_) {
            if (chunkLeft_ == 0) {
                size_t e = buf_.find("\r\n");
                if (e == std::string::npos) return true;
                std::string sizeLine = buf_.substr(0, e);
                buf_.erase(0, e + 2);
                if (sizeLine.empty()) continue;  // CRLF после предыдущего куска
                char* endp = nullptr;
                long long size = std::strtoll(sizeLine.c_str(), &endp, 16);
                if (endp == sizeLine.c_str() || size < 0) {
                    err = "неверный размер куска в ответе";
                    return false;
                }
                if (size == 0) {
                    chunkedDone_ = true;
                    return true;
                }
                chunkLeft_ = size;
            }
            if (buf_.empty()) return true;
            size_t take = static_cast<size_t>(std::min<long long>(chunkLeft_, static_cast<long long>(buf_.size())));
            std::string data = buf_.substr(0, take);
            buf_.erase(0, take);
            chunkLeft_ -= static_cast<long long>(take);
            deliver(std::move(data), onBody);
        }
        return true;
    }

    std::string buf_;
    bool headersDone_ = false;
    int status_ = 0;
    int64_t length_ = -1;  // -1 — до закрытия соединения
    int64_t received_ = 0;
    bool chunked_ = false;
    bool chunkedDone_ = false;
    long long chunkLeft_ = 0;
    bool stopped_ = false;
};

void shutdownSocket(void* h) { shutdown(static_cast<int>(reinterpret_cast<intptr_t>(h)), SHUT_RDWR); }

class PosixHttpClient : public HttpClient {
public:
    explicit PosixHttpClient(std::string userAgent) : userAgent_(std::move(userAgent)) {}

    HttpResponse post(const std::string& url, const std::string& body, const Headers& headers, int timeoutMs) override {
        return request("POST", url, body, headers, timeoutMs);
    }
    HttpResponse get(const std::string& url, const Headers& headers, int timeoutMs) override {
        return request("GET", url, std::string(), headers, timeoutMs);
    }
    HttpResponse remove(const std::string& url, const Headers& headers, int timeoutMs) override {
        return request("DELETE", url, std::string(), headers, timeoutMs);
    }
    HttpResponse patch(const std::string& url, const std::string& body, const Headers& headers, int timeoutMs) override {
        return request("PATCH", url, body, headers, timeoutMs);
    }
    HttpResponse put(const std::string& url, const std::string& body, const Headers& headers, int timeoutMs) override {
        return request("PUT", url, body, headers, timeoutMs);
    }

    HttpResponse stream(const std::string& url, const Headers& headers, int idleTimeoutMs, StreamCancel& cancel,
                        const std::function<bool(const std::string&)>& onData) override {
        HttpResponse r;
        Url u;
        if (!parseUrl(url, u, r.error)) return r;
        int fd = connectTo(u, 10000, r);
        if (fd < 0) return r;
        // Закрыть соединение может и cancel() из другого потока: он только будит чтение (shutdown),
        // закрывает сокет этот поток.
        if (!cancel.attach(reinterpret_cast<void*>(static_cast<intptr_t>(fd)), shutdownSocket)) {
            close(fd);
            r.error = "отменено";
            return r;
        }
        Headers all = headers;
        all.emplace_back("Accept", "text/event-stream");
        all.emplace_back("Cache-Control", "no-cache");
        exchange(fd, "GET", u, std::string(), all, 10000, idleTimeoutMs > 0 ? idleTimeoutMs : 60000, r, onData, &cancel);
        cancel.detach();
        close(fd);
        return r;
    }

private:
    HttpResponse request(const char* verb, const std::string& url, const std::string& body, const Headers& headers,
                         int timeoutMs) {
        HttpResponse r;
        Url u;
        if (!parseUrl(url, u, r.error)) return r;
        const int t = timeoutMs > 0 ? timeoutMs : 30000;
        int fd = connectTo(u, t, r);
        if (fd < 0) return r;
        exchange(fd, verb, u, body, headers, t, t, r,
                 [&r](const std::string& chunk) {
                     r.body += chunk;
                     return r.body.size() <= (16u << 20);
                 },
                 nullptr);
        if (r.error.empty() && r.body.size() > (16u << 20)) r.error = "ответ сервера больше 16 МБ";
        close(fd);
        return r;
    }

    // exchange — запрос и ответ по открытому соединению. Для потока (cancel) тело ошибки (не 200) копится
    // в r.body, а данные 200 идут в onData; для обычного запроса onData копит тело.
    void exchange(int fd, const char* verb, const Url& u, const std::string& body, const Headers& headers, int sendTimeoutMs,
                  int idleTimeoutMs, HttpResponse& r, const std::function<bool(const std::string&)>& onData,
                  StreamCancel* cancel) {
        std::string req = std::string(verb) + " " + u.target + " HTTP/1.1\r\nHost: " + u.hostHeader +
                          "\r\nUser-Agent: " + userAgent_ + "\r\nConnection: close\r\nAccept-Encoding: identity\r\n";
        const bool hasBody = !body.empty() || std::strcmp(verb, "POST") == 0 || std::strcmp(verb, "PUT") == 0 ||
                             std::strcmp(verb, "PATCH") == 0;
        if (!body.empty()) req += "Content-Type: application/json; charset=utf-8\r\n";
        if (hasBody) req += "Content-Length: " + std::to_string(body.size()) + "\r\n";
        for (const auto& h : headers) req += h.first + ": " + h.second + "\r\n";
        req += "\r\n";
        req += body;
        if (!sendAll(fd, req, Clock::now() + std::chrono::milliseconds(sendTimeoutMs), r)) return;

        ResponseParser parser;
        auto sink = [&](const std::string& chunk) {
            if (cancel && parser.status() != 200) {
                if (r.body.size() < 4096) r.body += chunk;
                return true;
            }
            return onData(chunk);
        };
        char buf[16384];
        for (;;) {
            bool to = false;
            if (!waitFd(fd, POLLIN, idleTimeoutMs, to)) {
                r.timeout = to;
                r.error = cancel && cancel->cancelled() ? "отменено"
                          : to                          ? "чтение ответа: таймаут"
                                                        : "чтение ответа: соединение разорвано";
                r.status = parser.status();
                return;
            }
            ssize_t n = recv(fd, buf, sizeof buf, 0);
            if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
            if (n < 0) {
                r.error = cancel && cancel->cancelled() ? "отменено" : "чтение ответа: соединение разорвано";
                r.status = parser.status();
                return;
            }
            if (n == 0) {
                std::string err;
                r.status = parser.status();
                if (cancel && cancel->cancelled()) {
                    r.error = "отменено";
                } else if (!parser.finish(err)) {
                    r.error = "чтение ответа: " + err;
                }
                return;
            }
            std::string err;
            if (!parser.feed(buf, static_cast<size_t>(n), sink, err)) {
                r.error = "чтение ответа: " + err;
                r.status = parser.status();
                return;
            }
            r.status = parser.status();
            if (parser.stopped() || parser.complete()) return;
        }
    }

    std::string userAgent_;
};

}  // namespace

std::unique_ptr<HttpClient> makeHttpClient(const std::string& userAgent) {
    return std::unique_ptr<HttpClient>(new PosixHttpClient(userAgent));
}

}  // namespace emu
