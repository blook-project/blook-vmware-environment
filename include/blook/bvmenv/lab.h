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

// ---------------------------------------------------------------------------
// Host disk safety
//
// The lab attaches/detaches its own VHDs only. A bring-up helper once detached
// the user's DevDrive by enumerating host disks, so all disk operations go
// through these helpers: they accept a path only when it is inside
// BVMENV_VM_ROOT and named like one of the tool's own images.
// ---------------------------------------------------------------------------
bool is_owned_vhd_path(const std::filesystem::path& vm_root,
                       const std::filesystem::path& candidate);
// Detaches exactly `vhd` (by explicit path, never by disk number). Throws when
// the path is not owned by bvmenv.
bool detach_own_vhd(const Config& config, const std::filesystem::path& vhd);
// Read-only report of attached file-backed virtual disks (for `doctor`).
std::vector<std::string> attached_file_backed_vhds();

struct CreateOptions {
    std::string name;
    std::string from = "base";
    int cpus = 0;       // 0 = use config default
    int memory_mb = 0;  // 0 = use config default
    // Rewrite serial0 to a named pipe so kd.exe can attach to the fork
    // (`bvmenv kd <vm>` prints the exact command line).
    bool kd_pipe = false;
    bool start = true;
    bool wait_ready = true;
    int ready_timeout_s = 240;
};

struct StateOptions {
    bool capture = true;  // capture + classify a frame
    bool use_ocr = true;
    int samples = 1;      // >1 enables hang detection
    int interval_ms = 0;  // 0 = config default
    // Hard wall clock for `state`: a hung guest must never hang the caller.
    int probe_budget_s = 20;
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

// Another type-2 hypervisor VMM driver running alongside VMware's vmx86 is a
// known cause of the host freeze this lab hit twice (Kernel-Power 41, no dump):
// MuMu emulator's MuMuVMMDrv, VirtualBox's VBox*, Nemu*. Scan for them and stop
// them before running VM tests (`bvmenv host-prep`).
struct VmmDriver {
    std::string name;
    std::string state;
    bool running = false;
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
    [[nodiscard]] std::vector<VmmDriver> other_vmm_drivers() const;

    // ---- misc ------------------------------------------------------------
    // Forces a real bugcheck through the i8042 keyboard controller
    // (Ctrl+ScrollLock twice). Requires CrashOnCtrlScroll in the image.
    bool force_bugcheck(const std::string& name);

    [[nodiscard]] const Config& config() const { return cfg_; }

   private:
    StateReport state_impl(const std::string& name, const StateOptions& options);
    StateReport sample_once(const std::string& name, const StateOptions& options);
    void base_build_impl(const BaseBuildOptions& options);

    Config cfg_;
    Vmware& vmware_;
};

}  // namespace blook::bvmenv
