// blook-vmware-environment: command line parsing shared by the CLI and tests.
#pragma once

#include <map>
#include <string>
#include <vector>

namespace blook::bvmenv {

struct ParsedArgs {
    std::vector<std::string> positional;
    std::map<std::string, std::string> flags;       // --key=value / --key value
    std::multimap<std::string, std::string> multi;  // repeated multi-value flags
    std::vector<std::string> trailing;              // everything after "--"

    [[nodiscard]] bool has(const std::string& key) const { return flags.count(key) > 0; }
    [[nodiscard]] std::string get(const std::string& key, const std::string& fallback = {}) const {
        auto it = flags.find(key);
        return it == flags.end() ? fallback : it->second;
    }
};

// Splits an argument vector (program name already removed) into the structure
// above. Flags that take no value must be listed in `boolean_flags`; the
// multi-value flags are collected into `multi` as well as `flags`.
ParsedArgs parse_args(const std::vector<std::string>& argv,
                      const std::vector<std::string>& boolean_flags);

}  // namespace blook::bvmenv
