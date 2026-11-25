#include "Json.hpp"

#include <array>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "StringUtil.hpp"

namespace vortex {

namespace {

constexpr int kMaxNestingDepth = 128;

void appendEscaped(std::string& out, std::string_view raw) {
    const std::string text = str::sanitizeUtf8(raw);
    out.push_back('"');
    for (const char ch : text) {
        const auto c = static_cast<unsigned char>(ch);
        switch (c) {
            case '"': out.append("\\\""); break;
            case '\\': out.append("\\\\"); break;
            case '\b': out.append("\\b"); break;
            case '\f': out.append("\\f"); break;
            case '\n': out.append("\\n"); break;
            case '\r': out.append("\\r"); break;
            case '\t': out.append("\\t"); break;
            default:
                if (c < 0x20 || c == 0x7F) {
                    std::array<char, 8> buffer{};
                    std::snprintf(buffer.data(), buffer.size(), "\\u%04x", static_cast<unsigned>(c));
                    out.append(buffer.data());
                } else {
                    out.push_back(ch);
                }
        }
    }
    out.push_back('"');
}

void appendNewline(std::string& out, int indent, int level) {
    if (indent < 0) return;
    out.push_back('\n');
    out.append(static_cast<std::size_t>(indent) * static_cast<std::size_t>(level), ' ');
}

void appendUtf8(std::string& out, std::uint32_t codePoint) {
    if (codePoint < 0x80) {
        out.push_back(static_cast<char>(codePoint));
    } else if (codePoint < 0x800) {
        out.push_back(static_cast<char>(0xC0u | (codePoint >> 6)));
        out.push_back(static_cast<char>(0x80u | (codePoint & 0x3Fu)));
    } else if (codePoint < 0x10000) {
        out.push_back(static_cast<char>(0xE0u | (codePoint >> 12)));
        out.push_back(static_cast<char>(0x80u | ((codePoint >> 6) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (codePoint & 0x3Fu)));
    } else {
        out.push_back(static_cast<char>(0xF0u | (codePoint >> 18)));
        out.push_back(static_cast<char>(0x80u | ((codePoint >> 12) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | ((codePoint >> 6) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (codePoint & 0x3Fu)));
    }
}

class Parser {
public:
    explicit Parser(std::string_view text) : text_(text) {}

    Result<JsonValue> run() {
        JsonValue value;
        skipWhitespace();
        if (!parseValue(value, 0)) return fail();
        skipWhitespace();
        if (pos_ != text_.size()) {
            setError("unexpected trailing characters");
            return fail();
        }
        return value;
    }

private:
    Result<JsonValue> fail() const {
        return Error{EINVAL, "JSON parse error at offset " + std::to_string(errorPos_) + ": " + error_};
    }

    void setError(const char* message) {
        if (error_.empty()) {
            error_ = message;
            errorPos_ = pos_;
        }
    }

    void skipWhitespace() noexcept {
        while (pos_ < text_.size()) {
            const char c = text_[pos_];
            if (c != ' ' && c != '\t' && c != '\n' && c != '\r') break;
            ++pos_;
        }
    }

    bool consumeLiteral(std::string_view literal) {
        if (text_.substr(pos_, literal.size()) != literal) {
            setError("invalid literal");
            return false;
        }
        pos_ += literal.size();
        return true;
    }

    bool parseValue(JsonValue& out, int depth) {
        if (depth > kMaxNestingDepth) {
            setError("nesting too deep");
            return false;
        }
        if (pos_ >= text_.size()) {
            setError("unexpected end of input");
            return false;
        }
        switch (text_[pos_]) {
            case 'n':
                if (!consumeLiteral("null")) return false;
                out = JsonValue(nullptr);
                return true;
            case 't':
                if (!consumeLiteral("true")) return false;
                out = JsonValue(true);
                return true;
            case 'f':
                if (!consumeLiteral("false")) return false;
                out = JsonValue(false);
                return true;
            case '"': {
                std::string text;
                if (!parseString(text)) return false;
                out = JsonValue(std::move(text));
                return true;
            }
            case '[': return parseArray(out, depth);
            case '{': return parseObject(out, depth);
            default: return parseNumber(out);
        }
    }

    bool parseHex4(std::uint32_t& out) {
        if (pos_ + 4 > text_.size()) {
            setError("truncated \\u escape");
            return false;
        }
        std::uint32_t value = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = text_[pos_ + static_cast<std::size_t>(i)];
            std::uint32_t digit = 0;
            if (c >= '0' && c <= '9') {
                digit = static_cast<std::uint32_t>(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                digit = static_cast<std::uint32_t>(c - 'a' + 10);
            } else if (c >= 'A' && c <= 'F') {
                digit = static_cast<std::uint32_t>(c - 'A' + 10);
            } else {
                setError("invalid \\u escape");
                return false;
            }
            value = (value << 4) | digit;
        }
        pos_ += 4;
        out = value;
        return true;
    }

    bool parseString(std::string& out) {
        ++pos_;  // opening quote
        for (;;) {
            if (pos_ >= text_.size()) {
                setError("unterminated string");
                return false;
            }
            const char c = text_[pos_];
            if (c == '"') {
                ++pos_;
                return true;
            }
            if (static_cast<unsigned char>(c) < 0x20) {
                setError("unescaped control character in string");
                return false;
            }
            if (c != '\\') {
                out.push_back(c);
                ++pos_;
                continue;
            }
            ++pos_;
            if (pos_ >= text_.size()) {
                setError("unterminated escape sequence");
                return false;
            }
            const char escape = text_[pos_++];
            switch (escape) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'u': {
                    std::uint32_t unit = 0;
                    if (!parseHex4(unit)) return false;
                    if (unit >= 0xD800 && unit <= 0xDBFF) {
                        if (text_.substr(pos_, 2) != "\\u") {
                            setError("unpaired surrogate");
                            return false;
                        }
                        pos_ += 2;
                        std::uint32_t low = 0;
                        if (!parseHex4(low)) return false;
                        if (low < 0xDC00 || low > 0xDFFF) {
                            setError("invalid low surrogate");
                            return false;
                        }
                        unit = 0x10000 + ((unit - 0xD800) << 10) + (low - 0xDC00);
                    } else if (unit >= 0xDC00 && unit <= 0xDFFF) {
                        setError("unpaired surrogate");
                        return false;
                    }
                    appendUtf8(out, unit);
                    break;
                }
                default:
                    --pos_;
                    setError("invalid escape sequence");
                    return false;
            }
        }
    }

    bool parseNumber(JsonValue& out) {
        const std::size_t start = pos_;
        bool isInteger = true;
        if (pos_ < text_.size() && text_[pos_] == '-') ++pos_;
        if (pos_ >= text_.size() || text_[pos_] < '0' || text_[pos_] > '9') {
            pos_ = start;
            setError("invalid value");
            return false;
        }
        if (text_[pos_] == '0') {
            ++pos_;
        } else {
            while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') ++pos_;
        }
        if (pos_ < text_.size() && text_[pos_] == '.') {
            isInteger = false;
            ++pos_;
            if (pos_ >= text_.size() || text_[pos_] < '0' || text_[pos_] > '9') {
                setError("invalid number: digits expected after decimal point");
                return false;
            }
            while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') ++pos_;
        }
        if (pos_ < text_.size() && (text_[pos_] == 'e' || text_[pos_] == 'E')) {
            isInteger = false;
            ++pos_;
            if (pos_ < text_.size() && (text_[pos_] == '+' || text_[pos_] == '-')) ++pos_;
            if (pos_ >= text_.size() || text_[pos_] < '0' || text_[pos_] > '9') {
                setError("invalid number: digits expected in exponent");
                return false;
            }
            while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') ++pos_;
        }
        const std::string token(text_.substr(start, pos_ - start));
        if (isInteger) {
            errno = 0;
            char* end = nullptr;
            const long long parsed = std::strtoll(token.c_str(), &end, 10);
            if (errno == 0 && end != nullptr && *end == '\0') {
                out = JsonValue(static_cast<std::int64_t>(parsed));
                return true;
            }
            // Out of int64 range: fall through and keep it as a double.
        }
        errno = 0;
        char* end = nullptr;
        const double parsed = std::strtod(token.c_str(), &end);
        if (end == nullptr || *end != '\0' || !std::isfinite(parsed)) {
            pos_ = start;
            setError("number out of range");
            return false;
        }
        out = JsonValue(parsed);
        return true;
    }

    bool parseArray(JsonValue& out, int depth) {
        ++pos_;  // '['
        JsonValue::Array elements;
        skipWhitespace();
        if (pos_ < text_.size() && text_[pos_] == ']') {
            ++pos_;
            out = JsonValue(std::move(elements));
            return true;
        }
        for (;;) {
            skipWhitespace();
            JsonValue element;
            if (!parseValue(element, depth + 1)) return false;
            elements.push_back(std::move(element));
            skipWhitespace();
            if (pos_ >= text_.size()) {
                setError("unterminated array");
                return false;
            }
            if (text_[pos_] == ',') {
                ++pos_;
                continue;
            }
            if (text_[pos_] == ']') {
                ++pos_;
                out = JsonValue(std::move(elements));
                return true;
            }
            setError("expected ',' or ']'");
            return false;
        }
    }

    bool parseObject(JsonValue& out, int depth) {
        ++pos_;  // '{'
        JsonValue::Object members;
        skipWhitespace();
        if (pos_ < text_.size() && text_[pos_] == '}') {
            ++pos_;
            out = JsonValue(std::move(members));
            return true;
        }
        for (;;) {
            skipWhitespace();
            if (pos_ >= text_.size() || text_[pos_] != '"') {
                setError("expected string key");
                return false;
            }
            std::string key;
            if (!parseString(key)) return false;
            skipWhitespace();
            if (pos_ >= text_.size() || text_[pos_] != ':') {
                setError("expected ':'");
                return false;
            }
            ++pos_;
            skipWhitespace();
            JsonValue value;
            if (!parseValue(value, depth + 1)) return false;
            members.emplace_back(std::move(key), std::move(value));
            skipWhitespace();
            if (pos_ >= text_.size()) {
                setError("unterminated object");
                return false;
            }
            if (text_[pos_] == ',') {
                ++pos_;
                continue;
            }
            if (text_[pos_] == '}') {
                ++pos_;
                out = JsonValue(std::move(members));
                return true;
            }
            setError("expected ',' or '}'");
            return false;
        }
    }

    std::string_view text_;
    std::size_t pos_ = 0;
    std::string error_;
    std::size_t errorPos_ = 0;
};

}  // namespace

bool JsonValue::asBool(bool fallback) const noexcept {
    if (const bool* value = std::get_if<bool>(&data_)) return *value;
    return fallback;
}

std::int64_t JsonValue::asInteger(std::int64_t fallback) const noexcept {
    if (const std::int64_t* value = std::get_if<std::int64_t>(&data_)) return *value;
    if (const double* value = std::get_if<double>(&data_)) {
        if (std::isfinite(*value) && *value >= -9.2e18 && *value <= 9.2e18) {
            return static_cast<std::int64_t>(*value);
        }
    }
    return fallback;
}

double JsonValue::asDouble(double fallback) const noexcept {
    if (const double* value = std::get_if<double>(&data_)) return *value;
    if (const std::int64_t* value = std::get_if<std::int64_t>(&data_)) return static_cast<double>(*value);
    return fallback;
}

const std::string& JsonValue::asString() const noexcept {
    static const std::string empty;
    if (const std::string* value = std::get_if<std::string>(&data_)) return *value;
    return empty;
}

const JsonValue::Array& JsonValue::asArray() const noexcept {
    static const Array empty;
    if (const Array* value = std::get_if<Array>(&data_)) return *value;
    return empty;
}

const JsonValue::Object& JsonValue::asObject() const noexcept {
    static const Object empty;
    if (const Object* value = std::get_if<Object>(&data_)) return *value;
    return empty;
}

JsonValue& JsonValue::set(std::string key, JsonValue value) {
    if (!isObject()) data_ = Object{};
    auto& members = std::get<Object>(data_);
    for (auto& member : members) {
        if (member.first == key) {
            member.second = std::move(value);
            return *this;
        }
    }
    members.emplace_back(std::move(key), std::move(value));
    return *this;
}

JsonValue& JsonValue::push(JsonValue value) {
    if (!isArray()) data_ = Array{};
    std::get<Array>(data_).push_back(std::move(value));
    return *this;
}

const JsonValue* JsonValue::find(std::string_view key) const noexcept {
    const Object* members = std::get_if<Object>(&data_);
    if (members == nullptr) return nullptr;
    for (const auto& member : *members) {
        if (member.first == key) return &member.second;
    }
    return nullptr;
}

const JsonValue& JsonValue::operator[](std::string_view key) const noexcept {
    static const JsonValue null;
    const JsonValue* found = find(key);
    return found != nullptr ? *found : null;
}

std::size_t JsonValue::size() const noexcept {
    if (const Array* elements = std::get_if<Array>(&data_)) return elements->size();
    if (const Object* members = std::get_if<Object>(&data_)) return members->size();
    return 0;
}

void JsonValue::dumpTo(std::string& out, int indent, int level) const {
    if (isNull()) {
        out.append("null");
    } else if (const bool* flag = std::get_if<bool>(&data_)) {
        out.append(*flag ? "true" : "false");
    } else if (const std::int64_t* integer = std::get_if<std::int64_t>(&data_)) {
        out.append(std::to_string(*integer));
    } else if (const double* real = std::get_if<double>(&data_)) {
        if (!std::isfinite(*real)) {
            out.append("null");  // JSON has no representation for NaN / infinity
        } else {
            std::array<char, 40> buffer{};
            std::snprintf(buffer.data(), buffer.size(), "%.17g", *real);
            out.append(buffer.data());
        }
    } else if (const std::string* text = std::get_if<std::string>(&data_)) {
        appendEscaped(out, *text);
    } else if (const Array* elements = std::get_if<Array>(&data_)) {
        if (elements->empty()) {
            out.append("[]");
            return;
        }
        out.push_back('[');
        for (std::size_t i = 0; i < elements->size(); ++i) {
            if (i != 0) out.push_back(',');
            appendNewline(out, indent, level + 1);
            (*elements)[i].dumpTo(out, indent, level + 1);
        }
        appendNewline(out, indent, level);
        out.push_back(']');
    } else if (const Object* members = std::get_if<Object>(&data_)) {
        if (members->empty()) {
            out.append("{}");
            return;
        }
        out.push_back('{');
        for (std::size_t i = 0; i < members->size(); ++i) {
            if (i != 0) out.push_back(',');
            appendNewline(out, indent, level + 1);
            appendEscaped(out, (*members)[i].first);
            out.append(indent < 0 ? ":" : ": ");
            (*members)[i].second.dumpTo(out, indent, level + 1);
        }
        appendNewline(out, indent, level);
        out.push_back('}');
    }
}

std::string JsonValue::dump(int indent) const {
    std::string out;
    dumpTo(out, indent, 0);
    if (indent >= 0) out.push_back('\n');
    return out;
}

Result<JsonValue> JsonValue::parse(std::string_view text) {
    return Parser(text).run();
}

}  // namespace vortex
