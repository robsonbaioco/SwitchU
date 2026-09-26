#include "OnlineTitleNames.hpp"

#include "core/DebugLog.hpp"
#include "themeshop/ThemeHttp.hpp"

#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <nlohmann/json.hpp>

namespace {

constexpr std::size_t kMaxNameLength = 128;

std::string hexId(std::uint64_t titleId) {
    char id[17]{};
    std::snprintf(id, sizeof(id), "%016llX", static_cast<unsigned long long>(titleId));
    return id;
}

std::string trimmed(const std::string& value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

} // namespace

bool OnlineTitleNames::isTitleIdFallback(const std::string& title, std::uint64_t titleId) {
    return title == hexId(titleId);
}

long OnlineTitleNames::today() {
    return static_cast<long>(std::time(nullptr) / 86400);
}

void OnlineTitleNames::load() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_entries.clear();
    std::ifstream input(kStorePath);
    if (!input) return;
    try {
        const auto json = nlohmann::json::parse(input);
        const auto titles = json.find("titles");
        if (titles == json.end() || !titles->is_object()) return;
        for (const auto& [key, value] : titles->items()) {
            if (!value.is_object()) continue;
            const std::uint64_t titleId = std::strtoull(key.c_str(), nullptr, 16);
            if (titleId == 0) continue;
            Entry entry;
            entry.name = value.value("name", std::string());
            entry.checked = value.value("checked", 0L);
            m_entries[titleId] = std::move(entry);
        }
    } catch (const std::exception& ex) {
        // A damaged store only costs the lookups it held.
        DebugLog::log("[titlenames] store unreadable, starting empty: %s", ex.what());
        m_entries.clear();
    }
}

bool OnlineTitleNames::save() const {
    nlohmann::json titles = nlohmann::json::object();
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (const auto& [titleId, entry] : m_entries)
            titles[hexId(titleId)] = {{"name", entry.name}, {"checked", entry.checked}};
    }
    const nlohmann::json json = {{"version", 1}, {"titles", std::move(titles)}};

    const std::string staging = std::string(kStorePath) + ".tmp";
    {
        std::ofstream output(staging, std::ios::trunc);
        if (!output) return false;
        output << json.dump(2);
        if (!output) return false;
    }
    std::remove(kStorePath);
    return std::rename(staging.c_str(), kStorePath) == 0;
}

std::string OnlineTitleNames::name(std::uint64_t titleId) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto it = m_entries.find(titleId);
    return it == m_entries.end() ? std::string() : it->second.name;
}

bool OnlineTitleNames::due(std::uint64_t titleId, long today) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto it = m_entries.find(titleId);
    if (it == m_entries.end()) return true;
    return it->second.name.empty() && today - it->second.checked >= kRetryMissAfterDays;
}

void OnlineTitleNames::record(std::uint64_t titleId, const std::string& name, long today) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto& entry = m_entries[titleId];
    entry.name = name;
    entry.checked = today;
}

OnlineTitleNames::Lookup OnlineTitleNames::fetch(std::uint64_t titleId) {
    Lookup lookup;
    lookup.titleId = titleId;
    const std::string url = std::string(kServiceUrl) + hexId(titleId) + "?fields=name";
    try {
        const auto json = nlohmann::json::parse(themeshop::http::getText(url));
        std::string name = json.is_object()
            ? trimmed(json.value("name", std::string())) : std::string();
        if (name.size() > kMaxNameLength) name.clear();
        if (name.empty()) {
            lookup.status = LookupStatus::Unknown;
        } else {
            lookup.status = LookupStatus::Found;
            lookup.name = std::move(name);
        }
    } catch (const std::exception& ex) {
        // The service answers 404 for a title it does not know, which is an
        // answer; anything else -- no connection, a timeout -- is not, and
        // leaves the title to be asked about next time.
        const std::string message = ex.what();
        lookup.status = message.find("HTTP error 404") != std::string::npos
            ? LookupStatus::Unknown : LookupStatus::Failed;
        if (lookup.status == LookupStatus::Failed)
            DebugLog::log("[titlenames] %s lookup failed: %s", hexId(titleId).c_str(), ex.what());
    }
    return lookup;
}
