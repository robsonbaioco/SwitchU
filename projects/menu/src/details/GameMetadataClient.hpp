#pragma once

#include <cstdint>
#include <future>
#include <mutex>
#include <string>
#include <vector>

namespace nxui {
class ThreadPool;
}

// The dossier's online facts, gathered on the console from public sources.
//
// This used to be one call to ncarvalho99's SwitchU API, which merged IGDB,
// Metacritic and HowLongToBeat on his server. Since 2026-09-21 that service
// answers only his own builds ("Official client required"), so the fork asks
// the sources directly:
//   - nlib (api.nlib.cc), by title id, no key: name, description, publisher,
//     developer, release date, genres, player count, screenshots. Native
//     Switch titles only.
//   - RAWG, with the player's own key: metascore, and an average playtime
//     that stands in for "main" when IGDB has none. Also the fallback for
//     game ports, which nlib does not know.
//   - IGDB, with the player's own Twitch application: time to beat (hastily,
//     normally, completely).
// Each source is optional and fails on its own; the dossier shows what came
// back. There is no source for a user score any more.
class GameMetadataClient {
public:
    enum class Phase { Idle, Loading, Ready, Failed };

    struct Keys {
        std::string rawgApiKey;
        std::string igdbClientId;
        std::string igdbClientSecret;
    };

    struct Snapshot {
        Phase phase = Phase::Idle;
        bool found = false;
        std::string title;
        std::string summary;
        std::string storyline;
        std::string releaseDate;
        std::vector<std::string> developers;
        std::vector<std::string> publishers;
        std::vector<std::string> genres;
        std::vector<std::string> themes;
        std::vector<std::string> gameModes;
        std::vector<std::string> screenshots;
        float hoursHastily = 0.f;
        float hoursMain = 0.f;
        float hoursCompletionist = 0.f;
        int metascore = -1;
        float userscore = -1.f;
        std::string error;
        std::uint64_t revision = 0;
    };

    void setKeys(Keys keys);
    // titleId is used for native Switch titles (platform "nintendo-switch");
    // title is the search term for RAWG and IGDB.
    void load(nxui::ThreadPool& pool, std::uint64_t titleId, std::string title,
              std::string platform);
    Snapshot snapshot() const;

private:
    static Snapshot fetch(std::uint64_t titleId, std::string title, std::string platform,
                          Keys keys, std::uint64_t revision);

    mutable std::mutex m_mutex;
    Snapshot m_snapshot;
    Keys m_keys;
    std::future<void> m_future;
    std::uint64_t m_revision = 0;
};
