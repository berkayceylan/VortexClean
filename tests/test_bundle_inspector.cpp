// Tests for bundle discovery and metadata extraction: Info.plist parsing via
// CoreFoundation, embedded helpers, declared launchd labels, and — against a
// real system application — the Security.framework code-signing path.
#include <algorithm>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#include "BundleInspector.hpp"
#include "FileUtil.hpp"
#include "MatchEngine.hpp"
#include "PathPolicy.hpp"
#include "TestSupport.hpp"

using namespace vortex;

namespace {

bool contains(const std::vector<std::string>& list, const std::string& value) {
    return std::find(list.begin(), list.end(), value) != list.end();
}

}  // namespace

VT_TEST(inspect_reads_info_plist_fields) {
    const vt::TempDir temp("vortex-bundle");
    const std::string bundle = vt::makeAppBundle(
        temp.path(), "Rocket Launcher.app", "com.acme.Rocket", "RocketBin",
        "  <key>CFBundleDisplayName</key>\n  <string>Rocket</string>\n"
        "  <key>SMPrivilegedExecutables</key>\n  <dict>\n"
        "    <key>com.acme.rocketd</key>\n    <string>identifier \"com.acme.rocketd\"</string>\n  </dict>\n");

    const auto inspected = BundleInspector::inspect(bundle);
    VT_REQUIRE(inspected.ok());
    const AppInfo& app = inspected.value();
    VT_CHECK_EQ(app.bundlePath, bundle);
    VT_CHECK_EQ(app.bundleId, std::string("com.acme.Rocket"));
    VT_CHECK_EQ(app.displayName, std::string("Rocket Launcher"));  // what Finder shows
    VT_CHECK_EQ(app.bundleDisplayName, std::string("Rocket"));
    VT_CHECK_EQ(app.bundleName, std::string("Rocket Launcher"));
    VT_CHECK_EQ(app.executableName, std::string("RocketBin"));
    VT_CHECK_EQ(app.executablePath, bundle + "/Contents/MacOS/RocketBin");
    VT_CHECK_EQ(app.shortVersion, std::string("1.2.3"));
    VT_CHECK_EQ(app.bundleVersion, std::string("42"));
    VT_CHECK(contains(app.helperLabels, "com.acme.rocketd"));
    VT_CHECK(!app.isAppleApp);
    VT_CHECK(!app.isSipProtected);
    VT_CHECK(!app.isAppStore);
    VT_CHECK(!app.isWrappedIosApp);
    VT_CHECK_EQ(app.ownerUid, ::getuid());
    VT_CHECK(app.modifiedEpoch > 1'600'000'000);

    // A bundle nobody signed.
    VT_CHECK(!app.signing.isSigned);
    VT_CHECK(app.signing.teamIdentifier.empty());
    VT_CHECK_EQ(app.signing.summary(), std::string("Unsigned"));

    // A path that reaches the bundle through a symlink is canonicalised.
    VT_REQUIRE(::symlink(bundle.c_str(), temp.sub("alias.app").c_str()) == 0);
    const auto viaLink = BundleInspector::inspect(temp.sub("alias.app"));
    VT_REQUIRE(viaLink.ok());
    VT_CHECK_EQ(viaLink.value().bundlePath, bundle);
}

VT_TEST(inspect_collects_embedded_helpers_and_labels) {
    const vt::TempDir temp("vortex-bundle");
    const std::string bundle = vt::makeAppBundle(temp.path(), "Rocket.app", "com.acme.Rocket", "Rocket");
    vt::makeAppBundle(bundle + "/Contents/Library/LoginItems", "RocketLogin.app", "com.acme.Rocket.login", "RocketLogin");
    vt::makeAppBundle(bundle + "/Contents/Frameworks", "Rocket Helper.app", "com.acme.Rocket.helper", "Rocket Helper");
    vt::writeFile(bundle + "/Contents/XPCServices/Worker.xpc/Contents/Info.plist",
                  vt::plistXml({{"CFBundleIdentifier", "com.acme.RocketWorker"}}));
    vt::writeFile(bundle + "/Contents/PlugIns/Share.appex/Contents/Info.plist",
                  vt::plistXml({{"CFBundleIdentifier", "com.acme.Rocket.share"}}));
    // Not a helper bundle: frameworks are libraries and are deliberately ignored.
    vt::writeFile(bundle + "/Contents/Frameworks/Sparkle.framework/Resources/Info.plist",
                  vt::plistXml({{"CFBundleIdentifier", "org.sparkle-project.Sparkle"}}));
    vt::writeFile(bundle + "/Contents/Library/LaunchServices/com.acme.rocket.privileged", "binary");
    vt::writeFile(bundle + "/Contents/Library/LaunchAgents/agent.plist",
                  vt::plistXml({{"Label", "com.acme.rocket.agent"}, {"BundleProgram", "Contents/MacOS/agent"}}));
    vt::writeFile(bundle + "/Contents/Library/LaunchDaemons/daemon.plist",
                  vt::plistXml({{"Label", "com.acme.rocket.daemon"}}));
    vt::writeFile(bundle + "/Contents/_MASReceipt/receipt", "receipt");

    const auto inspected = BundleInspector::inspect(bundle);
    VT_REQUIRE(inspected.ok());
    const AppInfo& app = inspected.value();
    for (const char* id : {"com.acme.Rocket.login", "com.acme.Rocket.helper", "com.acme.RocketWorker",
                           "com.acme.Rocket.share"}) {
        if (!contains(app.embeddedBundleIds, id)) {
            vt::reportFailure(__FILE__, __LINE__, std::string("embedded identifier missing: ") + id);
        }
    }
    VT_CHECK(!contains(app.embeddedBundleIds, "org.sparkle-project.Sparkle"));
    VT_CHECK(!contains(app.embeddedBundleIds, "com.acme.Rocket"));  // never lists itself
    for (const char* label : {"com.acme.rocket.privileged", "com.acme.rocket.agent", "com.acme.rocket.daemon"}) {
        if (!contains(app.helperLabels, label)) {
            vt::reportFailure(__FILE__, __LINE__, std::string("helper label missing: ") + label);
        }
    }
    VT_CHECK(app.isAppStore);

    // What was collected is exactly what the attribution engine needs.
    const InstalledAppIndex index(std::vector<AppInfo>{app});
    const MatchEngine engine(app, index);
    VT_CHECK(engine.matchIdentifier("com.acme.RocketWorker").matched);          // embedded, same vendor
    VT_CHECK(engine.matchIdentifier("com.acme.rocket.privileged").matched);     // declared label
    VT_CHECK(engine.matchIdentifier("com.acme.rocket.daemon").confidence == Confidence::Exact);

    // Options switch the optional work off.
    BundleInspector::Options bare;
    bare.enumerateEmbeddedBundles = false;
    bare.readSigningInfo = false;
    bare.readLastUsedDate = false;
    const auto quick = BundleInspector::inspect(bundle, bare);
    VT_REQUIRE(quick.ok());
    VT_CHECK(quick.value().embeddedBundleIds.empty());
    VT_CHECK_EQ(quick.value().lastUsedEpoch, std::int64_t{0});
}

VT_TEST(inspect_rejects_things_that_are_not_bundles) {
    const vt::TempDir temp("vortex-bundle");
    VT_CHECK(!BundleInspector::inspect(temp.sub("missing.app")).ok());

    vt::makeDirs(temp.sub("Empty.app/Contents"));
    const auto empty = BundleInspector::inspect(temp.sub("Empty.app"));
    VT_CHECK(!empty.ok());
    VT_CHECK_EQ(empty.error().code, ENOENT);

    vt::writeFile(temp.sub("file.app"), "not a directory");
    VT_CHECK(!BundleInspector::inspect(temp.sub("file.app")).ok());

    vt::writeFile(temp.sub("Broken.app/Contents/Info.plist"), "this is not a property list");
    VT_CHECK(!BundleInspector::inspect(temp.sub("Broken.app")).ok());

    vt::writeFile(temp.sub("Array.app/Contents/Info.plist"),
                  "<?xml version=\"1.0\"?><plist version=\"1.0\"><array><string>x</string></array></plist>");
    VT_CHECK(!BundleInspector::inspect(temp.sub("Array.app")).ok());

    // Wrong value types are tolerated: the field simply stays empty.
    vt::writeFile(temp.sub("Odd.app/Contents/Info.plist"),
                  vt::plistXml({{"CFBundleName", "Odd"}}, "  <key>CFBundleIdentifier</key>\n  <integer>7</integer>\n"));
    const auto odd = BundleInspector::inspect(temp.sub("Odd.app"));
    VT_REQUIRE(odd.ok());
    VT_CHECK(odd.value().bundleId.empty());
    VT_CHECK_EQ(odd.value().executableName, std::string("Odd"));

    VT_CHECK(BundleInspector::bundleIdentifier(temp.sub("missing.app")).empty());
    VT_CHECK(BundleInspector::infoPlistPath(temp.sub("Empty.app")).empty());
    VT_CHECK(BundleInspector::hasAppExtension("/Applications/Rocket.app"));
    VT_CHECK(BundleInspector::hasAppExtension("/Applications/Rocket.APP/"));
    VT_CHECK(!BundleInspector::hasAppExtension("/Applications/Rocket.application"));
    VT_CHECK(!BundleInspector::hasAppExtension("/Applications/.app"));
    VT_CHECK(!BundleInspector::hasAppExtension("/Applications/Rocket"));
}

VT_TEST(inspect_understands_other_bundle_layouts) {
    const vt::TempDir temp("vortex-bundle");
    // A framework keeps its Info.plist under Versions/Current/Resources.
    vt::writeFile(temp.sub("Kit.framework/Versions/A/Resources/Info.plist"),
                  vt::plistXml({{"CFBundleIdentifier", "com.acme.Kit"}}));
    VT_REQUIRE(::symlink("A", temp.sub("Kit.framework/Versions/Current").c_str()) == 0);
    VT_CHECK_EQ(BundleInspector::bundleIdentifier(temp.sub("Kit.framework")), std::string("com.acme.Kit"));

    // An iOS app wrapped for Apple silicon: Foo.app/Wrapper/Foo.app plus a WrappedBundle link.
    vt::writeFile(temp.sub("Pocket.app/Wrapper/Pocket.app/Info.plist"),
                  vt::plistXml({{"CFBundleIdentifier", "com.acme.pocket.ios"}, {"CFBundleExecutable", "Pocket"}}));
    const auto wrapperOnly = BundleInspector::inspect(temp.sub("Pocket.app"));
    VT_REQUIRE(wrapperOnly.ok());
    VT_CHECK_EQ(wrapperOnly.value().bundleId, std::string("com.acme.pocket.ios"));
    VT_CHECK(wrapperOnly.value().isWrappedIosApp);
    VT_CHECK_EQ(wrapperOnly.value().executablePath, temp.sub("Pocket.app/Wrapper/Pocket.app/Pocket"));
    VT_REQUIRE(::symlink("Wrapper/Pocket.app", temp.sub("Pocket.app/WrappedBundle").c_str()) == 0);
    const auto wrapped = BundleInspector::inspect(temp.sub("Pocket.app"));
    VT_REQUIRE(wrapped.ok());
    VT_CHECK_EQ(wrapped.value().bundleId, std::string("com.acme.pocket.ios"));
    VT_CHECK(wrapped.value().isWrappedIosApp);
}

VT_TEST(discovery_finds_bundles_without_descending_into_them) {
    const vt::TempDir temp("vortex-discover");
    const std::string apps = temp.sub("Applications");
    vt::makeAppBundle(apps, "Alpha.app", "com.acme.alpha", "Alpha");
    vt::makeAppBundle(apps + "/Utilities", "Beta.app", "com.acme.beta", "Beta");
    vt::makeAppBundle(apps + "/Suite/Tools", "Gamma.app", "com.acme.gamma", "Gamma");
    vt::makeAppBundle(apps + "/a/b/c/d", "TooDeep.app", "com.acme.deep", "TooDeep");
    vt::makeAppBundle(apps + "/Alpha.app/Contents/Helpers", "Nested.app", "com.acme.alpha.nested", "Nested");
    vt::makeAppBundle(apps + "/.hidden", "Hidden.app", "com.acme.hidden", "Hidden");
    vt::makeAppBundle(temp.sub("elsewhere"), "Linked.app", "com.acme.linked", "Linked");
    VT_REQUIRE(::symlink(temp.sub("elsewhere/Linked.app").c_str(), (apps + "/Linked.app").c_str()) == 0);
    vt::writeFile(apps + "/NotABundle.app", "a plain file with an app extension");
    vt::makeAppBundle(temp.sub("UserApps"), "Delta.app", "com.acme.delta", "Delta");

    const std::vector<std::string> found =
        BundleInspector::discoverBundles({apps, temp.sub("UserApps"), temp.sub("does-not-exist")});
    const std::vector<std::string> expected{
        apps + "/Alpha.app",
        apps + "/Suite/Tools/Gamma.app",
        apps + "/Utilities/Beta.app",
        temp.sub("UserApps/Delta.app"),
    };
    VT_CHECK_EQ(found, expected);

    // The depth limit is configurable; cancellation returns immediately.
    VT_CHECK(contains(BundleInspector::discoverBundles({apps}, CancellationToken{}, 6), apps + "/a/b/c/d/TooDeep.app"));
    const CancellationToken cancelled;
    cancelled.cancel();
    VT_CHECK(BundleInspector::discoverBundles({apps}, cancelled).empty());

    const std::vector<std::string> roots = BundleInspector::defaultSearchRoots("/Users/tester");
    VT_CHECK_EQ(roots, (std::vector<std::string>{"/Applications", "/Users/tester/Applications"}));
}

VT_TEST(inspect_reads_real_code_signatures) {
    // Present on every macOS installation, signed by Apple, protected by SIP.
    const std::string calculator = "/System/Applications/Calculator.app";
    if (!io::isDirectory(calculator)) {
        vt::note("no " + calculator + " on this system; skipping the live signature check");
        return;
    }
    const auto inspected = BundleInspector::inspect(calculator);
    VT_REQUIRE(inspected.ok());
    const AppInfo& app = inspected.value();
    VT_CHECK_EQ(app.bundleId, std::string("com.apple.calculator"));
    VT_CHECK_EQ(app.displayName, std::string("Calculator"));
    VT_CHECK(!app.shortVersion.empty());
    VT_CHECK(app.signing.isSigned);
    VT_CHECK(!app.signing.isAdHoc);
    VT_CHECK(app.signing.isApplePlatform);          // satisfies "anchor apple"
    VT_CHECK(!app.signing.authorityChain.empty());
    VT_CHECK_EQ(app.signing.signingIdentifier, std::string("com.apple.calculator"));
    VT_CHECK(app.isAppleApp);
    VT_CHECK(app.isSipProtected);                   // the engine will refuse to touch it
    VT_CHECK(PathPolicy::isSipProtected(calculator));

    // /Applications itself carries the "no unlink" flag (the folder cannot be
    // removed), but that must not lock the third-party apps installed inside it.
    VT_CHECK(!PathPolicy::isSipProtected("/Applications/VortexClean-Test-Not-Installed.app"));
    VT_CHECK(!PathPolicy::containerIsRestricted("/Applications/VortexClean-Test-Not-Installed.app"));
    VT_CHECK(PathPolicy::containerIsRestricted(calculator));
    const PathPolicy policy("/Users/tester");
    VT_CHECK(policy.classify("/Applications/VortexClean-Test-Not-Installed.app") == Protection::None);
    VT_CHECK(policy.classify("/Applications") == Protection::ProtectedRoot);

    // LaunchServices knows where an identifier is registered: seen from any other
    // bundle it belongs to "another application"; seen from its own bundle it does not.
    VT_CHECK(BundleInspector::isRegisteredToAnotherApplication("com.apple.calculator", "/Applications/Other.app"));
    VT_CHECK(BundleInspector::isRegisteredToAnotherApplication("COM.APPLE.CALCULATOR", "/Applications/Other.app"));
    VT_CHECK(!BundleInspector::isRegisteredToAnotherApplication("com.apple.calculator", calculator));
    VT_CHECK(!BundleInspector::isRegisteredToAnotherApplication("com.berkayceylan.VortexClean.never.registered.anywhere", calculator));
    VT_CHECK(!BundleInspector::isRegisteredToAnotherApplication("", calculator));

    // The auxiliary index lists the system's applications without offering them.
    const std::vector<AppInfo> auxiliary = BundleInspector::inspectAuxiliaryApplications();
    const auto found = std::find_if(auxiliary.begin(), auxiliary.end(),
                                    [](const AppInfo& app) { return app.bundleId == "com.apple.calculator"; });
    VT_REQUIRE(found != auxiliary.end());
    VT_CHECK(found->isSipProtected);
    VT_CHECK(found->isAppleApp);
    vt::note("Calculator.app: " + app.signing.summary() + ", chain of " +
             std::to_string(app.signing.authorityChain.size()) + " certificates");
}
