// blook-vmware-environment: CLI command implementations.
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include "blook/bvmenv/classify.h"
#include "blook/bvmenv/process.h"
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
int cmd_version(Context& ctx) {
    json::Value out(json::Object{});
    out["name"] = std::string("bvmenv");
    out["version"] = std::string(BVMENV_VERSION);
    out["vmware_dir"] = to_utf8(ctx.config.vmware_dir.wstring());
    out["vm_root"] = to_utf8(ctx.config.vm_root.wstring());
    if (ctx.json) {
        std::printf("%s\n", out.dump(2).c_str());
    } else {
        std::printf("bvmenv %s\n", BVMENV_VERSION);
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
    options.start = !args.has("no-start");
    options.wait_ready = !args.has("no-wait");
    if (args.has("timeout")) {
        options.ready_timeout_s = static_cast<int>(parse_ll(args.get("timeout"), 240));
    }
    const auto started = std::chrono::steady_clock::now();
    const VmInfo info = ctx.lab.create(options);
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
    ctx.lab.destroy(args.positional[0]);
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
        ctx.vmware.start(name, args.has("gui"));
        if (!args.has("no-wait"))
            ctx.lab.wait_ready(name, std::chrono::seconds(240));
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
            const ScreenReport report = classify_screen(*stats, {});
            result["classification"] = std::string(to_string(report.classification));
            result["confidence"] = report.confidence;
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
    if (request.command_line.empty()) {
        std::cerr << "bvmenv: exec requires a command (bvmenv exec <vm> -- <command>)\n";
        return 2;
    }
    request.timeout =
        std::chrono::seconds(parse_ll(args.get("timeout"), ctx.config.default_timeout_s));
    request.elevated = args.has("elevated");
    if (args.has("workdir"))
        request.workdir = args.get("workdir");
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

void print_help() {
    std::printf(
        "bvmenv %s - VMware Workstation lab for Windows kernel tests\n"
        "\n"
        "Usage: bvmenv [--json] [--verbose] <command> [options]\n"
        "\n"
        "Base image\n"
        "  base build [--iso <path>] [--disk-gb 80] [--force]   unattended base image + snapshot "
        "'base'\n"
        "  base verify [--json]                                 machine-check the base image "
        "contract\n"
        "\n"
        "Lifecycle\n"
        "  create <name> [--cpus 4] [--memory 6144] [--from base] [--no-start]\n"
        "  start <name> | stop <name> [--soft] | reset <name> | delete <name>\n"
        "  list [--json]\n"
        "\n"
        "Guest execution (guest ops over VMCI, no guest network needed)\n"
        "  exec <name> [--timeout 120] [--elevated] [--workdir <dir>] -- <command line>\n"
        "  push <name> <local> <guest>       pull <name> <guest> <local>\n"
        "  wait-ready <name> [--timeout 240]\n"
        "\n"
        "Host side observation (works on BSOD / hang / boot)\n"
        "  state <name> [--json] [--samples N] [--no-ocr] [--screenshot out.png]\n"
        "  screenshot <name> <out.png> [--classify]\n"
        "  key <name> <hid> <modifier> | keys <name> <text> | crash <name>\n"
        "\n"
        "End-to-end runner (used by downstream projects)\n"
        "  e2e <name> --bundle <dir> --entry <exe|cmd> [--args A] [--timeout N]\n"
        "             [--out <dir>] [--reset-on failure|always|never]\n"
        "\n"
        "Diagnostics\n"
        "  doctor [--json]     selftest [--json]     version\n"
        "\n"
        "Environment: BVMENV_VM_ROOT (default C:\\blook-vms), BVMENV_VMWARE_DIR,\n"
        "             BVMENV_WINDOWS_ISO, BVMENV_GUEST_USER/PASSWORD, BVMENV_OCR_COMMAND\n"
        "Contract: docs/e2e.md (bvmenv.e2e.v1) and docs/state.md (bvmenv.state.v1)\n",
        BVMENV_VERSION);
}

}  // namespace blook::bvmenv::cli
