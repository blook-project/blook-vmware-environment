// blook-vmware-environment: screen classification and stop code unit tests.
//
// These drive the classifier with synthetic statistics so the whole state
// contract is testable without a VM (the lab host has no image viewer, so the
// classifier is the only way evidence is judged).
#include <gtest/gtest.h>

#include "blook/bvmenv/classify.h"

using namespace blook::bvmenv;

namespace {

ScreenStats stats(double black, double white, double blue, double colored, double gray,
                  double dark = 0.0, double mean_b = 0.0) {
    ScreenStats s;
    s.width = 1024;
    s.height = 768;
    s.sampled_pixels = 10000;
    s.black_ratio = black;
    s.white_ratio = white;
    s.blue_ratio = blue;
    s.colored_ratio = colored;
    s.gray_ratio = gray;
    s.dark_ratio = dark != 0.0 ? dark : black;
    s.mean_b = mean_b;
    s.fingerprint = 1234;
    return s;
}

}  // namespace

TEST(Classify, BugcheckBlueScreen) {
    // Windows 11 bugcheck: mostly blue with light text.
    const ScreenStats s = stats(0.05, 0.03, 0.85, 0.05, 0.02, 0.1, 200.0);
    const ScreenReport r = classify_screen(s, "");
    EXPECT_EQ(r.classification, ScreenClass::Bsod);
    EXPECT_GT(r.confidence, 0.5);
}

TEST(Classify, BlueFrameWithLiveGuestIsLogonUiNotBugcheck) {
    // Regression: the Windows logon screen is dark blue with light text. Without
    // host context this used to be reported as a false BSOD (which would make
    // every downstream e2e reset its VM and report bogus failures).
    ScreenContext context;
    context.tools_running = true;
    context.guest_queryable = true;
    const ScreenStats s = stats(0.05, 0.03, 0.85, 0.05, 0.02, 0.1, 200.0);
    const ScreenReport r = classify_screen(s, "", context);
    EXPECT_EQ(r.classification, ScreenClass::Login);
    EXPECT_TRUE(r.stop_code.empty());
}

TEST(Classify, BlueFrameWithDeadGuestAndFrozenScreenIsBugcheck) {
    ScreenContext context;
    context.tools_running = false;
    context.guest_queryable = false;
    context.unchanged_samples = 2;
    const ScreenStats s = stats(0.05, 0.03, 0.85, 0.05, 0.02, 0.1, 200.0);
    const ScreenReport r = classify_screen(s, "", context);
    EXPECT_EQ(r.classification, ScreenClass::Bsod);
    EXPECT_GT(r.confidence, 0.8);
}

TEST(Classify, LiveGuestWithUnclearFrameIsDesktop) {
    ScreenContext context;
    context.guest_queryable = true;
    const ScreenReport r = classify_screen(stats(0.5, 0.05, 0.0, 0.05, 0.05), "", context);
    EXPECT_EQ(r.classification, ScreenClass::Desktop);
}

TEST(Classify, BugcheckFromOcrWinsOverColour) {
    const ScreenStats s = stats(0.2, 0.2, 0.2, 0.2, 0.2);
    const ScreenReport r = classify_screen(s, "Your device ran into a problem\nSTOP CODE: 0xD1");
    EXPECT_EQ(r.classification, ScreenClass::Bsod);
    EXPECT_GT(r.confidence, 0.9);
}

TEST(Classify, BlackScreen) {
    const ScreenReport r = classify_screen(stats(0.98, 0.0, 0.0, 0.0, 0.0), "");
    EXPECT_EQ(r.classification, ScreenClass::Black);
}

TEST(Classify, BootScreenOnBlack) {
    // Black background with the Windows boot logo/spinner (a little light).
    const ScreenReport r = classify_screen(stats(0.9, 0.01, 0.01, 0.02, 0.01), "");
    EXPECT_EQ(r.classification, ScreenClass::Boot);
}

TEST(Classify, LoginScreenFromOcr) {
    const ScreenReport r = classify_screen(stats(0.2, 0.1, 0.1, 0.5, 0.1), "欢迎\n其他用户\n登录");
    EXPECT_EQ(r.classification, ScreenClass::Login);
}

TEST(Classify, DesktopFromShellMarkers) {
    const ScreenReport r = classify_screen(stats(0.1, 0.3, 0.05, 0.45, 0.1), "回收站 开始");
    EXPECT_EQ(r.classification, ScreenClass::Desktop);
    EXPECT_GT(r.confidence, 0.7);
}

TEST(Classify, UnknownWhenNothingMatches) {
    const ScreenReport r = classify_screen(stats(0.5, 0.05, 0.0, 0.05, 0.05), "");
    EXPECT_EQ(r.classification, ScreenClass::Unknown);
}

TEST(Classify, EmptyStatsAreUnknown) {
    ScreenStats s;
    const ScreenReport r = classify_screen(s, "");
    EXPECT_EQ(r.classification, ScreenClass::Unknown);
    EXPECT_EQ(r.confidence, 0.0);
}

TEST(StopCode, ExtractsStopCodeLine) {
    EXPECT_EQ(extract_stop_code("STOP CODE: DRIVER_IRQL_NOT_LESS_OR_EQUAL"),
              "DRIVER_IRQL_NOT_LESS_OR_EQUAL");
    EXPECT_EQ(extract_stop_code("stop code: 0x000000d1"), "0x000000d1");
    EXPECT_EQ(extract_stop_code("错误代码: KMODE_EXCEPTION_NOT_HANDLED"),
              "KMODE_EXCEPTION_NOT_HANDLED");
}

TEST(StopCode, ExtractsMixedCaseOcrStopCode) {
    // Real OCR output from a live bugcheck frame (captured by scripts/demo-e2e.ps1):
    //   "Stop code: DRIVER IRQL NOT LESS OR EQUAL (0xD1)"
    EXPECT_EQ(extract_stop_code("Your device ran into a problem and needs to restart.\n"
                                "Stop code: DRIVER IRQL NOT LESS OR EQUAL (0xD1)"),
              "DRIVER");
}

TEST(StopCode, ExtractsHexWithoutLabel) {
    EXPECT_EQ(extract_stop_code("bugcheck 0x0000009F (0x3, ...)"), "0x0000009F");
}

TEST(StopCode, ExtractsKnownSymbol) {
    EXPECT_EQ(extract_stop_code("The computer has rebooted from a bugcheck.\n"
                                "Bugcheck was: PAGE_FAULT_IN_NONPAGED_AREA"),
              "PAGE_FAULT_IN_NONPAGED_AREA");
}

TEST(StopCode, EmptyWhenAbsent) {
    EXPECT_TRUE(extract_stop_code("Windows is starting up").empty());
    EXPECT_TRUE(extract_stop_code("").empty());
}
