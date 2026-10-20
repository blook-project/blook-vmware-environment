// blook-vmware-environment: VMware Workstation backend.
//
// Two subprocess backends are used, both from the Workstation installation:
//   * vmrun.exe  - power, snapshots, linked clones, guest operations
//                  (copy/run in guest over VMCI, no guest network needed)
//   * vmcli.exe  - host side operations vmrun cannot do without guest
//                  credentials: MKS screenshot capture, key injection,
//                  tools/state queries
#pragma once

#include <chrono>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "blook/bvmenv/config.h"
#include "blook/bvmenv/process.h"
#include "blook/bvmenv/types.h"

namespace blook::bvmenv {

class Vmware {
public:
    explicit Vmware(Config cfg) : cfg_(std::move(cfg)) {}

    [[nodiscard]] const Config& config() const { return cfg_; }

    // ---- discovery -------------------------------------------------------
    // Scans the VM store. `name` must match a directory under <root>\vms or be
    // the reserved name "base".
    [[nodiscard]] std::optional<VmInfo> find(const std::string& name) const;
    [[nodiscard]] std::vector<VmInfo> list() const;
    [[nodiscard]] VmInfo base() const;

    // ---- lifecycle -------------------------------------------------------
    void start(const std::string& name, bool gui = false);
    void stop(const std::string& name, bool hard = true);
    void reset_power(const std::string& name);
    void wait_power(const std::string& name, PowerState desired,
                    std::chrono::seconds timeout);

    void snapshot(const std::string& name, const std::string& snapshot_name);
    [[nodiscard]] bool has_snapshot(const std::string& name,
                                    const std::string& snapshot_name) const;
    void revert(const std::string& name, const std::string& snapshot_name);
    // Linked clone from a snapshot of the base image.
    void clone_linked(const std::filesystem::path& src_vmx,
                      const std::filesystem::path& dst_vmx,
                      const std::string& snapshot_name);
    void delete_vm(const std::string& name);

    // ---- host side state (no guest cooperation required) -----------------
    [[nodiscard]] PowerState power_state(const std::string& name) const;
    [[nodiscard]] ToolInfo tools_info(const std::string& name) const;
    // Captures the guest console framebuffer to a PNG on the host. Works while
    // the guest is booting, bugchecked or hung.
    bool capture_screenshot(const std::string& name,
                            const std::filesystem::path& png) const;
    // Best effort: IP from guest ops, else from the vmnet DHCP lease database.
    [[nodiscard]] std::string guest_ip(const std::string& name) const;

    // Injects a key event into the guest console (HID usage code + modifier
    // bitmask). Used for boot prompts and the Ctrl+ScrollLock crash primitive.
    bool send_key(const std::string& name, int hid_code, int modifier) const;
    bool send_key_sequence(const std::string& name, const std::string& text) const;

    // ---- guest operations (VMware Tools + credentials required) ----------
    bool guest_reachable(const std::string& name) const;
    ExecResult run(const ExecRequest& request);
    void copy_to_guest(const std::string& vm, const std::filesystem::path& local,
                       const std::string& guest_path);
    void copy_from_guest(const std::string& vm, const std::string& guest_path,
                         const std::filesystem::path& local);
    // Recursive copy; returns the number of files transferred.
    int push_tree(const std::string& vm, const std::filesystem::path& local_dir,
                  const std::string& guest_dir);
    int pull_tree(const std::string& vm, const std::string& guest_dir,
                  const std::filesystem::path& local_dir);
    void guest_mkdirs(const std::string& vm, const std::string& guest_dir);
    // Runs a command and returns its stdout (used by base verify).
    std::string guest_capture(const std::string& vm, const std::string& command,
                              bool* ok = nullptr,
                              std::chrono::seconds timeout = std::chrono::seconds(120));

    // ---- raw helpers -----------------------------------------------------
    ProcessResult vmrun(const std::vector<std::wstring>& args,
                        std::chrono::milliseconds timeout = std::chrono::milliseconds(0),
                        bool silent = true) const;
    ProcessResult vmcli(const std::vector<std::wstring>& args,
                        std::chrono::milliseconds timeout =
                            std::chrono::milliseconds(0)) const;
    [[nodiscard]] bool service_running() const;
    [[nodiscard]] std::string vmrun_version() const;

    // Path helpers for a VM's own artifacts.
    [[nodiscard]] std::filesystem::path vm_dir_of(const VmInfo& vm) const;
    [[nodiscard]] std::filesystem::path log_path_of(const VmInfo& vm) const;
    [[nodiscard]] std::filesystem::path serial_path_of(const VmInfo& vm) const;

private:
    [[nodiscard]] std::vector<std::wstring> run_args_base(const VmInfo& vm) const;
    [[nodiscard]] std::wstring vmx_wide(const std::string& name) const;

    Config cfg_;
};

}  // namespace blook::bvmenv
