// VortexClean core — structured JSON audit reports.
//
// One file per uninstall or dry run: ~/Library/Logs/VortexClean/audit-<timestamp>.json
// It records the target's metadata, every path that was attempted, its status
// (trashed, deleted, skipped_sip, skipped_permission, dry_run, error,
// skipped_missing), the bytes reclaimed and any error diagnostics.
#pragma once

#include <cstdint>
#include <string>

#include "DeletionEngine.hpp"
#include "Json.hpp"
#include "Result.hpp"

namespace vortex {

class AuditLogger {
public:
    /// The complete report as a JSON document (schema "vortexclean.audit/1").
    [[nodiscard]] static JsonValue buildDocument(const DeletionRequest& request, const DeletionReport& report);

    /// Writes the report into `directory` (created if necessary) and returns the
    /// path of the new file. The file is created exclusively and never overwrites
    /// an earlier report.
    [[nodiscard]] static Result<std::string> write(const DeletionRequest& request, const DeletionReport& report,
                                                   const std::string& directory);

    /// "2026-11-24T21:38:00.123Z"
    [[nodiscard]] static std::string iso8601(std::int64_t epochMs);
    /// "20261124-213800-123" (local time), used in the file name.
    [[nodiscard]] static std::string fileStamp(std::int64_t epochMs);
};

}  // namespace vortex
