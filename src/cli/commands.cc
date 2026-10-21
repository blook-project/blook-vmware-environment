// blook-vmware-environment: CLI command implementations.
#include <windows.h>

#include <algorithm>
#include <fstream>
#include <thread>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include "blook/bvmenv/classify.h"
#include "blook/bvmenv/process.h"
#include "blook/bvmenv/vmx.h"
#include "blook/bvmenv/util.h"
#include "cli.h"

namespace blook::bvmenv::cli {
namespace {

std::string json_or_text(const json::Value& value, bool json) {
    return json ? value.dump(2) : std::string{};
}

int require_name(const ParsedArgs& args, const char* what) {
    if (args.positional.empty()) {
        std::cerr << "bvmenv: " << what << " requires a VM name\n";
        return 2;
    }
    return 0;
}

long long parse_ll(const std::string& text, long long fallback) {
    try {
        return std::stoll(text);
    } catch (...) {
        return fallback;
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// printers
// ---------------------------------------------------------------------------
void print_state(const StateReport& report, bool json) {
    if (json) {
        std::printf("%s\n", report.to_json().dump(2).c_str());
        return;
    }
    std::printf("vm           : %s\n", report.vm.c_str());
    std::printf("power        : %s\n", std::string(to_string(report.power)).c_str());
    std::printf("tools        : %s %s\n", std::string(to_string(report.tools.state)).c_str(),
                report.tools.version.c_str());
    std::printf("screen       : %s (%.0f%%) %s\n",
                std::string(to_string(report.screen.classification)).c_str(),
                report.screen.confidence * 100.0, report.screen.reason.c_str());
    if (!report.screen.stop_code.empty()) {
        std::printf("stop code    : %s\n", report.screen.stop_code.c_str());
    }
    if (!report.ip.empty())
        std::printf("guest ip     : %s\n", report.ip.c_str());
    if (report.uptime_seconds) {
        std::printf("uptime       : %llds\n", *report.uptime_seconds);
    }
    if (report.hang_suspected) {
        std::printf("hang         : suspected (unchanged frames: %d/%d)\n",
                    report.unchanged_samples + 1, report.samples);
    }
    for (const auto& issue : report.log_issues) {
        std::printf("log %-8s : %s\n", issue.level.c_str(), issue.message.c_str());
    }
    if (!report.screen.screenshot.empty()) {
        std::printf("screenshot   : %s\n", report.screen.screenshot.c_str());
    }
}

void print_exec(const ExecResult& result, bool json) {
    if (json) {
        std::printf("%s\n", result.to_json().dump(2).c_str());
        return;
    }
    std::printf("exit_code    : %d%s\n", result.exit_code, result.timed_out ? " (timed out)" : "");
    if (!result.error.empty())
        std::printf("error        : %s\n", result.error.c_str());
    if (!result.out.empty()) {
        std::fwrite(result.out.data(), 1, result.out.size(), stdout);
        if (result.out.back() != '\n')
            std::fputc('\n', stdout);
    }
    if (!result.err.empty()) {
        std::fputs("--- stderr ---\n", stdout);
        std::fwrite(result.err.data(), 1, result.err.size(), stdout);
    }
}

// ---------------------------------------------------------------------------
// commands
// ---------------------------------------------------------------------------
namespace {

// The compiled version comes from BVMENV_VERSION at configure time, so a local
// build prints the default "0.1.0" even when the tree is newer - confusing while
// debugging. Prefer a version.txt shipped next to the binary or in the repo root
// (up to five levels), and fall back to the compiled value.
std::string resolve_version() {
    const std::string compiled = BVMENV_VERSION;
    wchar_t buffer[1024];
    const DWORD len = GetModuleFileNameW(nullptr, buffer, static_cast<DWORD>(std::size(buffer)));
    if (len == 0)
        return compiled;
    fs::path dir = fs::path(buffer).parent_path();
    std::error_code ec;
    for (int depth = 0; depth < 5 && !dir.empty(); ++depth) {
        const fs::path candidate = dir / "version.txt";
        if (fs::exists(candidate, ec)) {
            std::string text = read_text(candidate);
            while (!text.empty() && (text.back() == '\n' || text.back() == '\r' ||
                                     text.back() == ' ' || text.back() == '\t'))
                text.pop_back();
            if (!text.empty())
                return text;
        }
        dir = dir.parent_path();
    }
    return compiled;
}

}  // namespace

int cmd_version(Context& ctx) {
    json::Value out(json::Object{});
    out["name"] = std::string("bvmenv");
    out["version"] = resolve_version();
    out["vmware_dir"] = to_utf8(ctx.config.vmware_dir.wstring());
    out["vm_root"] = to_utf8(ctx.config.vm_root.wstring());
    if (ctx.json) {
        std::printf("%s\n", out.dump(2).c_str());
    } else {
        std::printf("bvmenv %s\n", resolve_version().c_str());
    }
    return 0;
}

int cmd_base(Context& ctx, const std::vector<std::string>& argv, bool verify_only) {
    if (verify_only) {
        const ParsedArgs args = parse_args(argv, {"json", "verbose", "deep"});
        BaseVerifyReport report = ctx.lab.base_verify("base", args.has("deep"));
        if (ctx.json) {
            std::printf("%s\n", report.to_json().dump(2).c_str());
        } else {
            for (const auto& [name, ok] : report.checks) {
                std::printf("[%s] %s\n", ok ? "ok" : "FAIL", name.c_str());
            }
            for (const auto& note : report.notes)
                std::printf("note: %s\n", note.c_str());
            std::printf("base verify: %s\n", report.ok ? "ok" : "FAILED");
        }
        return report.ok ? 0 : 1;
    }

    const ParsedArgs args = parse_args(argv, {"json", "verbose", "force"});
    BaseBuildOptions options;
    if (args.has("iso"))
        options.iso = fs::path(to_wide(args.get("iso")));
    options.force = args.has("force");
    if (args.has("disk-gb"))
        options.disk_gb = static_cast<int>(parse_ll(args.get("disk-gb"), 80));
    if (args.has("timeout")) {
        options.ready_timeout_s = static_cast<int>(parse_ll(args.get("timeout"), 2400));
    }
    ctx.lab.base_build(options);
    const VmInfo base = ctx.vmware.base();
    json::Value out(json::Object{});
    out["schema"] = std::string("bvmenv.base-build.v1");
    out["ok"] = true;
    out["vmx"] = base.vmx;
    out["snapshot"] = std::string("base");
    if (ctx.json) {
        std::printf("%s\n", out.dump(2).c_str());
    } else {
        std::printf("base image ready: %s (snapshot: base)\n", base.vmx.c_str());
    }
    return 0;
}

namespace {
int run_pin_vcpu(Context& ctx, const std::string& vm, const std::string& pcores);
fs::path find_repo_script(const char* name);
}  // namespace

namespace {

// Locate a script shipped with the repository (walk up from the executable).
fs::path find_repo_script(const char* name) {
    wchar_t buffer[1024];
    const DWORD len = GetModuleFileNameW(nullptr, buffer, static_cast<DWORD>(std::size(buffer)));
    if (len == 0)
        return {};
    std::error_code ec;
    fs::path dir = fs::path(buffer).parent_path();
    for (int depth = 0; depth < 6 && !dir.empty(); ++depth) {
        const fs::path candidate = dir / "scripts" / name;
        if (fs::exists(candidate, ec))
            return candidate;
        dir = dir.parent_path();
    }
    return {};
}

}  // namespace

int cmd_create(Context& ctx, const ParsedArgs& args) {
    if (const int rc = require_name(args, "create"); rc)
        return rc;
    CreateOptions options;
    options.name = args.positional[0];
    if (args.has("cpus"))
        options.cpus = static_cast<int>(parse_ll(args.get("cpus"), 0));
    if (args.has("memory")) {
        options.memory_mb = static_cast<int>(parse_ll(args.get("memory"), 0));
    }
    if (args.has("from"))
        options.from = args.get("from");
    options.kd_pipe = args.has("kd-pipe");
    options.serial_pipe = args.has("serial-pipe");
    options.pin_pcores = args.has("pin-pcores");
    options.pcore_list = args.get("pcores");
    if (options.kd_pipe && !args.has("unsafe-kd")) {
        std::cerr << "bvmenv: create --kd-pipe is disabled on this host (KD pipe correlates "
                     "with the host freezes); pass --unsafe-kd to override\n";
        return 2;
    }
    options.start = !args.has("no-start");
    options.wait_ready = !args.has("no-wait");
    if (args.has("timeout")) {
        options.ready_timeout_s = static_cast<int>(parse_ll(args.get("timeout"), 240));
    }
    const auto started = std::chrono::steady_clock::now();
    const VmInfo info = ctx.lab.create(options);
    if (options.pin_pcores && options.start) {
        run_pin_vcpu(ctx, options.name, options.pcore_list.empty() ? "0-15" : options.pcore_list);
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started);
    json::Value out(json::Object{});
    out["schema"] = std::string("bvmenv.create.v1");
    out["name"] = info.name;
    out["vmx"] = info.vmx;
    out["base"] = std::string("base");
    out["snapshot"] = std::string("base");
    out["cpus"] = info.cpus;
    out["memory_mb"] = info.memory_mb;
    out["duration_ms"] = static_cast<long long>(elapsed.count());
    if (ctx.json) {
        std::printf("%s\n", out.dump(2).c_str());
    } else {
        std::printf("created %s (%s) in %lldms\n", info.name.c_str(), info.vmx.c_str(),
                    static_cast<long long>(elapsed.count()));
    }
    return 0;
}

int cmd_delete(Context& ctx, const ParsedArgs& args) {
    if (const int rc = require_name(args, "delete"); rc)
        return rc;
    const std::string name = args.positional[0];
    try {
        ctx.lab.destroy(name);
    } catch (const std::exception& e) {
        ctx.logger.info(std::string("destroy failed: ") + e.what());
        if (!args.has("force"))
            throw;
        // Stale fork: no VMware process holds it, so removing the directory is
        // safe (a live VM keeps its files locked and the removal fails loudly).
        const fs::path dir = ctx.config.vm_root / "vms" / name;
        std::error_code ec;
        fs::remove_all(dir, ec);
        if (ec) {
            throw Error(std::string("force delete failed (a process may hold the VM): ") +
                        ec.message());
        }
        ctx.logger.info("force-deleted stale fork directory: " + to_utf8(dir.wstring()));
    }
    json::Value out(json::Object{});
    out["schema"] = std::string("bvmenv.delete.v1");
    out["ok"] = true;
    out["name"] = args.positional[0];
    if (ctx.json)
        std::printf("%s\n", out.dump(2).c_str());
    else
        std::printf("deleted %s\n", args.positional[0].c_str());
    return 0;
}

int cmd_start_stop(Context& ctx, const ParsedArgs& args, bool start) {
    if (const int rc = require_name(args, start ? "start" : "stop"); rc)
        return rc;
    const std::string name = args.positional[0];
    if (start) {
        // Idempotent: starting an already-running VM must return immediately
        // (a 120 s hang here was reported from a downstream preflight).
        if (ctx.vmware.power_state(name) == PowerState::On) {
            json::Value out(json::Object{});
            out["schema"] = std::string("bvmenv.power.v1");
            out["ok"] = true;
            out["name"] = name;
            out["action"] = std::string("start");
            out["power"] = std::string("on");
            out["already_running"] = true;
            if (ctx.json)
                std::printf("%s\n", out.dump(2).c_str());
            else
                std::printf("already running: %s\n", name.c_str());
            return 0;
        }
        ctx.vmware.start(name, args.has("gui"));
        if (!args.has("no-wait"))
            ctx.lab.wait_ready(name, std::chrono::seconds(600));
    } else {
        ctx.vmware.stop(name, !args.has("soft"));
    }
    json::Value out(json::Object{});
    out["schema"] = std::string("bvmenv.power.v1");
    out["ok"] = true;
    out["name"] = name;
    out["action"] = start ? "start" : "stop";
    out["power"] = std::string(to_string(ctx.vmware.power_state(name)));
    if (ctx.json)
        std::printf("%s\n", out.dump(2).c_str());
    else
        std::printf("%s: %s\n", start ? "started" : "stopped", name.c_str());
    return 0;
}

int cmd_reset(Context& ctx, const ParsedArgs& args) {
    if (const int rc = require_name(args, "reset"); rc)
        return rc;
    const std::string name = args.positional[0];
    const ResetReport report = ctx.lab.reset(name, "manual");
    if (ctx.json) {
        std::printf("%s\n", report.to_json().dump(2).c_str());
    } else {
        std::printf("reset %s: %s (%s, %lldms)\n", name.c_str(), report.ok ? "ok" : "FAILED",
                    report.strategy.c_str(), static_cast<long long>(report.duration.count()));
    }
    if (!report.ok)
        return 1;
    if (args.has("start") || args.has("wait")) {
        ctx.vmware.start(name, false);
        ctx.lab.wait_ready(name, std::chrono::seconds(240));
    }
    return 0;
}

int cmd_list(Context& ctx, const ParsedArgs& args) {
    (void)args;
    const auto vms = ctx.vmware.list();
    json::Value out(json::Object{});
    out["schema"] = std::string("bvmenv.list.v1");
    json::Value items(json::Array{});
    for (const auto& vm : vms) {
        json::Value item(json::Object{});
        item["name"] = vm.name;
        item["vmx"] = vm.vmx;
        item["power"] = std::string(to_string(vm.power));
        item["cpus"] = vm.cpus;
        item["memory_mb"] = vm.memory_mb;
        item["base"] = vm.is_base;
        item["created_at"] = vm.created_at;
        items.push(std::move(item));
    }
    out["vms"] = std::move(items);
    if (ctx.json) {
        std::printf("%s\n", out.dump(2).c_str());
    } else {
        for (const auto& vm : vms) {
            std::printf("%-16s %-10s %s\n", vm.name.c_str(),
                        std::string(to_string(vm.power)).c_str(), vm.vmx.c_str());
        }
    }
    return 0;
}

int cmd_state(Context& ctx, const ParsedArgs& args) {
    if (const int rc = require_name(args, "state"); rc)
        return rc;
    StateOptions options;
    options.capture = true;
    if (args.has("no-capture"))
        options.capture = false;
    options.use_ocr = !args.has("no-ocr");
    options.samples = static_cast<int>(parse_ll(args.get("samples"), 1));
    if (options.samples > 1)
        options.samples = std::max(options.samples, ctx.config.hang_samples);
    const StateReport report = ctx.lab.state(args.positional[0], options);
    print_state(report, ctx.json);
    if (args.has("screenshot")) {
        // Copy the captured frame to a stable location for evidence.
        if (!report.screen.screenshot.empty()) {
            std::error_code ec;
            fs::copy_file(fs::path(to_wide(report.screen.screenshot)),
                          fs::path(to_wide(args.get("screenshot"))),
                          fs::copy_options::overwrite_existing, ec);
        }
    }
    if (report.hang_suspected)
        return 4;
    return 0;
}

int cmd_screenshot(Context& ctx, const ParsedArgs& args) {
    if (args.positional.size() < 2) {
        std::cerr << "bvmenv: screenshot requires <name> <out.png>\n";
        return 2;
    }
    const std::string name = args.positional[0];
    const fs::path out = fs::path(to_wide(args.positional[1]));
    if (!ctx.vmware.capture_screenshot(name, out)) {
        std::cerr << "bvmenv: screenshot capture failed (is the VM running?)\n";
        return 1;
    }
    json::Value result(json::Object{});
    result["schema"] = std::string("bvmenv.screenshot.v1");
    result["ok"] = true;
    result["path"] = to_utf8(out.wstring());
    if (args.has("classify")) {
        std::string error;
        if (auto stats = analyze_image(out, &error)) {
            // OCR matters here: a bugcheck frame is blue with light text, so
            // without the text it looked like "boot" instead of "bsod".
            std::string ocr_engine;
            const std::string text = ocr_image(out, ctx.config.ocr_command, &ocr_engine);
            const ScreenReport report = classify_screen(*stats, text);
            result["classification"] = std::string(to_string(report.classification));
            result["confidence"] = report.confidence;
            result["reason"] = report.reason;
            if (!report.stop_code.empty())
                result["stop_code"] = report.stop_code;
            if (!ocr_engine.empty())
                result["ocr_engine"] = ocr_engine;
        }
    }
    if (ctx.json)
        std::printf("%s\n", result.dump(2).c_str());
    else
        std::printf("%s\n", to_utf8(out.wstring()).c_str());
    return 0;
}

int cmd_exec(Context& ctx, const ParsedArgs& args) {
    if (const int rc = require_name(args, "exec"); rc)
        return rc;
    ExecRequest request;
    request.vm = args.positional[0];
    if (!args.trailing.empty()) {
        request.command_line = join(args.trailing, " ");
    } else if (args.positional.size() > 1) {
        std::vector<std::string> cmd(args.positional.begin() + 1, args.positional.end());
        request.command_line = join(cmd, " ");
    }
    if (request.command_line.empty() && !args.has("self-test")) {
        std::cerr << "bvmenv: exec requires a command (bvmenv exec <vm> -- <command>)\n";
        return 2;
    }
    request.timeout =
        std::chrono::seconds(parse_ll(args.get("timeout"), ctx.config.default_timeout_s));
    request.elevated = args.has("elevated");
    {
        auto range = args.multi.equal_range("env");
        for (auto it = range.first; it != range.second; ++it)
            request.env.push_back(it->second);
    }
    if (args.has("workdir"))
        request.workdir = args.get("workdir");
    if (args.has("self-test")) {
        // Round trip every guest primitive separately so a failure names the
        // broken step instead of a bare exit_code=-1.
        json::Value steps(json::Array{});
        auto record = [&](const char* step, bool ok, const std::string& detail) {
            json::Value item(json::Object{});
            item["step"] = std::string(step);
            item["ok"] = ok;
            item["detail"] = detail;
            steps.push(std::move(item));
            return ok;
        };
        bool ok = true;
        try {
            ctx.vmware.guest_mkdirs(request.vm, "C:\\blook\\selftest");
            ok = record("mkdir", true, "C:\\blook\\selftest") && ok;
        } catch (const std::exception& e) {
            ok = record("mkdir", false, e.what()) && ok;
        }
        try {
            const fs::path probe = ctx.config.cache_dir() / "selftest.txt";
            write_text(probe, "bvmenv-self-test\r\n");
            ctx.vmware.copy_to_guest(request.vm, probe, "C:\\blook\\selftest\\probe.txt");
            ok = record("copy_to_guest", true, "probe.txt") && ok;
        } catch (const std::exception& e) {
            ok = record("copy_to_guest", false, e.what()) && ok;
        }
        ExecRequest probe = request;
        probe.command_line = "cmd /c \"echo BVMENV-SELFTEST & exit /b 7\"";
        probe.timeout = std::chrono::seconds(60);
        const ExecResult probe_result = ctx.lab.exec(probe);
        ok = record("run", probe_result.ran && !probe_result.timed_out,
                    probe_result.error.empty() ? "ok" : probe_result.error) &&
             ok;
        ok = record("stdout", probe_result.out.find("BVMENV-SELFTEST") != std::string::npos,
                    trim(probe_result.out).substr(0, 120)) &&
             ok;
        ok = record("exit_code", probe_result.exit_code == 7,
                    "exit_code=" + std::to_string(probe_result.exit_code)) &&
             ok;
        ExecRequest sleeper = request;
        sleeper.command_line = "cmd /c \"ping -n 10 127.0.0.1\"";
        sleeper.timeout = std::chrono::seconds(3);
        const ExecResult timeout_result = ctx.lab.exec(sleeper);
        ok = record("timeout", timeout_result.timed_out,
                    "timed_out=" + std::string(timeout_result.timed_out ? "true" : "false")) &&
             ok;
        json::Value out(json::Object{});
        out["schema"] = std::string("bvmenv.exec-selftest.v1");
        out["vm"] = request.vm;
        out["ok"] = ok;
        out["steps"] = std::move(steps);
        std::printf("%s\n", out.dump(2).c_str());
        return ok ? 0 : 1;
    }
    const ExecResult result = ctx.lab.exec(request);
    print_exec(result, ctx.json);
    if (!result.ran || result.timed_out)
        return 1;
    return result.exit_code == 0 ? 0 : 1;
}

int cmd_push_pull(Context& ctx, const ParsedArgs& args, bool push) {
    if (args.positional.size() < 3) {
        std::cerr << (push ? "bvmenv: push requires <name> <local> <guest>\n"
                           : "bvmenv: pull requires <name> <guest> <local>\n");
        return 2;
    }
    const std::string name = args.positional[0];
    const std::string a = args.positional[1];
    const std::string b = args.positional[2];
    json::Value out(json::Object{});
    out["schema"] = std::string("bvmenv.copy.v1");
    if (push) {
        const fs::path local = fs::path(to_wide(a));
        if (fs::is_directory(local)) {
            const int count = ctx.vmware.push_tree(name, local, b);
            out["files"] = count;
        } else {
            ctx.vmware.guest_mkdirs(name, b.substr(0, b.find_last_of('\\')));
            ctx.vmware.copy_to_guest(name, local, b);
            out["files"] = 1;
        }
    } else {
        const fs::path local = fs::path(to_wide(b));
        ctx.vmware.copy_from_guest(name, a, local);
        out["files"] = 1;
    }
    out["ok"] = true;
    if (ctx.json)
        std::printf("%s\n", out.dump(2).c_str());
    else
        std::printf("copied %lld file(s)`n", static_cast<long long>(out["files"].as_int(0)));
    return 0;
}

int cmd_e2e(Context& ctx, const ParsedArgs& args) {
    if (const int rc = require_name(args, "e2e"); rc)
        return rc;
    E2eRequest request;
    request.vm = args.positional[0];
    request.bundle = args.get("bundle");
    request.entry = args.get("entry");
    if (request.entry.empty() && args.positional.size() > 1)
        request.entry = args.positional[1];
    for (const auto& [key, value] : args.multi) {
        if (key == "args")
            request.args.push_back(value);
    }
    for (const auto& value : args.trailing)
        request.args.push_back(value);
    if (request.bundle.empty() || request.entry.empty()) {
        std::cerr << "bvmenv: e2e requires --bundle <dir> and --entry <exe|cmd>\n";
        return 2;
    }
    if (request.bundle.find('\\') == std::string::npos &&
        request.bundle.find('/') == std::string::npos) {
        std::cerr << "bvmenv: --bundle must be a host directory path\n";
        return 2;
    }
    if (!file_exists(fs::path(to_wide(request.bundle)))) {
        std::cerr << "bvmenv: bundle not found: " << request.bundle << "\n";
        return 2;
    }
    request.timeout = std::chrono::seconds(parse_ll(args.get("timeout"), 300));
    request.out_dir = args.get("out");
    request.reset_on = args.get("reset-on", "failure");
    if (args.has("wait-timeout")) {
        request.wait_ready_timeout_s = static_cast<int>(parse_ll(args.get("wait-timeout"), 180));
    }
    const E2eResult result = ctx.lab.e2e(request);
    json::Value out = result.to_json();
    std::printf("%s\n", out.dump(2).c_str());
    if (!ctx.json) {
        std::fprintf(stderr, "e2e: outcome=%s artifacts=%s\n",
                     std::string(to_string(result.outcome)).c_str(), result.artifact_dir.c_str());
    }
    return result.outcome == E2eOutcome::Pass ? 0 : 1;
}

int cmd_wait_ready(Context& ctx, const ParsedArgs& args) {
    if (const int rc = require_name(args, "wait-ready"); rc)
        return rc;
    const std::string name = args.positional[0];
    const auto timeout = std::chrono::seconds(parse_ll(args.get("timeout"), 240));
    const bool ok = ctx.lab.wait_ready(name, timeout);
    json::Value out(json::Object{});
    out["schema"] = std::string("bvmenv.wait-ready.v1");
    out["ok"] = ok;
    out["name"] = name;
    out["timeout_s"] = static_cast<long long>(timeout.count());
    if (ctx.json)
        std::printf("%s\n", out.dump(2).c_str());
    else
        std::printf("%s: %s\n", name.c_str(), ok ? "ready" : "NOT ready");
    return ok ? 0 : 1;
}

int cmd_key(Context& ctx, const ParsedArgs& args) {
    if (args.positional.size() < 2) {
        std::cerr << "bvmenv: key requires <name> <hid> [modifier] | keys <name> <text>\n";
        return 2;
    }
    const std::string name = args.positional[0];
    bool ok = false;
    if (args.positional.size() >= 3 && args.positional[1] == "--text") {
        ok = ctx.vmware.send_key_sequence(name, join(args.positional, " "));
    } else if (args.positional.size() >= 3) {
        ok = ctx.vmware.send_key(name, static_cast<int>(parse_ll(args.positional[1], 0)),
                                 static_cast<int>(parse_ll(args.positional[2], 0)));
    } else {
        std::vector<std::string> text(args.positional.begin() + 1, args.positional.end());
        ok = ctx.vmware.send_key_sequence(name, join(text, " "));
    }
    json::Value out(json::Object{});
    out["schema"] = std::string("bvmenv.key.v1");
    out["ok"] = ok;
    if (ctx.json)
        std::printf("%s\n", out.dump(2).c_str());
    else
        std::printf("key injection: %s\n", ok ? "ok" : "FAILED");
    return ok ? 0 : 1;
}

int cmd_crash(Context& ctx, const ParsedArgs& args) {
    if (const int rc = require_name(args, "crash"); rc)
        return rc;
    const std::string name = args.positional[0];
    const bool ok = ctx.lab.force_bugcheck(name);
    json::Value out(json::Object{});
    out["schema"] = std::string("bvmenv.crash.v1");
    out["ok"] = ok;
    out["name"] = name;
    out["method"] = std::string("ctrl+scrolllock x2 (i8042prt CrashOnCtrlScroll)");
    if (ctx.json)
        std::printf("%s\n", out.dump(2).c_str());
    else
        std::printf("bugcheck requested for %s\n", name.c_str());
    return ok ? 0 : 1;
}

int cmd_doctor(Context& ctx, const ParsedArgs& args) {
    (void)args;
    const json::Value report = ctx.lab.doctor();
    if (ctx.json) {
        std::printf("%s\n", report.dump(2).c_str());
    } else {
        for (const auto& check : report["checks"].items()) {
            std::printf("[%s] %s\n", check["ok"].as_bool() ? "ok" : "FAIL",
                        check["name"].as_string().c_str());
        }
        std::printf("doctor: %s\n", report["ok"].as_bool() ? "ok" : "issues found");
    }
    return report["ok"].as_bool() ? 0 : 1;
}

int cmd_selftest(Context& ctx, const ParsedArgs& args) {
    (void)args;
    json::Value out(json::Object{});
    out["schema"] = std::string("bvmenv.selftest.v1");
    json::Value steps(json::Array{});
    auto add = [&](const std::string& name, bool ok, const std::string& detail) {
        json::Value item(json::Object{});
        item["step"] = name;
        item["ok"] = ok;
        item["detail"] = detail;
        steps.push(std::move(item));
        return ok;
    };

    bool ok = true;
    std::string error;
    ok &= add("config resolves vmrun/vmcli", ctx.config.validate(&error), error);
    ok &= add("VMAuthdService running", ctx.vmware.service_running(),
              ctx.vmware.service_running() ? "ok" : "start it: gsudo -d net start VMAuthdService");
    const std::string version = ctx.vmware.vmrun_version();
    ok &= add("vmrun --version", !version.empty(), trim(version));
    ok &= add("vm registry reachable", file_exists(ctx.config.vm_root),
              to_utf8(ctx.config.vm_root.wstring()));

    // Host side state primitives only need a running VM; use the base image if
    // it exists and is running, otherwise just report availability.
    if (file_exists(ctx.config.base_vmx()) && ctx.vmware.power_state("base") == PowerState::On) {
        const fs::path png = ctx.config.cache_dir() / "selftest.png";
        const bool captured = ctx.vmware.capture_screenshot("base", png);
        ok &= add("vmcli MKS captureScreenshot", captured, to_utf8(png.wstring()));
        if (captured) {
            std::string image_error;
            const auto stats = analyze_image(png, &image_error);
            ok &= add("PNG decode + statistics", stats.has_value(), image_error);
            if (stats) {
                const ScreenReport report = classify_screen(*stats, {});
                ok &= add("screen classification", report.classification != ScreenClass::Unknown,
                          std::string(to_string(report.classification)));
            }
        }
    } else {
        add("vmcli MKS captureScreenshot", true, "skipped (base image not running)");
    }
    const std::wstring tess = find_executable(L"tesseract");
    add("OCR engine", !tess.empty() || !ctx.config.ocr_command.empty(),
        tess.empty() ? "tesseract not on PATH (BSOD stop codes degrade to colour only)"
                     : to_utf8(tess));

    out["ok"] = ok;
    out["steps"] = std::move(steps);
    if (ctx.json) {
        std::printf("%s\n", out.dump(2).c_str());
    } else {
        for (const auto& step : out["steps"].items()) {
            std::printf("[%s] %s %s\n", step["ok"].as_bool() ? "ok" : "FAIL",
                        step["step"].as_string().c_str(), step["detail"].as_string().c_str());
        }
        std::printf("selftest: %s\n", ok ? "ok" : "FAILED");
    }
    return ok ? 0 : 1;
}

// ---------------------------------------------------------------------------
// Kernel debugging over a named pipe.
//
// `create --kd-pipe` rewrites the fork's serial port to
// `\\.\\pipe\\blook-<vm>-kd` (VMware is the pipe server). `bvmenv kd <vm>`
// prints the exact kd.exe command line, and `bvmenv kd-log <vm> [--seconds N]`
// runs kd and captures its text output to a file - the only usable way to see
// DbgPrint output, because the serial log is KD packet noise without a debugger.
// ---------------------------------------------------------------------------
namespace {

fs::path find_kd() {
    const wchar_t* candidates[] = {
        L"Y:\\Program Files (x86)\\Windows Kits\\10\\Debuggers\\x64\\kd.exe",
        L"C:\\Program Files (x86)\\Windows Kits\\10\\Debuggers\\x64\\kd.exe",
    };
    for (const wchar_t* candidate : candidates) {
        if (file_exists(candidate))
            return fs::path(candidate);
    }
    const std::wstring found = find_executable(L"kd");
    return found.empty() ? fs::path{} : fs::path(found);
}

std::string kd_pipe_name(const std::string& vm) {
    return "\\\\.\\pipe\\blook-" + vm + "-kd";
}

std::string kd_command_line(const fs::path& kd, const std::string& pipe) {
    return to_utf8(kd.wstring()) + " -k com:pipe,port=" + pipe + ",resets=0";
}

}  // namespace

// ---------------------------------------------------------------------------
// Interactive kd session
//
// A kernel debugger must CONTINUE the target (otherwise the guest stays frozen
// at the initial breakpoint and looks hung), and to inspect a freeze it needs a
// *break-in* (Ctrl+Break) followed by debugger commands. Two Windows details
// make that possible from a background CLI:
//   * kd gets its own console (CREATE_NEW_CONSOLE) so we can AttachConsole() to
//     it and GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT) = Ctrl+Break;
//   * kd's `-logo <file>` writes the whole session to a file, so output does not
//     depend on a console handle we do not own.
// ---------------------------------------------------------------------------
namespace {

bool break_into(HANDLE process) {
    const DWORD pid = GetProcessId(process);
    if (pid == 0)
        return false;
    if (!AttachConsole(pid))
        return false;
    const BOOL sent = GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, pid);
    FreeConsole();
    // Re-attach to the parent console so later logging still works (best effort).
    AttachConsole(ATTACH_PARENT_PROCESS);
    return sent != 0;
}

void write_stdin(HANDLE in, const std::string& text) {
    DWORD written = 0;
    std::string data = text;
    if (data.empty() || data.back() != '\n')
        data += "\r\n";
    WriteFile(in, data.data(), static_cast<DWORD>(data.size()), &written, nullptr);
}

}  // namespace

int cmd_dump_pull(Context& ctx, const ParsedArgs& args) {
    // Fetch the kernel dump a stalled/bugchecked guest left behind, so it can be
    // analysed offline with `kd -z` instead of attaching kd to the VM (kd pipes
    // correlate with the host freezes on this box).
    if (const int rc = require_name(args, "dump"))
        return rc;
    const std::string name = args.positional[0];
    const fs::path out = args.has("out") ? fs::path(to_wide(args.get("out")))
                                         : (ctx.config.evidence_dir() / "dumps" / (name + ".dmp"));
    ensure_dir(out.parent_path());
    std::vector<std::string> candidates = {"C:\\Windows\\MEMORY.DMP"};
    // Kernel dump only: that is what `kd -z` needs for a stalled guest.
    std::string pulled;
    for (const auto& candidate : candidates) {
        try {
            ctx.vmware.copy_from_guest(name, candidate, out);
            pulled = candidate;
            break;
        } catch (const std::exception&) {
        }
    }
    json::Value result(json::Object{});
    result["schema"] = std::string("bvmenv.dump-pull.v1");
    result["vm"] = name;
    result["source"] = pulled;
    result["path"] = pulled.empty() ? std::string{} : to_utf8(out.wstring());
    if (!pulled.empty() && fs::exists(out)) {
        result["bytes"] = static_cast<long long>(fs::file_size(out));
        result["ok"] = true;
    } else {
        result["bytes"] = 0;
        result["ok"] = false;
    }
    if (ctx.json) {
        std::printf("%s\n", result.dump(2).c_str());
    } else if (pulled.empty()) {
        std::printf("no dump found (enable CrashDumpEnabled=1 and bugcheck the guest)\n");
    } else {
        std::printf("pulled %s -> %s (%lld bytes)\n", pulled.c_str(),
                    to_utf8(out.wstring()).c_str(), result["bytes"].as_int());
    }
    return result["ok"].as_bool() ? 0 : 1;
}

int cmd_serial_log(Context& ctx, const ParsedArgs& args) {
    // Host-side pipe SERVER for a fork created with --serial-pipe: whatever the
    // guest writes to COM1 is copied verbatim into a file. No kd, no break-in,
    // no packet handling - safe for a stalled guest.
    if (const int rc = require_name(args, "serial-log"))
        return rc;
    const std::string name = args.positional[0];
    const int seconds = static_cast<int>(parse_ll(args.get("seconds"), 60));
    const fs::path out = args.has("out")
                             ? fs::path(to_wide(args.get("out")))
                             : (ctx.config.evidence_dir() / "serial" / (name + "-serial.log"));
    ensure_dir(out.parent_path());
    const std::wstring pipe_name = L"\\\\.\\pipe\\blook-" + to_wide(name) + L"-serial";
    HANDLE pipe = CreateNamedPipeW(pipe_name.c_str(), PIPE_ACCESS_INBOUND,
                                   PIPE_TYPE_BYTE | PIPE_WAIT, 1, 64 * 1024, 64 * 1024, 0, nullptr);
    if (pipe == INVALID_HANDLE_VALUE) {
        std::cerr << "bvmenv: CreateNamedPipe failed (" << GetLastError() << ")\n";
        return 2;
    }
    std::ofstream file(out, std::ios::binary);
    if (!file) {
        CloseHandle(pipe);
        std::cerr << "bvmenv: cannot write " << to_utf8(out.wstring()) << "\n";
        return 2;
    }
    if (args.has("boot")) {
        // VMware connects as the pipe CLIENT at power-on: the server must exist
        // first, otherwise the port is inert for the whole boot.
        try {
            ctx.vmware.start(name, false);
        } catch (const std::exception& e) {
            std::cerr << "bvmenv: start failed: " << e.what() << "\n";
        }
    }
    std::atomic<bool> stop{false};
    std::atomic<long long> written{0};
    std::thread reader([&] {
        if (!ConnectNamedPipe(pipe, nullptr) && GetLastError() != ERROR_PIPE_CONNECTED)
            return;
        char buffer[8192];
        DWORD read = 0;
        while (!stop.load() && ReadFile(pipe, buffer, sizeof(buffer), &read, nullptr) && read > 0) {
            file.write(buffer, read);
            file.flush();
            written.fetch_add(read);
        }
    });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    while (std::chrono::steady_clock::now() < deadline)
        sleep_ms(200);
    stop = true;
    CancelIoEx(pipe, nullptr);
    DisconnectNamedPipe(pipe);
    CloseHandle(pipe);
    reader.join();
    file.close();
    json::Value result(json::Object{});
    result["schema"] = std::string("bvmenv.serial-log.v1");
    result["vm"] = name;
    result["pipe"] = to_utf8(pipe_name);
    result["path"] = to_utf8(out.wstring());
    result["seconds"] = seconds;
    result["bytes"] = written.load();
    result["ok"] = written.load() > 0;
    if (ctx.json) {
        std::printf("%s\n", result.dump(2).c_str());
    } else {
        std::printf("serial-log %s: %lld bytes -> %s\n", written.load() > 0 ? "ok" : "EMPTY",
                    written.load(), to_utf8(out.wstring()).c_str());
    }
    return written.load() > 0 ? 0 : 1;
}

int cmd_serial_probe(Context& ctx, const ParsedArgs& args) {
    // Assert that the fork's file-backed COM1 actually works: write a known
    // marker to COM1 inside the guest and require the host-side serial.log to
    // grow. A fork whose serial block lost tryNoRxLoss/yieldOnMsrRead has an
    // inert COM1 (log stays 0 bytes), which silently breaks serial KD.
    if (const int rc = require_name(args, "serial-probe"))
        return rc;
    const std::string name = args.positional[0];
    const fs::path log = ctx.config.vm_root / "vms" / name / "serial.log";
    std::error_code ec;
    const auto size_before = fs::exists(log, ec) ? fs::file_size(log, ec) : 0;
    const std::string marker = "BVMENV-SERIAL-PROBE-" + std::to_string(GetTickCount64());
    ExecRequest request;
    request.vm = name;
    request.command_line = "cmd /c echo " + marker + ">COM1";
    request.timeout = std::chrono::seconds(45);
    request.run_id = "serial-" + std::to_string(GetTickCount64());
    bool guest_ok = false;
    try {
        const ExecResult result = ctx.vmware.run(request);
        guest_ok = result.ran && !result.timed_out && result.exit_code == 0;
    } catch (const std::exception&) {
    }
    sleep_ms(2500);
    const auto size_after = fs::exists(log, ec) ? fs::file_size(log, ec) : 0;
    std::string tail;
    if (fs::exists(log, ec)) {
        const std::string text = read_text(log);
        tail = text.size() > 400 ? text.substr(text.size() - 400) : text;
    }
    const bool grew = size_after > size_before;
    json::Value out(json::Object{});
    out["schema"] = std::string("bvmenv.serial-probe.v1");
    out["vm"] = name;
    out["log"] = to_utf8(log.wstring());
    out["bytes_before"] = static_cast<long long>(size_before);
    out["bytes_after"] = static_cast<long long>(size_after);
    out["guest_write_ok"] = guest_ok;
    out["marker"] = marker;
    out["marker_in_log"] = tail.find(marker) != std::string::npos;
    out["grew"] = grew;
    out["ok"] = grew && guest_ok;
    if (ctx.json) {
        std::printf("%s\n", out.dump(2).c_str());
    } else {
        std::printf("serial %s: %lld -> %lld bytes (guest_write_ok=%s marker_in_log=%s)\n",
                    grew ? "WORKS" : "INERT", static_cast<long long>(size_before),
                    static_cast<long long>(size_after), guest_ok ? "true" : "false",
                    tail.find(marker) != std::string::npos ? "true" : "false");
    }
    return (grew && guest_ok) ? 0 : 1;
}

namespace {

// Runtime CPU pinning for a fork: sched.cpu.affinity in the .vmx prevents
// power-on on this Workstation build, so the mask is applied to the running
// vmware-vmx process (scripts/pin-vcpu.ps1 finds it, retries while it boots,
// retries elevated through gsudo when needed, and verifies the mask).
int run_pin_vcpu(Context& ctx, const std::string& vm, const std::string& pcores) {
    const auto info = ctx.vmware.find(vm);
    if (!info) {
        std::cerr << "bvmenv: no such VM: " << vm << "\n";
        return 2;
    }
    const fs::path script = find_repo_script("pin-vcpu.ps1");
    if (script.empty()) {
        std::cerr << "bvmenv: scripts/pin-vcpu.ps1 not found\n";
        return 2;
    }
    ProcessOptions options;
    options.command_line = L"powershell -NoProfile -ExecutionPolicy Bypass -File " +
                           script.wstring() + L" -Vmx " + to_wide(info->vmx) + L" -Pcores " +
                           to_wide(pcores);
    options.timeout = std::chrono::seconds(120);
    const ProcessResult result = run_process(options);
    std::printf("%s\n", trim(result.out + result.err).c_str());
    return result.exit_code == 0 ? 0 : 1;
}

}  // namespace

int cmd_pin_vcpu(Context& ctx, const ParsedArgs& args) {
    if (const int rc = require_name(args, "pin-vcpu"))
        return rc;
    const std::string pcores = args.get("pcores", "0-15");
    return run_pin_vcpu(ctx, args.positional[0], pcores);
}

int cmd_testsigning(Context& ctx, const ParsedArgs& args) {
    // Lab VMs run with testsigning ON so SCM can load test-signed drivers on
    // Win11 26100 (legacy SPC signatures are rejected there with 577 even with
    // the TSA root trusted). Owners may switch a single fork to DSE-on to test
    // the SPC path; a reboot is required, so it is a separate step on purpose.
    if (args.positional.size() < 2) {
        std::cerr << "bvmenv: testsigning requires <vm> on|off\n";
        return 2;
    }
    const std::string name = args.positional[0];
    const std::string mode = to_lower(args.positional[1]);
    if (mode != "on" && mode != "off") {
        std::cerr << "bvmenv: testsigning expects on|off\n";
        return 2;
    }
    ExecRequest request;
    request.vm = name;
    request.command_line = "cmd /c bcdedit /set {current} testsigning " + mode +
                           " & bcdedit /enum {current} | findstr /i testsigning";
    request.timeout = std::chrono::seconds(60);
    request.run_id = "testsigning-" + std::to_string(GetTickCount64());
    const ExecResult result = ctx.vmware.run(request);
    json::Value out(json::Object{});
    out["schema"] = std::string("bvmenv.testsigning.v1");
    out["vm"] = name;
    out["mode"] = mode;
    out["applied"] = result.ran && !result.timed_out && result.exit_code == 0;
    out["output"] = trim(result.out + result.err);
    if (args.has("reboot")) {
        ctx.vmware.stop(name, false);
        ctx.vmware.start(name, false);
        out["rebooted"] = true;
    }
    if (ctx.json) {
        std::printf("%s\n", out.dump(2).c_str());
    } else {
        std::printf("testsigning %s on %s (rebooted=%s)\n", mode.c_str(), name.c_str(),
                    args.has("reboot") ? "true" : "false");
    }
    return out["applied"].as_bool() ? 0 : 1;
}

int cmd_host_prep(Context& ctx, const ParsedArgs& args) {
    // Stop other hypervisor VMM drivers (MuMu/VirtualBox/Nemu) before running VM
    // tests: two type-2 VMMs at once froze this host twice.
    (void)args;
    json::Value out(json::Object{});
    out["schema"] = std::string("bvmenv.host-prep.v1");
    json::Value actions(json::Array{});
    std::vector<std::string> lines;
    bool ok = true;
    for (const auto& driver : ctx.lab.other_vmm_drivers()) {
        if (!driver.running)
            continue;
        ProcessOptions stop;
        stop.command_line = L"sc stop " + to_wide(driver.name);
        stop.timeout = std::chrono::seconds(30);
        ProcessResult result = run_process(stop);
        std::string how = "sc stop";
        if (result.exit_code != 0) {
            const std::wstring gsudo = find_executable(L"gsudo");
            if (!gsudo.empty()) {
                stop.command_line = gsudo + L" -d sc stop " + to_wide(driver.name);
                result = run_process(stop);
                how = "gsudo -d sc stop";
            }
        }
        const bool stopped = result.exit_code == 0;
        json::Value action(json::Object{});
        action["driver"] = driver.name;
        action["previous_state"] = driver.state;
        action["stopped"] = stopped;
        action["via"] = how;
        action["output"] = trim(result.out + result.err);
        lines.push_back(driver.name + (stopped ? ": stopped via " : ": FAILED to stop via ") + how);
        actions.push(std::move(action));
        if (!stopped)
            ok = false;
    }
    out["actions"] = actions;
    out["ok"] = ok;
    if (ctx.json) {
        std::printf("%s\n", out.dump(2).c_str());
    } else {
        for (const auto& line : lines)
            std::printf("%s\n", line.c_str());
        std::printf("host-prep %s\n", ok ? "ok" : "incomplete (run as administrator)");
    }
    return ok ? 0 : 1;
}

int cmd_kd_pipe(Context& ctx, const ParsedArgs& args) {
    // Convert an EXISTING fork to (or from) named-pipe KD without recreating it.
    // Owners already have forks created without --kd-pipe; this rewrites serial0
    // in their vmx so `start` + `kd-log` work immediately.
    if (args.positional.empty()) {
        std::cerr << "bvmenv: kd-pipe requires a VM name\n";
        return 2;
    }
    const std::string name = args.positional[0];
    const auto info = ctx.vmware.find(name);
    if (!info) {
        std::cerr << "bvmenv: no such VM: " << name << "\n";
        return 2;
    }
    if (info->power != PowerState::Off) {
        std::cerr << "bvmenv: stop the VM first (kd-pipe rewrites its .vmx)\n";
        return 2;
    }
    if (!args.has("unsafe-kd") && !args.has("off")) {
        std::cerr << "bvmenv: kd over the serial pipe is DISABLED by default on this host.\n"
                     "  A KD pipe on a VMware guest correlates with every host freeze we have\n"
                     "  seen (Kernel-Power 41, no dump): with the pipe in server mode a slow or\n"
                     "  absent debugger client can block a vCPU, and the documented client mode\n"
                     "  did not connect here.\n"
                     "  Use breadcrumbs instead (`bvmenv crash` + `bvmenv state`, DbgPrint via\n"
                     "  the guest log), or pass --unsafe-kd if you accept the risk.\n";
        return 2;
    }
    const bool enable = !args.has("off");
    Vmx vmx = Vmx::load(fs::path(to_wide(info->vmx)));
    if (enable) {
        vmx.set("serial0.present", "TRUE");
        vmx.set("serial0.fileType", "pipe");
        vmx.set("serial0.fileName", kd_pipe_name(name));
        vmx.set("serial0.pipe.endPoint", "client");
        vmx.remove("serial0.tryNoRxLoss");
        vmx.remove("serial0.yieldOnMsrRead");
    } else {
        vmx.set("serial0.present", "TRUE");
        vmx.set("serial0.fileType", "file");
        vmx.set("serial0.fileName", "serial.log");
        // Restore the rest of the file-backed block too: a fork whose serial is
        // missing tryNoRxLoss/yieldOnMsrRead has an inert COM1 (the log stays
        // 0 bytes no matter what the guest writes).
        vmx.set("serial0.tryNoRxLoss", "FALSE");
        vmx.set("serial0.yieldOnMsrRead", "TRUE");
        vmx.remove("serial0.pipe.endPoint");
    }
    vmx.save(fs::path(to_wide(info->vmx)));

    const fs::path kd = find_kd();
    json::Value out(json::Object{});
    out["schema"] = std::string("bvmenv.kd-pipe.v1");
    out["vm"] = name;
    out["enabled"] = enable;
    out["pipe"] = enable ? kd_pipe_name(name) : std::string{};
    if (!kd.empty())
        out["command"] = enable ? kd_command_line(kd, kd_pipe_name(name)) : std::string{};
    if (ctx.json) {
        std::printf("%s\n", out.dump(2).c_str());
    } else if (enable) {
        std::printf(
            "kd-pipe enabled for %s\n  pipe: %s\n  kd: %s\n", name.c_str(),
            kd_pipe_name(name).c_str(),
            kd.empty() ? "(kd.exe not found)" : kd_command_line(kd, kd_pipe_name(name)).c_str());
    } else {
        std::printf("kd-pipe disabled for %s (serial0 -> serial.log)\n", name.c_str());
    }
    return 0;
}

int cmd_kd(Context& ctx, const ParsedArgs& args) {
    if (args.positional.empty()) {
        std::cerr << "bvmenv: kd requires a VM name (create it with --kd-pipe)\n";
        return 2;
    }
    const std::string name = args.positional[0];
    if (!ctx.vmware.find(name)) {
        std::cerr << "bvmenv: no such VM: " << name << "\n";
        return 2;
    }
    const std::string pipe = kd_pipe_name(name);

    // A fork without the pipe serial cannot be debugged: say so instead of
    // waiting forever on a pipe server that never appears.
    {
        const Vmx vmx = Vmx::load(fs::path(to_wide(ctx.vmware.find(name)->vmx)));
        const bool is_pipe =
            to_lower(vmx.get("serial0.fileType")) == "pipe" && vmx.get("serial0.fileName") == pipe;
        if (!is_pipe) {
            std::cerr << "bvmenv: " << name << " has no KD pipe (serial0 is not " << pipe
                      << "); convert it with `bvmenv kd-pipe " << name
                      << "` (VM powered off) or re-create it with --kd-pipe\n";
            return 2;
        }
    }

    const fs::path kd = find_kd();
    if (kd.empty()) {
        std::cerr << "bvmenv: kd.exe not found (install the Windows SDK Debugging Tools)\n";
        return 2;
    }
    json::Value out(json::Object{});
    out["schema"] = std::string("bvmenv.kd.v1");
    out["vm"] = name;
    out["pipe"] = pipe;
    out["kd_exe"] = to_utf8(kd.wstring());
    out["command"] = kd_command_line(kd, pipe);

    const bool log_mode = args.has("log") || args.has("launch") || args.has("commands") ||
                          args.has("break-after") || args.has("reset");
    if (!log_mode) {
        if (ctx.json) {
            std::printf("%s\n", out.dump(2).c_str());
        } else {
            std::printf("%s\n", kd_command_line(kd, pipe).c_str());
        }
        return 0;
    }

    const int seconds = static_cast<int>(parse_ll(args.get("seconds"), 30));
    const int break_after = static_cast<int>(parse_ll(args.get("break-after"), 0));
    const bool no_continue = args.has("no-continue");
    const std::string commands = args.get("commands", "~* kv; !thread; q");
    const fs::path log_path = args.has("out")
                                  ? fs::path(to_wide(args.get("out")))
                                  : (ctx.config.evidence_dir() / "kd" / (name + "-kd.log"));
    ensure_dir(log_path.parent_path());
    const fs::path session_log = log_path.string() + ".session";

    const std::wstring kd_line =
        build_command_line({kd.wstring(), L"-logo", session_log.wstring(), L"-k",
                            L"com:pipe,port=" + to_wide(pipe) + L",resets=0,reconnect", L"-c",
                            no_continue ? L"" : L"g"});

    // stdin pipe so debugger commands can be injected after the break-in.
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE in_read = nullptr, in_write = nullptr;
    if (!CreatePipe(&in_read, &in_write, &sa, 0)) {
        std::cerr << "bvmenv: CreatePipe failed\n";
        return 2;
    }
    SetHandleInformation(in_write, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = in_read;
    si.hStdOutput = nullptr;
    si.hStdError = nullptr;
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> mutable_line(kd_line.begin(), kd_line.end());
    mutable_line.push_back(L'\0');
    if (!CreateProcessW(nullptr, mutable_line.data(), nullptr, nullptr, TRUE,
                        CREATE_NEW_CONSOLE | CREATE_NEW_PROCESS_GROUP, nullptr, nullptr, &si,
                        &pi)) {
        std::cerr << "bvmenv: failed to start kd (error " << GetLastError() << ")\n";
        CloseHandle(in_read);
        CloseHandle(in_write);
        return 2;
    }
    CloseHandle(pi.hThread);
    CloseHandle(in_read);

    const int run_seconds = break_after > 0 ? break_after : seconds;
    const PowerState power_before = ctx.vmware.power_state(name);
    if (args.has("reset")) {
        // Attach first, then let KD catch the boot: a powered-off fork has to be
        // started, a running one is reset. kd itself waits for the pipe server.
        sleep_ms(3000);
        try {
            if (power_before == PowerState::Off) {
                ctx.vmware.start(name, false);
                out["started"] = true;
            } else {
                ctx.vmware.reset_power(name);
                out["reset"] = true;
            }
        } catch (const std::exception& e) {
            out["boot_error"] = e.what();
        }
    }

    json::Value phases(json::Array{});
    json::Value stream_phase(json::Object{});
    stream_phase["name"] = std::string("stream");
    stream_phase["seconds"] = run_seconds;
    phases.push(std::move(stream_phase));

    const auto stream_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(std::max(3, run_seconds));
    while (std::chrono::steady_clock::now() < stream_deadline) {
        sleep_ms(250);
    }

    bool broke_in = false;
    if (args.has("commands") || break_after > 0) {
        broke_in = break_into(pi.hProcess);
        sleep_ms(1500);
        write_stdin(in_write, commands + "\r\n");
        sleep_ms(6000);
        write_stdin(in_write, "q\r\n");
        sleep_ms(1500);
        json::Value break_phase(json::Object{});
        break_phase["name"] = std::string("break-in");
        break_phase["commands"] = commands;
        break_phase["ctrl_break"] = broke_in;
        phases.push(std::move(break_phase));
    }

    // Shutting kd down resumes the target: never leave the guest at a breakpoint.
    if (WaitForSingleObject(pi.hProcess, 3000) == WAIT_TIMEOUT) {
        TerminateProcess(pi.hProcess, 0);
    }
    const DWORD waited = WaitForSingleObject(pi.hProcess, 5000);
    (void)waited;
    CloseHandle(in_write);
    CloseHandle(pi.hProcess);

    // Prefer kd's own session log (complete, not console-buffer dependent).
    std::string log;
    if (file_exists(session_log))
        log = read_text(session_log);
    write_text(log_path, log);
    const bool connected = log.find("Connected to Windows") != std::string::npos;
    out["log_path"] = to_utf8(log_path.wstring());
    out["seconds"] = run_seconds;
    out["continued"] = !no_continue;
    out["connected"] = connected;
    out["break_in"] = broke_in;
    out["target_resumed"] = true;
    out["phases"] = std::move(phases);
    out["bytes"] = static_cast<long long>(log.size());
    if (ctx.json) {
        std::printf("%s\n", out.dump(2).c_str());
    } else {
        std::printf("kd log (%lld bytes, connected=%s, break_in=%s): %s\n",
                    static_cast<long long>(log.size()), connected ? "true" : "false",
                    broke_in ? "true" : "false", to_utf8(log_path.wstring()).c_str());
    }
    return connected ? 0 : 2;
}

void print_help() {
    std::cout << R"(bvmenv )" << resolve_version()
              << R"( - VMware Workstation lab for Windows kernel tests

Usage: bvmenv [--json] [--verbose] <command> [options]
       bvmenv --allow-many ...    bypass the concurrent-VM budget (default 4)

Base image (built once, then cloned)
  base build [--iso <path>] [--disk-gb 80] [--force]   unattended base image + snapshot 'base'
  base verify [--deep] [--json]                        host-side contract (--deep = live guest check)

Lifecycle
  create <name> [--cpus 4] [--memory 6144] [--from base] [--no-start] [--kd-pipe]
  start <name> | stop <name> [--soft] | reset <name> | delete <name> [--force]
  list [--json]

Guest execution (guest ops over VMCI; falls back to a SYSTEM task without a session)
  exec <name> [--timeout 120] [--elevated] [--workdir <dir>] [--env K=V ...]
              [--self-test] -- <command line>
  push <name> <local> <guest>       pull <name> <guest> <local>
  wait-ready <name> [--timeout 600]

Host side observation (works on BSOD / hang / boot, no guest userland)
  state <name> [--json] [--samples N] [--no-ocr] [--screenshot out.png]
  screenshot <name> <out.png> [--classify]
  key <name> <hid> <modifier> | keys <name> <text> | crash <name>

Kernel debugging over a named pipe (DbgPrint / post-hang stacks)
  kd-pipe <vm> [--off]                       convert an existing fork's serial0 to \\.\pipe\blook-<vm>-kd
  kd <vm> [--json]                           print the kd.exe -k com:pipe,... command line
  kd-log <vm> [--seconds N] [--reset] [--no-continue] [--out file]
             [--break-after N] [--commands "~* kv; q"]

End-to-end runner (used by downstream projects, contract in docs/e2e.md)
  e2e <name> --bundle <dir> --entry <exe|cmd> [--args A] [--timeout N]
             [--out <dir>] [--reset-on failure|always|never] [--wait-timeout N]

Diagnostics
  pin-vcpu <vm> [--pcores "0-15"]     pin a running fork to P-cores
  testsigning <vm> on|off [--reboot]   toggle DSE on a fork (lab images run testsigning on)
  dump pull <vm> [--out f]              fetch MEMORY.DMP / newest minidump for kd -z
  serial-log <vm> [--seconds N] [--out f]  pipe-server serial capture (--serial-pipe forks)
  serial-probe <vm>                    assert the fork's file-backed COM1 works
  doctor [--json]    host-prep [--json]    selftest [--json]    version

Environment: BVMENV_VM_ROOT (default C:\blook-vms), BVMENV_VMWARE_DIR,
             BVMENV_WINDOWS_ISO, BVMENV_GUEST_USER/PASSWORD, BVMENV_OCR_COMMAND,
             BVMENV_MAX_RUNNING_VMS (default 4)
Contracts: docs/e2e.md (bvmenv.e2e.v1), docs/state.md (bvmenv.state.v1), docs/images.md
)";
}

}  // namespace blook::bvmenv::cli
