#include "wt/core/Json.h"

#include "wt/core/Utf8.h"

#include <cmath>
#include <locale>
#include <sstream>

namespace wt::core {
namespace {

constexpr int kMaxDepth = 256;

const Json& nullJson() {
    static const Json value;
    return value;
}

void appendEscaped(std::string& out, std::string_view text) {
    out.push_back('"');
    for (char c : text) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                static constexpr char kHex[] = "0123456789abcdef";
                out += "\\u00";
                out.push_back(kHex[(c >> 4) & 0xF]);
                out.push_back(kHex[c & 0xF]);
            } else {
                out.push_back(c);
            }
        }
    }
    out.push_back('"');
}

void appendNumber(std::string& out, double value) {
    if (!std::isfinite(value)) {
        out += "null"; // JSON has no NaN/Infinity
        return;
    }
    if (value == std::trunc(value) && std::fabs(value) < 9007199254740992.0) {
        out += std::to_string(static_cast<long long>(value));
        return;
    }
    std::ostringstream os;
    os.imbue(std::locale::classic()); // never a locale-specific decimal separator
    os.precision(17);
    os << value;
    out += os.str();
}

class Parser {
public:
    explicit Parser(std::string_view text) : m_text(text) {}

    std::optional<Json> run(std::string* error) {
        skipSpace();
        std::optional<Json> value = parseValue(0);
        skipSpace();
        if (value && m_pos != m_text.size()) {
            fail("unexpected trailing characters");
            value.reset();
        }
        if (!value && error) {
            *error = m_error + " at offset " + std::to_string(m_pos);
        }
        return value;
    }

private:
    void fail(const char* message) {
        if (m_error.empty()) {
            m_error = message;
        }
    }
    bool atEnd() const { return m_pos >= m_text.size(); }
    char peek() const { return atEnd() ? '\0' : m_text[m_pos]; }
    void skipSpace() {
        while (!atEnd() && (peek() == ' ' || peek() == '\t' || peek() == '\n' || peek() == '\r')) {
            ++m_pos;
        }
    }
    bool consume(std::string_view literal) {
        if (m_text.substr(m_pos, literal.size()) == literal) {
            m_pos += literal.size();
            return true;
        }
        return false;
    }

    std::optional<Json> parseValue(int depth) {
        if (depth > kMaxDepth) {
            fail("nesting too deep");
            return std::nullopt;
        }
        switch (peek()) {
        case '{': return parseObject(depth);
        case '[': return parseArray(depth);
        case '"': {
            auto s = parseString();
            return s ? std::optional<Json>(Json(std::move(*s))) : std::nullopt;
        }
        case 't':
            if (consume("true")) {
                return Json(true);
            }
            break;
        case 'f':
            if (consume("false")) {
                return Json(false);
            }
            break;
        case 'n':
            if (consume("null")) {
                return Json();
            }
            break;
        default:
            if (peek() == '-' || (peek() >= '0' && peek() <= '9')) {
                return parseNumber();
            }
            break;
        }
        fail("unexpected character");
        return std::nullopt;
    }

    std::optional<Json> parseObject(int depth) {
        ++m_pos; // {
        Json::Object object;
        skipSpace();
        if (peek() == '}') {
            ++m_pos;
            return Json(std::move(object));
        }
        for (;;) {
            skipSpace();
            if (peek() != '"') {
                fail("expected string key");
                return std::nullopt;
            }
            auto key = parseString();
            if (!key) {
                return std::nullopt;
            }
            skipSpace();
            if (peek() != ':') {
                fail("expected ':'");
                return std::nullopt;
            }
            ++m_pos;
            skipSpace();
            auto value = parseValue(depth + 1);
            if (!value) {
                return std::nullopt;
            }
            object.insert_or_assign(std::move(*key), std::move(*value));
            skipSpace();
            if (peek() == ',') {
                ++m_pos;
                continue;
            }
            if (peek() == '}') {
                ++m_pos;
                return Json(std::move(object));
            }
            fail("expected ',' or '}'");
            return std::nullopt;
        }
    }

    std::optional<Json> parseArray(int depth) {
        ++m_pos; // [
        Json::Array array;
        skipSpace();
        if (peek() == ']') {
            ++m_pos;
            return Json(std::move(array));
        }
        for (;;) {
            skipSpace();
            auto value = parseValue(depth + 1);
            if (!value) {
                return std::nullopt;
            }
            array.push_back(std::move(*value));
            skipSpace();
            if (peek() == ',') {
                ++m_pos;
                continue;
            }
            if (peek() == ']') {
                ++m_pos;
                return Json(std::move(array));
            }
            fail("expected ',' or ']'");
            return std::nullopt;
        }
    }

    std::optional<unsigned> parseHex4() {
        if (m_pos + 4 > m_text.size()) {
            return std::nullopt;
        }
        unsigned value = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = m_text[m_pos++];
            value <<= 4;
            if (c >= '0' && c <= '9') {
                value |= static_cast<unsigned>(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                value |= static_cast<unsigned>(c - 'a' + 10);
            } else if (c >= 'A' && c <= 'F') {
                value |= static_cast<unsigned>(c - 'A' + 10);
            } else {
                return std::nullopt;
            }
        }
        return value;
    }

    std::optional<std::string> parseString() {
        ++m_pos; // opening quote
        std::string out;
        while (!atEnd()) {
            const char c = m_text[m_pos++];
            if (c == '"') {
                return out;
            }
            if (static_cast<unsigned char>(c) < 0x20) {
                fail("control character in string");
                return std::nullopt;
            }
            if (c != '\\') {
                out.push_back(c);
                continue;
            }
            if (atEnd()) {
                break;
            }
            const char e = m_text[m_pos++];
            switch (e) {
            case '"': out.push_back('"'); break;
            case '\\': out.push_back('\\'); break;
            case '/': out.push_back('/'); break;
            case 'b': out.push_back('\b'); break;
            case 'f': out.push_back('\f'); break;
            case 'n': out.push_back('\n'); break;
            case 'r': out.push_back('\r'); break;
            case 't': out.push_back('\t'); break;
            case 'u': {
                auto unit = parseHex4();
                if (!unit) {
                    fail("invalid \\u escape");
                    return std::nullopt;
                }
                char32_t cp = *unit;
                if (cp >= 0xD800 && cp <= 0xDBFF && m_text.substr(m_pos, 2) == "\\u") {
                    const std::size_t save = m_pos;
                    m_pos += 2;
                    const auto low = parseHex4();
                    if (low && *low >= 0xDC00 && *low <= 0xDFFF) {
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (*low - 0xDC00);
                    } else {
                        m_pos = save; // not a pair: the next escape is decoded on its own
                    }
                }
                appendUtf8(out, cp); // lone surrogates become U+FFFD
                break;
            }
            default: fail("invalid escape"); return std::nullopt;
            }
        }
        fail("unterminated string");
        return std::nullopt;
    }

    std::optional<Json> parseNumber() {
        const std::size_t start = m_pos;
        auto digits = [this] {
            const std::size_t from = m_pos;
            while (peek() >= '0' && peek() <= '9') {
                ++m_pos;
            }
            return m_pos - from;
        };
        if (peek() == '-') {
            ++m_pos;
        }
        if (peek() == '0') {
            ++m_pos;
        } else if (digits() == 0) {
            fail("invalid number");
            return std::nullopt;
        }
        if (peek() == '.') {
            ++m_pos;
            if (digits() == 0) {
                fail("invalid number");
                return std::nullopt;
            }
        }
        if (peek() == 'e' || peek() == 'E') {
            ++m_pos;
            if (peek() == '+' || peek() == '-') {
                ++m_pos;
            }
            if (digits() == 0) {
                fail("invalid number");
                return std::nullopt;
            }
        }
        std::istringstream is(std::string(m_text.substr(start, m_pos - start)));
        is.imbue(std::locale::classic());
        double value = 0;
        is >> value;
        if (is.fail()) {
            fail("number out of range");
            return std::nullopt;
        }
        return Json(value);
    }

    std::string_view m_text;
    std::size_t m_pos = 0;
    std::string m_error;
};

} // namespace

bool Json::toBool(bool fallback) const {
    return isBool() ? std::get<bool>(m_value) : fallback;
}

double Json::toDouble(double fallback) const {
    return isNumber() ? std::get<double>(m_value) : fallback;
}

std::int64_t Json::toInt(std::int64_t fallback) const {
    if (!isNumber()) {
        return fallback;
    }
    const double v = std::get<double>(m_value);
    if (!std::isfinite(v) || v < -9.2e18 || v > 9.2e18) {
        return fallback;
    }
    return static_cast<std::int64_t>(v);
}

std::string Json::toString(std::string_view fallback) const {
    return isString() ? std::get<std::string>(m_value) : std::string(fallback);
}

const Json::Array& Json::items() const {
    static const Array empty;
    return isArray() ? std::get<Array>(m_value) : empty;
}

const Json::Object& Json::fields() const {
    static const Object empty;
    return isObject() ? std::get<Object>(m_value) : empty;
}

Json::Array& Json::asArray() {
    if (!isArray()) {
        m_value = Array{};
    }
    return std::get<Array>(m_value);
}

Json::Object& Json::asObject() {
    if (!isObject()) {
        m_value = Object{};
    }
    return std::get<Object>(m_value);
}

const Json& Json::operator[](std::string_view key) const {
    if (!isObject()) {
        return nullJson();
    }
    const auto& object = std::get<Object>(m_value);
    const auto it = object.find(key);
    return it == object.end() ? nullJson() : it->second;
}

Json& Json::operator[](std::string_view key) {
    auto& object = asObject();
    auto it = object.find(key);
    if (it == object.end()) {
        it = object.emplace(std::string(key), Json()).first;
    }
    return it->second;
}

bool Json::contains(std::string_view key) const {
    return isObject() && std::get<Object>(m_value).count(key) > 0;
}

std::string Json::dump(int indent) const {
    std::string out;
    dumpTo(out, indent, 0);
    return out;
}

void Json::dumpTo(std::string& out, int indent, int depth) const {
    auto newline = [&](int level) {
        if (indent >= 0) {
            out.push_back('\n');
            out.append(static_cast<std::size_t>(indent * level), ' ');
        }
    };
    switch (type()) {
    case Type::Null: out += "null"; break;
    case Type::Bool: out += std::get<bool>(m_value) ? "true" : "false"; break;
    case Type::Number: appendNumber(out, std::get<double>(m_value)); break;
    case Type::String: appendEscaped(out, std::get<std::string>(m_value)); break;
    case Type::Array: {
        const auto& array = std::get<Array>(m_value);
        out.push_back('[');
        for (std::size_t i = 0; i < array.size(); ++i) {
            if (i > 0) {
                out.push_back(',');
            }
            newline(depth + 1);
            array[i].dumpTo(out, indent, depth + 1);
        }
        if (!array.empty()) {
            newline(depth);
        }
        out.push_back(']');
        break;
    }
    case Type::Object: {
        const auto& object = std::get<Object>(m_value);
        out.push_back('{');
        bool first = true;
        for (const auto& [key, value] : object) {
            if (!first) {
                out.push_back(',');
            }
            first = false;
            newline(depth + 1);
            appendEscaped(out, key);
            out += indent >= 0 ? ": " : ":";
            value.dumpTo(out, indent, depth + 1);
        }
        if (!object.empty()) {
            newline(depth);
        }
        out.push_back('}');
        break;
    }
    }
}

std::optional<Json> Json::parse(std::string_view text, std::string* error) {
    return Parser(text).run(error);
}

} // namespace wt::core
