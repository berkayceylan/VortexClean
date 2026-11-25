// Tests for the building blocks: the JSON document model, string and path
// helpers, and the thread pool with its fork/join groups.
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "Cancellation.hpp"
#include "FileUtil.hpp"
#include "Json.hpp"
#include "LaunchControl.hpp"
#include "PathUtil.hpp"
#include "StringUtil.hpp"
#include "TestSupport.hpp"
#include "ThreadPool.hpp"

using namespace vortex;

// ---------------------------------------------------------------------------
// JSON
// ---------------------------------------------------------------------------

VT_TEST(json_parses_valid_documents) {
    const auto parsed = JsonValue::parse(R"(
        { "name": "Rocket", "count": 3, "ratio": -1.5e2, "big": 9007199254740993,
          "ok": true, "none": null, "tags": ["a", "b", []], "nested": {"k": {"deep": false}} }
    )");
    VT_REQUIRE(parsed.ok());
    const JsonValue& doc = parsed.value();
    VT_CHECK(doc.isObject());
    VT_CHECK_EQ(doc["name"].asString(), std::string("Rocket"));
    VT_CHECK(doc["count"].isInteger());
    VT_CHECK_EQ(doc["count"].asInteger(), std::int64_t{3});
    VT_CHECK(doc["ratio"].isDouble());
    VT_CHECK_EQ(doc["ratio"].asDouble(), -150.0);
    VT_CHECK_EQ(doc["big"].asInteger(), std::int64_t{9007199254740993});  // exact, beyond double precision
    VT_CHECK(doc["ok"].asBool(false));
    VT_CHECK(doc["none"].isNull());
    VT_CHECK_EQ(doc["tags"].size(), std::size_t{3});
    VT_CHECK(doc["tags"].asArray()[2].isArray());
    VT_CHECK(!doc["nested"]["k"]["deep"].asBool(true));
    VT_CHECK(doc["missing"].isNull());            // absent keys read as null ...
    VT_CHECK(doc.find("missing") == nullptr);     // ... but can be told apart
    VT_CHECK_EQ(doc["missing"]["deeper"].asString(), std::string());

    for (const char* scalar : {"0", "-0", "42", "1e3", "\"text\"", "true", "null", "[]", "{}", "  [ 1 , 2 ]  "}) {
        if (!JsonValue::parse(scalar).ok()) {
            vt::reportFailure(__FILE__, __LINE__, std::string("valid JSON rejected: ") + scalar);
        }
    }
    // Integers beyond int64 degrade to double instead of failing or wrapping.
    const auto huge = JsonValue::parse("123456789012345678901234567890");
    VT_REQUIRE(huge.ok());
    VT_CHECK(huge.value().isDouble());
}

VT_TEST(json_rejects_invalid_documents) {
    for (const char* bad : {
             "", "   ", "{", "}", "[", "]", "[1,]", "{\"a\":1,}", "{'a':1}", "{a:1}", "{\"a\" 1}", "{\"a\":}",
             "[1 2]", "01", "-", "+1", "1.", ".5", "1e", "1e+", "0x10", "NaN", "Infinity", "-Infinity", "tru",
             "nul", "True", "\"unterminated", "\"bad \\q escape\"", "\"\\u12\"", "\"\\uZZZZ\"",
             "\"\\ud800\"", "\"\\udc00\"", "\"\\ud800\\u0041\"", "\"tab\there\"", "\"line\nbreak\"",
             "[] []", "{} x", "1 2", "// comment\n1", "[1] // trailing", "\"a\" \"b\"", "{\"a\":1}}", "1e99999",
         }) {
        if (JsonValue::parse(bad).ok()) {
            vt::reportFailure(__FILE__, __LINE__, std::string("invalid JSON accepted: ") + bad);
        }
    }
    // Nesting is bounded, so hostile input cannot exhaust the stack.
    VT_CHECK(JsonValue::parse(std::string(100, '[') + std::string(100, ']')).ok());
    VT_CHECK(!JsonValue::parse(std::string(5000, '[') + std::string(5000, ']')).ok());
    VT_CHECK(!JsonValue::parse(std::string(200000, '[')).ok());
}

VT_TEST(json_strings_escape_and_unescape) {
    const auto parsed = JsonValue::parse(R"("quote \" slash \\ solidus \/ \b\f\n\r\t \u00e9 \u20ac \ud83d\ude80")");
    VT_REQUIRE(parsed.ok());
    VT_CHECK_EQ(parsed.value().asString(),
                std::string("quote \" slash \\ solidus / \b\f\n\r\t \xC3\xA9 \xE2\x82\xAC \xF0\x9F\x9A\x80"));

    // Round trip: whatever is emitted parses back to the same value.
    const std::string tricky = std::string("line1\nline2\t\"quoted\" back\\slash \x01\x1f\x7f caf\xC3\xA9 \xF0\x9F\x9A\x80") +
                               std::string(1, '\0') + "after-nul";
    const std::string dumped = JsonValue(tricky).dump(-1);
    for (const char ch : dumped) VT_CHECK(static_cast<unsigned char>(ch) >= 0x20);  // no raw control characters
    const auto back = JsonValue::parse(dumped);
    VT_REQUIRE(back.ok());
    VT_CHECK_EQ(back.value().asString(), tricky);

    // Invalid UTF-8 (possible in names taken from a damaged BOM) can never produce invalid JSON.
    const std::string broken = std::string("ok \xFF\xFE bad \xC3 trunc \xE2\x82 overlong \xC0\xAF end");
    const std::string safe = JsonValue(broken).dump(-1);
    const auto reparsed = JsonValue::parse(safe);
    VT_REQUIRE(reparsed.ok());
    VT_CHECK_EQ(reparsed.value().asString(), str::sanitizeUtf8(broken));
    VT_CHECK(reparsed.value().asString().find("\xEF\xBF\xBD") != std::string::npos);
    VT_CHECK_EQ(str::sanitizeUtf8("plain caf\xC3\xA9"), std::string("plain caf\xC3\xA9"));
    VT_CHECK_EQ(str::sanitizeUtf8("\xED\xA0\x80"), std::string("\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD"));  // surrogate
}

VT_TEST(json_documents_are_built_and_serialised_in_order) {
    JsonValue doc = JsonValue::makeObject();
    doc.set("zeta", 1).set("alpha", "two").set("flag", true).set("nothing", nullptr);
    doc.set("size", std::uint64_t{18446744073709551615ULL});  // above int64: degrades to double
    doc.set("bytes", std::size_t{4096});
    doc.set("negative", -7);
    JsonValue list = JsonValue::makeArray();
    list.push(1).push("x").push(JsonValue::makeObject());
    doc.set("list", std::move(list));
    doc.set("zeta", 99);  // replaces in place, keeps its position

    const std::string compact = doc.dump(-1);
    VT_CHECK(compact.starts_with(R"({"zeta":99,"alpha":"two","flag":true,"nothing":null,"size":)"));
    VT_CHECK(compact.ends_with(R"("bytes":4096,"negative":-7,"list":[1,"x",{}]})"));
    VT_CHECK(doc["size"].isDouble());
    VT_CHECK(doc["bytes"].isInteger());

    const std::string pretty = doc.dump(2);
    VT_CHECK(pretty.find("\n  \"alpha\": \"two\"") != std::string::npos);
    VT_CHECK(pretty.ends_with("}\n"));
    const auto back = JsonValue::parse(pretty);
    VT_REQUIRE(back.ok());
    VT_CHECK_EQ(back.value().dump(-1), compact);

    VT_CHECK_EQ(JsonValue(std::nan("")).dump(-1), std::string("null"));  // JSON cannot express NaN
    VT_CHECK_EQ(JsonValue(0.5).dump(-1), std::string("0.5"));
    VT_CHECK_EQ(JsonValue::makeArray().dump(-1), std::string("[]"));
    VT_CHECK_EQ(JsonValue().dump(-1), std::string("null"));
}

// ---------------------------------------------------------------------------
// Strings and paths
// ---------------------------------------------------------------------------

VT_TEST(string_helpers) {
    VT_CHECK_EQ(str::toLower("Com.ACME.R\xC3\x96" "cket"), std::string("com.acme.r\xC3\x96" "cket"));
    VT_CHECK(str::iequals("ABC", "abc"));
    VT_CHECK(!str::iequals("abc", "abcd"));
    VT_CHECK(str::istartsWith("Com.Apple.Safari", "com.apple."));
    VT_CHECK(str::iendsWith("Rocket.APP", ".app"));
    VT_CHECK_EQ(str::normalizeName("Visual Studio Code"), std::string("visualstudiocode"));
    VT_CHECK_EQ(str::normalizeName("zoom.us"), std::string("zoomus"));
    VT_CHECK_EQ(str::normalizeName("  -_. "), std::string());
    VT_CHECK_EQ(str::nameTokens("Rocket Helper (GPU)-2"), (std::vector<std::string>{"rocket", "helper", "gpu", "2"}));
    VT_CHECK_EQ(str::split("a.b..c", '.'), (std::vector<std::string>{"a", "b", "c"}));
    VT_CHECK_EQ(str::split("a.b..c", '.', true), (std::vector<std::string>{"a", "b", "", "c"}));
    VT_CHECK_EQ(std::string(str::trim("  x y \n")), std::string("x y"));
    VT_CHECK(str::isUuid("4F1A6B2C-0D3E-4F5A-8B7C-9D0E1F2A3B4C"));
    VT_CHECK(!str::isUuid("4F1A6B2C-0D3E-4F5A-8B7C-9D0E1F2A3B4"));
    VT_CHECK(!str::isUuid("4F1A6B2C00D3E-4F5A-8B7C-9D0E1F2A3B4C"));
    VT_CHECK(!str::isUuid("ZZ1A6B2C-0D3E-4F5A-8B7C-9D0E1F2A3B4C"));
    VT_CHECK(str::isHex("0a1B2c") && !str::isHex("0x1") && !str::isHex(""));
    VT_CHECK_EQ(str::formatBytes(0), std::string("0 bytes"));
    VT_CHECK_EQ(str::formatBytes(1), std::string("1 byte"));
    VT_CHECK_EQ(str::formatBytes(999), std::string("999 bytes"));
    VT_CHECK_EQ(str::formatBytes(1500), std::string("1.50 KB"));
    VT_CHECK_EQ(str::formatBytes(123456789), std::string("123 MB"));
    VT_CHECK_EQ(str::formatBytes(std::uint64_t{2500} * 1000 * 1000), std::string("2.50 GB"));
    VT_CHECK(str::fnv1a64("a") != str::fnv1a64("b"));
    VT_CHECK_EQ(str::fnv1a64(""), std::uint64_t{0xcbf29ce484222325ULL});
}

VT_TEST(path_helpers) {
    VT_CHECK_EQ(path::join("/a", "b"), std::string("/a/b"));
    VT_CHECK_EQ(path::join("/a/", "/b"), std::string("/a/b"));
    VT_CHECK_EQ(path::join("", "b"), std::string("b"));
    VT_CHECK_EQ(path::join("/", "b"), std::string("/b"));
    VT_CHECK_EQ(std::string(path::fileName("/a/b.app")), std::string("b.app"));
    VT_CHECK_EQ(std::string(path::fileName("/a/b.app/")), std::string("b.app"));
    VT_CHECK_EQ(path::parent("/a/b/c"), std::string("/a/b"));
    VT_CHECK_EQ(path::parent("/a"), std::string("/"));
    VT_CHECK_EQ(path::parent("name"), std::string());
    VT_CHECK_EQ(std::string(path::extension("x.tar.gz")), std::string("gz"));
    VT_CHECK_EQ(std::string(path::extension(".hidden")), std::string());
    VT_CHECK_EQ(std::string(path::extension("trailing.")), std::string());
    VT_CHECK_EQ(std::string(path::stem("com.acme.Rocket.plist")), std::string("com.acme.Rocket"));
    VT_CHECK_EQ(path::normalize("/a//b/./c/../d/"), std::string("/a/b/d"));
    VT_CHECK_EQ(path::normalize("/../../etc"), std::string("/etc"));  // never climbs above the root
    VT_CHECK_EQ(path::normalize("a/../../b"), std::string("../b"));
    VT_CHECK_EQ(path::normalize(""), std::string("."));
    VT_CHECK_EQ(path::normalize("/"), std::string("/"));
    VT_CHECK_EQ(path::depth("/a/b/c"), std::size_t{3});
    VT_CHECK(path::hasParentReference("a/../b") && !path::hasParentReference("a/..b/c"));

    VT_CHECK(path::isWithinOrEqual("/a/b", "/a/b"));
    VT_CHECK(path::isWithinOrEqual("/a/b/c", "/a/b"));
    VT_CHECK(!path::isStrictlyWithin("/a/b", "/a/b"));
    VT_CHECK(!path::isWithinOrEqual("/a/bc", "/a/b"));  // component aware, not a string prefix
    VT_CHECK(!path::isWithinOrEqual("/a", "/a/b"));
    VT_CHECK(path::isStrictlyWithin("/anything", "/"));
    VT_CHECK(!path::isWithinOrEqual("/A/B/c", "/a/b"));
    VT_CHECK(path::iIsWithinOrEqual("/A/B/c", "/a/b"));
    VT_CHECK(!path::isWithinOrEqual("", "/a") && !path::isWithinOrEqual("/a", ""));

    VT_CHECK_EQ(path::abbreviateHome("/Users/me/Library/x", "/Users/me"), std::string("~/Library/x"));
    VT_CHECK_EQ(path::abbreviateHome("/Users/me", "/Users/me"), std::string("~"));
    VT_CHECK_EQ(path::abbreviateHome("/Users/meow/x", "/Users/me"), std::string("/Users/meow/x"));
    VT_CHECK_EQ(path::resolveSystemAliases("/etc/hosts"), std::string("/private/etc/hosts"));
    VT_CHECK_EQ(path::resolveSystemAliases("/var/db/receipts"), std::string("/private/var/db/receipts"));
    VT_CHECK_EQ(path::resolveSystemAliases("/tmp"), std::string("/private/tmp"));
    VT_CHECK_EQ(path::resolveSystemAliases("/System/Volumes/Data/Applications/X.app"), std::string("/Applications/X.app"));
    VT_CHECK_EQ(path::resolveSystemAliases("/variable/x"), std::string("/variable/x"));
    VT_CHECK(!path::homeDirectory().empty());
    VT_CHECK(path::homeDirectory().front() == '/');
}

VT_TEST(file_helpers_refuse_unsafe_inputs) {
    const vt::TempDir temp("vortex-io");
    vt::writeFile(temp.sub("a/file.txt"), "hello");
    const auto text = io::readTextFile(temp.sub("a/file.txt"), 1024);
    VT_REQUIRE(text.ok());
    VT_CHECK_EQ(text.value(), std::string("hello"));
    VT_CHECK(!io::readTextFile(temp.sub("a/file.txt"), 3).ok());   // over the size limit
    VT_CHECK(!io::readTextFile(temp.sub("a"), 1024).ok());         // a directory
    VT_CHECK(!io::readTextFile(temp.sub("missing"), 1024).ok());

    const auto listing = io::listDirectory(temp.sub("a"));
    VT_REQUIRE(listing.ok());
    VT_REQUIRE(listing.value().size() == 1);
    VT_CHECK_EQ(listing.value().front().name, std::string("file.txt"));
    const auto missing = io::listDirectory(temp.sub("missing"));
    VT_CHECK(!missing.ok());
    VT_CHECK_EQ(missing.error().code, ENOENT);

    VT_CHECK(io::makeDirectories(temp.sub("x/y/z"), 0755).ok());
    VT_CHECK(io::makeDirectories(temp.sub("x/y/z"), 0755).ok());  // idempotent
    VT_CHECK(!io::makeDirectories(temp.sub("a/file.txt/sub"), 0755).ok());
    VT_CHECK(io::writeFileExclusive(temp.sub("x/new.txt"), "data", 0644).ok());
    const Status again = io::writeFileExclusive(temp.sub("x/new.txt"), "other", 0644);
    VT_CHECK(!again.ok());
    VT_CHECK_EQ(again.error().code, EEXIST);
    VT_CHECK_EQ(vt::readFile(temp.sub("x/new.txt")), std::string("data"));  // never overwritten
}

VT_TEST(launchd_labels_are_validated_before_use) {
    for (const char* good : {"com.acme.rocketd", "com.acme.Rocket-Helper_2", "a"}) {
        VT_CHECK(LaunchControl::isValidLabel(good));
    }
    for (const char* bad : {"", "-rf", ".hidden", "com.acme; rm -rf /", "com.acme rocket", "com/acme",
                            "com.acme\nx", "$(id)", "com.acme`id`", "label\"quoted"}) {
        if (LaunchControl::isValidLabel(bad)) {
            vt::reportFailure(__FILE__, __LINE__, std::string("unsafe label accepted: ") + bad);
        }
    }
    VT_CHECK(!LaunchControl::isValidLabel(std::string(300, 'a')));
    VT_CHECK_EQ(LaunchControl::userServiceTarget(501, "com.acme.agent"), std::string("gui/501/com.acme.agent"));
    VT_CHECK_EQ(LaunchControl::systemServiceTarget("com.acme.rocketd"), std::string("system/com.acme.rocketd"));

    LaunchctlResult result;
    VT_CHECK(!result.serviceIsGone());
    result.spawned = true;
    for (const int status : {0, 3, 113, 36}) {
        result.exitStatus = status;
        VT_CHECK(result.serviceIsGone());
    }
    result.exitStatus = 1;
    VT_CHECK(!result.serviceIsGone());
    result.exitStatus = 0;
    result.timedOut = true;
    VT_CHECK(!result.serviceIsGone());
}

// ---------------------------------------------------------------------------
// Thread pool
// ---------------------------------------------------------------------------

VT_TEST(thread_pool_runs_every_task_in_parallel) {
    ThreadPool pool(4);
    VT_CHECK_EQ(pool.threadCount(), std::size_t{4});
    std::atomic<int> sum{0};
    std::mutex mutex;
    std::set<std::thread::id> threads;
    {
        TaskGroup group(pool);
        for (int i = 1; i <= 2000; ++i) {
            group.run([&, i] {
                sum += i;
                const std::lock_guard<std::mutex> lock(mutex);
                threads.insert(std::this_thread::get_id());
            });
        }
        group.wait();
    }
    VT_CHECK_EQ(sum.load(), 2000 * 2001 / 2);
    VT_CHECK(threads.size() >= 2);  // the work really was spread out

    ThreadPool defaultPool;
    VT_CHECK(defaultPool.threadCount() >= 2 && defaultPool.threadCount() <= 8);
}

VT_TEST(thread_pool_supports_nested_groups_without_deadlock) {
    // Two workers, and every task forks and joins a group of its own: with a
    // naive blocking wait this would deadlock as soon as both workers wait.
    ThreadPool pool(2);
    std::atomic<int> leaves{0};
    TaskGroup outer(pool);
    for (int i = 0; i < 8; ++i) {
        outer.run([&] {
            TaskGroup middle(pool);
            for (int j = 0; j < 8; ++j) {
                middle.run([&] {
                    TaskGroup inner(pool);
                    for (int k = 0; k < 4; ++k) inner.run([&] { ++leaves; });
                    inner.wait();
                });
            }
            middle.wait();
        });
    }
    outer.wait();
    VT_CHECK_EQ(leaves.load(), 8 * 8 * 4);
}

VT_TEST(thread_pool_propagates_exceptions_and_drains_on_shutdown) {
    std::atomic<int> ran{0};
    {
        ThreadPool pool(3);
        TaskGroup group(pool);
        for (int i = 0; i < 20; ++i) {
            group.run([&, i] {
                ++ran;
                if (i == 7) throw std::runtime_error("task 7 failed");
            });
        }
        bool caught = false;
        try {
            group.wait();
        } catch (const std::runtime_error& error) {
            caught = std::string(error.what()) == "task 7 failed";
        }
        VT_CHECK(caught);
        VT_CHECK_EQ(ran.load(), 20);  // one failure does not stop the others
        group.wait();                 // the error is reported once
    }

    // Destroying the pool finishes everything that was queued: nothing is dropped.
    std::atomic<int> finished{0};
    {
        ThreadPool pool(2);
        for (int i = 0; i < 200; ++i) {
            pool.submit([&] {
                std::this_thread::sleep_for(std::chrono::microseconds(200));
                ++finished;
            });
        }
    }
    VT_CHECK_EQ(finished.load(), 200);
}

VT_TEST(cancellation_tokens_share_state_across_copies) {
    const CancellationToken token;
    const CancellationToken copy = token;
    const CancellationToken unrelated;
    VT_CHECK(!token.isCancelled() && !copy.isCancelled());
    copy.cancel();
    VT_CHECK(token.isCancelled());
    VT_CHECK(!unrelated.isCancelled());

    // Cooperative cancellation stops a busy task group within milliseconds.
    ThreadPool pool(4);
    const CancellationToken stop;
    std::atomic<std::uint64_t> iterations{0};
    TaskGroup group(pool);
    for (int i = 0; i < 4; ++i) {
        group.run([&] {
            while (!stop.isCancelled()) ++iterations;
        });
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    const auto cancelledAt = std::chrono::steady_clock::now();
    stop.cancel();
    group.wait();
    const double ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - cancelledAt).count();
    VT_CHECK(iterations.load() > 0);
    VT_CHECK(ms < 100.0);
    vt::note("4 busy workers stopped " + std::to_string(ms) + " ms after cancel()");
}
