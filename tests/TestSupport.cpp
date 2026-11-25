#include "TestSupport.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <sys/stat.h>
#include <system_error>
#include <unistd.h>

namespace vt {

namespace {

struct TestCase {
    const char* name;
    void (*function)();
};

std::vector<TestCase>& registry() {
    static std::vector<TestCase> cases;
    return cases;
}

int g_failuresInCurrentTest = 0;

[[noreturn]] void die(const std::string& message) {
    std::fprintf(stderr, "fixture error: %s (%s)\n", message.c_str(), std::strerror(errno));
    std::abort();
}

/// Makes everything below `root` removable again (tests create read-only and locked items).
void unlockTree(const std::filesystem::path& root) {
    std::error_code ec;
    ::lchflags(root.c_str(), 0);
    ::chmod(root.c_str(), 0700);
    for (std::filesystem::recursive_directory_iterator
             it(root, std::filesystem::directory_options::skip_permission_denied, ec),
         end;
         !ec && it != end; it.increment(ec)) {
        const std::filesystem::path& current = it->path();
        struct stat st {};
        if (::lstat(current.c_str(), &st) != 0) continue;
        ::lchflags(current.c_str(), 0);
        if (S_ISDIR(st.st_mode)) ::chmod(current.c_str(), 0700);
    }
}

}  // namespace

Registrar::Registrar(const char* name, void (*function)()) {
    registry().push_back(TestCase{name, function});
}

void reportFailure(const char* file, int line, const std::string& message) {
    ++g_failuresInCurrentTest;
    std::fprintf(stderr, "    %s:%d: %s\n", file, line, message.c_str());
}

void note(const std::string& message) {
    std::printf("    %s\n", message.c_str());
}

int runAll(int argc, char** argv) {
    const std::string filter = argc > 1 ? argv[1] : "";
    int failedTests = 0;
    int ranTests = 0;
    const auto suiteStart = std::chrono::steady_clock::now();
    for (const TestCase& test : registry()) {
        if (!filter.empty() && std::string_view(test.name).find(filter) == std::string_view::npos) continue;
        ++ranTests;
        g_failuresInCurrentTest = 0;
        std::printf("[ RUN  ] %s\n", test.name);
        std::fflush(stdout);
        const auto start = std::chrono::steady_clock::now();
        try {
            test.function();
        } catch (const AbortTest&) {
            // already reported by VT_REQUIRE
        } catch (const std::exception& error) {
            reportFailure(__FILE__, __LINE__, std::string("unexpected exception: ") + error.what());
        } catch (...) {
            reportFailure(__FILE__, __LINE__, "unexpected non-standard exception");
        }
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        if (g_failuresInCurrentTest == 0) {
            std::printf("[  OK  ] %s (%.1f ms)\n", test.name, ms);
        } else {
            ++failedTests;
            std::printf("[ FAIL ] %s (%.1f ms)\n", test.name, ms);
        }
        std::fflush(stdout);
    }
    const double total =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - suiteStart).count();
    std::printf("\n%d test(s) run, %d failed (%.0f ms)\n", ranTests, failedTests, total);
    if (ranTests == 0) {
        std::printf("no test matched the filter '%s'\n", filter.c_str());
        return 2;
    }
    return failedTests == 0 ? 0 : 1;
}

// ---------------------------------------------------------------------------
// Fixtures
// ---------------------------------------------------------------------------

TempDir::TempDir(std::string_view label) {
    const char* base = std::getenv("TMPDIR");
    std::string pattern = (base != nullptr && base[0] == '/') ? base : "/tmp";
    if (pattern.back() != '/') pattern.push_back('/');
    pattern.append(label);
    pattern.append(".XXXXXX");
    std::vector<char> buffer(pattern.begin(), pattern.end());
    buffer.push_back('\0');
    if (::mkdtemp(buffer.data()) == nullptr) die("mkdtemp failed for " + pattern);
    char resolved[PATH_MAX];
    if (::realpath(buffer.data(), resolved) == nullptr) die("realpath failed");
    path_ = resolved;
}

TempDir::~TempDir() {
    if (path_.empty()) return;
    unlockTree(path_);
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
}

std::string TempDir::sub(std::string_view relative) const {
    std::string out = path_;
    if (!relative.empty()) {
        out.push_back('/');
        out.append(relative);
    }
    return out;
}

void makeDirs(const std::string& path) {
    std::error_code ec;
    std::filesystem::create_directories(path, ec);
    if (ec) die("create_directories failed for " + path);
}

void writeFile(const std::string& path, std::string_view content) {
    makeDirs(std::filesystem::path(path).parent_path().string());
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream) die("cannot create " + path);
    stream.write(content.data(), static_cast<std::streamsize>(content.size()));
    if (!stream) die("cannot write " + path);
}

void writeFile(const std::string& path, const std::vector<std::uint8_t>& content) {
    writeFile(path, std::string_view(reinterpret_cast<const char*>(content.data()), content.size()));
}

void writeFileOfSize(const std::string& path, std::size_t bytes, char fill) {
    writeFile(path, std::string(bytes, fill));
}

std::string readFile(const std::string& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) return {};
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    return buffer.str();
}

bool exists(const std::string& path) {
    struct stat st {};
    return ::lstat(path.c_str(), &st) == 0;
}

bool isDir(const std::string& path) {
    struct stat st {};
    return ::lstat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

std::vector<std::string> snapshotTree(const std::string& root) {
    std::vector<std::string> lines;
    std::error_code ec;
    for (std::filesystem::recursive_directory_iterator
             it(root, std::filesystem::directory_options::skip_permission_denied, ec),
         end;
         !ec && it != end; it.increment(ec)) {
        struct stat st {};
        if (::lstat(it->path().c_str(), &st) != 0) continue;
        const char type = S_ISDIR(st.st_mode) ? 'd' : (S_ISLNK(st.st_mode) ? 'l' : 'f');
        std::string line = std::filesystem::relative(it->path(), root, ec).string();
        line += std::string(" [") + type + " " + (S_ISDIR(st.st_mode) ? "-" : std::to_string(st.st_size)) + " " +
                std::to_string(st.st_mtimespec.tv_sec) + "." + std::to_string(st.st_mtimespec.tv_nsec) + "]";
        lines.push_back(std::move(line));
    }
    std::sort(lines.begin(), lines.end());
    return lines;
}

std::string plistXml(const std::vector<std::pair<std::string, std::string>>& stringValues,
                     std::string_view extraXml) {
    std::string xml =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
        "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
        "<plist version=\"1.0\">\n<dict>\n";
    for (const auto& [key, value] : stringValues) {
        xml += "  <key>" + key + "</key>\n  <string>" + value + "</string>\n";
    }
    xml.append(extraXml);
    xml += "</dict>\n</plist>\n";
    return xml;
}

std::string makeAppBundle(const std::string& parentDir, std::string_view fileName, std::string_view bundleId,
                          std::string_view executable, std::string_view extraPlistXml) {
    const std::string bundle = parentDir + "/" + std::string(fileName);
    makeDirs(bundle + "/Contents/MacOS");
    makeDirs(bundle + "/Contents/Resources");
    std::string displayName(fileName);
    if (displayName.size() > 4 && displayName.ends_with(".app")) displayName.resize(displayName.size() - 4);
    writeFile(bundle + "/Contents/Info.plist",
              plistXml(
                  {
                      {"CFBundleIdentifier", std::string(bundleId)},
                      {"CFBundleName", displayName},
                      {"CFBundleExecutable", std::string(executable)},
                      {"CFBundleShortVersionString", "1.2.3"},
                      {"CFBundleVersion", "42"},
                      {"CFBundlePackageType", "APPL"},
                  },
                  extraPlistXml));
    writeFile(bundle + "/Contents/MacOS/" + std::string(executable), "#!/bin/sh\nexit 0\n");
    ::chmod((bundle + "/Contents/MacOS/" + std::string(executable)).c_str(), 0755);
    writeFileOfSize(bundle + "/Contents/Resources/data.bin", 20000);
    return bundle;
}

}  // namespace vt

int main(int argc, char** argv) {
    return vt::runAll(argc, argv);
}
