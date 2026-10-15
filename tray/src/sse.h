// Разбор потока Server-Sent Events (журнал сервера: /control/v1/journal/stream).
#pragma once

#include <string>
#include <vector>

namespace tray {

struct SseEvent {
    std::string event = "message";
    std::string data;
    std::string id;
};

class SseParser {
public:
    // feed принимает очередной кусок потока, возвращает завершённые события.
    std::vector<SseEvent> feed(const std::string& chunk);

private:
    void line(const std::string& l, std::vector<SseEvent>& out);

    std::string buf_;
    SseEvent cur_;
    bool hasData_ = false;
};

}  // namespace tray
