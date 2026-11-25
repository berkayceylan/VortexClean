// Developer tool: builds a synthetic disk full of fictional applications.
//
//   vortex_make_demo_disk <new directory> <any Mach-O executable>
//   VortexClean --root <new directory>
//
// The result is a miniature macOS file system (Applications, Library, a home
// folder for the user "demo", package receipts with real BOM files) on which the
// real engine can be tried, and from which every published screenshot is taken.
// All names are invented. Large files are APFS clones of one another, so a disk
// that appears to hold several gigabytes occupies a few dozen megabytes.
#import <CoreGraphics/CoreGraphics.h>
#import <Foundation/Foundation.h>
#import <ImageIO/ImageIO.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <sys/clonefile.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>

#include "BomBuilder.hpp"

namespace {

constexpr std::uint64_t kKB = 1000;
constexpr std::uint64_t kMB = 1000 * kKB;
constexpr std::uint64_t kBlobBytes = 32 * kMB;

std::string gRoot;  // the directory that plays the role of "/"
std::string gBlob;  // the file everything large is cloned from

[[noreturn]] void die(const std::string &what) {
    std::fprintf(stderr, "vortex_make_demo_disk: %s: %s\n", what.c_str(), std::strerror(errno));
    std::exit(1);
}

std::string at(const std::string &logical) { return gRoot + logical; }

void makeDirectory(const std::string &logical) {
    const std::string path = at(logical);
    for (std::size_t slash = path.find('/', 1);; slash = path.find('/', slash + 1)) {
        const std::string prefix = path.substr(0, slash);
        if (::mkdir(prefix.c_str(), 0755) != 0 && errno != EEXIST) die(prefix);
        if (slash == std::string::npos) return;
    }
}

void makeParent(const std::string &logical) { makeDirectory(logical.substr(0, logical.rfind('/'))); }

void writeBytes(const std::string &logical, const void *data, std::size_t size, mode_t mode = 0644) {
    makeParent(logical);
    const int fd = ::open(at(logical).c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, mode);
    if (fd < 0) die(logical);
    const auto *bytes = static_cast<const char *>(data);
    while (size > 0) {
        const ssize_t written = ::write(fd, bytes, size);
        if (written <= 0) die(logical);
        bytes += written;
        size -= static_cast<std::size_t>(written);
    }
    ::close(fd);
}

void writeText(const std::string &logical, const std::string &text) { writeBytes(logical, text.data(), text.size()); }

/// A directory that occupies about `size` bytes on disk.
void fill(const std::string &directory, std::uint64_t size) {
    makeDirectory(directory);
    for (int part = 1; size > 0; ++part) {
        const std::uint64_t chunk = std::min(size, kBlobBytes);
        const std::string file = at(directory + "/data-" + std::to_string(part) + ".bin");
        if (::clonefile(gBlob.c_str(), file.c_str(), 0) != 0) die(file);
        if (chunk < kBlobBytes && ::truncate(file.c_str(), static_cast<off_t>(chunk)) != 0) die(file);
        size -= chunk;
    }
}

std::string plist(const std::string &body) {
    return "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
           "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
           "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
           "<plist version=\"1.0\"><dict>\n" + body + "</dict></plist>\n";
}

std::string key(const std::string &name, const std::string &value) {
    return "<key>" + name + "</key><string>" + value + "</string>\n";
}

// ---------------------------------------------------------------------------
// Icons
// ---------------------------------------------------------------------------

enum class Glyph { Hexagon, Lens, Waves, Page, Grid, Frames, Prompt, Sun, Envelope, Bars, Peaks, Spark };

struct Color {
    CGFloat r, g, b;
};

void strokeLine(CGContextRef c, std::vector<CGPoint> points) {
    CGContextAddLines(c, points.data(), points.size());
    CGContextStrokePath(c);
}

void fillRounded(CGContextRef c, CGRect rect, CGFloat radius) {
    CGPathRef path = CGPathCreateWithRoundedRect(rect, radius, radius, nullptr);
    CGContextAddPath(c, path);
    CGContextFillPath(c);
    CGPathRelease(path);
}

/// Draws a glyph inside the unit square centred on the origin (-0.5 ... 0.5).
void drawGlyph(CGContextRef c, Glyph glyph) {
    CGContextSetLineCap(c, kCGLineCapRound);
    CGContextSetLineJoin(c, kCGLineJoinRound);
    CGContextSetLineWidth(c, 0.085);
    const auto polygon = [c](int sides, CGFloat radius, CGFloat phase) {
        for (int i = 0; i < sides; ++i) {
            const CGFloat angle = phase + 2.0 * M_PI * i / sides;
            const CGPoint p = CGPointMake(radius * std::cos(angle), radius * std::sin(angle));
            i == 0 ? CGContextMoveToPoint(c, p.x, p.y) : CGContextAddLineToPoint(c, p.x, p.y);
        }
        CGContextClosePath(c);
    };
    switch (glyph) {
        case Glyph::Hexagon:
            polygon(6, 0.40, M_PI / 6.0);
            CGContextStrokePath(c);
            for (int y = -1; y <= 1; ++y) {
                for (int x = -1; x <= 1; ++x) {
                    if ((x + y) % 2 == 0) CGContextFillRect(c, CGRectMake(x * 0.13 - 0.055, y * 0.13 - 0.055, 0.11, 0.11));
                }
            }
            break;
        case Glyph::Lens:
            CGContextStrokeEllipseInRect(c, CGRectMake(-0.36, -0.36, 0.72, 0.72));
            CGContextFillEllipseInRect(c, CGRectMake(-0.15, -0.15, 0.30, 0.30));
            break;
        case Glyph::Waves:
            for (int row = -1; row <= 1; ++row) {
                const CGFloat y = row * 0.21;
                CGContextMoveToPoint(c, -0.36, y);
                CGContextAddCurveToPoint(c, -0.22, y + 0.16, -0.14, y + 0.16, 0.0, y);
                CGContextAddCurveToPoint(c, 0.14, y - 0.16, 0.22, y - 0.16, 0.36, y);
                CGContextStrokePath(c);
            }
            break;
        case Glyph::Page:
            for (int row = 0; row < 4; ++row) {
                strokeLine(c, {CGPointMake(-0.32, 0.27 - row * 0.18), CGPointMake(row == 3 ? 0.06 : 0.32, 0.27 - row * 0.18)});
            }
            break;
        case Glyph::Grid:
            for (int y = 0; y < 3; ++y) {
                for (int x = 0; x < 3; ++x) {
                    CGContextSetAlpha(c, (x == 0 || y == 2) ? 1.0 : 0.6);
                    fillRounded(c, CGRectMake(-0.37 + x * 0.26, -0.37 + y * 0.26, 0.22, 0.22), 0.04);
                }
            }
            CGContextSetAlpha(c, 1.0);
            break;
        case Glyph::Frames:
            CGContextSetAlpha(c, 0.55);
            fillRounded(c, CGRectMake(-0.24, -0.10, 0.60, 0.42), 0.07);
            CGContextSetAlpha(c, 1.0);
            fillRounded(c, CGRectMake(-0.36, -0.32, 0.60, 0.42), 0.07);
            break;
        case Glyph::Prompt:
            strokeLine(c, {CGPointMake(-0.28, 0.24), CGPointMake(-0.02, 0.02), CGPointMake(-0.28, -0.20)});
            strokeLine(c, {CGPointMake(0.06, -0.24), CGPointMake(0.32, -0.24)});
            break;
        case Glyph::Sun:
            CGContextFillEllipseInRect(c, CGRectMake(-0.17, -0.17, 0.34, 0.34));
            for (int i = 0; i < 8; ++i) {
                const CGFloat a = M_PI * i / 4.0;
                strokeLine(c, {CGPointMake(0.28 * std::cos(a), 0.28 * std::sin(a)), CGPointMake(0.38 * std::cos(a), 0.38 * std::sin(a))});
            }
            break;
        case Glyph::Envelope:
            CGContextStrokeRect(c, CGRectMake(-0.36, -0.25, 0.72, 0.50));
            strokeLine(c, {CGPointMake(-0.34, 0.23), CGPointMake(0.0, -0.04), CGPointMake(0.34, 0.23)});
            break;
        case Glyph::Bars: {
            const CGFloat heights[] = {0.22, 0.46, 0.70, 0.40, 0.58, 0.28};
            for (int i = 0; i < 6; ++i) {
                fillRounded(c, CGRectMake(-0.38 + i * 0.135, -heights[i] / 2.0, 0.085, heights[i]), 0.042);
            }
            break;
        }
        case Glyph::Peaks:
            CGContextAddLines(c, std::vector<CGPoint>{CGPointMake(-0.40, -0.26), CGPointMake(-0.12, 0.28), CGPointMake(0.06, -0.06),
                                                      CGPointMake(0.17, 0.10), CGPointMake(0.40, -0.26)}.data(), 5);
            CGContextClosePath(c);
            CGContextFillPath(c);
            break;
        case Glyph::Spark:
            for (int i = 0; i < 8; ++i) {
                const CGFloat radius = i % 2 == 0 ? 0.40 : 0.13;
                const CGFloat a = M_PI / 2.0 + M_PI * i / 4.0;
                const CGPoint p = CGPointMake(radius * std::cos(a), radius * std::sin(a));
                i == 0 ? CGContextMoveToPoint(c, p.x, p.y) : CGContextAddLineToPoint(c, p.x, p.y);
            }
            CGContextClosePath(c);
            CGContextFillPath(c);
            break;
    }
}

CGImageRef createIcon(size_t pixels, Color top, Color bottom, Glyph glyph, CGColorSpaceRef space) CF_RETURNS_RETAINED;
CGImageRef createIcon(size_t pixels, Color top, Color bottom, Glyph glyph, CGColorSpaceRef space) {
    CGContextRef c = CGBitmapContextCreate(nullptr, pixels, pixels, 8, 0, space,
                                           static_cast<CGBitmapInfo>(kCGImageAlphaPremultipliedLast));
    if (c == nullptr) die("bitmap context");
    const auto size = static_cast<CGFloat>(pixels);
    // The macOS icon grid: an 824-point body inside a 1024-point canvas.
    const CGRect body = CGRectInset(CGRectMake(0, 0, size, size), size * 100.0 / 1024.0, size * 100.0 / 1024.0);
    CGPathRef outline = CGPathCreateWithRoundedRect(body, size * 186.0 / 1024.0, size * 186.0 / 1024.0, nullptr);
    CGContextSaveGState(c);
    CGContextAddPath(c, outline);
    CGContextClip(c);
    const CGFloat components[] = {bottom.r, bottom.g, bottom.b, 1.0, top.r, top.g, top.b, 1.0};
    CGGradientRef gradient = CGGradientCreateWithColorComponents(space, components, nullptr, 2);
    CGContextDrawLinearGradient(c, gradient, CGPointMake(0, CGRectGetMinY(body)), CGPointMake(0, CGRectGetMaxY(body)), 0);
    CGGradientRelease(gradient);
    CGContextRestoreGState(c);
    CGPathRelease(outline);

    CGContextTranslateCTM(c, size / 2.0, size / 2.0);
    CGContextScaleCTM(c, body.size.width * 0.72, body.size.width * 0.72);
    CGContextSetRGBFillColor(c, 1, 1, 1, 0.96);
    CGContextSetRGBStrokeColor(c, 1, 1, 1, 0.96);
    drawGlyph(c, glyph);
    CGImageRef image = CGBitmapContextCreateImage(c);
    CGContextRelease(c);
    return image;
}

void writeIcon(const std::string &logical, Color top, Color bottom, Glyph glyph) {
    makeParent(logical);
    CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    NSURL *url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:at(logical).c_str()]];
    CGImageDestinationRef destination =
        CGImageDestinationCreateWithURL((__bridge CFURLRef)url, CFSTR("com.apple.icns"), 5, nullptr);
    if (destination == nullptr) die(logical);
    for (const size_t pixels : {size_t{32}, size_t{64}, size_t{128}, size_t{256}, size_t{512}}) {
        CGImageRef image = createIcon(pixels, top, bottom, glyph, space);
        NSDictionary *properties = @{(__bridge NSString *)kCGImagePropertyDPIWidth: @72, (__bridge NSString *)kCGImagePropertyDPIHeight: @72};
        CGImageDestinationAddImage(destination, image, (__bridge CFDictionaryRef)properties);
        CGImageRelease(image);
    }
    if (!CGImageDestinationFinalize(destination)) die(logical);
    CFRelease(destination);
    CGColorSpaceRelease(space);
}

// ---------------------------------------------------------------------------
// Applications
// ---------------------------------------------------------------------------

struct Helper {
    std::string relativePath;  // inside Contents/
    std::string identifier;
};

struct Application {
    std::string name;
    std::string identifier;
    std::string version;
    std::string build;
    std::uint64_t size;
    Color top, bottom;
    Glyph glyph;
    std::vector<Helper> helpers{};
};

void makeApplication(const Application &app, const std::string &executable) {
    const std::string contents = "/Applications/" + app.name + ".app/Contents";
    const auto info = [](const std::string &name, const std::string &identifier, const std::string &version,
                         const std::string &build) {
        return plist(key("CFBundleExecutable", name) + key("CFBundleIdentifier", identifier) + key("CFBundleName", name) +
                     key("CFBundleDisplayName", name) + key("CFBundlePackageType", "APPL") + key("CFBundleIconFile", "AppIcon") +
                     key("CFBundleShortVersionString", version) + key("CFBundleVersion", build) +
                     key("LSMinimumSystemVersion", "13.0"));
    };
    const auto install = [&executable](const std::string &logical) {
        makeParent(logical);
        if (::clonefile(executable.c_str(), at(logical).c_str(), 0) != 0 && errno == EXDEV) {
            die("the executable must be on the same volume as the disk");
        }
        ::chmod(at(logical).c_str(), 0755);
    };
    writeText(contents + "/Info.plist", info(app.name, app.identifier, app.version, app.build));
    install(contents + "/MacOS/" + app.name);
    writeIcon(contents + "/Resources/AppIcon.icns", app.top, app.bottom, app.glyph);
    fill(contents + "/Resources/Assets", app.size);
    for (const Helper &helper : app.helpers) {
        const std::string bundle = contents + "/" + helper.relativePath;
        const std::size_t slash = bundle.rfind('/');
        const std::string stem = bundle.substr(slash + 1, bundle.size() - slash - 5);
        writeText(bundle + "/Contents/Info.plist", info(stem, helper.identifier, app.version, app.build));
        install(bundle + "/Contents/MacOS/" + stem);
    }
}

std::string launchdJob(const std::string &label, const std::string &program) {
    return plist(key("Label", label) + "<key>ProgramArguments</key><array><string>" + program +
                 "</string></array>\n<key>RunAtLoad</key><true/>\n");
}

void makeReadOnly(const std::string &logical) {
    if (::chmod(at(logical).c_str(), 0555) != 0) die(logical);
}

}  // namespace

int main(int argc, const char *argv[]) {
    @autoreleasepool {
        if (argc != 3) {
            std::fprintf(stderr, "usage: %s <new directory> <any Mach-O executable>\n", argv[0]);
            return 2;
        }
        gRoot = argv[1];
        const std::string executable = argv[2];
        if (gRoot.empty() || gRoot.front() != '/') {
            std::fprintf(stderr, "vortex_make_demo_disk: the directory must be an absolute path\n");
            return 2;
        }
        // Only ever into a directory this run creates itself.
        if (::mkdir(gRoot.c_str(), 0755) != 0) die(gRoot);

        gBlob = gRoot + "/.blob";
        {
            std::vector<std::uint8_t> blob(kBlobBytes);
            std::uint32_t state = 0x2545F491;
            for (std::uint8_t &byte : blob) {
                state ^= state << 13;
                state ^= state >> 17;
                state ^= state << 5;
                byte = static_cast<std::uint8_t>(state);
            }
            writeBytes("/.blob", blob.data(), blob.size());
        }

        const std::vector<Application> applications{
            {"Fernwood Writer", "com.fernwood.writer", "16.2", "16240", 1126 * kMB, {.26, .72, .46}, {.07, .45, .30}, Glyph::Page},
            {"Fernwood Sheets", "com.fernwood.sheets", "16.2", "16240", 934 * kMB, {.17, .66, .62}, {.05, .40, .42}, Glyph::Grid},
            {"Atlasmith", "com.atlasmith.maps", "3.8.0", "3802", 884 * kMB, {.95, .62, .33}, {.80, .34, .16}, Glyph::Peaks},
            {"Fernwood Slides", "com.fernwood.slides", "16.2", "16240", 791 * kMB, {.55, .74, .25}, {.30, .50, .10}, Glyph::Frames},
            {"PixelForge Studio", "com.pixelforge.Studio", "4.2.1", "4210", 612 * kMB, {.56, .42, .96}, {.30, .18, .72}, Glyph::Hexagon,
             {{"Library/LoginItems/PixelForge Export Helper.app", "com.pixelforge.Studio.ExportHelper"}}},
            {"Cantata Audio", "com.cantata-audio.Cantata", "9.1", "9107", 538 * kMB, {.97, .40, .52}, {.77, .13, .34}, Glyph::Bars},
            {"Kestrel Mail", "com.kestrelmail.Kestrel", "2.14", "2140", 213 * kMB, {.36, .64, .98}, {.10, .36, .84}, Glyph::Envelope},
            {"Tidewater Backup", "com.tidewaterlabs.backup", "7.0.3", "7031", 148 * kMB, {.24, .62, .86}, {.07, .30, .55}, Glyph::Waves,
             {{"Library/LoginItems/Tidewater Menu.app", "com.tidewaterlabs.backup.menu"}}},
            {"PixelForge Viewer", "com.pixelforge.Viewer", "4.2.0", "4200", 127 * kMB, {.70, .58, .98}, {.44, .32, .82}, Glyph::Lens},
            {"Basalt Terminal", "dev.basalt.Terminal", "1.9.4", "194", 96 * kMB, {.36, .39, .45}, {.13, .15, .19}, Glyph::Prompt},
            {"Marigold Planner", "com.marigoldplanner.mac", "5.3", "531", 64 * kMB, {.99, .78, .28}, {.93, .55, .10}, Glyph::Sun},
            {"Lumenote", "com.lumenote.Lumenote", "2.0.8", "208", 41 * kMB, {.40, .80, .90}, {.16, .52, .70}, Glyph::Spark},
        };
        for (const Application &app : applications) makeApplication(app, executable);

        const std::string library = "/Users/demo/Library";

        // ---- PixelForge Studio: an ordinary application, plus a vendor folder it
        // ---- shares with PixelForge Viewer.
        fill(library + "/Application Support/com.pixelforge.Studio", 184 * kMB);
        fill(library + "/Application Support/PixelForge/Presets", 96 * kMB);
        fill(library + "/Caches/com.pixelforge.Studio", 743 * kMB);
        fill(library + "/Caches/com.pixelforge.Studio.ShipIt", 61 * kMB);
        fill(library + "/Containers/com.pixelforge.Studio.ExportHelper/Data", 38 * kMB);
        fill(library + "/HTTPStorages/com.pixelforge.Studio", 1300 * kKB);
        fill(library + "/WebKit/com.pixelforge.Studio", 22 * kMB);
        fill(library + "/Saved Application State/com.pixelforge.Studio.savedState", 94 * kKB);
        fill(library + "/Logs/PixelForge Studio", 3400 * kKB);
        writeText(library + "/Preferences/com.pixelforge.Studio.plist", plist(key("RecentProject", "Harbour at dawn")));
        writeText(library + "/LaunchAgents/com.pixelforge.Studio.updater.plist",
                  launchdJob("com.pixelforge.Studio.updater",
                             "/Applications/PixelForge Studio.app/Contents/MacOS/PixelForge Studio"));

        // ---- Tidewater Backup: installed by a package, with a root daemon, a
        // ---- command-line tool, and a kernel extension from before SIP.
        const std::string daemon = "com.tidewaterlabs.backup.daemon";
        writeText("/Library/LaunchDaemons/" + daemon + ".plist", launchdJob(daemon, "/Library/PrivilegedHelperTools/" + daemon));
        fill("/Library/Application Support/Tidewater Backup", 264 * kMB);
        fill("/Library/PreferencePanes/Tidewater.prefPane/Contents/Resources", 5200 * kKB);
        writeText("/Library/PreferencePanes/Tidewater.prefPane/Contents/Info.plist",
                  plist(key("CFBundleIdentifier", "com.tidewaterlabs.backup.prefpane")));
        fill("/System/Library/Extensions/TidewaterFS.kext/Contents/MacOS", 780 * kKB);
        const auto clone = [](const std::string &logical, std::uint64_t size) {
            makeParent(logical);
            if (::clonefile(gBlob.c_str(), at(logical).c_str(), 0) != 0) die(logical);
            if (::truncate(at(logical).c_str(), static_cast<off_t>(size)) != 0) die(logical);
        };
        clone("/Library/PrivilegedHelperTools/" + daemon, 9400 * kKB);
        clone("/usr/local/bin/tidewater", 3100 * kKB);
        clone("/usr/local/share/man/man1/tidewater.1", 14 * kKB);
        // Someone else's files next to it: the folders themselves are not Tidewater's.
        clone("/usr/local/bin/harbourctl", 900 * kKB);
        clone("/usr/local/share/man/man1/harbourctl.1", 9 * kKB);
        fill(library + "/Caches/com.tidewaterlabs.backup", 412 * kMB);
        fill(library + "/Application Support/Tidewater Backup", 35 * kMB);
        fill(library + "/Logs/Tidewater Backup", 8800 * kKB);
        writeText(library + "/Preferences/com.tidewaterlabs.backup.plist", plist(key("Destination", "Vault")));

        vt::BomBuilder bom;
        bom.dir("./Applications/Tidewater Backup.app").file("./Applications/Tidewater Backup.app/Contents/Info.plist", 700);
        bom.file("./Applications/Tidewater Backup.app/Contents/MacOS/Tidewater Backup", 60000, 0100755);
        bom.file("./Library/LaunchDaemons/" + daemon + ".plist", 420);
        bom.file("./Library/PrivilegedHelperTools/" + daemon, 9400000, 0100755);
        bom.dir("./Library/PreferencePanes/Tidewater.prefPane").file("./Library/PreferencePanes/Tidewater.prefPane/Contents/Info.plist", 300);
        bom.file("./Library/PreferencePanes/Tidewater.prefPane/Contents/Resources/data-1.bin", 5200000);
        bom.dir("./System/Library/Extensions/TidewaterFS.kext").file("./System/Library/Extensions/TidewaterFS.kext/Contents/MacOS/data-1.bin", 780000);
        bom.file("./usr/local/bin/tidewater", 3100000, 0100755).file("./usr/local/share/man/man1/tidewater.1", 14000);
        const std::vector<std::uint8_t> bomImage = bom.build();
        writeBytes("/private/var/db/receipts/com.tidewaterlabs.backup.pkg.bom", bomImage.data(), bomImage.size());
        writeText("/private/var/db/receipts/com.tidewaterlabs.backup.pkg.plist",
                  plist(key("PackageIdentifier", "com.tidewaterlabs.backup.pkg") + key("PackageVersion", "7.0.3") +
                        key("InstallPrefixPath", "/") + key("InstallProcessName", "Installer") +
                        "<key>InstallDate</key><date>2026-03-04T09:26:00Z</date>\n"));

        // ---- The Fernwood suite: three applications around shared folders.
        fill(library + "/Application Support/Fernwood/Templates", 412 * kMB);
        fill(library + "/Application Support/com.fernwood.writer", 57 * kMB);
        fill(library + "/Caches/com.fernwood.writer", 326 * kMB);
        fill(library + "/Caches/com.fernwood.sheets", 188 * kMB);
        fill(library + "/Caches/Fernwood/Fonts", 143 * kMB);
        fill(library + "/Containers/com.fernwood.writer/Data", 74 * kMB);
        fill(library + "/HTTPStorages/com.fernwood.writer", 2100 * kKB);
        fill(library + "/Saved Application State/com.fernwood.writer.savedState", 120 * kKB);
        fill(library + "/Logs/Fernwood", 18 * kMB);
        writeText(library + "/Preferences/com.fernwood.writer.plist", plist(key("Theme", "Fern")));
        writeText(library + "/Preferences/com.fernwood.sheets.plist", plist(key("Theme", "Fern")));
        fill("/Library/Application Support/Fernwood/Licensing", 6400 * kKB);

        // ---- What only root may change is read-only here, so that the interface
        // ---- marks it the way it would on a real disk.
        for (const char *directory : {"/Library/LaunchDaemons", "/Library/PrivilegedHelperTools", "/Library/PreferencePanes",
                                      "/Library/Application Support", "/usr/local/bin", "/usr/local/share/man/man1",
                                      "/private/var/db/receipts"}) {
            makeReadOnly(directory);
        }
        std::printf("%s\n", gRoot.c_str());
    }
    return 0;
}
