#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>

// Names for titles the console cannot name.
//
// Some releases -- downgraded builds in particular -- carry no usable NACP
// name, so the grid shows their title id and the SteamGridDB search, which
// goes by name, finds nothing for them. The nlib title API answers by title
// id, so each such title is asked about once and the answer kept on the card.
// A name the owner typed (AppConfig::customTitles) still wins over it.
//
// Only titles the console left unnamed are ever sent, and one request per title
// for as long as the answer is kept. Misses are asked again after a while, in
// case the service learns the title later.
class OnlineTitleNames {
public:
    static constexpr const char* kStorePath = "sdmc:/config/SwitchU/title_names.json";
    static constexpr const char* kServiceUrl = "https://api.nlib.cc/nx/";
    static constexpr long kRetryMissAfterDays = 30;

    enum class LookupStatus { Found, Unknown, Failed };
    struct Lookup {
        std::uint64_t titleId = 0;
        LookupStatus status = LookupStatus::Failed;
        std::string name;
    };

    void load();
    bool save() const;

    // The name found for this title, or empty when there is none.
    std::string name(std::uint64_t titleId) const;
    // True when the title has never been asked about, or was not known the
    // last time and that answer is old enough to ask again.
    bool due(std::uint64_t titleId, long today) const;
    // name empty records a miss.
    void record(std::uint64_t titleId, const std::string& name, long today);

    // Blocking network call; run it off the main thread.
    static Lookup fetch(std::uint64_t titleId);
    // What AppListLoader shows when a title has no name: its id in upper hex.
    static bool isTitleIdFallback(const std::string& title, std::uint64_t titleId);
    static long today();

private:
    struct Entry {
        std::string name;
        long checked = 0;
    };

    // Read by composeRootPending() on the catalogue thread while the main
    // thread records answers.
    mutable std::mutex m_mutex;
    std::unordered_map<std::uint64_t, Entry> m_entries;
};
