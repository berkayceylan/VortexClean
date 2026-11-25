// VortexClean core — minimal value-or-error type (C++20 has no std::expected).
#pragma once

#include <string>
#include <utility>
#include <variant>

namespace vortex {

/// An error with an errno-style (or domain specific) code and a human readable message.
struct Error {
    int code = 0;
    std::string message;
};

/// Holds either a value of type T or an Error. Never throws on construction.
template <typename T>
class Result {
public:
    Result(T value) : storage_(std::in_place_index<0>, std::move(value)) {}  // NOLINT(google-explicit-constructor)
    Result(Error error) : storage_(std::in_place_index<1>, std::move(error)) {}  // NOLINT(google-explicit-constructor)

    [[nodiscard]] static Result failure(int code, std::string message) {
        return Result(Error{code, std::move(message)});
    }

    [[nodiscard]] bool ok() const noexcept { return storage_.index() == 0; }
    explicit operator bool() const noexcept { return ok(); }

    [[nodiscard]] T& value() & { return std::get<0>(storage_); }
    [[nodiscard]] const T& value() const& { return std::get<0>(storage_); }
    [[nodiscard]] T&& value() && { return std::get<0>(std::move(storage_)); }

    [[nodiscard]] const Error& error() const { return std::get<1>(storage_); }

private:
    std::variant<T, Error> storage_;
};

/// Result for operations that produce no value.
using Status = Result<std::monostate>;

[[nodiscard]] inline Status success() { return Status(std::monostate{}); }
[[nodiscard]] inline Status failure(int code, std::string message) {
    return Status(Error{code, std::move(message)});
}

}  // namespace vortex
