#include "events.h"

#include <algorithm>
#include <chrono>

#include "json.h"

namespace emu {

void SseParser::feed(const std::string& chunk, std::vector<Event>& out) {
    buf_ += chunk;
    size_t start = 0;
    for (;;) {
        size_t nl = buf_.find('\n', start);
        if (nl == std::string::npos) break;
        std::string l = buf_.substr(start, nl - start);
        if (!l.empty() && l.back() == '\r') l.pop_back();
        line(std::move(l), out);
        start = nl + 1;
    }
    buf_.erase(0, start);
}

void SseParser::line(std::string l, std::vector<Event>& out) {
    if (l.empty()) {
        if (any_) out.push_back(cur_);
        cur_ = Event();
        any_ = false;
        return;
    }
    if (l[0] == ':') return;  // комментарий (пинг)
    size_t colon = l.find(':');
    std::string field = l.substr(0, colon);
    std::string value = colon == std::string::npos ? std::string() : l.substr(colon + 1);
    if (!value.empty() && value[0] == ' ') value.erase(0, 1);
    if (field == "id") {
        cur_.id = value;
    } else if (field == "event") {
        cur_.event = value;
    } else if (field == "data") {
        if (!cur_.data.empty()) cur_.data += '\n';
        cur_.data += value;
    } else {
        return;
    }
    any_ = true;
}

EventListener::EventListener(HttpClient& http, DeviceClient& client, OnEvent onEvent)
    : http_(http), client_(client), onEvent_(std::move(onEvent)) {}

EventListener::~EventListener() { stop(); }

void EventListener::start() {
    std::lock_guard<std::mutex> lock(mu_);
    if (thread_.joinable() || stop_) return;
    thread_ = std::thread([this] { run(); });
}

void EventListener::stop() {
    {
        std::lock_guard<std::mutex> lock(mu_);
        stop_ = true;
        if (current_) current_->cancel();
    }
    cv_.notify_all();
    if (thread_.joinable() && thread_.get_id() != std::this_thread::get_id()) thread_.join();
}

bool EventListener::stopping() {
    std::lock_guard<std::mutex> lock(mu_);
    return stop_;
}

bool EventListener::sleepFor(int ms) {
    std::unique_lock<std::mutex> lock(mu_);
    return !cv_.wait_for(lock, std::chrono::milliseconds(ms), [this] { return stop_; });
}

void EventListener::run() {
    const std::string base = client_.endpoint().serverUrl;
    int backoff = 1000;
    while (!stopping()) {
        std::string session = client_.sessionId();
        if (session.empty()) {
            std::string message;
            if (!client_.reopen(session, message)) {
                if (!sleepFor(backoff)) break;
                backoff = std::min(backoff * 2, 10000);
                continue;
            }
            session = client_.sessionId();
        }
        auto cancel = std::make_shared<StreamCancel>();
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (stop_) break;
            current_ = cancel;
        }
        Headers headers{{"X-Emulkon-Session", session}};
        if (!cursor_.empty()) headers.push_back({"Last-Event-ID", cursor_});
        SseParser parser;
        std::vector<SseParser::Event> events;
        // Сервер пингует раз в 15 с: тишина дольше 45 с — соединение потеряно.
        HttpResponse r = http_.stream(base + "/api/v1/devices/" + client_.deviceId() + "/events", headers, 45000, *cancel,
                                      [&](const std::string& chunk) {
                                          events.clear();
                                          parser.feed(chunk, events);
                                          for (const auto& e : events) {
                                              if (e.event == "ready") {
                                                  backoff = 1000;
                                                  continue;
                                              }
                                              if (e.event != "device") continue;
                                              Json j;
                                              std::string err;
                                              if (!parseJson(e.data, j, err)) continue;
                                              const Json* name = j.get("name");
                                              const Json* data = j.get("data");
                                              if (!name || name->text.empty()) continue;
                                              if (!e.id.empty()) cursor_ = e.id;
                                              onEvent_(name->text, data ? data->text : std::string());
                                          }
                                          return true;
                                      });
        {
            std::lock_guard<std::mutex> lock(mu_);
            current_.reset();
        }
        if (stopping()) break;
        if (r.status == 404) break;  // сервер без потока событий: не долбим его
        if (r.status == 401) {       // сессия просрочена или сервер перезапущен
            std::string message;
            client_.reopen(session, message);
        }
        if (!sleepFor(backoff)) break;
        backoff = std::min(backoff * 2, 10000);
    }
}

}  // namespace emu
