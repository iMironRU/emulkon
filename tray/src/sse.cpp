#include "sse.h"

namespace tray {

std::vector<SseEvent> SseParser::feed(const std::string& chunk) {
    std::vector<SseEvent> out;
    buf_ += chunk;
    size_t pos;
    while ((pos = buf_.find('\n')) != std::string::npos) {
        std::string l = buf_.substr(0, pos);
        buf_.erase(0, pos + 1);
        if (!l.empty() && l.back() == '\r') l.pop_back();
        line(l, out);
    }
    return out;
}

void SseParser::line(const std::string& l, std::vector<SseEvent>& out) {
    if (l.empty()) {  // конец события
        if (hasData_ || cur_.event != "message") out.push_back(cur_);
        cur_ = SseEvent();
        hasData_ = false;
        return;
    }
    if (l[0] == ':') return;  // комментарий (ping)
    size_t colon = l.find(':');
    std::string field = l.substr(0, colon);
    std::string value = colon == std::string::npos ? std::string() : l.substr(colon + 1);
    if (!value.empty() && value[0] == ' ') value.erase(0, 1);
    if (field == "event") {
        cur_.event = value;
    } else if (field == "data") {
        if (hasData_) cur_.data += '\n';
        cur_.data += value;
        hasData_ = true;
    } else if (field == "id") {
        cur_.id = value;
    }
}

}  // namespace tray
