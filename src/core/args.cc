// blook-vmware-environment: command line parsing.
#include "blook/bvmenv/args.h"

#include <algorithm>

namespace blook::bvmenv {

ParsedArgs parse_args(const std::vector<std::string>& argv,
                      const std::vector<std::string>& boolean_flags) {
    ParsedArgs parsed;
    bool trailing = false;
    for (std::size_t i = 0; i < argv.size(); ++i) {
        const std::string& arg = argv[i];
        if (trailing) {
            parsed.trailing.push_back(arg);
            continue;
        }
        if (arg == "--") {
            trailing = true;
            continue;
        }
        if (arg.size() > 2 && arg[0] == '-' && arg[1] == '-') {
            const std::size_t eq = arg.find('=');
            const std::string key =
                eq == std::string::npos ? arg.substr(2) : arg.substr(2, eq - 2);
            const bool is_boolean =
                std::find(boolean_flags.begin(), boolean_flags.end(), key) != boolean_flags.end();
            if (eq != std::string::npos) {
                parsed.flags[key] = arg.substr(eq + 1);
            } else if (is_boolean) {
                parsed.flags[key] = "1";
            } else if (i + 1 < argv.size() &&
                       !(argv[i + 1].size() > 2 && argv[i + 1][1] == '-')) {
                parsed.flags[key] = argv[++i];
            } else {
                parsed.flags[key] = "1";
            }
            if (key == "args" || key == "collect") {
                parsed.multi.emplace(key, parsed.flags[key]);
            }
            continue;
        }
        parsed.positional.push_back(arg);
    }
    return parsed;
}

}  // namespace blook::bvmenv
