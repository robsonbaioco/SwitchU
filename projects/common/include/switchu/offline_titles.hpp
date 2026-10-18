#pragma once
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace switchu::offline_titles {

// Titles the owner wants to play with the console's internet off. The menu
// edits the list from the dossier and the daemon reads it on every launch, so
// it lives in its own file rather than in the menu's config.json: the daemon
// does not parse JSON, and one hex title id per line cannot be half-read.
inline constexpr const char* kPath = "sdmc:/config/SwitchU/offline_titles.txt";

inline std::vector<std::uint64_t> load() {
    std::vector<std::uint64_t> ids;
    FILE* f = std::fopen(kPath, "r");
    if (!f) return ids;
    char line[64];
    while (std::fgets(line, sizeof(line), f)) {
        std::uint64_t id = 0;
        if (std::sscanf(line, "%" SCNx64, &id) == 1 && id != 0)
            ids.push_back(id);
    }
    std::fclose(f);
    return ids;
}

inline bool contains(std::uint64_t titleId) {
    for (std::uint64_t id : load())
        if (id == titleId) return true;
    return false;
}

inline bool save(const std::vector<std::uint64_t>& ids) {
    FILE* f = std::fopen(kPath, "w");
    if (!f) return false;
    for (std::uint64_t id : ids)
        std::fprintf(f, "%016" PRIX64 "\n", id);
    return std::fclose(f) == 0;
}

} // namespace switchu::offline_titles
