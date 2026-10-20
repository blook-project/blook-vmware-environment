// blook-vmware-environment: bvmenv entry point.
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include "blook/bvmenv/process.h"
#include "blook/bvmenv/util.h"
#include "cli.h"

namespace blook::bvmenv::cli {
namespace {

std::vector<std::string> collect_argv(int argc, char** argv) {
    std::vector<std::string> out;
    for (int i = 1; i < argc; ++i) {
        const int width = MultiByteToWideChar(CP_ACP, 0, argv[i], -1, nullptr, 0);
        std::wstring wide(static_cast<std::size_t>(width), L'\0');
        MultiByteToWideChar(CP_ACP, 0, argv[i], -1, wide.data(), width);
        if (!wide.empty() && wide.back() == L'\0') wide.pop_back();
        out.push_back(to_utf8(wide));
    }
    return out;
}

bool wants_json(const std::vector<std::string>& argv) {
    for (const auto& arg : argv) {
        if (arg == "--json") return true;
    }
    return false;
}

// `base build` needs an elevated token (diskpart/dism/bcdboot). Re-exec through
// gsudo when available so the user still gets a one-command experience.
int maybe_reexec_elevated(const std::vector<std::string>& argv) {
    if (is_process_elevated()) return -1;
    const std::wstring gsudo = find_executable(L"gsudo");
    if (gsudo.empty()) {
        std::cerr << "bvmenv: base build requires an elevated shell (diskpart/dism/bcdboot);\n"
                     "        run it from an administrator terminal or install gsudo\n";
        return 2;
    }
    wchar_t self[MAX_PATH * 2];
    const DWORD len = GetModuleFileNameW(nullptr, self, static_cast<DWORD>(std::size(self)));
    std::vector<std::wstring> call{gsudo, L"-d", std::wstring(self, len)};
    for (const auto& arg : argv) call.push_back(to_wide(arg));
    ProcessOptions options;
    options.command_line = build_command_line(call);
    const ProcessResult result = run_process(options);
    std::fwrite(result.out.data(), 1, result.out.size(), stdout);
    std::fwrite(result.err.data(), 1, result.err.size(), stderr);
    return static_cast<int>(result.exit_code);
}

int dispatch(Context& ctx, const std::vector<std::string>& argv) {
    const std::string command = argv.empty() ? "" : argv[0];

    if (command.empty() || command == "help" || command == "--help" || command == "-h") {
        print_help();
        return 0;
    }
    if (command == "version" || command == "--version") return cmd_version(ctx);

    const std::vector<std::string> rest(argv.begin() + 1, argv.end());

    if (command == "base") {
        const std::string sub = rest.empty() ? "build" : rest[0];
        if (sub == "build" || sub == "verify") {
            if (!is_process_elevated() && sub == "build") {
                const int rc = maybe_reexec_elevated(argv);
                if (rc >= 0) return rc;
            }
            return cmd_base(ctx, std::vector<std::string>(rest.begin() + std::min<std::size_t>(1, rest.size()),
                                                         rest.end()),
                            sub == "verify");
        }
        std::cerr << "bvmenv: unknown base subcommand: " << sub << "\n";
        return 2;
    }

    // Shared flag sets.
    const std::vector<std::string> boolean_flags = {
        "json",  "verbose", "force",  "elevated", "hard",   "soft", "no-start",
        "start", "no-ocr",  "help",   "all",      "pristine",
    };

    if (command == "create") {
        return cmd_create(ctx, parse_args(rest, boolean_flags));
    }
    if (command == "delete" || command == "destroy" || command == "remove") {
        return cmd_delete(ctx, parse_args(rest, boolean_flags));
    }
    if (command == "start") return cmd_start_stop(ctx, parse_args(rest, boolean_flags), true);
    if (command == "stop") return cmd_start_stop(ctx, parse_args(rest, boolean_flags), false);
    if (command == "reset") return cmd_reset(ctx, parse_args(rest, boolean_flags));
    if (command == "list") return cmd_list(ctx, parse_args(rest, boolean_flags));
    if (command == "state" || command == "status") {
        return cmd_state(ctx, parse_args(rest, boolean_flags));
    }
    if (command == "screenshot") return cmd_screenshot(ctx, parse_args(rest, boolean_flags));
    if (command == "exec" || command == "run") return cmd_exec(ctx, parse_args(rest, boolean_flags));
    if (command == "push") return cmd_push_pull(ctx, parse_args(rest, boolean_flags), true);
    if (command == "pull") return cmd_push_pull(ctx, parse_args(rest, boolean_flags), false);
    if (command == "e2e") return cmd_e2e(ctx, parse_args(rest, boolean_flags));
    if (command == "wait-ready" || command == "wait") {
        return cmd_wait_ready(ctx, parse_args(rest, boolean_flags));
    }
    if (command == "key" || command == "keys") return cmd_key(ctx, parse_args(rest, boolean_flags));
    if (command == "crash") return cmd_crash(ctx, parse_args(rest, boolean_flags));
    if (command == "doctor") return cmd_doctor(ctx, parse_args(rest, boolean_flags));
    if (command == "selftest") return cmd_selftest(ctx, parse_args(rest, boolean_flags));

    std::cerr << "bvmenv: unknown command: " << command << "\n";
    print_help();
    return 2;
}

}  // namespace
}  // namespace blook::bvmenv::cli

int main(int argc, char** argv) {
    using namespace blook::bvmenv;
    using namespace blook::bvmenv::cli;

    const std::vector<std::string> args = collect_argv(argc, argv);

    Config config = Config::load();
    config.verbose = false;
    for (const auto& arg : args) {
        if (arg == "--verbose" || arg == "-v") config.verbose = true;
    }
    config.log_file = config.vm_root / "bvmenv.log";

    Context context(config);
    context.json = wants_json(args);
    context.logger.configure(config.verbose, config.log_file);

    try {
        return dispatch(context, args);
    } catch (const Error& e) {
        if (context.json) {
            json::Value err(json::Object{});
            err["schema"] = std::string("bvmenv.error.v1");
            err["ok"] = false;
            err["error"] = e.what();
            std::printf("%s\n", err.dump(2).c_str());
        } else {
            std::cerr << "bvmenv: error: " << e.what() << "\n";
        }
        return 2;
    } catch (const std::exception& e) {
        std::cerr << "bvmenv: unexpected error: " << e.what() << "\n";
        return 3;
    }
}
