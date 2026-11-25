// Tests for attribution: exact bundle-identifier matching, shared-vendor and
// shared-team exclusion, rejection of generic tokens, the com.apple.* rule, and
// an end-to-end scan of a synthetic system whose result is compared against an
// exact expected set (anything extra is a false positive, anything missing a
// false negative).
#include <algorithm>
#include <atomic>
#include <mutex>
#include <set>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#include "BomBuilder.hpp"
#include "BundleInspector.hpp"
#include "MatchEngine.hpp"
#include "PathPolicy.hpp"
#include "PathUtil.hpp"
#include "RemnantScanner.hpp"
#include "TestSupport.hpp"

using namespace vortex;

namespace {

constexpr const char* kTeam = "ABCDE12345";
constexpr const char* kUuid = "4F1A6B2C-0D3E-4F5A-8B7C-9D0E1F2A3B4C";

AppInfo makeApp(const std::string& bundlePath, const std::string& bundleId, const std::string& name,
                const std::string& executable, const std::string& team = {}) {
    AppInfo app;
    app.bundlePath = bundlePath;
    app.bundleId = bundleId;
    app.displayName = name;
    app.bundleName = name;
    app.executableName = executable;
    app.executablePath = bundlePath + "/Contents/MacOS/" + executable;
    app.signing.teamIdentifier = team;
    app.signing.isSigned = !team.empty();
    return app;
}

AppInfo rocketApp() {
    return makeApp("/Applications/Rocket.app", "com.acme.Rocket", "Rocket", "Rocket", kTeam);
}

std::vector<std::string> names(std::initializer_list<const char*> list) {
    return std::vector<std::string>(list.begin(), list.end());
}

}  // namespace

// ---------------------------------------------------------------------------
// Vocabulary: stop-words and short tokens
// ---------------------------------------------------------------------------

VT_TEST(generic_tokens_are_never_distinctive) {
    for (const char* word : {"App", "Mac", "Pro", "Helper", "Update", "Client", "Agent", "Tool", "Updater",
                             "Service", "Launcher", "Installer", "Daemon", "Manager", "Desktop", "Electron",
                             "helper", "UPDATE", "Tools", "Utilities", "Support", "Framework"}) {
        if (MatchEngine::isDistinctiveName(word)) {
            vt::reportFailure(__FILE__, __LINE__, std::string("stop-word accepted: ") + word);
        }
    }
    // Single tokens shorter than four characters, whatever they are.
    for (const char* word : {"Go", "VLC", "Arc", "Zed", "7z", "ab", "abc", "X", ""}) {
        if (MatchEngine::isDistinctiveName(word)) {
            vt::reportFailure(__FILE__, __LINE__, std::string("short token accepted: ") + word);
        }
    }
    // Phrases built only from generic or short tokens.
    for (const char* phrase : {"Pro Tools", "App Helper", "Update Agent", "Mac App", "Helper (GPU)", "Go 2",
                               "My App", "Web Client", "The Tool", "12345", "2024", "com.apple.Safari"}) {
        if (MatchEngine::isDistinctiveName(phrase)) {
            vt::reportFailure(__FILE__, __LINE__, std::string("generic phrase accepted: ") + phrase);
        }
    }
    for (const char* name : {"Slack", "IINA", "Rocket", "Visual Studio Code", "App Cleaner", "Tower 2",
                             "1Password", "iTerm2", "Sublime Text", "Be My Eyes"}) {
        if (!MatchEngine::isDistinctiveName(name)) {
            vt::reportFailure(__FILE__, __LINE__, std::string("distinctive name rejected: ") + name);
        }
    }
    VT_CHECK(MatchEngine::isStopWord("helper"));
    VT_CHECK(MatchEngine::isStopWord("agent"));
    VT_CHECK(!MatchEngine::isStopWord("rocket"));
}

VT_TEST(an_app_with_only_generic_names_gets_no_name_matches) {
    // Display name, executable and the last identifier component are all stop-words.
    const AppInfo generic = makeApp("/Applications/Helper.app", "com.acme.Helper", "Helper", "Agent");
    const InstalledAppIndex index;
    const MatchEngine engine(generic, index);

    for (const char* entry : {"Helper", "Agent", "helper", "Helper.log", "Agent.plist", "Helper Data"}) {
        VT_CHECK(!engine.matchName(entry));
        VT_CHECK(!engine.matchEntry(entry, true));
    }
    VT_CHECK(!engine.matchDiagnosticReport("Agent-2026-01-02-030405.ips"));
    VT_CHECK(!engine.matchDiagnosticReport("Helper-2026-01-02-030405.ips"));
    // Its exact identifier is still unambiguous.
    const MatchVerdict exact = engine.matchEntry("com.acme.Helper.plist", true);
    VT_REQUIRE(exact.matched);
    VT_CHECK(exact.confidence == Confidence::Exact);
    VT_CHECK(exact.reason == MatchReason::ExactBundleId);
}

// ---------------------------------------------------------------------------
// Identifier rules
// ---------------------------------------------------------------------------

VT_TEST(exact_bundle_identifier_matching) {
    const AppInfo rocket = rocketApp();
    const InstalledAppIndex index;
    const MatchEngine engine(rocket, index);

    const MatchVerdict exact = engine.matchIdentifier("com.acme.Rocket");
    VT_REQUIRE(exact.matched);
    VT_CHECK(exact.confidence == Confidence::Exact);
    VT_CHECK(exact.reason == MatchReason::ExactBundleId);
    VT_CHECK(!exact.shared);
    VT_CHECK(engine.matchIdentifier("COM.ACME.ROCKET").matched);  // identifiers compare case-insensitively
    VT_CHECK(engine.matchIdentifier("  com.acme.Rocket ").matched);

    // The decorated spellings found in Library folders all reduce to the identifier.
    for (const std::string& entry :
         {std::string("com.acme.Rocket.plist"), std::string("com.acme.Rocket.savedState"),
          std::string("com.acme.Rocket.binarycookies"), std::string("com.acme.Rocket.plist.lockfile"),
          std::string("com.acme.rocket.sfl3"), std::string("com.acme.Rocket.") + kUuid + ".plist",
          std::string("com.acme.Rocket.0a1b2c3d4e5f.plist")}) {
        const MatchVerdict v = engine.matchEntry(entry, false);
        if (!v.matched || v.confidence != Confidence::Exact) {
            vt::reportFailure(__FILE__, __LINE__, "expected an exact match for " + entry);
        }
    }

    // Dotted children are attributed with high (not exact) confidence.
    for (const char* child : {"com.acme.Rocket.helper", "com.acme.Rocket.ShipIt", "com.acme.Rocket.xpc.service"}) {
        const MatchVerdict v = engine.matchIdentifier(child);
        VT_CHECK(v.matched);
        VT_CHECK(v.confidence == Confidence::High);
        VT_CHECK(v.reason == MatchReason::BundleIdPrefix);
    }

    // Look-alikes are not children: no dot boundary, different vendor, truncated.
    for (const char* other :
         {"com.acme.RocketPro", "com.acme.Rocket2", "com.acme.Rocket-backup", "com.acme.Rocke", "com.acme",
          "com", "xcom.acme.Rocket", "org.acme.Rocket", "com.acme.Rocket.", "net.acme.Rocket.helper", ""}) {
        if (engine.matchIdentifier(other).matched) {
            vt::reportFailure(__FILE__, __LINE__, std::string("look-alike identifier matched: ") + other);
        }
    }
    for (const char* entry : {"com.acme.RocketPro.plist", "com.acme.Rocket2.savedState", "com.acme.plist"}) {
        VT_CHECK(!engine.matchEntry(entry, true));
    }

    // An identifier whose last component is literally "app" must not be mistaken for an extension.
    const AppInfo dotApp = makeApp("/Applications/Thing.app", "com.acme.app", "Thing", "Thing");
    const MatchEngine dotAppEngine(dotApp, index);
    VT_CHECK(dotAppEngine.matchEntry("com.acme.app", false).confidence == Confidence::Exact);
    VT_CHECK(dotAppEngine.matchEntry("com.acme.app", false).matched);
    VT_CHECK(dotAppEngine.matchEntry("com.acme.app.plist", false).matched);
}

VT_TEST(longest_identifier_owner_wins) {
    const AppInfo rocket = rocketApp();
    const AppInfo pro = makeApp("/Applications/Rocket Pro.app", "com.acme.Rocket.Pro", "Rocket Pro", "Rocket Pro", kTeam);
    const InstalledAppIndex index(std::vector<AppInfo>{rocket, pro});

    const MatchEngine rocketEngine(rocket, index);
    // The sibling's identifier is a dotted child of ours, but the sibling is installed: hands off.
    VT_CHECK(!rocketEngine.matchEntry("com.acme.Rocket.Pro.plist", true));
    VT_CHECK(!rocketEngine.matchEntry("com.acme.Rocket.Pro", true));
    VT_CHECK(!rocketEngine.matchEntry("com.acme.Rocket.Pro.savedState", true));
    VT_CHECK(!rocketEngine.matchEntry("com.acme.Rocket.Pro.cache", true));
    VT_CHECK(!rocketEngine.matchName("Rocket Pro"));
    VT_CHECK(rocketEngine.matchEntry("com.acme.Rocket.plist", true).confidence == Confidence::Exact);
    VT_CHECK(rocketEngine.matchEntry("com.acme.Rocket.plist", true).matched);
    VT_CHECK(rocketEngine.matchEntry("com.acme.Rocket.other", true).matched);

    const MatchEngine proEngine(pro, index);
    VT_CHECK(proEngine.matchEntry("com.acme.Rocket.Pro.plist", true).matched);
    VT_CHECK(proEngine.matchEntry("com.acme.Rocket.Pro.cache", true).matched);
    VT_CHECK(!proEngine.matchEntry("com.acme.Rocket.plist", true));
    VT_CHECK(!proEngine.matchEntry("com.acme.Rocket", true));
    VT_CHECK(!proEngine.matchName("Rocket"));

    // With the sibling uninstalled the very same entry becomes attributable again.
    const InstalledAppIndex alone(std::vector<AppInfo>{rocket});
    const MatchEngine aloneEngine(rocket, alone);
    VT_CHECK(aloneEngine.matchEntry("com.acme.Rocket.Pro.plist", true).matched);
}

VT_TEST(identifiers_registered_to_another_app_are_not_attributed) {
    // "Chrome for Testing" is a separate product whose identifier extends
    // Chrome's, and it lives where discovery never looks (a tool's cache).
    const AppInfo chrome =
        makeApp("/Applications/Google Chrome.app", "com.google.Chrome", "Google Chrome", "Google Chrome", "EQHXZ8M8AV");
    const InstalledAppIndex index(std::vector<AppInfo>{chrome});
    std::vector<std::string> asked;
    const MatchEngine::ForeignOwnerLookup registry = [&asked](std::string_view identifier) {
        asked.emplace_back(identifier);
        // What LaunchServices would say: registered to an app outside Chrome's bundle.
        return identifier == "com.google.chrome.for.testing" ||
               identifier == "com.google.chrome.remote_desktop.remote-assistance-host-v2";
    };
    const MatchEngine engine(chrome, index, registry);

    // The other product's own identifier, its children, and decorated spellings.
    for (const char* entry : {"com.google.chrome.for.testing", "com.google.chrome.for.testing.plist",
                              "com.google.chrome.for.testing.helper", "com.google.chrome.for.testing.03gHKT",
                              "com.google.Chrome.for.Testing.savedState",
                              "com.google.chrome.remote_desktop.remote-assistance-host-v2"}) {
        if (engine.matchEntry(entry, true).matched) {
            vt::reportFailure(__FILE__, __LINE__, std::string("another app's identifier was attributed: ") + entry);
        }
    }
    // Chrome's own helpers and files are unaffected.
    for (const char* entry : {"com.google.Chrome.helper", "com.google.Chrome.plist", "com.google.Chrome.ShipIt",
                              "com.google.Chrome.chrome_chrome_url_fetcher_.P3G2mt", "com.google.Chrome"}) {
        if (!engine.matchEntry(entry, false).matched) {
            vt::reportFailure(__FILE__, __LINE__, std::string("own identifier lost: ") + entry);
        }
    }
    // The registry is consulted for the identifier and its dotted ancestors only
    // down to (not including) our own identifier, and every answer is cached.
    const auto count = [&asked](const std::string& id) { return std::count(asked.begin(), asked.end(), id); };
    VT_CHECK_EQ(count("com.google.chrome.for.testing"), 1);
    VT_CHECK_EQ(count("com.google.chrome.helper"), 1);
    VT_CHECK_EQ(count("com.google.chrome"), 0);  // an exact match needs no second opinion
    VT_CHECK_EQ(count("com.google"), 0);

    // Without a registry the same entries fall back to the plain prefix rule.
    const MatchEngine plain(chrome, index);
    VT_CHECK(plain.matchEntry("com.google.chrome.for.testing.plist", false).matched);
}

VT_TEST(apple_identifiers_are_never_attributed) {
    for (const char* id : {"com.apple.Safari", "COM.APPLE.finder", "com.apple", "group.com.apple.notes",
                           "ABCDE12345.com.apple.shared", "systemgroup.com.apple.configurationprofiles"}) {
        if (!MatchEngine::isAppleIdentifier(id)) {
            vt::reportFailure(__FILE__, __LINE__, std::string("not recognised as Apple: ") + id);
        }
    }
    for (const char* id : {"com.applesauce.app", "org.apple.foo", "com.vendor.apple", "com.acme.Rocket", ""}) {
        if (MatchEngine::isAppleIdentifier(id)) {
            vt::reportFailure(__FILE__, __LINE__, std::string("wrongly treated as Apple: ") + id);
        }
    }

    const AppInfo rocket = rocketApp();
    const InstalledAppIndex index;
    const MatchEngine engine(rocket, index);
    VT_CHECK(!engine.matchIdentifier("com.apple.Rocket"));
    VT_CHECK(!engine.matchName("com.apple.Rocket"));
    VT_CHECK(!engine.matchEntry("com.apple.Rocket.plist", true));
    VT_CHECK(!engine.matchGroupIdentifier("group.com.apple.Rocket"));
    VT_CHECK(!engine.matchGroupIdentifier(std::string(kTeam) + ".com.apple.Rocket"));

    // Even when the target itself is an Apple application, com.apple.* is off limits.
    AppInfo xcode = makeApp("/Applications/Xcode.app", "com.apple.dt.Xcode", "Xcode", "Xcode");
    xcode.isAppleApp = true;
    const MatchEngine appleEngine(xcode, index);
    VT_CHECK(appleEngine.targetIsApple());
    VT_CHECK(!appleEngine.matchIdentifier("com.apple.dt.Xcode"));
    VT_CHECK(!appleEngine.matchEntry("com.apple.dt.Xcode.plist", true));
    VT_CHECK(!appleEngine.matchEntry("com.apple.dt.Xcode.savedState", true));
    VT_CHECK(!appleEngine.isVendorDirectoryName("Apple"));
    VT_CHECK_EQ(MatchEngine::vendorToken("com.apple.dt.Xcode"), std::string());
    // Only the plain product name can still be matched for it.
    VT_CHECK(appleEngine.matchName("Xcode").matched);
}

VT_TEST(embedded_helpers_and_declared_labels) {
    AppInfo rocket = rocketApp();
    rocket.embeddedBundleIds = names({"com.acme.RocketHelper", "org.sparkle-project.Sparkle", "com.acme.Rocket.xpc"});
    rocket.helperLabels = names({"com.acme.rocketd"});
    AppInfo other = makeApp("/Applications/Other.app", "org.other.Thing", "Other Thing", "Other Thing");
    other.embeddedBundleIds = names({"org.sparkle-project.Sparkle"});
    const InstalledAppIndex index(std::vector<AppInfo>{rocket, other});
    const MatchEngine engine(rocket, index);

    const MatchVerdict helper = engine.matchIdentifier("com.acme.RocketHelper");
    VT_REQUIRE(helper.matched);
    VT_CHECK(helper.confidence == Confidence::High);
    VT_CHECK(helper.reason == MatchReason::EmbeddedHelperId);
    VT_CHECK(!helper.shared);

    // A third-party component that many apps embed never identifies this app.
    VT_CHECK(!engine.matchIdentifier("org.sparkle-project.Sparkle"));
    VT_CHECK(!engine.matchEntry("org.sparkle-project.Sparkle.plist", true));

    const MatchVerdict label = engine.matchEntry("com.acme.rocketd.plist", false);
    VT_REQUIRE(label.matched);
    VT_CHECK(label.confidence == Confidence::Exact);
    VT_CHECK(label.reason == MatchReason::HelperLabel);
    VT_CHECK(engine.matchIdentifier("com.acme.rocketd").matched);

    // The same helper embedded in a sibling makes the match a shared one.
    AppInfo sibling = makeApp("/Applications/Acme Studio.app", "com.acme.Studio", "Acme Studio", "Acme Studio");
    sibling.embeddedBundleIds = names({"com.acme.RocketHelper"});
    const InstalledAppIndex withSibling(std::vector<AppInfo>{rocket, sibling});
    const MatchEngine sharedEngine(rocket, withSibling);
    const MatchVerdict sharedHelper = sharedEngine.matchIdentifier("com.acme.RocketHelper");
    VT_REQUIRE(sharedHelper.matched);
    VT_CHECK(sharedHelper.shared);
    VT_CHECK_EQ(sharedHelper.sharedWith, names({"Acme Studio"}));
}

// ---------------------------------------------------------------------------
// Name rules
// ---------------------------------------------------------------------------

VT_TEST(name_matching_is_whole_name_equality) {
    const AppInfo rocket = rocketApp();
    const InstalledAppIndex index;
    const MatchEngine engine(rocket, index);

    for (const char* entry : {"Rocket", "rocket", "ROCKET"}) {
        const MatchVerdict v = engine.matchName(entry);
        VT_CHECK(v.matched);
        VT_CHECK(v.confidence == Confidence::Medium);
        VT_CHECK(v.reason == MatchReason::AppNameMatch);
    }
    VT_CHECK(engine.matchEntry("Rocket.log", true).matched);
    VT_CHECK(!engine.matchEntry("Rocket.log", false));  // identifier-only locations ignore names

    // Never a substring, a prefix or a near miss.
    for (const char* entry : {"Rocket Helper", "RocketLauncher", "My Rocket", "Rockets", "Rocke", "Pocket",
                              "Rocket2", "Rocket Pro", "Sprocket"}) {
        if (engine.matchName(entry).matched) {
            vt::reportFailure(__FILE__, __LINE__, std::string("fuzzy name matched: ") + entry);
        }
    }

    AppInfo code = makeApp("/Applications/Visual Studio Code.app", "com.microsoft.VSCode", "Visual Studio Code", "Electron");
    code.bundleName = "Code";
    const MatchEngine codeEngine(code, index);
    VT_CHECK(codeEngine.matchName("Code").reason == MatchReason::AppNameMatch);
    VT_CHECK(codeEngine.matchName("Code").matched);
    VT_CHECK(codeEngine.matchName("Visual Studio Code").matched);
    VT_CHECK(!codeEngine.matchName("Electron"));  // the executable name is a stop-word
    const MatchVerdict idName = codeEngine.matchName("VSCode");
    VT_REQUIRE(idName.matched);
    VT_CHECK(idName.reason == MatchReason::BundleIdNameHeuristic);

    const AppInfo tower = makeApp("/Applications/Tower.app", "com.fournova.Tower3", "Tower", "TowerCore");
    const MatchEngine towerEngine(tower, index);
    const MatchVerdict executable = towerEngine.matchName("TowerCore");
    VT_REQUIRE(executable.matched);
    VT_CHECK(executable.reason == MatchReason::ExecutableNameHeuristic);
    VT_CHECK(executable.confidence == Confidence::Medium);
}

VT_TEST(a_name_shared_with_another_installed_app_is_flagged) {
    const AppInfo rocket = rocketApp();
    const AppInfo namesake = makeApp("/Applications/Setapp/Rocket.app", "io.other.rocket", "Rocket", "Rocket");
    const InstalledAppIndex index(std::vector<AppInfo>{rocket, namesake});
    const MatchEngine engine(rocket, index);

    const MatchVerdict v = engine.matchName("Rocket");
    VT_REQUIRE(v.matched);
    VT_CHECK(v.shared);
    VT_CHECK_EQ(v.sharedWith, names({"Rocket"}));
    // Identifier matches stay unambiguous.
    VT_CHECK(!engine.matchIdentifier("com.acme.Rocket").shared);
    VT_CHECK(!engine.matchIdentifier("io.other.rocket"));
}

VT_TEST(diagnostic_reports_need_the_exact_process_name) {
    const AppInfo rocket = rocketApp();
    const InstalledAppIndex index;
    const MatchEngine engine(rocket, index);

    for (const char* file : {"Rocket-2026-01-02-030405.ips", "Rocket_2026-01-02-030405_Mac.crash",
                             "Rocket-2026-01-02-030405.diag", "Rocket Helper (GPU)-2026-01-02-030405.ips"}) {
        const MatchVerdict v = engine.matchDiagnosticReport(file);
        if (!v.matched || v.confidence != Confidence::Medium) {
            vt::reportFailure(__FILE__, __LINE__, std::string("report not matched: ") + file);
        }
    }
    for (const char* file : {"RocketLauncher-2026-01-02-030405.ips", "Rocket-notes.ips", "Rocket.ips",
                             "Pocket-2026-01-02-030405.ips", "Rocket-2026-01-02-030405.txt",
                             "rocket-2026-01-02-030405.ips"}) {
        if (engine.matchDiagnosticReport(file).matched) {
            vt::reportFailure(__FILE__, __LINE__, std::string("unrelated report matched: ") + file);
        }
    }
    VT_CHECK(engine.matchCrashReporterRecord(std::string("Rocket_") + kUuid + ".plist").matched);
    VT_CHECK(!engine.matchCrashReporterRecord("Rocket_not-a-uuid.plist"));
    VT_CHECK(!engine.matchCrashReporterRecord(std::string("Other_") + kUuid + ".plist"));
    VT_CHECK(!engine.matchCrashReporterRecord(std::string("Rocket_") + kUuid + ".txt"));
}

// ---------------------------------------------------------------------------
// Shared vendor and shared team
// ---------------------------------------------------------------------------

VT_TEST(vendor_tokens_are_conservative) {
    VT_CHECK_EQ(MatchEngine::vendorToken("com.google.Chrome"), std::string("google"));
    VT_CHECK_EQ(MatchEngine::vendorToken("us.zoom.xos"), std::string("zoom"));
    VT_CHECK_EQ(MatchEngine::vendorToken("org.videolan.vlc"), std::string("videolan"));
    VT_CHECK_EQ(MatchEngine::vendorToken("com.jetbrains.intellij.ce"), std::string("jetbrains"));
    // Hosting platforms, placeholders, stop-words and short tokens are not vendors.
    for (const char* id : {"com.github.someone.app", "io.github.someone.app", "com.apple.Safari", "com.app.thing",
                           "com.yourcompany.Thing", "io.xy.z", "Rocket", "com.acme", "maccatalyst.com.acme.app",
                           "c0m.acme.app", ""}) {
        if (!MatchEngine::vendorToken(id).empty()) {
            vt::reportFailure(__FILE__, __LINE__, std::string("vendor derived from: ") + id);
        }
    }
}

VT_TEST(shared_vendor_folders_are_recognised) {
    const AppInfo chrome =
        makeApp("/Applications/Google Chrome.app", "com.google.Chrome", "Google Chrome", "Google Chrome", "EQHXZ8M8AV");
    const AppInfo drive =
        makeApp("/Applications/Google Drive.app", "com.google.drivefs", "Google Drive", "Google Drive", "EQHXZ8M8AV");
    const InstalledAppIndex alone(std::vector<AppInfo>{chrome});
    const MatchEngine engine(chrome, alone);

    VT_CHECK(engine.isVendorDirectoryName("Google"));
    VT_CHECK(engine.isVendorDirectoryName("google"));
    VT_CHECK(engine.isVendorDirectoryName("google.com"));
    VT_CHECK(!engine.isVendorDirectoryName("Google Inc"));
    VT_CHECK(!engine.isVendorDirectoryName("Chrome"));
    VT_CHECK(!engine.isVendorDirectoryName("Googled"));
    VT_CHECK(engine.vendorSiblings().empty());

    // The identifier's own last component ("Chrome") is a name of the product.
    VT_CHECK(engine.matchName("Chrome").reason == MatchReason::BundleIdNameHeuristic);
    VT_CHECK(engine.matchVendorChild("Google Chrome").matched);
    VT_CHECK(!engine.matchVendorChild("Drive"));
    VT_CHECK(!engine.matchVendorChild("GoogleUpdater"));

    // A product name with the vendor prefix removed ("Drive" for "Google Drive")
    // is only meaningful inside the vendor's own folder, never at the top level.
    const MatchEngine driveEngine(drive, alone);
    VT_CHECK(!driveEngine.matchName("Drive"));
    VT_CHECK(!driveEngine.matchEntry("Drive", true));
    const MatchVerdict insideVendor = driveEngine.matchVendorChild("Drive");
    VT_REQUIRE(insideVendor.matched);
    VT_CHECK(insideVendor.confidence == Confidence::Medium);
    VT_CHECK(!driveEngine.matchVendorChild("Chrome"));

    const InstalledAppIndex suite(std::vector<AppInfo>{chrome, drive});
    const MatchEngine suiteEngine(chrome, suite);
    VT_CHECK_EQ(suiteEngine.vendorSiblings(), names({"Google Drive"}));

    // When vendor and product share a name the folder is the product's, not a vendor folder.
    const AppInfo spotify = makeApp("/Applications/Spotify.app", "com.spotify.client", "Spotify", "Spotify");
    const MatchEngine spotifyEngine(spotify, alone);
    VT_CHECK(!spotifyEngine.isVendorDirectoryName("Spotify"));
    VT_CHECK(spotifyEngine.matchName("Spotify").matched);
}

VT_TEST(group_containers_respect_team_and_entitlements) {
    AppInfo rocket = rocketApp();
    rocket.signing.applicationGroups =
        names({"ABCDE12345.com.acme.rocket.shared", "group.com.acme.rocket", "ABCDE12345.com.acme.common"});
    const std::string team = kTeam;
    {
        const InstalledAppIndex index(std::vector<AppInfo>{rocket});
        const MatchEngine engine(rocket, index);

        const MatchVerdict entitled = engine.matchGroupIdentifier(team + ".com.acme.rocket.shared");
        VT_REQUIRE(entitled.matched);
        VT_CHECK(entitled.confidence == Confidence::Exact);
        VT_CHECK(entitled.reason == MatchReason::EntitledAppGroup);
        VT_CHECK(!entitled.shared);
        VT_CHECK(engine.matchGroupIdentifier("group.com.acme.rocket").confidence == Confidence::Exact);

        const MatchVerdict byId = engine.matchGroupIdentifier(team + ".com.acme.Rocket");
        VT_REQUIRE(byId.matched);
        VT_CHECK(byId.reason == MatchReason::ExactBundleId);
        VT_CHECK(engine.matchGroupIdentifier("group.com.acme.Rocket.extras").reason == MatchReason::BundleIdPrefix);

        // A bare team-prefixed group with no sibling installed: ours, with high confidence.
        const MatchVerdict teamOnly = engine.matchGroupIdentifier(team + ".misc");
        VT_REQUIRE(teamOnly.matched);
        VT_CHECK(teamOnly.confidence == Confidence::High);
        VT_CHECK(teamOnly.reason == MatchReason::TeamIdGroupContainer);
        VT_CHECK(!teamOnly.shared);

        for (const std::string& other : {std::string("ZZZZZ99999.misc"), team, team + "x.misc",
                                         std::string("group.org.other.thing"), std::string("misc")}) {
            if (engine.matchGroupIdentifier(other).matched) {
                vt::reportFailure(__FILE__, __LINE__, "foreign group matched: " + other);
            }
        }
    }
    {
        AppInfo studio = makeApp("/Applications/Acme Studio.app", "com.acme.Studio", "Acme Studio", "Acme Studio", kTeam);
        studio.signing.applicationGroups = names({"ABCDE12345.com.acme.suite", "ABCDE12345.com.acme.common"});
        const InstalledAppIndex index(std::vector<AppInfo>{rocket, studio});
        const MatchEngine engine(rocket, index);

        // Same team, sibling installed: the bare team group is shared and only medium confidence.
        const MatchVerdict teamOnly = engine.matchGroupIdentifier(team + ".misc");
        VT_REQUIRE(teamOnly.matched);
        VT_CHECK(teamOnly.confidence == Confidence::Medium);
        VT_CHECK(teamOnly.shared);
        VT_CHECK_EQ(teamOnly.sharedWith, names({"Acme Studio"}));

        // A group only the sibling is entitled to is the sibling's.
        VT_CHECK(!engine.matchGroupIdentifier(team + ".com.acme.suite"));
        // That holds even for a target without any entitlements of its own
        // (an unsandboxed app): the sibling's declaration settles the question.
        AppInfo plain = rocketApp();
        const MatchEngine plainEngine(plain, index);
        VT_CHECK(!plainEngine.matchGroupIdentifier(team + ".com.acme.suite"));
        VT_CHECK(plainEngine.matchGroupIdentifier(team + ".misc").shared);  // undeclared: listed, but shared

        // A group both apps are entitled to is ours, but shared.
        const MatchVerdict common = engine.matchGroupIdentifier(team + ".com.acme.common");
        VT_REQUIRE(common.matched);
        VT_CHECK(common.confidence == Confidence::Exact);
        VT_CHECK(common.shared);
        VT_CHECK_EQ(common.sharedWith, names({"Acme Studio"}));

        // The group only we are entitled to stays exclusive.
        VT_CHECK(!engine.matchGroupIdentifier(team + ".com.acme.rocket.shared").shared);
    }
}

VT_TEST(candidate_stems_strip_known_decorations) {
    const auto contains = [](const std::vector<std::string>& list, const std::string& value) {
        return std::find(list.begin(), list.end(), value) != list.end();
    };
    const auto plist = MatchEngine::candidateStems("com.acme.Rocket.plist");
    VT_CHECK_EQ(plist.front(), std::string("com.acme.Rocket.plist"));
    VT_CHECK(contains(plist, "com.acme.Rocket"));
    VT_CHECK(contains(MatchEngine::candidateStems("com.acme.Rocket.plist.lockfile"), "com.acme.Rocket"));
    VT_CHECK(contains(MatchEngine::candidateStems(std::string("com.acme.Rocket.") + kUuid + ".plist"), "com.acme.Rocket"));
    VT_CHECK(contains(MatchEngine::candidateStems("com.acme.Rocket.savedState"), "com.acme.Rocket"));
    // Unknown extensions are left alone.
    VT_CHECK_EQ(MatchEngine::candidateStems("com.acme.Rocket.xyz"), names({"com.acme.Rocket.xyz"}));
    VT_CHECK_EQ(MatchEngine::candidateStems("Rocket"), names({"Rocket"}));
}

VT_TEST(top_most_paths_are_selected) {
    const std::vector<std::string> paths{"/a/b", "/a/b/c", "/a/bc", "/x/y", "/a/b", "/x", "/a/b c/d"};
    // "/a/b/c" is inside "/a/b"; the second "/a/b" duplicates the first; "/x/y" is inside "/x".
    VT_CHECK_EQ(topMostPathIndices(paths), (std::vector<std::size_t>{0, 2, 5, 6}));
    VT_CHECK_EQ(topMostPathIndices({}), std::vector<std::size_t>{});
    VT_CHECK_EQ(topMostPathIndices({"/only"}), std::vector<std::size_t>{0});
}

// ---------------------------------------------------------------------------
// Path policy
// ---------------------------------------------------------------------------

VT_TEST(path_policy_protects_system_and_shared_locations) {
    const PathPolicy policy("/Users/tester");
    for (const char* p : {"/System/Library/CoreServices/Finder.app", "/usr/bin/ssh", "/bin/ls", "/sbin/mount",
                          "/usr/lib/libc.dylib", "/Library/Apple/usr/bin/rvictl", "/System",
                          "/System/Volumes/Preboot/x", "/system/library/x"}) {
        if (policy.classifyLexically(p) != Protection::SipProtected) {
            vt::reportFailure(__FILE__, __LINE__, std::string("not SIP protected: ") + p);
        }
    }
    for (const char* p : {"/", "/Applications", "/Library", "/Users", "/Users/tester", "/Users/other",
                          "/Users/tester/Library", "/Users/tester/Library/Caches", "/Users/tester/Documents",
                          "/Users/tester/Library/Application Support", "/Users/tester/Library/Preferences/ByHost",
                          "/Library/LaunchDaemons", "/Library/Application Support", "/private/var/db/receipts",
                          "/usr/local", "/usr/local/bin", "/opt", "/private/etc", "/etc", "/var", "/tmp",
                          "/Volumes/External", "/Users/other/Library/Caches", "/users/tester/library/caches"}) {
        if (policy.classifyLexically(p) != Protection::ProtectedRoot) {
            vt::reportFailure(__FILE__, __LINE__, std::string("not a protected root: ") + p);
        }
    }
    for (const char* p : {"/Applications/Rocket.app", "/Users/tester/Library/Caches/com.acme.Rocket",
                          "/Library/LaunchDaemons/com.acme.rocketd.plist", "/usr/local/bin/rocketctl",
                          "/private/var/db/receipts/com.acme.pkg.Rocket.bom", "/Library/Acme",
                          "/System/Volumes/Data/Applications/Rocket.app", "/opt/acme/tool",
                          "/Users/tester/Library/Preferences/com.acme.Rocket.plist"}) {
        if (policy.classifyLexically(p) != Protection::None) {
            vt::reportFailure(__FILE__, __LINE__, std::string("wrongly protected: ") + p);
        }
    }
    for (const char* p : {"relative/path", "", "/a/../b", "/a/./b", "/a//b", "/a/b/", "Applications/Rocket.app"}) {
        if (policy.classifyLexically(p) != Protection::Invalid) {
            vt::reportFailure(__FILE__, __LINE__, std::string("not rejected as invalid: ") + p);
        }
    }

    // Re-rooted at a scratch directory the same rules apply below that directory.
    const PathPolicy scratch("/private/tmp/root/Users/tester", "/private/tmp/root");
    VT_CHECK(scratch.classifyLexically("/private/tmp/root/Library/Application Support") == Protection::ProtectedRoot);
    VT_CHECK(scratch.classifyLexically("/private/tmp/root/System/Library/x") == Protection::SipProtected);
    VT_CHECK(scratch.classifyLexically("/private/tmp/root/Users/tester/Library/Caches") == Protection::ProtectedRoot);
    VT_CHECK(scratch.classifyLexically("/private/tmp/root/Library/Application Support/Acme") == Protection::None);
    VT_CHECK(scratch.classifyLexically("/private/tmp/root") == Protection::ProtectedRoot);
    VT_CHECK(scratch.classifyLexically("/System/Library/x") == Protection::SipProtected);
    VT_CHECK(scratch.isProtectedRoot("/private/tmp/root/etc"));
}

// ---------------------------------------------------------------------------
// End-to-end scan of a synthetic system
// ---------------------------------------------------------------------------

namespace {

struct Fixture {
    vt::TempDir temp{"vortex-scan"};
    std::string root;
    std::string home;
    std::string userLibrary;
    std::string systemLibrary;
    std::string receipts;
    ScanRoots roots;
    AppInfo rocket;
    AppInfo pro;
    AppInfo studio;
    AppInfo helperApp;
    ThreadPool pool{4};

    Fixture() {
        root = temp.path();
        home = root + "/Users/tester";
        userLibrary = home + "/Library";
        systemLibrary = root + "/Library";
        receipts = root + "/private/var/db/receipts";

        roots.homeDir = home;
        roots.userLibrary = userLibrary;
        roots.systemLibrary = systemLibrary;
        roots.receiptDirs = {receipts};
        roots.legacyReceiptsDir = systemLibrary + "/Receipts";
        roots.userCacheDir = root + "/private/var/folders/xx/C";
        roots.userTempDir = root + "/private/var/folders/xx/T";
        roots.installRoot = root;

        const std::string apps = root + "/Applications";
        rocket = makeApp(vt::makeAppBundle(apps, "Rocket.app", "com.acme.Rocket", "Rocket"), "com.acme.Rocket",
                         "Rocket", "Rocket", kTeam);
        rocket.signing.applicationGroups = names({"ABCDE12345.com.acme.rocket.shared"});
        rocket.helperLabels = names({"com.acme.rocketd"});
        pro = makeApp(vt::makeAppBundle(apps, "Rocket Pro.app", "com.acme.Rocket.Pro", "Rocket Pro"),
                      "com.acme.Rocket.Pro", "Rocket Pro", "Rocket Pro", kTeam);
        studio = makeApp(vt::makeAppBundle(apps, "Acme Studio.app", "com.acme.Studio", "Acme Studio"),
                         "com.acme.Studio", "Acme Studio", "Acme Studio", kTeam);
        helperApp = makeApp(vt::makeAppBundle(apps, "Helper.app", "org.other.helper", "Helper"), "org.other.helper",
                            "Helper", "Helper");
    }

    [[nodiscard]] InstalledAppIndex index() const {
        return InstalledAppIndex(std::vector<AppInfo>{rocket, pro, studio, helperApp});
    }

    void userFile(const std::string& relative, std::size_t size = 100) const {
        vt::writeFileOfSize(userLibrary + "/" + relative, size);
    }
    void systemFile(const std::string& relative, std::size_t size = 100) const {
        vt::writeFileOfSize(systemLibrary + "/" + relative, size);
    }

    void writeReceipt(const std::string& packageId, const std::string& prefix, const vt::BomBuilder& bom) const {
        vt::writeFile(receipts + "/" + packageId + ".plist",
                      vt::plistXml({{"PackageIdentifier", packageId},
                                    {"InstallPrefixPath", prefix},
                                    {"PackageVersion", "1.0"},
                                    {"InstallProcessName", "Installer"}}));
        vt::writeFile(receipts + "/" + packageId + ".bom", bom.build());
    }
};

const Remnant* findRemnant(const ScanResult& result, const std::string& path) {
    for (const Remnant& remnant : result.remnants) {
        if (remnant.path == path) return &remnant;
    }
    return nullptr;
}

std::set<std::string> pathSet(const ScanResult& result) {
    std::set<std::string> paths;
    for (const Remnant& remnant : result.remnants) paths.insert(remnant.path);
    return paths;
}

void reportSetDifference(const std::set<std::string>& actual, const std::set<std::string>& expected,
                         const std::string& root) {
    for (const std::string& path : actual) {
        if (!expected.contains(path)) {
            vt::reportFailure(__FILE__, __LINE__, "FALSE POSITIVE: " + path.substr(root.size()));
        }
    }
    for (const std::string& path : expected) {
        if (!actual.contains(path)) {
            vt::reportFailure(__FILE__, __LINE__, "MISSED: " + path.substr(root.size()));
        }
    }
}

}  // namespace

VT_TEST(scanner_finds_exactly_the_remnants_of_the_target) {
    Fixture f;
    const std::string& U = f.userLibrary;
    const std::string& S = f.systemLibrary;
    const std::string R = f.root;
    std::set<std::string> expected{f.rocket.bundlePath};
    const auto expectUser = [&](const std::string& relative) { expected.insert(U + "/" + relative); };
    const auto expectSystem = [&](const std::string& relative) { expected.insert(S + "/" + relative); };

    // ---- Preferences ------------------------------------------------------
    f.userFile("Preferences/com.acme.Rocket.plist");
    expectUser("Preferences/com.acme.Rocket.plist");
    f.userFile("Preferences/com.acme.Rocket.Pro.plist");        // sibling
    f.userFile("Preferences/com.acme.Studio.plist");            // sibling
    f.userFile("Preferences/com.apple.Rocket.plist");           // Apple
    f.userFile("Preferences/com.acme.RocketLauncher.plist");    // look-alike
    f.userFile(std::string("Preferences/ByHost/com.acme.Rocket.") + kUuid + ".plist");
    expectUser(std::string("Preferences/ByHost/com.acme.Rocket.") + kUuid + ".plist");
    f.userFile(std::string("Preferences/ByHost/com.acme.Studio.") + kUuid + ".plist");

    // ---- Caches -----------------------------------------------------------
    f.userFile("Caches/com.acme.Rocket/Cache.db", 3000);
    expectUser("Caches/com.acme.Rocket");
    f.userFile("Caches/com.acme.Rocket.ShipIt/update.zip");
    expectUser("Caches/com.acme.Rocket.ShipIt");
    f.userFile("Caches/Helper/x");                              // stop-word folders belong to nobody
    f.userFile("Caches/Update/x");
    f.userFile("Caches/com.acme.Rocket.Pro/x");                 // sibling

    // ---- Application Support ----------------------------------------------
    f.userFile("Application Support/Rocket/state.json");
    expectUser("Application Support/Rocket");
    f.userFile("Application Support/Rocket Pro/state.json");    // sibling by name
    f.userFile("Application Support/App/x");
    f.userFile("Application Support/Agent/x");
    f.userFile("Application Support/Acme/Rocket/licence.key");  // inside the shared vendor folder
    f.userFile("Application Support/Acme/Studio/project.db");
    f.userFile("Application Support/Acme/Shared/fonts.bin");
    expectUser("Application Support/Acme/Rocket");
    expectUser("Application Support/Acme");                     // listed, but shared and unchecked
    f.userFile("Application Support/com.apple.sharedfilelist/"
               "com.apple.LSSharedFileList.ApplicationRecentDocuments/com.acme.rocket.sfl3");
    expectUser("Application Support/com.apple.sharedfilelist/"
               "com.apple.LSSharedFileList.ApplicationRecentDocuments/com.acme.rocket.sfl3");
    f.userFile("Application Support/com.apple.sharedfilelist/"
               "com.apple.LSSharedFileList.ApplicationRecentDocuments/com.acme.studio.sfl3");
    f.userFile(std::string("Application Support/CrashReporter/Rocket_") + kUuid + ".plist");
    expectUser(std::string("Application Support/CrashReporter/Rocket_") + kUuid + ".plist");
    f.userFile(std::string("Application Support/CrashReporter/Helper_") + kUuid + ".plist");

    // ---- State, containers, web data --------------------------------------
    f.userFile("Saved Application State/com.acme.Rocket.savedState/windows.plist");
    expectUser("Saved Application State/com.acme.Rocket.savedState");
    f.userFile("Saved Application State/com.acme.Rocket.Pro.savedState/windows.plist");
    f.userFile("Containers/com.acme.Rocket/Data/file");
    expectUser("Containers/com.acme.Rocket");
    f.userFile("Containers/com.acme.Rocket.ShareExtension/Data/file");
    expectUser("Containers/com.acme.Rocket.ShareExtension");
    vt::writeFile(U + "/Containers/9A8B7C6D-1111-2222-3333-444455556666/.com.apple.containermanagerd.metadata.plist",
                  vt::plistXml({{"MCMMetadataIdentifier", "com.acme.Rocket"}}));
    expectUser("Containers/9A8B7C6D-1111-2222-3333-444455556666");
    vt::writeFile(U + "/Containers/0A0B0C0D-1111-2222-3333-444455556666/.com.apple.containermanagerd.metadata.plist",
                  vt::plistXml({{"MCMMetadataIdentifier", "org.other.thing"}}));
    f.userFile("Containers/com.apple.Notes/Data/file");
    f.userFile("Group Containers/ABCDE12345.com.acme.rocket.shared/db");
    expectUser("Group Containers/ABCDE12345.com.acme.rocket.shared");
    f.userFile("Group Containers/ABCDE12345.suite/db");         // same team as the siblings
    expectUser("Group Containers/ABCDE12345.suite");
    f.userFile("Group Containers/ZZZZZ99999.other/db");
    f.userFile("Group Containers/group.com.apple.notes/db");
    f.userFile("WebKit/com.acme.Rocket/WebsiteData/x");
    expectUser("WebKit/com.acme.Rocket");
    f.userFile("HTTPStorages/com.acme.Rocket/httpstorages.sqlite");
    expectUser("HTTPStorages/com.acme.Rocket");
    f.userFile("HTTPStorages/com.acme.Rocket.binarycookies");
    expectUser("HTTPStorages/com.acme.Rocket.binarycookies");
    f.userFile("Cookies/com.acme.Rocket.binarycookies");
    expectUser("Cookies/com.acme.Rocket.binarycookies");
    f.userFile("Cookies/com.acme.Studio.binarycookies");
    f.userFile("Application Scripts/com.acme.Rocket/script.scpt");
    expectUser("Application Scripts/com.acme.Rocket");

    // ---- Logs -------------------------------------------------------------
    f.userFile("Logs/Rocket/rocket.log");
    expectUser("Logs/Rocket");
    f.userFile("Logs/Updater/x");
    f.userFile("Logs/DiagnosticReports/Rocket-2026-01-02-030405.ips");
    expectUser("Logs/DiagnosticReports/Rocket-2026-01-02-030405.ips");
    f.userFile("Logs/DiagnosticReports/RocketLauncher-2026-01-02-030405.ips");
    f.userFile("Logs/DiagnosticReports/Helper-2026-01-02-030405.ips");

    // ---- launchd ----------------------------------------------------------
    const std::string agentProgram = f.rocket.bundlePath + "/Contents/MacOS/agent";
    vt::writeFile(U + "/LaunchAgents/com.acme.Rocket.agent.plist",
                  vt::plistXml({{"Label", "com.acme.Rocket.agent"}, {"Program", agentProgram}}));
    expectUser("LaunchAgents/com.acme.Rocket.agent.plist");
    vt::writeFile(U + "/LaunchAgents/com.other.updater.plist",
                  vt::plistXml({{"Label", "com.other.updater"}, {"Program", "/usr/bin/true"}}));
    // An Apple-labelled job is never attributed, even if it launches something in our bundle.
    vt::writeFile(U + "/LaunchAgents/com.apple.rocket.plist",
                  vt::plistXml({{"Label", "com.apple.rocket"}, {"Program", agentProgram}}));
    vt::writeFile(U + "/LaunchAgents/custom-name.plist",
                  vt::plistXml({{"Label", "custom.job"}},
                               "  <key>AssociatedBundleIdentifiers</key>\n"
                               "  <array><string>com.acme.Rocket</string></array>\n"));
    expectUser("LaunchAgents/custom-name.plist");
    vt::writeFile(S + "/LaunchDaemons/com.acme.rocketd.plist",
                  vt::plistXml({{"Label", "com.acme.rocketd"},
                                {"Program", S + "/PrivilegedHelperTools/com.acme.rocketd"}}));
    expectSystem("LaunchDaemons/com.acme.rocketd.plist");
    vt::writeFile(S + "/LaunchDaemons/com.apple.something.plist", vt::plistXml({{"Label", "com.apple.something"}}));
    f.systemFile("PrivilegedHelperTools/com.acme.rocketd");
    expectSystem("PrivilegedHelperTools/com.acme.rocketd");
    f.systemFile("PrivilegedHelperTools/org.other.helperd");
    // A daemon tied to the app only through AssociatedBundleIdentifiers: its plist
    // matches, and the helper it runs comes along although its name says nothing.
    vt::writeFile(S + "/LaunchDaemons/net.updates.broker.plist",
                  vt::plistXml({{"Label", "net.updates.broker"}, {"Program", S + "/PrivilegedHelperTools/broker-tool"}},
                               "  <key>AssociatedBundleIdentifiers</key>\n"
                               "  <array><string>com.acme.Rocket</string></array>\n"));
    expectSystem("LaunchDaemons/net.updates.broker.plist");
    f.systemFile("PrivilegedHelperTools/broker-tool");
    expectSystem("PrivilegedHelperTools/broker-tool");
    // A job of ours that launches a shared interpreter does not make the interpreter ours.
    vt::writeFile(S + "/LaunchDaemons/com.acme.Rocket.shell.plist",
                  vt::plistXml({{"Label", "com.acme.Rocket.shell"}, {"Program", R + "/bin/sh"}}));
    expectSystem("LaunchDaemons/com.acme.Rocket.shell.plist");
    vt::writeFileOfSize(R + "/bin/sh", 10);
    // An unrelated daemon and its helper stay untouched.
    vt::writeFile(S + "/LaunchDaemons/org.other.daemon.plist",
                  vt::plistXml({{"Label", "org.other.daemon"}, {"Program", S + "/PrivilegedHelperTools/org.other.helperd"}}));

    // ---- Plug-in bundles --------------------------------------------------
    vt::writeFile(U + "/QuickLook/RocketPreview.qlgenerator/Contents/Info.plist",
                  vt::plistXml({{"CFBundleIdentifier", "com.acme.Rocket.qlgenerator"}}));
    expectUser("QuickLook/RocketPreview.qlgenerator");
    vt::makeDirs(U + "/PreferencePanes/Rocket.prefPane/Contents");
    expectUser("PreferencePanes/Rocket.prefPane");
    vt::writeFile(U + "/Frameworks/Unrelated.framework/Resources/Info.plist",
                  vt::plistXml({{"CFBundleIdentifier", "org.other.framework"}}));

    // ---- System Library and per-user cache/temp ----------------------------
    f.systemFile("Application Support/Rocket/shared.db");
    expectSystem("Application Support/Rocket");
    f.systemFile("Preferences/com.acme.Rocket.plist");
    expectSystem("Preferences/com.acme.Rocket.plist");
    f.systemFile("Preferences/com.acme.Studio.plist");
    vt::writeFileOfSize(f.roots.userCacheDir + "/com.acme.Rocket/cache.bin", 10);
    expected.insert(f.roots.userCacheDir + "/com.acme.Rocket");
    vt::writeFileOfSize(f.roots.userTempDir + "/com.acme.Rocket/tmp.bin", 10);
    expected.insert(f.roots.userTempDir + "/com.acme.Rocket");
    vt::writeFileOfSize(f.roots.userTempDir + "/com.apple.something/tmp.bin", 10);

    // ---- Package receipts -------------------------------------------------
    // (1) The package that installed the app: an exact, verified receipt.
    vt::BomBuilder main;
    main.dir("./Applications/Rocket.app/Contents/MacOS")
        .file("./Applications/Rocket.app/Contents/MacOS/Rocket", 10)
        .file("./Library/Application Support/RocketEngine/engine.dylib", 500)
        .file("./Library/Application Support/RocketEngine/data/model.bin", 900)
        .file("./Library/Frameworks/Shared.framework/Versions/A/Shared", 700)
        .file("./usr/local/bin/rocketctl", 50)
        .file("./usr/local/bin/rocket-only", 60)
        .file("./Library/LaunchDaemons/com.acme.rocketd.plist", 200)
        .file("./etc/rocket.conf", 30)
        .file("./System/Library/Extensions/Rocket.kext/Contents/Info.plist", 40)
        .file("./Library/Application Support/com.apple.RocketBridge.plist", 20);
    f.writeReceipt("com.acme.pkg.Rocket", "", main);
    expected.insert(f.receipts + "/com.acme.pkg.Rocket.plist");
    expected.insert(f.receipts + "/com.acme.pkg.Rocket.bom");

    f.systemFile("Application Support/RocketEngine/engine.dylib", 500);
    f.systemFile("Application Support/RocketEngine/data/model.bin", 900);
    expectSystem("Application Support/RocketEngine");          // fully covered: offered as one unit
    f.systemFile("Frameworks/Shared.framework/Versions/A/Shared", 700);
    f.systemFile("Frameworks/Shared.framework/Versions/B/Other", 700);  // foreign content
    expectSystem("Frameworks/Shared.framework/Versions/A");    // only the covered part
    vt::writeFileOfSize(R + "/usr/local/bin/rocketctl", 50);
    vt::writeFileOfSize(R + "/usr/local/bin/rocket-only", 60);
    vt::writeFileOfSize(R + "/usr/local/bin/unrelated", 60);
    expected.insert(R + "/usr/local/bin/rocketctl");           // also in another package: shared
    expected.insert(R + "/usr/local/bin/rocket-only");
    vt::writeFileOfSize(R + "/etc/rocket.conf", 30);
    expected.insert(R + "/etc/rocket.conf");
    vt::writeFileOfSize(R + "/System/Library/Extensions/Rocket.kext/Contents/Info.plist", 40);  // SIP location
    f.systemFile("Application Support/com.apple.RocketBridge.plist", 20);
    expectSystem("Application Support/com.apple.RocketBridge.plist");  // only thanks to the exact receipt

    // (2) An unrelated package that installed one of the same files.
    vt::BomBuilder tools;
    tools.file("./usr/local/bin/rocketctl", 50).file("./usr/local/bin/othertool", 10);
    f.writeReceipt("org.other.pkg.tools", "/", tools);
    vt::writeFileOfSize(R + "/usr/local/bin/othertool", 10);

    // (3) An Apple receipt that happens to list the app: never evidence.
    vt::BomBuilder apple;
    apple.dir("./Applications/Rocket.app").file("./Library/Application Support/AppleThing/x", 5);
    f.writeReceipt("com.apple.pkg.Something", "", apple);
    f.systemFile("Application Support/AppleThing/x", 5);

    // (4) A helper package matched only by its identifier: high confidence, so
    //     its Apple-named payload is refused while its own folder is accepted.
    vt::BomBuilder helper;
    helper.file("./opt/rocket/tool", 77).file("./Library/Application Support/com.apple.helperbits", 5);
    f.writeReceipt("com.acme.Rocket.helperpkg", "", helper);
    expected.insert(f.receipts + "/com.acme.Rocket.helperpkg.plist");
    expected.insert(f.receipts + "/com.acme.Rocket.helperpkg.bom");
    vt::writeFileOfSize(R + "/opt/rocket/tool", 77);
    expected.insert(R + "/opt/rocket");
    f.systemFile("Application Support/com.apple.helperbits", 5);

    // ---- Scan -------------------------------------------------------------
    const InstalledAppIndex index = f.index();
    const RemnantScanner scanner(f.roots, f.pool);
    std::atomic<int> discoveredCalls{0};
    std::atomic<int> sizedCalls{0};
    std::size_t discoveredCount = 0;
    ScanCallbacks callbacks;
    callbacks.onDiscovered = [&](const std::vector<Remnant>& remnants) {
        ++discoveredCalls;
        discoveredCount = remnants.size();
    };
    callbacks.onSized = [&](std::size_t, const Remnant&) { ++sizedCalls; };

    const ScanResult result = scanner.scan(f.rocket, index, CancellationToken{}, ScanOptions{}, callbacks);
    VT_CHECK(!result.cancelled);
    VT_CHECK(!result.permissionDenied);
    reportSetDifference(pathSet(result), expected, R);
    VT_CHECK_EQ(result.remnants.size(), expected.size());  // no duplicates either
    VT_CHECK_EQ(discoveredCalls.load(), 1);
    VT_CHECK_EQ(discoveredCount, result.remnants.size());
    VT_CHECK_EQ(sizedCalls.load(), static_cast<int>(result.remnants.size()));

    // The application itself comes first.
    VT_REQUIRE(!result.remnants.empty());
    VT_CHECK_EQ(result.remnants.front().path, f.rocket.bundlePath);
    VT_CHECK(result.remnants.front().category == RemnantCategory::AppBundle);
    VT_CHECK(result.remnants.front().reason == MatchReason::TargetBundle);
    VT_CHECK(result.remnants.front().defaultSelected);

    const auto check = [&](const std::string& path, RemnantCategory category, Confidence confidence,
                           MatchReason reason, bool shared) {
        const Remnant* remnant = findRemnant(result, path);
        if (remnant == nullptr) {
            vt::reportFailure(__FILE__, __LINE__, "not found: " + path.substr(R.size()));
            return;
        }
        const bool ok = remnant->category == category && remnant->confidence == confidence &&
                        remnant->reason == reason && remnant->isShared == shared &&
                        remnant->defaultSelected == !shared && remnant->usageKnown && !remnant->sipProtected;
        if (!ok) {
            vt::reportFailure(__FILE__, __LINE__,
                              path.substr(R.size()) + ": got " + displayName(remnant->category) + " / " +
                                  displayName(remnant->confidence) + " / " + displayName(remnant->reason) +
                                  (remnant->isShared ? " / shared" : " / exclusive") +
                                  (remnant->defaultSelected ? " / checked" : " / unchecked"));
        }
    };
    using C = RemnantCategory;
    using M = MatchReason;
    check(U + "/Preferences/com.acme.Rocket.plist", C::Preferences, Confidence::Exact, M::ExactBundleId, false);
    check(U + std::string("/Preferences/ByHost/com.acme.Rocket.") + kUuid + ".plist", C::Preferences,
          Confidence::Exact, M::ExactBundleId, false);
    check(U + "/Caches/com.acme.Rocket", C::Caches, Confidence::Exact, M::ExactBundleId, false);
    check(U + "/Caches/com.acme.Rocket.ShipIt", C::Caches, Confidence::High, M::BundleIdPrefix, false);
    check(U + "/Application Support/Rocket", C::ApplicationSupport, Confidence::Medium, M::AppNameMatch, false);
    check(U + "/Application Support/Acme/Rocket", C::ApplicationSupport, Confidence::Medium, M::AppNameMatch, false);
    check(U + "/Application Support/Acme", C::ApplicationSupport, Confidence::Medium, M::VendorDirectory, true);
    check(U + "/Saved Application State/com.acme.Rocket.savedState", C::SavedState, Confidence::Exact,
          M::ExactBundleId, false);
    check(U + "/Containers/com.acme.Rocket", C::Containers, Confidence::Exact, M::ExactBundleId, false);
    check(U + "/Containers/com.acme.Rocket.ShareExtension", C::Containers, Confidence::High, M::BundleIdPrefix, false);
    check(U + "/Containers/9A8B7C6D-1111-2222-3333-444455556666", C::Containers, Confidence::Exact,
          M::ContainerMetadata, false);
    check(U + "/Group Containers/ABCDE12345.com.acme.rocket.shared", C::GroupContainers, Confidence::Exact,
          M::EntitledAppGroup, false);
    check(U + "/Group Containers/ABCDE12345.suite", C::GroupContainers, Confidence::Medium,
          M::TeamIdGroupContainer, true);
    check(U + "/Logs/Rocket", C::Logs, Confidence::Medium, M::AppNameMatch, false);
    check(U + "/Logs/DiagnosticReports/Rocket-2026-01-02-030405.ips", C::Logs, Confidence::Medium,
          M::ExecutableNameHeuristic, false);
    check(U + "/WebKit/com.acme.Rocket", C::WebKit, Confidence::Exact, M::ExactBundleId, false);
    check(U + "/HTTPStorages/com.acme.Rocket", C::HTTPStorages, Confidence::Exact, M::ExactBundleId, false);
    check(U + "/Cookies/com.acme.Rocket.binarycookies", C::Cookies, Confidence::Exact, M::ExactBundleId, false);
    check(U + "/Application Scripts/com.acme.Rocket", C::ApplicationScripts, Confidence::Exact, M::ExactBundleId, false);
    check(U + "/LaunchAgents/com.acme.Rocket.agent.plist", C::LaunchAgents, Confidence::Exact,
          M::LaunchdProgramInBundle, false);
    check(U + "/LaunchAgents/custom-name.plist", C::LaunchAgents, Confidence::Exact, M::LaunchdAssociatedBundleId, false);
    check(U + "/QuickLook/RocketPreview.qlgenerator", C::QuickLook, Confidence::High, M::BundleIdPrefix, false);
    check(U + "/PreferencePanes/Rocket.prefPane", C::PreferencePanes, Confidence::Medium, M::AppNameMatch, false);
    check(S + "/PrivilegedHelperTools/com.acme.rocketd", C::PrivilegedHelperTools, Confidence::Exact, M::HelperLabel, false);
    check(S + "/LaunchDaemons/net.updates.broker.plist", C::LaunchDaemons, Confidence::Exact,
          M::LaunchdAssociatedBundleId, false);
    check(S + "/PrivilegedHelperTools/broker-tool", C::PrivilegedHelperTools, Confidence::High, M::LaunchdJobProgram, false);
    check(S + "/LaunchDaemons/com.acme.Rocket.shell.plist", C::LaunchDaemons, Confidence::High, M::BundleIdPrefix, false);
    check(S + "/Application Support/RocketEngine", C::ApplicationSupport, Confidence::Exact, M::BomReceipt, false);
    check(S + "/Frameworks/Shared.framework/Versions/A", C::Frameworks, Confidence::Exact, M::BomReceipt, false);
    check(R + "/usr/local/bin/rocket-only", C::PackagePayload, Confidence::Exact, M::BomReceipt, false);
    check(R + "/usr/local/bin/rocketctl", C::PackagePayload, Confidence::Exact, M::BomReceipt, true);
    check(R + "/etc/rocket.conf", C::PackagePayload, Confidence::Exact, M::BomReceipt, false);
    check(S + "/Application Support/com.apple.RocketBridge.plist", C::ApplicationSupport, Confidence::Exact,
          M::BomReceipt, false);
    check(f.receipts + "/com.acme.pkg.Rocket.bom", C::Receipts, Confidence::Exact, M::BomReceipt, false);
    check(f.receipts + "/com.acme.Rocket.helperpkg.plist", C::Receipts, Confidence::High, M::ReceiptPackageId, false);
    check(R + "/opt/rocket", C::PackagePayload, Confidence::High, M::BomReceipt, false);
    check(f.roots.userCacheDir + "/com.acme.Rocket", C::Caches, Confidence::Exact, M::ExactBundleId, false);

    // Details that matter to the UI and to deletion.
    const Remnant* vendor = findRemnant(result, U + "/Application Support/Acme");
    VT_REQUIRE(vendor != nullptr);
    VT_CHECK_EQ(vendor->sharedWith, names({"Rocket Pro", "Acme Studio"}));
    VT_CHECK(!vendor->warning.empty());
    const Remnant* teamGroup = findRemnant(result, U + "/Group Containers/ABCDE12345.suite");
    VT_REQUIRE(teamGroup != nullptr);
    VT_CHECK_EQ(teamGroup->sharedWith, names({"Rocket Pro", "Acme Studio"}));
    const Remnant* sharedTool = findRemnant(result, R + "/usr/local/bin/rocketctl");
    VT_REQUIRE(sharedTool != nullptr);
    VT_CHECK_EQ(sharedTool->sharedWith, names({"package org.other.pkg.tools"}));
    VT_CHECK_EQ(sharedTool->receiptPackageId, std::string("com.acme.pkg.Rocket"));

    const Remnant* agent = findRemnant(result, U + "/LaunchAgents/com.acme.Rocket.agent.plist");
    VT_REQUIRE(agent != nullptr);
    VT_CHECK_EQ(agent->launchdLabel, std::string("com.acme.Rocket.agent"));
    const Remnant* associated = findRemnant(result, U + "/LaunchAgents/custom-name.plist");
    VT_REQUIRE(associated != nullptr);
    VT_CHECK_EQ(associated->launchdLabel, std::string("custom.job"));
    const Remnant* daemon = findRemnant(result, S + "/LaunchDaemons/com.acme.rocketd.plist");
    VT_REQUIRE(daemon != nullptr);
    VT_CHECK(daemon->confidence == Confidence::Exact);
    VT_CHECK(daemon->category == RemnantCategory::LaunchDaemons);
    VT_CHECK_EQ(daemon->launchdLabel, std::string("com.acme.rocketd"));

    const Remnant* cache = findRemnant(result, U + "/Caches/com.acme.Rocket");
    VT_REQUIRE(cache != nullptr);
    VT_CHECK(cache->isDirectory);
    VT_CHECK_EQ(cache->usage.logicalBytes, std::uint64_t{3000});
    VT_CHECK(cache->domain == Domain::User);
    VT_CHECK(!cache->requiresRoot);
    const Remnant* engineDir = findRemnant(result, S + "/Application Support/RocketEngine");
    VT_REQUIRE(engineDir != nullptr);
    VT_CHECK_EQ(engineDir->usage.logicalBytes, std::uint64_t{1400});
    VT_CHECK(engineDir->domain == Domain::System);

    VT_CHECK_EQ(result.matchedPackageIds, names({"com.acme.Rocket.helperpkg", "com.acme.pkg.Rocket"}));

    // Nothing that was scanned may have been modified.
    VT_CHECK(vt::exists(U + "/Preferences/com.acme.Rocket.plist"));
    VT_CHECK(vt::exists(S + "/Application Support/RocketEngine/engine.dylib"));
}

VT_TEST(sandbox_roots_stay_inside_their_directory) {
    vt::TempDir temp{"vortex-sandbox"};
    const std::string root = temp.path();
    VT_CHECK(!ScanRoots::sandbox("/").ok());
    VT_CHECK(!ScanRoots::sandbox(root + "/missing").ok());
    VT_CHECK(!ScanRoots::systemDefault().isSandbox());

    const auto sandbox = ScanRoots::sandbox(root + "/");
    VT_REQUIRE(sandbox.ok());
    const ScanRoots& roots = sandbox.value();
    VT_CHECK(roots.isSandbox());
    VT_CHECK(!roots.consultLaunchServices);
    std::vector<std::string> locations{roots.homeDir,           roots.userLibrary,  roots.systemLibrary,
                                       roots.legacyReceiptsDir, roots.userCacheDir, roots.userTempDir};
    locations.insert(locations.end(), roots.receiptDirs.begin(), roots.receiptDirs.end());
    for (const std::string& location : locations) VT_CHECK(vortex::path::isStrictlyWithin(location, roots.installRoot));
    for (const std::string& location : BundleInspector::defaultSearchRoots(roots.homeDir, roots.installRoot)) {
        VT_CHECK(vortex::path::isStrictlyWithin(location, roots.installRoot));
    }
    for (const std::string& location : BundleInspector::auxiliarySearchRoots(roots.installRoot)) {
        VT_CHECK(vortex::path::isStrictlyWithin(location, roots.installRoot));
    }

    // People see the synthetic disk as if it were the real one.
    VT_CHECK_EQ(roots.displayPath(root + "/Applications/Rocket.app"), std::string("/Applications/Rocket.app"));
    VT_CHECK_EQ(roots.displayPath(roots.homeDir + "/Library/Caches/x"), std::string("~/Library/Caches/x"));
    VT_CHECK_EQ(roots.displayPath("/elsewhere/x"), std::string("/elsewhere/x"));
}

VT_TEST(scanner_resolves_launchd_programs_inside_a_sandbox) {
    vt::TempDir temp{"vortex-sandbox"};
    const auto sandbox = ScanRoots::sandbox(temp.path());
    VT_REQUIRE(sandbox.ok());
    const ScanRoots& roots = sandbox.value();
    const std::string bundle =
        vt::makeAppBundle(roots.installRoot + "/Applications", "Rocket.app", "com.acme.Rocket", "Rocket");
    const AppInfo rocket = makeApp(bundle, "com.acme.Rocket", "Rocket", "Rocket");
    // The job names its program the way it would on a real disk, without the sandbox prefix.
    const std::string job = roots.userLibrary + "/LaunchAgents/net.unrelated.updater.plist";
    vt::writeFile(job, vt::plistXml({{"Label", "net.unrelated.updater"},
                                     {"Program", "/Applications/Rocket.app/Contents/MacOS/Rocket"}}));
    ThreadPool pool{2};
    const ScanResult result = RemnantScanner(roots, pool).scan(
        rocket, InstalledAppIndex(std::vector<AppInfo>{rocket}), CancellationToken{});
    const Remnant* found = findRemnant(result, job);
    VT_REQUIRE(found != nullptr);
    VT_CHECK(found->reason == MatchReason::LaunchdProgramInBundle);
}

VT_TEST(scanner_never_offers_a_folder_that_holds_an_application) {
    // A suite installed into its own folder: the receipt lists the folder, both
    // applications and a loose file, so the folder is "fully covered" — and must
    // still never be offered as a unit, because that would remove the sibling too.
    Fixture f;
    const std::string suite = f.root + "/Applications/Acme Suite";
    const AppInfo rocket = makeApp(vt::makeAppBundle(suite, "Rocket.app", "com.acme.suite.Rocket", "Rocket"),
                                   "com.acme.suite.Rocket", "Rocket", "Rocket", kTeam);
    const AppInfo studio = makeApp(vt::makeAppBundle(suite, "Studio.app", "com.acme.suite.Studio", "Studio"),
                                   "com.acme.suite.Studio", "Studio", "Studio", kTeam);
    vt::writeFileOfSize(suite + "/ReadMe.rtf", 300);
    vt::writeFileOfSize(suite + "/Extras/sample.dat", 400);

    vt::BomBuilder bom;
    for (const char* app : {"Rocket", "Studio"}) {
        const std::string base = std::string("./Applications/Acme Suite/") + app + ".app/Contents";
        bom.file(base + "/Info.plist", 100)
            .file(base + "/MacOS/" + app, 100)
            .file(base + "/Resources/data.bin", 20000);
    }
    bom.file("./Applications/Acme Suite/ReadMe.rtf", 300).file("./Applications/Acme Suite/Extras/sample.dat", 400);
    f.writeReceipt("com.acme.pkg.suite", "", bom);

    const RemnantScanner scanner(f.roots, f.pool);
    {
        const InstalledAppIndex index(std::vector<AppInfo>{rocket, studio});
        const ScanResult result = scanner.scan(rocket, index, CancellationToken{});
        const std::set<std::string> expected{
            rocket.bundlePath,
            studio.bundlePath,          // listed as a separate application: shared, unchecked
            suite + "/ReadMe.rtf",      // payload shared with the sibling
            suite + "/Extras",          // a plain covered folder is still offered as one unit
            f.receipts + "/com.acme.pkg.suite.plist",
            f.receipts + "/com.acme.pkg.suite.bom",
        };
        reportSetDifference(pathSet(result), expected, f.root);
        VT_CHECK(findRemnant(result, suite) == nullptr);
        for (const Remnant& remnant : result.remnants) {
            if (remnant.path == rocket.bundlePath) continue;
            VT_CHECK(remnant.isShared);          // everything else came with a suite that is still in use
            VT_CHECK(!remnant.defaultSelected);
        }
        const Remnant* sibling = findRemnant(result, studio.bundlePath);
        VT_REQUIRE(sibling != nullptr);
        VT_CHECK_EQ(sibling->sharedWith, names({"Studio"}));
    }
    {
        // With the sibling uninstalled the loose payload is the target's alone —
        // but the folder that holds the target is still not offered as a unit.
        vt::TempDir trash("vortex-scan-moved");
        VT_REQUIRE(::rename(studio.bundlePath.c_str(), trash.sub("Studio.app").c_str()) == 0);
        const InstalledAppIndex index(std::vector<AppInfo>{rocket});
        const ScanResult result = scanner.scan(rocket, index, CancellationToken{});
        const std::set<std::string> expected{
            rocket.bundlePath,
            suite + "/ReadMe.rtf",
            suite + "/Extras",
            f.receipts + "/com.acme.pkg.suite.plist",
            f.receipts + "/com.acme.pkg.suite.bom",
        };
        reportSetDifference(pathSet(result), expected, f.root);
        const Remnant* readme = findRemnant(result, suite + "/ReadMe.rtf");
        VT_REQUIRE(readme != nullptr);
        VT_CHECK(readme->defaultSelected);
        VT_CHECK(!readme->isShared);
    }
}

VT_TEST(scanner_treats_another_installed_copy_as_owner_of_the_data) {
    Fixture f;
    f.userFile("Preferences/com.acme.Rocket.plist");
    f.userFile("Caches/com.acme.Rocket/x");
    // A second copy of the very same application, e.g. one left in ~/Downloads.
    const AppInfo copy = makeApp(vt::makeAppBundle(f.home + "/Downloads", "Rocket.app", "com.acme.Rocket", "Rocket"),
                                 "com.acme.Rocket", "Rocket", "Rocket", kTeam);
    const InstalledAppIndex index(std::vector<AppInfo>{f.rocket, f.pro, copy});
    const RemnantScanner scanner(f.roots, f.pool);

    const ScanResult result = scanner.scan(copy, index, CancellationToken{});
    VT_REQUIRE(result.remnants.size() == 3);
    VT_CHECK_EQ(result.remnants.front().path, copy.bundlePath);
    VT_CHECK(result.remnants.front().defaultSelected);  // the copy itself may go
    for (std::size_t i = 1; i < result.remnants.size(); ++i) {
        VT_CHECK(result.remnants[i].isShared);
        VT_CHECK(!result.remnants[i].defaultSelected);
        VT_CHECK(result.remnants[i].warning.find(f.rocket.bundlePath) != std::string::npos);
    }
    VT_CHECK(!result.notes.empty());
}

VT_TEST(scanner_never_attributes_apple_components) {
    Fixture f;
    AppInfo xcode = makeApp(vt::makeAppBundle(f.root + "/Applications", "Xcode.app", "com.apple.dt.Xcode", "Xcode"),
                            "com.apple.dt.Xcode", "Xcode", "Xcode");
    xcode.isAppleApp = true;
    f.userFile("Preferences/com.apple.dt.Xcode.plist");
    f.userFile("Caches/com.apple.dt.Xcode/x");
    f.userFile("Saved Application State/com.apple.dt.Xcode.savedState/x");
    f.userFile("Containers/com.apple.dt.Xcode/x");
    f.userFile("Application Support/Apple/x");
    f.userFile("Application Support/Xcode/x");
    vt::BomBuilder bom;
    bom.dir("./Applications/Xcode.app").file("./Library/Developer/PrivateFrameworks/x", 5);
    f.writeReceipt("com.apple.pkg.Xcode", "", bom);
    vt::writeFileOfSize(f.systemLibrary + "/Developer/PrivateFrameworks/x", 5);

    const InstalledAppIndex index(std::vector<AppInfo>{xcode, f.rocket});
    const RemnantScanner scanner(f.roots, f.pool);
    const ScanResult result = scanner.scan(xcode, index, CancellationToken{});

    const std::set<std::string> expected{xcode.bundlePath, f.userLibrary + "/Application Support/Xcode"};
    reportSetDifference(pathSet(result), expected, f.root);
    VT_CHECK(result.matchedPackageIds.empty());
    VT_CHECK(!result.notes.empty());
}

VT_TEST(scanner_locks_sip_protected_applications) {
    Fixture f;
    f.userFile("Preferences/com.acme.Rocket.plist");
    AppInfo locked = f.rocket;
    locked.isSipProtected = true;
    const InstalledAppIndex index = f.index();
    const RemnantScanner scanner(f.roots, f.pool);
    const ScanResult result = scanner.scan(locked, index, CancellationToken{});

    VT_REQUIRE(result.remnants.size() == 1);  // nothing else is even looked for
    VT_CHECK(result.remnants.front().sipProtected);
    VT_CHECK(!result.remnants.front().defaultSelected);
    VT_CHECK(!result.remnants.front().requiresRoot);
    VT_CHECK(!result.notes.empty());
}

VT_TEST(scanner_reports_privacy_denials) {
    if (::geteuid() == 0) {
        vt::note("running as root: permission denials cannot be provoked");
        return;
    }
    Fixture f;
    f.userFile("Containers/com.acme.Rocket/Data/secret.db", 4096);
    f.userFile("Caches/com.acme.Rocket/x");
    VT_REQUIRE(::chmod((f.userLibrary + "/Containers/com.acme.Rocket").c_str(), 0000) == 0);
    vt::makeDirs(f.userLibrary + "/Cookies");
    VT_REQUIRE(::chmod((f.userLibrary + "/Cookies").c_str(), 0000) == 0);

    const InstalledAppIndex index = f.index();
    const RemnantScanner scanner(f.roots, f.pool);
    const ScanResult result = scanner.scan(f.rocket, index, CancellationToken{});

    VT_CHECK(result.permissionDenied);
    const Remnant* container = findRemnant(result, f.userLibrary + "/Containers/com.acme.Rocket");
    VT_REQUIRE(container != nullptr);
    VT_CHECK(container->permissionDenied);
    const auto denied = [&](const std::string& path) {
        return std::find(result.deniedPaths.begin(), result.deniedPaths.end(), path) != result.deniedPaths.end();
    };
    VT_CHECK(denied(f.userLibrary + "/Containers/com.acme.Rocket"));
    VT_CHECK(denied(f.userLibrary + "/Cookies"));  // a location that could not even be listed

    ::chmod((f.userLibrary + "/Containers/com.acme.Rocket").c_str(), 0700);
    ::chmod((f.userLibrary + "/Cookies").c_str(), 0700);
}

VT_TEST(scanner_stops_promptly_when_cancelled) {
    Fixture f;
    for (int i = 0; i < 50; ++i) f.userFile("Caches/com.acme.Rocket/f" + std::to_string(i), 2000);
    f.userFile("Preferences/com.acme.Rocket.plist");
    const InstalledAppIndex index = f.index();
    const RemnantScanner scanner(f.roots, f.pool);

    // Cancelled before it starts.
    {
        const CancellationToken token;
        token.cancel();
        bool discovered = false;
        ScanCallbacks callbacks;
        callbacks.onDiscovered = [&](const std::vector<Remnant>&) { discovered = true; };
        const ScanResult result = scanner.scan(f.rocket, index, token, ScanOptions{}, callbacks);
        VT_CHECK(result.cancelled);
        VT_CHECK(!discovered);
        VT_CHECK(result.elapsedSeconds < 0.5);
    }
    // Cancelled from a callback in the middle of the run: sizing is abandoned.
    {
        const CancellationToken token;
        ScanCallbacks callbacks;
        callbacks.onDiscovered = [&](const std::vector<Remnant>&) { token.cancel(); };
        const ScanResult result = scanner.scan(f.rocket, index, token, ScanOptions{}, callbacks);
        VT_CHECK(result.cancelled);
        VT_CHECK(result.elapsedSeconds < 0.5);
    }
    // Sizes can be skipped altogether.
    {
        ScanOptions options;
        options.computeSizes = false;
        const ScanResult result = scanner.scan(f.rocket, index, CancellationToken{}, options);
        VT_CHECK(!result.cancelled);
        VT_REQUIRE(result.remnants.size() >= 3);
        VT_CHECK(!result.remnants[1].usageKnown);
    }
}

VT_TEST(scanner_offers_an_exclusive_vendor_folder_as_one_unit) {
    Fixture f;
    // Only Rocket is installed: no sibling of the vendor, nothing of the same team.
    f.userFile("Application Support/Acme/Rocket/state.db");
    f.userFile("Caches/Acme/Rocket/cache.db");
    f.userFile("Caches/Acme/Mystery/blob.bin");  // something that cannot be attributed
    const InstalledAppIndex index(std::vector<AppInfo>{f.rocket, f.helperApp});
    const RemnantScanner scanner(f.roots, f.pool);
    const ScanResult result = scanner.scan(f.rocket, index, CancellationToken{});

    // Everything inside is ours -> the vendor folder itself, selected, and no nested duplicate.
    const Remnant* whole = findRemnant(result, f.userLibrary + "/Application Support/Acme");
    VT_REQUIRE(whole != nullptr);
    VT_CHECK(whole->defaultSelected);
    VT_CHECK(!whole->isShared);
    VT_CHECK(whole->reason == MatchReason::VendorDirectory);
    VT_CHECK(findRemnant(result, f.userLibrary + "/Application Support/Acme/Rocket") == nullptr);

    // Unattributable content -> our child is selected, the folder is listed but left unchecked.
    const Remnant* partial = findRemnant(result, f.userLibrary + "/Caches/Acme");
    VT_REQUIRE(partial != nullptr);
    VT_CHECK(!partial->defaultSelected);
    VT_CHECK(!partial->warning.empty());
    const Remnant* child = findRemnant(result, f.userLibrary + "/Caches/Acme/Rocket");
    VT_REQUIRE(child != nullptr);
    VT_CHECK(child->defaultSelected);
}
