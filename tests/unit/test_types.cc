// blook-vmware-environment: value types / enum / timestamp unit tests.
#include <gtest/gtest.h>

#include <regex>

#include "blook/bvmenv/types.h"

using namespace blook::bvmenv;

TEST(Types, PowerStateRoundTrip) {
    for (const PowerState state :
         {PowerState::Off, PowerState::On, PowerState::Suspended, PowerState::Unknown}) {
        const auto parsed = parse_power_state(to_string(state));
        ASSERT_TRUE(parsed.has_value());
        EXPECT_EQ(*parsed, state);
    }
    EXPECT_FALSE(parse_power_state("sleeping").has_value());
}

TEST(Types, ToolsStateRoundTrip) {
    for (const ToolsState state : {ToolsState::NotInstalled, ToolsState::Installed,
                                   ToolsState::Running, ToolsState::Unknown}) {
        const auto parsed = parse_tools_state(to_string(state));
        ASSERT_TRUE(parsed.has_value());
        EXPECT_EQ(*parsed, state);
    }
}

TEST(Types, ScreenClassRoundTrip) {
    for (const ScreenClass cls :
         {ScreenClass::Black, ScreenClass::Boot, ScreenClass::Login, ScreenClass::Desktop,
          ScreenClass::Bsod, ScreenClass::Hang, ScreenClass::Unknown}) {
        const auto parsed = parse_screen_class(to_string(cls));
        ASSERT_TRUE(parsed.has_value());
        EXPECT_EQ(*parsed, cls);
    }
}

TEST(Types, E2eOutcomeRoundTrip) {
    for (const E2eOutcome outcome : {E2eOutcome::Pass, E2eOutcome::Fail, E2eOutcome::Timeout,
                                     E2eOutcome::Bsod, E2eOutcome::Hang, E2eOutcome::Error}) {
        const auto parsed = parse_e2e_outcome(to_string(outcome));
        ASSERT_TRUE(parsed.has_value());
        EXPECT_EQ(*parsed, outcome);
    }
}

TEST(Types, TimestampsAreUtc8601) {
    const std::string stamp = now_iso8601();
    const std::regex pattern(R"(^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}Z$)");
    EXPECT_TRUE(std::regex_match(stamp, pattern)) << stamp;

    const std::string file_stamp = now_stamp();
    const std::regex file_pattern(R"(^\d{8}T\d{6}Z$)");
    EXPECT_TRUE(std::regex_match(file_stamp, file_pattern)) << file_stamp;
}

TEST(Types, StateReportJsonShape) {
    StateReport report;
    report.vm = "vm-drv";
    report.power = PowerState::On;
    report.tools.state = ToolsState::Running;
    report.tools.version = "13.0.0";
    report.screen.classification = ScreenClass::Bsod;
    report.screen.confidence = 0.97;
    report.screen.stop_code = "0x000000D1";
    report.hang_suspected = false;
    report.sampled_at = now_iso8601();

    const json::Value out = report.to_json();
    EXPECT_EQ(out["schema"].as_string(), "bvmenv.state.v1");
    EXPECT_EQ(out["power"].as_string(), "on");
    EXPECT_EQ(out["tools"]["state"].as_string(), "running");
    EXPECT_EQ(out["screen"]["classification"].as_string(), "bsod");
    EXPECT_EQ(out["screen"]["stop_code"].as_string(), "0x000000D1");
    EXPECT_FALSE(out["hang"]["suspected"].as_bool(true));

    // The report must round trip through JSON (downstream parses it).
    const auto parsed = json::Value::parse(out.dump(2));
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ((*parsed)["screen"]["classification"].as_string(), "bsod");
}

TEST(Types, E2eResultJsonShape) {
    E2eResult result;
    result.run_id = "20261021T031500Z";
    result.outcome = E2eOutcome::Bsod;
    result.exit_code = -1;
    result.timed_out = false;
    result.reset.performed = true;
    result.reset.ok = true;
    result.reset.strategy = "snapshot";

    const json::Value out = result.to_json();
    EXPECT_EQ(out["schema"].as_string(), "bvmenv.e2e.v1");
    EXPECT_EQ(out["outcome"].as_string(), "bsod");
    EXPECT_TRUE(out["reset"]["performed"].as_bool());
    EXPECT_EQ(out["reset"]["strategy"].as_string(), "snapshot");
}

TEST(ExceedsTimeoutBudget, WithinBudgetIsNotTimeout) {
    ExecResult result;
    result.duration = std::chrono::milliseconds(1200);
    result.exit_code = 0;
    EXPECT_FALSE(exceeds_timeout_budget(result, std::chrono::seconds(30)));
}

TEST(ExceedsTimeoutBudget, AtOrOverBudgetIsTimeout) {
    ExecResult result;
    result.duration = std::chrono::milliseconds(9310);
    result.exit_code = 0;  // late artifact: must not be trusted
    EXPECT_TRUE(exceeds_timeout_budget(result, std::chrono::seconds(5)));
}

TEST(ExceedsTimeoutBudget, LateArtifactBecomesTimedOutViaRunWrapper) {
    // Regression for the SYSTEM-fallback success path, which used to return a
    // killed command as a success because its exit-code file arrived late.
    ExecResult result;
    result.duration = std::chrono::milliseconds(9100);
    result.exit_code = 0;
    result.ran = true;
    ASSERT_TRUE(exceeds_timeout_budget(result, std::chrono::seconds(5)));
}

TEST(ExceedsTimeoutBudget, ZeroTimeoutDisablesTheRule) {
    ExecResult result;
    result.duration = std::chrono::hours(1);
    EXPECT_FALSE(exceeds_timeout_budget(result, std::chrono::seconds(0)));
}

TEST(ExceedsTimeoutBudget, AlreadyTimedOutStaysTimedOut) {
    ExecResult result;
    result.timed_out = true;
    result.duration = std::chrono::milliseconds(10);
    EXPECT_TRUE(exceeds_timeout_budget(result, std::chrono::seconds(5)));
}
