// blook-vmware-environment: .vmx parsing unit tests.
#include <gtest/gtest.h>

#include "blook/bvmenv/vmx.h"

using namespace blook::bvmenv;

TEST(Vmx, ParsesKeysValuesAndComments) {
    const Vmx vmx = Vmx::parse(
        "# a comment\n"
        ".encoding = \"UTF-8\"\n"
        "displayName = \"vm-drv\"\n"
        "memsize = \"6144\"\n"
        "vhv.enable = \"TRUE\"\n"
        "  spurious = no-space-value  \n");
    EXPECT_EQ(vmx.get("displayName"), "vm-drv");
    EXPECT_EQ(vmx.get_int("memsize"), 6144);
    EXPECT_TRUE(vmx.get_bool("vhv.enable"));
    EXPECT_EQ(vmx.get("spurious"), "no-space-value");
    EXPECT_EQ(vmx.get("missing", "fallback"), "fallback");
    EXPECT_EQ(vmx.get_int("missing", 7), 7);
    EXPECT_TRUE(vmx.has(".encoding"));
    EXPECT_FALSE(vmx.has("nope"));
}

TEST(Vmx, SetUpdatesInPlaceAndAppends) {
    Vmx vmx = Vmx::parse("displayName = \"old\"\nmemsize = \"2048\"\n");
    vmx.set("displayName", "new");
    vmx.set("numvcpus", "4");
    EXPECT_EQ(vmx.get("displayName"), "new");
    EXPECT_EQ(vmx.get("numvcpus"), "4");
    // Existing key stays in position 0.
    EXPECT_EQ(vmx.entries()[0].key, "displayName");
    EXPECT_EQ(vmx.entries().size(), 3u);
}

TEST(Vmx, RemoveDropsKeys) {
    Vmx vmx = Vmx::parse("a = \"1\"\nb = \"2\"\nc = \"3\"\n");
    vmx.remove("b");
    EXPECT_FALSE(vmx.has("b"));
    EXPECT_EQ(vmx.dump(), "a = \"1\"\nc = \"3\"\n");
}

TEST(Vmx, DumpRoundTrips) {
    const std::string original =
        ".encoding = \"UTF-8\"\n"
        "virtualHW.version = \"21\"\n"
        "sata0:1.fileName = \"C:/vm/windows11.iso\"\n"
        "serial0.fileName = \"\\\\.\\pipe\\blook-kd\"\n";
    const Vmx first = Vmx::parse(original);
    const Vmx second = Vmx::parse(first.dump());
    EXPECT_EQ(first.dump(), second.dump());
    EXPECT_EQ(second.get("serial0.fileName"), "\\\\.\\pipe\\blook-kd");
}

TEST(Vmx, BoolParsingIsCaseInsensitive) {
    const Vmx vmx = Vmx::parse("a = \"true\"\nb = \"FALSE\"\nc = \"1\"\n");
    EXPECT_TRUE(vmx.get_bool("a"));
    EXPECT_FALSE(vmx.get_bool("b", true));
    // Non true/false values fall back.
    EXPECT_TRUE(vmx.get_bool("c", true));
}

TEST(Vmx, IgnoresJunkLines) {
    const Vmx vmx = Vmx::parse("garbage-without-equals\n= \"no key\"\nk = \"v\"\n");
    EXPECT_EQ(vmx.entries().size(), 1u);
    EXPECT_EQ(vmx.get("k"), "v");
}
