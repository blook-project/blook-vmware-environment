// blook-vmware-environment: unit test runner.
//
// A tiny main() so the test target does not need gtest_main (and therefore no
// extra package / runtime mismatch).
#include <gtest/gtest.h>

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
