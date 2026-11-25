// VortexClean core — thin, error-reporting wrappers over POSIX file I/O.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <sys/types.h>
#include <vector>

#include "Result.hpp"

namespace vortex::io {

struct DirEntry {
    std::string name;
    std::uint8_t type = 0;  ///< DT_* constant from <dirent.h> (DT_UNKNOWN when the FS does not say)
};

/// Lists a directory without following a symlink at `path` itself. "." and ".."
/// are omitted. The Error code is the errno of the failing call.
[[nodiscard]] Result<std::vector<DirEntry>> listDirectory(const std::string& path);

/// Reads a regular file completely. Refuses FIFOs, devices and anything larger
/// than `maxBytes` so a hostile path can never block or exhaust memory.
[[nodiscard]] Result<std::vector<std::uint8_t>> readBinaryFile(const std::string& path, std::size_t maxBytes);
[[nodiscard]] Result<std::string> readTextFile(const std::string& path, std::size_t maxBytes);

[[nodiscard]] bool existsNoFollow(const std::string& path) noexcept;
[[nodiscard]] bool isDirectory(const std::string& path) noexcept;          ///< follows symlinks
[[nodiscard]] bool isDirectoryNoFollow(const std::string& path) noexcept;
[[nodiscard]] bool isRegularFile(const std::string& path) noexcept;        ///< follows symlinks

/// mkdir -p. Existing directories are not an error.
[[nodiscard]] Status makeDirectories(const std::string& path, mode_t mode);

/// Creates `path` exclusively (fails if it exists, never follows a symlink),
/// writes `content`, and fsyncs before closing.
[[nodiscard]] Status writeFileExclusive(const std::string& path, std::string_view content, mode_t mode);

/// strerror_r wrapper returning a std::string.
[[nodiscard]] std::string errnoMessage(int errorNumber);

}  // namespace vortex::io
