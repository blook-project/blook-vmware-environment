// blook-vmware-environment: .vmx parsing and writing.
#include "blook/bvmenv/vmx.h"

#include <sstream>

#include "blook/bvmenv/util.h"

namespace blook::bvmenv {
namespace {

std::string unquote(std::string_view raw, bool* quoted) {
    std::string value = trim(raw);
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
        if (quoted) *quoted = true;
        return value.substr(1, value.size() - 2);
    }
    if (quoted) *quoted = false;
    return value;
}

}  // namespace

Vmx Vmx::parse(std::string_view text) {
    Vmx doc;
    std::size_t pos = 0;
    while (pos < text.size()) {
        std::size_t eol = text.find('\n', pos);
        if (eol == std::string_view::npos) eol = text.size();
        std::string line = trim(text.substr(pos, eol - pos));
        pos = eol + 1;
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        const std::size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        Entry entry;
        entry.key = trim(line.substr(0, eq));
        entry.value = unquote(line.substr(eq + 1), &entry.quoted);
        if (entry.key.empty()) continue;
        doc.entries_.push_back(std::move(entry));
    }
    return doc;
}

Vmx Vmx::load(const std::filesystem::path& path) {
    return parse(read_text(path));
}

std::string Vmx::dump() const {
    std::string out;
    for (const auto& entry : entries_) {
        out += entry.key;
        out += " = ";
        if (entry.quoted) {
            out += '"';
            out += entry.value;
            out += '"';
        } else {
            out += entry.value;
        }
        out += '\n';
    }
    return out;
}

void Vmx::save(const std::filesystem::path& path) const { write_text(path, dump()); }

bool Vmx::has(std::string_view key) const {
    return std::any_of(entries_.begin(), entries_.end(),
                       [&](const Entry& e) { return e.key == key; });
}

std::string Vmx::get(std::string_view key, std::string_view fallback) const {
    for (const auto& entry : entries_) {
        if (entry.key == key) return entry.value;
    }
    return std::string(fallback);
}

int Vmx::get_int(std::string_view key, int fallback) const {
    const std::string value = get(key);
    if (value.empty()) return fallback;
    try {
        return std::stoi(value);
    } catch (...) {
        return fallback;
    }
}

bool Vmx::get_bool(std::string_view key, bool fallback) const {
    const std::string value = to_lower(get(key));
    if (value == "true") return true;
    if (value == "false") return false;
    return fallback;
}

void Vmx::set(std::string key, std::string value) {
    for (auto& entry : entries_) {
        if (entry.key == key) {
            entry.value = std::move(value);
            entry.quoted = true;
            return;
        }
    }
    entries_.push_back(Entry{std::move(key), std::move(value), true});
}

void Vmx::remove(std::string_view key) {
    entries_.erase(std::remove_if(entries_.begin(), entries_.end(),
                                  [&](const Entry& e) { return e.key == key; }),
                   entries_.end());
}

}  // namespace blook::bvmenv
