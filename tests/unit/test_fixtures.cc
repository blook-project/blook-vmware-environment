// blook-vmware-environment: classification tests against real captured frames.
//
// These are the ground truth the lab has to judge without being able to look at
// an image: the PNGs under tests/fixtures were captured from real guests with
// `vmcli MKS captureScreenshot` at known states. They are skipped when the
// fixtures are absent (e.g. a source tarball without binary fixtures).
#include <gtest/gtest.h>

#include <filesystem>

#include "blook/bvmenv/classify.h"
#include "blook/bvmenv/util.h"

using namespace blook::bvmenv;

namespace {

fs::path fixture(const char* name) {
    // Resolved through a compile time define so the test works regardless of
    // the working directory xmake runs it from.
    return fs::path(BVMENV_TEST_SOURCE_DIR) / "tests" / "fixtures" / name;
}

ScreenClass classify_fixture(const char* name, ScreenContext context, std::string* reason) {
    const fs::path path = fixture(name);
    if (!file_exists(path))
        return ScreenClass::Unknown;
    std::string error;
    const auto stats = analyze_image(path, &error);
    if (!stats) {
        if (reason)
            *reason = error;
        return ScreenClass::Unknown;
    }
    const ScreenReport report = classify_screen(*stats, "", context);
    if (reason)
        *reason = report.reason;
    return report.classification;
}

}  // namespace

TEST(Fixtures, LogonScreenIsLoginNotBugcheck) {
    // Real logon screen: dark blue with the account name, guest alive.
    ScreenContext context;
    context.tools_running = true;
    context.guest_queryable = true;
    std::string reason;
    const ScreenClass cls = classify_fixture("login-screen.png", context, &reason);
    if (cls == ScreenClass::Unknown && !file_exists(fixture("login-screen.png"))) {
        GTEST_SKIP() << "fixture missing";
    }
    EXPECT_EQ(cls, ScreenClass::Login) << reason;
}

TEST(Fixtures, BootFrameIsBootOrBlack) {
    ScreenContext context;  // guest not up yet
    std::string reason;
    const ScreenClass cls = classify_fixture("boot-black.png", context, &reason);
    if (!file_exists(fixture("boot-black.png")))
        GTEST_SKIP() << "fixture missing";
    EXPECT_TRUE(cls == ScreenClass::Boot || cls == ScreenClass::Black) << reason;
}

TEST(Fixtures, DesktopFramesAreDesktop) {
    ScreenContext context;
    context.tools_running = true;
    context.guest_queryable = true;
    for (const char* name : {"desktop-shell.png", "desktop-setup.png"}) {
        if (!file_exists(fixture(name)))
            continue;
        std::string reason;
        const ScreenClass cls = classify_fixture(name, context, &reason);
        EXPECT_EQ(cls, ScreenClass::Desktop) << name << ": " << reason;
    }
}
