// A deliberately tiny test harness (the project uses no third-party libraries).
//
//   VT_TEST(name) { ... }      registers a test case
//   VT_CHECK(cond)             records a failure and continues
//   VT_REQUIRE(cond)           records a failure and aborts the current test
//   VT_CHECK_EQ(a, b)          like VT_CHECK(a == b), printing both values
#pragma once

#include <cstddef>
#include <cstdint>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace vt {

struct AbortTest {};

class Registrar {
public:
    Registrar(const char* name, void (*function)());
};

void reportFailure(const char* file, int line, const std::string& message);
void note(const std::string& message);  ///< informational output shown under the running test

/// Runs every registered test (optionally filtered by a substring given as argv[1]).
int runAll(int argc, char** argv);

template <typename T>
std::string describe(const T& value) {
    if constexpr (requires(std::ostringstream& os, const T& v) { os << v; }) {
        std::ostringstream os;
        os << value;
        return os.str();
    } else {
        return "<value>";
    }
}
inline std::string describe(bool value) { return value ? "true" : "false"; }

// ---------------------------------------------------------------------------
// Fixtures
// ---------------------------------------------------------------------------

/// A unique scratch directory with a canonical (symlink-free) path, removed on destruction.
class TempDir {
public:
    explicit TempDir(std::string_view label = "vortex-test");
    ~TempDir();
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    [[nodiscard]] const std::string& path() const noexcept { return path_; }
    [[nodiscard]] std::string sub(std::string_view relative) const;

private:
    std::string path_;
};

void makeDirs(const std::string& path);
void writeFile(const std::string& path, std::string_view content);
void writeFile(const std::string& path, const std::vector<std::uint8_t>& content);
void writeFileOfSize(const std::string& path, std::size_t bytes, char fill = 'x');
[[nodiscard]] std::string readFile(const std::string& path);
[[nodiscard]] bool exists(const std::string& path);  ///< lstat based: true for dangling symlinks too
[[nodiscard]] bool isDir(const std::string& path);

/// Every path below `root` (relative, sorted) with its type and size: used to
/// prove that an operation left a tree untouched.
[[nodiscard]] std::vector<std::string> snapshotTree(const std::string& root);

/// Writes a minimal but valid application bundle and returns its path.
std::string makeAppBundle(const std::string& parentDir, std::string_view fileName, std::string_view bundleId,
                          std::string_view executable, std::string_view extraPlistXml = {});

/// An XML property list with string values.
[[nodiscard]] std::string plistXml(const std::vector<std::pair<std::string, std::string>>& stringValues,
                                   std::string_view extraXml = {});

}  // namespace vt

#define VT_CONCAT_INNER(a, b) a##b
#define VT_CONCAT(a, b) VT_CONCAT_INNER(a, b)

#define VT_TEST(name)                                                             \
    static void VT_CONCAT(vt_test_, name)();                                      \
    static const ::vt::Registrar VT_CONCAT(vt_registrar_, name)(#name, &VT_CONCAT(vt_test_, name)); \
    static void VT_CONCAT(vt_test_, name)()

#define VT_CHECK(condition)                                                       \
    do {                                                                          \
        if (!(condition)) ::vt::reportFailure(__FILE__, __LINE__, "CHECK failed: " #condition); \
    } while (false)

#define VT_REQUIRE(condition)                                                     \
    do {                                                                          \
        if (!(condition)) {                                                       \
            ::vt::reportFailure(__FILE__, __LINE__, "REQUIRE failed: " #condition); \
            throw ::vt::AbortTest{};                                              \
        }                                                                         \
    } while (false)

#define VT_CHECK_EQ(actual, expected)                                             \
    do {                                                                          \
        const auto& vt_actual = (actual);                                         \
        const auto& vt_expected = (expected);                                     \
        if (!(vt_actual == vt_expected)) {                                        \
            ::vt::reportFailure(__FILE__, __LINE__,                               \
                                std::string("CHECK_EQ failed: " #actual " == " #expected "\n      actual:   ") + \
                                    ::vt::describe(vt_actual) + "\n      expected: " + ::vt::describe(vt_expected)); \
        }                                                                         \
    } while (false)
