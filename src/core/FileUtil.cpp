#include "FileUtil.hpp"

#include <array>
#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "PathUtil.hpp"

namespace vortex::io {

namespace {

/// Closes a descriptor on scope exit.
class FdGuard {
public:
    explicit FdGuard(int fd) noexcept : fd_(fd) {}
    ~FdGuard() {
        if (fd_ >= 0) ::close(fd_);
    }
    FdGuard(const FdGuard&) = delete;
    FdGuard& operator=(const FdGuard&) = delete;
    [[nodiscard]] int get() const noexcept { return fd_; }
    int release() noexcept {
        const int fd = fd_;
        fd_ = -1;
        return fd;
    }

private:
    int fd_;
};

}  // namespace

std::string errnoMessage(int errorNumber) {
    std::array<char, 256> buffer{};
    if (::strerror_r(errorNumber, buffer.data(), buffer.size()) != 0) {
        return "errno " + std::to_string(errorNumber);
    }
    return std::string(buffer.data());
}

Result<std::vector<DirEntry>> listDirectory(const std::string& path) {
    const int fd = ::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) {
        const int err = errno;
        return Error{err, "cannot open directory '" + path + "': " + errnoMessage(err)};
    }
    DIR* dir = ::fdopendir(fd);
    if (dir == nullptr) {
        const int err = errno;
        ::close(fd);
        return Error{err, "cannot read directory '" + path + "': " + errnoMessage(err)};
    }
    std::vector<DirEntry> entries;
    for (;;) {
        errno = 0;
        const struct dirent* entry = ::readdir(dir);
        if (entry == nullptr) {
            const int err = errno;
            ::closedir(dir);
            if (err != 0) {
                return Error{err, "error while reading directory '" + path + "': " + errnoMessage(err)};
            }
            break;
        }
        const std::string_view name(entry->d_name);
        if (name == "." || name == "..") continue;
        entries.push_back(DirEntry{std::string(name), entry->d_type});
    }
    return entries;
}

Result<std::vector<std::uint8_t>> readBinaryFile(const std::string& path, std::size_t maxBytes) {
    // O_NONBLOCK keeps open(2) from hanging on a FIFO; the S_ISREG check below
    // then rejects anything that is not a plain file.
    const FdGuard fd(::open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC));
    if (fd.get() < 0) {
        const int err = errno;
        return Error{err, "cannot open '" + path + "': " + errnoMessage(err)};
    }
    struct stat st {};
    if (::fstat(fd.get(), &st) != 0) {
        const int err = errno;
        return Error{err, "cannot stat '" + path + "': " + errnoMessage(err)};
    }
    if (!S_ISREG(st.st_mode)) {
        return Error{EINVAL, "'" + path + "' is not a regular file"};
    }
    if (st.st_size < 0 || static_cast<std::uint64_t>(st.st_size) > maxBytes) {
        return Error{EFBIG, "'" + path + "' exceeds the size limit of " + std::to_string(maxBytes) + " bytes"};
    }
    std::vector<std::uint8_t> data(static_cast<std::size_t>(st.st_size));
    std::size_t offset = 0;
    while (offset < data.size()) {
        const ssize_t got = ::read(fd.get(), data.data() + offset, data.size() - offset);
        if (got < 0) {
            if (errno == EINTR) continue;
            const int err = errno;
            return Error{err, "cannot read '" + path + "': " + errnoMessage(err)};
        }
        if (got == 0) break;  // file shrank underneath us; keep what we have
        offset += static_cast<std::size_t>(got);
    }
    data.resize(offset);
    return data;
}

Result<std::string> readTextFile(const std::string& path, std::size_t maxBytes) {
    auto bytes = readBinaryFile(path, maxBytes);
    if (!bytes) return bytes.error();
    const auto& data = bytes.value();
    return std::string(reinterpret_cast<const char*>(data.data()), data.size());
}

bool existsNoFollow(const std::string& path) noexcept {
    struct stat st {};
    return ::lstat(path.c_str(), &st) == 0;
}

bool isDirectory(const std::string& path) noexcept {
    struct stat st {};
    return ::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool isDirectoryNoFollow(const std::string& path) noexcept {
    struct stat st {};
    return ::lstat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool isRegularFile(const std::string& path) noexcept {
    struct stat st {};
    return ::stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

Status makeDirectories(const std::string& path, mode_t mode) {
    const std::string normalized = path::normalize(path);
    if (normalized.empty() || normalized == "." || normalized == "/") return success();
    std::string current;
    if (normalized.front() == '/') current = "/";
    for (const std::string_view part : path::components(normalized)) {
        current = path::join(current, part);
        if (::mkdir(current.c_str(), mode) == 0) continue;
        const int err = errno;
        if (err == EEXIST || err == EISDIR) {
            if (isDirectory(current)) continue;
            return failure(ENOTDIR, "'" + current + "' exists and is not a directory");
        }
        return failure(err, "cannot create directory '" + current + "': " + errnoMessage(err));
    }
    return success();
}

Status writeFileExclusive(const std::string& path, std::string_view content, mode_t mode) {
    FdGuard fd(::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, mode));
    if (fd.get() < 0) {
        const int err = errno;
        return failure(err, "cannot create '" + path + "': " + errnoMessage(err));
    }
    std::size_t offset = 0;
    while (offset < content.size()) {
        const ssize_t wrote = ::write(fd.get(), content.data() + offset, content.size() - offset);
        if (wrote < 0) {
            if (errno == EINTR) continue;
            const int err = errno;
            ::unlink(path.c_str());
            return failure(err, "cannot write '" + path + "': " + errnoMessage(err));
        }
        offset += static_cast<std::size_t>(wrote);
    }
    if (::fsync(fd.get()) != 0 && errno != EINVAL && errno != ENOTSUP) {
        const int err = errno;
        return failure(err, "cannot flush '" + path + "': " + errnoMessage(err));
    }
    if (::close(fd.release()) != 0) {
        const int err = errno;
        return failure(err, "cannot close '" + path + "': " + errnoMessage(err));
    }
    return success();
}

}  // namespace vortex::io
