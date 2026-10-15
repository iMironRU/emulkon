// Минимальный JSON для протокола Device API (docs/protocol.md): без внешних зависимостей.
// Числа хранятся исходным текстом — без потери точности при пересылке туда и обратно.
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace emu {

struct Json {
    enum class Type { Null, Bool, Number, String, Array, Object };

    Type type = Type::Null;
    bool b = false;
    std::string text;  // Number — исходная запись, String — значение (UTF-8)
    std::vector<Json> items;                           // Array
    std::vector<std::pair<std::string, Json>> fields;  // Object (порядок сохраняется)

    static Json null() { return Json(); }
    static Json boolean(bool v);
    static Json number(int64_t v);
    static Json number(double v);
    static Json numberRaw(std::string raw);
    static Json string(std::string v);
    static Json array();
    static Json object();

    bool isNull() const { return type == Type::Null; }
    bool isBool() const { return type == Type::Bool; }
    bool isNumber() const { return type == Type::Number; }
    bool isString() const { return type == Type::String; }
    bool isArray() const { return type == Type::Array; }
    bool isObject() const { return type == Type::Object; }

    // Целое без дробной части и экспоненты.
    bool isInteger() const;
    int64_t asInt() const;
    double asDouble() const;

    const Json* get(const std::string& key) const;
    Json& set(const std::string& key, Json v);
    Json& push(Json v);

    std::string dump() const;
};

bool operator==(const Json& a, const Json& b);
inline bool operator!=(const Json& a, const Json& b) { return !(a == b); }

// parseJson разбирает текст; при ошибке возвращает false и описание в err.
bool parseJson(const std::string& s, Json& out, std::string& err);

}  // namespace emu
