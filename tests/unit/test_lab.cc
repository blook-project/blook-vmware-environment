// blook-vmware-environment: lab level behaviour that does not need a healthy
// guest (fail-fast paths and timeout bounds).
//
// The point of these tests is that a broken/absent VM must never make bvmenv
// block: `wait-ready` is the call that used to hang the downstream e2e owners.
#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>

#include "blook/bvmenv/config.h"
#include "blook/bvmenv/lab.h"
#include "blook/bvmenv/util.h"
#include "blook/bvmenv/vmware.h"

using namespace blook::bvmenv;

namespace {

Config scratch_config() {
    Config cfg = Config::load();
    cfg.vm_root = fs::temp_directory_path() / "bvmenv-tests-vm-store";
    ensure_dir(cfg.vm_root / "vms");
    cfg.command_timeout_ms = 15000;
    return cfg;
}

bool vmware_available(const Config& cfg) { return file_exists(cfg.vmcli()); }

}  // namespace

TEST(Lab, WaitReadyFailsFastForMissingVm) {
    Config cfg = scratch_config();
    Vmware vmware(cfg);
    Lab lab(cfg, vmware);
    const auto started = std::chrono::steady_clock::now();
    EXPECT_THROW(lab.wait_ready("definitely-not-a-vm", std::chrono::seconds(30)), Error);
    const auto elapsed = std::chrono::steady_clock::now() - started;
    // Must be a name check, not a poll loop.
    EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(), 2000);
}

TEST(Lab, StateThrowsForMissingVm) {
    Config cfg = scratch_config();
    Vmware vmware(cfg);
    Lab lab(cfg, vmware);
    ASSERT_FALSE(vmware.find("no-such-vm").has_value());
    EXPECT_THROW(lab.state("no-such-vm", StateOptions{}), Error);
}

TEST(Lab, ResetReportsFailureInsteadOfThrowingForMissingVm) {
    Config cfg = scratch_config();
    Vmware vmware(cfg);
    Lab lab(cfg, vmware);
    const ResetReport report = lab.reset("no-such-vm", "test");
    EXPECT_TRUE(report.performed);
    EXPECT_FALSE(report.ok);
}

TEST(Lab, CreateRejectsReservedAndInvalidNames) {
    Config cfg = scratch_config();
    Vmware vmware(cfg);
    Lab lab(cfg, vmware);
    CreateOptions options;
    options.name = "base";
    EXPECT_THROW((void)lab.create(options), Error);
    options.name = "../escape";
    EXPECT_THROW((void)vmware.find(options.name), Error);
}

TEST(Lab, WaitReadyHonoursTimeoutOnPoweredOffVm) {
    Config cfg = scratch_config();
    if (!vmware_available(cfg)) {
        GTEST_SKIP() << "VMware not installed on this host";
    }
    // Create a VM directory with a vmx that is not running: wait_ready must
    // return within the timeout (and not start the VM).
    const std::string name = "bvmenv-tests-off";
    const fs::path dir = cfg.vm_dir(name);
    ensure_dir(dir);
    write_text(dir / (name + ".vmx"), "displayName = \"" + name + "\"\nmemsize = \"1024\"\n");
    Vmware vmware(cfg);
    Lab lab(cfg, vmware);
    const auto started = std::chrono::steady_clock::now();
    bool threw = false;
    try {
        lab.wait_ready(name, std::chrono::seconds(3));
    } catch (const Error&) {
        threw = true;
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - started);
    EXPECT_TRUE(threw) << "powered-off VM must fail instead of waiting";
    EXPECT_LT(elapsed.count(), 10);
    remove_tree(dir);
}

TEST(Lab, DoctorReportsMissingVmwareGracefully) {
    Config cfg = scratch_config();
    cfg.vmware_dir = fs::temp_directory_path() / "bvmenv-tests-no-vmware";
    Vmware vmware(cfg);
    Lab lab(cfg, vmware);
    const json::Value report = lab.doctor();
    EXPECT_EQ(report["schema"].as_string(), "bvmenv.doctor.v1");
    EXPECT_FALSE(report["ok"].as_bool(true));
    EXPECT_FALSE(report["config_ok"].as_bool(true));
    EXPECT_FALSE(report["config_error"].as_string().empty());
}
