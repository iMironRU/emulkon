#include "json.h"

#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace emu {

namespace {

// Числа JSON — всегда с точкой, независимо от локали процесса: клиент 1С под Linux включает локаль системы
// (ru_RU — десятичная запятая), и strtod/printf("%g") там читали «0.532» как 0 и писали «0,532».
double parseNumber(const std::string& s) {
    double v = 0;
    std::from_chars(s.data(), s.data() + s.size(), v);
    return v;
}

}  // namespace

Json Json::boolean(bool v) {
    Json j;
    j.type = Type::Bool;
    j.b = v;
    return j;
}

Json Json::number(int64_t v) {
    Json j;
    j.type = Type::Number;
    j.text = std::to_string(v);
    return j;
}

Json Json::number(double v) {
    Json j;
    j.type = Type::Number;
    if (!std::isfinite(v)) {
        j.type = Type::Null;  // в JSON нет NaN/Inf
        return j;
    }
    // Кратчайшая запись, которая читается обратно в то же число; без локали.
    char buf[64];
    auto r = std::to_chars(buf, buf + sizeof buf, v);
    j.text.assign(buf, r.ptr);
    return j;
}

Json Json::numberRaw(std::string raw) {
    Json j;
    j.type = Type::Number;
    j.text = std::move(raw);
    return j;
}

Json Json::string(std::string v) {
    Json j;
    j.type = Type::String;
    j.text = std::move(v);
    return j;
}

Json Json::array() {
    Json j;
    j.type = Type::Array;
    return j;
}

Json Json::object() {
    Json j;
    j.type = Type::Object;
    return j;
}

bool Json::isInteger() const {
    if (type != Type::Number || text.empty()) return false;
    for (char c : text)
        if (c == '.' || c == 'e' || c == 'E') return false;
    return true;
}

int64_t Json::asInt() const {
    if (type == Type::Bool) return b ? 1 : 0;
    if (type != Type::Number) return 0;
    if (isInteger()) return std::strtoll(text.c_str(), nullptr, 10);
    return static_cast<int64_t>(parseNumber(text));
}

double Json::asDouble() const {
    if (type == Type::Bool) return b ? 1 : 0;
    if (type != Type::Number) return 0;
    return parseNumber(text);
}

const Json* Json::get(const std::string& key) const {
    if (type != Type::Object) return nullptr;
    for (const auto& f : fields)
        if (f.first == key) return &f.second;
    return nullptr;
}

Json& Json::set(const std::string& key, Json v) {
    type = Type::Object;
    for (auto& f : fields)
        if (f.first == key) {
            f.second = std::move(v);
            return *this;
        }
    fields.emplace_back(key, std::move(v));
    return *this;
}

Json& Json::push(Json v) {
    type = Type::Array;
    items.push_back(std::move(v));
    return *this;
}

static void dumpString(const std::string& s, std::string& out) {
    out += '"';
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04x", c);
                    out += buf;
                } else {
                    out += static_cast<char>(c);  // UTF-8 как есть
                }
        }
    }
    out += '"';
}

static void dumpTo(const Json& j, std::string& out) {
    switch (j.type) {
        case Json::Type::Null: out += "null"; break;
        case Json::Type::Bool: out += j.b ? "true" : "false"; break;
        case Json::Type::Number: out += j.text; break;
        case Json::Type::String: dumpString(j.text, out); break;
        case Json::Type::Array:
            out += '[';
            for (size_t i = 0; i < j.items.size(); ++i) {
                if (i) out += ',';
                dumpTo(j.items[i], out);
            }
            out += ']';
            break;
        case Json::Type::Object:
            out += '{';
            for (size_t i = 0; i < j.fields.size(); ++i) {
                if (i) out += ',';
                dumpString(j.fields[i].first, out);
                out += ':';
                dumpTo(j.fields[i].second, out);
            }
            out += '}';
            break;
    }
}

std::string Json::dump() const {
    std::string out;
    dumpTo(*this, out);
    return out;
}

bool operator==(const Json& a, const Json& b) {
    if (a.type != b.type) {
        return false;
    }
    switch (a.type) {
        case Json::Type::Null: return true;
        case Json::Type::Bool: return a.b == b.b;
        case Json::Type::Number: return a.text == b.text || a.asDouble() == b.asDouble();
        case Json::Type::String: return a.text == b.text;
        case Json::Type::Array: return a.items == b.items;
        case Json::Type::Object: return a.fields == b.fields;
    }
    return false;
}

// --- разбор ---

namespace {

struct Parser {
    const std::string& s;
    size_t i = 0;
    std::string err;
    int depth = 0;

    explicit Parser(const std::string& src) : s(src) {}

    void ws() {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) ++i;
    }

    bool fail(const char* what) {
        if (err.empty()) err = std::string(what) + " (позиция " + std::to_string(i) + ")";
        return false;
    }

    static void appendUtf8(std::string& out, uint32_t cp) {
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

    bool hex4(uint32_t& v) {
        if (i + 4 > s.size()) return fail("обрыв \\u");
        v = 0;
        for (int k = 0; k < 4; ++k) {
            char c = s[i++];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= static_cast<uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f') v |= static_cast<uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= static_cast<uint32_t>(c - 'A' + 10);
            else return fail("неверный \\u");
        }
        return true;
    }

    bool str(std::string& out) {
        ++i;  // "
        while (i < s.size()) {
            char c = s[i++];
            if (c == '"') return true;
            if (c != '\\') {
                out += c;
                continue;
            }
            if (i >= s.size()) break;
            char e = s[i++];
            switch (e) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u': {
                    uint32_t cp;
                    if (!hex4(cp)) return false;
                    if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < s.size() && s[i] == '\\' && s[i + 1] == 'u') {
                        i += 2;
                        uint32_t lo;
                        if (!hex4(lo)) return false;
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    }
                    appendUtf8(out, cp);
                    break;
                }
                default: return fail("неверная escape-последовательность");
            }
        }
        return fail("незакрытая строка");
    }

    bool value(Json& out) {
        if (++depth > 64) return fail("слишком глубокая вложенность");
        ws();
        if (i >= s.size()) return fail("неожиданный конец");
        char c = s[i];
        bool ok = true;
        if (c == '{') {
            out = Json::object();
            ++i;
            ws();
            if (i < s.size() && s[i] == '}') {
                ++i;
            } else {
                for (;;) {
                    ws();
                    if (i >= s.size() || s[i] != '"') { ok = fail("ожидается ключ"); break; }
                    std::string key;
                    if (!str(key)) { ok = false; break; }
                    ws();
                    if (i >= s.size() || s[i] != ':') { ok = fail("ожидается ':'"); break; }
                    ++i;
                    Json v;
                    if (!value(v)) { ok = false; break; }
                    out.fields.emplace_back(std::move(key), std::move(v));
                    ws();
                    if (i < s.size() && s[i] == ',') { ++i; continue; }
                    if (i < s.size() && s[i] == '}') { ++i; break; }
                    ok = fail("ожидается ',' или '}'");
                    break;
                }
            }
        } else if (c == '[') {
            out = Json::array();
            ++i;
            ws();
            if (i < s.size() && s[i] == ']') {
                ++i;
            } else {
                for (;;) {
                    Json v;
                    if (!value(v)) { ok = false; break; }
                    out.items.push_back(std::move(v));
                    ws();
                    if (i < s.size() && s[i] == ',') { ++i; continue; }
                    if (i < s.size() && s[i] == ']') { ++i; break; }
                    ok = fail("ожидается ',' или ']'");
                    break;
                }
            }
        } else if (c == '"') {
            out = Json::string("");
            ok = str(out.text);
        } else if (s.compare(i, 4, "true") == 0) {
            out = Json::boolean(true);
            i += 4;
        } else if (s.compare(i, 5, "false") == 0) {
            out = Json::boolean(false);
            i += 5;
        } else if (s.compare(i, 4, "null") == 0) {
            out = Json::null();
            i += 4;
        } else if (c == '-' || (c >= '0' && c <= '9')) {
            size_t st = i;
            if (s[i] == '-') ++i;
            while (i < s.size() && ((s[i] >= '0' && s[i] <= '9') || s[i] == '.' || s[i] == 'e' || s[i] == 'E' || s[i] == '+' || s[i] == '-')) ++i;
            out = Json::numberRaw(s.substr(st, i - st));
        } else {
            ok = fail("неожиданный символ");
        }
        --depth;
        return ok;
    }
};

}  // namespace

bool parseJson(const std::string& s, Json& out, std::string& err) {
    Parser p(s);
    if (!p.value(out)) {
        err = p.err;
        return false;
    }
    p.ws();
    if (p.i != s.size()) {
        err = "лишние данные после JSON";
        return false;
    }
    return true;
}

}  // namespace emu
