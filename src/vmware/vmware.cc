// blook-vmware-environment: VMware Workstation backend (vmrun + vmcli).
#include "blook/bvmenv/vmware.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <regex>
#include <sstream>
#include <string>
#include <thread>

#include "blook/bvmenv/process.h"
#include "blook/bvmenv/util.h"
#include "blook/bvmenv/vmx.h"

namespace blook::bvmenv {
namespace {

std::wstring widen(const std::string& s) {
    return to_wide(s);
}

std::wstring widen(const fs::path& p) {
    return p.wstring();
}

// vmrun reports "Total running VMs: N" plus the vmx paths.
std::vector<std::string> split_lines(const std::string& text) {
    std::vector<std::string> lines;
    std::size_t pos = 0;
    while (pos < text.size()) {
        std::size_t eol = text.find('\n', pos);
        if (eol == std::string::npos)
            eol = text.size();
        std::string line = text.substr(pos, eol - pos);
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (!line.empty())
            lines.push_back(line);
        pos = eol + 1;
    }
    return lines;
}

std::string value_after_colon(const std::string& text, const std::string& key) {
    for (const auto& line : split_lines(text)) {
        const std::size_t colon = line.find(':');
        if (colon == std::string::npos)
            continue;
        if (trim(line.substr(0, colon)) == key)
            return trim(line.substr(colon + 1));
    }
    return {};
}

}  // namespace

// ---------------------------------------------------------------------------
// raw helpers
// ---------------------------------------------------------------------------
ProcessResult Vmware::vmrun(const std::vector<std::wstring>& args,
                            std::chrono::milliseconds timeout, bool silent) const {
    std::vector<std::wstring> argv;
    argv.push_back(cfg_.vmrun().wstring());
    argv.push_back(L"-T");
    argv.push_back(L"ws");
    for (const auto& a : args)
        argv.push_back(a);

    ProcessOptions options;
    options.command_line = build_command_line(argv);
    options.timeout =
        timeout.count() > 0 ? timeout : std::chrono::milliseconds(cfg_.command_timeout_ms);
    const auto started = std::chrono::steady_clock::now();
    ProcessResult result = run_process(options);
    log_command(options.command_line,
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - started)
                    .count(),
                static_cast<int>(result.exit_code), result.timed_out);
    (void)silent;
    return result;
}

ProcessResult Vmware::vmcli(const std::vector<std::wstring>& args,
                            std::chrono::milliseconds timeout) const {
    std::vector<std::wstring> argv;
    argv.push_back(cfg_.vmcli().wstring());
    for (const auto& a : args)
        argv.push_back(a);
    ProcessOptions options;
    options.command_line = build_command_line(argv);
    options.timeout =
        timeout.count() > 0 ? timeout : std::chrono::milliseconds(cfg_.command_timeout_ms);
    const auto started = std::chrono::steady_clock::now();
    ProcessResult result = run_process(options);
    log_command(options.command_line,
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - started)
                    .count(),
                static_cast<int>(result.exit_code), result.timed_out);
    return result;
}

bool Vmware::service_running() const {
    SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!manager)
        return false;
    SC_HANDLE service = OpenServiceW(manager, L"VMAuthdService", SERVICE_QUERY_STATUS);
    bool running = false;
    if (service) {
        SERVICE_STATUS status{};
        if (QueryServiceStatus(service, &status)) {
            running = status.dwCurrentState == SERVICE_RUNNING;
        }
        CloseServiceHandle(service);
    }
    CloseServiceHandle(manager);
    return running;
}

std::string Vmware::vmrun_version() const {
    const ProcessResult r = vmrun({L"--version"}, std::chrono::seconds(20));
    return trim(r.out + r.err);
}

std::wstring Vmware::vmx_wide(const std::string& name) const {
    if (name == "base")
        return cfg_.base_vmx().wstring();
    return cfg_.vmx_for(name).wstring();
}

// ---------------------------------------------------------------------------
// discovery
// ---------------------------------------------------------------------------
VmInfo Vmware::base() const {
    VmInfo info;
    info.name = "base";
    info.vmx = to_utf8(cfg_.base_vmx().wstring());
    info.dir = to_utf8(cfg_.base_dir().wstring());
    info.is_base = true;
    if (!file_exists(cfg_.base_vmx())) {
        throw Error("base image not found at " + info.vmx + " (run: bvmenv base build)");
    }
    const Vmx vmx = Vmx::load(cfg_.base_vmx());
    info.cpus = vmx.get_int("numvcpus", 0);
    info.memory_mb = vmx.get_int("memsize", 0);
    return info;
}

std::optional<VmInfo> Vmware::find(const std::string& name) const {
    if (name == "base") {
        if (!file_exists(cfg_.base_vmx()))
            return std::nullopt;
        VmInfo info = base();
        info.power = power_state("base");
        return info;
    }
    if (name.empty() || name.find_first_of("\\/") != std::string::npos || name == "." ||
        name == "..") {
        throw Error("invalid VM name: " + name);
    }
    const fs::path dir = cfg_.vm_dir(name);
    const fs::path vmx = cfg_.vmx_for(name);
    if (!file_exists(vmx))
        return std::nullopt;
    VmInfo info;
    info.name = name;
    info.vmx = to_utf8(vmx.wstring());
    info.dir = to_utf8(dir.wstring());
    const Vmx doc = Vmx::load(vmx);
    info.cpus = doc.get_int("numvcpus", 0);
    info.memory_mb = doc.get_int("memsize", 0);
    // Metadata sidecar written by `create`.
    const fs::path meta = dir / ".bvmenv.json";
    if (file_exists(meta)) {
        std::string error;
        if (auto parsed = json::Value::parse(read_text(meta), &error);
            parsed && parsed->is_object()) {
            info.base = (*parsed)["base"].as_string();
            info.snapshot = (*parsed)["snapshot"].as_string();
            info.created_at = (*parsed)["created_at"].as_string();
        }
    }
    info.power = power_state(name);
    return info;
}

std::vector<VmInfo> Vmware::list() const {
    std::vector<VmInfo> out;
    if (file_exists(cfg_.base_vmx())) {
        VmInfo info = base();
        info.power = power_state("base");
        out.push_back(std::move(info));
    }
    const fs::path root = cfg_.vm_root / "vms";
    std::error_code ec;
    if (fs::is_directory(root, ec)) {
        for (const auto& entry : fs::directory_iterator(root, ec)) {
            if (!entry.is_directory())
                continue;
            const std::string name = to_utf8(entry.path().filename().wstring());
            if (auto info = find(name))
                out.push_back(std::move(*info));
        }
    }
    std::sort(out.begin(), out.end(), [](const VmInfo& a, const VmInfo& b) {
        if (a.is_base != b.is_base)
            return a.is_base;
        return a.name < b.name;
    });
    return out;
}

// ---------------------------------------------------------------------------
// lifecycle
// ---------------------------------------------------------------------------
int Vmware::running_managed_count() const {
    int running = 0;
    for (const auto& vm : list()) {
        if (vm.power == PowerState::On)
            ++running;
    }
    return running;
}

void Vmware::start(const std::string& name, bool gui) {
    // Budget guard: the host has frozen twice under many concurrent guests.
    if (!allow_many_ && power_state(name) != PowerState::On) {
        const int running = running_managed_count();
        if (running >= cfg_.max_running_vms) {
            std::string names;
            for (const auto& vm : this->list()) {
                if (vm.power != PowerState::On)
                    continue;
                names += (names.empty() ? "" : ", ") + vm.name;
            }
            throw Error("VM budget reached (" + std::to_string(running) + " of " +
                        std::to_string(cfg_.max_running_vms) + " managed VMs running: " + names +
                        "); stop one first (bvmenv stop <vm>) or pass --allow-many "
                        "(BVMENV_MAX_RUNNING_VMS overrides the budget)");
        }
    }
    const ProcessResult r =
        vmrun({L"start", vmx_wide(name), gui ? L"gui" : L"nogui"}, std::chrono::seconds(180));
    if (r.exit_code != 0) {
        throw Error("vmrun start failed for " + name + ": " + trim(r.out + r.err));
    }
}

void Vmware::stop(const std::string& name, bool hard) {
    const ProcessResult r =
        vmrun({L"stop", vmx_wide(name), hard ? L"hard" : L"soft"}, std::chrono::seconds(180));
    if (r.exit_code != 0) {
        throw Error("vmrun stop failed for " + name + ": " + trim(r.out + r.err));
    }
}

void Vmware::reset_power(const std::string& name) {
    const ProcessResult r = vmrun({L"reset", vmx_wide(name)}, std::chrono::seconds(180));
    if (r.exit_code != 0) {
        throw Error("vmrun reset failed for " + name + ": " + trim(r.out + r.err));
    }
}

void Vmware::wait_power(const std::string& name, PowerState desired, std::chrono::seconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (power_state(name) == desired)
            return;
        sleep_ms(500);
    }
    throw Error("timed out waiting for VM " + name + " to become " +
                std::string(to_string(desired)));
}

void Vmware::snapshot(const std::string& name, const std::string& snapshot_name) {
    const ProcessResult r =
        vmrun({L"snapshot", vmx_wide(name), widen(snapshot_name)}, std::chrono::seconds(600));
    if (r.exit_code != 0) {
        throw Error("vmrun snapshot failed: " + trim(r.out + r.err));
    }
}

bool Vmware::has_snapshot(const std::string& name, const std::string& snapshot_name) const {
    // listSnapshots fails on a VM without snapshots, so ignore the exit code.
    const ProcessResult r = vmrun({L"listSnapshots", vmx_wide(name)}, std::chrono::seconds(60));
    for (const auto& line : split_lines(r.out + r.err)) {
        if (trim(line) == snapshot_name)
            return true;
    }
    return false;
}

void Vmware::revert(const std::string& name, const std::string& snapshot_name) {
    const ProcessResult r = vmrun({L"revertToSnapshot", vmx_wide(name), widen(snapshot_name)},
                                  std::chrono::seconds(300));
    if (r.exit_code != 0) {
        throw Error("vmrun revertToSnapshot failed: " + trim(r.out + r.err));
    }
}

void Vmware::clone_linked(const fs::path& src_vmx, const fs::path& dst_vmx,
                          const std::string& snapshot_name) {
    ensure_dir(dst_vmx.parent_path());
    const ProcessResult r = vmrun({L"clone", src_vmx.wstring(), dst_vmx.wstring(), L"linked",
                                   L"-snapshot=" + widen(snapshot_name)},
                                  std::chrono::seconds(600));
    if (r.exit_code != 0 || !file_exists(dst_vmx)) {
        throw Error("vmrun clone (linked) failed: " + trim(r.out + r.err));
    }
}

void Vmware::delete_vm(const std::string& name) {
    const std::wstring vmx = vmx_wide(name);
    if (power_state(name) != PowerState::Off) {
        stop(name, true);
        wait_power(name, PowerState::Off, std::chrono::seconds(60));
    }
    const ProcessResult r = vmrun({L"deleteVM", vmx}, std::chrono::seconds(300));
    if (r.exit_code != 0) {
        throw Error("vmrun deleteVM failed: " + trim(r.out + r.err));
    }
    // VMware removes the vmx and disks but leaves the directory behind.
    if (name != "base") {
        remove_tree(cfg_.vm_dir(name));
    }
}

// ---------------------------------------------------------------------------
// host side state
// ---------------------------------------------------------------------------
PowerState Vmware::power_state(const std::string& name) const {
    // `vmrun list` only reports running VMs; use vmcli for the precise state.
    const ProcessResult q = vmcli({vmx_wide(name), L"Power", L"query"}, std::chrono::seconds(30));
    const std::string text = to_lower(q.out + q.err);
    if (text.find("poweredon") != std::string::npos || value_after_colon(q.out, "state") == "on") {
        return PowerState::On;
    }
    if (text.find("poweredoff") != std::string::npos ||
        value_after_colon(q.out, "state") == "off") {
        return PowerState::Off;
    }
    if (text.find("suspended") != std::string::npos)
        return PowerState::Suspended;

    // Fallback: parse `vmrun list`.
    const ProcessResult r = vmrun({L"list"}, std::chrono::seconds(30));
    const std::string vmx = to_lower(to_utf8(vmx_wide(name)));
    for (const auto& line : split_lines(r.out)) {
        if (to_lower(line) == vmx)
            return PowerState::On;
    }
    if (file_exists(cfg_.vm_root / "vms" / name / (name + ".vmss")))
        return PowerState::Suspended;
    if (file_exists(cfg_.base_dir() / "blook-base.vmss") && name == "base") {
        return PowerState::Suspended;
    }
    return PowerState::Off;
}

ToolInfo Vmware::tools_info(const std::string& name) const {
    ToolInfo info;
    const ProcessResult r = vmcli({vmx_wide(name), L"Tools", L"Query"}, std::chrono::seconds(30));
    const std::string text = r.out + r.err;
    if (r.exit_code != 0) {
        info.state = ToolsState::Unknown;
        return info;
    }
    info.version = value_after_colon(text, "version");
    info.running = to_lower(value_after_colon(text, "running")) == "true";
    const std::string version_status = to_lower(value_after_colon(text, "versionStatus"));
    // `vmSupportStatus` reports "not_running" even for a healthy running guest on
    // this Workstation build, so it must not decide the state: relying on it made
    // `wait-ready` (and therefore `base verify --deep`) time out on a good clone.
    if (version_status == "no_tools") {
        info.state = ToolsState::NotInstalled;
    } else if (info.running) {
        info.state = ToolsState::Running;
    } else {
        info.state = ToolsState::Installed;
    }
    if (info.version == "0")
        info.version.clear();
    return info;
}

bool Vmware::capture_screenshot(const std::string& name, const fs::path& png) const {
    ensure_dir(png.parent_path());
    std::error_code ec;
    fs::remove(png, ec);
    const ProcessResult r = vmcli({vmx_wide(name), L"MKS", L"captureScreenshot", png.wstring()},
                                  std::chrono::seconds(60));
    if (r.exit_code != 0)
        return false;
    return file_exists(png) && blook::bvmenv::file_size(png) > 0;
}

std::string Vmware::guest_ip(const std::string& name) const {
    const ProcessResult r = vmrun({L"getGuestIPAddress", vmx_wide(name)}, std::chrono::seconds(30));
    if (r.exit_code == 0) {
        const std::string value = trim(r.out);
        if (!value.empty() && value.find('.') != std::string::npos &&
            value.find("Unknown") == std::string::npos) {
            return value;
        }
    }
    return {};
}

bool Vmware::send_key(const std::string& name, int hid_code, int modifier) const {
    const ProcessResult r =
        vmcli({vmx_wide(name), L"MKS", L"sendKeyEvent", widen(std::to_string(hid_code)),
               widen(std::to_string(modifier))},
              std::chrono::seconds(30));
    return r.exit_code == 0;
}

bool Vmware::send_key_sequence(const std::string& name, const std::string& text) const {
    const ProcessResult r =
        vmcli({vmx_wide(name), L"MKS", L"sendKeySequence", widen(text)}, std::chrono::seconds(60));
    return r.exit_code == 0;
}

// ---------------------------------------------------------------------------
// guest operations
// ---------------------------------------------------------------------------
bool Vmware::guest_reachable(const std::string& name) const {
    const ProcessResult r = vmrun({L"checkToolsState", vmx_wide(name)}, std::chrono::seconds(30));
    return contains_ci(r.out, "running");
}

bool Vmware::guest_ping(const std::string& name, std::chrono::milliseconds timeout) const {
    const ProcessResult r = vmrun(
        {L"-gu", widen(cfg_.guest_user), L"-gp", widen(cfg_.guest_password), L"runProgramInGuest",
         vmx_wide(name), L"C:\\Windows\\System32\\cmd.exe", L"/c", L"exit 0"},
        timeout);
    return r.exit_code == 0 && !r.timed_out;
}

void Vmware::run_script(const std::string& vm, const std::string& script,
                        std::chrono::milliseconds timeout) const {
    const ProcessResult r =
        vmrun({L"-gu", widen(cfg_.guest_user), L"-gp", widen(cfg_.guest_password),
               L"runScriptInGuest", vmx_wide(vm), L"", widen(script)},
              timeout);
    if (r.exit_code != 0 || r.timed_out) {
        throw Error(std::string("guest script failed") + (r.timed_out ? " (timeout)" : "") + ": " +
                    tail_of(trim(r.out + r.err), 400));
    }
}

void Vmware::guest_mkdirs(const std::string& vm, const std::string& guest_dir) {
    // NOTE: must not go through run()/run_once() - run_once() calls this method to
    // create its own job directory, so routing it back through run() recurses
    // until the stack overflows. Creating directories stays on the plain script
    // path; the SYSTEM fallback for exec covers the command itself.
    run_script(vm, "if not exist \"" + guest_dir + "\" mkdir \"" + guest_dir + "\"",
               std::chrono::seconds(60));
}

void Vmware::copy_to_guest(const std::string& vm, const fs::path& local,
                           const std::string& guest_path) {
    const ProcessResult r =
        vmrun({L"-gu", widen(cfg_.guest_user), L"-gp", widen(cfg_.guest_password),
               L"copyFileFromHostToGuest", vmx_wide(vm), local.wstring(), widen(guest_path)},
              std::chrono::seconds(300));
    if (r.exit_code != 0) {
        throw Error("copy to guest failed (" + to_utf8(local.wstring()) + " -> " + guest_path +
                    "): " + trim(r.out + r.err));
    }
}

void Vmware::copy_from_guest(const std::string& vm, const std::string& guest_path,
                             const fs::path& local) {
    ensure_dir(local.parent_path());
    const ProcessResult r =
        vmrun({L"-gu", widen(cfg_.guest_user), L"-gp", widen(cfg_.guest_password),
               L"copyFileFromGuestToHost", vmx_wide(vm), widen(guest_path), local.wstring()},
              std::chrono::seconds(300));
    if (r.exit_code != 0) {
        throw Error("copy from guest failed (" + guest_path + " -> " + to_utf8(local.wstring()) +
                    "): " + trim(r.out + r.err));
    }
}

int Vmware::push_tree(const std::string& vm, const fs::path& local_dir,
                      const std::string& guest_dir) {
    if (!fs::is_directory(local_dir))
        throw Error("bundle is not a directory: " + to_utf8(local_dir.wstring()));
    guest_mkdirs(vm, guest_dir);
    int count = 0;
    for (const auto& entry : fs::recursive_directory_iterator(local_dir)) {
        if (!entry.is_regular_file())
            continue;
        const fs::path rel = fs::relative(entry.path(), local_dir);
        std::string rel_guest = to_utf8(rel.generic_wstring());
        std::replace(rel_guest.begin(), rel_guest.end(), '/', '\\');
        const std::string target = guest_dir + "\\" + rel_guest;
        const std::size_t slash = target.find_last_of('\\');
        if (slash != std::string::npos && slash > guest_dir.size()) {
            guest_mkdirs(vm, target.substr(0, slash));
        }
        copy_to_guest(vm, entry.path(), target);
        ++count;
    }
    return count;
}

int Vmware::pull_tree(const std::string& vm, const std::string& guest_dir,
                      const fs::path& local_dir) {
    // Enumerate the guest tree through `dir /b /s /a-d` (no shared folders).
    const ProcessResult r =
        vmrun({L"-gu", widen(cfg_.guest_user), L"-gp", widen(cfg_.guest_password),
               L"runProgramInGuest", vmx_wide(vm), L"C:\\Windows\\System32\\cmd.exe", L"/c",
               L"dir /b /s /a-d " + widen(guest_dir)},
              std::chrono::seconds(120));
    int count = 0;
    const std::string base = to_lower(guest_dir);
    for (const auto& raw : split_lines(r.out)) {
        std::string line = trim(raw);
        if (line.empty())
            continue;
        if (!contains_ci(line, "File(s)") && !contains_ci(line, "Dir(s)")) {
            // lines are absolute guest paths
        }
        if (line.size() < 3 || line[1] != ':')
            continue;
        std::string rel = line.substr(base.size());
        if (!rel.empty() && (rel[0] == '\\' || rel[0] == '/'))
            rel = rel.substr(1);
        if (rel.empty())
            continue;
        std::replace(rel.begin(), rel.end(), '\\', '/');
        const fs::path target = local_dir / fs::path(rel);
        copy_from_guest(vm, line, target);
        ++count;
    }
    return count;
}

std::string Vmware::guest_capture(const std::string& vm, const std::string& command, bool* ok,
                                  std::chrono::seconds timeout) {
    // Reuse the general execution path with a throwaway run directory.
    ExecRequest request;
    request.vm = vm;
    request.command_line = command;
    request.timeout = timeout;
    request.run_id = "capture-" + std::to_string(GetTickCount64());
    ExecResult result = run(request);
    if (ok)
        *ok = result.ran && !result.timed_out && result.exit_code == 0;
    return result.out;
}

ExecResult Vmware::run(const ExecRequest& request) {
    // Prefer the user session (HKCU, user profile), but never fail because the
    // guest sits at the logon/lock UI: fall back to a SYSTEM scheduled task.
    ExecResult primary = run_once(request, request.elevated);
    auto sticky = [&](ExecResult r) {
        if (exceeds_timeout_budget(r, request.timeout)) {
            r.timed_out = true;
            r.exit_code = -1;
        }
        return r;
    };
    const bool usable = primary.ran && !primary.timed_out && primary.exit_code >= 0;
    if (request.elevated || usable)
        return sticky(std::move(primary));
    ExecResult fallback = run_once(request, true);
    if (fallback.ran && !fallback.timed_out && fallback.exit_code >= 0) {
        fallback.error.clear();
        return fallback;
    }
    // Report the primary failure unless the fallback did better.
    if (!fallback.error.empty() && primary.error.empty())
        return sticky(std::move(fallback));
    return sticky(std::move(primary));
}

ExecResult Vmware::run_once(const ExecRequest& request, bool system) {
    ExecResult result;
    result.run_id =
        request.run_id.empty() ? "run-" + std::to_string(GetTickCount64()) : request.run_id;
    const std::string guest_dir = "C:\\blook\\runs\\" + result.run_id;
    result.guest_dir = guest_dir;
    const std::string out_file = guest_dir + "\\stdout.txt";
    const std::string err_file = guest_dir + "\\stderr.txt";
    const std::string rc_file = guest_dir + "\\exit.txt";
    const std::string job_guest = guest_dir + "\\job.cmd";

    const auto started = std::chrono::steady_clock::now();
    result.exec_timeout_s = static_cast<long long>(request.timeout.count());
    guest_mkdirs(request.vm, guest_dir);

    // The job runs from a batch file pushed into the guest: each line is
    // expanded separately, so `%ERRORLEVEL%` is captured *after* the command
    // ran. (A single `&`-joined line would expand before execution and leave
    // the exit-code file empty.)
    std::string job;
    job += "@echo off\r\n";
    // Delayed expansion: `!ERRORLEVEL!` is read when the line runs, which is
    // the only reliable way to capture the code of the preceding command in a
    // VIX-hosted batch (a plain `%ERRORLEVEL%` expands to nothing there).
    job += "setlocal EnableDelayedExpansion\r\n";
    for (const auto& assignment : request.env) {
        job += "set " + assignment + "\r\n";
    }
    if (request.workdir && !request.workdir->empty()) {
        job += "cd /d " + batch_quote(*request.workdir) + "\r\n";
    }
    job += "call " + request.command_line + " 1>" + batch_quote(out_file) + " 2>" +
           batch_quote(err_file) + "\r\n";
    // A freshly spawned cmd.exe has no ERRORLEVEL variable yet, so reading it
    // directly after the call writes an empty file. `set` first makes the value
    // concrete and per-line expansion captures it.
    job += "set BVMENV_RC=!ERRORLEVEL!\r\n";
    job += "echo !BVMENV_RC!>" + batch_quote(rc_file) + "\r\n";
    job += "exit /b 0\r\n";
    const fs::path local_job = cfg_.cache_dir() / "runs" / result.run_id / "job.cmd";
    write_text(local_job, job);
    copy_to_guest(request.vm, local_job, job_guest);

    if (!system) {
        // Direct call (not run_script) because a timeout is a *result* here, not
        // an error: e2e has to classify it.
        const ProcessResult r =
            vmrun({L"-gu", widen(cfg_.guest_user), L"-gp", widen(cfg_.guest_password),
                   L"runScriptInGuest", vmx_wide(request.vm), L"",
                   widen("call " + batch_quote(job_guest))},
                  std::chrono::duration_cast<std::chrono::milliseconds>(request.timeout));
        result.ran = r.started;
        result.timed_out = r.timed_out;
        if (!r.started)
            result.error = r.error;
        if (!r.timed_out && r.exit_code != 0) {
            result.error = tail_of(trim(r.out + r.err), 400);
        }
    } else {
        // SYSTEM scheduled task: full token even when UAC is enabled.
        const std::string task = "bvmenv_" + result.run_id;
        std::string helper;
        helper += "@echo off\r\n";
        helper += "schtasks /create /tn " + task + " /tr \"cmd.exe /c " + job_guest +
                  "\" /sc once /st 00:00 /ru SYSTEM /rl HIGHEST /f >nul\r\n";
        helper += "schtasks /run /tn " + task + " >nul\r\n";
        const fs::path local_helper = cfg_.cache_dir() / "runs" / result.run_id / "elevate.cmd";
        write_text(local_helper, helper);
        const std::string helper_guest = guest_dir + "\\elevate.cmd";
        copy_to_guest(request.vm, local_helper, helper_guest);
        run_script(request.vm, "call " + batch_quote(helper_guest), std::chrono::seconds(60));

        const auto deadline = started + request.timeout;
        bool done = false;
        const fs::path local_rc = cfg_.cache_dir() / "runs" / result.run_id / "exit.txt";
        while (std::chrono::steady_clock::now() < deadline) {
            std::error_code ec;
            fs::remove(local_rc, ec);
            const ProcessResult r =
                vmrun({L"-gu", widen(cfg_.guest_user), L"-gp", widen(cfg_.guest_password),
                       L"copyFileFromGuestToHost", vmx_wide(request.vm), widen(rc_file),
                       local_rc.wstring()},
                      std::chrono::seconds(30));
            if (r.exit_code == 0 && file_exists(local_rc)) {
                done = true;
                break;
            }
            sleep_ms(500);
        }
        result.ran = done;
        result.timed_out = !done;
        try {
            run_script(request.vm, "schtasks /delete /tn " + task + " /f >nul 2>&1",
                       std::chrono::seconds(30));
        } catch (...) {
        }
    }

    // Collect artifacts (also on timeout: partial output is valuable evidence).
    const fs::path local_out = cfg_.cache_dir() / "runs" / result.run_id / "stdout.txt";
    const fs::path local_err = cfg_.cache_dir() / "runs" / result.run_id / "stderr.txt";
    const fs::path local_rc = cfg_.cache_dir() / "runs" / result.run_id / "exit.txt";
    auto try_pull = [&](const std::string& guest, const fs::path& local) {
        std::error_code ec;
        fs::remove(local, ec);
        const ProcessResult r =
            vmrun({L"-gu", widen(cfg_.guest_user), L"-gp", widen(cfg_.guest_password),
                   L"copyFileFromGuestToHost", vmx_wide(request.vm), widen(guest), local.wstring()},
                  std::chrono::seconds(60));
        return r.exit_code == 0 && file_exists(local);
    };
    if (try_pull(out_file, local_out))
        result.out = read_text(local_out);
    if (try_pull(err_file, local_err))
        result.err = read_text(local_err);
    const bool have_rc = try_pull(rc_file, local_rc);
    if (have_rc) {
        try {
            result.exit_code = std::stoi(trim(read_text(local_rc)));
        } catch (...) {
            result.exit_code = -1;
            result.error = "guest wrote an unusable exit code: " + trim(read_text(local_rc));
        }
    } else if (!result.timed_out) {
        result.exit_code = -1;
        if (result.error.empty()) {
            result.error = "guest did not report an exit code (job.cmd did not finish)";
        }
    }

    result.duration = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started);
    std::error_code ec;
    fs::remove_all(cfg_.cache_dir() / "runs" / result.run_id, ec);
    return result;
}

fs::path Vmware::vm_dir_of(const VmInfo& vm) const {
    return fs::path(to_wide(vm.dir));
}

fs::path Vmware::log_path_of(const VmInfo& vm) const {
    return vm_dir_of(vm) / L"vmware.log";
}

fs::path Vmware::serial_path_of(const VmInfo& vm) const {
    return vm_dir_of(vm) / L"serial.log";
}

}  // namespace blook::bvmenv
