// blook-vmware-environment: configuration and host layout.
#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace blook::bvmenv {

struct Config {
    // VMware Workstation installation directory (vmrun.exe, vmcli.exe, ...).
    std::filesystem::path vmware_dir;
    // Root of the lab's VM store. Base image under <root>\base, forks under
    // <root>\vms\<name>.
    std::filesystem::path vm_root;
    // Source Windows installation media (used by `base build`).
    std::filesystem::path windows_iso;
    // Repo guest payload (unattend.xml + provisioning scripts).
    std::filesystem::path guest_assets;
    // Optional OCR binary used for BSOD stop codes. Empty = auto detect.
    std::filesystem::path ocr_command;

    std::string guest_user = "blook";
    std::string guest_password = "blook";

    int default_cpus = 4;
    int default_memory_mb = 6144;
    int default_timeout_s = 120;
    int hang_samples = 3;              // consecutive identical frames = hang suspect
    int hang_sample_interval_ms = 2000;

    bool verbose = false;
    std::filesystem::path log_file;    // --verbose target

    // Absolute paths derived from vm_root.
    [[nodiscard]] std::filesystem::path base_dir() const { return vm_root / "base"; }
    [[nodiscard]] std::filesystem::path base_vmx() const { return base_dir() / "blook-base.vmx"; }
    [[nodiscard]] std::filesystem::path vm_dir(const std::string& name) const {
        return vm_root / "vms" / name;
    }
    [[nodiscard]] std::filesystem::path vmx_for(const std::string& name) const {
        return vm_dir(name) / (name + ".vmx");
    }
    [[nodiscard]] std::filesystem::path cache_dir() const { return vm_root / "_cache"; }
    [[nodiscard]] std::filesystem::path evidence_dir() const { return vm_root / "evidence"; }

    [[nodiscard]] std::filesystem::path vmrun() const;
    [[nodiscard]] std::filesystem::path vmcli() const;
    [[nodiscard]] std::filesystem::path vdiskmanager() const;

    // Loads defaults, then BVMENV_* environment overrides:
    //   BVMENV_VMWARE_DIR, BVMENV_VM_ROOT, BVMENV_WINDOWS_ISO,
    //   BVMENV_GUEST_ASSETS, BVMENV_GUEST_USER, BVMENV_GUEST_PASSWORD,
    //   BVMENV_OCR_COMMAND, BVMENV_CPUS, BVMENV_MEMORY_MB
    static Config load();

    // Resolved lazily; returns false and fills `error` when required pieces are
    // missing (used by `bvmenv doctor`).
    bool validate(std::string* error) const;
};

// Directory holding the shipped guest payload (unattend.xml + provisioning
// scripts). Looked up next to the executable: <exe>\guest, then <exe>\..\guest.
std::filesystem::path default_guest_assets();

}  // namespace blook::bvmenv
