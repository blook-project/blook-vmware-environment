// blook-vmware-environment: JSON unit tests.
#include <gtest/gtest.h>

#include "blook/bvmenv/json.h"

using namespace blook::bvmenv::json;

TEST(Json, DumpsScalars) {
    EXPECT_EQ(Value{}.dump(), "null");
    EXPECT_EQ(Value(true).dump(), "true");
    EXPECT_EQ(Value(42).dump(), "42");
    EXPECT_EQ(Value(0.5).dump(), "0.5");
    EXPECT_EQ(Value("hi").dump(), "\"hi\"");
}

TEST(Json, EscapesStrings) {
    const std::string dumped = Value("a\"b\\c\nd\te").dump();
    EXPECT_EQ(dumped, "\"a\\\"b\\\\c\\nd\\te\"");
    // Control characters become \u escapes.
    EXPECT_EQ(Value(std::string("\x01", 1)).dump(), "\"\\u0001\"");
}

TEST(Json, BuildsNestedObjectsAndArrays) {
    Value root(Object{});
    root["ok"] = true;
    root["count"] = 3;
    Value list(Array{});
    list.push(Value("a"));
    list.push(Value(2));
    root["items"] = list;

    const std::string text = root.dump(0);
    EXPECT_EQ(text, "{\"count\":3,\"items\":[\"a\",2],\"ok\":true}");
}

TEST(Json, ParsesRoundTrip) {
    Value root(Object{});
    root["schema"] = "bvmenv.e2e.v1";
    root["ok"] = false;
    root["n"] = 7;
    root["nested"] = Object{{"a", Value(1)}, {"b", Value("two")}};
    Value array(Array{});
    array.push(Value(1.5));
    array.push(Object{{"k", Value("v")}});
    root["arr"] = array;

    const std::string text = root.dump(2);
    std::string error;
    const auto parsed = Value::parse(text, &error);
    ASSERT_TRUE(parsed.has_value()) << error;
    EXPECT_EQ((*parsed)["schema"].as_string(), "bvmenv.e2e.v1");
    EXPECT_FALSE((*parsed)["ok"].as_bool(true));
    EXPECT_EQ((*parsed)["n"].as_int(), 7);
    EXPECT_EQ((*parsed)["nested"]["b"].as_string(), "two");
    ASSERT_EQ((*parsed)["arr"].items().size(), 2u);
    EXPECT_DOUBLE_EQ((*parsed)["arr"].items()[0].as_number(), 1.5);
    EXPECT_EQ((*parsed)["arr"].items()[1]["k"].as_string(), "v");
}

TEST(Json, ParsesEscapesAndUnicode) {
    const auto parsed = Value::parse(R"({"a":"x\ny","b":"\u0041\u00e9"})");
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ((*parsed)["a"].as_string(), "x\ny");
    EXPECT_EQ((*parsed)["b"].as_string(), "A\xc3\xa9");
}

TEST(Json, RejectsMalformedInput) {
    std::string error;
    EXPECT_FALSE(Value::parse("{", &error).has_value());
    EXPECT_FALSE(error.empty());
    EXPECT_FALSE(Value::parse("{\"a\":}", &error).has_value());
    EXPECT_FALSE(Value::parse("[1,]", &error).has_value());
    EXPECT_FALSE(Value::parse("true false", &error).has_value());
    EXPECT_FALSE(Value::parse("\"unterminated", &error).has_value());
}

TEST(Json, MissingFieldReturnsNull) {
    const auto parsed = Value::parse(R"({"a":1})");
    ASSERT_TRUE(parsed.has_value());
    EXPECT_TRUE((*parsed)["missing"].is_null());
    EXPECT_EQ((*parsed)["missing"].as_int(-1), -1);
}
