// События устройства для 1С (docs/protocol.md §1.4): компонента слушает поток SSE сервера
// GET /api/v1/devices/{device_id}/events и на каждое событие поднимает в 1С внешнее событие
// ExternalEvent(DeviceID, имя, данные) — требования к драйверам, раздел 3.8.
#pragma once

#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "device_client.h"
#include "http.h"

namespace emu {

// SseParser — разбор text/event-stream по кускам: поля id, event, data; пустая строка — конец события.
class SseParser {
public:
    struct Event {
        std::string id, event, data;
    };
    void feed(const std::string& chunk, std::vector<Event>& out);

private:
    void line(std::string l, std::vector<Event>& out);
    std::string buf_;
    Event cur_;
    bool any_ = false;
};

// EventListener — фоновый поток событий одного подключённого устройства. Переподключается сам,
// пропущенное дочитывает по курсору (Last-Event-ID). onEvent вызывается из фонового потока.
class EventListener {
public:
    using OnEvent = std::function<void(const std::string& name, const std::string& data)>;

    EventListener(HttpClient& http, DeviceClient& client, OnEvent onEvent);
    ~EventListener();
    EventListener(const EventListener&) = delete;
    EventListener& operator=(const EventListener&) = delete;

    void start();
    // stop закрывает соединение и ждёт поток; повторный вызов безопасен.
    void stop();

private:
    void run();
    bool sleepFor(int ms);  // false — остановлен
    bool stopping();

    HttpClient& http_;
    DeviceClient& client_;
    OnEvent onEvent_;
    std::thread thread_;
    std::mutex mu_;
    std::condition_variable cv_;
    bool stop_ = false;
    std::shared_ptr<StreamCancel> current_;
    std::string cursor_;  // id последнего полученного события
};

}  // namespace emu
