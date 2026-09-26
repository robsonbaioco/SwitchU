#include "GameMetadataClient.hpp"

#include "core/DebugLog.hpp"
#include "themeshop/ThemeHttp.hpp"

#include <nlohmann/json.hpp>
#include <nxui/core/I18n.hpp>
#include <nxui/core/ThreadPool.hpp>
#include <switch.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <set>
#include <stdexcept>

namespace {

constexpr const char* kNlibBase = "https://api.nlib.cc/nx/";
constexpr const char* kRawgGames = "https://api.rawg.io/api/games";
constexpr const char* kTwitchToken = "https://id.twitch.tv/oauth2/token";
constexpr const char* kIgdbGames = "https://api.igdb.com/v4/games";
constexpr const char* kIgdbTimeToBeat = "https://api.igdb.com/v4/game_time_to_beats";

// Below this, a search result is a different game that shares words.
constexpr float kMinimumMatch = 0.75f;

std::string encodeUrlComponent(const std::string& text) {
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(text.size() * 3);
    for (unsigned char ch : text) {
        if (std::isalnum(ch) || ch == '-' || ch == '_' || ch == '.' || ch == '~') {
            out.push_back((char)ch);
        } else {
            out += '%';
            out += hex[ch >> 4];
            out += hex[ch & 0x0F];
        }
    }
    return out;
}

bool isHttpsUrl(const std::string& value) {
    return value.rfind("https://", 0) == 0 && value.size() <= 2048;
}

std::string readString(const nlohmann::json& json, const char* key,
                       const std::string& fallback = std::string()) {
    // Sources send an explicit null for fields a game does not have.
    // json::value() only falls back when the key is absent, so a present null
    // would throw and fail the whole dossier instead of leaving one field empty.
    const auto it = json.find(key);
    if (it == json.end() || !it->is_string()) return fallback;
    return it->get<std::string>();
}

float readNumber(const nlohmann::json& json, const char* key, float fallback = 0.f) {
    const auto it = json.find(key);
    if (it == json.end() || !it->is_number()) return fallback;
    const float value = it->get<float>();
    return std::isfinite(value) ? value : fallback;
}

// nlib names carry the trademark signs from the eShop listing.
std::string withoutMarks(std::string name) {
    for (const char* mark : {"\xE2\x84\xA2", "\xC2\xAE", "\xC2\xA9"}) {
        for (std::size_t at; (at = name.find(mark)) != std::string::npos;)
            name.erase(at, std::char_traits<char>::length(mark));
    }
    return name;
}

std::string normalized(const std::string& value) {
    std::string out;
    for (unsigned char c : value)
        if (std::isalnum(c)) out.push_back(static_cast<char>(std::tolower(c)));
    return out;
}

std::set<std::string> tokens(const std::string& value) {
    std::set<std::string> out;
    std::string token;
    for (unsigned char c : value) {
        if (std::isalnum(c)) {
            token.push_back(static_cast<char>(std::tolower(c)));
        } else if (!token.empty()) {
            out.insert(std::move(token));
            token.clear();
        }
    }
    if (!token.empty()) out.insert(std::move(token));
    return out;
}

// The same idea as the SteamGridDB matcher: exact after normalisation, or most
// of the words shared, or one title containing most of the other.
float similarity(const std::string& wanted, const std::string& candidate) {
    const std::string a = normalized(wanted);
    const std::string b = normalized(candidate);
    if (a.empty() || b.empty()) return 0.f;
    if (a == b) return 1.f;
    const auto ta = tokens(wanted);
    const auto tb = tokens(candidate);
    int shared = 0;
    for (const auto& t : ta) shared += tb.count(t) ? 1 : 0;
    const int unionCount = static_cast<int>(ta.size() + tb.size()) - shared;
    const int smaller = static_cast<int>(std::min(ta.size(), tb.size()));
    const float coverage = smaller > 0 ? static_cast<float>(shared) / smaller : 0.f;
    const float jaccard = unionCount > 0 ? static_cast<float>(shared) / unionCount : 0.f;
    const float score = coverage * 0.65f + jaccard * 0.35f;
    const bool contained = a.find(b) != std::string::npos || b.find(a) != std::string::npos;
    const float ratio = static_cast<float>(std::min(a.size(), b.size()))
        / static_cast<float>(std::max(a.size(), b.size()));
    return contained && ratio >= 0.68f ? std::max(score, 0.84f) : score;
}

int igdbPlatform(const std::string& slug) {
    static const std::pair<const char*, int> ids[] = {
        {"nintendo-switch", 130}, {"pc", 6}, {"playstation-5", 167}, {"playstation-4", 48},
        {"xbox-series-x", 169}, {"xbox-one", 49}, {"xbox-360", 12}, {"playstation-3", 9},
        {"playstation-2", 8}, {"playstation", 7}, {"gamecube", 21}, {"wii", 5}, {"wii-u", 41},
        {"nintendo-64", 4}, {"super-nintendo", 19}, {"nes", 18}, {"game-boy-advance", 24},
        {"nintendo-ds", 20}, {"nintendo-3ds", 37}, {"psp", 38}, {"playstation-vita", 46},
        {"dreamcast", 23}, {"sega-genesis", 29},
    };
    for (const auto& [name, id] : ids)
        if (slug == name) return id;
    return 0;
}

bool isNotFound(const std::exception& ex) {
    return std::string(ex.what()).find("HTTP error 404") != std::string::npos;
}

// ---- nlib ------------------------------------------------------------------

// true when nlib knew the title.
bool fillFromNlib(std::uint64_t titleId, GameMetadataClient::Snapshot& out) {
    char id[17]{};
    std::snprintf(id, sizeof(id), "%016llX", static_cast<unsigned long long>(titleId));
    nlohmann::json json;
    try {
        json = nlohmann::json::parse(themeshop::http::getText(
            std::string(kNlibBase) + id
            + "?fields=name,description,publisher,developer,releaseDate,category,"
              "numberOfPlayers,screens"));
    } catch (const std::exception& ex) {
        if (isNotFound(ex)) return false;
        throw;
    }
    if (!json.is_object()) return false;

    const std::string name = withoutMarks(readString(json, "name"));
    if (name.empty()) return false;
    out.title = name;
    out.summary = readString(json, "description");
    out.releaseDate = readString(json, "releaseDate");
    if (const std::string publisher = readString(json, "publisher"); !publisher.empty())
        out.publishers.push_back(publisher);
    if (const std::string developer = readString(json, "developer"); !developer.empty())
        out.developers.push_back(developer);
    if (const auto category = json.find("category");
        category != json.end() && category->is_array()) {
        for (const auto& genre : *category)
            if (genre.is_string() && genre.get<std::string>() != "Other")
                out.genres.push_back(genre.get<std::string>());
    }
    const int players = static_cast<int>(readNumber(json, "numberOfPlayers", 0.f));
    auto& i18n = nxui::I18n::instance();
    if (players == 1)
        out.gameModes.push_back(i18n.tr("dialog.details_single_player", "Single player"));
    else if (players > 1)
        out.gameModes.push_back(i18n.tr("dialog.details_multiplayer", "Multiplayer")
                                + " (" + std::to_string(players) + ")");
    if (const auto screens = json.find("screens"); screens != json.end() && screens->is_object()) {
        if (const auto list = screens->find("screenshots");
            list != screens->end() && list->is_array()) {
            for (const auto& value : *list) {
                if (!value.is_string()) continue;
                std::string url = value.get<std::string>();
                // nlib lists them as http://; the same paths answer over https.
                if (url.rfind("http://", 0) == 0) url.replace(0, 7, "https://");
                if (isHttpsUrl(url)) out.screenshots.push_back(url);
            }
        }
    }
    return true;
}

// ---- RAWG ------------------------------------------------------------------

bool fillFromRawg(const std::string& key, const std::string& title, const std::string& platform,
                  bool nlibFound, GameMetadataClient::Snapshot& out) {
    std::string url = std::string(kRawgGames) + "?key=" + encodeUrlComponent(key)
        + "&search=" + encodeUrlComponent(title) + "&page_size=10";
    // RAWG's platform id for Switch. Ports search every platform and rely on
    // the name match.
    if (platform == "nintendo-switch") url += "&platforms=7";
    const auto json = nlohmann::json::parse(themeshop::http::getText(url));
    const auto results = json.find("results");
    if (results == json.end() || !results->is_array()) return false;

    const nlohmann::json* best = nullptr;
    float bestScore = 0.f;
    for (const auto& game : *results) {
        if (!game.is_object()) continue;
        const float score = similarity(title, readString(game, "name"));
        if (score > bestScore) { bestScore = score; best = &game; }
    }
    if (!best || bestScore < kMinimumMatch) {
        DebugLog::log("[metadata] rawg: no close match for '%s' (best %.2f)",
                      title.c_str(), bestScore);
        return false;
    }
    const float metacritic = readNumber(*best, "metacritic", -1.f);
    if (metacritic >= 0.f && metacritic <= 100.f)
        out.metascore = static_cast<int>(std::lround(metacritic));
    const float playtime = readNumber(*best, "playtime", 0.f);
    if (playtime > 0.f && out.hoursMain <= 0.f)
        out.hoursMain = playtime;
    if (!nlibFound) {
        out.title = readString(*best, "name", title);
        out.releaseDate = readString(*best, "released");
        if (const auto genres = best->find("genres"); genres != best->end() && genres->is_array())
            for (const auto& genre : *genres)
                if (genre.is_object())
                    if (std::string name = readString(genre, "name"); !name.empty())
                        out.genres.push_back(std::move(name));
    }
    return true;
}

// ---- IGDB ------------------------------------------------------------------

std::mutex g_tokenMutex;
std::string g_tokenClientId;
std::string g_token;
u64 g_tokenExpiresTick = 0;

std::string igdbToken(const std::string& clientId, const std::string& secret, bool refresh) {
    std::lock_guard<std::mutex> lock(g_tokenMutex);
    const u64 now = armGetSystemTick();
    if (!refresh && !g_token.empty() && g_tokenClientId == clientId && now < g_tokenExpiresTick)
        return g_token;
    // In the body rather than the query string, which the HTTP log would print.
    const std::string body = "client_id=" + encodeUrlComponent(clientId)
        + "&client_secret=" + encodeUrlComponent(secret)
        + "&grant_type=client_credentials";
    const auto json = nlohmann::json::parse(themeshop::http::postText(kTwitchToken, body));
    g_token = readString(json, "access_token");
    if (g_token.empty())
        throw std::runtime_error("Twitch did not return an access token");
    g_tokenClientId = clientId;
    // Renewed a minute early.
    const float seconds = std::max(60.f, readNumber(json, "expires_in", 3600.f) - 60.f);
    g_tokenExpiresTick = now + armNsToTicks(static_cast<u64>(seconds) * 1'000'000'000ULL);
    return g_token;
}

nlohmann::json igdbQuery(const GameMetadataClient::Keys& keys, const char* endpoint,
                         const std::string& query) {
    for (int attempt = 0; attempt < 2; ++attempt) {
        const std::string token = igdbToken(keys.igdbClientId, keys.igdbClientSecret, attempt > 0);
        try {
            return nlohmann::json::parse(themeshop::http::postText(endpoint, query, {
                "Client-ID: " + keys.igdbClientId,
                "Authorization: Bearer " + token,
            }));
        } catch (const std::exception& ex) {
            // A revoked or expired token: fetch a new one once.
            if (attempt == 0 && std::string(ex.what()).find("HTTP error 401") != std::string::npos)
                continue;
            throw;
        }
    }
    return {};
}

bool fillFromIgdb(const GameMetadataClient::Keys& keys, const std::string& title,
                  const std::string& platform, GameMetadataClient::Snapshot& out) {
    std::string term;
    for (char c : title) if (c != '"' && c != '\\') term.push_back(c);
    std::string query = "search \"" + term + "\"; fields id,name; limit 10;";
    if (const int id = igdbPlatform(platform); id > 0)
        query = "search \"" + term + "\"; fields id,name; where platforms = ("
              + std::to_string(id) + "); limit 10;";
    const auto games = igdbQuery(keys, kIgdbGames, query);
    if (!games.is_array()) return false;

    long long bestId = 0;
    float bestScore = 0.f;
    std::string bestName;
    for (const auto& game : games) {
        if (!game.is_object() || !game.contains("id") || !game["id"].is_number_integer()) continue;
        const std::string name = readString(game, "name");
        const float score = similarity(title, name);
        if (score > bestScore) { bestScore = score; bestId = game["id"].get<long long>(); bestName = name; }
    }
    if (bestId == 0 || bestScore < kMinimumMatch) {
        DebugLog::log("[metadata] igdb: no close match for '%s' (best %.2f)",
                      title.c_str(), bestScore);
        return false;
    }
    const auto times = igdbQuery(keys, kIgdbTimeToBeat,
        "fields hastily,normally,completely; where game_id = " + std::to_string(bestId)
        + "; limit 1;");
    if (!times.is_array() || times.empty() || !times[0].is_object())
        return true;   // the game is known, it just has no time-to-beat data
    // IGDB counts seconds.
    const auto hours = [&](const char* key) { return readNumber(times[0], key, 0.f) / 3600.f; };
    if (const float h = hours("hastily"); h > 0.f) out.hoursHastily = h;
    if (const float h = hours("normally"); h > 0.f) out.hoursMain = h;
    if (const float h = hours("completely"); h > 0.f) out.hoursCompletionist = h;
    if (out.title.empty()) out.title = bestName;
    return true;
}

} // namespace

void GameMetadataClient::setKeys(Keys keys) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_keys = std::move(keys);
}

void GameMetadataClient::load(nxui::ThreadPool& pool, std::uint64_t titleId, std::string title,
                              std::string platform) {
    std::lock_guard<std::mutex> lock(m_mutex);
    const std::uint64_t revision = ++m_revision;
    m_snapshot = {};
    m_snapshot.phase = Phase::Loading;
    m_snapshot.revision = revision;
    m_future = pool.submit([this, titleId, title = std::move(title),
                            platform = std::move(platform), keys = m_keys, revision]() {
        Snapshot next;
        try {
            next = fetch(titleId, title, platform, keys, revision);
        } catch (const std::exception& ex) {
            next.phase = Phase::Failed;
            next.error = ex.what();
            next.revision = revision;
        } catch (...) {
            next.phase = Phase::Failed;
            next.error = "Unknown metadata error";
            next.revision = revision;
        }
        std::lock_guard<std::mutex> resultLock(m_mutex);
        if (revision == m_revision)
            m_snapshot = std::move(next);
    });
}

GameMetadataClient::Snapshot GameMetadataClient::snapshot() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_snapshot;
}

GameMetadataClient::Snapshot GameMetadataClient::fetch(std::uint64_t titleId, std::string title,
                                                       std::string platform, Keys keys,
                                                       std::uint64_t revision) {
    Snapshot result;
    result.phase = Phase::Ready;
    result.revision = revision;

    // Every source is tried on its own. The dossier fails only when none of
    // them could be reached; a source that answered "not found" is an answer.
    bool answered = false;
    std::string firstError;
    const auto attempt = [&](const char* source, auto&& call) {
        try {
            const bool found = call();
            answered = true;
            result.found = result.found || found;
        } catch (const std::exception& ex) {
            DebugLog::log("[metadata] %s failed for '%s': %s", source, title.c_str(), ex.what());
            if (firstError.empty()) firstError = ex.what();
        }
    };

    bool nlibFound = false;
    if (platform == "nintendo-switch" && titleId != 0)
        attempt("nlib", [&] { return nlibFound = fillFromNlib(titleId, result); });
    // The console's own name is often worse than nlib's (a hex id, an
    // abbreviation); search the others with the better one.
    const std::string searchTitle = nlibFound && !result.title.empty() ? result.title : title;
    if (!keys.rawgApiKey.empty())
        attempt("rawg", [&] { return fillFromRawg(keys.rawgApiKey, searchTitle, platform,
                                                  nlibFound, result); });
    if (!keys.igdbClientId.empty() && !keys.igdbClientSecret.empty())
        attempt("igdb", [&] { return fillFromIgdb(keys, searchTitle, platform, result); });

    if (!answered && !firstError.empty())
        throw std::runtime_error(firstError);
    if (result.title.empty())
        result.title = title;
    return result;
}
