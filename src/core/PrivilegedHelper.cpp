#include "PrivilegedHelper.hpp"

#include <array>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <fcntl.h>
#include <pwd.h>
#include <sys/stat.h>
#include <sys/stdio.h>
#include <unistd.h>

#include "FileUtil.hpp"
#include "PathPolicy.hpp"
#include "PathUtil.hpp"
#include "StringUtil.hpp"
#include "Version.hpp"

namespace vortex::privileged {

namespace {

constexpr int kProtocolVersion = 1;

class Fd {
public:
    explicit Fd(int fd) noexcept : fd_(fd) {}
    ~Fd() {
        if (fd_ >= 0) ::close(fd_);
    }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    [[nodiscard]] int get() const noexcept { return fd_; }
    [[nodiscard]] bool valid() const noexcept { return fd_ >= 0; }

private:
    int fd_;
};

void fail(ItemOutcome& outcome, ItemStatus status, int code, std::string message) {
    outcome.status = status;
    outcome.errorCode = code;
    outcome.errorMessage = std::move(message);
}

void appendNote(ItemOutcome& outcome, std::string_view text) {
    if (text.empty()) return;
    if (!outcome.note.empty()) outcome.note += "; ";
    outcome.note.append(text);
}

/// "Foo.app" -> "Foo 2.app", "notes" -> "notes 2".
std::string numberedName(std::string_view name, int number) {
    const std::string_view extension = path::extension(name);
    const std::string suffix = " " + std::to_string(number);
    if (extension.empty()) return std::string(name) + suffix;
    return std::string(path::stem(name)) + suffix + "." + std::string(extension);
}

/// Moves one entry into the invoking user's Trash, like Finder does after
/// authenticating: the item keeps its ownership, nothing is chown'ed, and the
/// rename is exclusive so it can never replace something that is already there.
Result<std::string> moveToInvokerTrash(const std::string& itemPath, const HelperPolicy& policy) {
    const std::string parent = path::parent(itemPath);
    const std::string name(path::fileName(itemPath));

    const Fd parentFd(::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW_ANY));
    if (!parentFd.valid()) {
        const int error = errno;
        return Error{error, "cannot open '" + parent + "': " + io::errnoMessage(error)};
    }
    const Fd homeFd(::open(policy.invokerHome.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW_ANY));
    if (!homeFd.valid()) {
        const int error = errno;
        return Error{error, "cannot open the home directory: " + io::errnoMessage(error)};
    }

    int rawTrash = ::openat(homeFd.get(), ".Trash", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (rawTrash < 0 && errno == ENOENT) {
        if (::mkdirat(homeFd.get(), ".Trash", 0700) == 0) {
            rawTrash = ::openat(homeFd.get(), ".Trash", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
            struct stat homeStat {};
            if (rawTrash >= 0 && ::fstat(homeFd.get(), &homeStat) == 0) {
                ::fchown(rawTrash, policy.invokerUid, homeStat.st_gid);
            }
        }
    }
    const Fd trashFd(rawTrash);
    if (!trashFd.valid()) {
        const int error = errno;
        return Error{error, "cannot open the Trash: " + io::errnoMessage(error)};
    }
    struct stat trashStat {};
    if (::fstat(trashFd.get(), &trashStat) != 0 || !S_ISDIR(trashStat.st_mode) ||
        trashStat.st_uid != policy.invokerUid) {
        return Error{EPERM, "the Trash folder does not belong to the user who authorised this operation"};
    }

    for (int attempt = 1; attempt <= 1000; ++attempt) {
        const std::string destination = attempt == 1 ? name : numberedName(name, attempt);
        if (::renameatx_np(parentFd.get(), name.c_str(), trashFd.get(), destination.c_str(), RENAME_EXCL) == 0) {
            return path::join(policy.trashDirectory(), destination);
        }
        const int error = errno;
        if (error == EEXIST) continue;
        if (error == EXDEV) {
            return Error{error, "the item is on a different volume than the Trash; use permanent deletion instead"};
        }
        return Error{error, "cannot move '" + itemPath + "' to the Trash: " + io::errnoMessage(error)};
    }
    return Error{EEXIST, "could not find a free name in the Trash for '" + name + "'"};
}

std::string serviceTargetFor(const DeletionItem& item, uid_t invokerUid, ItemOutcome& outcome) {
    const bool isDaemon = item.category == RemnantCategory::LaunchDaemons;
    const bool isAgent = item.category == RemnantCategory::LaunchAgents;
    if ((!isDaemon && !isAgent) || item.launchdLabel.empty()) return {};
    if (!LaunchControl::isValidLabel(item.launchdLabel)) {
        appendNote(outcome, "launchd label has unusual characters; the service was not booted out");
        return {};
    }
    return isDaemon ? LaunchControl::systemServiceTarget(item.launchdLabel)
                    : LaunchControl::userServiceTarget(invokerUid, item.launchdLabel);
}

}  // namespace

// ---------------------------------------------------------------------------
// Wire format
// ---------------------------------------------------------------------------

JsonValue encodeRequest(const PrivilegedRequest& request) {
    JsonValue json = JsonValue::makeObject();
    json.set("protocol", kProtocolVersion);
    json.set("mode", jsonKey(request.mode));
    json.set("invoker_uid", static_cast<std::int64_t>(request.invokerUid));
    JsonValue items = JsonValue::makeArray();
    for (const DeletionItem& item : request.items) {
        JsonValue entry = JsonValue::makeObject();
        entry.set("path", item.path);
        entry.set("category", jsonKey(item.category));
        entry.set("launchd_label", item.launchdLabel);
        items.push(std::move(entry));
    }
    json.set("items", std::move(items));
    return json;
}

Result<PrivilegedRequest> decodeRequest(const JsonValue& json) {
    if (!json.isObject()) return Error{EINVAL, "request is not a JSON object"};
    if (json["protocol"].asInteger(-1) != kProtocolVersion) return Error{EINVAL, "unsupported protocol version"};

    PrivilegedRequest request;
    if (!parseDeleteMode(json["mode"].asString(), request.mode)) return Error{EINVAL, "invalid mode"};
    const std::int64_t uid = json["invoker_uid"].asInteger(-1);
    if (uid < 0 || uid > 0x7FFFFFFF) return Error{EINVAL, "invalid invoker uid"};
    request.invokerUid = static_cast<uid_t>(uid);

    const JsonValue& items = json["items"];
    if (!items.isArray()) return Error{EINVAL, "items is not an array"};
    for (const JsonValue& entry : items.asArray()) {
        if (!entry.isObject() || !entry["path"].isString()) return Error{EINVAL, "malformed item"};
        DeletionItem item;
        item.path = entry["path"].asString();
        if (!parseCategory(entry["category"].asString(), item.category)) return Error{EINVAL, "invalid category"};
        item.launchdLabel = entry["launchd_label"].asString();
        request.items.push_back(std::move(item));
    }
    return request;
}

JsonValue encodeResponse(const PrivilegedResponse& response) {
    JsonValue json = JsonValue::makeObject();
    json.set("protocol", kProtocolVersion);
    json.set("helper_pid", static_cast<std::int64_t>(response.helperPid));
    json.set("error", response.error);
    JsonValue outcomes = JsonValue::makeArray();
    for (const ItemOutcome& outcome : response.outcomes) {
        JsonValue entry = JsonValue::makeObject();
        entry.set("path", outcome.item.path);
        entry.set("category", jsonKey(outcome.item.category));
        entry.set("status", jsonKey(outcome.status));
        entry.set("error_code", outcome.errorCode);
        entry.set("error_message", outcome.errorMessage);
        entry.set("trash_path", outcome.trashPath);
        entry.set("note", outcome.note);
        entry.set("launchctl_target", outcome.launchctlTarget);
        entry.set("launchctl_ran", outcome.launchctlRan);
        entry.set("launchctl_exit", outcome.launchctlExit);
        outcomes.push(std::move(entry));
    }
    json.set("outcomes", std::move(outcomes));
    return json;
}

Result<PrivilegedResponse> decodeResponse(const JsonValue& json) {
    if (!json.isObject()) return Error{EINVAL, "response is not a JSON object"};
    if (json["protocol"].asInteger(-1) != kProtocolVersion) return Error{EINVAL, "unsupported protocol version"};

    PrivilegedResponse response;
    response.helperPid = static_cast<pid_t>(json["helper_pid"].asInteger(0));
    response.error = json["error"].asString();
    const JsonValue& outcomes = json["outcomes"];
    if (!outcomes.isArray()) return Error{EINVAL, "outcomes is not an array"};
    for (const JsonValue& entry : outcomes.asArray()) {
        if (!entry.isObject() || !entry["path"].isString()) return Error{EINVAL, "malformed outcome"};
        ItemOutcome outcome;
        outcome.item.path = entry["path"].asString();
        if (!parseCategory(entry["category"].asString(), outcome.item.category)) {
            return Error{EINVAL, "invalid category in outcome"};
        }
        if (!parseItemStatus(entry["status"].asString(), outcome.status)) {
            return Error{EINVAL, "invalid status in outcome"};
        }
        outcome.errorCode = static_cast<int>(entry["error_code"].asInteger(0));
        outcome.errorMessage = entry["error_message"].asString();
        outcome.trashPath = entry["trash_path"].asString();
        outcome.note = entry["note"].asString();
        outcome.launchctlTarget = entry["launchctl_target"].asString();
        outcome.launchctlRan = entry["launchctl_ran"].asBool(false);
        outcome.launchctlExit = static_cast<int>(entry["launchctl_exit"].asInteger(-1));
        outcome.elevated = true;
        outcome.requiresRoot = true;
        response.outcomes.push_back(std::move(outcome));
    }
    return response;
}

Status writeFrame(std::FILE* stream, std::string_view payload) {
    if (stream == nullptr) return failure(EBADF, "no stream");
    const std::string header = std::to_string(payload.size()) + "\n";
    if (std::fwrite(header.data(), 1, header.size(), stream) != header.size() ||
        (!payload.empty() && std::fwrite(payload.data(), 1, payload.size(), stream) != payload.size()) ||
        std::fflush(stream) != 0) {
        return failure(EIO, "cannot write to the helper pipe");
    }
    return success();
}

Result<std::string> readFrame(std::FILE* stream, std::size_t maxBytes) {
    if (stream == nullptr) return Error{EBADF, "no stream"};
    // Header: up to 20 decimal digits terminated by a newline.
    std::size_t length = 0;
    int digits = 0;
    for (;;) {
        const int ch = std::fgetc(stream);
        if (ch == EOF) return Error{EIO, "the helper pipe closed before a complete message arrived"};
        if (ch == '\n') break;
        if (ch < '0' || ch > '9' || ++digits > 20) return Error{EPROTO, "malformed message header"};
        length = length * 10 + static_cast<std::size_t>(ch - '0');
        if (length > maxBytes) return Error{EMSGSIZE, "message exceeds the size limit"};
    }
    if (digits == 0) return Error{EPROTO, "malformed message header"};
    std::string payload(length, '\0');
    if (length != 0 && std::fread(payload.data(), 1, length, stream) != length) {
        return Error{EIO, "the helper pipe closed in the middle of a message"};
    }
    return payload;
}

// ---------------------------------------------------------------------------
// Policy
// ---------------------------------------------------------------------------

std::string HelperPolicy::trashDirectory() const {
    return path::join(invokerHome, ".Trash");
}

Result<HelperPolicy> HelperPolicy::production(uid_t invokerUid) {
    const long suggested = ::sysconf(_SC_GETPW_R_SIZE_MAX);
    std::vector<char> buffer(suggested > 0 ? static_cast<std::size_t>(suggested) : 4096);
    struct passwd entry {};
    struct passwd* found = nullptr;
    if (::getpwuid_r(invokerUid, &entry, buffer.data(), buffer.size(), &found) != 0 || found == nullptr ||
        found->pw_dir == nullptr || found->pw_dir[0] != '/') {
        return Error{ENOENT, "cannot determine the home directory of user " + std::to_string(invokerUid)};
    }
    HelperPolicy policy;
    policy.systemRoot = "/";
    policy.invokerUid = invokerUid;
    policy.invokerHome = path::normalize(found->pw_dir);
    if (const auto canonical = path::canonical(policy.invokerHome)) policy.invokerHome = *canonical;
    return policy;
}

bool isLocationAllowed(const HelperPolicy& policy, const std::string& absolutePath) {
    static constexpr auto kSystemLocations = std::to_array<std::string_view>({
        "/Applications",
        "/Library",
        "/usr/local",
        "/opt",
        "/Users/Shared",
        "/private/var/db/receipts",
    });
    // With a relocated root (tests) nothing outside that root is ever in scope.
    if (policy.systemRoot != "/" && !path::isStrictlyWithin(absolutePath, policy.systemRoot)) return false;
    const PathPolicy pathPolicy(policy.invokerHome, policy.systemRoot);
    const std::string logical = pathPolicy.logicalPath(absolutePath);
    for (const std::string_view location : kSystemLocations) {
        if (path::iIsStrictlyWithin(logical, location)) return true;
    }
    if (!policy.invokerHome.empty()) {
        const std::string home = pathPolicy.logicalPath(policy.invokerHome);
        for (const std::string_view sub : {std::string_view("Library"), std::string_view("Applications")}) {
            if (path::iIsStrictlyWithin(logical, path::join(home, sub))) return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// Execution
// ---------------------------------------------------------------------------

std::vector<ItemOutcome> execute(const PrivilegedRequest& request, const HelperPolicy& policy,
                                 const HelperHooks& hooks) {
    const PathPolicy pathPolicy(policy.invokerHome, policy.systemRoot);
    std::vector<ItemOutcome> outcomes;
    outcomes.reserve(request.items.size());

    for (const DeletionItem& item : request.items) {
        ItemOutcome outcome;
        outcome.item = item;
        outcome.elevated = true;
        outcome.requiresRoot = true;

        // The request is validated from scratch: the helper trusts nothing it is told.
        const Protection protection = pathPolicy.classify(item.path);
        if (protection == Protection::Invalid) {
            fail(outcome, ItemStatus::Error, EINVAL, "refused: not an absolute, normalised path");
        } else if (protection == Protection::ProtectedRoot) {
            fail(outcome, ItemStatus::Error, EPERM, "refused: protected shared directory");
        } else if (protection == Protection::SipProtected) {
            fail(outcome, ItemStatus::SkippedSip, EPERM, "protected by System Integrity Protection");
        } else if (!isLocationAllowed(policy, item.path)) {
            fail(outcome, ItemStatus::Error, EPERM,
                 "refused: outside the locations the privileged helper may modify");
        } else {
            struct stat st {};
            if (::lstat(item.path.c_str(), &st) != 0) {
                const int error = errno;
                if (error == ENOENT || error == ENOTDIR) {
                    fail(outcome, ItemStatus::SkippedMissing, error, "the item no longer exists");
                } else {
                    fail(outcome, ItemStatus::Error, error, "cannot examine the item: " + io::errnoMessage(error));
                }
            } else {
                if (const std::string target = serviceTargetFor(item, policy.invokerUid, outcome); !target.empty()) {
                    const LaunchctlResult booted = hooks.bootout ? hooks.bootout(target) : LaunchControl::bootout(target);
                    outcome.launchctlTarget = target;
                    outcome.launchctlRan = booted.spawned;
                    outcome.launchctlExit = booted.exitStatus;
                    appendNote(outcome, booted.detail);
                }
                if (request.mode == DeleteMode::Permanent) {
                    const Status removed = DeletionEngine::removeTree(item.path);
                    if (removed) {
                        outcome.status = ItemStatus::Deleted;
                    } else {
                        const int error = removed.error().code;
                        // Root is only ever refused by SIP, TCC or a system file flag.
                        fail(outcome, error == EPERM ? ItemStatus::SkippedPermission : ItemStatus::Error, error,
                             removed.error().message);
                    }
                } else {
                    auto moved = moveToInvokerTrash(item.path, policy);
                    if (moved) {
                        outcome.status = ItemStatus::Trashed;
                        outcome.trashPath = moved.value();
                        appendNote(outcome, "moved to the Trash with its original ownership");
                    } else {
                        const int error = moved.error().code;
                        fail(outcome, error == EPERM ? ItemStatus::SkippedPermission : ItemStatus::Error, error,
                             moved.error().message);
                    }
                }
            }
        }
        outcomes.push_back(std::move(outcome));
    }
    return outcomes;
}

int runHelperMain(std::FILE* in, std::FILE* out) {
    ::signal(SIGPIPE, SIG_IGN);
    ::umask(022);

    PrivilegedResponse response;
    response.helperPid = ::getpid();
    const auto reply = [&response, out](int exitStatus) {
        const Status written = writeFrame(out, encodeResponse(response).dump(-1));
        return written ? exitStatus : EXIT_FAILURE;
    };

    if (::geteuid() != 0) {
        response.error = "the privileged helper must be started through administrator authorization";
        return reply(EXIT_FAILURE);
    }
    // Authorization Services starts the tool with an effective uid of 0 while the
    // real uid is still the user who authenticated. That real uid is the only
    // trustworthy statement of who asked; remember it, then become root outright
    // so that launchctl (which inspects the real uid) can reach the system domain.
    const uid_t realUid = ::getuid();
    if (::setuid(0) != 0) {
        response.error = "cannot assume root privileges";
        return reply(EXIT_FAILURE);
    }

    auto frame = readFrame(in);
    if (!frame) {
        response.error = frame.error().message;
        return reply(EXIT_FAILURE);
    }
    auto json = JsonValue::parse(frame.value());
    if (!json) {
        response.error = json.error().message;
        return reply(EXIT_FAILURE);
    }
    auto request = decodeRequest(json.value());
    if (!request) {
        response.error = request.error().message;
        return reply(EXIT_FAILURE);
    }
    if (realUid != 0) request.value().invokerUid = realUid;

    auto policy = HelperPolicy::production(request.value().invokerUid);
    if (!policy) {
        response.error = policy.error().message;
        return reply(EXIT_FAILURE);
    }
    response.outcomes = execute(request.value(), policy.value());
    return reply(EXIT_SUCCESS);
}

}  // namespace vortex::privileged
