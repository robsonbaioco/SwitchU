#include "SteamGridDbManager.hpp"

#include "ArtworkCache.hpp"
#include "core/DebugLog.hpp"
#include "gallery/GameGalleryClient.hpp"
#include "themeshop/ThemeHttp.hpp"

#include <nxui/core/I18n.hpp>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <list>
#include <nlohmann/json.hpp>
#include <set>
#include <system_error>

namespace {

constexpr const char* kApiBase = "https://www.steamgriddb.com/api/v2";
constexpr int kMatcherVersion = 3; // v3 always matches against the cached English title.

struct ArtworkKindInfo {
    const char* endpoint = "/icons";
    const char* label = "Icon";
    const char* flag = "icon";
    const char* idKey = "selectedIconId";
    const char* indexKey = "selectedIconIndex";
    // Appended after nsfw/humor (and optional types=static).
    const char* querySuffix = "&mimes=image/png";
    bool useStaticTypes = false;
    bool requireLandscape = false;
    bool requirePortrait = false;
    bool prepareCache = false;
    int prepW = 0;
    int prepH = 0;
};

ArtworkKindInfo kindInfo(SteamGridDbManager::ArtworkKind kind) {
    using Kind = SteamGridDbManager::ArtworkKind;
    switch (kind) {
    case Kind::Hero:
        // Cap to ~1080p-class heroes (skip 3840x1240). UI cards use 640x360.
        return {"/heroes", "Hero", "hero", "selectedHeroId", "selectedHeroIndex",
                "&mimes=image/png,image/jpeg&dimensions=1920x620,1600x650",
                true, true, false, true, 640, 360};
    case Kind::Logo:
        // Logos have no SGDB dimension enum; PNG for alpha. UI uses 384x192.
        return {"/logos", "Logo", "logo", "selectedLogoId", "selectedLogoIndex",
                "&mimes=image/png", true, false, false, true, 384, 192};
    case Kind::Cover: // == Kind::Poster
        // SteamGridDB "grids", portrait 600x900 only: the same file feeds the
        // fork's Cover/Shelf/Flow views (decoded at up to 720px) and PoloNX's
        // single-row poster tiles. prepW/H are the IconStreamer fit box (see
        // prepareFit), not letterboxed. The "cover" flag matches the metadata
        // the fork's scan already wrote.
        return {"/grids", "Cover", "cover", "selectedCoverId", "selectedCoverIndex",
                "&mimes=image/png,image/jpeg&dimensions=600x900",
                true, false, true, true, 320, 480};
    case Kind::GridWide:
        // Landscape SGDB grids for 2x1 / 4x2 HOME tiles (separate from Hero+Logo).
        return {"/grids", "Poster wide", "gridWide", "selectedGridWideId",
                "selectedGridWideIndex",
                "&mimes=image/png,image/jpeg&dimensions=460x215,920x430",
                true, true, false, true, 460, 215};
    case Kind::Icon:
    default:
        // Icons are PNG-only on SGDB. Grid icons draw at 160px; prefer 256/512,
        // keep 1024 so games without smaller uploads still appear (ranked down).
        return {"/icons", "Icon", "icon", "selectedIconId", "selectedIconIndex",
                "&mimes=image/png&dimensions=256,512,1024",
                false, false, false, false, 0, 0};
    }
}

std::string pathForKind(SteamGridDbManager::ArtworkKind kind, std::uint64_t titleId) {
    using Kind = SteamGridDbManager::ArtworkKind;
    switch (kind) {
    case Kind::Hero:     return SteamGridDbManager::heroPath(titleId);
    case Kind::Logo:     return SteamGridDbManager::logoPath(titleId);
    case Kind::Cover:    return SteamGridDbManager::coverPath(titleId); // == Poster
    case Kind::GridWide: return SteamGridDbManager::gridWidePath(titleId);
    case Kind::Icon:
    default:             return SteamGridDbManager::iconPath(titleId);
    }
}

std::string titleDirectory(std::uint64_t titleId);

// Metadata always lives in the title directory; covers live in their own
// folder (sdmc:/config/SwitchU/covers/<TID16>.jpg), shared with the views.
void ensureArtworkDirectories(SteamGridDbManager::ArtworkKind kind, std::uint64_t titleId) {
    std::error_code ec;
    std::filesystem::create_directories(titleDirectory(titleId), ec);
    if (kind == SteamGridDbManager::ArtworkKind::Cover) {
        ec.clear();
        std::filesystem::create_directories(SteamGridDbManager::kCoverRoot, ec);
    }
}

bool looksLikeHttpsClockFailure(const std::string& error) {
    if (error.empty())
        return true;
    std::string lower = error;
    for (char& c : lower)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return lower.find("ssl") != std::string::npos
        || lower.find("tls") != std::string::npos
        || lower.find("certificate") != std::string::npos
        || lower.find("cert verify") != std::string::npos
        || lower.find("peer certificate") != std::string::npos
        || lower.find("certificate has expired") != std::string::npos
        || lower.find("https failed") != std::string::npos;
}

// Wrong console clocks break TLS to SteamGridDB (PoloNX #131) and used to surface as a blank
// picker error (empty curl what()). Point people at System clock sync.
std::string friendlySteamGridDbTransportError(const std::string& error) {
    if (!looksLikeHttpsClockFailure(error))
        return error.empty() ? "SteamGridDB search failed." : error;
    return nxui::I18n::instance().tr(
        "settings.steamgriddb.https_clock_hint",
        "HTTPS failed. If the console clock is wrong, fix it in Settings > System "
        "(Synchronize Clock via Internet, or set the date and time yourself).");
}

std::string titleDirectory(std::uint64_t titleId) {
    char id[17]{};
    std::snprintf(id, sizeof(id), "%016llX", static_cast<unsigned long long>(titleId));
    return std::string(SteamGridDbManager::kCacheRoot) + "/" + id;
}

std::string percentEncode(const std::string& value) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(value.size() * 2);
    for (unsigned char c : value) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(kHex[c >> 4]);
            out.push_back(kHex[c & 15]);
        }
    }
    return out;
}

std::string normalized(std::string value) {
    std::string out;
    out.reserve(value.size());
    for (unsigned char c : value) {
        if (std::isalnum(c))
            out.push_back(static_cast<char>(std::tolower(c)));
    }
    return out;
}

std::set<std::string> titleTokens(const std::string& value) {
    std::set<std::string> tokens;
    std::string token;
    for (unsigned char c : value) {
        if (std::isalnum(c)) {
            token.push_back(static_cast<char>(std::tolower(c)));
        } else if (!token.empty()) {
            tokens.insert(std::move(token));
            token.clear();
        }
    }
    if (!token.empty()) tokens.insert(std::move(token));
    return tokens;
}

float titleSimilarity(const std::string& requested, const std::string& candidate) {
    const std::string a = normalized(requested);
    const std::string b = normalized(candidate);
    if (a.empty() || b.empty()) return 0.f;
    if (a == b) return 1.f;

    const auto aTokens = titleTokens(requested);
    const auto bTokens = titleTokens(candidate);
    if (aTokens.empty() || bTokens.empty()) return 0.f;

    int intersection = 0;
    for (const auto& token : aTokens)
        if (bTokens.count(token)) ++intersection;
    const int unionCount = static_cast<int>(aTokens.size() + bTokens.size()) - intersection;
    const int smallerCount = static_cast<int>(std::min(aTokens.size(), bTokens.size()));
    const float jaccard = unionCount > 0 ? static_cast<float>(intersection) / unionCount : 0.f;
    const float coverage = smallerCount > 0 ? static_cast<float>(intersection) / smallerCount : 0.f;

    // Containment helps with harmless edition/subtitle suffixes, but only when
    // the shorter normalized title still represents most of the longer one.
    const std::size_t shorter = std::min(a.size(), b.size());
    const std::size_t longer = std::max(a.size(), b.size());
    const bool contained = a.find(b) != std::string::npos || b.find(a) != std::string::npos;
    const float containmentRatio = longer > 0 ? static_cast<float>(shorter) / longer : 0.f;
    const float tokenScore = coverage * 0.65f + jaccard * 0.35f;
    return contained && containmentRatio >= 0.68f
        ? std::max(tokenScore, 0.84f)
        : tokenScore;
}

nlohmann::json apiData(const std::string& url, const std::list<std::string>& headers) {
    auto body = themeshop::http::getText(url, headers);
    auto json = nlohmann::json::parse(body);
    if (!json.value("success", false)) {
        std::string error = "SteamGridDB request failed";
        if (json.contains("errors") && json["errors"].is_array() && !json["errors"].empty())
            error = json["errors"].front().get<std::string>();
        throw std::runtime_error(error);
    }
    return json.value("data", nlohmann::json::array());
}

// This fork reaches SteamGridDB through its own gallery service, which needs no
// personal key. Upstream 1.2 talks to the public API directly and therefore
// requires one; keeping only that made artwork unavailable to every user who
// had not registered an account. The proxy is the default and the direct API is
// used only when the player has configured a key, which also unlocks logos: the
// proxy exposes searches, heroes and grids, and has no logo endpoint.
constexpr const char* kProxyBase = GameGalleryClient::kServiceUrl;

nlohmann::json proxyData(const std::string& url, const char* member,
                         const std::list<std::string>& headers) {
    auto body = themeshop::http::getText(url, headers);
    auto json = nlohmann::json::parse(body);
    const auto it = json.find(member);
    if (it == json.end() || !it->is_array())
        throw std::runtime_error("Artwork service response is invalid");
    return *it;
}

struct Backend {
    bool proxy = true;
    std::list<std::string> headers;
};

Backend makeBackend(const std::string& apiKey) {
    Backend backend;
    backend.proxy = apiKey.empty();
    backend.headers.push_back("Accept: application/json");
    if (!backend.proxy)
        backend.headers.push_front("Authorization: Bearer " + apiKey);
    return backend;
}

nlohmann::json searchGames(const Backend& backend, const std::string& query) {
    if (!backend.proxy)
        return apiData(std::string(kApiBase) + "/search/autocomplete/" + percentEncode(query),
                       backend.headers);
    return proxyData(std::string(kProxyBase) + "/v1/search?query=" + percentEncode(query),
                     "games", backend.headers);
}

nlohmann::json listArtwork(const Backend& backend, long long gameId,
                           SteamGridDbManager::ArtworkKind kind) {
    using ArtworkKind = SteamGridDbManager::ArtworkKind;
    if (!backend.proxy) {
        // Endpoint, static-only flag, MIME and dimension filters per kind
        // (covers stay 600x900; icons ask for 256/512/1024).
        const auto info = kindInfo(kind);
        std::string url = std::string(kApiBase) + info.endpoint + "/game/"
                        + std::to_string(gameId) + "?nsfw=false&humor=false";
        if (info.useStaticTypes)
            url += "&types=static";
        url += info.querySuffix;
        return apiData(url, backend.headers);
    }
    if (kind == ArtworkKind::Logo)
        throw std::runtime_error("Logos need a SteamGridDB API key");
    const char* member = kind == ArtworkKind::Hero ? "heroes" : "grids";
    std::string url = std::string(kProxyBase) + "/v1/games/" + std::to_string(gameId) + "/"
                    + member;
    // Icons are drawn in a square tile, so ask the grid endpoint for the square
    // dimension instead of the default 600x900 portrait covers.
    if (kind == ArtworkKind::Icon)
        url += "?dimensions=1024x1024";
    else if (kind == ArtworkKind::Cover)
        url += "?dimensions=600x900";
    else if (kind == ArtworkKind::GridWide)
        url += "?dimensions=460x215,920x430";
    return proxyData(url, member, backend.headers);
}

const nlohmann::json* chooseGame(const nlohmann::json& games, const std::string& title,
                                 float& outScore, std::string& outClosest,
                                 bool& outAmbiguous) {
    constexpr float kMinimumMatchScore = 0.78f;
    outScore = 0.f;
    outClosest.clear();
    outAmbiguous = false;
    if (!games.is_array() || games.empty()) return nullptr;
    const nlohmann::json* best = nullptr;
    float secondBestScore = 0.f;
    for (const auto& game : games) {
        if (!game.is_object() || !game.contains("id")) continue;
        float score = titleSimilarity(title, game.value("name", std::string()));
        if (game.value("verified", false) && score < 1.f)
            score = std::min(1.f, score + 0.02f);
        if (!best || score > outScore) {
            secondBestScore = outScore;
            best = &game;
            outScore = score;
            outClosest = game.value("name", std::string());
        } else if (score > secondBestScore) {
            secondBestScore = score;
        }
    }
    outAmbiguous = outScore < 0.999f && secondBestScore >= outScore - 0.04f;
    if (outAmbiguous) return nullptr;
    return outScore >= kMinimumMatchScore ? best : nullptr;
}

bool cacheUsesCurrentMatcher(std::uint64_t titleId) {
    std::ifstream input(titleDirectory(titleId) + "/metadata.json");
    if (!input.is_open()) return false;
    try {
        nlohmann::json metadata;
        input >> metadata;
        const bool manuallySelected = metadata.contains("selectedHeroId")
                                   || metadata.contains("selectedLogoId");
        return manuallySelected
            || metadata.value("matcherVersion", 0) == kMatcherVersion;
    } catch (...) {
        return false;
    }
}

bool artworkWasChosenByHand(std::uint64_t titleId) {
    std::ifstream input(titleDirectory(titleId) + "/metadata.json");
    if (!input.is_open()) return false;
    try {
        nlohmann::json metadata;
        input >> metadata;
        return metadata.contains("selectedHeroId")
            || metadata.contains("selectedLogoId");
    } catch (...) {
        return false;
    }
}

void removeCachedArtwork(std::uint64_t titleId) {
    std::error_code ec;
    const std::string hero = SteamGridDbManager::heroPath(titleId);
    const std::string logo = SteamGridDbManager::logoPath(titleId);
    const std::string gridWide = SteamGridDbManager::gridWidePath(titleId);
    std::filesystem::remove(hero, ec);
    ec.clear();
    std::filesystem::remove(logo, ec);
    ec.clear();
    std::filesystem::remove(gridWide, ec);
    ec.clear();
    std::filesystem::remove(hero + ".640x360.rgba-cache", ec);
    ec.clear();
    std::filesystem::remove(hero + ".1280x720.rgba-cache", ec); // legacy prep size
    ec.clear();
    std::filesystem::remove(logo + ".384x192.rgba-cache", ec);
    ec.clear();
    std::filesystem::remove(logo + ".640x180.rgba-cache", ec); // legacy prep size
    ec.clear();
    std::filesystem::remove(gridWide + ".460x215.rgba-cache", ec);
    ec.clear();
    std::filesystem::remove(titleDirectory(titleId) + "/metadata.json", ec);
}

const nlohmann::json* chooseImage(const nlohmann::json& images, bool portrait) {
    if (!images.is_array()) return nullptr;
    const nlohmann::json* best = nullptr;
    long bestScore = -1000000;
    for (const auto& image : images) {
        if (!image.is_object() || !image.contains("url")) continue;
        std::string url = image.value("url", std::string());
        std::string lowerUrl = url;
        std::transform(lowerUrl.begin(), lowerUrl.end(), lowerUrl.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        const std::size_t query = lowerUrl.find('?');
        if (query != std::string::npos) lowerUrl.resize(query);
        const bool supported = lowerUrl.ends_with(".png")
                            || lowerUrl.ends_with(".jpg")
                            || lowerUrl.ends_with(".jpeg");
        if (!supported) continue;
        const int w = image.value("width", 0);
        const int h = image.value("height", 0);
        const bool orientationOk = portrait ? (h > w) : (w >= h);
        const long score = image.value("score", 0L)
                         + image.value("upvotes", 0L)
                         - image.value("downvotes", 0L)
                         + (orientationOk ? 100000L : 0L);
        if (!best || score > bestScore) {
            best = &image;
            bestScore = score;
        }
    }
    return best;
}

std::vector<const nlohmann::json*> rankedImages(const nlohmann::json& images,
                                                bool requireLandscape,
                                                bool requirePortrait = false) {
    std::vector<const nlohmann::json*> ranked;
    if (!images.is_array()) return ranked;
    for (const auto& image : images) {
        if (!image.is_object() || !image.contains("url")) continue;
        std::string url = image.value("url", std::string());
        std::transform(url.begin(), url.end(), url.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        const std::size_t query = url.find('?');
        if (query != std::string::npos) url.resize(query);
        if (!url.ends_with(".png") && !url.ends_with(".jpg") && !url.ends_with(".jpeg"))
            continue;
        const int w = image.value("width", 0);
        const int h = image.value("height", 0);
        if (requireLandscape && w < h)
            continue;
        if (requirePortrait && h <= w)
            continue;
        ranked.push_back(&image);
    }
    std::stable_sort(ranked.begin(), ranked.end(), [](const auto* a, const auto* b) {
        const long scoreA = a->value("score", 0L) + a->value("upvotes", 0L)
                          - a->value("downvotes", 0L);
        const long scoreB = b->value("score", 0L) + b->value("upvotes", 0L)
                          - b->value("downvotes", 0L);
        return scoreA > scoreB;
    });
    return ranked;
}

nlohmann::json readMetadata(std::uint64_t titleId) {
    std::ifstream input(titleDirectory(titleId) + "/metadata.json");
    if (!input.is_open()) return nlohmann::json::object();
    try {
        nlohmann::json metadata;
        input >> metadata;
        return metadata.is_object() ? metadata : nlohmann::json::object();
    } catch (...) {
        return nlohmann::json::object();
    }
}

void writeMetadata(std::uint64_t titleId, const nlohmann::json& metadata) {
    if (titleId == 0) return;
    std::error_code ec;
    std::filesystem::create_directories(titleDirectory(titleId), ec);
    std::ofstream output(titleDirectory(titleId) + "/metadata.json", std::ios::trunc);
    if (output.is_open())
        output << metadata.dump(2);
}

bool saveBytes(const std::string& path, const std::vector<std::uint8_t>& bytes) {
    if (bytes.empty()) return false;
    const std::string tmp = path + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out.is_open()) return false;
        out.write(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
        if (!out.good()) return false;
    }
    std::error_code ec;
    std::filesystem::remove(path, ec);
    ec.clear();
    std::filesystem::rename(tmp, path, ec);
    return !ec;
}

bool fetchImage(const nlohmann::json& images, bool portrait,
                const std::string& path,
                const themeshop::http::ProgressCallback& onProgress = {}) {
    const auto* image = chooseImage(images, portrait);
    if (!image) return false;
    const std::string url = image->value("url", std::string());
    if (url.empty()) return false;
    return saveBytes(path, themeshop::http::getBytes(url, {}, onProgress));
}

bool prepareArtwork(SteamGridDbManager::ArtworkKind kind, const std::string& path) {
    const auto info = kindInfo(kind);
    if (!info.prepareCache)
        return true;
    if (kind == SteamGridDbManager::ArtworkKind::Poster)
        return steamgriddb::artwork::prepareFit(path, info.prepW, info.prepH);
    return steamgriddb::artwork::prepare(path, info.prepW, info.prepH,
                                         kind == SteamGridDbManager::ArtworkKind::Hero
                                         || kind == SteamGridDbManager::ArtworkKind::GridWide);
}

void removePreparedArtwork(SteamGridDbManager::ArtworkKind kind, const std::string& path) {
    const auto info = kindInfo(kind);
    if (!info.prepareCache)
        return;
    if (kind == SteamGridDbManager::ArtworkKind::Poster) {
        steamgriddb::artwork::removeFit(path, info.prepW, info.prepH);
        // Legacy fixed-canvas cache from earlier poster prep.
        steamgriddb::artwork::remove(path, 600, 900);
        return;
    }
    steamgriddb::artwork::remove(path, info.prepW, info.prepH);
}

std::string expandQueryAlias(const std::string& query) {
    const std::string norm = normalized(query);
    static const std::unordered_map<std::string, std::string> kAliases = {
        {"gtav", "Grand Theft Auto V"},
        {"gta5", "Grand Theft Auto V"},
        {"gtaiv", "Grand Theft Auto IV"},
        {"gta4", "Grand Theft Auto IV"},
        {"gtasa", "Grand Theft Auto: San Andreas"},
        {"gtavc", "Grand Theft Auto: Vice City"},
        {"gtaiii", "Grand Theft Auto III"},
        {"gta3", "Grand Theft Auto III"},
        {"gta", "Grand Theft Auto"},
        {"botw", "The Legend of Zelda: Breath of the Wild"},
        {"totk", "The Legend of Zelda: Tears of the Kingdom"},
        {"smb", "Super Mario Bros."},
        {"smw", "Super Mario World"},
        {"sm64", "Super Mario 64"},
        {"smo", "Super Mario Odyssey"},
        {"mk8", "Mario Kart 8"},
        {"mk8d", "Mario Kart 8 Deluxe"},
        {"gow", "God of War"},
        {"rdr2", "Red Dead Redemption 2"},
        {"rdr", "Red Dead Redemption"},
        {"re4", "Resident Evil 4"},
        {"re2", "Resident Evil 2"},
        {"re3", "Resident Evil 3"},
        {"re", "Resident Evil"},
        {"mgs", "Metal Gear Solid"},
        {"mgs3", "Metal Gear Solid 3"},
        {"ffvii", "Final Fantasy VII"},
        {"ff7", "Final Fantasy VII"},
        {"ffx", "Final Fantasy X"},
        {"ff10", "Final Fantasy X"},
        {"ff", "Final Fantasy"},
        {"pkmn", "Pokemon"},
        {"zelda", "The Legend of Zelda"},
        {"tf2", "Team Fortress 2"},
        {"csgo", "Counter-Strike: Global Offensive"},
        {"cs2", "Counter-Strike 2"},
        {"acnh", "Animal Crossing: New Horizons"},
        {"ssbu", "Super Smash Bros. Ultimate"}
    };
    auto it = kAliases.find(norm);
    if (it != kAliases.end()) return it->second;
    return {};
}

} // namespace

SteamGridDbManager::~SteamGridDbManager() {
    cancelAndWait();
}

SteamGridDbManager::BrowseResult SteamGridDbManager::browse(
    const std::string& apiKey, std::uint64_t titleId, const std::string& title,
    const std::string& query, ArtworkKind kind) {
    BrowseResult result;
    result.titleId = titleId;
    result.title = title;
    result.query = query.empty() ? title : query;
    result.kind = kind;
    if (titleId == 0 || result.query.empty() || kind == ArtworkKind::None) {
        result.error = "Invalid SteamGridDB search";
        return result;
    }

    const Backend backend = makeBackend(apiKey);
    try {
        std::vector<std::string> searchQueries;
        const std::string alias = expandQueryAlias(result.query);
        if (!alias.empty() && alias != result.query) {
            searchQueries.push_back(alias);
        }
        searchQueries.push_back(result.query);

        nlohmann::json allGames = nlohmann::json::array();
        std::set<long long> seenGameIds;
        for (const auto& q : searchQueries) {
            try {
                const auto games = searchGames(backend, q);
                if (games.is_array()) {
                    for (const auto& g : games) {
                        if (!g.is_object() || !g.contains("id")) continue;
                        long long gid = g.at("id").get<long long>();
                        if (seenGameIds.insert(gid).second) {
                            allGames.push_back(g);
                        }
                    }
                }
            } catch (...) {
            }
            if (!allGames.empty()) break;
        }

        if (allGames.empty()) {
            throw std::runtime_error("No games found for '" + result.query + "'");
        }

        struct ScoredGame {
            long long id = 0;
            std::string name;
            float score = 0.f;
        };
        std::vector<ScoredGame> scored;
        const auto queryTokens = titleTokens(result.query);
        const auto aliasTokens = !alias.empty() ? titleTokens(alias) : std::set<std::string>{};

        for (const auto& g : allGames) {
            if (!g.is_object() || !g.contains("id")) continue;
            std::string gname = g.value("name", std::string());
            long long gid = g.at("id").get<long long>();
            float s = titleSimilarity(result.query, gname);
            if (!alias.empty()) {
                s = std::max(s, titleSimilarity(alias, gname));
            }
            const auto gTokens = titleTokens(gname);
            if (!queryTokens.empty() && !gTokens.empty()) {
                int matched = 0;
                for (const auto& qt : queryTokens) {
                    if (gTokens.count(qt)) ++matched;
                }
                if (matched == static_cast<int>(queryTokens.size())) {
                    s = std::max(s, 0.82f);
                }
            }
            if (!aliasTokens.empty() && !gTokens.empty()) {
                int matched = 0;
                for (const auto& at : aliasTokens) {
                    if (gTokens.count(at)) ++matched;
                }
                if (matched == static_cast<int>(aliasTokens.size())) {
                    s = std::max(s, 0.88f);
                }
            }
            if (g.value("verified", false) && s < 1.f) {
                s = std::min(1.f, s + 0.02f);
            }
            scored.push_back({gid, std::move(gname), s});
        }

        std::sort(scored.begin(), scored.end(), [](const ScoredGame& a, const ScoredGame& b) {
            return a.score > b.score;
        });

        const auto info = kindInfo(kind);
        constexpr std::size_t kMaxCandidates = 18;
        for (const auto& sg : scored) {
            if (result.candidates.size() >= kMaxCandidates) break;
            if (!result.candidates.empty() && sg.score < 0.35f) break;

            try {
                const auto images = listArtwork(backend, sg.id, kind);
                const auto ranked = rankedImages(images, info.requireLandscape,
                                                 info.requirePortrait);
                for (std::size_t i = 0; i < ranked.size() && result.candidates.size() < kMaxCandidates; ++i) {
                    Candidate candidate;
                    candidate.id = ranked[i]->value("id", 0LL);
                    candidate.url = ranked[i]->value("url", std::string());
                    candidate.thumbnailUrl = ranked[i]->value("thumb", candidate.url);
                    candidate.width = ranked[i]->value("width", 0);
                    candidate.height = ranked[i]->value("height", 0);
                    if (!candidate.url.empty()) {
                        result.candidates.push_back(std::move(candidate));
                    }
                }
                if (result.gameId == 0) {
                    result.gameId = sg.id;
                    result.gameName = sg.name;
                    result.matchScore = sg.score;
                }
            } catch (...) {
            }
        }

        if (result.candidates.empty()) {
            throw std::runtime_error("No " + std::string(info.label)
                                     + " artwork available for '" + result.query + "'");
        }
        DebugLog::log("[steamgriddb] browse '%s' games=%d best='%s' score=%.2f candidates=%d",
                      result.query.c_str(), static_cast<int>(scored.size()),
                      result.gameName.c_str(), result.matchScore,
                      static_cast<int>(result.candidates.size()));
        result.success = true;
    } catch (const std::exception& ex) {
        result.error = friendlySteamGridDbTransportError(ex.what() ? ex.what() : "");
        DebugLog::log("[steamgriddb] browse '%s' failed: %s",
                      result.query.c_str(), ex.what());
    }
    return result;
}

SteamGridDbManager::ApplyResult SteamGridDbManager::applyCandidate(
    const BrowseResult& browseResult, const Candidate& candidate,
    const ProgressCallback& onProgress,
    const AbortCheck& shouldAbort) {
    ApplyResult result;
    result.titleId = browseResult.titleId;
    result.kind = browseResult.kind;
    auto aborted = [&]() {
        return shouldAbort && shouldAbort();
    };
    try {
        if (aborted()) {
            result.cancelled = true;
            result.message = "Cancelled";
            return result;
        }
        const auto info = kindInfo(result.kind);
        const std::string destination = pathForKind(result.kind, result.titleId);
        ensureArtworkDirectories(result.kind, result.titleId);
        if (info.prepareCache)
            removePreparedArtwork(result.kind, destination);
        if (onProgress) onProgress("Downloading artwork...", 0.02f);
        DebugLog::log("[steamgriddb] apply start title=%016llX kind=%d url=%s %dx%d",
                      static_cast<unsigned long long>(result.titleId),
                      static_cast<int>(result.kind),
                      candidate.url.c_str(),
                      candidate.width,
                      candidate.height);
        auto downloadProgress = [&](std::uint64_t downloaded, std::uint64_t total) {
            if (!onProgress) return;
            const float networkProgress = total > 0
                ? std::clamp(static_cast<float>(downloaded) / static_cast<float>(total), 0.f, 1.f)
                : 0.f;
            onProgress("Downloading artwork...", 0.02f + networkProgress * 0.68f);
        };
        if (candidate.url.empty())
            throw std::runtime_error("Artwork download failed");
        // The fork's ThemeHttp has no abort hook, so a Cancel is honoured once
        // the transfer returns and before anything is written.
        auto bytes = themeshop::http::getBytes(candidate.url, {}, downloadProgress);
        if (aborted()) {
            result.cancelled = true;
            result.message = "Cancelled";
            DebugLog::log("[steamgriddb] apply cancelled during download title=%016llX",
                          static_cast<unsigned long long>(result.titleId));
            return result;
        }
        if (bytes.empty() || !saveBytes(destination, bytes))
            throw std::runtime_error("Artwork download failed");
        DebugLog::log("[steamgriddb] apply downloaded title=%016llX bytes=%zu",
                      static_cast<unsigned long long>(result.titleId),
                      bytes.size());

        if (info.prepareCache) {
            if (aborted()) {
                result.cancelled = true;
                result.message = "Cancelled";
                return result;
            }
            if (onProgress) onProgress("Preparing artwork cache...", 0.74f);
            if (!prepareArtwork(result.kind, destination))
                throw std::runtime_error("Artwork decode failed");
        }
        if (aborted()) {
            result.cancelled = true;
            result.message = "Cancelled";
            return result;
        }
        if (onProgress) onProgress("Saving artwork...", 0.96f);

        nlohmann::json metadata = readMetadata(result.titleId);
        metadata["titleId"] = result.titleId;
        metadata["title"] = browseResult.title;
        metadata["steamGridDbGameId"] = browseResult.gameId;
        metadata["steamGridDbName"] = browseResult.gameName;
        metadata["matcherVersion"] = kMatcherVersion;
        metadata["matchScore"] = browseResult.matchScore;
        if (!browseResult.query.empty())
            metadata["searchQuery"] = browseResult.query;
        metadata[info.flag] = true;
        metadata[info.idKey] = candidate.id;
        writeMetadata(result.titleId, metadata);
        result.success = true;
        result.message = std::string(info.label) + " applied";
        if (onProgress) onProgress(result.message, 1.f);
        DebugLog::log("[steamgriddb] apply done title=%016llX kind=%d",
                      static_cast<unsigned long long>(result.titleId),
                      static_cast<int>(result.kind));
    } catch (const std::exception& ex) {
        if (aborted()) {
            result.cancelled = true;
            result.message = "Cancelled";
            DebugLog::log("[steamgriddb] apply cancelled title=%016llX",
                          static_cast<unsigned long long>(result.titleId));
        } else {
            result.message = ex.what();
            DebugLog::log("[steamgriddb] apply candidate failed: %s", ex.what());
        }
    }
    return result;
}

void SteamGridDbManager::wait() {
    if (m_task.valid()) m_task.wait();
}

void SteamGridDbManager::cancelAndWait() {
    m_cancelRequested.store(true);
    wait();
}

std::string SteamGridDbManager::heroPath(std::uint64_t titleId) {
    return titleDirectory(titleId) + "/hero.img";
}

std::string SteamGridDbManager::logoPath(std::uint64_t titleId) {
    return titleDirectory(titleId) + "/logo.img";
}

std::string SteamGridDbManager::iconPath(std::uint64_t titleId) {
    return titleDirectory(titleId) + "/icon.img";
}

std::string SteamGridDbManager::coverPath(std::uint64_t titleId) {
    char hex[17];
    std::snprintf(hex, sizeof(hex), "%016llX", static_cast<unsigned long long>(titleId));
    return std::string(kCoverRoot) + "/" + hex + ".jpg";
}

std::string SteamGridDbManager::gridWidePath(std::uint64_t titleId) {
    return titleDirectory(titleId) + "/gridwide.img";
}

bool SteamGridDbManager::hasArtwork(std::uint64_t titleId) {
    std::error_code ec;
    return cacheUsesCurrentMatcher(titleId)
        && (std::filesystem::exists(heroPath(titleId), ec)
        || std::filesystem::exists(logoPath(titleId), ec));
}

bool SteamGridDbManager::hasIcon(std::uint64_t titleId) {
    std::error_code ec;
    return titleId != 0 && std::filesystem::exists(iconPath(titleId), ec);
}

bool SteamGridDbManager::hasPoster(std::uint64_t titleId) {
    std::error_code ec;
    return titleId != 0 && std::filesystem::exists(coverPath(titleId), ec);
}

bool SteamGridDbManager::hasGridWide(std::uint64_t titleId) {
    std::error_code ec;
    return titleId != 0 && std::filesystem::exists(gridWidePath(titleId), ec);
}

std::string SteamGridDbManager::savedSearchQuery(std::uint64_t titleId) {
    if (titleId == 0) return {};
    return readMetadata(titleId).value("searchQuery", std::string());
}

void SteamGridDbManager::saveSearchQuery(std::uint64_t titleId, const std::string& query) {
    if (titleId == 0) return;
    std::string trimmed = query;
    while (!trimmed.empty() && std::isspace(static_cast<unsigned char>(trimmed.front())))
        trimmed.erase(trimmed.begin());
    while (!trimmed.empty() && std::isspace(static_cast<unsigned char>(trimmed.back())))
        trimmed.pop_back();
    if (trimmed.empty()) return;
    nlohmann::json metadata = readMetadata(titleId);
    metadata["titleId"] = titleId;
    metadata["searchQuery"] = trimmed;
    writeMetadata(titleId, metadata);
}

bool SteamGridDbManager::clearArtwork(std::uint64_t titleId, ArtworkKind kind) {
    if (titleId == 0)
        return false;

    std::error_code ec;
    bool removed = false;
    auto removeFile = [&](const std::string& path) {
        if (std::filesystem::remove(path, ec))
            removed = true;
        ec.clear();
    };

    const bool clearHero = kind == ArtworkKind::None || kind == ArtworkKind::Hero;
    const bool clearLogo = kind == ArtworkKind::None || kind == ArtworkKind::Logo;
    const bool clearIcon = kind == ArtworkKind::None || kind == ArtworkKind::Icon;
    const bool clearPoster = kind == ArtworkKind::None || kind == ArtworkKind::Cover;
    const bool clearGridWide = kind == ArtworkKind::None || kind == ArtworkKind::GridWide;

    if (clearHero) {
        removeFile(heroPath(titleId));
        removeFile(heroPath(titleId) + ".640x360.rgba-cache");
        removeFile(heroPath(titleId) + ".1280x720.rgba-cache"); // legacy
    }
    if (clearLogo) {
        removeFile(logoPath(titleId));
        removeFile(logoPath(titleId) + ".384x192.rgba-cache");
        removeFile(logoPath(titleId) + ".640x180.rgba-cache"); // legacy
    }
    if (clearIcon)
        removeFile(iconPath(titleId));
    if (clearPoster) {
        removeFile(coverPath(titleId));
        removeFile(coverPath(titleId) + ".fit320x480.rgba-cache");
        removeFile(coverPath(titleId) + ".600x900.rgba-cache"); // legacy poster prep
    }
    if (clearGridWide) {
        removeFile(gridWidePath(titleId));
        removeFile(gridWidePath(titleId) + ".460x215.rgba-cache");
    }

    nlohmann::json metadata = readMetadata(titleId);
    if (clearHero) {
        metadata["hero"] = false;
        metadata.erase("selectedHeroId");
        metadata.erase("selectedHeroIndex");
    }
    if (clearLogo) {
        metadata["logo"] = false;
        metadata.erase("selectedLogoId");
        metadata.erase("selectedLogoIndex");
    }
    if (clearIcon) {
        metadata["icon"] = false;
        metadata.erase("selectedIconId");
        metadata.erase("selectedIconIndex");
    }
    if (clearPoster) {
        metadata["cover"] = false;
        metadata.erase("selectedCoverId");
        metadata.erase("selectedCoverIndex");
    }
    if (clearGridWide) {
        metadata["gridWide"] = false;
        metadata.erase("selectedGridWideId");
        metadata.erase("selectedGridWideIndex");
    }

    const bool anyLeft = metadata.value("hero", false)
                      || metadata.value("logo", false)
                      || metadata.value("icon", false)
                      || metadata.value("cover", false)
                      || metadata.value("gridWide", false)
                      || std::filesystem::exists(heroPath(titleId), ec)
                      || std::filesystem::exists(logoPath(titleId), ec)
                      || std::filesystem::exists(iconPath(titleId), ec)
                      || std::filesystem::exists(coverPath(titleId), ec)
                      || std::filesystem::exists(gridWidePath(titleId), ec);
    ec.clear();
    if (!anyLeft) {
        removeFile(titleDirectory(titleId) + "/metadata.json");
        std::filesystem::remove(titleDirectory(titleId), ec);
    } else {
        std::ofstream output(titleDirectory(titleId) + "/metadata.json", std::ios::trunc);
        if (output.is_open())
            output << metadata.dump(2);
    }
    return removed;
}

bool SteamGridDbManager::start(const std::string& apiKey,
                               const std::vector<AppEntry>& apps) {
    if (apps.empty() || m_running.exchange(true)) return false;
    if (m_task.valid()) m_task.get();
    m_cancelRequested.store(false);
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const std::uint64_t nextRevision = m_status.revision + 1;
        m_status = {};
        m_status.running = true;
        m_status.total = static_cast<int>(apps.size());
        m_status.message = "Starting SteamGridDB scan...";
        m_status.progress01 = 0.f;
        m_status.revision = nextRevision;
    }
    m_task = std::async(std::launch::async,
                        [this, apiKey, apps]() mutable { scrape(std::move(apiKey), std::move(apps)); });
    return true;
}

bool SteamGridDbManager::startSelectNext(const std::string& apiKey,
                                         std::uint64_t titleId,
                                         const std::string& title,
                                         ArtworkKind kind, int direction) {
    if (titleId == 0 || title.empty() || kind == ArtworkKind::None
        || m_running.exchange(true))
        return false;
    if (m_task.valid()) m_task.get();
    m_cancelRequested.store(false);
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const std::uint64_t nextRevision = m_status.revision + 1;
        m_status = {};
        m_status.running = true;
        m_status.total = 1;
        m_status.currentTitle = title;
        m_status.message = "Loading SteamGridDB choices...";
        m_status.progress01 = 0.f;
        m_status.selectedKind = kind;
        m_status.revision = nextRevision;
    }
    m_task = std::async(std::launch::async,
                        [this, apiKey, titleId, title, kind, direction]() mutable {
                            selectNext(std::move(apiKey), titleId, std::move(title), kind,
                                       direction < 0 ? -1 : 1);
                        });
    return true;
}

void SteamGridDbManager::selectNext(std::string apiKey, std::uint64_t titleId,
                                    std::string title, ArtworkKind kind, int direction) {
    const Backend backend = makeBackend(apiKey);
    bool saved = false;
    int selectedIndex = -1;
    int selectionCount = 0;
    std::string message;
    try {
        nlohmann::json metadata = readMetadata(titleId);
        long long gameId = metadata.value("steamGridDbGameId", 0LL);
        std::string matchedName = metadata.value("steamGridDbName", std::string());
        float matchScore = metadata.value("matchScore", 0.f);
        if (gameId <= 0 || metadata.value("matcherVersion", 0) != kMatcherVersion) {
            auto games = searchGames(backend, title);
            std::string closest;
            bool ambiguous = false;
            const auto* game = chooseGame(games, title, matchScore, closest, ambiguous);
            if (!game) {
                const std::string alias = expandQueryAlias(title);
                if (!alias.empty() && alias != title) {
                    try {
                        games = searchGames(backend, alias);
                        game = chooseGame(games, alias, matchScore, closest, ambiguous);
                    } catch (...) {
                    }
                }
            }
            if (!game) throw std::runtime_error("No sufficiently close game match");
            gameId = game->at("id").get<long long>();
            matchedName = game->value("name", title);
        }

        const auto info = kindInfo(kind);
        const std::string destination = pathForKind(kind, titleId);
        const auto images = listArtwork(backend, gameId, kind);
        const auto ranked = rankedImages(images, info.requireLandscape,
                                         info.requirePortrait);
        selectionCount = static_cast<int>(ranked.size());
        if (ranked.empty()) throw std::runtime_error("No artwork available");
        std::error_code existingEc;
        const bool hasCurrent = metadata.value(info.flag, false)
            && std::filesystem::exists(destination, existingEc);
        const int currentIndex = hasCurrent ? metadata.value(info.indexKey, 0)
                                           : (direction > 0 ? -1 : 0);
        selectedIndex = (currentIndex + direction + selectionCount) % selectionCount;
        const std::string imageUrl = ranked[(size_t)selectedIndex]->value("url", std::string());
        ensureArtworkDirectories(kind, titleId);
        if (info.prepareCache)
            removePreparedArtwork(kind, destination);
        saved = !imageUrl.empty() && saveBytes(destination, themeshop::http::getBytes(imageUrl));
        if (!saved) throw std::runtime_error("Artwork download failed");
        if (!prepareArtwork(kind, destination))
            throw std::runtime_error("Artwork decode failed");

        metadata["titleId"] = titleId;
        metadata["title"] = title;
        metadata["steamGridDbGameId"] = gameId;
        metadata["steamGridDbName"] = matchedName;
        metadata["matcherVersion"] = kMatcherVersion;
        metadata["matchScore"] = matchScore;
        metadata[info.indexKey] = selectedIndex;
        metadata[info.flag] = true;
        std::ofstream output(titleDirectory(titleId) + "/metadata.json", std::ios::trunc);
        if (output.is_open()) output << metadata.dump(2);
        message = std::string(info.label) + " " + std::to_string(selectedIndex + 1) + "/"
                + std::to_string(selectionCount) + " applied";
    } catch (const std::exception& ex) {
        message = ex.what();
        DebugLog::log("[steamgriddb] manual selection '%s' failed: %s",
                      title.c_str(), ex.what());
    }

    updateStatus([&](Status& status) {
        status.running = false;
        status.finished = true;
        status.completed = 1;
        status.matched = saved ? 1 : 0;
        status.failed = saved ? 0 : 1;
        status.currentTitle.clear();
        status.message = message;
        status.lastCompletedTitleId = titleId;
        status.selectedKind = kind;
        status.selectedIndex = selectedIndex;
        status.selectionCount = selectionCount;
        status.progress01 = 1.f;
    });
    m_running.store(false);
}

SteamGridDbManager::Status SteamGridDbManager::status() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_status;
}

void SteamGridDbManager::updateStatus(const std::function<void(Status&)>& change) {
    std::lock_guard<std::mutex> lock(m_mutex);
    change(m_status);
    ++m_status.revision;
}

void SteamGridDbManager::scrape(std::string apiKey, std::vector<AppEntry> apps) {
    const Backend backend = makeBackend(apiKey);

    std::error_code ec;
    std::filesystem::create_directories(kCacheRoot, ec);
    std::filesystem::create_directories(kCoverRoot, ec);
    std::string fatalError;

    for (const auto& app : apps) {
        if (m_cancelRequested.load()) break;
        if (!app.isApplication() || app.titleId == 0) {
            updateStatus([](Status& s) {
                ++s.completed;
                s.progress01 = s.total > 0
                    ? static_cast<float>(s.completed) / static_cast<float>(s.total) : 1.f;
            });
            continue;
        }

        updateStatus([&](Status& s) {
            s.currentTitle = app.title;
            s.message = "Searching " + app.title;
            s.progress01 = s.total > 0
                ? static_cast<float>(s.completed) / static_cast<float>(s.total) : 0.f;
        });

        bool matched = false;
        // Every cache written before an API key was configured has a hero and
        // no logo: the gallery proxy has no logo endpoint. hasArtwork() is
        // satisfied by the hero alone, so those titles were skipped on every
        // later scan and their logos could never arrive, however many times the
        // scan was run with a key in place. All fifteen cached games on the
        // reporter's card were in exactly that state.
        //
        // Such a title is topped up instead of skipped: the cached hero and the
        // metadata stay where they are and only the logo is fetched, so nothing
        // already downloaded is put at risk. Artwork picked by hand is never
        // touched -- per-title browsing owns that.
        std::error_code logoEc;
        const bool topUpLogo = !backend.proxy
            && hasArtwork(app.titleId)
            && !std::filesystem::exists(logoPath(app.titleId), logoEc)
            && !artworkWasChosenByHand(app.titleId);
        // The cover is also the single-row poster (ArtworkKind::Poster).
        std::error_code coverEc;
        const bool topUpCover = hasArtwork(app.titleId)
            && !std::filesystem::exists(coverPath(app.titleId), coverEc)
            && !artworkWasChosenByHand(app.titleId);
        // hasArtwork() is satisfied by a logo alone, so a title whose earlier
        // scan saved the logo but not the hero (a dropped connection, or a
        // search under the hex id) was skipped on every later scan and never
        // got its backdrop. Reported for the downgraded Zelda: hero=0 logo=1
        // on every boot, while the scan matched it at score 1.00 and only
        // fetched the cover. Top the hero up like the logo and the cover.
        std::error_code heroEc;
        const bool topUpHero = hasArtwork(app.titleId)
            && !std::filesystem::exists(heroPath(app.titleId), heroEc)
            && !artworkWasChosenByHand(app.titleId);
        // Landscape SGDB grid for 2x1 / 4x2 HOME tiles (PoloNX), only for
        // titles whose tile has that shape.
        const bool wantsGridWide =
            (app.widgetColumns == 2 && app.widgetRows == 1)
            || (app.widgetColumns == 4 && app.widgetRows == 2);
        const bool topUpGridWide = wantsGridWide
            && hasArtwork(app.titleId)
            && !hasGridWide(app.titleId)
            && !artworkWasChosenByHand(app.titleId);
        const bool topUp = topUpLogo || topUpCover || topUpHero || topUpGridWide;
        // The global Settings scan is incremental: once an application has a
        // trusted hero, logo, or cover, leave its manual/default choices untouched.
        // Per-title browsing is the explicit path for replacing artwork.
        if (hasArtwork(app.titleId) && !topUp) {
            updateStatus([](Status& s) {
                ++s.completed;
                ++s.matched;
                s.progress01 = s.total > 0
                    ? static_cast<float>(s.completed) / static_cast<float>(s.total) : 1.f;
            });
            continue;
        }

        // Version 1 accepted the first autocomplete result as a fallback. Its
        // cached files cannot be trusted (for example Nintendo Labo -> Land).
        if (!topUp)
            removeCachedArtwork(app.titleId);

        try {
            const std::string& searchTitle = app.steamGridDbTitle();
            auto games = searchGames(backend, searchTitle);
            float matchScore = 0.f;
            std::string closest;
            bool ambiguous = false;
            const auto* game = chooseGame(games, searchTitle, matchScore, closest, ambiguous);
            if (!game) {
                const std::string alias = expandQueryAlias(searchTitle);
                if (!alias.empty() && alias != searchTitle) {
                    try {
                        games = searchGames(backend, alias);
                        game = chooseGame(games, alias, matchScore, closest, ambiguous);
                    } catch (...) {
                    }
                }
            }
            if (!game) {
                DebugLog::log("[steamgriddb] display='%s' search='%s' rejected candidates=%d closest='%s' score=%.2f ambiguous=%d",
                              app.title.c_str(), searchTitle.c_str(),
                              games.is_array() ? static_cast<int>(games.size()) : 0,
                              closest.empty() ? "none" : closest.c_str(), matchScore,
                              ambiguous ? 1 : 0);
                throw std::runtime_error("No sufficiently close game match");
            }
            DebugLog::log("[steamgriddb] display='%s' search='%s' matched '%s' score=%.2f id=%lld",
                          app.title.c_str(), searchTitle.c_str(),
                          game->value("name", std::string()).c_str(),
                          matchScore, game->at("id").get<long long>());
            const long long gameId = game->at("id").get<long long>();

            const std::string dir = titleDirectory(app.titleId);
            std::filesystem::create_directories(dir, ec);
            auto fetchArtwork = [&](ArtworkKind kind,
                                    bool portrait,
                                    const std::string& destination,
                                    float phaseStart, float phaseSpan) {
                if (m_cancelRequested.load())
                    throw std::runtime_error("Cancelled");
                const auto info = kindInfo(kind);
                // "heroes", "logos", "grids", "icons" -- the fork's log wording.
                const char* label = info.endpoint + 1;
                try {
                    const auto images = listArtwork(backend, gameId, kind);
                    if (m_cancelRequested.load())
                        throw std::runtime_error("Cancelled");
                    auto networkProgress = [&](std::uint64_t downloaded, std::uint64_t total) {
                        const float fraction = total > 0
                            ? std::clamp(static_cast<float>(downloaded)
                                         / static_cast<float>(total), 0.f, 1.f)
                            : 0.f;
                        updateStatus([&](Status& s) {
                            s.message = "Downloading artwork for " + app.title;
                            s.progress01 = s.total > 0
                                ? (static_cast<float>(s.completed)
                                   + phaseStart + phaseSpan * fraction)
                                    / static_cast<float>(s.total)
                                : 0.f;
                        });
                    };
                    const bool saved = fetchImage(images, portrait, destination, networkProgress);
                    if (m_cancelRequested.load())
                        throw std::runtime_error("Cancelled");
                    if (saved && info.prepareCache) {
                        updateStatus([&](Status& s) {
                            s.message = "Preparing artwork for " + app.title;
                            s.progress01 = s.total > 0
                                ? (static_cast<float>(s.completed) + phaseStart + phaseSpan)
                                    / static_cast<float>(s.total)
                                : 0.f;
                        });
                        if (!prepareArtwork(kind, destination))
                            throw std::runtime_error("Artwork decode failed");
                    }
                    DebugLog::log("[steamgriddb] '%s' %s candidates=%d saved=%d",
                                  app.title.c_str(), label,
                                  images.is_array() ? static_cast<int>(images.size()) : 0,
                                  saved ? 1 : 0);
                    return saved;
                } catch (const std::exception& ex) {
                    if (std::string(ex.what()) == "Cancelled" || m_cancelRequested.load())
                        throw;
                    // A missing or unsupported asset (a logo, a wide grid) must
                    // not discard a valid hero or cover for the same game.
                    DebugLog::log("[steamgriddb] '%s' %s failed: %s",
                                  app.title.c_str(), label, ex.what());
                    return false;
                }
            };

            // On a top-up the files already on disk are the ones chosen for
            // this title; downloading them again would spend the bandwidth to
            // replace a file with itself, and could land on a different image.
            // Only the missing kinds are fetched.
            auto keepOrFetch = [&](ArtworkKind kind, bool portrait,
                                   const std::string& destination,
                                   float phaseStart, float phaseSpan) {
                std::error_code existEc;
                if (topUp && std::filesystem::exists(destination, existEc))
                    return true;
                return fetchArtwork(kind, portrait, destination, phaseStart, phaseSpan);
            };
            const bool heroOk = keepOrFetch(ArtworkKind::Hero, false,
                                            heroPath(app.titleId), 0.08f, 0.20f);
            if (m_cancelRequested.load()) break;
            const bool logoOk = keepOrFetch(ArtworkKind::Logo, false,
                                            logoPath(app.titleId), 0.30f, 0.20f);
            if (m_cancelRequested.load()) break;
            // One 600x900 grid serves as both the Cover view art and the
            // single-row poster.
            const bool coverOk = keepOrFetch(ArtworkKind::Cover, true,
                                             coverPath(app.titleId), 0.52f, 0.20f);
            if (m_cancelRequested.load()) break;
            bool gridWideOk = false;
            if (wantsGridWide) {
                gridWideOk = keepOrFetch(ArtworkKind::GridWide, false,
                                         gridWidePath(app.titleId), 0.74f, 0.20f);
                if (m_cancelRequested.load()) break;
            }
            matched = heroOk || logoOk || coverOk || gridWideOk;

            nlohmann::json metadata;
            metadata["titleId"] = app.titleId;
            metadata["title"] = app.title;
            metadata["searchTitle"] = searchTitle;
            metadata["steamGridDbGameId"] = gameId;
            metadata["steamGridDbName"] = game->value("name", app.title);
            metadata["matcherVersion"] = kMatcherVersion;
            metadata["matchScore"] = matchScore;
            metadata["hero"] = heroOk;
            metadata["logo"] = logoOk;
            metadata["cover"] = coverOk;
            metadata["gridWide"] = gridWideOk;
            std::ofstream meta(dir + "/metadata.json", std::ios::trunc);
            if (meta.is_open()) meta << metadata.dump(2);
        } catch (const std::exception& ex) {
            DebugLog::log("[steamgriddb] '%s' failed: %s", app.title.c_str(), ex.what());
            // The cache was left intact for a top-up, so the title still has the
            // artwork it had before and must not be reported as missing.
            if (topUp)
                matched = true;
            const std::string error = ex.what();
            if (error.find("HTTP error 401") != std::string::npos
                || error.find("HTTP error 403") != std::string::npos) {
                fatalError = backend.proxy
                    ? "The artwork service rejected the request"
                    : "The SteamGridDB API key was rejected";
            } else if (error.find("HTTP error 429") != std::string::npos) {
                fatalError = "Artwork rate limit reached; try again later";
            } else if (error.find("nifm") != std::string::npos
                       || error.find("Internet connection is not ready")
                          != std::string::npos) {
                // Without this the console being offline was indistinguishable
                // from a title simply having no artwork: every entry failed and
                // the only thing said was how many were missing, so a mistyped
                // API key and a dropped connection looked identical.
                fatalError = "No internet connection";
            }
        }

        updateStatus([&](Status& s) {
            ++s.completed;
            if (matched) ++s.matched;
            else ++s.failed;
            s.lastCompletedTitleId = app.titleId;
            s.progress01 = s.total > 0
                ? static_cast<float>(s.completed) / static_cast<float>(s.total) : 1.f;
        });
        if (!fatalError.empty()) {
            updateStatus([&](Status& s) {
                s.failed += std::max(0, s.total - s.completed);
                s.completed = s.total;
                s.message = fatalError;
            });
            break;
        }
    }

    updateStatus([&](Status& s) {
        s.running = false;
        s.finished = true;
        s.currentTitle.clear();
        if (m_cancelRequested.load())
            s.message = "SteamGridDB scan cancelled";
        else if (!fatalError.empty())
            s.message = fatalError;
        else
            s.message = "SteamGridDB scan complete";
        s.progress01 = 1.f;
    });
    m_running.store(false);
}
