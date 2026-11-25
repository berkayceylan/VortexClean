#import "VCSystemHooks.h"

#import <Foundation/Foundation.h>
#import <Security/Authorization.h>
#import <Security/AuthorizationTags.h>
#import <Security/Security.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <sys/wait.h>
#include <unistd.h>

#include "FileUtil.hpp"
#include "Json.hpp"
#include "PathUtil.hpp"
#include "PrivilegedHelper.hpp"
#include "Version.hpp"

namespace vcbridge {

namespace {

/// errno equivalent of a Cocoa file error, so the engine can reason about it.
int posixCodeForError(NSError *error) {
    if (error == nil) return EIO;
    if ([error.domain isEqualToString:NSPOSIXErrorDomain]) return static_cast<int>(error.code);
    NSError *underlying = error.userInfo[NSUnderlyingErrorKey];
    if ([underlying isKindOfClass:[NSError class]] && [underlying.domain isEqualToString:NSPOSIXErrorDomain]) {
        return static_cast<int>(underlying.code);
    }
    if ([error.domain isEqualToString:NSCocoaErrorDomain]) {
        switch (error.code) {
            case NSFileNoSuchFileError:
            case NSFileReadNoSuchFileError: return ENOENT;
            case NSFileReadNoPermissionError:
            case NSFileWriteNoPermissionError: return EACCES;
            case NSFileWriteVolumeReadOnlyError: return EROFS;
            case NSFileWriteOutOfSpaceError: return ENOSPC;
            case NSFileWriteFileExistsError: return EEXIST;
            case NSFeatureUnsupportedError: return ENOTSUP;
            default: break;
        }
    }
    return EIO;
}

// AuthorizationExecuteWithPrivileges has been deprecated since OS X 10.7 in
// favour of SMJobBless/SMAppService. Those replacements require a Developer ID
// signed app and helper, so they fail by design for a locally built, ad-hoc
// signed copy. The symbol is therefore resolved at run time: nothing is
// referenced at link time, and its absence is detected and reported instead of
// crashing.
using ExecuteWithPrivilegesFn = OSStatus (*)(AuthorizationRef, const char *, AuthorizationFlags, char *const *,
                                             FILE **);

ExecuteWithPrivilegesFn executeWithPrivileges() {
    static const ExecuteWithPrivilegesFn function =
        reinterpret_cast<ExecuteWithPrivilegesFn>(::dlsym(RTLD_DEFAULT, "AuthorizationExecuteWithPrivileges"));
    return function;
}

std::string describeStatus(OSStatus status) {
    NSString *message = CFBridgingRelease(SecCopyErrorMessageString(status, nullptr));
    if (message.length > 0) return std::string(message.UTF8String);
    return "error " + std::to_string(static_cast<int>(status));
}

}  // namespace

vortex::DeletionHooks sandboxHooks(const std::string& home) {
    vortex::DeletionHooks hooks;
    hooks.moveToTrash = [home](const std::string& path) -> vortex::Result<std::string> {
        const std::string trash = vortex::path::join(home, ".Trash");
        if (auto made = vortex::io::makeDirectories(trash, 0700); !made) return made.error();
        const std::string name(vortex::path::fileName(path));
        for (int attempt = 1; attempt < 10000; ++attempt) {
            const std::string target =
                vortex::path::join(trash, attempt == 1 ? name : name + " " + std::to_string(attempt));
            if (::renamex_np(path.c_str(), target.c_str(), RENAME_EXCL) == 0) return target;
            if (errno != EEXIST) return vortex::Error{errno, "cannot move '" + path + "' to the sandbox Trash"};
        }
        return vortex::Error{EEXIST, "no free name in the sandbox Trash for '" + name + "'"};
    };
    hooks.bootout = [](const std::string& serviceTarget) {
        vortex::LaunchctlResult result;
        result.spawned = true;
        result.exitStatus = 0;
        result.serviceTarget = serviceTarget;
        result.detail = "sandbox: launchd was not contacted";
        return result;
    };
    return hooks;
}

vortex::Result<std::string> moveToTrash(const std::string& path) {
    @autoreleasepool {
        NSString *nsPath = [[NSFileManager defaultManager] stringWithFileSystemRepresentation:path.c_str()
                                                                                       length:path.size()];
        if (nsPath == nil) return vortex::Error{EINVAL, "the path is not valid"};
        NSURL *url = [NSURL fileURLWithPath:nsPath];
        NSURL *resulting = nil;
        NSError *error = nil;
        if ([[NSFileManager defaultManager] trashItemAtURL:url resultingItemURL:&resulting error:&error]) {
            const char *destination = resulting.fileSystemRepresentation;
            return std::string(destination != nullptr ? destination : "");
        }
        NSString *description = error.localizedDescription;
        if (description.length == 0) description = @"The item could not be moved to the Trash.";
        return vortex::Error{posixCodeForError(error), std::string(description.UTF8String)};
    }
}

bool isElevationAvailable() {
    return executeWithPrivileges() != nullptr;
}

vortex::PrivilegedBatchResult runPrivilegedBatch(const std::vector<vortex::DeletionItem>& items,
                                                 vortex::DeleteMode mode) {
    vortex::PrivilegedBatchResult result;
    if (items.empty()) {
        result.authorized = true;
        return result;
    }
    @autoreleasepool {
        const ExecuteWithPrivilegesFn execute = executeWithPrivileges();
        if (execute == nullptr) {
            result.error = "Administrator elevation is not available on this version of macOS. "
                           "The listed items must be removed manually.";
            return result;
        }
        NSString *executable = NSBundle.mainBundle.executablePath;
        if (executable.length == 0) {
            result.error = "Cannot locate the VortexClean executable to start the privileged helper.";
            return result;
        }

        AuthorizationRef authorization = nullptr;
        OSStatus status = AuthorizationCreate(nullptr, kAuthorizationEmptyEnvironment, kAuthorizationFlagDefaults,
                                              &authorization);
        if (status != errAuthorizationSuccess || authorization == nullptr) {
            result.error = "Cannot create an authorization session: " + describeStatus(status);
            return result;
        }

        NSString *prompt = [NSString
            stringWithFormat:@"VortexClean needs administrator privileges to remove %lu %@ that belong%@ to the system.",
                             static_cast<unsigned long>(items.size()), items.size() == 1 ? @"item" : @"items",
                             items.size() == 1 ? @"s" : @""];
        const char *promptText = prompt.UTF8String;
        AuthorizationItem environmentItems[] = {
            {kAuthorizationEnvironmentPrompt, std::strlen(promptText), const_cast<char *>(promptText), 0},
        };
        const AuthorizationEnvironment environment = {1, environmentItems};
        AuthorizationItem rightItems[] = {{kAuthorizationRightExecute, 0, nullptr, 0}};
        const AuthorizationRights rights = {1, rightItems};
        const AuthorizationFlags flags = static_cast<AuthorizationFlags>(
            kAuthorizationFlagInteractionAllowed | kAuthorizationFlagExtendRights | kAuthorizationFlagPreAuthorize);

        status = AuthorizationCopyRights(authorization, &rights, &environment, flags, nullptr);
        if (status != errAuthorizationSuccess) {
            AuthorizationFree(authorization, kAuthorizationFlagDestroyRights);
            result.error = status == errAuthorizationCanceled
                               ? std::string("Administrator authorization was cancelled.")
                               : "Administrator authorization failed: " + describeStatus(status);
            return result;
        }

        std::string toolPath = executable.fileSystemRepresentation;
        std::string helperArgument = vortex::privileged::kHelperArgument;
        char *const arguments[] = {helperArgument.data(), nullptr};
        FILE *channel = nullptr;
        status = execute(authorization, toolPath.c_str(), kAuthorizationFlagDefaults, arguments, &channel);
        if (status != errAuthorizationSuccess || channel == nullptr) {
            AuthorizationFree(authorization, kAuthorizationFlagDestroyRights);
            result.error = "The privileged helper could not be started: " + describeStatus(status);
            return result;
        }
        result.authorized = true;

        vortex::PrivilegedRequest request;
        request.mode = mode;
        request.invokerUid = ::getuid();
        request.items = items;

        pid_t helperPid = 0;
        const vortex::Status sent =
            vortex::privileged::writeFrame(channel, vortex::privileged::encodeRequest(request).dump(-1));
        if (!sent) {
            result.error = sent.error().message;
        } else {
            auto frame = vortex::privileged::readFrame(channel);
            if (!frame) {
                result.error = frame.error().message;
            } else {
                auto json = vortex::JsonValue::parse(frame.value());
                auto response = json ? vortex::privileged::decodeResponse(json.value())
                                     : vortex::Result<vortex::PrivilegedResponse>(json.error());
                if (!response) {
                    result.error = "The privileged helper returned an unreadable answer: " + response.error().message;
                } else {
                    helperPid = response.value().helperPid;
                    result.error = response.value().error;
                    result.outcomes = std::move(response.value().outcomes);
                }
            }
        }
        std::fclose(channel);

        // The helper is a child of this process; collect it so it does not linger as a zombie.
        if (helperPid > 0) {
            int childStatus = 0;
            while (::waitpid(helperPid, &childStatus, 0) < 0 && errno == EINTR) {
            }
        }
        AuthorizationFree(authorization, kAuthorizationFlagDestroyRights);
    }
    return result;
}

}  // namespace vcbridge
