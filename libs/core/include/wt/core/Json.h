#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace wt::core {

// Small JSON value used for configuration files (settings, sessions, themes, vault headers).
// Objects keep their keys sorted so serialised files are stable and diff well.
// Numbers are stored as double; integers up to 2^53 round-trip exactly.
class Json {
public:
    enum class Type : std::uint8_t { Null, Bool, Number, String, Array, Object };
    using Array = std::vector<Json>;
    using Object = std::map<std::string, Json, std::less<>>;

    Json() = default;
    Json(std::nullptr_t) {}
    Json(bool value) : m_value(value) {}
    Json(int value) : m_value(static_cast<double>(value)) {}
    Json(unsigned value) : m_value(static_cast<double>(value)) {}
    Json(std::int64_t value) : m_value(static_cast<double>(value)) {}
    Json(std::uint64_t value) : m_value(static_cast<double>(value)) {}
    Json(double value) : m_value(value) {}
    Json(const char* value) : m_value(std::string(value)) {}
    Json(std::string value) : m_value(std::move(value)) {}
    Json(std::string_view value) : m_value(std::string(value)) {}
    Json(Array value) : m_value(std::move(value)) {}
    Json(Object value) : m_value(std::move(value)) {}

    static Json array() { return Json(Array{}); }
    static Json object() { return Json(Object{}); }

    Type type() const { return static_cast<Type>(m_value.index()); }
    bool isNull() const { return type() == Type::Null; }
    bool isBool() const { return type() == Type::Bool; }
    bool isNumber() const { return type() == Type::Number; }
    bool isString() const { return type() == Type::String; }
    bool isArray() const { return type() == Type::Array; }
    bool isObject() const { return type() == Type::Object; }

    // Typed accessors return the fallback when the value has a different type.
    bool toBool(bool fallback = false) const;
    double toDouble(double fallback = 0) const;
    std::int64_t toInt(std::int64_t fallback = 0) const;
    std::string toString(std::string_view fallback = {}) const;
    const Array& items() const;   // empty unless an array
    const Object& fields() const; // empty unless an object

    // Mutable access converts the value to an array/object first.
    Array& asArray();
    Object& asObject();

    // Object lookup; returns a null value for missing keys or non-objects.
    const Json& operator[](std::string_view key) const;
    // Inserts (converting to an object if needed).
    Json& operator[](std::string_view key);
    bool contains(std::string_view key) const;
    void push(Json value) { asArray().push_back(std::move(value)); }

    // Compact when indent < 0, otherwise pretty-printed with `indent` spaces.
    std::string dump(int indent = -1) const;
    // Strict RFC 8259 parser (nesting limited to 256 levels). On failure returns nullopt and,
    // if `error` is given, a message with the byte offset.
    static std::optional<Json> parse(std::string_view text, std::string* error = nullptr);

    friend bool operator==(const Json&, const Json&) = default;

private:
    void dumpTo(std::string& out, int indent, int depth) const;

    std::variant<std::nullptr_t, bool, double, std::string, Array, Object> m_value;
};

} // namespace wt::core
