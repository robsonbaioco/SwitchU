#include "ThemeCatalogClient.hpp"

#include "ThemeHttp.hpp"

#include "core/DebugLog.hpp"

#include <nxui/core/I18n.hpp>
#include <fstream>
#include <filesystem>
#include <nxui/core/ThreadPool.hpp>

#include <curlpp/Easy.hpp>
#include <curlpp/Exception.hpp>
#include <curlpp/Infos.hpp>
#include <curlpp/Options.hpp>
#include <curlpp/cURLpp.hpp>
#include <nlohmann/json.hpp>
#include <switch.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <stdexcept>

namespace {

void readStringOpt(const nlohmann::json& j, const char* key, std::string& out) {
    auto it = j.find(key);
    if (it == j.end() || !it->is_string())
        return;
    out = it->get<std::string>();
}

void appendUniqueString(std::vector<std::string>& out, std::string value) {
    if (value.empty())
        return;
    if (std::find(out.begin(), out.end(), value) != out.end())
        return;
    out.push_back(std::move(value));
}

void readStringArrayOpt(const nlohmann::json& j, const char* key, std::vector<std::string>& out) {
    auto it = j.find(key);
    if (it == j.end() || !it->is_array())
        return;

    for (const auto& item : *it) {
        if (item.is_string())
            appendUniqueString(out, item.get<std::string>());
    }
}

bool startsWith(const std::string& value, const std::string& prefix) {
    return value.size() >= prefix.size() && value.compare(0, prefix.size(), prefix) == 0;
}

bool safeThemeId(const std::string& value) {
    if (value.size() < 2 || value.size() > 64)
        return false;
    for (unsigned char ch : value) {
        if (!std::islower(ch) && !std::isdigit(ch) && ch != '_' && ch != '-')
            return false;
    }
    return true;
}

// SD copy of one catalogue index, used when the network is down. The default
// (our mirror) keeps PoloNX's original file name; any other source gets a
// stable FNV-1a suffix so two indexes never share a file.
std::string catalogDiskPathFor(const std::string& url) {
    std::string path = "sdmc:/config/SwitchU/community_preview_cache/catalog_index";
    if (url != ThemeCatalogClient::kDefaultCatalogUrl) {
        std::uint32_t hash = 2166136261u;
        for (unsigned char ch : url) {
            hash ^= ch;
            hash *= 16777619u;
        }
        char suffix[16];
        std::snprintf(suffix, sizeof(suffix), "_%08x", (unsigned)hash);
        path += suffix;
    }
    return path + ".json";
}

std::string trimSlashes(std::string value) {
    while (!value.empty() && value.front() == '/')
        value.erase(value.begin());
    while (!value.empty() && value.back() == '/')
        value.pop_back();
    return value;
}

std::string parentDirectory(const std::string& path) {
    std::string clean = trimSlashes(path);
    std::size_t slash = clean.find_last_of('/');
    if (slash == std::string::npos)
        return {};
    return clean.substr(0, slash);
}

std::string joinPath(const std::string& base, const std::string& relative) {
    if (base.empty())
        return relative;
    if (relative.empty())
        return base;
    if (base.back() == '/')
        return base + relative;
    return base + "/" + relative;
}

std::string deriveManifestPath(const std::string& path) {
    if (path.empty())
        return {};
    return path + "/theme.json";
}

std::string resolvePreviewPath(const ThemeCatalogClient::Entry& entry, std::string previewPath) {
    previewPath = trimSlashes(std::move(previewPath));
    if (previewPath.empty())
        return {};
    if (startsWith(previewPath, "https://") || startsWith(previewPath, "http://"))
        return previewPath;

    if (!entry.path.empty())
        return joinPath(trimSlashes(entry.path), previewPath);

    return joinPath(parentDirectory(entry.manifest), previewPath);
}

void appendResolvedPreviewPath(const ThemeCatalogClient::Entry& entry,
                               std::vector<std::string>& out,
                               const std::string& previewPath) {
    appendUniqueString(out, resolvePreviewPath(entry, previewPath));
}

void readResolvedStringArrayOpt(const nlohmann::json& j,
                                const char* key,
                                const ThemeCatalogClient::Entry& entry,
                                std::vector<std::string>& out) {
    auto it = j.find(key);
    if (it == j.end() || !it->is_array())
        return;

    for (const auto& item : *it) {
        if (item.is_string())
            appendResolvedPreviewPath(entry, out, item.get<std::string>());
    }
}

void ensureCoverFirst(ThemeCatalogClient::Entry& entry) {
    if (entry.cover.empty()) {
        if (!entry.screenshots.empty())
            entry.cover = entry.screenshots.front();
        return;
    }

    auto it = std::find(entry.screenshots.begin(), entry.screenshots.end(), entry.cover);
    if (it == entry.screenshots.end()) {
        entry.screenshots.insert(entry.screenshots.begin(), entry.cover);
        return;
    }

    if (it != entry.screenshots.begin()) {
        std::string cover = *it;
        entry.screenshots.erase(it);
        entry.screenshots.insert(entry.screenshots.begin(), std::move(cover));
    }
}


bool jsonDeclaresIcons(const nlohmann::json& manifest) {
    auto iconsOf = [](const nlohmann::json& node) -> bool {
        if (!node.is_object() && !node.is_string())
            return false;
        if (node.is_string())
            return !node.get<std::string>().empty();
        for (const char* key : {"basePath", "base_path", "path", "base", "directory", "dir"}) {
            auto it = node.find(key);
            if (it != node.end() && it->is_string() && !it->get<std::string>().empty())
                return true;
        }
        auto filesIt = node.find("files");
        if (filesIt != node.end() && filesIt->is_array() && !filesIt->empty())
            return true;
        return false;
    };

    auto iconsIt = manifest.find("icons");
    if (iconsIt != manifest.end() && iconsOf(*iconsIt))
        return true;
    auto themeIt = manifest.find("theme");
    if (themeIt != manifest.end() && themeIt->is_object()) {
        auto themeIcons = themeIt->find("icons");
        if (themeIcons != themeIt->end() && iconsOf(*themeIcons))
            return true;
    }
    return false;
}

bool manifestDeclaresIcons(const std::string& catalogUrl, const ThemeCatalogClient::Entry& entry) {
    if (entry.manifest.empty())
        return false;
    std::size_t slash = catalogUrl.find_last_of('/');
    if (slash == std::string::npos)
        return false;
    const std::string manifestUrl = catalogUrl.substr(0, slash + 1) + trimSlashes(entry.manifest);
    const std::string manifestText = themeshop::http::getText(manifestUrl);
    nlohmann::json manifest = nlohmann::json::parse(manifestText);
    return jsonDeclaresIcons(manifest);
}

struct PreviewData {
    std::string cover;
    std::vector<std::string> screenshots;
    bool hasIcons = false;
    bool hasMusic = false;
    bool hasSounds = false;
    bool fetchedManifest = false;
};

std::string absoluteFromCatalog(const std::string& catalogUrl, const std::string& relative) {
    if (relative.empty()) return relative;
    if (relative.rfind("http://", 0) == 0 || relative.rfind("https://", 0) == 0)
        return relative;
    std::size_t slash = catalogUrl.find_last_of('/');
    if (slash == std::string::npos) return relative;
    return catalogUrl.substr(0, slash + 1) + trimSlashes(relative);
}

bool jsonDeclaresBundledAudioDir(const nlohmann::json& j, const char* key) {
    auto it = j.find(key);
    if (it == j.end())
        return false;
    if (it->is_boolean())
        return it->get<bool>();
    if (it->is_string())
        return !it->get<std::string>().empty();
    if (it->is_array())
        return !it->empty();
    if (it->is_object())
        return true;
    return false;
}

bool jsonDeclaresMusic(const nlohmann::json& j) {
    if (j.contains("hasMusic") && j["hasMusic"].is_boolean())
        return j["hasMusic"].get<bool>();
    if (j.contains("has_music") && j["has_music"].is_boolean())
        return j["has_music"].get<bool>();
    if (jsonDeclaresBundledAudioDir(j, "music"))
        return true;
    auto audioIt = j.find("audio");
    if (audioIt != j.end() && audioIt->is_object()) {
        if (jsonDeclaresBundledAudioDir(*audioIt, "music"))
            return true;
        std::string preset;
        if (audioIt->contains("preset") && (*audioIt)["preset"].is_string())
            preset = (*audioIt)["preset"].get<std::string>();
        else if (audioIt->contains("soundPreset") && (*audioIt)["soundPreset"].is_string())
            preset = (*audioIt)["soundPreset"].get<std::string>();
            // package:/bundled without an explicit music key still usually ships BGM.
        if (preset == "bundled" || preset == "theme" || preset == "package"
            || preset.rfind("package:", 0) == 0)
            return true;
    }
    std::string soundPreset;
    if (j.contains("soundPreset") && j["soundPreset"].is_string())
        soundPreset = j["soundPreset"].get<std::string>();
    if (soundPreset == "bundled" || soundPreset == "theme" || soundPreset == "package"
        || soundPreset.rfind("package:", 0) == 0)
        return true;
    return false;
}

bool jsonDeclaresSounds(const nlohmann::json& j) {
    if (j.contains("hasSounds") && j["hasSounds"].is_boolean())
        return j["hasSounds"].get<bool>();
    if (j.contains("has_sounds") && j["has_sounds"].is_boolean())
        return j["has_sounds"].get<bool>();
    if (j.contains("hasSfx") && j["hasSfx"].is_boolean())
        return j["hasSfx"].get<bool>();
    if (jsonDeclaresBundledAudioDir(j, "sfx") || jsonDeclaresBundledAudioDir(j, "sounds"))
        return true;
    auto audioIt = j.find("audio");
    if (audioIt != j.end() && audioIt->is_object()) {
        if (jsonDeclaresBundledAudioDir(*audioIt, "sfx")
            || jsonDeclaresBundledAudioDir(*audioIt, "sounds"))
            return true;
        // Explicit music-only packages should not claim Sounds.
        if (jsonDeclaresBundledAudioDir(*audioIt, "music"))
            return false;
    }
    if (jsonDeclaresBundledAudioDir(j, "music"))
        return false;
    std::string soundPreset;
    if (j.contains("soundPreset") && j["soundPreset"].is_string())
        soundPreset = j["soundPreset"].get<std::string>();
    if (soundPreset == "bundled" || soundPreset == "theme" || soundPreset == "package"
        || soundPreset.rfind("package:", 0) == 0)
        return true;
    return false;
}

PreviewData derivePreviewDataFromManifest(const std::string& catalogUrl,
                                          const ThemeCatalogClient::Entry& entry) {
    if (entry.manifest.empty())
        return {};

    std::size_t slash = catalogUrl.find_last_of('/');
    if (slash == std::string::npos)
        return {};

    const std::string manifestUrl = catalogUrl.substr(0, slash + 1) + trimSlashes(entry.manifest);
    const std::string manifestText = themeshop::http::getText(manifestUrl);

    nlohmann::json manifest;
    try {
        manifest = nlohmann::json::parse(manifestText);
    } catch (const std::exception& ex) {
        throw std::runtime_error(std::string("Invalid theme manifest JSON: ") + ex.what());
    }

    PreviewData data;
    data.fetchedManifest = true;
    data.hasIcons = jsonDeclaresIcons(manifest);
    data.hasMusic = jsonDeclaresMusic(manifest);
    data.hasSounds = jsonDeclaresSounds(manifest);
    std::string cover;
    readStringOpt(manifest, "cover", cover);
    if (!cover.empty())
        data.cover = resolvePreviewPath(entry, cover);

    appendUniqueString(data.screenshots, data.cover);

    std::string screenshot;
    readStringOpt(manifest, "screenshot", screenshot);
    appendResolvedPreviewPath(entry, data.screenshots, screenshot);
    readResolvedStringArrayOpt(manifest, "screenshots", entry, data.screenshots);

    auto previewIt = manifest.find("preview");
    if (previewIt == manifest.end() || !previewIt->is_object()) {
        if (data.cover.empty() && !data.screenshots.empty())
            data.cover = data.screenshots.front();
        return data;
    }

    cover.clear();
    readStringOpt(*previewIt, "cover", cover);
    if (!cover.empty() && data.cover.empty())
        data.cover = resolvePreviewPath(entry, cover);
    appendResolvedPreviewPath(entry, data.screenshots, cover);

    auto screenshotsIt = previewIt->find("screenshots");
    if (screenshotsIt != previewIt->end() && screenshotsIt->is_array()) {
        for (const auto& screenshot : *screenshotsIt) {
            if (screenshot.is_string())
                appendResolvedPreviewPath(entry, data.screenshots, screenshot.get<std::string>());
        }
    }

    auto screenshotIt = previewIt->find("screenshot");
    if (screenshotIt != previewIt->end() && screenshotIt->is_string())
        appendResolvedPreviewPath(entry, data.screenshots, screenshotIt->get<std::string>());

    if (data.cover.empty() && !data.screenshots.empty())
        data.cover = data.screenshots.front();

    return data;
}

} // namespace

ThemeCatalogClient::ThemeCatalogClient(std::string catalogUrl) {
    m_catalogUrls.push_back(std::move(catalogUrl));
    m_catalogUrls.emplace_back(kUpstreamCatalogUrl);
}

void ThemeCatalogClient::setCatalogUrl(std::string catalogUrl) {
    std::lock_guard<std::mutex> lk(m_mutex);
    m_catalogUrls.clear();
    m_catalogUrls.push_back(std::move(catalogUrl));
}

void ThemeCatalogClient::setCatalogUrls(std::vector<std::string> urls) {
    std::lock_guard<std::mutex> lk(m_mutex);
    m_catalogUrls = std::move(urls);
}

void ThemeCatalogClient::refresh(nxui::ThreadPool& pool) {
    if (m_future.valid() && m_future.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
        return;

    if (m_future.valid())
        m_future.get();

    std::string url;
    std::vector<std::string> urls;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        urls = m_catalogUrls;
        url = urls.empty() ? std::string() : urls.front();
        // Traduzida na origem: esta mensagem e escrita no estado da
        // transferencia e desenhada tal e qual, sem passar por i18n depois.
        m_loading.begin(nxui::I18n::instance().tr("themeshop.community.loading",
                                                  "Loading theme catalog..."));
        ++m_revision;
    }

    DebugLog::log("[themeshop] catalog refresh start: %d source(s)", (int)urls.size());

    m_future = pool.submit([this, urls]() {
        Snapshot loaded;
        std::string lastError;
        int okSources = 0;
        // One unreachable catalogue must not empty the shop: whatever the other
        // returns is still worth showing, and only a total failure is an error.
        for (const auto& source : urls) {
            try {
                Snapshot part = loadCatalog(source);
                ++okSources;
                for (auto& entry : part.entries) {
                    const bool dup = std::any_of(loaded.entries.begin(), loaded.entries.end(),
                                                 [&](const Entry& e) { return e.id == entry.id; });
                    if (!dup)
                        loaded.entries.push_back(std::move(entry));
                }
                // Icon packs (PoloNX's index carries them) merge the same way:
                // first source wins an id, the rest keep their own base URL.
                for (auto& icon : part.icons) {
                    const bool dup = std::any_of(loaded.icons.begin(), loaded.icons.end(),
                                                 [&](const IconEntry& e) { return e.id == icon.id; });
                    if (!dup)
                        loaded.icons.push_back(std::move(icon));
                }
                DebugLog::log("[themeshop] catalog %s: %d entries, %d icon packs",
                              source.c_str(), (int)part.entries.size(), (int)part.icons.size());
            } catch (const std::exception& ex) {
                lastError = ex.what();
                DebugLog::log("[themeshop] catalog %s failed: %s", source.c_str(), ex.what());
            } catch (...) {
                lastError = "Unknown network error";
                DebugLog::log("[themeshop] catalog %s failed: unknown error", source.c_str());
            }
        }

        try {
            if (okSources == 0) {
                // Mensagem para quem le, nao o texto da excecao. Ela ecoava o erro
                // cru -- em ingles e citando a URL do catalogo que falhou por
                // ultimo, que podia ser o da outra aba. O detalhe continua no log.
                throw std::runtime_error(nxui::I18n::instance().tr(
                    "themeshop.community.offline",
                    "No connection. Check the internet and try again."));
            }
            loaded.loading.succeed();
            DebugLog::log("[themeshop] catalog refresh done: %d entries from %d source(s)",
                          (int)loaded.entries.size(), okSources);
        } catch (const std::exception& ex) {
            loaded.loading.fail(ex.what());
            DebugLog::log("[themeshop] catalog refresh failed: %s", ex.what());
        } catch (...) {
            loaded.loading.fail("Unknown network error");
            DebugLog::log("[themeshop] catalog refresh failed: unknown error");
        }

        std::lock_guard<std::mutex> lk(m_mutex);
        if (loaded.loading.isReady()) {
            m_entries = std::move(loaded.entries);
            m_icons = std::move(loaded.icons);
        }
        m_loading = loaded.loading;
        ++m_revision;
    });
}

bool ThemeCatalogClient::poll() {
    if (!m_future.valid())
        return false;
    if (m_future.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
        return false;

    m_future.get();
    return true;
}

ThemeCatalogClient::Snapshot ThemeCatalogClient::snapshot() const {
    std::lock_guard<std::mutex> lk(m_mutex);

    Snapshot snapshot;
    snapshot.loading = m_loading;
    snapshot.entries = m_entries;
    snapshot.icons = m_icons;
    snapshot.revision = m_revision;
    return snapshot;
}

ThemeCatalogClient::Snapshot ThemeCatalogClient::loadCatalog(const std::string& url) {
    Snapshot snapshot;

    // Prefer a fresh network catalog; keep the SD copy as an offline fallback
    // so Community still paints when raw.githubusercontent.com is unreachable.
    // (Previously disk-only after the first fetch, so renames/new themes never
    // appeared until the user hit Refresh.)
    // One file per source: the fork reads two indexes, and a single shared
    // file let the second overwrite the first, so offline the shop showed
    // PoloNX's list under both. Every name starts with "catalog_index" so
    // ThemeShopScreen::refreshCommunityCatalog(force) can drop them all.
    const std::string catalogDiskPath = catalogDiskPathFor(url);
    const char* kCatalogDiskPath = catalogDiskPath.c_str();
    std::string diskBody;
    {
        std::ifstream in(kCatalogDiskPath, std::ios::binary);
        if (in) {
            diskBody.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        }
    }

    std::string body;
    try {
        body = themeshop::http::getText(url);
    } catch (const std::exception& ex) {
        DebugLog::log("[themeshop] catalog network fetch failed: %s", ex.what());
    } catch (...) {
        DebugLog::log("[themeshop] catalog network fetch failed: unknown error");
    }

    if (!body.empty()) {
        std::error_code ec;
        std::filesystem::create_directories("sdmc:/config/SwitchU/community_preview_cache", ec);
        std::ofstream out(kCatalogDiskPath, std::ios::binary | std::ios::trunc);
        if (out)
            out.write(body.data(), static_cast<std::streamsize>(body.size()));
        DebugLog::log("[themeshop] catalog fetched and cached (%zu bytes)", body.size());
    } else if (!diskBody.empty()) {
        body = std::move(diskBody);
        DebugLog::log("[themeshop] catalog loaded from disk cache (%zu bytes)", body.size());
    } else {
        throw std::runtime_error("Catalog body empty");
    }

    nlohmann::json root;
    try {
        root = nlohmann::json::parse(body);
    } catch (const std::exception& ex) {
        throw std::runtime_error(std::string("Invalid catalog JSON: ") + ex.what());
    }

    // Icon packs are optional so catalogs published before the icon shop keep
    // working unchanged.
    if (auto iconsIt = root.find("icons"); iconsIt != root.end() && iconsIt->is_array()) {
        for (const auto& item : *iconsIt) {
            if (!item.is_object())
                continue;

            IconEntry icon;
            readStringOpt(item, "id", icon.id);
            readStringOpt(item, "name", icon.name);
            readStringOpt(item, "author", icon.author);
            readStringOpt(item, "version", icon.version);
            readStringOpt(item, "description", icon.description);
            readStringOpt(item, "path", icon.path);
            readStringOpt(item, "manifest", icon.manifest);
            readStringOpt(item, "cover", icon.cover);
            readStringOpt(item, "theme", icon.theme);
            if (item.contains("builtin") && item["builtin"].is_boolean())
                icon.builtin = item["builtin"].get<bool>();
            if (item.contains("colour") && item["colour"].is_boolean())
                icon.colour = item["colour"].get<bool>();

            if (icon.id.empty())
                continue;
            if (icon.name.empty())
                icon.name = icon.id;
            if (icon.path.empty())
                icon.path = "icons/" + icon.id;
            if (icon.manifest.empty())
                icon.manifest = icon.path + "/pack.json";
            if (icon.cover.empty())
                icon.cover = icon.path + "/preview.png";

            // Paths stay relative (the installer walks them); remember the base.
            icon.catalogUrl = url;
            snapshot.icons.push_back(std::move(icon));
        }

        std::sort(snapshot.icons.begin(), snapshot.icons.end(),
                  [](const IconEntry& lhs, const IconEntry& rhs) {
            if (lhs.builtin != rhs.builtin)
                return lhs.builtin; // built-ins first, like the console list
            if (lhs.name != rhs.name)
                return lhs.name < rhs.name;
            return lhs.id < rhs.id;
        });
    }

    auto themesIt = root.find("themes");
    if (themesIt == root.end() || !themesIt->is_array())
        throw std::runtime_error("Catalog is missing the themes array");

    snapshot.entries.reserve(themesIt->size());
    bool previewFallbackDisabled = false;
    for (const auto& item : *themesIt) {
        if (!item.is_object())
            continue;

        Entry entry;
        readStringOpt(item, "id", entry.id);
        readStringOpt(item, "name", entry.name);
        readStringOpt(item, "author", entry.author);
        readStringOpt(item, "version", entry.version);
        readStringOpt(item, "path", entry.path);
        readStringOpt(item, "manifest", entry.manifest);
        readStringOpt(item, "cover", entry.cover);
        readStringOpt(item, "package", entry.package);
        if (auto it = item.find("packageBytes"); it != item.end() && it->is_number_unsigned())
            entry.packageBytes = it->get<std::uint64_t>();
        if (auto it = item.find("installedBytes"); it != item.end() && it->is_number_unsigned())
            entry.installedBytes = it->get<std::uint64_t>();
        readStringOpt(item, "thumbSheet", entry.thumbSheet);
        readStringOpt(item, "thumbSheetHd", entry.thumbSheetHd);
        {
            auto readInt = [&](const char* key, int& out) {
                auto it = item.find(key);
                if (it != item.end() && it->is_number_integer()) out = it->get<int>();
            };
            readInt("thumbCols", entry.thumbCols);
            readInt("thumbRows", entry.thumbRows);
            auto fpsIt = item.find("thumbFps");
            if (fpsIt != item.end() && fpsIt->is_number())
                entry.thumbFps = fpsIt->get<float>();
        }
        // Uma folha sem grade valida nao anima; melhor cair na capa parada do
        // que percorrer recortes inventados.
        if (entry.thumbCols < 1 || entry.thumbRows < 1)
            entry.thumbSheet.clear();
        readStringArrayOpt(item, "screenshots", entry.screenshots);
        entry.catalogUrl = url;

        std::string screenshot;
        readStringOpt(item, "screenshot", screenshot);
        appendUniqueString(entry.screenshots, screenshot);

        // Catalog may advertise icons/audio; otherwise peek theme.json (rare today).
        if (item.contains("hasIcons") && item["hasIcons"].is_boolean())
            entry.hasIcons = item["hasIcons"].get<bool>();
        else if (item.contains("has_icons") && item["has_icons"].is_boolean())
            entry.hasIcons = item["has_icons"].get<bool>();
        else if (item.contains("icons"))
            entry.hasIcons = jsonDeclaresIcons(item);
        entry.hasMusic = jsonDeclaresMusic(item);
        entry.hasSounds = jsonDeclaresSounds(item);

        // The id eventually becomes a directory name on the SD card. Do not
        // retain an entry that could alter that destination.
        if (!safeThemeId(entry.id) || entry.name.empty())
            continue;
        if (entry.manifest.empty())
            entry.manifest = deriveManifestPath(entry.path);
        // Only when there is no cover at all (fork rule, kept over PoloNX's
        // "also when badges are unknown").
        //
        // This used to fire when either the cover or the screenshots were
        // missing, and a catalogue that lists covers but not screenshots -- ours
        // does -- made it fetch all 56 manifests on every refresh. With the
        // network down that became 112 failing requests, each tearing down and
        // rebuilding the HTTP stack. PoloNX now refreshes the catalogue on every
        // Theme Shop open, which would make that cost per open. Badges read from
        // the index itself (above) still show; a theme whose index entry does
        // not declare them gets them once its manifest is fetched for a cover.
        //
        // Extra screenshots are a nicety. A missing cover is the only thing
        // worth a round trip, because without it the entry has nothing to show.
        if (entry.cover.empty() && !entry.manifest.empty() && !previewFallbackDisabled) {
            try {
                PreviewData preview = derivePreviewDataFromManifest(url, entry);
                if (entry.cover.empty())
                    entry.cover = preview.cover;
                for (auto& screenshotPath : preview.screenshots)
                    appendUniqueString(entry.screenshots, std::move(screenshotPath));
                if (preview.fetchedManifest) {
                    if (!entry.hasIcons)
                        entry.hasIcons = preview.hasIcons;
                    if (!entry.hasMusic)
                        entry.hasMusic = preview.hasMusic;
                    if (!entry.hasSounds)
                        entry.hasSounds = preview.hasSounds;
                }
            } catch (const std::exception& ex) {
                // One failure stops the rest. If the network is down for this
                // entry it is down for the other fifty-five, and retrying each
                // of them is how a missing connection turned into a frozen menu.
                previewFallbackDisabled = true;
                DebugLog::log("[themeshop] manifest preview fallback failed for %s: %s "
                              "(skipping the rest)",
                              entry.id.c_str(),
                              ex.what());
            }
        }

        ensureCoverFirst(entry);

        // Preview paths are relative to their own index. Resolving them here
        // means every consumer downstream can stay ignorant of which catalogue
        // an entry came from — only the installer, which walks a whole theme
        // directory, still needs the origin.
        entry.cover = absoluteFromCatalog(url, entry.cover);
        for (auto& shot : entry.screenshots)
            shot = absoluteFromCatalog(url, shot);

        snapshot.entries.push_back(std::move(entry));
    }

    std::sort(snapshot.entries.begin(), snapshot.entries.end(), [](const Entry& lhs, const Entry& rhs) {
        if (lhs.name != rhs.name)
            return lhs.name < rhs.name;
        return lhs.id < rhs.id;
    });

    if (snapshot.entries.empty())
        snapshot.loading.succeed("Catalog is empty.");
    else
        snapshot.loading.succeed(std::to_string(snapshot.entries.size()) + " themes available.");

    return snapshot;
}
