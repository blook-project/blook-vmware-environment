// blook-vmware-environment: argument parsing unit tests.
#include <gtest/gtest.h>

#include "blook/bvmenv/args.h"

using namespace blook::bvmenv;

namespace {
const std::vector<std::string> kBooleans = {"json", "elevated", "force", "hard"};
}

TEST(Args, PositionalsAndValues) {
    const ParsedArgs a = parse_args({"vm-drv", "--cpus", "8", "--memory", "8192"}, kBooleans);
    ASSERT_EQ(a.positional.size(), 1u);
    EXPECT_EQ(a.positional[0], "vm-drv");
    EXPECT_EQ(a.get("cpus"), "8");
    EXPECT_EQ(a.get("memory"), "8192");
}

TEST(Args, EqualsFormAndBooleanFlags) {
    const ParsedArgs a = parse_args({"vm-drv", "--timeout=600", "--json"}, kBooleans);
    EXPECT_EQ(a.get("timeout"), "600");
    EXPECT_TRUE(a.has("json"));
    EXPECT_EQ(a.get("json"), "1");
}

TEST(Args, TrailingStopsFlagParsing) {
    const ParsedArgs a =
        parse_args({"vm-drv", "--timeout", "30", "--", "cmd", "/c", "echo", "--json"}, kBooleans);
    ASSERT_EQ(a.trailing.size(), 4u);
    EXPECT_EQ(a.trailing[0], "cmd");
    EXPECT_EQ(a.trailing[3], "--json");
    EXPECT_EQ(a.get("timeout"), "30");
    EXPECT_FALSE(a.has("json"));
}

TEST(Args, RepeatedMultiValueFlags) {
    const ParsedArgs a = parse_args({"vm", "--args", "a", "--args", "b=c", "--args=d"}, kBooleans);
    EXPECT_EQ(a.multi.count("args"), 3u);
    std::vector<std::string> values;
    auto range = a.multi.equal_range("args");
    for (auto it = range.first; it != range.second; ++it) values.push_back(it->second);
    EXPECT_EQ(values, (std::vector<std::string>{"a", "b=c", "d"}));
}

TEST(Args, BooleanFlagDoesNotSwallowNextValue) {
    const ParsedArgs a = parse_args({"--json", "vm-drv"}, kBooleans);
    EXPECT_TRUE(a.has("json"));
    ASSERT_EQ(a.positional.size(), 1u);
    EXPECT_EQ(a.positional[0], "vm-drv");
}

TEST(Args, ValueFlagWithoutValueBecomesTrue) {
    const ParsedArgs a = parse_args({"vm-drv", "--timeout"}, kBooleans);
    EXPECT_EQ(a.get("timeout"), "1");
}

TEST(Args, EmptyInput) {
    const ParsedArgs a = parse_args({}, kBooleans);
    EXPECT_TRUE(a.positional.empty());
    EXPECT_TRUE(a.flags.empty());
    EXPECT_TRUE(a.trailing.empty());
}
