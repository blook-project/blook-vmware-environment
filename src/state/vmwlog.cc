// blook-vmware-environment: vmware.log / serial.log parsing.
#include <algorithm>
#include <fstream>
#include <regex>
#include <string>

#include "blook/bvmenv/classify.h"
#include "blook/bvmenv/util.h"

namespace blook::bvmenv {
namespace {

std::string read_tail(const fs::path& path, std::size_t max_bytes) {
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return {};
    in.seekg(0, std::ios::end);
    const auto size = static_cast<std::size_t>(in.tellg());
    const std::size_t start = size > max_bytes ? size - max_bytes : 0;
    in.seekg(static_cast<std::streamoff>(start), std::ios::beg);
    std::string data;
    data.resize(size - start);
    in.read(data.data(), static_cast<std::streamsize>(data.size()));
    return data;
}

std::vector<std::string> lines_of(const std::string& text) {
    std::vector<std::string> lines;
    std::size_t pos = 0;
    while (pos < text.size()) {
        std::size_t eol = text.find('\n', pos);
        if (eol == std::string::npos)
            eol = text.size();
        std::string line = text.substr(pos, eol - pos);
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        lines.push_back(std::move(line));
        pos = eol + 1;
    }
    return lines;
}

bool is_interesting_vmware_line(const std::string& line) {
    // Known-harmless noise: VMware looks for optional host config files.
    if (contains_ci(line, "dictionary.load"))
        return false;
    if (contains_ci(line, "configdb"))
        return false;
    if (contains_ci(line, "vspherefeatures"))
        return false;
    if (contains_ci(line, "config.ini"))
        return false;
    if (contains_ci(line, "Couldn't read file"))
        return false;
    if (line.find("-ERROR") != std::string::npos)
        return true;
    if (line.find("Msg_Post") != std::string::npos)
        return true;
    if (contains_ci(line, "guest panic"))
        return true;
    if (contains_ci(line, "no operating system"))
        return true;
    if (contains_ci(line, "cannot open"))
        return true;
    if (contains_ci(line, "failed to"))
        return true;
    if (contains_ci(line, "triple fault"))
        return true;
    return false;
}

}  // namespace

std::vector<LogIssue> parse_vmware_log(const fs::path& log_path, std::size_t max_issues) {
    std::vector<LogIssue> issues;
    const std::string text = read_tail(log_path, 512 * 1024);
    if (text.empty())
        return issues;
    const auto lines = lines_of(text);
    for (const auto& line : lines) {
        if (!is_interesting_vmware_line(line))
            continue;
        LogIssue issue;
        issue.level = line.find("-ERROR") != std::string::npos ? "error" : "warning";
        issue.source = "vmware.log";
        issue.message = trim(line);
        issues.push_back(std::move(issue));
    }
    if (issues.size() > max_issues) {
        issues.erase(issues.begin(), issues.end() - static_cast<std::ptrdiff_t>(max_issues));
    }
    return issues;
}

std::vector<LogIssue> parse_serial_log(const fs::path& log_path, std::size_t max_issues) {
    std::vector<LogIssue> issues;
    const std::string text = read_tail(log_path, 256 * 1024);
    if (text.empty())
        return issues;
    for (const auto& line : lines_of(text)) {
        const bool bugcheck = contains_ci(line, "STOP:") || contains_ci(line, "BugCheck") ||
                              contains_ci(line, "Fatal System Error") ||
                              contains_ci(line, "*** STOP") || contains_ci(line, "kdbugcheck");
        if (!bugcheck)
            continue;
        LogIssue issue;
        issue.level = "error";
        issue.source = "serial.log";
        issue.message = trim(line);
        issues.push_back(std::move(issue));
    }
    if (issues.size() > max_issues) {
        issues.erase(issues.begin(), issues.end() - static_cast<std::ptrdiff_t>(max_issues));
    }
    return issues;
}

}  // namespace blook::bvmenv
