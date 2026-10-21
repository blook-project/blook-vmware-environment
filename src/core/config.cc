// blook-vmware-environment: configuration.
#include "blook/bvmenv/config.h"

#include <windows.h>

#include <cstdlib>
#include <string>

#include "blook/bvmenv/process.h"
#include "blook/bvmenv/util.h"

namespace blook::bvmenv {
namespace {

std::string env_or(const char* name, const std::string& fallback) {
    const char* value = std::getenv(name);
    return value && *value ? std::string(value) : fallback;
}

fs::path env_path_or(const char* name, const fs::path& fallback) {
    const char* value = std::getenv(name);
    if (!value || !*value)
        return fallback;
    return fs::path(to_wide(value));
}

// Reads the installation directory from the registry, falling back to the
// historical default location.
fs::path detect_vmware_dir() {
    HKEY key = nullptr;
    const wchar_t* subkeys[] = {
        L"SOFTWARE\\WOW6432Node\\VMware, Inc.\\VMware Workstation",
        L"SOFTWARE\\VMware, Inc.\\VMware Workstation",
    };
    for (const auto* subkey : subkeys) {
        for (HKEY root : {HKEY_LOCAL_MACHINE, HKEY_CURRENT_USER}) {
            if (RegOpenKeyExW(root, subkey, 0, KEY_READ, &key) != ERROR_SUCCESS)
                continue;
            wchar_t buffer[MAX_PATH * 2];
            DWORD size = sizeof(buffer);
            DWORD type = 0;
            const LONG rc = RegQueryValueExW(key, L"InstallPath", nullptr, &type,
                                             reinterpret_cast<LPBYTE>(buffer), &size);
            RegCloseKey(key);
            if (rc == ERROR_SUCCESS && (type == REG_SZ || type == REG_EXPAND_SZ)) {
                std::wstring path(buffer, size / sizeof(wchar_t));
                while (!path.empty() && (path.back() == L'\0' || path.back() == L'\\'))
                    path.pop_back();
                if (!path.empty())
                    return fs::path(path);
            }
        }
    }
    return fs::path(L"Y:\\Program Files (x86)\\VMware\\VMware Workstation");
}

int parse_int(const std::string& text, int fallback) {
    try {
        return std::stoi(text);
    } catch (...) {
        return fallback;
    }
}

fs::path exe_dir() {
    wchar_t buffer[MAX_PATH * 2];
    const DWORD len = GetModuleFileNameW(nullptr, buffer, static_cast<DWORD>(std::size(buffer)));
    if (len == 0)
        return fs::current_path();
    return fs::path(std::wstring(buffer, len)).parent_path();
}

}  // namespace

fs::path default_guest_assets() {
    // The payload lives next to the sources while the binary lives in the build
    // tree (build/windows/<arch>/<mode>/bvmenv.exe) or in a release package
    // (bin/x64/bvmenv.exe + guest/). Walk up until a guest payload is found.
    std::error_code ec;
    fs::path dir = exe_dir();
    for (int level = 0; level < 8; ++level) {
        const fs::path candidate = dir / L"guest";
        if (fs::exists(candidate / L"unattend.xml", ec)) {
            return fs::weakly_canonical(candidate, ec);
        }
        if (!dir.has_parent_path() || dir.parent_path() == dir)
            break;
        dir = dir.parent_path();
    }
    return exe_dir() / L"guest";
}

Config Config::load() {
    Config cfg;
    cfg.vmware_dir = env_path_or("BVMENV_VMWARE_DIR", detect_vmware_dir());
    cfg.vm_root = env_path_or("BVMENV_VM_ROOT", fs::path(L"C:\\blook-vms"));
    cfg.windows_iso = env_path_or(
        "BVMENV_WINDOWS_ISO", fs::path(L"C:\\vm\\zh-cn_windows_11_consumer_editions_version_25h2_"
                                       L"updated_jan_2026_x64_dvd_fb37d9ae.iso"));
    cfg.guest_assets = env_path_or("BVMENV_GUEST_ASSETS", default_guest_assets());
    cfg.ocr_command = env_path_or("BVMENV_OCR_COMMAND", fs::path{});
    cfg.guest_user = env_or("BVMENV_GUEST_USER", cfg.guest_user);
    cfg.guest_password = env_or("BVMENV_GUEST_PASSWORD", cfg.guest_password);
    cfg.default_cpus = parse_int(env_or("BVMENV_CPUS", ""), cfg.default_cpus);
    cfg.default_memory_mb = parse_int(env_or("BVMENV_MEMORY_MB", ""), cfg.default_memory_mb);
    cfg.default_timeout_s = parse_int(env_or("BVMENV_TIMEOUT", ""), cfg.default_timeout_s);
    cfg.hang_samples = parse_int(env_or("BVMENV_HANG_SAMPLES", ""), cfg.hang_samples);
    cfg.max_running_vms = parse_int(env_or("BVMENV_MAX_RUNNING_VMS", ""), cfg.max_running_vms);
    cfg.command_timeout_ms = parse_int(env_or("BVMENV_CMD_TIMEOUT_MS", ""), cfg.command_timeout_ms);
    return cfg;
}

fs::path Config::vmrun() const {
    return vmware_dir / L"vmrun.exe";
}
fs::path Config::vmcli() const {
    return vmware_dir / L"vmcli.exe";
}
fs::path Config::vdiskmanager() const {
    return vmware_dir / L"vmware-vdiskmanager.exe";
}

bool Config::validate(std::string* error) const {
    auto fail = [&](const std::string& message) {
        if (error)
            *error = message;
        return false;
    };
    if (!file_exists(vmrun())) {
        return fail("vmrun.exe not found under vmware_dir (" + to_utf8(vmware_dir.wstring()) +
                    "); set BVMENV_VMWARE_DIR");
    }
    if (!file_exists(vmcli())) {
        return fail("vmcli.exe not found under vmware_dir (" + to_utf8(vmware_dir.wstring()) + ")");
    }
    return true;
}

}  // namespace blook::bvmenv
