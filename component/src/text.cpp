#include "text.h"

#include <cstdint>
#include <cstdlib>
#include <vector>

namespace emu {

std::u16string utf8to16(const std::string& s) {
    std::u16string out;
    out.reserve(s.size());
    size_t i = 0;
    while (i < s.size()) {
        uint32_t cp;
        unsigned char c = static_cast<unsigned char>(s[i]);
        int extra;
        if (c < 0x80) { cp = c; extra = 0; }
        else if ((c >> 5) == 0x6) { cp = c & 0x1F; extra = 1; }
        else if ((c >> 4) == 0xE) { cp = c & 0x0F; extra = 2; }
        else if ((c >> 3) == 0x1E) { cp = c & 0x07; extra = 3; }
        else { out += u'�'; ++i; continue; }
        bool bad = false;
        for (int k = 1; k <= extra; ++k) {
            if (i + k >= s.size()) { bad = true; break; }
            unsigned char cc = static_cast<unsigned char>(s[i + k]);
            if ((cc >> 6) != 0x2) { bad = true; break; }
            cp = (cp << 6) | (cc & 0x3F);
        }
        if (bad) { out += u'�'; ++i; continue; }
        i += extra + 1;
        if (cp >= 0x10000) {
            cp -= 0x10000;
            out += static_cast<char16_t>(0xD800 + (cp >> 10));
            out += static_cast<char16_t>(0xDC00 + (cp & 0x3FF));
        } else {
            out += static_cast<char16_t>(cp);
        }
    }
    return out;
}

std::string utf16to8(const std::u16string& s) {
    std::string out;
    out.reserve(s.size() * 2);
    for (size_t i = 0; i < s.size(); ++i) {
        uint32_t cp = s[i];
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < s.size() && s[i + 1] >= 0xDC00 && s[i + 1] <= 0xDFFF) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (s[i + 1] - 0xDC00);
            ++i;
        }
        if (cp < 0x80) {
            out += static_cast<char>(cp);
        } else if (cp < 0x800) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }
    return out;
}

std::string utf16to8(const char16_t* s) {
    if (!s) return {};
    return utf16to8(std::u16string(s));
}

static char16_t upper(char16_t c) {
    if (c >= u'a' && c <= u'z') return static_cast<char16_t>(c - 32);
    if (c >= 0x0430 && c <= 0x044F) return static_cast<char16_t>(c - 0x20);  // а-я
    if (c >= 0x0450 && c <= 0x045F) return static_cast<char16_t>(c - 0x50);  // ѐ-џ, в т.ч. ё
    return c;
}

bool equalsIgnoreCase(const std::u16string& a, const char16_t* b) {
    size_t i = 0;
    for (; i < a.size(); ++i) {
        if (b[i] == 0 || upper(a[i]) != upper(b[i])) return false;
    }
    return b[i] == 0;
}

std::string xmlAttr(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            case '\n': out += "&#10;"; break;
            case '\r': out += "&#13;"; break;
            case '\t': out += "&#9;"; break;
            default: out += c;
        }
    }
    return out;
}

static void appendUtf8(std::string& out, uint32_t cp) {
    std::u16string tmp;
    if (cp >= 0x10000) {
        cp -= 0x10000;
        tmp += static_cast<char16_t>(0xD800 + (cp >> 10));
        tmp += static_cast<char16_t>(0xDC00 + (cp & 0x3FF));
    } else {
        tmp += static_cast<char16_t>(cp);
    }
    out += utf16to8(tmp);
}

static std::string xmlUnescape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '&') {
            out += s[i];
            continue;
        }
        size_t semi = s.find(';', i);
        if (semi == std::string::npos || semi - i > 10) {
            out += s[i];
            continue;
        }
        std::string ent = s.substr(i + 1, semi - i - 1);
        if (ent == "amp") out += '&';
        else if (ent == "lt") out += '<';
        else if (ent == "gt") out += '>';
        else if (ent == "quot") out += '"';
        else if (ent == "apos") out += '\'';
        else if (!ent.empty() && ent[0] == '#') {
            uint32_t cp = (ent.size() > 1 && (ent[1] == 'x' || ent[1] == 'X'))
                              ? static_cast<uint32_t>(std::strtoul(ent.c_str() + 2, nullptr, 16))
                              : static_cast<uint32_t>(std::strtoul(ent.c_str() + 1, nullptr, 10));
            appendUtf8(out, cp);
        } else {
            out += s.substr(i, semi - i + 1);  // неизвестная сущность — как есть
        }
        i = semi;
    }
    return out;
}

// attr ищет значение атрибута name внутри тега tag (одинарные или двойные кавычки).
static bool attr(const std::string& tag, const std::string& name, std::string& value) {
    size_t pos = 0;
    while ((pos = tag.find(name, pos)) != std::string::npos) {
        bool startOk = pos > 0 && (tag[pos - 1] == ' ' || tag[pos - 1] == '\t' || tag[pos - 1] == '\n' || tag[pos - 1] == '\r');
        size_t p = pos + name.size();
        while (p < tag.size() && (tag[p] == ' ' || tag[p] == '\t')) ++p;
        if (startOk && p < tag.size() && tag[p] == '=') {
            ++p;
            while (p < tag.size() && (tag[p] == ' ' || tag[p] == '\t')) ++p;
            if (p < tag.size() && (tag[p] == '"' || tag[p] == '\'')) {
                char q = tag[p];
                size_t end = tag.find(q, p + 1);
                if (end == std::string::npos) return false;
                value = xmlUnescape(tag.substr(p + 1, end - p - 1));
                return true;
            }
        }
        pos = p;
    }
    return false;
}

std::map<std::string, std::string> parseConnectionParameters(const std::string& xml) {
    std::map<std::string, std::string> out;
    size_t pos = 0;
    while ((pos = xml.find("<Parameter", pos)) != std::string::npos) {
        size_t after = pos + 10;
        // <Parameters> — не наш тег.
        if (after < xml.size() && xml[after] != ' ' && xml[after] != '\t' && xml[after] != '\n' && xml[after] != '\r' && xml[after] != '/') {
            pos = after;
            continue;
        }
        size_t end = xml.find('>', after);
        if (end == std::string::npos) break;
        std::string tag = xml.substr(pos, end - pos);
        std::string name, value;
        if (attr(tag, "Name", name)) {
            attr(tag, "Value", value);
            out[name] = value;
        }
        pos = end;
    }
    return out;
}

namespace {

// Аргументы как их разбирает 1С: пробелы вне кавычек разделяют, кавычки снимаются, "" внутри
// кавычек — сама кавычка.
std::vector<std::string> splitCommandLine(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    bool quoted = false, any = false;
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (c == '"') {
            if (quoted && i + 1 < s.size() && s[i + 1] == '"') {
                cur += '"';
                ++i;
            } else {
                quoted = !quoted;
            }
            any = true;
        } else if (!quoted && (c == ' ' || c == '\t')) {
            if (any) out.push_back(cur);
            cur.clear();
            any = false;
        } else {
            cur += c;
            any = true;
        }
    }
    if (any) out.push_back(cur);
    return out;
}

std::string upper(std::string s) {
    for (auto& c : s) c = static_cast<char>(c >= 'a' && c <= 'z' ? c - 'a' + 'A' : c);
    return s;
}

// Строка соединения 1С: File="…"; или Srvr="…";Ref="…"; (кавычки внутри уже сняты разбором).
std::string fromConnectionString(const std::string& cs) {
    std::string file, srvr, ref;
    size_t pos = 0;
    while (pos < cs.size()) {
        size_t end = cs.find(';', pos);
        std::string part = cs.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
        size_t eq = part.find('=');
        if (eq != std::string::npos) {
            std::string key = upper(part.substr(0, eq)), value = part.substr(eq + 1);
            if (value.size() >= 2 && value.front() == '"' && value.back() == '"') value = value.substr(1, value.size() - 2);
            if (key == "FILE") file = value;
            else if (key == "SRVR") srvr = value;
            else if (key == "REF") ref = value;
        }
        if (end == std::string::npos) break;
        pos = end + 1;
    }
    if (!file.empty()) return file;
    if (!srvr.empty() && !ref.empty()) return srvr + "/" + ref;
    return std::string();
}

}  // namespace

std::string infobaseFromCommandLine(const std::string& commandLine) {
    std::vector<std::string> args = splitCommandLine(commandLine);
    for (size_t i = 0; i < args.size(); ++i) {
        const std::string& a = args[i];
        if (a.empty() || a[0] != '/') continue;
        std::string key = upper(a);
        // Значение — в том же аргументе (/F"путь") или в следующем (/F "путь").
        auto value = [&](size_t keyLen) {
            std::string v = a.substr(keyLen);
            if (v.empty() && i + 1 < args.size() && !args[i + 1].empty() && args[i + 1][0] != '/') v = args[i + 1];
            return v;
        };
        if (key.rfind("/IBCONNECTIONSTRING", 0) == 0) {
            std::string v = fromConnectionString(value(19));
            if (!v.empty()) return v;
        } else if (key.rfind("/IBNAME", 0) == 0) {
            std::string v = value(7);
            if (!v.empty()) return v;
        } else if (key.rfind("/F", 0) == 0) {
            // У /F — путь: отличаем от других ключей на F по «:» или «\».
            std::string v = value(2);
            if (v.find_first_of(":\\") != std::string::npos) return v;
        } else if (key.rfind("/S", 0) == 0) {
            // /S"сервер\база"; ключи вроде /SAOnRestart разделителя не содержат.
            std::string v = value(2);
            size_t sep = v.find_first_of("\\/");
            if (sep != std::string::npos && sep > 0 && sep + 1 < v.size()) return v.substr(0, sep) + "/" + v.substr(sep + 1);
        }
    }
    return std::string();
}

}  // namespace emu
