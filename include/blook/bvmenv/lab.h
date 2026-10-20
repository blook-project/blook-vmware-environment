// blook-vmware-environment: high level lab operations.
#pragma once

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "blook/bvmenv/config.h"
#include "blook/bvmenv/types.h"
#include "blook/bvmenv/vmware.h"

namespace blook::bvmenv {

struct CreateOptions {
    std::string name;
    std::string from = "base";
    int cpus = 0;       // 0 = use config default
    int memory_mb = 0;  // 0 = use config default
    bool start = true;
    bool wait_ready = true;
    int ready_timeout_s = 240;
};

struct StateOptions {
    bool capture = true;  // capture + classify a frame
    bool use_ocr = true;
    int samples = 1;      // >1 enables hang detection
    int interval_ms = 0;  // 0 = config default
};

struct BaseBuildOptions {
    std::filesystem::path iso;
    bool force = false;  // wipe an existing base image
    int disk_gb = 80;
    int ready_timeout_s = 2400;
};

struct BaseVerifyReport {
    bool ok = false;
    std::vector<std::pair<std::string, bool>> checks;
    std::string provision_json;
    std::vector<std::string> notes;

    [[nodiscard]] json::Value to_json() const;
};

class Lab {
   public:
    Lab(Config cfg, Vmware& vmware) : cfg_(std::move(cfg)), vmware_(vmware) {}

    // ---- lifecycle -------------------------------------------------------
    VmInfo create(const CreateOptions& options);
    void destroy(const std::string& name);
    ResetReport reset(const std::string& name, const std::string& reason);
    bool wait_ready(const std::string& name, std::chrono::seconds timeout);

    // ---- observation -----------------------------------------------------
    StateReport state(const std::string& name, const StateOptions& options);

    // ---- execution -------------------------------------------------------
    ExecResult exec(const ExecRequest& request);
    E2eResult e2e(const E2eRequest& request);

    // ---- base image ------------------------------------------------------
    BaseVerifyReport base_verify(const std::string& name = "base", bool deep = false);
    void base_build(const BaseBuildOptions& options);
    // Machine-readable snapshot of the host/toolchain state (`bvmenv doctor`).
    json::Value doctor();

    // ---- misc ------------------------------------------------------------
    // Forces a real bugcheck through the i8042 keyboard controller
    // (Ctrl+ScrollLock twice). Requires CrashOnCtrlScroll in the image.
    bool force_bugcheck(const std::string& name);

    [[nodiscard]] const Config& config() const { return cfg_; }

   private:
    StateReport sample_once(const std::string& name, const StateOptions& options);

    Config cfg_;
    Vmware& vmware_;
};

}  // namespace blook::bvmenv
