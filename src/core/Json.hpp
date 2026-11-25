// VortexClean core — a small, strict JSON document model.
//
// Used to emit audit logs, to parse them back in the test-suite, and as the wire
// format between the app and its privileged helper. Objects preserve insertion
// order so that emitted reports are stable and diff-friendly.
#pragma once

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "Result.hpp"

namespace vortex {

class JsonValue {
public:
    using Array = std::vector<JsonValue>;
    using Member = std::pair<std::string, JsonValue>;
    using Object = std::vector<Member>;

    JsonValue() noexcept : data_(nullptr) {}
    JsonValue(std::nullptr_t) noexcept : data_(nullptr) {}  // NOLINT(google-explicit-constructor)
    JsonValue(bool value) noexcept : data_(value) {}        // NOLINT(google-explicit-constructor)
    JsonValue(double value) noexcept : data_(value) {}      // NOLINT(google-explicit-constructor)
    JsonValue(const char* value) : data_(std::string(value)) {}        // NOLINT(google-explicit-constructor)
    JsonValue(std::string value) : data_(std::move(value)) {}          // NOLINT(google-explicit-constructor)
    JsonValue(std::string_view value) : data_(std::string(value)) {}   // NOLINT(google-explicit-constructor)
    JsonValue(Array value) : data_(std::move(value)) {}                // NOLINT(google-explicit-constructor)
    JsonValue(Object value) : data_(std::move(value)) {}               // NOLINT(google-explicit-constructor)

    /// Any integer type other than bool. Values above INT64_MAX degrade to double.
    template <std::integral T>
        requires(!std::same_as<T, bool>)
    JsonValue(T value) noexcept {  // NOLINT(google-explicit-constructor)
        if (std::cmp_less_equal(value, std::numeric_limits<std::int64_t>::max())) {
            data_ = static_cast<std::int64_t>(value);
        } else {
            data_ = static_cast<double>(value);
        }
    }

    [[nodiscard]] static JsonValue makeObject() { return JsonValue(Object{}); }
    [[nodiscard]] static JsonValue makeArray() { return JsonValue(Array{}); }

    [[nodiscard]] bool isNull() const noexcept { return std::holds_alternative<std::nullptr_t>(data_); }
    [[nodiscard]] bool isBool() const noexcept { return std::holds_alternative<bool>(data_); }
    [[nodiscard]] bool isInteger() const noexcept { return std::holds_alternative<std::int64_t>(data_); }
    [[nodiscard]] bool isDouble() const noexcept { return std::holds_alternative<double>(data_); }
    [[nodiscard]] bool isNumber() const noexcept { return isInteger() || isDouble(); }
    [[nodiscard]] bool isString() const noexcept { return std::holds_alternative<std::string>(data_); }
    [[nodiscard]] bool isArray() const noexcept { return std::holds_alternative<Array>(data_); }
    [[nodiscard]] bool isObject() const noexcept { return std::holds_alternative<Object>(data_); }

    [[nodiscard]] bool asBool(bool fallback = false) const noexcept;
    [[nodiscard]] std::int64_t asInteger(std::int64_t fallback = 0) const noexcept;
    [[nodiscard]] double asDouble(double fallback = 0.0) const noexcept;
    /// The string payload, or an empty string when this is not a string.
    [[nodiscard]] const std::string& asString() const noexcept;
    /// The elements, or an empty array when this is not an array.
    [[nodiscard]] const Array& asArray() const noexcept;
    /// The members, or an empty object when this is not an object.
    [[nodiscard]] const Object& asObject() const noexcept;

    /// Sets (or replaces) a member. A null value is promoted to an object first.
    JsonValue& set(std::string key, JsonValue value);
    /// Appends an element. A null value is promoted to an array first.
    JsonValue& push(JsonValue value);

    [[nodiscard]] const JsonValue* find(std::string_view key) const noexcept;
    /// Member lookup that yields a shared null value when the key is absent.
    [[nodiscard]] const JsonValue& operator[](std::string_view key) const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;

    /// Serialises to text. `indent` < 0 produces the compact single-line form.
    [[nodiscard]] std::string dump(int indent = 2) const;

    /// Strict RFC 8259 parser: one top-level value, no comments, no trailing
    /// commas, bounded nesting depth.
    [[nodiscard]] static Result<JsonValue> parse(std::string_view text);

private:
    void dumpTo(std::string& out, int indent, int level) const;

    std::variant<std::nullptr_t, bool, std::int64_t, double, std::string, Array, Object> data_;
};

}  // namespace vortex
