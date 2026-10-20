// blook-vmware-environment: .vmx document handling.
//
// A .vmx file is a flat list of `key = "value"` lines with comments. Order is
// preserved on save, unknown lines are kept verbatim so that VMware's own
// bookkeeping keys survive a round trip.
#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace blook::bvmenv {

class Vmx {
   public:
    struct Entry {
        std::string key;
        std::string value;
        bool quoted = true;
    };

    // Throws std::runtime_error when the file cannot be read.
    static Vmx load(const std::filesystem::path& path);
    static Vmx parse(std::string_view text);

    [[nodiscard]] std::string dump() const;
    void save(const std::filesystem::path& path) const;

    [[nodiscard]] bool has(std::string_view key) const;
    [[nodiscard]] std::string get(std::string_view key, std::string_view fallback = {}) const;
    [[nodiscard]] int get_int(std::string_view key, int fallback = 0) const;
    [[nodiscard]] bool get_bool(std::string_view key, bool fallback = false) const;

    void set(std::string key, std::string value);
    void remove(std::string_view key);

    [[nodiscard]] const std::vector<Entry>& entries() const { return entries_; }

   private:
    std::vector<Entry> entries_;
};

}  // namespace blook::bvmenv
