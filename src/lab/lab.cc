// blook-vmware-environment: high level lab operations.
#include "blook/bvmenv/lab.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <regex>
#include <string>
#include <thread>

#include "blook/bvmenv/classify.h"
#include "blook/bvmenv/process.h"
#include "blook/bvmenv/util.h"
#include "blook/bvmenv/vmx.h"

namespace blook::bvmenv {
namespace {

constexpr const char* kBaseSnapshot = "base";
constexpr const char* kReadySnapshot = "ready";

json::Value checks_json(const std::vector<std::pair<std::string, bool>>& checks) {
    json::Value out(json::Array{});
    for (const auto& [name, ok] : checks) {
        json::Value item(json::Object{});
        item["name"] = name;
        item["ok"] = ok;
        out.push(std::move(item));
    }
    return out;
}

std::string iso_drive_letter(const fs::path& iso) {
    ProcessOptions options;
    const std::wstring ps = L"powershell.exe";
    std::vector<std::wstring> argv{
        ps,
        L"-NoProfile",
        L"-NonInteractive",
        L"-Command",
        L"(Mount-DiskImage -ImagePath '" + iso.wstring() + L"' -PassThru | Get-Volume).DriveLetter",
    };
    options.command_line = build_command_line(argv);
    options.timeout = std::chrono::seconds(120);
    const ProcessResult result = run_process(options);
    const std::string letter = trim(result.out);
    if (letter.size() != 1) {
        throw Error("failed to mount Windows ISO " + to_utf8(iso.wstring()) + ": " +
                    trim(result.out + result.err));
    }
    return letter;
}

void dismount_iso(const fs::path& iso) {
    ProcessOptions options;
    std::vector<std::wstring> argv{
        L"powershell.exe", L"-NoProfile", L"-NonInteractive", L"-Command",
        L"Dismount-DiskImage -ImagePath '" + iso.wstring() + L"' | Out-Null"};
    options.command_line = build_command_line(argv);
    options.timeout = std::chrono::seconds(120);
    run_process(options);
}

void run_checked(const std::vector<std::wstring>& argv, const std::string& what,
                 std::chrono::milliseconds timeout = std::chrono::minutes(30),
                 const std::wstring& workdir = {}) {
    ProcessOptions options;
    options.command_line = build_command_line(argv);
    options.timeout = timeout;
    if (!workdir.empty())
        options.working_dir = workdir;
    const ProcessResult result = run_process(options);
    if (result.exit_code != 0) {
        throw Error(what + " failed (exit " + std::to_string(result.exit_code) +
                    "): " + tail_of(trim(result.out + result.err), 2000));
    }
}

// Looks for a built guest artifact (e.g. the VT-x probe driver) next to the
// executable, which is how the release package and the build tree ship it.
fs::path find_guest_artifact(const wchar_t* name) {
    wchar_t buffer[MAX_PATH * 2];
    const DWORD len = GetModuleFileNameW(nullptr, buffer, static_cast<DWORD>(std::size(buffer)));
    if (len == 0)
        return {};
    const fs::path base = fs::path(std::wstring(buffer, len)).parent_path();
    for (const fs::path& dir :
         {base, base / L"..", base / L".." / L"..", base / L".." / L".." / L".."}) {
        std::error_code ec;
        const fs::path candidate = (dir / name).lexically_normal();
        if (fs::exists(candidate, ec))
            return fs::weakly_canonical(candidate, ec);
    }
    return {};
}

std::string run_capture(const std::vector<std::wstring>& argv,
                        std::chrono::milliseconds timeout = std::chrono::minutes(20)) {
    ProcessOptions options;
    options.command_line = build_command_line(argv);
    options.timeout = timeout;
    const ProcessResult result = run_process(options);
    return result.out + result.err;
}

}  // namespace

// ---------------------------------------------------------------------------
// create / destroy / reset
// ---------------------------------------------------------------------------
VmInfo Lab::create(const CreateOptions& options) {
    if (options.name.empty())
        throw Error("vm name is required");
    if (options.name == "base")
        throw Error("'base' is reserved for the base image");
    if (options.from != "base") {
        throw Error("only --from base is supported (linked clones of the base snapshot)");
    }
    if (auto existing = vmware_.find(options.name)) {
        throw Error("VM '" + options.name + "' already exists at " + existing->vmx);
    }
    const VmInfo base = vmware_.base();
    if (!vmware_.has_snapshot("base", kBaseSnapshot)) {
        throw Error("base image has no snapshot named '" + std::string(kBaseSnapshot) +
                    "' (rebuild the base image)");
    }
    if (base.power != PowerState::Off) {
        vmware_.stop("base", true);
        vmware_.wait_power("base", PowerState::Off, std::chrono::seconds(120));
    }

    const fs::path dir = cfg_.vm_dir(options.name);
    const fs::path vmx_path = cfg_.vmx_for(options.name);
    ensure_dir(dir);

    vmware_.clone_linked(cfg_.base_vmx(), vmx_path, kBaseSnapshot);

    // Per-VM fixups: unique display name, unique serial log, fresh identity so
    // that two forks never share a UUID or a MAC address.
    Vmx vmx = Vmx::load(vmx_path);
    vmx.set("displayName", options.name);
    vmx.remove("uuid.bios");
    vmx.remove("uuid.location");
    vmx.set("ethernet0.addressType", "generated");
    vmx.remove("ethernet0.address");
    vmx.remove("ethernet0.generatedAddress");
    vmx.remove("ethernet0.generatedAddressOffset");
    if (options.kd_pipe) {
        // Named pipe KD: VMware is the pipe server, kd.exe the client.
        vmx.set("serial0.fileType", "pipe");
        vmx.set("serial0.fileName", "\\\\.\\pipe\\blook-" + options.name + "-kd");
        vmx.set("serial0.pipe.endPoint", "server");
        vmx.remove("serial0.tryNoRxLoss");
        vmx.remove("serial0.yieldOnMsrRead");
    } else {
        vmx.set("serial0.fileType", "file");
        vmx.set("serial0.fileName", "serial.log");
    }
    vmx.remove("nvram");
    vmx.set("extendedConfigFile", options.name + ".vmxf");
    vmx.set("vmxstats.filename", options.name + ".scoreboard");
    if (options.cpus > 0)
        vmx.set("numvcpus", std::to_string(options.cpus));
    if (options.memory_mb > 0)
        vmx.set("memsize", std::to_string(options.memory_mb));
    vmx.save(vmx_path);

    json::Value meta(json::Object{});
    meta["name"] = options.name;
    meta["base"] = "base";
    meta["snapshot"] = kBaseSnapshot;
    meta["created_at"] = now_iso8601();
    meta["cpus"] = options.cpus > 0 ? options.cpus : base.cpus;
    meta["memory_mb"] = options.memory_mb > 0 ? options.memory_mb : base.memory_mb;
    write_text(dir / ".bvmenv.json", meta.dump(2));

    VmInfo info = *vmware_.find(options.name);
    if (options.start) {
        vmware_.start(options.name, false);
        if (options.wait_ready) {
            if (!wait_ready(options.name, std::chrono::seconds(options.ready_timeout_s))) {
                throw Error("VM '" + options.name +
                            "' started but never became ready (see state/screenshot)");
            }
            // Snapshot the healthy, booted state so `reset` is a fast revert.
            vmware_.snapshot(options.name, kReadySnapshot);
        }
    }
    return info;
}

void Lab::destroy(const std::string& name) {
    if (name == "base")
        throw Error("refusing to delete the base image");
    if (!vmware_.find(name))
        throw Error("no such VM: " + name);
    vmware_.delete_vm(name);
}

ResetReport Lab::reset(const std::string& name, const std::string& reason) {
    ResetReport report;
    report.performed = true;
    report.reason = reason;
    const auto started = std::chrono::steady_clock::now();
    try {
        const auto info = vmware_.find(name);
        if (!info)
            throw Error("no such VM: " + name);
        if (info->power != PowerState::Off) {
            vmware_.stop(name, true);
            vmware_.wait_power(name, PowerState::Off, std::chrono::seconds(60));
        }
        if (vmware_.has_snapshot(name, kReadySnapshot)) {
            vmware_.revert(name, kReadySnapshot);
            report.strategy = "snapshot";
        } else if (vmware_.has_snapshot(name, kBaseSnapshot)) {
            vmware_.revert(name, kBaseSnapshot);
            report.strategy = "snapshot";
        } else {
            // Fallback: rebuild the fork from the base snapshot.
            const std::string from = name;
            const int cpus = info->cpus;
            const int memory = info->memory_mb;
            vmware_.delete_vm(from);
            CreateOptions options;
            options.name = from;
            options.cpus = cpus;
            options.memory_mb = memory;
            options.wait_ready = true;
            create(options);
            report.strategy = "delete-reclone";
        }
        report.ok = true;
    } catch (const std::exception& e) {
        report.ok = false;
        report.strategy = "failed: " + std::string(e.what());
    }
    report.duration = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started);
    return report;
}

bool Lab::wait_ready(const std::string& name, std::chrono::seconds timeout) {
    // Fail fast on a bad VM name instead of polling for the whole timeout.
    const auto info = vmware_.find(name);
    if (!info)
        throw Error("no such VM: " + name + " (create it with `bvmenv create " + name + "`)");

    const auto deadline = std::chrono::steady_clock::now() + timeout;
    bool started = false;
    for (;;) {
        const PowerState power = vmware_.power_state(name);
        if (power == PowerState::Off) {
            // wait_ready never powers a VM on: that is `start`/`create`'s job.
            if (std::chrono::steady_clock::now() >= deadline) {
                throw Error("VM " + name + " is powered off; start it first (`bvmenv start " +
                            name + "`)");
            }
            sleep_ms(1000);
            continue;
        }
        started = true;
        // Non-blocking tools query first; only probe guest ops when Tools says
        // it is running, and always with a bounded timeout.
        const ToolInfo tools = vmware_.tools_info(name);
        if (tools.state == ToolsState::Running) {
            // Probe through run(): it transparently falls back to a SYSTEM
            // scheduled task at the logon/lock UI, so readiness never depends
            // on auto-logon having completed.
            ExecRequest probe;
            probe.vm = name;
            probe.command_line = "cmd /c exit 0";
            probe.timeout = std::chrono::seconds(20);
            probe.run_id = "ready-" + std::to_string(GetTickCount64());
            const ExecResult result = vmware_.run(probe);
            if (result.ran && !result.timed_out && result.exit_code == 0)
                return true;
        }
        if (std::chrono::steady_clock::now() >= deadline)
            return false;
        sleep_ms(1000);
    }
    return started;
}

// ---------------------------------------------------------------------------
// state
// ---------------------------------------------------------------------------
StateReport Lab::sample_once(const std::string& name, const StateOptions& options) {
    StateReport report;
    const auto info = vmware_.find(name);
    if (!info)
        throw Error("no such VM: " + name);
    report.vm = name;
    report.vmx = info->vmx;
    report.power = info->power;
    report.tools = vmware_.tools_info(name);
    report.sampled_at = now_iso8601();

    // Guest side extras are best effort: they must never gate the state result.
    report.guest_queryable = vmware_.guest_reachable(name);
    if (report.guest_queryable) {
        report.ip = vmware_.guest_ip(name);
        if (options.samples <= 1) {
            bool ok = false;
            const std::string out = vmware_.guest_capture(
                name,
                "powershell -NoProfile -Command \"[int]((Get-Date)-(Get-CimInstance "
                "Win32_OperatingSystem).LastBootUpTime).TotalSeconds\"",
                &ok, std::chrono::seconds(10));
            if (ok) {
                try {
                    report.uptime_seconds = std::stoll(trim(out));
                } catch (...) {
                }
            }
        }
    }

    if (options.capture && report.power == PowerState::On) {
        const fs::path png = cfg_.cache_dir() / "shots" / (name + "-" + now_stamp() + ".png");
        if (vmware_.capture_screenshot(name, png)) {
            std::string error;
            if (auto stats = analyze_image(png, &error)) {
                std::string text;
                std::string engine;
                if (options.use_ocr && !cfg_.ocr_command.empty()) {
                    text = ocr_image(png, cfg_.ocr_command, &engine);
                } else if (options.use_ocr) {
                    text = ocr_image(png, {}, &engine);
                }
                ScreenContext screen_context;
                screen_context.tools_running = report.tools.state == ToolsState::Running;
                screen_context.guest_queryable = report.guest_queryable;
                report.screen = classify_screen(*stats, text, screen_context);
                report.screen.screenshot = to_utf8(png.wstring());
                // Do NOT turn an unclear frame into "desktop" just because guest operations
                // answer: they also answer (through the SYSTEM fallback) on the
                // logon/lock UI, which would mask a stuck auto-logon.
                if (report.tools.state == ToolsState::Running &&
                    report.screen.classification == ScreenClass::Unknown) {
                    report.screen.confidence = 0.4;
                    report.screen.reason += " (guest operations answered, frame unclear)";
                }
                if (engine.empty() && options.use_ocr) {
                    report.screen.reason += " (no OCR engine available)";
                }
            } else {
                report.screen.classification = ScreenClass::Unknown;
                report.screen.reason = error;
            }
        } else {
            report.screen.classification = ScreenClass::Unknown;
            report.screen.reason = "screenshot capture failed";
        }
    } else if (report.power == PowerState::Off) {
        report.screen.classification = ScreenClass::Unknown;
        report.screen.reason = "VM powered off";
    }

    report.log_issues = parse_vmware_log(vmware_.log_path_of(*info));
    for (const auto& issue : parse_serial_log(vmware_.serial_path_of(*info))) {
        report.log_issues.push_back(issue);
    }
    // When the guest printed a bugcheck to the serial port, trust it.
    for (const auto& issue : report.log_issues) {
        if (contains_ci(issue.message, "STOP") || contains_ci(issue.message, "BugCheck")) {
            const std::string code = extract_stop_code(issue.message);
            if (!code.empty() && report.screen.stop_code.empty())
                report.screen.stop_code = code;
        }
    }
    return report;
}

StateReport Lab::state(const std::string& name, const StateOptions& options) {
    const int samples = std::max(1, options.samples);
    const int interval =
        options.interval_ms > 0 ? options.interval_ms : cfg_.hang_sample_interval_ms;

    StateReport report = sample_once(name, options);
    report.samples = 1;

    if (samples > 1 && report.power == PowerState::On) {
        std::uint64_t fingerprint = 0;
        int unchanged = 0;
        int seen = 0;
        for (int i = 1; i < samples; ++i) {
            sleep_ms(interval);
            StateReport sample = sample_once(name, options);
            ++seen;
            std::uint64_t sample_fp = 0;
            if (!sample.screen.screenshot.empty()) {
                std::string error;
                if (auto stats =
                        analyze_image(fs::path(to_wide(sample.screen.screenshot)), &error)) {
                    sample_fp = stats->fingerprint;
                }
            }
            if (i == 1) {
                fingerprint = sample_fp;
            } else if (sample_fp != 0 && sample_fp == fingerprint) {
                ++unchanged;
            } else {
                unchanged = 0;
                fingerprint = sample_fp;
            }
            report = sample;
            report.samples = i + 1;
        }
        report.unchanged_samples = unchanged;
        // Hang = the framebuffer never changes AND the guest stopped answering.
        const bool guest_dead =
            report.tools.state != ToolsState::Running || !vmware_.guest_reachable(name);
        if (unchanged + 1 >= cfg_.hang_samples && guest_dead &&
            report.screen.classification != ScreenClass::Bsod) {
            report.hang_suspected = true;
            report.screen.classification = ScreenClass::Hang;
            report.screen.confidence = 0.8;
            report.screen.reason = "framebuffer unchanged over " + std::to_string(unchanged + 1) +
                                   " samples and the guest stopped responding";
        }
    }
    return report;
}

// ---------------------------------------------------------------------------
// exec
// ---------------------------------------------------------------------------
ExecResult Lab::exec(const ExecRequest& request) {
    ExecResult result = vmware_.run(request);
    return result;
}

// ---------------------------------------------------------------------------
// e2e
// ---------------------------------------------------------------------------
namespace {

void copy_or_empty(const fs::path& from, const fs::path& to) {
    if (!file_exists(from)) {
        write_text(to, "");
        return;
    }
    write_text(to, read_text(from));
}

std::string join_args(const std::vector<std::string>& args) {
    std::string out;
    for (const auto& arg : args) {
        if (!out.empty())
            out += ' ';
        const bool needs_quotes = arg.find_first_of(" \t") != std::string::npos;
        out += needs_quotes ? shell_quote(arg) : arg;
    }
    return out;
}

}  // namespace

E2eResult Lab::e2e(const E2eRequest& request) {
    E2eResult result;
    result.run_id = now_stamp();
    result.vm = request.vm;
    result.bundle = request.bundle;
    result.entry = request.entry;
    result.args = request.args;
    result.started_at = now_iso8601();
    const auto started = std::chrono::steady_clock::now();

    const fs::path out_root = request.out_dir.empty() ? (cfg_.evidence_dir() / "e2e")
                                                      : fs::path(to_wide(request.out_dir));
    const fs::path artifact_dir = out_root / result.run_id;
    ensure_dir(artifact_dir);
    result.artifact_dir = to_utf8(artifact_dir.wstring());

    try {
        if (!vmware_.find(request.vm))
            throw Error("no such VM: " + request.vm);
        // Make sure the VM is up and responsive.
        if (vmware_.power_state(request.vm) != PowerState::On) {
            vmware_.start(request.vm, false);
        }
        if (!wait_ready(request.vm, std::chrono::seconds(request.wait_ready_timeout_s))) {
            throw Error("VM '" + request.vm + "' did not become ready within " +
                        std::to_string(request.wait_ready_timeout_s) + "s");
        }

        StateOptions state_options;
        state_options.capture = true;
        state_options.samples = 1;
        result.state_before = state(request.vm, state_options);

        const std::string guest_run_dir = "C:\\blook\\e2e\\" + result.run_id;
        const std::string guest_bundle = guest_run_dir + "\\bundle";
        int pushed = 0;
        if (!request.bundle.empty()) {
            pushed = vmware_.push_tree(request.vm, fs::path(to_wide(request.bundle)), guest_bundle);
        }

        std::string entry = request.entry;
        if (entry.find('\\') == std::string::npos && entry.find('/') == std::string::npos) {
            entry = guest_bundle + "\\" + entry;
        }

        ExecRequest exec_request;
        exec_request.vm = request.vm;
        exec_request.command_line = shell_quote(entry) + " " + join_args(request.args);
        exec_request.timeout = request.timeout;
        exec_request.run_id = result.run_id;
        result.exec = vmware_.run(exec_request);
        result.exit_code = result.exec.exit_code;
        result.timed_out = result.exec.timed_out;

        // Give the guest a moment, then classify what is on screen.
        sleep_ms(1500);
        StateOptions after_options;
        after_options.capture = true;
        after_options.samples = 3;
        result.state_after = state(request.vm, after_options);

        if (result.timed_out) {
            result.outcome = E2eOutcome::Timeout;
        } else if (result.state_after.screen.classification == ScreenClass::Bsod ||
                   !result.state_after.screen.stop_code.empty()) {
            result.outcome = E2eOutcome::Bsod;
        } else if (result.state_after.hang_suspected ||
                   result.state_after.screen.classification == ScreenClass::Hang) {
            result.outcome = E2eOutcome::Hang;
        } else if (result.exec.exit_code == 0) {
            result.outcome = E2eOutcome::Pass;
        } else {
            result.outcome = E2eOutcome::Fail;
        }

        // Artifacts.
        copy_or_empty(artifact_dir / "stdout.tmp", artifact_dir / "stdout.txt");
        write_text(artifact_dir / "stdout.txt", result.exec.out);
        write_text(artifact_dir / "stderr.txt", result.exec.err);
        write_text(artifact_dir / "state.json", result.state_after.to_json().dump(2));
        write_text(artifact_dir / "state-before.json", result.state_before.to_json().dump(2));
        if (!result.state_after.screen.screenshot.empty()) {
            const fs::path shot = artifact_dir / "screen.png";
            std::error_code ec;
            fs::copy_file(fs::path(to_wide(result.state_after.screen.screenshot)), shot,
                          fs::copy_options::overwrite_existing, ec);
        }
        // vmware.log tail (bounded) for root cause analysis.
        json::Value log_json(json::Object{});
        json::Value issues(json::Array{});
        for (const auto& issue : result.state_after.log_issues) {
            json::Value item(json::Object{});
            item["level"] = issue.level;
            item["source"] = issue.source;
            item["message"] = issue.message;
            issues.push(std::move(item));
        }
        log_json["issues"] = std::move(issues);
        if (const auto info = vmware_.find(request.vm)) {
            const fs::path log_path = vmware_.log_path_of(*info);
            if (file_exists(log_path)) {
                log_json["tail"] = tail_of(read_text(log_path), 16384);
            }
        }
        write_text(artifact_dir / "vmware.log.tail.json", log_json.dump(2));
        write_text(artifact_dir / "exec.json", result.exec.to_json().dump(2));

        // Reset policy.
        if (request.reset_on == "always" ||
            (request.reset_on == "failure" && result.outcome != E2eOutcome::Pass)) {
            result.reset =
                reset(request.vm, std::string("e2e:") + std::string(to_string(result.outcome)));
        }
        write_text(artifact_dir / "result.json", result.to_json().dump(2));
        (void)pushed;
    } catch (const std::exception& e) {
        result.outcome = E2eOutcome::Error;
        result.error = e.what();
        write_text(artifact_dir / "result.json", result.to_json().dump(2));
    }

    result.finished_at = now_iso8601();
    result.duration = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started);
    return result;
}

// ---------------------------------------------------------------------------
// bugcheck primitive
// ---------------------------------------------------------------------------
bool Lab::force_bugcheck(const std::string& name) {
    // Ctrl+ScrollLock twice triggers a 0xE2 (MANUALLY_INITIATED_CRASH) when the
    // guest was provisioned with CrashOnCtrlScroll.
    constexpr int kHidScrollLock = 0x47;
    constexpr int kModCtrl = 0x01;
    if (!vmware_.send_key(name, kHidScrollLock, kModCtrl))
        return false;
    sleep_ms(120);
    if (!vmware_.send_key(name, kHidScrollLock, kModCtrl))
        return false;
    return true;
}

// ---------------------------------------------------------------------------
// base verify
// ---------------------------------------------------------------------------
json::Value BaseVerifyReport::to_json() const {
    json::Value out(json::Object{});
    out["schema"] = std::string("bvmenv.base-verify.v1");
    out["ok"] = ok;
    out["checks"] = checks_json(checks);
    if (!provision_json.empty()) {
        if (auto parsed = json::Value::parse(provision_json))
            out["provision"] = *parsed;
    }
    json::Value notes_json(json::Array{});
    for (const auto& item_text : notes)
        notes_json.push(json::Value(item_text));
    out["notes"] = std::move(notes_json);
    return out;
}

BaseVerifyReport Lab::base_verify(const std::string& name, bool deep) {
    BaseVerifyReport report;
    const VmInfo base = vmware_.base();
    if (!vmware_.has_snapshot("base", kBaseSnapshot)) {
        report.checks.emplace_back("snapshot \"base\" exists on the base image", false);
        report.notes.push_back("run `bvmenv base build` first");
        return report;
    }
    report.checks.emplace_back("snapshot \"base\" exists on the base image", true);
    report.checks.emplace_back("base image is powered off",
                               vmware_.power_state("base") == PowerState::Off);
    report.checks.emplace_back(
        "base disk present",
        blook::bvmenv::file_size(cfg_.base_dir() / L"blook-base.vmdk") > 1024ull * 1024 * 1024);
    report.checks.emplace_back("base metadata present",
                               file_exists(cfg_.base_dir() / L"blook-base.vmx"));

    if (!deep) {
        // Fast, host-side only: never boots anything. The live contract checks
        // (test signing, VT-x probe, ...) need a booted guest, so they are
        // opt-in through --deep.
        report.notes.push_back(
            "host-side checks only; run `bvmenv base verify --deep` for the live guest contract");
        report.ok = std::all_of(report.checks.begin(), report.checks.end(),
                                [](const auto& c) { return c.second; });
        return report;
    }

    // Verify on a throwaway fork so the base image itself is never dirtied.
    const std::string probe = name == "base" ? "verify-" + now_stamp() : name;
    const bool temporary = (probe != name);
    try {
        if (temporary) {
            CreateOptions options;
            options.name = probe;
            options.wait_ready = true;
            options.ready_timeout_s = 900;  // fresh clone first boot: device setup + logon
            create(options);
        } else if (vmware_.power_state(probe) != PowerState::On) {
            vmware_.start(probe, false);
            wait_ready(probe, std::chrono::seconds(900));
        }

        const fs::path check_script = cfg_.guest_assets / "blook" / "verify.ps1";
        if (!file_exists(check_script)) {
            report.checks.emplace_back("guest verify.ps1 available", false);
        } else {
            vmware_.guest_mkdirs(probe, "C:\\blook");
            vmware_.copy_to_guest(probe, check_script, "C:\\blook\\verify.ps1");
            // The CPUID check needs the user mode probe inside the guest.
            if (const fs::path nested = find_guest_artifact(L"guest-nested-virt-check.exe");
                !nested.empty()) {
                vmware_.copy_to_guest(probe, nested, "C:\\blook\\nested-virt-check.exe");
            }
            bool ok = false;
            const std::string raw = vmware_.guest_capture(
                probe, "powershell -NoProfile -ExecutionPolicy Bypass -File C:\\blook\\verify.ps1",
                &ok, std::chrono::seconds(300));
            report.provision_json = raw;
            std::string parse_error;
            auto parsed = json::Value::parse(trim(raw), &parse_error);
            if (!parsed) {
                report.checks.emplace_back("guest verification script produced JSON", false);
                report.notes.push_back("raw output: " + tail_of(trim(raw), 800));
            } else {
                if ((*parsed)["failures"].is_array()) {
                    const auto& failures = (*parsed)["failures"].items();
                    report.checks.emplace_back("provisioning reported no failures",
                                               failures.empty());
                    for (const auto& failure : failures) {
                        report.notes.push_back("provision failure: " + failure.as_string());
                    }
                }
                const json::Value& checks = (*parsed)["checks"];
                if (checks.is_array()) {
                    for (const auto& item : checks.items()) {
                        const std::string check_name = item["name"].as_string();
                        const bool check_ok = item["ok"].as_bool();
                        report.checks.emplace_back(check_name, check_ok);
                    }
                }
            }
        }
    } catch (const std::exception& e) {
        report.checks.emplace_back("verification run completed", false);
        report.notes.push_back(e.what());
    }

    if (temporary) {
        try {
            vmware_.delete_vm(probe);
        } catch (const std::exception& e) {
            report.notes.push_back(std::string("cleanup of ") + probe + " failed: " + e.what());
        }
    }

    report.ok = !report.checks.empty() && std::all_of(report.checks.begin(), report.checks.end(),
                                                      [](const auto& c) { return c.second; });
    (void)base;
    return report;
}

// ---------------------------------------------------------------------------
// base build
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Guest driver signing
//
// The in-guest probes/drivers are test signed with a lab certificate so they
// load with `testsigning on` (byte-for-byte equal to what a downstream project
// needs for its own test drivers). The certificate is created once on the host
// and its public part is staged into the guest trust stores by provisioning.
// ---------------------------------------------------------------------------
namespace {

std::string run_and_capture(const std::vector<std::wstring>& argv,
                            std::chrono::milliseconds timeout = std::chrono::minutes(2)) {
    ProcessOptions options;
    options.command_line = build_command_line(argv);
    options.timeout = timeout;
    const ProcessResult result = run_process(options);
    if (result.exit_code != 0) {
        throw Error("command failed: " + to_utf8(argv.front()) + ": " +
                    tail_of(trim(result.out + result.err), 400));
    }
    return result.out;
}

// Returns the signing certificate thumbprint, creating the certificate on first
// use. The public .cer is written next to it for the guest trust stores.
std::string ensure_signing_cert(const Config& cfg) {
    const fs::path dir = cfg.vm_root / "_certs";
    ensure_dir(dir);
    const fs::path cer = dir / "blook-lab-test.cer";
    const fs::path cache = dir / "thumbprint.txt";
    if (file_exists(cache) && file_exists(cer)) {
        const std::string thumb = trim(read_text(cache));
        if (!thumb.empty())
            return thumb;
    }
    const std::string script =
        "$c = Get-ChildItem Cert:\\CurrentUser\\My | Where-Object { $_.Subject -eq "
        "'CN=blook-lab-test' } | Select-Object -First 1; "
        "if (-not $c) { $c = New-SelfSignedCertificate -Type CodeSigningCert -Subject "
        "'CN=blook-lab-test' -CertStoreLocation Cert:\\CurrentUser\\My -KeyExportPolicy "
        "Exportable -NotAfter (Get-Date).AddYears(5) }; "
        "Export-Certificate -Cert $c -FilePath '" +
        to_utf8(cer.wstring()) + "' -Force | Out-Null; $c.Thumbprint";
    const std::string thumb = trim(run_and_capture(
        {L"powershell.exe", L"-NoProfile", L"-NonInteractive", L"-Command", to_wide(script)}));
    if (thumb.size() < 32)
        throw Error("could not create/read the lab signing certificate");
    write_text(cache, thumb + "\n");
    return thumb;
}

fs::path find_signtool() {
    const wchar_t* roots[] = {L"Y:\\Program Files (x86)\\Windows Kits\\10\\bin",
                              L"C:\\Program Files (x86)\\Windows Kits\\10\\bin"};
    for (const wchar_t* root : roots) {
        std::error_code ec;
        if (!fs::is_directory(root, ec))
            continue;
        for (const auto& entry : fs::directory_iterator(root, ec)) {
            if (!entry.is_directory())
                continue;
            const fs::path candidate = entry.path() / L"x64" / L"signtool.exe";
            if (file_exists(candidate))
                return candidate;
        }
    }
    return {};
}

}  // namespace

void Lab::base_build(const BaseBuildOptions& options) {
    // A failed build must never leave a half-built `base`: `create` would fork
    // from a broken image and every downstream e2e would fail mysteriously.
    try {
        base_build_impl(options);
    } catch (...) {
        const fs::path dir = cfg_.base_dir();
        try {
            vmware_.stop("base", true);
        } catch (...) {
        }
        try {
            vmware_.delete_vm("base");
        } catch (...) {
        }
        remove_tree(dir);
        ensure_dir(dir);
        throw;
    }
}

void Lab::base_build_impl(const BaseBuildOptions& options) {
    if (!is_process_elevated()) {
        throw Error(
            "`base build` needs an elevated process (diskpart/dism/bcdboot); run it from "
            "an elevated shell or via `gsudo -d bvmenv base build`");
    }
    const fs::path iso = options.iso.empty() ? cfg_.windows_iso : options.iso;
    if (!file_exists(iso))
        throw Error("Windows ISO not found: " + to_utf8(iso.wstring()));
    if (!file_exists(cfg_.guest_assets / "unattend.xml")) {
        throw Error("guest assets missing: " + to_utf8(cfg_.guest_assets.wstring()) +
                    " (expected unattend.xml + blook\\*)");
    }
    const fs::path dir = cfg_.base_dir();
    ensure_dir(dir);
    const fs::path vhd = dir / "blook-base.vhd";
    const fs::path vmdk = dir / "blook-base.vmdk";
    if (options.force) {
        // A partial previous attempt may leave a VHD/VMDK behind without a vmx,
        // and diskpart refuses to create a VHD that already exists: wipe the
        // whole base directory whenever a rebuild is requested.
        try {
            vmware_.stop("base", true);
        } catch (...) {
        }
        remove_tree(dir);
        ensure_dir(dir);
    } else if (file_exists(cfg_.base_vmx())) {
        throw Error("base image already exists at " + to_utf8(cfg_.base_vmx().wstring()) +
                    " (use --force to rebuild)");
    }

    // 1. VHD with one active NTFS partition.
    //
    // Safety: the lab only ever creates/detaches blook-base.vhd (inside
    // BVMENV_VM_ROOT) by explicit path. If V: is occupied, or if one of our
    // images is still attached, the operator is told to fix it - the tool never
    // enumerates host disks or detaches anything it does not own.
    if (file_exists(L"V:\\")) {
        throw Error(
            "drive V: is already in use; refusing to touch an unknown volume. Dismount it, or "
            "detach our own image with an explicit path: diskpart \"select vdisk "
            "file=\\\"<BVMENV_VM_ROOT>\\\\base\\\\blook-base.vhd\\\"\" + \"detach vdisk noerr\"");
    }
    if (is_owned_vhd_path(cfg_.vm_root, vhd)) {
        // Best effort, allowlisted, explicit path only: a previous crashed build
        // may have left our own VHD attached.
        if (file_exists(vhd)) {
            try {
                detach_own_vhd(cfg_, vhd);
            } catch (const std::exception& e) {
                throw Error(std::string("could not detach the previous base VHD: ") + e.what() +
                            " (detach it manually by explicit path, then re-run)");
            }
        }
    }
    const fs::path dp_create = cfg_.cache_dir() / "diskpart-create.txt";
    std::string dp_script;
    dp_script += "create vdisk file=\"" + to_utf8(vhd.wstring()) +
                 "\" maximum=" + std::to_string(options.disk_gb * 1024) + " type=expandable\r\n";
    dp_script += "select vdisk file=\"" + to_utf8(vhd.wstring()) + "\"\r\n";
    dp_script += "attach vdisk\r\n";
    dp_script += "create partition primary\r\n";
    dp_script += "active\r\n";
    dp_script += "format fs=ntfs quick label=Windows\r\n";
    dp_script += "assign letter=V\r\n";
    write_text(dp_create, dp_script);
    run_checked({L"diskpart.exe", L"/s", dp_create.wstring()}, "diskpart create",
                std::chrono::minutes(10));
    if (!file_exists(L"V:\\"))
        throw Error("VHD was not mounted as V:");

    // 2. Apply the image (index 4 = Windows 11 Pro on the consumer media).
    const std::string letter = iso_drive_letter(iso);
    const std::string wim = letter + ":\\sources\\install.wim";
    run_checked({L"dism.exe", L"/Apply-Image", L"/ImageFile=" + to_wide(wim), L"/Index:4",
                 L"/ApplyDir=V:\\"},
                "dism apply", std::chrono::minutes(45));
    dismount_iso(iso);

    // 3. Boot files for BIOS/MBR (Secure Boot stays off: test signing needs it).
    run_checked({L"bcdboot.exe", L"V:\\Windows", L"/s", L"V:", L"/f", L"BIOS"}, "bcdboot",
                std::chrono::minutes(5));

    // 4. Stage the guest payload + answer file.
    const fs::path guest = cfg_.guest_assets;
    ensure_dir(L"V:\\blook");
    ensure_dir(L"V:\\Windows\\Panther\\Unattend");
    ensure_dir(L"V:\\Windows\\Setup\\Scripts");
    std::error_code ec;
    for (const auto& entry : fs::recursive_directory_iterator(guest / "blook", ec)) {
        if (!entry.is_regular_file())
            continue;
        const fs::path rel = fs::relative(entry.path(), guest / "blook");
        const fs::path target = fs::path(L"V:\\blook") / rel;
        ensure_dir(target.parent_path());
        fs::copy_file(entry.path(), target, fs::copy_options::overwrite_existing, ec);
    }
    fs::copy_file(guest / "unattend.xml", L"V:\\Windows\\Panther\\Unattend\\Unattend.xml",
                  fs::copy_options::overwrite_existing, ec);
    fs::copy_file(guest / "unattend.xml", L"V:\\blook\\unattend.xml",
                  fs::copy_options::overwrite_existing, ec);
    // The VT-x probe driver is a build artifact, not part of the guest payload.
    if (const fs::path vtx = find_guest_artifact(L"guest-vtx-probe.sys"); !vtx.empty()) {
        fs::copy_file(vtx, L"V:\\blook\\vtx-probe.sys", fs::copy_options::overwrite_existing, ec);
    } else {
        throw Error(
            "guest-vtx-probe.sys not found next to bvmenv.exe; build it with "
            "`xmake f --guest_drivers=y && xmake guest-vtx-probe`");
    }
    write_text(L"V:\\Windows\\Setup\\Scripts\\SetupComplete.cmd",
               "@echo off\r\n"
               "rem blook-vmware-environment: insurance kick for guest provisioning.\r\n"
               "if exist C:\\blook\\stage.cmd call C:\\blook\\stage.cmd\r\n");

    // 4b. Test-sign the guest drivers and stage the public certificate. The
    // VT-x probe and the crash/hang drivers must load with testsigning on, so
    // they are signed with a lab code-signing certificate that provisioning
    // adds to the guest's Root + TrustedPublisher stores.
    {
        const fs::path signtool = find_signtool();
        const std::string thumb = ensure_signing_cert(cfg_);
        int signed_files = 0;
        for (const auto& entry : fs::directory_iterator(L"V:\\blook")) {
            if (!entry.is_regular_file())
                continue;
            if (to_lower(to_utf8(entry.path().extension().wstring())) != ".sys")
                continue;
            if (signtool.empty()) {
                throw Error(
                    "signtool.exe not found under Windows Kits\\10\\bin; install the "
                    "WDK/SDK signing tools to build the base image");
            }
            run_and_capture({signtool.wstring(), L"sign", L"/sha1", to_wide(thumb), L"/s", L"My",
                             L"/fd", L"SHA256", entry.path().wstring()},
                            std::chrono::minutes(2));
            ++signed_files;
        }
        if (signed_files == 0) {
            throw Error(
                "no guest .sys files were staged into V:\\blook; build them with "
                "`xmake -y guest-vtx-probe` (etc.) first");
        }
        const fs::path cer = cfg_.vm_root / "_certs" / "blook-lab-test.cer";
        fs::copy_file(cer, L"V:\\blook\\blook-lab-test.cer", fs::copy_options::overwrite_existing,
                      ec);
    }

    // 5. Offline registry: answer file hint + OOBE/autologon pre-seeding.
    //
    // OOBE is the most fragile part of an *imaged* (rather than installed)
    // Windows: if the "Getting ready" stage wedges, the guest never gets a
    // session and guest operations are unusable. The answer file drives
    // specialize/oobeSystem, but these keys are belt-and-braces so a machine
    // that skips OOBE still lands on a desktop with a live session, which is
    // what every downstream test needs.
    run_checked(
        {L"reg.exe", L"load", L"HKLM\\BVM_SYSTEM", L"V:\\Windows\\System32\\config\\SYSTEM"},
        "reg load SYSTEM", std::chrono::minutes(2));
    run_capture({L"reg.exe", L"add", L"HKLM\\BVM_SYSTEM\\Setup", L"/v", L"UnattendFile", L"/t",
                 L"REG_SZ", L"/d", L"C:\\Windows\\Panther\\Unattend\\Unattend.xml", L"/f"});
    run_checked({L"reg.exe", L"unload", L"HKLM\\BVM_SYSTEM"}, "reg unload SYSTEM",
                std::chrono::minutes(2));

    run_checked(
        {L"reg.exe", L"load", L"HKLM\\BVM_SOFTWARE", L"V:\\Windows\\System32\\config\\SOFTWARE"},
        "reg load SOFTWARE", std::chrono::minutes(2));
    const wchar_t* const oobe = L"HKLM\\BVM_SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\OOBE";
    run_capture({L"reg.exe", L"add", oobe, L"/v", L"SkipMachineOOBE", L"/t", L"REG_DWORD", L"/d",
                 L"1", L"/f"});
    run_capture({L"reg.exe", L"add", oobe, L"/v", L"SkipUserOOBE", L"/t", L"REG_DWORD", L"/d", L"1",
                 L"/f"});
    const wchar_t* const winlogon =
        L"HKLM\\BVM_SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Winlogon";
    run_capture({L"reg.exe", L"add", winlogon, L"/v", L"AutoAdminLogon", L"/t", L"REG_SZ", L"/d",
                 L"1", L"/f"});
    run_capture({L"reg.exe", L"add", winlogon, L"/v", L"DefaultUserName", L"/t", L"REG_SZ", L"/d",
                 L"blook", L"/f"});
    run_capture({L"reg.exe", L"add", winlogon, L"/v", L"DefaultPassword", L"/t", L"REG_SZ", L"/d",
                 L"blook", L"/f"});
    run_capture({L"reg.exe", L"add", winlogon, L"/v", L"DefaultDomainName", L"/t", L"REG_SZ", L"/d",
                 L"BLOOK-BASE", L"/f"});
    run_capture({L"reg.exe", L"add", winlogon, L"/v", L"ForceAutoLogon", L"/t", L"REG_SZ", L"/d",
                 L"1", L"/f"});
    run_checked({L"reg.exe", L"unload", L"HKLM\\BVM_SOFTWARE"}, "reg unload SOFTWARE",
                std::chrono::minutes(2));

    // 6. Detach + convert to VMDK (VMDK is required for snapshots/clones).
    const fs::path dp_detach = cfg_.cache_dir() / "diskpart-detach.txt";
    write_text(dp_detach,
               "select vdisk file=\"" + to_utf8(vhd.wstring()) + "\"\r\ndetach vdisk\r\n");
    run_checked({L"diskpart.exe", L"/s", dp_detach.wstring()}, "diskpart detach",
                std::chrono::minutes(5));
    sleep_ms(2000);
    run_checked({cfg_.vdiskmanager().wstring(), L"-r", vhd.wstring(), L"-t", L"0", vmdk.wstring()},
                "vhd->vmdk conversion", std::chrono::minutes(30));
    if (!file_exists(vmdk))
        throw Error("VMDK conversion produced no disk");

    // 7. VM definition (bios firmware: VMware's EFI firmware does not boot this
    //    install media from SATA, and Secure Boot must stay off anyway).
    Vmx vmx;
    vmx.set("config.version", "8");
    vmx.set("virtualHW.version", "21");
    vmx.set("displayName", "blook-base");
    vmx.set("guestOS", "windows11-64");
    vmx.set("memsize", std::to_string(cfg_.default_memory_mb));
    vmx.set("numvcpus", std::to_string(cfg_.default_cpus));
    vmx.set("cpuid.coresPerSocket", std::to_string(std::max(1, cfg_.default_cpus / 2)));
    vmx.set("firmware", "bios");
    vmx.set("vhv.enable", "TRUE");  // nested VT-x for downstream hypervisors
    vmx.set("vmci0.present", "TRUE");
    vmx.set("tools.upgrade.policy", "manual");
    vmx.set("powerType.powerOff", "soft");
    vmx.set("powerType.powerOn", "soft");
    vmx.set("powerType.reset", "soft");
    vmx.set("powerType.suspend", "soft");
    vmx.set("floppy0.present", "FALSE");
    vmx.set("sata0.present", "TRUE");
    vmx.set("sata0:0.present", "TRUE");
    vmx.set("sata0:0.fileName", "blook-base.vmdk");
    vmx.set("sata0:0.deviceType", "disk");
    vmx.set("sata0:1.present", "TRUE");
    vmx.set("sata0:1.deviceType", "cdrom-image");
    vmx.set("sata0:1.fileName",
            to_utf8((cfg_.vmware_dir / L"windows.iso").wstring()));  // VMware Tools media
    vmx.set("sata0:1.startConnected", "TRUE");
    vmx.set("ethernet0.present", "TRUE");
    vmx.set("ethernet0.connectionType", "nat");
    vmx.set("ethernet0.virtualDev", "e1000e");  // PCI, no PCIe slot needed
    vmx.set("ethernet0.addressType", "generated");
    vmx.set("serial0.present", "TRUE");
    vmx.set("serial0.fileType", "file");
    vmx.set("serial0.fileName", "serial.log");
    vmx.set("svga.present", "TRUE");
    vmx.set("svga.autodetect", "FALSE");
    vmx.set("svga.vramSize", "33554432");
    vmx.set("sound.present", "FALSE");
    vmx.set("usb.present", "FALSE");
    // PCIe root ports: the working hand-written layout. A vmxnet3 NIC needs a
    // free PCIe slot; without these bridges the VM fails to power on with
    //   [msg.pci.noslotavail] No PCIe slot available for Ethernet0
    vmx.set("pciBridge0.present", "TRUE");
    for (int bridge = 4; bridge <= 7; ++bridge) {
        const std::string prefix = "pciBridge" + std::to_string(bridge);
        vmx.set(prefix + ".present", "TRUE");
        vmx.set(prefix + ".virtualDev", "pcieRootPort");
        vmx.set(prefix + ".functions", "8");
    }
    vmx.set("isolation.tools.hgfs.disable", "TRUE");
    vmx.set("msg.autoAnswer", "TRUE");
    vmx.set("uuid.action", "create");
    vmx.set("extendedConfigFile", "blook-base.vmxf");
    vmx.set("vmxstats.filename", "blook-base.scoreboard");
    vmx.set("bios.bootOrder", "hdd");
    vmx.save(cfg_.base_vmx());

    // 8. Boot it: the guest provisions itself (VMware Tools + test baseline)
    //    and powers off when done.
    try {
        vmware_.start("base", false);
    } catch (const std::exception& e) {
        // Surface the real reason from the hypervisor log, not just "canceled".
        std::string reason;
        const fs::path log = cfg_.base_dir() / "vmware.log";
        if (file_exists(log)) {
            const std::string text = tail_of(read_text(log), 200 * 1024);
            for (const char* needle : {"msg.pci.noslotavail", "power on failed", "Msg_Post",
                                       "Cannot open", "not supported"}) {
                const std::size_t pos = text.rfind(needle);
                if (pos == std::string::npos)
                    continue;
                const std::size_t eol = text.find_first_of("\r\n", pos);
                reason = trim(
                    text.substr(pos, eol == std::string::npos ? std::string::npos : eol - pos));
                break;
            }
        }
        if (!reason.empty()) {
            throw Error(std::string(e.what()) + " - hypervisor log: " + reason);
        }
        throw;
    }
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(options.ready_timeout_s);
    bool finished = false;
    while (std::chrono::steady_clock::now() < deadline) {
        sleep_ms(10000);
        if (vmware_.power_state("base") == PowerState::Off) {
            finished = true;
            break;
        }
    }
    if (!finished) {
        throw Error("guest provisioning did not finish within " +
                    std::to_string(options.ready_timeout_s) +
                    "s (inspect C:\\blook\\provision.log inside the guest)");
    }
    // 9. Snapshot the pristine base.
    if (!vmware_.has_snapshot("base", kBaseSnapshot)) {
        vmware_.snapshot("base", kBaseSnapshot);
    }
}

// ---------------------------------------------------------------------------
// doctor
// ---------------------------------------------------------------------------
json::Value Lab::doctor() {
    json::Value out(json::Object{});
    out["schema"] = std::string("bvmenv.doctor.v1");
    out["vmware_dir"] = to_utf8(cfg_.vmware_dir.wstring());
    out["vm_root"] = to_utf8(cfg_.vm_root.wstring());
    out["elevated"] = is_process_elevated();

    std::vector<std::pair<std::string, bool>> checks;
    checks.emplace_back("vmrun.exe present", file_exists(cfg_.vmrun()));
    checks.emplace_back("vmcli.exe present", file_exists(cfg_.vmcli()));
    checks.emplace_back("vmware-vdiskmanager.exe present", file_exists(cfg_.vdiskmanager()));
    checks.emplace_back("VMAuthdService running", vmware_.service_running());
    checks.emplace_back("Windows ISO present", file_exists(cfg_.windows_iso));
    checks.emplace_back("guest assets present", file_exists(cfg_.guest_assets / "unattend.xml"));
    checks.emplace_back("base image present", file_exists(cfg_.base_vmx()));
    checks.emplace_back("base snapshot present", file_exists(cfg_.base_vmx()) &&
                                                     vmware_.has_snapshot("base", kBaseSnapshot));
    const std::wstring tesseract = find_executable(L"tesseract");
    checks.emplace_back("OCR engine available", !tesseract.empty() || !cfg_.ocr_command.empty());

    out["checks"] = checks_json(checks);

    // Host disk safety: read-only report of attached file-backed virtual disks.
    // bvmenv only detaches its own images by explicit path; a stale attachment
    // is still worth surfacing because it blocks `base build`.
    json::Value attached(json::Array{});
    for (const auto& entry : attached_file_backed_vhds())
        attached.push(json::Value(entry));
    out["attached_vhds"] = std::move(attached);
    checks.emplace_back("no file-backed virtual disks attached to the host",
                        out["attached_vhds"].items().empty());

    out["ok"] = std::all_of(checks.begin(), checks.end(), [](const auto& c) { return c.second; });
    std::string version_error;
    out["config_ok"] = cfg_.validate(&version_error);
    if (!version_error.empty())
        out["config_error"] = version_error;
    out["vmrun_version"] = vmware_.vmrun_version();

    json::Value vms(json::Array{});
    try {
        for (const auto& vm : vmware_.list()) {
            json::Value item(json::Object{});
            item["name"] = vm.name;
            item["power"] = std::string(to_string(vm.power));
            item["vmx"] = vm.vmx;
            item["base"] = vm.is_base;
            vms.push(std::move(item));
        }
    } catch (const std::exception& e) {
        out["vms_error"] = e.what();
    }
    out["vms"] = std::move(vms);
    return out;
}

}  // namespace blook::bvmenv
