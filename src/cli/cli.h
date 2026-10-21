// blook-vmware-environment: CLI internal declarations.
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "blook/bvmenv/args.h"
#include "blook/bvmenv/config.h"
#include "blook/bvmenv/lab.h"
#include "blook/bvmenv/util.h"
#include "blook/bvmenv/vmware.h"

namespace blook::bvmenv::cli {

struct Context {
    Config config;
    Vmware vmware;
    Lab lab;
    bool json = false;
    Logger logger;

    Context(Config cfg) : config(std::move(cfg)), vmware(config), lab(config, vmware) {}
};

// Individual commands. `args` excludes the command words themselves.
int cmd_base(Context& ctx, const std::vector<std::string>& argv, bool verify_only);
int cmd_create(Context& ctx, const ParsedArgs& args);
int cmd_delete(Context& ctx, const ParsedArgs& args);
int cmd_start_stop(Context& ctx, const ParsedArgs& args, bool start);
int cmd_reset(Context& ctx, const ParsedArgs& args);
int cmd_list(Context& ctx, const ParsedArgs& args);
int cmd_state(Context& ctx, const ParsedArgs& args);
int cmd_screenshot(Context& ctx, const ParsedArgs& args);
int cmd_exec(Context& ctx, const ParsedArgs& args);
int cmd_push_pull(Context& ctx, const ParsedArgs& args, bool push);
int cmd_e2e(Context& ctx, const ParsedArgs& args);
int cmd_wait_ready(Context& ctx, const ParsedArgs& args);
int cmd_key(Context& ctx, const ParsedArgs& args);
int cmd_crash(Context& ctx, const ParsedArgs& args);
int cmd_kd(Context& ctx, const ParsedArgs& args);
int cmd_doctor(Context& ctx, const ParsedArgs& args);
int cmd_selftest(Context& ctx, const ParsedArgs& args);
int cmd_version(Context& ctx);

void print_help();

// Emits either JSON or a human readable representation of a state report.
void print_state(const StateReport& report, bool json);
void print_exec(const ExecResult& result, bool json);

}  // namespace blook::bvmenv::cli
