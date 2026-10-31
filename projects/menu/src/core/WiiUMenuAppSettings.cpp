#include "WiiUMenuApp.hpp"
#include "icons/IconPackInstaller.hpp"
#include "icons/IconPackManager.hpp"
#include "themeshop/ThemePackageInstaller.hpp"
#include "settings/SettingsGlassTuning.hpp"
#include "themeshop/ThemeHttp.hpp"
#include "widgets/AdaptiveIconLoad.hpp"
#include "widgets/GlossyIcon.hpp"
#include "DebugLog.hpp"

#include <switchu/fs_remove.hpp>
#include <switchu/sd_commit.hpp>
#include <switchu/self_uninstall.hpp>
#include <switchu/offline_titles.hpp>

#include <nxui/core/I18n.hpp>
#include <fmt/format.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cctype>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <cstdio>
#include <nlohmann/json.hpp>
#include <system_error>
#include <utility>
#include <unordered_map>
#include <unordered_set>
#include <cmath>

// Defined in WiiUMenuApp.cpp (shared theme-sound helpers).
std::string resolveThemeSoundBase(const std::string& installPath);
size_t appendMp3TracksFromDir(AudioManager& audio,
                              const std::string& musicDir,
                              const std::string& albumFolder);

namespace {

// Deleting a theme goes through the POSIX walk in switchu/fs_remove.hpp.
//
// This used std::filesystem::remove_all and discarded the error, which on the
// console meant the folder simply stayed. Removing a live wallpaper freed the
// space in the list and nothing on the card.
bool removeDirectoryRecursive(const std::string& path, std::string* failedPath = nullptr) {
    return switchu::removeRecursive(path, failedPath);
}

bool pathExists(const std::string& path) {
    if (path.empty())
        return false;

    std::error_code ec;
    return std::filesystem::exists(path, ec);
}

bool stageSelfUninstallRequest() {
    namespace uninstall = switchu::self_uninstall;

    std::error_code ec;
    std::filesystem::create_directories(uninstall::kDirectory, ec);
    if (ec)
        return false;

    // A complete temporary marker is renamed into place only after it has been
    // flushed. The daemon ignores a missing request, so a power loss cannot
    // turn a partial write into an uninstall request. Commit it before rename:
    // FAT has no atomic rename durability guarantee across sudden power loss.
    std::remove(uninstall::kRequestTemporary);
    std::FILE* request = std::fopen(uninstall::kRequestTemporary, "wb");
    if (!request)
        return false;
    const bool wrote = std::fwrite(uninstall::kRequestContents, 1,
                                   sizeof(uninstall::kRequestContents) - 1, request)
                       == sizeof(uninstall::kRequestContents) - 1;
    const bool closed = std::fclose(request) == 0;
    if (!wrote || !closed) {
        std::remove(uninstall::kRequestTemporary);
        return false;
    }
    if (!switchu::commitSdCard("write self-uninstall marker"))
        return false;

    // A prior failed attempt leaves a valid request marker behind. Replacing it
    // is intentional: this confirmation stages a fresh, fully committed request.
    if (std::remove(uninstall::kRequest) != 0 && errno != ENOENT)
        return false;
    if (std::rename(uninstall::kRequestTemporary, uninstall::kRequest) != 0)
        return false;
    if (switchu::commitSdCard("stage self-uninstall"))
        return true;

    // The marker might be durable even though the commit call reported an
    // error; retain it rather than falsely claim cancellation and re-enable a
    // removal the player already confirmed. The daemon validates it on boot.
    return false;
}

std::string joinPath(const std::string& base, const std::string& name) {
    if (base.empty())
        return name;
    if (name.empty())
        return base;
    if (base.back() == '/')
        return base + name;
    return base + "/" + name;
}

std::string themeFolderFromInstallPath(const std::string& installPath) {
    if (installPath.empty())
        return {};
    const auto pos = installPath.find_last_of("/\\");
    return (pos == std::string::npos) ? installPath : installPath.substr(pos + 1);
}

std::string themeFolderFromPreset(const ThemePreset& preset) {
    std::string folder = themeFolderFromInstallPath(preset.installPath);
    if (!folder.empty())
        return folder;
    std::string id = preset.id.empty() ? preset.name : preset.id;
    if (id.rfind("package:", 0) == 0)
        id = id.substr(8);
    return id;
}

std::string resolvedThemeIconPackId(const ThemePreset& preset, const std::string& themeFolder) {
    if (!preset.icons.packId.empty())
        return preset.icons.packId;
    return themeFolder;
}

bool writeIconPackManifest(const std::string& destDir,
                           const std::string& packId,
                           const std::string& name,
                           const std::string& author) {
    if (destDir.empty() || packId.empty())
        return false;
    nlohmann::json root = {
        {"id", packId},
        {"name", name.empty() ? packId : name},
        {"author", author.empty() ? "Theme" : author},
        {"version", "1.0.0"},
    };
    std::error_code ec;
    std::filesystem::create_directories(destDir, ec);
    std::ofstream out(joinPath(destDir, "pack.json"), std::ios::binary | std::ios::trunc);
    if (!out)
        return false;
    out << root.dump(2) << '\n';
    return static_cast<bool>(out);
}

bool isAbsoluteThemePath(const std::string& path) {
    return (!path.empty() && path.front() == '/') || (path.find(":/") != std::string::npos);
}

std::string trimWhitespace(const std::string& value) {
    const auto begin = value.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) return {};
    const auto end = value.find_last_not_of(" \t\r\n");
    return value.substr(begin, end - begin + 1);
}

std::string trimSlashes(std::string path) {
    while (!path.empty() && path.front() == '/')
        path.erase(path.begin());
    while (!path.empty() && path.back() == '/')
        path.pop_back();
    return path;
}

bool readJsonString(const nlohmann::json& obj, const char* key, std::string& out) {
    auto it = obj.find(key);
    if (it == obj.end() || !it->is_string())
        return false;
    out = it->get<std::string>();
    return !out.empty();
}

std::string installedThemePreviewPath(const ThemePreset& preset) {
    if (preset.installPath.empty())
        return {};

    auto resolveLocal = [&](const std::string& raw) -> std::string {
        if (raw.empty() || raw.rfind("http://", 0) == 0 || raw.rfind("https://", 0) == 0)
            return {};
        std::string path = isAbsoluteThemePath(raw) ? raw : joinPath(preset.installPath, trimSlashes(raw));
        return pathExists(path) ? path : std::string();
    };

    nlohmann::json manifest;
    std::ifstream input(joinPath(preset.installPath, "theme.json"));
    if (input) {
        try {
            input >> manifest;
        } catch (...) {
            manifest = nlohmann::json{};
        }
    }

    if (manifest.is_object()) {
        std::string raw;
        if ((readJsonString(manifest, "cover", raw)
             || readJsonString(manifest, "screenshot", raw)
             || readJsonString(manifest, "thumbnail", raw))) {
            std::string path = resolveLocal(raw);
            if (!path.empty())
                return path;
        }

        auto previewIt = manifest.find("preview");
        if (previewIt != manifest.end() && previewIt->is_object()) {
            raw.clear();
            if ((readJsonString(*previewIt, "cover", raw)
                 || readJsonString(*previewIt, "screenshot", raw)
                 || readJsonString(*previewIt, "thumbnail", raw))) {
                std::string path = resolveLocal(raw);
                if (!path.empty())
                    return path;
            }

            auto screenshotsIt = previewIt->find("screenshots");
            if (screenshotsIt != previewIt->end() && screenshotsIt->is_array()) {
                for (const auto& value : *screenshotsIt) {
                    if (!value.is_string())
                        continue;
                    std::string path = resolveLocal(value.get<std::string>());
                    if (!path.empty())
                        return path;
                }
            }
        }

        auto screenshotsIt = manifest.find("screenshots");
        if (screenshotsIt != manifest.end() && screenshotsIt->is_array()) {
            for (const auto& value : *screenshotsIt) {
                if (!value.is_string())
                    continue;
                std::string path = resolveLocal(value.get<std::string>());
                if (!path.empty())
                    return path;
            }
        }
    }

    static constexpr const char* kFallbacks[] = {
        "media/screenshots/00.png",
        "media/screenshots/cover.png",
        "preview/cover.png",
        "preview/screenshot.png",
        "preview/cover.jpg",
        "preview/screenshot.jpg",
        "screenshots/cover.png",
        "screenshots/screenshot.png",
        "cover.png",
        "screenshot.png",
    };
    for (const char* fallback : kFallbacks) {
        std::string path = joinPath(preset.installPath, fallback);
        if (pathExists(path))
            return path;
    }

    return {};
}

const char* safeLogPath(const std::string& path) {
    return path.empty() ? "<empty>" : path.c_str();
}

const char* themeSourceName(ThemePresetSource source) {
    switch (source) {
        case ThemePresetSource::BuiltIn: return "builtin";
        case ThemePresetSource::UserPreset: return "user";
        case ThemePresetSource::InstalledPackage: return "package";
        default: return "unknown";
    }
}

WaraWaraBackground::Layout toBackgroundLayout(ThemeBackgroundLayout layout) {
    switch (layout) {
        case ThemeBackgroundLayout::Grid:
            return WaraWaraBackground::Layout::Grid;
        case ThemeBackgroundLayout::Floating:
        default:
            return WaraWaraBackground::Layout::Floating;
    }
}

WaraWaraBackground::ShapeSet toBackgroundShapeSet(ThemeBackgroundShapeSet shapeSet) {
    switch (shapeSet) {
        case ThemeBackgroundShapeSet::Circle:
            return WaraWaraBackground::ShapeSet::Circle;
        case ThemeBackgroundShapeSet::Triangle:
            return WaraWaraBackground::ShapeSet::Triangle;
        case ThemeBackgroundShapeSet::Square:
            return WaraWaraBackground::ShapeSet::Square;
        case ThemeBackgroundShapeSet::Diamond:
            return WaraWaraBackground::ShapeSet::Diamond;
        case ThemeBackgroundShapeSet::Hexagon:
            return WaraWaraBackground::ShapeSet::Hexagon;
        case ThemeBackgroundShapeSet::Mixed:
        default:
            return WaraWaraBackground::ShapeSet::Mixed;
    }
}

WaraWaraBackground::Symmetry toBackgroundSymmetry(ThemeBackgroundSymmetry symmetry) {
    switch (symmetry) {
        case ThemeBackgroundSymmetry::MirrorX:
            return WaraWaraBackground::Symmetry::MirrorHorizontal;
        case ThemeBackgroundSymmetry::MirrorY:
            return WaraWaraBackground::Symmetry::MirrorVertical;
        case ThemeBackgroundSymmetry::Quad:
            return WaraWaraBackground::Symmetry::Quad;
        case ThemeBackgroundSymmetry::None:
        default:
            return WaraWaraBackground::Symmetry::None;
    }
}

std::string sourceLabel(ThemePresetSource source) {
    auto& i18n = nxui::I18n::instance();
    switch (source) {
        case ThemePresetSource::BuiltIn:
            return i18n.tr("themeshop.source.builtin", "Built-in");
        case ThemePresetSource::UserPreset:
            return i18n.tr("themeshop.source.custom", "Custom");
        case ThemePresetSource::InstalledPackage:
            return i18n.tr("themeshop.source.community", "Community");
    }

    return i18n.tr("themeshop.source.unknown", "Unknown");
}

std::string defaultThemeRef() {
    // Keep first-run behavior aligned with the author's original static theme.
    // Animated wallpapers remain available only through separately installed
    // themes, rather than inflating every SwitchU installation.
    return "builtin:Default Dark";
}

} // namespace

// Radius 3 at half resolution reached six screen pixels; at full resolution it
// reaches three, which is the sharpening that was reported as too strong on a
// dark theme. 0.4 maps back to a reach of six, so the default looks like what
// it replaced and the slider goes either way from there.
void WiiUMenuApp::applyGlassSharpness(float sharpness) {
    const float s = std::clamp(sharpness, 0.f, 1.f);
    settings::debug::settingsGlassTuning().preBlurRadius = 9.f - s * 7.f;
}

void WiiUMenuApp::createSettings() {
    if (m_settings) return;

    m_settings = std::make_shared<SettingsScreen>();
    if (m_overlayLayer) {
        m_overlayLayer->addChild(m_settings);
    }
    m_settings->setFont(&m_fontNormal);
    m_settings->setSmallFont(&m_fontSmall);
    m_settings->setTheme(&m_theme);
    m_settings->setWireframeState(m_showWireframe);
    m_settings->setGridLayoutState(m_config.gridColumns, m_config.gridRows);
    m_settings->setUiLanguageOverride(m_config.uiLanguageOverride);
    m_settings->setDefaultProfileState(m_config.defaultProfileEnabled,
                                       m_config.defaultProfileUid);
    m_settings->setClockUse12HourState(m_config.clockUse12Hour);
    m_settings->setAccessibilityEnabledState(m_config.accessibilityEnabled);
    m_settings->setAccessibilitySpeechState(m_config.accessibilitySpeakHints,
                                            m_config.accessibilitySpeakContextEveryFocus,
                                            m_config.accessibilitySpeakPosition,
                                            m_config.accessibilitySpeechRate);
    m_settings->setAccessibilitySpeechPreferences(m_config.accessibilitySpeakHints,
                                                  m_config.accessibilitySpeakPosition);
    m_settings->setSteamGridDbState(m_config.steamGridDbEnabled,
                                    !m_config.steamGridDbApiKey.empty(),
                                    m_config.steamGridDbShowInGrid,
                                    m_config.steamGridDbShowInDynamicLine,
                                    m_config.steamGridDbShowInFolders,
                                    m_config.steamGridDbShowPosterInDynamicLine,
                                    m_config.steamGridDbShowLogoInDynamicLine);
    m_settings->setMetadataKeyState(!m_config.rawgApiKey.empty(),
                                    !m_config.igdbClientId.empty(),
                                    !m_config.igdbClientSecret.empty());

    m_settings->onNavigateSfx([this]() { m_audio.playSfx(Sfx::Navigate); });
    m_settings->onActivateSfx([this]() { m_audio.playSfx(Sfx::Activate); });
    m_settings->onCloseSfx([this]() { m_audio.playSfx(Sfx::ModalHide); });
    m_settings->onAccessibilityAnnouncement([this](const std::string& text) {
        m_accessibility.announce(text);
    });
    m_settings->onAccessibilityStructuredAnnouncement([this](const std::string& context,
                                                             const std::string& position,
                                                             const std::string& summary,
                                                             bool forceRepeat,
                                                             bool forceContext) {
        m_accessibility.announceStructuredFocus(context, position, summary, forceRepeat, forceContext);
    });
    m_settings->onToggleSfx([this](bool on) {
        m_audio.playSfx(on ? Sfx::ThemeToggle : Sfx::ToggleOff);
    });
    m_settings->onSliderSfx([this](bool up) {
        m_audio.playSfx(up ? Sfx::SliderUp : Sfx::SliderDown);
    });
    m_settings->onWireframeChange([this](bool enabled) {
        m_showWireframe = enabled;
        app().renderer().setBoxWireframeEnabled(enabled);
    });
    m_settings->onGridColumnsChange([this](int cols) {
        cols = std::clamp(cols, 3, 8);
        if (m_config.gridColumns == cols)
            return;
        m_config.gridColumns = cols;
        reflowHomeGrid();
    });
    m_settings->onGridRowsChange([this](int rows) {
        rows = std::clamp(rows, 2, 5);
        if (m_config.gridRows == rows)
            return;
        m_config.gridRows = rows;
        reflowHomeGrid();
    });
    m_settings->onUiLanguageChange([this](const std::string& tag) {
        m_config.uiLanguageOverride = tag;
        if (m_settings) m_settings->setUiLanguageOverride(tag);
        applyUiLanguage();
        app().gpu().waitIdle();
        m_fontNormal.clearCache();
        m_fontSmall.clearCache();
        m_fontClock.clearCache();
        m_settingsNeedRefresh = true;
        // Switching the catalogue only changes what tr() returns the next time
        // it is asked. Whatever asked once and kept the answer stays in the old
        // language until it is built again, which is why a restart was needed:
        //  - A widget tile's header and its two lines are read from tr() in
        //    makeIcon(), so the home model is composed again here.
        //  - The SwitchU screen, the update tab and its changelog among it,
        //    builds its strings when it is constructed and is then cached, so
        //    dropping it means the next open builds it in the new language.
        //    Only while it is off screen: the language lives in Settings, so it
        //    always is, but freeing a live overlay would take its textures too.
        // The action hints and the title pill are rebuilt every frame and were
        // never part of this.
        //
        // The same fix went into WiiUMenuAppUpstreamSettings.cpp first, which is
        // the copy of this handler inside an #if 0 block. It compiled, shipped
        // in 2.3.0 and did nothing.
        reflowHomeGrid();
        if (m_themeShop && !m_themeShop->isActive())
            m_themeShop.reset();
    });
    m_settings->onDefaultProfileChange([this](const std::string& uidHex) {
        m_config.defaultProfileEnabled = !uidHex.empty();
        m_config.defaultProfileUid = uidHex;
        if (m_settings)
            m_settings->setDefaultProfileState(m_config.defaultProfileEnabled,
                                               m_config.defaultProfileUid);
    });
    m_settings->onAddUser([this]() {
        m_launcher.launchUserCreator();
    });
    m_settings->onClockUse12HourChange([this](bool enabled) {
        if (m_config.clockUse12Hour == enabled)
            return;
        m_config.clockUse12Hour = enabled;
        if (m_clock)
            m_clock->setUse12HourClock(enabled);
        m_lockScreen.setUse12HourClock(enabled);
    });
    m_settings->onAccessibilityEnabledChange([this](bool enabled) {
        if (m_config.accessibilityEnabled == enabled)
            return;
        m_config.accessibilityEnabled = enabled;
        if (m_settings)
            m_settings->setAccessibilityVoiceEnabled(enabled);
        if (m_themeShop)
            m_themeShop->setAccessibilityVoiceEnabled(enabled);
        if (m_gameGallery)
            m_gameGallery->setAccessibilityVoiceEnabled(enabled);
        if (m_gameMods)
            m_gameMods->setAccessibilityVoiceEnabled(enabled);
        if (m_gameCheats)
            m_gameCheats->setAccessibilityVoiceEnabled(enabled);
        if (m_gameDetails)
            m_gameDetails->setAccessibilityVoiceEnabled(enabled);
        if (enabled) {
            m_accessibility.setEnabled(true);
            m_accessibility.announce(nxui::I18n::instance().tr(
                "accessibility.speech.enabled",
                "Voice guidance enabled."));
        } else {
            m_accessibility.announceAndDisable(nxui::I18n::instance().tr(
                "accessibility.speech.disabled",
                "Voice guidance disabled."));
        }
    });
    m_settings->onAccessibilitySpeakHintsChange([this](bool enabled) {
        m_config.accessibilitySpeakHints = enabled;
        m_accessibility.setSpeakHints(enabled);
        if (m_settings)
            m_settings->setAccessibilitySpeechPreferences(m_config.accessibilitySpeakHints,
                                                          m_config.accessibilitySpeakPosition);
        if (m_themeShop)
            m_themeShop->setAccessibilitySpeechPreferences(m_config.accessibilitySpeakHints,
                                                           m_config.accessibilitySpeakPosition);
        if (m_gameGallery)
            m_gameGallery->setAccessibilitySpeechPreferences(m_config.accessibilitySpeakHints,
                                                             m_config.accessibilitySpeakPosition);
        if (m_gameMods)
            m_gameMods->setAccessibilitySpeechPreferences(m_config.accessibilitySpeakHints,
                                                          m_config.accessibilitySpeakPosition);
        if (m_gameCheats)
            m_gameCheats->setAccessibilitySpeechPreferences(m_config.accessibilitySpeakHints,
                                                            m_config.accessibilitySpeakPosition);
        if (m_gameDetails)
            m_gameDetails->setAccessibilitySpeechPreferences(m_config.accessibilitySpeakHints,
                                                              m_config.accessibilitySpeakPosition);
        if (m_dialog)
            m_dialog->setAccessibilitySpeechPreferences(m_config.accessibilitySpeakHints,
                                                        m_config.accessibilitySpeakPosition);
        if (m_userSelect)
            m_userSelect->setAccessibilitySpeechPreferences(m_config.accessibilitySpeakHints,
                                                            m_config.accessibilitySpeakPosition);
    });
    m_settings->onAccessibilitySpeakContextEveryFocusChange([this](bool enabled) {
        m_config.accessibilitySpeakContextEveryFocus = enabled;
        m_accessibility.setSpeakContextEveryFocus(enabled);
    });
    m_settings->onAccessibilitySpeakPositionChange([this](bool enabled) {
        m_config.accessibilitySpeakPosition = enabled;
        if (m_settings)
            m_settings->setAccessibilitySpeechPreferences(m_config.accessibilitySpeakHints,
                                                          m_config.accessibilitySpeakPosition);
        if (m_themeShop)
            m_themeShop->setAccessibilitySpeechPreferences(m_config.accessibilitySpeakHints,
                                                           m_config.accessibilitySpeakPosition);
        if (m_gameGallery)
            m_gameGallery->setAccessibilitySpeechPreferences(m_config.accessibilitySpeakHints,
                                                             m_config.accessibilitySpeakPosition);
        if (m_gameMods)
            m_gameMods->setAccessibilitySpeechPreferences(m_config.accessibilitySpeakHints,
                                                          m_config.accessibilitySpeakPosition);
        if (m_gameCheats)
            m_gameCheats->setAccessibilitySpeechPreferences(m_config.accessibilitySpeakHints,
                                                            m_config.accessibilitySpeakPosition);
        if (m_gameDetails)
            m_gameDetails->setAccessibilitySpeechPreferences(m_config.accessibilitySpeakHints,
                                                              m_config.accessibilitySpeakPosition);
        if (m_dialog)
            m_dialog->setAccessibilitySpeechPreferences(m_config.accessibilitySpeakHints,
                                                        m_config.accessibilitySpeakPosition);
        if (m_userSelect)
            m_userSelect->setAccessibilitySpeechPreferences(m_config.accessibilitySpeakHints,
                                                            m_config.accessibilitySpeakPosition);
    });
    m_settings->onAccessibilitySpeechRateChange([this](int rate) {
        m_config.accessibilitySpeechRate = std::clamp(rate, 120, 320);
        m_accessibility.setSpeechRate(m_config.accessibilitySpeechRate);
    });
    m_settings->onNetConnect([this]() {
        m_pendingNetConnect = true;
        m_settings->hide();
        m_navigator.resetToHome();
    });
    m_settings->onSteamGridDbEnabledChange([this](bool enabled) {
        m_config.steamGridDbEnabled = enabled;
        if (m_steamGridDbBackdrop) {
            m_steamGridDbBackdrop->setEnabled(enabled);
            if (enabled) showFocusedSteamGridDbArtwork(true);
        }
        syncSteamGridDbLogoArea();
        syncSingleRowPosterPresentation(true);
    });
    m_settings->onSteamGridDbViewFlagsChange([this](bool grid, bool line, bool folders) {
        m_config.steamGridDbShowInGrid = grid;
        m_config.steamGridDbShowInDynamicLine = line;
        m_config.steamGridDbShowInFolders = folders;
        m_config.save();
        if (m_steamGridDbBackdrop) {
            // The focused title may already have been classified as "no
            // artwork" or kept a stale set from before the toggle; drop both so
            // the hero comes back instead of staying blank.
            m_steamGridDbBackdrop->invalidateArtworkCaches();
            m_steamGridDbBackdrop->setEnabled(m_config.steamGridDbEnabled
                                              && steamGridDbArtworkAllowedHere());
        }
        showFocusedSteamGridDbArtwork(true);
    });
    m_settings->onSteamGridDbPosterInLineChange([this](bool enabled) {
        m_config.steamGridDbShowPosterInDynamicLine = enabled;
        m_config.save();
        syncSingleRowPosterPresentation(true);
    });
    m_settings->onSteamGridDbLogoInLineChange([this](bool enabled) {
        m_config.steamGridDbShowLogoInDynamicLine = enabled;
        m_config.save();
        syncSteamGridDbLogoArea();
    });
    m_settings->onSteamGridDbApiKeyRequest([this]() {
        editSteamGridDbApiKey();
    });
    m_settings->onMetadataKeyRequest([this](int index) {
        editMetadataKey(index);
    });
    m_settings->onRotateLogsRequest([this]() {
        auto& i18n = nxui::I18n::instance();
        const Result rc = m_launcher.rotateLogs();
        switchu::commitSdCard("log rotation");
        if (m_settings) {
            m_settings->requestToast(
                R_SUCCEEDED(rc)
                    ? i18n.tr("settings.system.save_logs_done",
                              "Logs saved. Copy menu-*.log and daemon-*.log from config/SwitchU.")
                    : i18n.tr("settings.system.save_logs_failed",
                              "The menu log was saved; the daemon did not answer."),
                4.5f);
        }
    });
    m_settings->onSendLogsRequest([this]() {
        confirmSendLogs();
    });
    m_settings->onConsoleNicknameRequest([this]() {
        auto& i18n = nxui::I18n::instance();
        SetSysDeviceNickName current{};
        setsysGetDeviceNickname(&current);
        requestTextEntry(
            i18n.tr("settings.system.console_nickname", "Console Nickname"),
            i18n.tr("settings.system.console_nickname_guide", "Enter a name for this console"),
            current.nickname, 32, false,
            [this](const std::string& value) {
                const std::string trimmed = trimWhitespace(value);
                if (trimmed.empty())
                    return;
                SetSysDeviceNickName nickname{};
                std::snprintf(nickname.nickname, sizeof(nickname.nickname), "%s",
                              trimmed.c_str());
                const Result rc = setsysSetDeviceNickname(&nickname);
                DebugLog::log("[settings] console nickname rc=0x%X", rc);
                if (m_settings)
                    m_settings->rebuildCurrentTab();
            });
    });
    m_settings->onSteamGridDbScrapeRequest([this]() {
        startSteamGridDbScrape();
    });
    m_settings->onControllerPairing([this]() {
        if (m_settings) m_settings->hide();
        m_navigator.resetToHome();
        scheduleLeaveCapture([this]() { m_launcher.launchControllerPairing(); });
    });
    m_settings->onControllerRemapping([this]() {
        if (m_settings) m_settings->hide();
        m_navigator.resetToHome();
        scheduleLeaveCapture([this]() { m_launcher.launchControllerRemapping(); });
    });
    m_settings->onControllerTest([this]() {
        if (!m_controllerTest) return;
        // The controller test is created in onCreate; the settings overlay is
        // created lazily the first time it is opened, so it is appended to the
        // overlay layer after it and painted over it. Focus moved to a screen
        // the player could not see: the settings panel stayed on screen and
        // stopped responding, which on hardware read as a frozen console.
        raiseOverlay(m_controllerTest);
        m_navigator.navigate(switchu::navigation::Route::ControllerTest);
        m_controllerTest->show();
        focusManager().setFocus(m_controllerTest.get());
        m_audio.playSfx(Sfx::ModalShow);
    });
    m_settings->onSoftwareDelete([this](uint64_t titleId, const std::string& title) {
        startSoftwareDeletion(titleId, title, false);
    });
    m_settings->onSleepRequest([this]() {
        if (m_settings) m_settings->hide();
        m_launcher.enterSleep();
    });
    m_settings->onShutdownRequest([this]() {
        if (m_settings) m_settings->hide();
        m_launcher.shutdown();
    });
    m_settings->onRebootRequest([this]() {
        if (m_settings) m_settings->hide();
        m_launcher.reboot();
    });
    m_settings->onDialogRequest([this](const std::string& title,
                                       const std::string& msg,
                                       std::vector<SettingsScreen::DialogButtonDef> buttons) {
        if (!m_dialog) return;
        std::vector<OverlayDialog::ButtonDef> dlgButtons;
        bool preserveReturnFocus = (m_dialog && m_dialog->isActive() && focusManager().current() == m_dialog.get() && m_dialogReturnFocus != nullptr);
        for (size_t i = 0; i < buttons.size(); ++i) {
            auto cb = buttons[i].onPress;
            bool isLast = (i == buttons.size() - 1);
            if (buttons.size() == 1) {
                dlgButtons.push_back({buttons[i].label, [this, cb]() {
                    m_audio.playSfx(Sfx::ConfirmPositive);
                    m_dialog->hide();
                    if (cb) cb();
                }, true});
            } else if (isLast) {
                dlgButtons.push_back({buttons[i].label, [cb]() { if (cb) cb(); }, true});
            } else {
                dlgButtons.push_back({buttons[i].label, [this, cb]() {
                    m_audio.playSfx(Sfx::ConfirmPositive);
                    m_dialog->hide();
                    if (cb) cb();
                }, true});
            }
        }
        if (!preserveReturnFocus)
            m_dialogReturnFocus = focusManager().current();
        m_dialog->show(title, msg, std::move(dlgButtons));
        focusManager().setFocus(m_dialog.get());
    });
    m_settings->onDateTimeEditorRequest(
        [this](const TabbedOverlayScreen::DateTimeEditorValue& initial,
               TabbedOverlayScreen::DateTimeCommitCb onCommit) {
            if (!m_dialog) return;
            m_dialogReturnFocus = m_settings.get();
            OverlayDialog::DateTimeValue value;
            value.year = initial.year;
            value.month = initial.month;
            value.day = initial.day;
            value.hour = initial.hour;
            value.minute = initial.minute;
            m_dialog->showDateTimeEditor(
                value,
                [this, onCommit = std::move(onCommit)](
                    const OverlayDialog::DateTimeValue& edited) mutable {
                    TabbedOverlayScreen::DateTimeEditorValue committed;
                    committed.year = edited.year;
                    committed.month = edited.month;
                    committed.day = edited.day;
                    committed.hour = edited.hour;
                    committed.minute = edited.minute;
                    const bool saved = !onCommit || onCommit(committed);
                    if (saved)
                        m_clockService.invalidate();
                    return saved;
                });
            focusManager().setFocus(m_dialog.get());
        });
    // Timezone picker and its search filter (PoloNX #131).
    m_settings->onTimezonePickerRequest(
        [this](const std::vector<std::string>& zones, int selectedIndex,
               TabbedOverlayScreen::TimezoneSelectCb onSelect,
               TabbedOverlayScreen::TimezoneSearchCb onSearch) {
            if (!m_dialog) return;
            m_dialogReturnFocus = m_settings.get();
            m_dialog->showTimezonePicker(
                zones,
                selectedIndex,
                [this, onSelect = std::move(onSelect)](const std::string& zone) mutable {
                    if (onSelect) onSelect(zone);
                    m_clockService.invalidate();
                },
                [onSearch = std::move(onSearch)]() mutable {
                    if (onSearch) onSearch();
                });
            focusManager().setFocus(m_dialog.get());
        });
    m_settings->onTextEntryRequest(
        [this](const std::string& title, const std::string& guide,
               const std::string& initial, int maxLength,
               TabbedOverlayScreen::TextEntryAcceptCb onAccept) {
            requestTextEntry(title, guide, initial, maxLength, false,
                             std::move(onAccept));
        });
    m_settings->onClosed([this]() {
        m_navigator.routeDidClose(switchu::navigation::Route::Settings);
        m_configSaveFuture = m_threadPool.submit([cfg = m_config]() {
            cfg.save();
        });
        DebugLog::log("[config] save queued");
        if (isCurrentFocusableWidget(m_sidebar.settingsButton())) {
            m_suppressNextNavigateSfx = true;
            focusManager().setFocus(m_sidebar.settingsButton());
        }
    });

    // This screen is created on first use, after the shared overlays already
    // exist. Restore their original z-order so settings-owned dialogs, launch
    // blackouts, and the pointer remain above it.
    raiseOverlay(m_dialog);
    raiseOverlay(m_progressDialog);
    raiseOverlay(m_launchAnim);
    raiseOverlay(m_pointerCursor);

}

void WiiUMenuApp::createQuickSettings() {
    if (m_quickSettings) return;

    m_quickSettings = std::make_shared<QuickSettingsOverlay>();
    if (m_overlayLayer) {
        m_overlayLayer->addChild(m_quickSettings);
    }
    m_quickSettings->setFont(&m_fontNormal);
    m_quickSettings->setSmallFont(&m_fontSmall);
    m_quickSettings->setIconFont(&m_fontIcons);
    m_quickSettings->setTheme(&m_theme);
    m_quickSettings->setInput(&app().input());

    QuickSettingsOverlay::Callbacks cbs;
    cbs.onBrightnessChanged = [](float /*val*/) {
    };
    cbs.onBgmVolumeChanged = [this](float val) {
        m_audio.setVolume(val);
        m_config.musicVolume = val;
    };
    cbs.onSfxVolumeChanged = [this](float val) {
        m_audio.setSfxVolume(val);
        m_config.sfxVolume = val;
    };
    cbs.onAirplaneModeToggled = [](bool /*enabled*/) {
    };
    cbs.onWifiToggled = [](bool /*enabled*/) {
    };
    cbs.onActivityLogRequested = [this]() {
        closeQuickSettings();
        openActivityLog();
    };
    cbs.onSleepRequested = [this]() {
        if (!m_dialog) return;
        auto& i18n = nxui::I18n::instance();
        m_audio.playSfx(Sfx::ModalShow);
        m_dialogReturnFocus = focusManager().current();
        raiseOverlay(m_dialog);
        m_dialog->show(
            i18n.tr("power.title", "Power"),
            i18n.tr("settings.sleep.sleep_confirm", "Put the console into sleep mode?"),
            {
                {i18n.tr("button.cancel", "Cancel"), []() {}, true},
                {i18n.tr("power.sleep", "Sleep"), [this]() {
#ifdef SWITCHU_MENU
                    m_audio.playSfx(Sfx::ConfirmPositive);
                    m_launcher.enterSleep();
#else
                    m_audio.playSfx(Sfx::ConfirmPositive);
                    app().requestExit();
#endif
                }, true}
            });
    };
    cbs.onRebootRequested = [this]() {
        if (!m_dialog) return;
        auto& i18n = nxui::I18n::instance();
        m_audio.playSfx(Sfx::ModalShow);
        m_dialogReturnFocus = focusManager().current();
        raiseOverlay(m_dialog);
        m_dialog->show(
            i18n.tr("power.title", "Power"),
            i18n.tr("settings.sleep.reboot_confirm", "Restart the console?"),
            {
                {i18n.tr("button.cancel", "Cancel"), []() {}, true},
                {i18n.tr("power.reboot", "Reboot"), [this]() {
#ifdef SWITCHU_MENU
                    m_audio.playSfx(Sfx::ConfirmPositive);
                    m_launcher.reboot();
#else
                    m_audio.playSfx(Sfx::ConfirmPositive);
                    app().requestExit();
#endif
                }, true}
            });
    };
    cbs.onShutdownRequested = [this]() {
        if (!m_dialog) return;
        auto& i18n = nxui::I18n::instance();
        m_audio.playSfx(Sfx::ModalShow);
        m_dialogReturnFocus = focusManager().current();
        raiseOverlay(m_dialog);
        m_dialog->show(
            i18n.tr("power.title", "Power"),
            i18n.tr("settings.sleep.shutdown_confirm", "Power off the console?"),
            {
                {i18n.tr("button.cancel", "Cancel"), []() {}, true},
                {i18n.tr("power.shutdown", "Shutdown"), [this]() {
#ifdef SWITCHU_MENU
                    m_audio.playSfx(Sfx::ConfirmPositive);
                    m_launcher.shutdown();
#else
                    m_audio.playSfx(Sfx::ConfirmPositive);
                    app().requestExit();
#endif
                }, true}
            });
    };
    cbs.onClose = [this]() {
        closeQuickSettings();
    };
    cbs.onNavigateSfx = [this]() { m_audio.playSfx(Sfx::Navigate); };
    cbs.onActivateSfx = [this]() { m_audio.playSfx(Sfx::Activate); };
    cbs.onToggleOffSfx = [this]() { m_audio.playSfx(Sfx::ToggleOff); };
    cbs.onBatterySaverToggled = [this](bool enabled) {
        applyBatterySaver(enabled);
    };
    m_quickSettings->setBatterySaver(m_config.batterySaver);
    m_quickSettings->setIconGlyphResolver([this](const char* fileName) {
        return resolveActiveIconGlyphPath(fileName);
    });

    m_quickSettings->setCallbacks(cbs);
}

void WiiUMenuApp::openQuickSettings() {
    createQuickSettings();
    const char* refused =
        !m_quickSettings ? "not created" :
        m_quickSettings->isActive() ? "already active" :
        m_editMode ? "edit mode" :
        (m_dialog && m_dialog->isActive()) ? "dialog active" :
        (m_userSelect && m_userSelect->isActive()) ? "user select active" : nullptr;
    if (refused) {
        DebugLog::log("[quicksettings] open refused: %s", refused);
        return;
    }

    m_dialogReturnFocus = focusManager().current();
    m_quickSettings->setInitialValues(0.5f, m_audio.volume(), m_audio.sfxVolume(), false, true);
    m_quickSettings->setBatteryStatus(m_consoleBatteryPercent, m_consoleBatteryCharging);
    m_quickSettings->setBatterySaver(m_config.batterySaver);
    m_quickSettings->setFrameTapLiteEngaged(m_frameTapStatus.viewerConnected());

    raiseOverlay(m_quickSettings);
    m_quickSettings->show();
    focusManager().setFocus(m_quickSettings.get());
    m_audio.playSfx(Sfx::ModalShow);
}

void WiiUMenuApp::closeQuickSettings() {
    if (!m_quickSettings || !m_quickSettings->isActive()) return;
    m_quickSettings->hide();
    m_config.save();
    if (isCurrentFocusableWidget(m_dialogReturnFocus)) {
        m_suppressNextNavigateSfx = true;
        focusManager().setFocus(m_dialogReturnFocus);
    } else if (m_grid) {
        auto* target = m_grid->focusManager().current();
        if (target && isCurrentFocusableWidget(target)) {
            m_suppressNextNavigateSfx = true;
            focusManager().setFocus(target);
        } else {
            auto icons = m_grid->allIcons();
            if (!icons.empty()) {
                m_suppressNextNavigateSfx = true;
                focusManager().setFocus(icons[0].get());
            }
        }
    }
    m_dialogReturnFocus = nullptr;
    m_audio.playSfx(Sfx::ModalHide);
}

void WiiUMenuApp::createActivityLog() {
    if (m_activityLog) return;

    m_activityLog = std::make_shared<ActivityLogScreen>();
    m_activityLog->setRect({32.f, 20.f, 1216.f, 642.f});
    if (m_overlayLayer) {
        m_overlayLayer->addChild(m_activityLog);
    }
    m_activityLog->setFont(&m_fontNormal);
    m_activityLog->setSmallFont(&m_fontSmall);
    m_activityLog->setTheme(&m_theme);
    m_activityLog->setThreadPool(&m_threadPool);
    m_activityLog->setRenderContext(&app().gpu(), &app().renderer());
    m_activityLog->setManager(&m_activityLogManager);
    m_activityLog->setInput(&app().input());
    m_activityLog->setIconProvider([this](std::uint64_t titleId) -> nxui::Texture* {
        if (m_grid) {
            for (const auto& icon : m_grid->allIcons()) {
                if (icon && icon->titleId() == titleId && icon->texture()) {
                    return icon->texture();
                }
            }
        }
        return nullptr;
    });

    m_activityLog->onNavigateSfx([this]() { m_audio.playSfx(Sfx::Navigate); });
    m_activityLog->onActivateSfx([this]() { m_audio.playSfx(Sfx::Activate); });
    m_activityLog->onCloseSfx([this]() { m_audio.playSfx(Sfx::ModalHide); });
    m_activityLog->onTabChangeSfx([this]() { m_audio.playSfx(Sfx::ThemeToggle); });
    m_activityLog->onDateChangeSfx([this](bool forward) {
        m_audio.playSfx(forward ? Sfx::SliderUp : Sfx::SliderDown);
    });
    m_activityLog->onClose([this]() {
        closeActivityLog();
    });
    m_activityLog->onTitleSelected([this](std::uint64_t titleId) {
        if (titleId != 0) {
            m_gameDetailsReturnFocus = m_activityLog.get();
            showGameDetails(titleId, "");
        }
    });
}

void WiiUMenuApp::openActivityLog(std::uint64_t initialTitleId) {
    if (m_editMode) return;
    if (m_dialog && m_dialog->isActive()) return;
    if (m_userSelect && m_userSelect->isActive()) return;

    createActivityLog();
    if (!m_activityLog || m_activityLog->isActive()) return;

    m_activityLogReturnFocus = focusManager().current();

    if (m_activityLogManager.allTimeRankings().empty()) {
        // Fallback: populate if prefetch hasn't finished yet
        std::vector<std::pair<std::uint64_t, std::string>> installed;
        if (!m_allApps.empty()) {
            for (const auto& app : m_allApps) {
                if (app.titleId != 0 && (app.isApplication() || m_config.isGamePort(app.titleId))) {
                    installed.emplace_back(app.titleId, app.title);
                }
            }
        } else {
            for (int i = 0; i < m_model.count(); ++i) {
                const auto& entry = m_model.at(i);
                if (entry.titleId != 0 && (entry.isApplication() || m_config.isGamePort(entry.titleId))) {
                    installed.emplace_back(entry.titleId, entry.title);
                }
            }
        }
        m_activityLogManager.refresh(installed);
    }

    raiseOverlay(m_activityLog);
    m_activityLog->open(initialTitleId);
    m_navigator.navigate(switchu::navigation::Route::ActivityLog);
    focusManager().setFocus(m_activityLog.get());
    m_audio.playSfx(Sfx::ModalShow);
}

void WiiUMenuApp::closeActivityLog() {
    if (!m_activityLog || !m_activityLog->isActive()) return;
    m_activityLog->hide();
    m_navigator.routeDidClose(switchu::navigation::Route::ActivityLog);
    m_audio.playSfx(Sfx::ModalHide);

    if (isCurrentFocusableWidget(m_activityLogReturnFocus)) {
        m_suppressNextNavigateSfx = true;
        focusManager().setFocus(m_activityLogReturnFocus);
    } else if (m_grid) {
        auto* target = m_grid->focusManager().current();
        if (target && isCurrentFocusableWidget(target)) {
            m_suppressNextNavigateSfx = true;
            focusManager().setFocus(target);
        } else {
            auto icons = m_grid->allIcons();
            if (!icons.empty()) {
                m_suppressNextNavigateSfx = true;
                focusManager().setFocus(icons[0].get());
            }
        }
    }
    m_activityLogReturnFocus = nullptr;
}

void WiiUMenuApp::createWaraWaraPlaza() {
    if (m_plazaScreen) return;

    m_plazaScreen = std::make_shared<warawara::WaraWaraPlazaScreen>();
    m_plazaScreen->setFonts(&m_fontNormal, &m_fontSmall);
    m_plazaScreen->initGpuAssets(app().gpu(), app().renderer());
    m_plazaScreen->setServices(&m_miiAvatarManager,
                               &m_plazaDialogueEngine,
                               &m_animalesePlayer,
                               &m_activityLogManager);
    refreshPlazaCommunities();

    m_plazaScreen->onClose([this]() {
        closeWaraWaraPlaza();
    });

    m_plazaScreen->onNavigateSfx([this]() {
        m_audio.playSfx(Sfx::Navigate);
    });
    m_plazaScreen->onActivateSfx([this]() {
        m_audio.playSfx(Sfx::Activate);
    });

    m_plazaScreen->onLaunchGame([this](std::uint64_t titleId) {
        if (titleId == 0) return;
        for (auto& app : m_allApps) {
            if (app.titleId == titleId) {
                GlossyIcon* iconSource = nullptr;
                if (m_grid) {
                    for (auto& icon : m_grid->allIcons()) {
                        if (icon && icon->titleId() == titleId) {
                            iconSource = icon.get();
                            break;
                        }
                    }
                }
                closeWaraWaraPlaza();
                if (iconSource) {
                    m_grid->focusManager().setFocus(iconSource);
                    focusManager().setFocus(iconSource);
                }
#ifdef SWITCHU_MENU
                activateApplication(iconSource, &app, app.titleId, app.title);
#endif
                break;
            }
        }
    });

    if (m_overlayLayer) {
        m_overlayLayer->addChild(m_plazaScreen);
    }
}

void WiiUMenuApp::refreshPlazaCommunities() {
    if (!m_plazaScreen) return;

    std::vector<warawara::WaraWaraPlazaScreen::GameCommunityEntry> entries;
    entries.reserve(10);

    // Plaza holds up to 10 communities. To prevent page-eviction thrashing on
    // the home grid and endless flickering between images and placeholder tiles,
    // Plaza loads and retains its own textures directly via AppListLoader::loadIconData,
    // pumping at most one decode/upload per frame without touching IconStreamer.
    const auto* gridIcons = m_grid ? &m_grid->allIcons() : nullptr;
    std::unordered_map<std::uint64_t, int> displayIndexByTitle;
    displayIndexByTitle.reserve(static_cast<std::size_t>(std::max(0, m_model.count())));
    for (int i = 0; i < m_model.count(); ++i) {
        const auto& displayEntry = m_model.at(i);
        if (displayEntry.isApplication() && displayEntry.titleId != 0)
            displayIndexByTitle.emplace(displayEntry.titleId, i);
    }

    const int communityCount = std::min(10, static_cast<int>(m_allApps.size()));
    for (int offset = 0; offset < communityCount; ++offset) {
        const int appPos = (m_plazaIconPumpIndex + offset) % communityCount;
        const auto& candidate = m_allApps[static_cast<std::size_t>(appPos)];
        if (candidate.titleId == 0 || candidate.isWidget())
            continue;

        auto& tex = m_plazaCommunityTextures[candidate.titleId];
        if (!tex.valid()) {
            std::vector<uint8_t> raw = AppListLoader::loadIconData(candidate.titleId);
            if (!raw.empty()) {
                tex.loadFromMemory(this->app().gpu(), this->app().renderer(),
                                   raw.data(), raw.size(), 256);
            }
            m_plazaIconPumpIndex = (appPos + 1) % communityCount;
            break;
        }
    }

    for (const auto& app : m_allApps) {
        if (entries.size() >= 10) break;
        if (app.titleId == 0 || app.isWidget()) continue;

        const auto displayFound = displayIndexByTitle.find(app.titleId);
        const int displayIndex = displayFound == displayIndexByTitle.end()
            ? -1 : displayFound->second;

        warawara::WaraWaraPlazaScreen::GameCommunityEntry entry;
        entry.titleId = app.titleId;
        entry.title = app.title;

        const auto* stats = m_activityLogManager.findTitleStats(app.titleId);
        std::uint64_t playtimeSec = stats ? stats->totalPlaytimeSeconds : 0;
        if (playtimeSec > 0) {
            std::uint32_t hours = static_cast<std::uint32_t>(playtimeSec / 3600);
            std::uint32_t minutes = static_cast<std::uint32_t>((playtimeSec % 3600) / 60);
            if (hours > 0) {
                entry.subtitle = std::to_string(hours) + "h " + std::to_string(minutes) + "m played";
            } else {
                entry.subtitle = std::to_string(minutes) + "m played";
            }
        } else {
            entry.subtitle = "Installed Game";
        }

        auto texIt = m_plazaCommunityTextures.find(app.titleId);
        if (texIt != m_plazaCommunityTextures.end() && texIt->second.valid()) {
            entry.iconTexture = &texIt->second;
        } else if (gridIcons && displayIndex >= 0 &&
                   displayIndex < static_cast<int>(gridIcons->size())) {
            const auto& icon = (*gridIcons)[static_cast<std::size_t>(displayIndex)];
            if (icon && icon->titleId() == app.titleId && icon->texture() && icon->texture()->valid())
                entry.iconTexture = icon->texture();
        }

        entries.push_back(std::move(entry));
    }

    m_plazaScreen->setupCommunities(entries);
}

void WiiUMenuApp::openWaraWaraPlaza() {
    if (m_editMode) return;
    if ((m_dialog && m_dialog->isActive()) ||
        (m_userSelect && m_userSelect->isActive()) ||
        (m_settings && m_settings->isActive()) ||
        (m_themeShop && m_themeShop->isActive()) ||
        (m_activityLog && m_activityLog->isActive())) {
        return;
    }

    if (!m_plazaScreen) {
        createWaraWaraPlaza();
    } else {
        refreshPlazaCommunities();
    }
    m_plazaReturnFocus = focusManager().current();
    if (m_cursor) {
        m_cursor->setVisible(false);
    }
    m_navigator.navigate(switchu::navigation::Route::WaraWaraPlaza);
    m_plazaIconPumpIndex = 0;
    m_plazaScreen->open();
    // focusRoot() also returns the Plaza while active, but dispatchInput() runs
    // before onUpdate() and must not spend a frame repairing stale HOME focus.
    // Bind the owner explicitly now so its registered Button A action receives
    // the very first controller press reliably.
    focusManager().setFocus(m_plazaScreen.get());
    if (m_screenSwapButton) {
        m_screenSwapButton->setPlazaActive(true);
    }
    m_audio.playSfx(Sfx::ThemeToggle);
}

// PoloNX #167/#177: the arrangement on screen during a grid <-> single-row
// prefetch, and whether SteamGridDB art is wanted in the current view.
AppLayoutMode WiiUMenuApp::presentationLayoutMode() const {
    // During prefetch the logical mode is already single-row, but tiles and
    // hero must keep the previous arrangement until the morph commits.
    if (m_layoutTransition.kind == LayoutTransitionKind::PrefetchThenMorph)
        return m_layoutTransition.visualMode;
    if (m_grid)
        return m_grid->layoutMode();
    return m_appLayoutMode;
}

bool WiiUMenuApp::steamGridDbArtworkAllowedHere() const {
    if (!m_config.steamGridDbEnabled)
        return false;
    if (m_openFolderId != 0)
        return m_config.steamGridDbShowInFolders;
    if (presentationLayoutMode() == AppLayoutMode::DynamicLine)
        return m_config.steamGridDbShowInDynamicLine;
    return m_config.steamGridDbShowInGrid;
}

void WiiUMenuApp::closeWaraWaraPlaza() {
    if (!m_plazaScreen) return;
    if (!m_plazaScreen->isActive() && m_navigator.route() != switchu::navigation::Route::WaraWaraPlaza) {
        return;
    }

    m_plazaScreen->close();
    m_plazaScreen->setVisible(false);
    m_plazaCommunityTextures.clear();
    m_navigator.routeDidClose(switchu::navigation::Route::WaraWaraPlaza);
    if (m_screenSwapButton) {
        m_screenSwapButton->setPlazaActive(false);
    }
    m_audio.playSfx(Sfx::ModalHide);

    nxui::Widget* target = nullptr;
    if (isCurrentFocusableWidget(m_plazaReturnFocus)) {
        target = m_plazaReturnFocus;
    } else if (m_grid) {
        target = m_grid->focusManager().current();
        if (!target || !isCurrentFocusableWidget(target)) {
            auto icons = m_grid->allIcons();
            if (!icons.empty() && icons[0]) {
                target = icons[0].get();
            }
        }
    }
    m_plazaReturnFocus = nullptr;

    if (target) {
        m_suppressNextNavigateSfx = true;
        if (m_grid) {
            m_grid->focusManager().setFocus(target);
        }
        focusManager().setFocus(target);
        if (m_cursor) {
            m_cursor->moveTo(target->focusRect().expanded(4.f), 0.f);
            m_cursor->setVisible(true);
        }
    }
}

void WiiUMenuApp::createAutoThemeScreen() {
    if (m_autoThemeScreen) return;
    m_autoThemeScreen = std::make_shared<AutoThemeScreen>();
    if (m_overlayLayer) m_overlayLayer->addChild(m_autoThemeScreen);
    m_autoThemeScreen->setFont(&m_fontNormal);
    m_autoThemeScreen->setSmallFont(&m_fontSmall);
    m_autoThemeScreen->setTheme(&m_theme);
    m_autoThemeScreen->setAccessibilityVoiceEnabled(m_config.accessibilityEnabled);
    m_autoThemeScreen->setAccessibilitySpeechPreferences(m_config.accessibilitySpeakHints,
                                                         m_config.accessibilitySpeakPosition);
    m_autoThemeScreen->onNavigateSfx([this]() { m_audio.playSfx(Sfx::Navigate); });
    m_autoThemeScreen->onActivateSfx([this]() { m_audio.playSfx(Sfx::Activate); });
    m_autoThemeScreen->onCloseSfx([this]() { m_audio.playSfx(Sfx::ModalHide); });
    m_autoThemeScreen->onToggleSfx([this](bool on) {
        m_audio.playSfx(on ? Sfx::ThemeToggle : Sfx::ToggleOff);
    });
    m_autoThemeScreen->onSliderSfx([this](bool up) {
        m_audio.playSfx(up ? Sfx::SliderUp : Sfx::SliderDown);
    });
    m_autoThemeScreen->onAccessibilityAnnouncement([this](const std::string& text) {
        m_accessibility.announce(text);
    });
    m_autoThemeScreen->onAccessibilityStructuredAnnouncement(
        [this](const std::string& context, const std::string& position,
               const std::string& summary, bool forceRepeat, bool forceContext) {
            m_accessibility.announceStructuredFocus(context, position, summary,
                                                    forceRepeat, forceContext);
        });
    m_autoThemeScreen->onChanged([this](const AutoThemeScreen::State& st) {
        m_config.autoThemeMode = st.mode == 1 ? "manual"
                               : (st.mode == 2 ? "geo" : "off");
        m_config.autoThemeDayPreset = st.dayPreset;
        m_config.autoThemeNightPreset = st.nightPreset;
        m_config.autoThemeDayStartHour = std::clamp(st.dayStartHour, 0, 23);
        m_config.autoThemeNightStartHour = std::clamp(st.nightStartHour, 0, 23);
        applyAutoThemeConfig();
        maybeFetchGeoLocation();
        evaluateAutoTheme(true);
        refreshAutoThemeSummary();
        pushGeoDisplayToScreen();
    });
    m_autoThemeScreen->onClosed([this]() {
        if (m_configSaveFuture.valid())
            m_configSaveFuture.wait();
        m_configSaveFuture = m_threadPool.submit([cfg = m_config]() { cfg.save(); });
        DebugLog::log("[config] save queued");
        // Return to the Theme Shop it was opened from.
        if (m_themeShop && m_themeShop->isActive()) {
            m_navigator.navigate(switchu::navigation::Route::ThemeShop);
            m_themeShop->rebuildCurrentTab();
            focusManager().setFocus(m_themeShop.get());
        } else {
            m_navigator.routeDidClose(switchu::navigation::Route::AutoTheme);
        }
    });
}

void WiiUMenuApp::openAutoThemeSettings() {
    createAutoThemeScreen();
    if (!m_autoThemeScreen) return;

    std::vector<AutoThemeScreen::PresetOption> presets;
    presets.reserve(m_allPresets.size());
    for (const auto& p : m_allPresets) {
        AutoThemeScreen::PresetOption opt;
        opt.id = p.id.empty() ? p.name : p.id;
        opt.name = p.name;
        presets.push_back(std::move(opt));
    }

    AutoThemeScreen::State st;
    st.mode = m_config.autoThemeMode == "manual" ? 1
            : (m_config.autoThemeMode == "geo" ? 2 : 0);
    st.dayPreset = m_config.autoThemeDayPreset;
    st.nightPreset = m_config.autoThemeNightPreset;
    st.dayStartHour = m_config.autoThemeDayStartHour;
    st.nightStartHour = m_config.autoThemeNightStartHour;
    st.geoResolved = m_config.autoThemeGeoResolved;
    st.geoCity = m_config.autoThemeGeoResolved
        ? (m_config.autoThemeGeoCity.empty()
               ? fmt::format("{:.2f}, {:.2f}", m_config.autoThemeGeoLat, m_config.autoThemeGeoLon)
               : m_config.autoThemeGeoCity)
        : std::string();

    m_autoThemeScreen->setTheme(&m_theme);
    m_autoThemeScreen->configure(st, std::move(presets));
    pushGeoDisplayToScreen(); // fill in computed sunrise/sunset

    m_navigator.navigate(switchu::navigation::Route::AutoTheme);
    m_autoThemeScreen->show();
    focusManager().setFocus(m_autoThemeScreen.get());
    m_audio.playSfx(Sfx::ModalShow);
}

void WiiUMenuApp::refreshAutoThemeSummary() {
    if (!m_themeShop) return;
    auto& i18n = nxui::I18n::instance();
    std::string summary;
    if (m_config.autoThemeMode == "manual") {
        summary = fmt::format("{} ({:02d}:00 / {:02d}:00)",
                              i18n.tr("autotheme.summary.manual", "Manual hours"),
                              std::clamp(m_config.autoThemeDayStartHour, 0, 23),
                              std::clamp(m_config.autoThemeNightStartHour, 0, 23));
    } else if (m_config.autoThemeMode == "geo") {
        summary = i18n.tr("autotheme.summary.geo", "Geolocation (IP)");
    } else {
        summary = i18n.tr("autotheme.summary.off", "Off");
    }
    m_themeShop->setAutoThemeSummary(summary);
}

void WiiUMenuApp::toggleWaraWaraPlaza() {
    if (m_plazaScreen && m_plazaScreen->isActive()) {
        closeWaraWaraPlaza();
    } else {
        openWaraWaraPlaza();
    }
}

void WiiUMenuApp::createThemeShop() {
    if (m_themeShop) return;

    auto showThemeShopInfo = [this](const std::string& title, const std::string& message) {
        if (!m_dialog) return;
        m_dialogReturnFocus = focusManager().current();
        m_dialog->show(title, message, {{"OK", {}, true}});
        focusManager().setFocus(m_dialog.get());
    };
    auto queueThemeTransfer = [this, showThemeShopInfo](const std::string& themeId, bool applyAfterInstall) {
        if (!m_themeShop)
            return;

        const auto* entry = m_themeShop->findCommunityThemeEntry(themeId);
        if (!entry) {
            auto& i18n = nxui::I18n::instance();
            showThemeShopInfo(i18n.tr("sidebar.theme_shop", "SwitchU"),
                              i18n.tr("themeshop.community.selected_missing",
                                      "The selected community theme is no longer available in the catalog."));
            return;
        }

        ThemeCatalogClient::Entry entryCopy = *entry;
        ThemePackageInstaller::Mode mode = applyAfterInstall
            ? ThemePackageInstaller::Mode::InstallAndApply
            : ThemePackageInstaller::Mode::InstallOnly;
        std::string destination = ThemePackageInstaller::destinationRootFor(entryCopy.id, mode);

        // The catalog now publishes one definitive package: BC7 at 912x512.
        // It fixes BC1 artifacts in dark scenes while retaining the complete
        // animation at 30 fps inside the measured image-memory budget.
        auto startTransfer = [this, entryCopy, applyAfterInstall]() {
            startThemePackageTransfer(entryCopy, applyAfterInstall);
        };

        // Finish any in-flight uninstall before we decide "already installed".
        if (m_themeDeleteFuture.valid()) {
            m_themeDeleteFuture.wait();
            m_themeDeleteFuture = {};
        }

        // Only a real package (theme.json) counts. Empty/orphan folders left by a
        // failed async delete used to trigger a bogus "Replace Installed Theme".
        const bool packagePresent = pathExists(joinPath(destination, "theme.json"));
        if (!packagePresent) {
            if (pathExists(destination)) {
                const bool cleaned = removeDirectoryRecursive(destination);
                DebugLog::log("[themeshop] cleared orphan theme folder before install: %s (%s)",
                              safeLogPath(destination), cleaned ? "ok" : "failed");
            }
            startTransfer();
            return;
        }

        auto& i18n = nxui::I18n::instance();
        if (!m_dialog) {
            startTransfer();
            return;
        }

        m_dialogReturnFocus = focusManager().current();
        m_dialog->show(
            applyAfterInstall
                ? i18n.tr("themeshop.community.overwrite_apply_title", "Replace And Apply Theme")
                : i18n.tr("themeshop.community.overwrite_install_title", "Replace Installed Theme"),
            applyAfterInstall
                ? i18n.tr("themeshop.community.overwrite_apply_message",
                          "A package with this theme ID is already installed. Replace it with the GitHub version, then apply it?")
                : i18n.tr("themeshop.community.overwrite_install_message",
                          "A package with this theme ID is already installed. Replace it with the version from GitHub?"),
            {
                {i18n.tr("button.cancel", "Cancel"), {}, true},
                {i18n.tr("button.replace", "Replace"), [startTransfer]() { startTransfer(); }, true}
            },
            1,
            {});
        focusManager().setFocus(m_dialog.get());
    };
    auto clearCompletedThemeTransferState = [this]() {
        if (!m_themePackageTransfer)
            return;

        bool isRunning = false;
        {
            std::lock_guard<std::mutex> lk(m_themePackageTransfer->mutex);
            isRunning = m_themePackageTransfer->state.isRunning();
        }

        if (isRunning)
            return;

        m_themePackageTransfer.reset();
        m_themePackageTransferUiRevision = 0;
        m_themePackageTransferHandledRevision = 0;
        if (m_themeShop) {
            ThemeTransferState cleared;
            m_themeShop->setPackageTransferState(cleared, {}, false);
        }
    };

    m_themeShop = std::make_shared<ThemeShopScreen>();
    if (m_overlayLayer) {
        m_overlayLayer->addChild(m_themeShop);
    }
    m_themeShop->setFont(&m_fontNormal);
    m_themeShop->setSmallFont(&m_fontSmall);
    m_themeShop->setTheme(&m_theme);
    m_themeShop->setThreadPool(&m_threadPool);
    m_themeShop->setRenderContext(&app().gpu(), &app().renderer());
    m_themeShop->youTubeClient().setBackendUrl(m_config.ytdlBackendUrl);
    m_themeShop->setMusicState(m_audio.isPlaying(), m_audio.volume(), m_audio.sfxVolume());
    m_themeShop->setGridLayoutState(m_config.gridColumns, m_config.gridRows);
    m_themeShop->setDynamicPagesState(m_config.dynamicPages);
    m_themeShop->setAppearanceState(m_config.glassSharpness,
                                    m_config.backgroundSpeed,
                                    m_config.backgroundBlur);
    m_themeShop->setActionHintStyleState(m_config.actionHintStyle);
    m_themeShop->setSortLargeTilesState(m_config.sortLargeTiles);
    m_themeShop->setBatterySaverState(m_config.batterySaver);
    m_themeShop->setFrameTapLiteEngaged(m_frameTapStatus.viewerConnected());
    m_themeShop->setIconContrastAdaptState(m_config.iconContrastAdapt);
    m_themeShop->setThemeMusicOverrideState(m_config.themeMusicOverride);
    m_themeShop->setThemeMusicAddToPlaylistState(m_config.themeMusicAddToPlaylist);
    m_themeShop->setThemeMusicPlayOnApplyState(m_config.themeMusicPlayOnApply);
    m_themeShop->setThemeMatchAccent(m_activeColors.accentH,
                                     m_activeColors.accentS,
                                     m_activeColors.accentL);
    {
        std::unordered_map<std::string, ThemeShopScreen::IconPackTintState> tints;
        for (const auto& [packId, tint] : m_config.iconPackTints) {
            ThemeShopScreen::IconPackTintState state;
            state.enabled = tint.enabled;
            state.flatten = tint.flatten;
            state.grayscale = tint.grayscale;
            state.matchTheme = tint.matchTheme;
            state.hue = tint.hue;
            state.sat = tint.sat;
            state.light = tint.light;
            tints[packId] = state;
        }
        m_themeShop->setIconPackTintState(tints);
    }
    m_themeShop->setInputSource(&app().input());
    refreshAutoThemeSummary();
    m_themeShop->setSteamGridDbOpacity(m_config.steamGridDbOpacity);
    m_themeShop->setSteamGridDbState(m_config.steamGridDbEnabled,
                                    !m_config.steamGridDbApiKey.empty());
    m_themeShop->setMetadataKeyState(!m_config.rawgApiKey.empty(),
                                     !m_config.igdbClientId.empty(),
                                     !m_config.igdbClientSecret.empty());
    m_themeShop->setAccessibilityVoiceEnabled(m_config.accessibilityEnabled);
    m_themeShop->setAccessibilitySpeechPreferences(m_config.accessibilitySpeakHints,
                                                   m_config.accessibilitySpeakPosition);

    m_themeShop->onMusicEnabledChange([this](bool enabled) {
        if (enabled) m_audio.play(); else m_audio.stop();
        m_config.musicEnabled = enabled;
    });
    m_themeShop->onMusicVolumeChange([this](float v) {
        m_audio.setVolume(v);
        m_config.musicVolume = v;
    });
    m_themeShop->onSfxVolumeChange([this](float v) {
        m_audio.setSfxVolume(v);
        m_config.sfxVolume = v;
    });
    // Sole home for these three now. They used to be mirrored into Settings >
    // Display so the two screens agreed; with the Display copies gone there is
    // nothing left to keep in step.
    m_themeShop->onGlassSharpnessChange([this](float v) {
        m_config.glassSharpness = v;
        applyGlassSharpness(v);
    });
    m_themeShop->onBackgroundSpeedChange([this](float v) {
        m_config.backgroundSpeed = v;
        m_config.backgroundSpeedChosen = true;
        if (m_background) m_background->setSpeedScale(v * 2.f);
        if (m_background) m_background->setWallpaperSpeedScale(v);
    });
    m_themeShop->onBackgroundBlurChange([this](float v) {
        m_config.backgroundBlur = v;
        if (m_background) m_background->setBlurStrength(v);
    });
    m_themeShop->onSteamGridDbOpacityChange([this](float v) {
        m_config.steamGridDbOpacity = v;
        if (m_steamGridDbBackdrop) m_steamGridDbBackdrop->setArtworkOpacityScale(v);
    });
    m_themeShop->onSteamGridDbEnabledChange([this](bool enabled) {
        m_config.steamGridDbEnabled = enabled;
        if (m_steamGridDbBackdrop) {
            m_steamGridDbBackdrop->setEnabled(enabled);
            if (enabled) showFocusedSteamGridDbArtwork(true);
        }
    });
    m_themeShop->onSteamGridDbApiKeyRequest([this]() {
        editSteamGridDbApiKey();
    });
    m_themeShop->onSteamGridDbScrapeRequest([this]() {
        startSteamGridDbScrape();
    });
    m_themeShop->onMetadataKeyRequest([this](int index) {
        editMetadataKey(index);
    });
    m_themeShop->onGridColumnsChange([this](int cols) {
        cols = std::clamp(cols, 3, 8);
        if (m_config.gridColumns == cols)
            return;
        m_config.gridColumns = cols;
        reflowHomeGrid();
    });
    m_themeShop->onGridRowsChange([this](int rows) {
        rows = std::clamp(rows, 2, 5);
        if (m_config.gridRows == rows)
            return;
        m_config.gridRows = rows;
        reflowHomeGrid();
    });
    m_themeShop->onDynamicPagesChange([this](bool enabled) {
        if (m_config.dynamicPages == enabled)
            return;
        m_config.dynamicPages = enabled;
        m_config.save();
        reflowHomeGrid();
    });
    m_themeShop->onActionHintStyleChange([this](int style) {
        m_config.actionHintStyle = style == 0 ? "panel" : "capsules";
    });
    m_themeShop->onSortLargeTilesChange([this](bool enabled) {
        if (m_config.sortLargeTiles == enabled)
            return;
        m_config.sortLargeTiles = enabled;
        m_config.save();
        reflowHomeGrid();
    });
    m_themeShop->onBatterySaverChange([this](bool enabled) {
        applyBatterySaver(enabled);
    });
    // PoloNX's Cursor Movement / Page Transition callbacks are not wired: the
    // fork carries neither mode (InterfaceTab does not offer them).
    m_themeShop->setLayoutModeState(m_appLayoutMode);
    m_themeShop->onLayoutModeChange([this](AppLayoutMode mode) {
        setAppLayoutMode(mode);
    });

    m_themeShop->onAutoThemeOpen([this]() { openAutoThemeSettings(); });
    m_themeShop->onNextTrack([this]() {
        m_audio.nextTrack();
        m_audio.playSfx(Sfx::ConfirmPositive);
    });
    m_themeShop->onNavigateSfx([this]() { m_audio.playSfx(Sfx::Navigate); });
    m_themeShop->onActivateSfx([this]() { m_audio.playSfx(Sfx::Activate); });
    m_themeShop->onCloseSfx([this]() { m_audio.playSfx(Sfx::ModalHide); });
    m_themeShop->onAccessibilityAnnouncement([this](const std::string& text) {
        m_accessibility.announce(text);
    });
    m_themeShop->onAccessibilityStructuredAnnouncement([this](const std::string& context,
                                                              const std::string& position,
                                                              const std::string& summary,
                                                              bool forceRepeat,
                                                              bool forceContext) {
        m_accessibility.announceStructuredFocus(context, position, summary, forceRepeat, forceContext);
    });
    m_themeShop->onToggleSfx([this](bool on) {
        m_audio.playSfx(on ? Sfx::ThemeToggle : Sfx::ToggleOff);
    });
    m_themeShop->onSliderSfx([this](bool up) {
        m_audio.playSfx(up ? Sfx::SliderUp : Sfx::SliderDown);
    });
    m_themeShop->onUpdateCheck([this]() { startUpdateCheck(true); });
    m_themeShop->onSelfUninstall([this]() {
        auto& i18n = nxui::I18n::instance();
        m_audio.playSfx(Sfx::ModalShow);
        raiseOverlay(m_dialog);
        m_dialog->show(
            i18n.tr("themeshop.uninstall", "Uninstall SwitchU"),
            i18n.tr("themeshop.uninstall_information",
                    "This permanently removes SwitchU from your console and restarts into the stock Nintendo HOME Menu.\n\n"
                    "All SwitchU data on your SD card will be completely deleted, including:\n"
                    "• All settings, configurations, and logs\n"
                    "• All downloaded static and animated themes\n"
                    "• All cached game artwork and icons\n"
                    "• SwitchU menu binaries and SwitchU Manager\n\n"
                    "To use SwitchU again in the future, a fresh manual installation will be required."),
            {
                {i18n.tr("button.cancel", "Cancel"), [this]() {}, true},
                {i18n.tr("themeshop.uninstall_continue", "Continue"), [this]() {
                    auto& i18n = nxui::I18n::instance();
                    m_audio.playSfx(Sfx::ModalShow);
                    raiseOverlay(m_dialog);
                    m_dialog->show(
                        i18n.tr("themeshop.uninstall_confirm_title", "Permanently Delete SwitchU?"),
                        i18n.tr("themeshop.uninstall_confirm",
                                "This action cannot be undone. "
                                "All SwitchU files, themes, artwork, and settings will be permanently erased from your SD card."),
                        {
                            {i18n.tr("button.cancel", "Cancel"), [this]() {}, true},
                            {i18n.tr("themeshop.uninstall_confirm_action", "Uninstall and delete everything"),
                             [this]() {
                                auto& i18n = nxui::I18n::instance();
                                if (m_progressDialog) {
                                    m_progressDialog->setTheme(&m_theme);
                                    raiseOverlay(m_progressDialog);
                                    m_progressDialog->show(
                                        i18n.tr("themeshop.uninstall_progress_title", "Removing SwitchU"),
                                        i18n.tr("themeshop.uninstall_preparing", "Preparing removal..."), 0.25f);
                                    focusManager().setFocus(m_progressDialog.get());
                                }

                                if (!stageSelfUninstallRequest()) {
                                    DebugLog::log("[uninstall] failed to stage request");
                                    if (m_progressDialog)
                                        m_progressDialog->hide();
                                    raiseOverlay(m_dialog);
                                    m_dialog->show(
                                        i18n.tr("themeshop.uninstall_failed_title", "Removal could not be prepared"),
                                        i18n.tr("themeshop.uninstall_failed",
                                                "SwitchU was not changed. Check that the SD card is writable and try again."),
                                        {{i18n.tr("button.ok", "OK"), [this]() {}, true}}, 0, {});
                                    focusManager().setFocus(m_dialog.get());
                                    return;
                                }

                                if (m_progressDialog)
                                    m_progressDialog->updateState(
                                        i18n.tr("themeshop.uninstall_restarting", "Restarting..."), 1.f);
                                const Result rc = m_launcher.requestSelfUninstall();
                                DebugLog::log("[uninstall] daemon request rc=0x%X", rc);
                                if (R_FAILED(rc)) {
                                    if (m_progressDialog)
                                        m_progressDialog->hide();
                                    raiseOverlay(m_dialog);
                                    m_dialog->show(
                                        i18n.tr("themeshop.uninstall_failed_title", "Removal could not be prepared"),
                                        i18n.tr("themeshop.uninstall_daemon_failed",
                                                "SwitchU was not changed because the restart request failed."),
                                        {{i18n.tr("button.ok", "OK"), [this]() {}, true}}, 0, {});
                                    focusManager().setFocus(m_dialog.get());
                                }
                             }, false},
                        },
                        0, {});
                    focusManager().setFocus(m_dialog.get());
                }, false},
            },
            0, {});
        focusManager().setFocus(m_dialog.get());
    });
    m_themeShop->onReleaseNotes([this]() { showReleaseNotes(); });
    m_themeShop->onUpdateInstall([this]() {
        // The notes dialog is the confirmation step: nothing downloads until a
        // player has seen what changed and chosen to go ahead.
        if (!m_pendingUpdate.downloadUrl.empty())
            offerUpdate(m_pendingUpdate, false);
    });
    // Offered only while a downloaded update waits for the reboot that applies
    // it. Asked for so the player does not have to leave the tab that told them
    // a restart was needed in order to perform it.
    m_themeShop->onUpdateRestart([this]() {
        auto& i18n = nxui::I18n::instance();
        m_audio.playSfx(Sfx::ModalShow);
        raiseOverlay(m_dialog);
        m_dialog->show(
            i18n.tr("themeshop.update_restart", "Restart now"),
            i18n.tr("themeshop.update_restart_confirm",
                    "The console restarts and applies the update while it starts. "
                    "This first boot takes about half a minute longer than usual."),
            {
                {i18n.tr("button.cancel", "Cancel"), [this]() {}, true},
                {i18n.tr("themeshop.update_restart", "Restart now"), [this]() {
                     DebugLog::log("[update] restart requested from the Update tab");
                     m_launcher.reboot();
                 }, false},
            },
            0, {});
        focusManager().setFocus(m_dialog.get());
    });

    m_themeShop->onNetConnectRequest([this]() {
        m_pendingNetConnect = true;
        if (m_themeShop) m_themeShop->hide();
        m_navigator.resetToHome();
    });
    // O daemon relê os títulos e joga fora os nomes e ícones em cache. A grade
    // se reconstrói quando ele avisa que o catálogo mudou, então aqui não há o
    // que esperar.
    m_themeShop->onReloadCatalog([this]() {
        const Result rc = m_launcher.refreshCatalog();
        DebugLog::log("[catalog] reload requested rc=0x%X", rc);
        auto& i18n = nxui::I18n::instance();
        if (m_settings) {
            m_settings->requestToast(R_SUCCEEDED(rc)
                ? i18n.tr("settings.display.reload_grid_done", "Reading the installed titles again...")
                : i18n.tr("settings.display.reload_grid_failed", "Could not ask for a reload."), 2.6f);
        }
        m_refreshQueued = true;
        m_deferredRefreshFrames = std::max(m_deferredRefreshFrames, 3);
    });
    // The slow half, now that the ordinary reload no longer pays for it: every
    // name and icon is read from the titles again, about a second each.
    m_themeShop->onRebuildControlCache([this]() {
        const Result rc = m_launcher.rebuildControlCache();
        DebugLog::log("[catalog] control cache rebuild requested rc=0x%X", rc);
        auto& i18n = nxui::I18n::instance();
        if (m_settings) {
            m_settings->requestToast(R_SUCCEEDED(rc)
                ? i18n.tr("settings.display.rebuild_names_done",
                          "Reading names and icons again. This takes a few minutes.")
                : i18n.tr("settings.display.reload_grid_failed", "Could not ask for a reload."),
                4.0f);
        }
        m_refreshQueued = true;
        m_deferredRefreshFrames = std::max(m_deferredRefreshFrames, 3);
    });
    m_themeShop->onIconContrastAdaptChange([this](bool enabled) {
        if (m_config.iconContrastAdapt == enabled)
            return;
        m_config.iconContrastAdapt = enabled;
        m_config.save();
        syncColourIconContrast();
    });
    m_themeShop->onThemeMusicOverrideChange([this](bool enabled) {
        if (m_config.themeMusicOverride == enabled)
            return;
        m_config.themeMusicOverride = enabled;
        m_config.save();
    });
    m_themeShop->onThemeMusicAddToPlaylistChange([this](bool enabled) {
        if (m_config.themeMusicAddToPlaylist == enabled)
            return;
        m_config.themeMusicAddToPlaylist = enabled;
        m_config.save();
    });
    m_themeShop->onThemeMusicPlayOnApplyChange([this](bool enabled) {
        if (m_config.themeMusicPlayOnApply == enabled)
            return;
        m_config.themeMusicPlayOnApply = enabled;
        m_config.save();
    });
    m_themeShop->onIconPackTintChange([this](const std::string& packId,
                                             const ThemeShopScreen::IconPackTintState& tint) {
        AppConfig::IconPackTint cfg;
        cfg.enabled = tint.enabled;
        cfg.flatten = tint.flatten;
        cfg.grayscale = tint.grayscale;
        cfg.matchTheme = tint.matchTheme;
        cfg.hue = std::clamp(tint.hue, 0.f, 1.f);
        cfg.sat = std::clamp(tint.sat, 0.f, 1.f);
        cfg.light = std::clamp(tint.light, 0.f, 1.f);
        const AppConfig::IconPackTint previous = m_config.iconPackTintFor(packId);
        if (previous.enabled == cfg.enabled
            && previous.flatten == cfg.flatten
            && previous.grayscale == cfg.grayscale
            && previous.matchTheme == cfg.matchTheme
            && std::fabs(previous.hue - cfg.hue) < 0.0005f
            && std::fabs(previous.sat - cfg.sat) < 0.0005f
            && std::fabs(previous.light - cfg.light) < 0.0005f)
            return;
        m_config.setIconPackTint(packId, cfg);
        m_config.save();
        const std::string activeId = m_config.iconPack.empty()
            ? switchu::icons::kDefaultPackId : m_config.iconPack;
        const std::string id = packId.empty() ? switchu::icons::kDefaultPackId : packId;
        if (id == activeId)
            syncColourIconContrast();
    });
    m_themeShop->onIconPackApply([this](const std::string& packId) {
        applyIconPack(packId);
    });
    m_themeShop->onIconPackDownload([this](const std::string& packId) {
        const auto* entry = findCatalogIconEntry(packId);
        if (!entry)
            return;
        startIconPackTransfer(*entry, false);
    });
    m_themeShop->onIconPackDownloadApply([this](const std::string& packId) {
        const auto* entry = findCatalogIconEntry(packId);
        if (!entry)
            return;
        startIconPackTransfer(*entry, true);
    });
    m_themeShop->onIconPackDelete([this](const std::string& packId) {
        auto& i18n = nxui::I18n::instance();
        if (!m_dialog) return;
        m_dialogReturnFocus = focusManager().current();
        m_dialog->show(
            i18n.tr("themeshop.icons.remove", "Remove Icon Pack"),
            i18n.tr("themeshop.icons.remove_confirm",
                    "Delete this icon pack from the console?"),
            {{i18n.tr("button.cancel", "Cancel"), {}, false},
             {i18n.tr("themeshop.installed.remove_confirm_yes", "Remove"),
              [this, packId]() { removeIconPack(packId); }, true}});
        focusManager().setFocus(m_dialog.get());
    });
    m_themeShop->onIconCatalogUpdated([this]() {
        refreshThemeShopState();
    });
    m_themeShop->onThemeShopApply([this](const std::string& presetId) {
        DebugLog::log("[theme-apply] request from Theme Shop: preset=%s", presetId.c_str());
        ThemePreset* preset = findPresetPtr(presetId);
        if (!preset) {
            DebugLog::log("[theme-apply] preset not found: %s", presetId.c_str());
            return;
        }

        activateThemePreset(preset, true);
    });
    m_themeShop->onThemeShopDelete([this](const std::string& presetId) {
        auto& i18n = nxui::I18n::instance();
        ThemePreset* preset = findPresetPtr(presetId);
        if (!preset || preset->source == ThemePresetSource::BuiltIn) {
            if (!m_dialog) return;
            m_dialogReturnFocus = focusManager().current();
            m_dialog->show(
                i18n.tr("themeshop.installed.remove", "Remove Theme"),
                i18n.tr("settings.theme.builtin_readonly", "Built-in presets cannot be modified."),
                {{ i18n.tr("button.ok", "OK"), {}, true }});
            focusManager().setFocus(m_dialog.get());
            return;
        }

        if (!m_dialog) {
            deletePreset(presetId);
            return;
        }
        m_dialogReturnFocus = focusManager().current();
        m_dialog->show(
            i18n.tr("themeshop.installed.remove", "Remove Theme"),
            i18n.tr("themeshop.installed.remove_confirm",
                    "Remove this theme from the console?"),
            {{i18n.tr("button.cancel", "Cancel"), {}, false},
             {i18n.tr("themeshop.installed.remove_confirm_yes", "Remove"),
              [this, presetId]() { deletePreset(presetId); }, true}});
        focusManager().setFocus(m_dialog.get());
    });
    m_themeShop->onThemeShopDownload([queueThemeTransfer](const std::string& themeId) {
        queueThemeTransfer(themeId, false);
    });
    m_themeShop->onThemeShopDownloadInstall([queueThemeTransfer](const std::string& themeId) {
        queueThemeTransfer(themeId, true);
    });
    m_themeShop->onDialogRequest([this](const std::string& title,
                                        const std::string& msg,
                                        std::vector<ThemeShopScreen::DialogButtonDef> buttons) {
        if (!m_dialog) return;
        std::vector<OverlayDialog::ButtonDef> dlgButtons;
        bool preserveReturnFocus = (m_dialog && m_dialog->isActive() && focusManager().current() == m_dialog.get() && m_dialogReturnFocus != nullptr);
        for (size_t i = 0; i < buttons.size(); ++i) {
            auto cb = buttons[i].onPress;
            bool isLast = (i == buttons.size() - 1);
            if (buttons.size() == 1) {
                dlgButtons.push_back({buttons[i].label, [this, cb]() {
                    m_audio.playSfx(Sfx::ConfirmPositive);
                    m_dialog->hide();
                    if (cb) cb();
                }, true});
            } else if (isLast) {
                dlgButtons.push_back({buttons[i].label, [cb]() { if (cb) cb(); }, true});
            } else {
                dlgButtons.push_back({buttons[i].label, [this, cb]() {
                    m_audio.playSfx(Sfx::ConfirmPositive);
                    m_dialog->hide();
                    if (cb) cb();
                }, true});
            }
        }
        if (!preserveReturnFocus)
            m_dialogReturnFocus = focusManager().current();
        m_dialog->show(title, msg, std::move(dlgButtons));
        focusManager().setFocus(m_dialog.get());
    });
    m_themeShop->onRequestTextEntry([this](const std::string& title,
                                           const std::string& guide,
                                           const std::string& initial,
                                           std::function<void(std::string)> onAccept) {
        requestTextEntry(title, guide, initial, 64, false, std::move(onAccept), nullptr);
    });
    m_themeShop->onProgressShow([this](const std::string& title, const std::string& message, float p) {
        if (!m_progressDialog) return;
        raiseOverlay(m_progressDialog);
        m_progressDialog->setTheme(&m_theme);
        m_progressDialog->show(title, message, p);
        focusManager().setFocus(m_progressDialog.get());
    });
    m_themeShop->onProgressUpdate([this](const std::string& message, float p) {
        if (m_progressDialog && m_progressDialog->isActive()) {
            m_progressDialog->updateState(message, p);
        }
    });
    m_themeShop->onProgressHide([this]() {
        if (m_progressDialog && m_progressDialog->isActive()) {
            m_progressDialog->hide();
        }
    });
    m_themeShop->onMusicDownloaded([this]() {
        DebugLog::log("[themeshop] Music downloaded, reloading BGM tracks...");
        reloadMusicTracks();
    });
    m_themeShop->onPlayMusic([this](const std::string& path, const std::string& title) {
        DebugLog::log("[themeshop] Playing track: %s (%s)", title.c_str(), path.c_str());
        m_audio.stop();
        m_audio.clearTracks();
        m_audio.loadTrack(path, title);
        m_audio.play();
        syncMediaCenterState();
    });
    m_themeShop->onClosed([this, clearCompletedThemeTransferState]() {
        m_themeShop->resetMusicTabState();
        m_configSaveFuture = m_threadPool.submit([cfg = m_config]() {
            cfg.save();
        });
        DebugLog::log("[config] save queued");
        clearCompletedThemeTransferState();
        if (isCurrentFocusableWidget(m_sidebar.themeShopButton())) {
            m_suppressNextNavigateSfx = true;
            focusManager().setFocus(m_sidebar.themeShopButton());
        }
    });

    raiseOverlay(m_dialog);
    raiseOverlay(m_progressDialog);
    raiseOverlay(m_launchAnim);
    raiseOverlay(m_pointerCursor);

}

void WiiUMenuApp::createGameGallery() {
    if (m_gameGallery)
        return;

    m_gameGallery = std::make_shared<GameGalleryScreen>();
    if (m_overlayLayer)
        m_overlayLayer->addChild(m_gameGallery);
    m_gameGallery->setFont(&m_fontNormal);
    m_gameGallery->setSmallFont(&m_fontSmall);
    m_gameGallery->setTheme(&m_theme);
    m_gameGallery->setThreadPool(&m_threadPool);
    m_gameGallery->setRenderContext(&app().gpu(), &app().renderer());
    m_gameGallery->setAccessibilityVoiceEnabled(m_config.accessibilityEnabled);
    m_gameGallery->setAccessibilitySpeechPreferences(m_config.accessibilitySpeakHints,
                                                     m_config.accessibilitySpeakPosition);
    m_gameGallery->onNavigateSfx([this]() { m_audio.playSfx(Sfx::Navigate); });
    m_gameGallery->onActivateSfx([this]() { m_audio.playSfx(Sfx::Activate); });
    m_gameGallery->onCloseSfx([this]() { m_audio.playSfx(Sfx::ModalHide); });
    m_gameGallery->onAccessibilityAnnouncement([this](const std::string& text) {
        m_accessibility.announce(text);
    });
    m_gameGallery->onApplyArtwork([this](std::uint64_t titleId, const std::string& title,
                                         GameGalleryClient::Category category,
                                         GameGalleryClient::Asset asset,
                                         std::shared_ptr<const std::vector<std::uint8_t>> bytes) {
        confirmGameArtwork(titleId, title, category, std::move(asset), std::move(bytes));
    });
    m_gameGallery->onResetArtwork([this](std::uint64_t titleId, const std::string& title,
                                         GameGalleryClient::Category category) {
        confirmRestoreGameArtwork(titleId, title, category);
    });
    m_gameGallery->onClosed([this]() {
        if (m_gameDetails && m_gameGalleryReturnFocus == m_gameDetails.get()) {
            m_gameDetails->resumeFromChild();
            m_suppressNextNavigateSfx = true;
            focusManager().setFocus(m_gameDetails.get());
        } else if (isCurrentFocusableWidget(m_gameGalleryReturnFocus)) {
            m_suppressNextNavigateSfx = true;
            focusManager().setFocus(m_gameGalleryReturnFocus);
        }
        m_gameGalleryReturnFocus = nullptr;
    });
}

void WiiUMenuApp::createGameDetails() {
    if (m_gameDetails)
        return;

    m_gameDetails = std::make_shared<GameDetailsScreen>();
    if (m_overlayLayer)
        m_overlayLayer->addChild(m_gameDetails);
    m_gameDetails->setFont(&m_fontNormal);
    m_gameDetails->setSmallFont(&m_fontSmall);
    m_gameDetails->setTheme(&m_theme);
    m_gameDetails->setThreadPool(&m_threadPool);
    m_gameDetails->setRenderContext(&app().gpu(), &app().renderer());
    m_gameDetails->setAccessibilityVoiceEnabled(m_config.accessibilityEnabled);
    m_gameDetails->setAccessibilitySpeechPreferences(m_config.accessibilitySpeakHints,
                                                     m_config.accessibilitySpeakPosition);
    m_gameDetails->onNavigateSfx([this]() { m_audio.playSfx(Sfx::Navigate); });
    m_gameDetails->onActivateSfx([this]() { m_audio.playSfx(Sfx::Activate); });
    m_gameDetails->onCloseSfx([this]() { m_audio.playSfx(Sfx::ModalHide); });
    m_gameDetails->onAccessibilityAnnouncement([this](const std::string& text) {
        m_accessibility.announce(text);
    });
    m_gameDetails->onOpenGallery([this]() {
        if (!m_gameDetails) return;
        const std::uint64_t titleId = m_gameDetails->titleId();
        const std::string title = m_gameDetails->title();
        // Gallery takes over immediately. Its Back action must resume this
        // dossier, not skip the whole parent flow and land on the home icon.
        m_gameDetailsReturnFocus = nullptr;
        m_gameDetails->hide();
        showGameGallery(titleId, title);
    });
    m_gameDetails->onShowArtwork([this]() {
        if (!m_gameDetails) return;
        m_dialogReturnFocus = m_gameDetails.get();
        showGameArtworkStatus(m_gameDetails->titleId(), m_gameDetails->title());
    });
    m_gameDetails->onRestoreArtwork([this]() {
        if (!m_gameDetails) return;
        m_dialogReturnFocus = m_gameDetails.get();
        showGameArtworkRestoreMenu(m_gameDetails->titleId(), m_gameDetails->title());
    });
    m_gameDetails->onManageMods([this]() {
        if (!m_gameDetails) return;
        showGameMods(m_gameDetails->titleId(), m_gameDetails->title());
    });
    m_gameDetails->onCheats([this]() {
        if (!m_gameDetails) return;
        showGameCheats(m_gameDetails->titleId(), m_gameDetails->title());
    });
    m_gameDetails->onFolderAction([this]() {
        if (!m_gameDetails) return;
        showFolderAssignment(m_gameDetails->titleId(), m_gameDetails->title());
    });
    m_gameDetails->onRemoveGamePort([this]() {
        if (!m_gameDetails) return;
        removeGamePort(m_gameDetails->titleId());
    });
    m_gameDetails->onMarkAsGamePort([this]() {
        if (!m_gameDetails) return;
        m_dialogReturnFocus = m_gameDetails.get();
        showGamePortPlatformMenu(m_gameDetails->titleId(), m_gameDetails->title());
    });
    m_gameDetails->onEditSearchTitle([this]() {
        if (!m_gameDetails) return;
        const std::uint64_t titleId = m_gameDetails->titleId();
        auto& i18n = nxui::I18n::instance();
        requestTextEntry(
            i18n.tr("dialog.edit_search_title", "Edit search title"),
            i18n.tr("dialog.edit_search_title_guide", "Search title"),
            m_gameDetails->searchTitle(), 128, false,
            [this, titleId](const std::string& value) {
                const std::string trimmed = trimWhitespace(value);
                if (trimmed.empty()) return;
                m_config.setGamePortSearchTitle(titleId, trimmed);
                m_config.save();
                if (m_gameDetails && m_gameDetails->titleId() == titleId)
                    m_gameDetails->updateSearchTitle(trimmed);
            });
    });
    // Only reachable from the dossier once a title is already marked as a
    // port -- "Mark as game port" is offered instead in the rail when it is
    // not. Folds edit-search-title and unmark behind one entry rather than
    // two permanent rail rows, matching the same "roll related settings into
    // one submenu" shrink applied to Active artwork.
    m_gameDetails->onPortOptions([this]() {
        if (!m_gameDetails || !m_dialog) return;
        raiseOverlay(m_dialog);
        const std::uint64_t titleId = m_gameDetails->titleId();
        const std::string title = m_gameDetails->title();
        auto& i18n = nxui::I18n::instance();
        auto returnToDetails = [this]() {
            if (m_gameDetails && m_gameDetails->isActive())
                focusManager().setFocus(m_gameDetails.get());
        };
        m_dialog->show(
            i18n.tr("dialog.port_options", "Port options"), title,
            {
                {i18n.tr("dialog.details_rename", "Rename"),
                 [this, titleId]() {
                     auto& editI18n = nxui::I18n::instance();
                     requestTextEntry(
                         editI18n.tr("dialog.details_rename", "Rename"),
                         editI18n.tr("dialog.details_rename_guide",
                                     "Enter a name. Leave it empty to use the original."),
                         m_gameDetails ? m_gameDetails->title() : std::string{}, 128, false,
                         [this, titleId](const std::string& value) {
                             const std::string original = [&]() {
                                 for (const auto& app : m_allApps) {
                                     if (app.titleId != titleId) continue;
                                     return app.title;
                                 }
                                 return std::string{};
                             }();
                             const std::string custom = trimWhitespace(value);
                             m_config.setCustomTitle(titleId, custom);
                             m_config.save();
                             switchu::commitSdCard("custom title");
                             for (auto& app : m_allApps) {
                                 if (app.titleId != titleId) continue;
                                 app.title = m_config.customTitle(titleId, original);
                                 break;
                             }
                             if (m_gameDetails && m_gameDetails->titleId() == titleId)
                                 m_gameDetails->updateTitle(m_config.customTitle(titleId, original));
                             if (m_grid && m_openFolderId == 0)
                                 applyDisplayModel(buildRootFolderModel(), titleId, false);
                         });
                 }, false},
                {i18n.tr("dialog.edit_search_title", "Edit search title"),
                 [this, titleId]() {
                     auto& editI18n = nxui::I18n::instance();
                     requestTextEntry(
                         editI18n.tr("dialog.edit_search_title", "Edit search title"),
                         editI18n.tr("dialog.edit_search_title_guide", "Search title"),
                         m_gameDetails ? m_gameDetails->searchTitle() : std::string{}, 128, false,
                         [this, titleId](const std::string& value) {
                             const std::string trimmed = trimWhitespace(value);
                             if (trimmed.empty()) return;
                             m_config.setGamePortSearchTitle(titleId, trimmed);
                             m_config.save();
                             if (m_gameDetails && m_gameDetails->titleId() == titleId)
                                 m_gameDetails->updateSearchTitle(trimmed);
                         });
                 }, false},
                {i18n.tr("dialog.unmark_port", "Unmark port"),
                 [this, titleId]() { removeGamePort(titleId); }, false},
                {i18n.tr("button.cancel", "Cancel"), returnToDetails, true},
            },
            0, returnToDetails);
        focusManager().setFocus(m_dialog.get());
    });
    m_gameDetails->onRename([this]() {
        if (!m_gameDetails) return;
        const std::uint64_t titleId = m_gameDetails->titleId();
        auto& i18n = nxui::I18n::instance();
        requestTextEntry(
            i18n.tr("dialog.details_rename", "Rename"),
            i18n.tr("dialog.details_rename_guide",
                    "Enter a name. Leave it empty to use the original."),
            m_gameDetails->title(), 128, false,
            [this, titleId](const std::string& value) {
                // An empty field restores the original title reported by the
                // catalogue. Preserve it before changing the visible entry so
                // the restoration does not simply reapply the custom name.
                const std::string original = [&]() {
                    for (const auto& app : m_allApps) {
                        if (app.titleId != titleId) continue;
                        return app.title;
                    }
                    return std::string{};
                }();
                const std::string custom = trimWhitespace(value);
                m_config.setCustomTitle(titleId, custom);
                m_config.save();
                switchu::commitSdCard("custom title");
                for (auto& app : m_allApps) {
                    if (app.titleId != titleId) continue;
                    const std::string previous = app.title;
                    app.title = m_config.customTitle(titleId, original);
                    // The same rule composeRootPending() applies at load. Left
                    // out here, a title with no name of its own kept its hex id
                    // as the SteamGridDB search term until the menu restarted,
                    // so renaming it and fetching artwork straight away found
                    // nothing. A typed name always becomes the search term,
                    // as it does there.
                    if (!custom.empty() || app.englishTitle.empty() || app.englishTitle == previous)
                        app.englishTitle = app.title;
                    // The picker remembers the last query per title; a rename
                    // must replace it or the old term keeps winning.
                    if (!custom.empty())
                        SteamGridDbManager::saveSearchQuery(titleId, app.englishTitle);
                    break;
                }
                if (m_gameDetails && m_gameDetails->titleId() == titleId)
                    m_gameDetails->updateTitle(m_config.customTitle(titleId, original));
                // The entry above is what the grid, the sort and the folders
                // are built from, so recomposing is enough -- and instant.
                // Re-reading the catalogue would cost a full control-data pass
                // over every installed title for one renamed game.
                if (m_grid && m_openFolderId == 0)
                    applyDisplayModel(buildRootFolderModel(), titleId, false);
            });
    });
    m_gameDetails->onTogglePlayOffline([this]() {
        if (!m_gameDetails) return;
        const std::uint64_t titleId = m_gameDetails->titleId();
        auto ids = switchu::offline_titles::load();
        const auto it = std::find(ids.begin(), ids.end(), titleId);
        const bool offline = it == ids.end();
        if (offline)
            ids.push_back(titleId);
        else
            ids.erase(it);
        std::error_code ec;
        std::filesystem::create_directories("sdmc:/config/SwitchU", ec);
        const bool saved = switchu::offline_titles::save(ids);
        switchu::commitSdCard("offline titles");
        DebugLog::log("[offline] title=%016llX offline=%d saved=%d count=%zu",
                      (unsigned long long)titleId, offline ? 1 : 0, saved ? 1 : 0, ids.size());
        if (!saved) return;
        m_gameDetails->setPlayOffline(offline);
        m_audio.playSfx(offline ? Sfx::ConfirmPositive : Sfx::ToggleOff);
    });
    m_gameDetails->onToggleFavorite([this]() {
        if (!m_gameDetails) return;
        const std::uint64_t titleId = m_gameDetails->titleId();
        const bool current = m_config.isFavorite(titleId);
        m_config.setFavorite(titleId, !current);
        m_config.save();
        switchu::commitSdCard("toggle favorite");
        m_gameDetails->setFavorite(!current);
        if (!current) {
            m_audio.playSfx(Sfx::Activate);
        } else {
            m_audio.playSfx(Sfx::ToggleOff);
        }
        if (m_openFolderId != 0) {
            applyDisplayModel(buildOpenFolderModel(m_openFolderId), titleId, false);
        } else {
            applyDisplayModel(buildRootFolderModel(), titleId, false);
        }
    });
    m_gameDetails->onDeleteSoftware([this]() {
        if (!m_gameDetails) return;
        m_dialogReturnFocus = m_gameDetails.get();
        confirmDeleteSoftware(m_gameDetails->titleId(), m_gameDetails->title());
    });
    m_gameDetails->onClosed([this]() {
        if (isCurrentFocusableWidget(m_gameDetailsReturnFocus)) {
            m_suppressNextNavigateSfx = true;
            focusManager().setFocus(m_gameDetailsReturnFocus);
        }
        m_gameDetailsReturnFocus = nullptr;
    });
}

void WiiUMenuApp::reloadThemePresets() {
    m_allPresets = ThemePreset::builtInPresets();
    if (m_config.safeMode()) {
        // Boot-guard safe mode: only the two shipped defaults. A user or package
        // theme (and any video or audio it names) is exactly what may have been
        // crashing the menu, so it is not even parsed. Nothing on the card changes.
        m_allPresets.erase(std::remove_if(m_allPresets.begin(), m_allPresets.end(),
            [](const ThemePreset& p) { return p.name != "Default Dark" && p.name != "Default Light"; }),
            m_allPresets.end());
        DebugLog::log("[safe-mode] presets limited to the built-in defaults (%zu)", m_allPresets.size());
        return;
    }
    auto userPresets = ThemePreset::loadUserPresets();
    auto installedPackages = ThemePreset::loadInstalledPackages();
    m_allPresets.insert(m_allPresets.end(), userPresets.begin(), userPresets.end());
    m_allPresets.insert(m_allPresets.end(), installedPackages.begin(), installedPackages.end());
}

void WiiUMenuApp::startThemePackageTransfer(const ThemeCatalogClient::Entry& entry, bool installMode) {
    syncThemePackageTransfer();

    if (m_themePackageTransferFuture.valid()
        && m_themePackageTransferFuture.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
        if (m_themeShop) {
            auto& i18n = nxui::I18n::instance();
            m_themeShop->requestToast(i18n.tr("themeshop.community.transfer_busy",
                                              "Another theme transfer is already running."),
                                      2.5f);
        }
        return;
    }

    auto shared = std::make_shared<ThemePackageTransferShared>();
    shared->themeId = entry.id;
    shared->installMode = installMode;
    shared->destinationPath = ThemePackageInstaller::destinationRootFor(
        entry.id,
        installMode ? ThemePackageInstaller::Mode::InstallAndApply
                    : ThemePackageInstaller::Mode::InstallOnly);
    auto& i18n = nxui::I18n::instance();
    const std::string preparingLabel = installMode
        ? i18n.tr("themeshop.transfer.prepare_apply", "Preparing download + apply...")
        : i18n.tr("themeshop.transfer.prepare_install", "Preparing download + install...");
    const std::string unknownTransferError = i18n.tr("themeshop.transfer.unknown_error", "Unknown transfer error");
    shared->state.begin(preparingLabel);
    shared->revision = 1;

    m_themePackageTransfer = shared;
    m_themePackageTransferUiRevision = 0;
    m_themePackageTransferHandledRevision = 0;

    if (m_themeShop)
        m_themeShop->setPackageTransferState(shared->state, shared->themeId, shared->installMode);
    // Theme Shop shows on-card / thin-sheet progress — skip the modal so we
    // don't steal focus or pressure the Community preview GPU budget.
    const bool inlineThemeShopProgress = m_themeShop && m_themeShop->isActive();
    if (m_progressDialog && !inlineThemeShopProgress) {
        m_progressDialog->setTheme(&m_theme);
        raiseOverlay(m_progressDialog);
        m_progressDialog->show(i18n.tr("themeshop.transfer.dialog_title", "Downloading Theme"),
                               preparingLabel,
                               -1.f);
        focusManager().setFocus(m_progressDialog.get());
    }

    // The entry's own index, not the shop's first one: with two catalogues
    // merged, half the entries resolve against the other.
    const std::string catalogUrl = !entry.catalogUrl.empty()
        ? entry.catalogUrl
        : (m_themeShop ? m_themeShop->communityCatalogUrl()
                       : std::string(ThemeCatalogClient::kDefaultCatalogUrl));
    ThemePackageInstaller::Mode mode = installMode
        ? ThemePackageInstaller::Mode::InstallAndApply
        : ThemePackageInstaller::Mode::InstallOnly;

    DebugLog::log("[themeshop] package transfer start: id=%s apply=%d",
                  entry.id.c_str(),
                  installMode ? 1 : 0);

    m_themePackageTransferFuture = m_threadPool.submit([shared, catalogUrl, entry, mode, unknownTransferError]() {
        try {
            auto result = ThemePackageInstaller::run(
                catalogUrl,
                entry,
                mode,
                [shared](std::string label, float progress) {
                    std::lock_guard<std::mutex> lk(shared->mutex);
                    shared->state.update(std::move(label), progress);
                    ++shared->revision;
                });

            std::lock_guard<std::mutex> lk(shared->mutex);
            shared->destinationPath = result.destinationPath;
            shared->state.succeed(result.message);
            ++shared->revision;
            DebugLog::log("[themeshop] package transfer success: %s", result.themeId.c_str());
        } catch (const std::exception& ex) {
            std::lock_guard<std::mutex> lk(shared->mutex);
            shared->state.fail(ex.what());
            ++shared->revision;
            DebugLog::log("[themeshop] package transfer failed: %s", ex.what());
        } catch (...) {
            std::lock_guard<std::mutex> lk(shared->mutex);
            shared->state.fail(unknownTransferError);
            ++shared->revision;
            DebugLog::log("[themeshop] package transfer failed: unknown error");
        }
    });
}

const ThemeCatalogClient::IconEntry* WiiUMenuApp::findCatalogIconEntry(
    const std::string& packId) const {
    if (!m_themeShop)
        return nullptr;
    const auto& icons = m_themeShop->catalogIconEntries();
    for (const auto& icon : icons) {
        if (icon.id == packId)
            return &icon;
    }
    return nullptr;
}

void WiiUMenuApp::startIconPackTransfer(const ThemeCatalogClient::IconEntry& entry,
                                        bool applyAfter) {
    syncThemePackageTransfer();

    if (m_themePackageTransferFuture.valid()
        && m_themePackageTransferFuture.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
        if (m_themeShop) {
            auto& i18n = nxui::I18n::instance();
            m_themeShop->requestToast(i18n.tr("themeshop.community.transfer_busy",
                                              "Another theme transfer is already running."),
                                      2.5f);
        }
        return;
    }
    if (!m_themeShop)
        return;

    auto& i18n = nxui::I18n::instance();
    auto shared = std::make_shared<ThemePackageTransferShared>();
    shared->themeId = entry.id;
    shared->installMode = applyAfter;
    shared->isIconPack = true;
    shared->destinationPath =
        switchu::icons::IconPackInstaller::destinationRootFor(entry.id);
    const std::string preparingLabel =
        i18n.tr("themeshop.icons.prepare_download", "Preparing icon pack download...");
    const std::string unknownTransferError =
        i18n.tr("themeshop.transfer.unknown_error", "Unknown transfer error");
    shared->state.begin(preparingLabel);
    shared->revision = 1;

    m_themePackageTransfer = shared;
    m_themePackageTransferUiRevision = 0;
    m_themePackageTransferHandledRevision = 0;

    m_themeShop->setPackageTransferState(shared->state, shared->themeId, true);
    const bool inlineThemeShopProgress = m_themeShop->isActive();
    if (m_progressDialog && !inlineThemeShopProgress) {
        m_progressDialog->setTheme(&m_theme);
        m_progressDialog->show(i18n.tr("themeshop.icons.dialog_title", "Downloading Icon Pack"),
                               preparingLabel,
                               -1.f);
        focusManager().setFocus(m_progressDialog.get());
    }

    const std::string catalogUrl = m_themeShop->communityCatalogUrl();
    DebugLog::log("[icon-install] transfer start: id=%s apply=%d",
                  entry.id.c_str(), applyAfter ? 1 : 0);

    m_themePackageTransferFuture = m_threadPool.submit(
        [shared, catalogUrl, entry, unknownTransferError]() {
            try {
                auto result = switchu::icons::IconPackInstaller::install(
                    catalogUrl,
                    entry,
                    [shared](std::string label, float progress) {
                        std::lock_guard<std::mutex> lk(shared->mutex);
                        shared->state.update(std::move(label), progress);
                        ++shared->revision;
                    });

                std::lock_guard<std::mutex> lk(shared->mutex);
                shared->destinationPath = result.destinationPath;
                shared->state.succeed(result.message);
                ++shared->revision;
                DebugLog::log("[icon-install] transfer success: %s", result.packId.c_str());
            } catch (const std::exception& ex) {
                std::lock_guard<std::mutex> lk(shared->mutex);
                shared->state.fail(ex.what());
                ++shared->revision;
                DebugLog::log("[icon-install] transfer failed: %s", ex.what());
            } catch (...) {
                std::lock_guard<std::mutex> lk(shared->mutex);
                shared->state.fail(unknownTransferError);
                ++shared->revision;
                DebugLog::log("[icon-install] transfer failed: unknown error");
            }
        });
}

void WiiUMenuApp::removeIconPack(const std::string& packId) {
    if (packId.empty())
        return;
    auto& i18n = nxui::I18n::instance();

    if (resolvedIconPackId() == packId) {
        // Fall back to Default before the glyphs disappear.
        applyIconPack(switchu::icons::kDefaultPackId);
    }

    if (!switchu::icons::IconPackInstaller::remove(packId)) {
        if (m_themeShop)
            m_themeShop->requestToast(
                i18n.tr("themeshop.icons.remove_failed", "Could not remove this icon pack."), 2.8f);
        return;
    }

    m_config.iconPackTints.erase(packId);
    m_config.save();
    if (m_themeShop)
        m_themeShop->invalidateIconPackPreviews();
    refreshThemeShopState();
    if (m_themeShop && m_themeShop->detailOpen())
        m_themeShop->closeDetailView();
    if (m_themeShop)
        m_themeShop->requestToast(
            i18n.tr("themeshop.icons.removed", "Icon pack removed."), 2.4f);
}

void WiiUMenuApp::extractThemeIconPack(const std::string& packId, const std::string& themeRoot) {
    if (packId.empty() || themeRoot.empty())
        return;

    const std::string themeFolder = themeFolderFromInstallPath(themeRoot);
    std::string targetPackId = packId;
    std::string themeName;
    std::string themeAuthor;
    {
        // Prefer theme.icons.packId when present (e.g. Atmosphere → atmos).
        nlohmann::json manifest;
        std::ifstream input(joinPath(themeRoot, "theme.json"));
        if (input) {
            try {
                input >> manifest;
            } catch (...) {
                manifest = nlohmann::json{};
            }
        }
        if (manifest.is_object()) {
            readJsonString(manifest, "name", themeName);
            readJsonString(manifest, "author", themeAuthor);
            auto readPackId = [&](const nlohmann::json& iconsNode) {
                if (!iconsNode.is_object())
                    return;
                for (const char* key : {"packId", "pack_id", "pack", "iconPack", "icon_pack"}) {
                    std::string value;
                    if (readJsonString(iconsNode, key, value)) {
                        targetPackId = std::move(value);
                        return;
                    }
                }
            };
            if (auto icons = manifest.find("icons"); icons != manifest.end())
                readPackId(*icons);
            if (auto theme = manifest.find("theme"); theme != manifest.end() && theme->is_object()) {
                if (auto icons = theme->find("icons"); icons != theme->end())
                    readPackId(*icons);
            }
        }
    }

    const std::string sourceDir = themeRoot + "/assets/icons";
    const std::string destDir =
        switchu::icons::IconPackInstaller::destinationRootFor(targetPackId);

    // Companion pack already on the console (builtin atmos, prior download):
    // drop the theme-local copy and any slug-named orphan clone.
    const std::string existing =
        switchu::icons::resolvePackIconsDirectory(SD_ASSETS, targetPackId);
    if (!existing.empty()) {
        std::error_code ec;
        if (switchu::icons::iconDirLooksUsable(sourceDir))
            std::filesystem::remove_all(sourceDir, ec);
        if (!themeFolder.empty() && themeFolder != targetPackId) {
            const std::string orphan =
                switchu::icons::IconPackInstaller::destinationRootFor(themeFolder);
            if (orphan != existing && pathExists(orphan)) {
                std::filesystem::remove_all(orphan, ec);
                DebugLog::log("[icon-pack] removed orphan theme clone pack=%s",
                              themeFolder.c_str());
            }
            if (m_config.iconPack == themeFolder) {
                m_config.iconPack = targetPackId;
                m_config.save();
            }
        }
        DebugLog::log("[icon-pack] theme icons skipped; pack %s already present at %s",
                      targetPackId.c_str(), existing.c_str());
        return;
    }

    if (!switchu::icons::iconDirLooksUsable(sourceDir))
        return;

    std::error_code ec;
    std::filesystem::create_directories(destDir, ec);

    int copied = 0;
    for (const auto& entry : std::filesystem::directory_iterator(sourceDir, ec)) {
        if (ec || !entry.is_regular_file(ec))
            continue;
        const std::string name = entry.path().filename().string();
        if (name.size() < 4 || name.compare(name.size() - 4, 4, ".png") != 0)
            continue; // skip the theme's own *_disabled markers

        std::ifstream input(entry.path(), std::ios::binary);
        if (!input)
            continue;
        std::string bytes((std::istreambuf_iterator<char>(input)),
                          std::istreambuf_iterator<char>());
        std::ofstream output(destDir + "/" + name, std::ios::binary | std::ios::trunc);
        if (!output)
            continue;
        output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        if (output)
            ++copied;
    }

    if (copied == 0) {
        DebugLog::log("[icon-pack] theme icons copy failed: %s", sourceDir.c_str());
        return;
    }

    writeIconPackManifest(destDir, targetPackId,
                          themeName.empty() ? targetPackId : themeName,
                          themeAuthor);

    // The pack now lives in the shared icon directory: drop the copy inside the
    // theme so it is not listed twice.
    std::filesystem::remove_all(sourceDir, ec);
    if (!themeFolder.empty() && themeFolder != targetPackId) {
        const std::string orphan =
            switchu::icons::IconPackInstaller::destinationRootFor(themeFolder);
        if (pathExists(orphan))
            std::filesystem::remove_all(orphan, ec);
    }
    DebugLog::log("[icon-pack] theme icons extracted: theme=%s pack=%s files=%d",
                  themeFolder.c_str(), targetPackId.c_str(), copied);
}

void WiiUMenuApp::syncThemePackageTransfer() {
    auto shared = m_themePackageTransfer;
    if (!shared)
        return;

    bool futureReady = false;
    if (m_themePackageTransferFuture.valid()
        && m_themePackageTransferFuture.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        m_themePackageTransferFuture.get();
        futureReady = true;
    }

    ThemeTransferState state;
    std::string themeId;
    bool installMode = false;
    bool isIconPack = false;
    std::uint64_t revision = 0;
    std::string destinationPath;
    {
        std::lock_guard<std::mutex> lk(shared->mutex);
        state = shared->state;
        themeId = shared->themeId;
        installMode = shared->installMode;
        isIconPack = shared->isIconPack;
        revision = shared->revision;
        destinationPath = shared->destinationPath;
    }

    if (revision != m_themePackageTransferUiRevision) {
        if (m_themeShop)
            m_themeShop->setPackageTransferState(state, themeId, installMode);
        const bool inlineThemeShopProgress = m_themeShop && m_themeShop->isActive();
        if (m_progressDialog && state.isRunning() && !inlineThemeShopProgress) {
            auto& i18n = nxui::I18n::instance();
            m_progressDialog->updateState(
                state.label().empty()
                    ? i18n.tr("themeshop.transfer.downloading", "Downloading theme...")
                    : state.label(),
                state.progress01());
            if (focusManager().current() != m_progressDialog.get())
                focusManager().setFocus(m_progressDialog.get());
        }
        m_themePackageTransferUiRevision = revision;
    }

    if (!futureReady || revision == m_themePackageTransferHandledRevision)
        return;

    if (state.isReady() && isIconPack) {
        auto& i18n = nxui::I18n::instance();
        DebugLog::log("[icon-install] transfer ready: id=%s apply=%d destination=%s",
                      themeId.c_str(), installMode ? 1 : 0, safeLogPath(destinationPath));
        if (installMode)
            applyIconPack(themeId);
        else
            refreshThemeShopState();
        state.succeed(i18n.tr("themeshop.icons.installed", "Icon pack installed."));
        if (m_themeShop)
            m_themeShop->setPackageTransferState(state, themeId, installMode);
    }

    if (state.isReady() && !isIconPack) {
        DebugLog::log("[theme-apply] transfer ready: id=%s installMode=%d destination=%s",
                      themeId.c_str(),
                      installMode ? 1 : 0,
                      safeLogPath(destinationPath));

        // Older theme packages still bundle sidebar glyphs. Move them into the
        // shared icon directory so they show up as a normal downloaded pack
        // (and keep working after the theme is removed).
        extractThemeIconPack(themeId, destinationPath);
        if (m_themeShop)
            m_themeShop->invalidateIconPackPreviews();

        reloadThemePresets();
        DebugLog::log("[theme-apply] presets reloaded after transfer: count=%zu", m_allPresets.size());

        auto& i18n = nxui::I18n::instance();
        std::string successMessage;
        if (installMode) {
            ThemePreset* preset = findPresetPtr("package:" + themeId);
            if (preset) {
                m_forceThemeResourceReload = !preset->fonts.regularPath.empty()
                    || !preset->fonts.smallPath.empty()
                    || !preset->background.imagePath.empty();
                if (!preset->icons.basePath.empty())
                    m_sidebar.invalidateAssetsCache();
                DebugLog::log("[theme-apply] auto-applying installed package: id=%s", themeId.c_str());
                activateThemePreset(preset, true);
                successMessage = i18n.tr("themeshop.transfer.installed_applied", "Theme installed and applied.");
            } else {
                DebugLog::log("[theme-apply] auto-apply failed, preset missing after install: id=%s", themeId.c_str());
                refreshThemeShopState();
                successMessage = i18n.tr("themeshop.transfer.installed_apply_failed",
                                         "Theme installed, but it could not be applied automatically.");
            }
        } else {
            DebugLog::log("[theme-apply] install-only transfer complete: id=%s", themeId.c_str());
            refreshThemeShopState();
            successMessage = i18n.tr("themeshop.transfer.installed", "Theme installed.");
        }

        state.succeed(successMessage);
        if (m_themeShop)
            m_themeShop->setPackageTransferState(state, themeId, installMode);
    }

    if (m_themeShop && !state.label().empty())
        m_themeShop->requestToast(state.label(), state.hasFailed() ? 3.5f : 2.8f);
    if (m_progressDialog && m_progressDialog->isActive()) {
        m_progressDialog->updateState(state.label(), state.progress01());
        m_progressDialog->hide();
        if (m_themeShop && m_themeShop->isActive())
            focusManager().setFocus(m_themeShop.get());
    }

    m_themePackageTransferHandledRevision = revision;
}

std::vector<ThemeShopScreen::ThemeShopEntry> WiiUMenuApp::buildThemeShopEntries() {
    std::vector<ThemeShopScreen::ThemeShopEntry> entries;
    entries.reserve(m_allPresets.size());

    ThemePreset* activePreset = findPresetPtr(m_activePresetName);
    std::string activeId = activePreset ? (activePreset->id.empty() ? activePreset->name : activePreset->id)
                                        : m_activePresetName;

    for (const auto& preset : m_allPresets) {
        ThemeShopScreen::ThemeShopEntry entry;
        entry.id = preset.id.empty() ? preset.name : preset.id;
        entry.name = preset.name;
        entry.author = preset.author;
        entry.version = preset.version;
        entry.source = sourceLabel(preset.source);
        if (preset.soundPreset.rfind("package:", 0) == 0)
            entry.soundPreset = "Bundled";
        else if (preset.soundPreset == "wiiu")
            entry.soundPreset = "Wii U";
        else
            entry.soundPreset = preset.soundPreset;
        entry.coverPath = installedThemePreviewPath(preset);
        // Só o caminho. Medir o que ele ocupa é percorrer centenas de quadros no
        // cartão, e isso a loja faz numa thread de trabalho, não aqui.
        entry.installPath = preset.installPath;
        entry.active = (entry.id == activeId);
        entry.removable = (preset.source != ThemePresetSource::BuiltIn);
        {
            const std::string folder = themeFolderFromPreset(preset);
            const std::string companionId = resolvedThemeIconPackId(preset, folder);
            const std::string iconsDir = resolveThemeAssetPath(preset, preset.icons.basePath);
            entry.hasIcons = switchu::icons::iconDirLooksUsable(iconsDir);
            if (!entry.hasIcons && !companionId.empty()) {
                const std::string packDir =
                    switchu::icons::resolvePackIconsDirectory(SD_ASSETS, companionId);
                entry.hasIcons = switchu::icons::iconDirLooksUsable(packDir);
            }
            if (!entry.hasIcons && !folder.empty() && folder != companionId) {
                const std::string folderPackDir =
                    switchu::icons::resolvePackIconsDirectory(SD_ASSETS, folder);
                entry.hasIcons = switchu::icons::iconDirLooksUsable(folderPackDir);
            }
        }
        // Cheap local dir checks — no I/O beyond a couple of is_directory calls.
        if (!preset.installPath.empty()) {
            const std::string soundBase = resolveThemeSoundBase(preset.installPath);
            const std::string root = soundBase.empty() ? preset.installPath : soundBase;
            entry.hasMusic = pathExists(root + "/music");
            entry.hasSounds = pathExists(root + "/sfx");
        }
        entries.push_back(std::move(entry));
    }

    return entries;
}


std::string WiiUMenuApp::resolvedIconPackId() const {
    const std::string configured = m_config.iconPack.empty()
        ? switchu::icons::kDefaultPackId : m_config.iconPack;
    // Normalize legacy theme:package:<id> ids to theme:<folder>.
    const std::string normalized = switchu::icons::isThemePackId(configured)
        ? switchu::icons::makeThemePackId(configured)
        : configured;
    const std::string dir = switchu::icons::resolvePackIconsDirectory(SD_ASSETS, normalized);
    if (dir.empty()) {
        // Log once per distinct id — resolvedIconPackId is hit every frame.
        static std::string s_lastFailed;
        if (s_lastFailed != normalized) {
            s_lastFailed = normalized;
            DebugLog::log("[icon-pack] resolve failed pack=%s (configured=%s); healing to default",
                          normalized.c_str(), configured.c_str());
        }
        return switchu::icons::kDefaultPackId;
    }
    return normalized;
}

std::string WiiUMenuApp::iconsDirectoryForPack(const std::string& packId) const {
    const std::string id = switchu::icons::isThemePackId(packId)
        ? switchu::icons::makeThemePackId(packId) : packId;
    if (!switchu::icons::isThemePackId(id))
        return {};
    return switchu::icons::resolvePackIconsDirectory(SD_ASSETS, id);
}

namespace {

// Packs that ship with SwitchU: always present, never removable from the shop.
bool isBuiltinIconPack(const std::string& packId) {
    return packId == switchu::icons::kDefaultPackId
        || packId == switchu::icons::kAtmosPackId;
}

} // namespace

std::vector<ThemeShopScreen::ThemeShopEntry> WiiUMenuApp::buildIconPackEntries() {
    std::vector<ThemeShopScreen::ThemeShopEntry> entries;
    const std::string activeId = resolvedIconPackId();

    // Companion pack ids claimed by installed themes (theme.icons.packId).
    std::unordered_map<std::string, std::string> companionByThemeFolder;
    std::unordered_set<std::string> companionPackIds;
    for (const auto& preset : m_allPresets) {
        if (preset.source != ThemePresetSource::InstalledPackage)
            continue;
        const std::string folder = themeFolderFromPreset(preset);
        if (folder.empty())
            continue;
        if (!preset.icons.packId.empty()) {
            companionByThemeFolder[folder] = preset.icons.packId;
            companionPackIds.insert(preset.icons.packId);
        }
    }

    // Heal slug-named clones when the theme now points at a companion pack.
    for (const auto& [folder, companionId] : companionByThemeFolder) {
        if (folder.empty() || folder == companionId)
            continue;
        const std::string companionDir =
            switchu::icons::resolvePackIconsDirectory(SD_ASSETS, companionId);
        if (companionDir.empty())
            continue;
        const std::string orphan =
            switchu::icons::IconPackInstaller::destinationRootFor(folder);
        if (!pathExists(orphan))
            continue;
        std::error_code ec;
        std::filesystem::remove_all(orphan, ec);
        DebugLog::log("[icon-pack] healed orphan clone pack=%s -> %s",
                      folder.c_str(), companionId.c_str());
        if (m_config.iconPack == folder) {
            m_config.iconPack = companionId;
            m_config.save();
        }
    }

    auto addPack = [&](const switchu::icons::IconPackInfo& pack) {
        // Hide theme-folder clones that duplicate a companion pack id.
        if (companionByThemeFolder.count(pack.id)
            && companionByThemeFolder[pack.id] != pack.id
            && companionPackIds.count(companionByThemeFolder[pack.id])) {
            const std::string& companion = companionByThemeFolder[pack.id];
            if (!switchu::icons::resolvePackIconsDirectory(SD_ASSETS, companion).empty())
                return;
        }

        const bool alreadyListed = std::any_of(
            entries.begin(), entries.end(),
            [&](const ThemeShopScreen::ThemeShopEntry& e) { return e.id == pack.id; });
        if (alreadyListed)
            return;

        ThemeShopScreen::ThemeShopEntry entry;
        entry.id = pack.id;
        entry.name = pack.name;
        entry.author = pack.author;
        entry.version = "1.0.0";
        entry.builtin = isBuiltinIconPack(pack.id);
        entry.source = pack.fromTheme ? "Theme"
            : (entry.builtin ? "Built-in" : "Installed");
        entry.coverPath = switchu::icons::ensurePackPreview(pack.directory, pack.id);
        entry.active = (pack.id == activeId);
        // Downloaded / extracted packs on SD can be removed; builtins and
        // in-theme glyphs stay tied to their theme.
        entry.installed = true;
        entry.removable = !pack.fromTheme && !entry.builtin;
        entry.colourPack = pack.colourPack;
        entries.push_back(std::move(entry));
    };

    for (const auto& pack : switchu::icons::discoverPacks(SD_ASSETS))
        addPack(pack);
    // Theme icon folders are discovered from disk the same way as SD packs.
    for (const auto& pack : switchu::icons::discoverThemePacks())
        addPack(pack);

    // Online catalog: packs that are not on the console yet.
    if (m_themeShop) {
        const std::string& catalogUrl = m_themeShop->communityCatalogUrl();
        const std::size_t slash = catalogUrl.find_last_of('/');
        const std::string catalogBase =
            slash == std::string::npos ? std::string() : catalogUrl.substr(0, slash + 1);
        for (const auto& icon : m_themeShop->catalogIconEntries()) {
            const bool alreadyListed = std::any_of(
                entries.begin(), entries.end(),
                [&](const ThemeShopScreen::ThemeShopEntry& e) { return e.id == icon.id; });
            if (alreadyListed)
                continue;
            // Catalog row whose theme companion is already installed as another id.
            if (!icon.theme.empty()) {
                std::string linkedPack;
                if (auto it = companionByThemeFolder.find(icon.theme);
                    it != companionByThemeFolder.end())
                    linkedPack = it->second;
                const bool themeCompanionInstalled = std::any_of(
                    entries.begin(), entries.end(),
                    [&](const ThemeShopScreen::ThemeShopEntry& e) {
                        return e.installed && (e.id == icon.theme
                            || (!linkedPack.empty() && e.id == linkedPack));
                    });
                if (themeCompanionInstalled)
                    continue;
            }

            ThemeShopScreen::ThemeShopEntry entry;
            entry.id = icon.id;
            entry.name = icon.name;
            entry.author = icon.author;
            entry.version = icon.version;
            entry.description = icon.description;
            entry.builtin = icon.builtin;
            entry.catalog = true;
            entry.installed = false;
            entry.removable = false;
            entry.active = false;
            entry.colourPack = icon.colour;
            entry.source = "Shop";
            if (!icon.cover.empty()) {
                const bool absolute = icon.cover.rfind("https://", 0) == 0
                    || icon.cover.rfind("http://", 0) == 0;
                entry.coverUrl = absolute ? icon.cover : catalogBase + icon.cover;
            }
            entries.push_back(std::move(entry));
        }
    }
    return entries;
}

std::string WiiUMenuApp::resolveActiveIconGlyphPath(const char* fileName) const {
    return switchu::icons::resolveGlyphPath(SD_ASSETS, resolvedIconPackId(), fileName);
}

void WiiUMenuApp::reloadSidebarIcons() {
    const std::string packId = resolvedIconPackId();
    // Theme packs resolve to their on-disk icons dir; SD packs leave this empty
    // and SidebarManager loads from icons/<packId>/ via packId.
    const std::string customIconsBase = iconsDirectoryForPack(packId);
    const std::string fallbackPackId = switchu::icons::isThemePackId(packId)
        ? switchu::icons::kDefaultPackId : packId;
    DebugLog::log("[icon-pack] reload pack=%s custom=%s fallback=%s",
                  packId.c_str(),
                  customIconsBase.empty() ? "<empty>" : customIconsBase.c_str(),
                  fallbackPackId.c_str());
    m_sidebar.invalidateAssetsCache();
    m_sidebar.reloadAssets(app().gpu(), app().renderer(), SD_ASSETS, customIconsBase, fallbackPackId);
    // The layout-flip chrome button uses the same pack, so refresh it too.
    if (m_layoutFlip) {
        m_layoutFlip->setIconHslAdjust(m_sidebar.iconHslAdjust());
        m_layoutFlip->setColourIconContrast(m_sidebar.colourIconContrast());
        m_layoutFlip->reloadIcon(app().gpu(), app().renderer(),
                                 resolveActiveIconGlyphPath(switchu::icons::kChangeViewFile));
    }
}

void WiiUMenuApp::applyIconPack(const std::string& packId) {
    std::string id = packId.empty() ? switchu::icons::kDefaultPackId : packId;
    if (switchu::icons::isThemePackId(id))
        id = switchu::icons::makeThemePackId(id);
    else
        id = switchu::icons::canonicalizeIconPackId(id);
    const bool packChanged = m_config.iconPack != id;
    if (packChanged) {
        m_config.iconPack = id;
        m_config.save();
    }
    DebugLog::log("[icon-pack] apply pack=%s changed=%d", id.c_str(), packChanged ? 1 : 0);
    // Bake once with the pack's current HSL (avoid reload → sync → reload).
    applyActivePackHslToSidebar();
    reloadSidebarIcons();
    if (m_quickSettings)
        m_quickSettings->invalidateMusicControlIcons();
    syncColourIconContrast();
    refreshThemeShopState();
    if (m_themeShop)
        m_themeShop->requestToast(
            nxui::I18n::instance().tr("themeshop.icons.applied", "Icon pack applied."), 2.0f);
}

void WiiUMenuApp::refreshThemeShopState(bool rebuildTab) {
    if (!m_themeShop) return;

    ThemePreset* activePreset = findPresetPtr(m_activePresetName);
    std::string activeId = activePreset ? (activePreset->id.empty() ? activePreset->name : activePreset->id)
                                        : m_activePresetName;

    // Normalize legacy theme:package:* ids without clobbering a valid selection.
    const std::string activePackId = resolvedIconPackId();
    if (m_config.iconPack != activePackId && switchu::icons::isThemePackId(m_config.iconPack)
        && activePackId != switchu::icons::kDefaultPackId) {
        m_config.iconPack = activePackId;
        m_config.save();
    }

    m_themeShop->setTheme(&m_theme);
    m_themeShop->setMusicState(m_audio.isPlaying(), m_audio.volume(), m_audio.sfxVolume());
    m_themeShop->setGridLayoutState(m_config.gridColumns, m_config.gridRows);
    m_themeShop->setActionHintStyleState(m_config.actionHintStyle);
    m_themeShop->setSortLargeTilesState(m_config.sortLargeTiles);
    m_themeShop->setBatterySaverState(m_config.batterySaver);
    m_themeShop->setFrameTapLiteEngaged(m_frameTapStatus.viewerConnected());
    m_themeShop->setIconContrastAdaptState(m_config.iconContrastAdapt);
    m_themeShop->setThemeMusicOverrideState(m_config.themeMusicOverride);
    m_themeShop->setThemeMusicAddToPlaylistState(m_config.themeMusicAddToPlaylist);
    m_themeShop->setThemeMusicPlayOnApplyState(m_config.themeMusicPlayOnApply);
    m_themeShop->setThemeMatchAccent(m_activeColors.accentH,
                                     m_activeColors.accentS,
                                     m_activeColors.accentL);
    {
        std::unordered_map<std::string, ThemeShopScreen::IconPackTintState> tints;
        for (const auto& [packId, tint] : m_config.iconPackTints) {
            ThemeShopScreen::IconPackTintState state;
            state.enabled = tint.enabled;
            state.flatten = tint.flatten;
            state.grayscale = tint.grayscale;
            state.matchTheme = tint.matchTheme;
            state.hue = tint.hue;
            state.sat = tint.sat;
            state.light = tint.light;
            tints[packId] = state;
        }
        m_themeShop->setIconPackTintState(tints);
    }
    m_themeShop->setThemeShopState(buildThemeShopEntries(), activeId);
    m_themeShop->setIconPackState(buildIconPackEntries(), activePackId);
    // refreshState() honours begin/endStateRefreshBatch (fork), unlike a
    // direct rebuildCurrentTab().
    if (rebuildTab)
        m_themeShop->refreshState();
}

void WiiUMenuApp::applyActivePackHslToSidebar() {
    const AppConfig::IconPackTint packTint = m_config.iconPackTintFor(resolvedIconPackId());
    switchu::icons::HslAdjust hsl;
    hsl.enabled = packTint.enabled;
    hsl.flatten = packTint.flatten;
    hsl.grayscale = packTint.grayscale;
    hsl.hue = std::clamp(packTint.hue, 0.f, 1.f);
    hsl.sat = std::clamp(packTint.sat, 0.f, 1.f);
    hsl.light = std::clamp(packTint.light, 0.f, 1.f);
    m_sidebar.setIconHslAdjust(hsl);
    if (m_layoutFlip)
        m_layoutFlip->setIconHslAdjust(hsl);
}

void WiiUMenuApp::activateThemePreset(ThemePreset* preset, bool applyBundledSound) {
    if (!preset) return;

    const std::string presetRef = preset->id.empty() ? preset->name : preset->id;
    DebugLog::log("[theme-apply] begin preset=%s name=%s source=%s installPath=%s applyBundledSound=%d soundPreset=%s",
                  presetRef.c_str(),
                  preset->name.c_str(),
                  themeSourceName(preset->source),
                  safeLogPath(preset->installPath),
                  applyBundledSound ? 1 : 0,
                  safeLogPath(preset->soundPreset));

    m_activePresetName = preset->id.empty() ? preset->name : preset->id;
    m_activeColors = preset->colors;
    m_activeMode = preset->mode;
    m_config.themePreset = m_activePresetName;

    // Themes that ship icons become the active Icons-library pack (overrideable).
    {
        std::string folder = themeFolderFromPreset(*preset);
        if (folder.empty())
            folder = presetRef;
        const std::string companionId = resolvedThemeIconPackId(*preset, folder);
        // Prefer theme.icons.packId (e.g. atmos), then folder extract, then theme:*.
        std::string selectedId;
        std::string selectedDir;
        auto tryPack = [&](const std::string& id) {
            if (id.empty() || !selectedId.empty())
                return;
            const std::string dir =
                switchu::icons::resolvePackIconsDirectory(SD_ASSETS, id);
            if (dir.empty())
                return;
            selectedId = id;
            selectedDir = dir;
        };
        tryPack(companionId);
        if (folder != companionId)
            tryPack(folder);
        tryPack(switchu::icons::makeThemePackId(folder));
        if (!selectedId.empty() && m_config.iconPack != selectedId) {
            m_config.iconPack = selectedId;
            DebugLog::log("[icon-pack] theme apply selected pack=%s dir=%s",
                          selectedId.c_str(), selectedDir.c_str());
        }
    }
    m_config.save();

    // Rematch Match-theme packs to the new accent before baking sidebar glyphs
    // so we only reloadAssets once (old path: bake → rematch → bake again).
    rematchIconPackTintsToTheme(/*syncSidebar=*/false);
    applyActivePackHslToSidebar();

    // Cancel in-flight Theme Shop preview workers so Apply does not fight
    // cover uploads for GPU / waitIdle under Applet pressure (Frame Tap).
    if (m_themeShop)
        m_themeShop->pausePreviewWorkForSleep();

    DebugLog::log("[theme-apply] rebuildThemeFromColors start: preset=%s", presetRef.c_str());
    rebuildThemeFromColors();
    DebugLog::log("[theme-apply] rebuildThemeFromColors done: preset=%s", presetRef.c_str());


    // HSL already applied above - sync should be a no-op reload when unchanged.
    syncColourIconContrast();

    // Sound already loads off-thread; queue after visuals so Apply returns sooner.
    // Theme Shop Options: replace playlist (legacy), merge into it, or leave alone.
    if (applyBundledSound && !preset->soundPreset.empty()) {
        if (m_config.themeMusicOverride) {
            DebugLog::log("[theme-apply] changeSoundPreset queued: preset=%s sound=%s",
                          presetRef.c_str(),
                          preset->soundPreset.c_str());
            m_config.soundPreset = preset->soundPreset;
            changeSoundPreset(preset->soundPreset);
        } else if (m_config.themeMusicAddToPlaylist) {
            const std::string base = resolveThemeSoundBase(preset->installPath);
            const std::string musicDir = base.empty() ? std::string() : (base + "/music");
            std::string album = "theme";
            if (!preset->installPath.empty()) {
                const auto pos = preset->installPath.find_last_of("/\\");
                album = "theme-" + ((pos == std::string::npos)
                    ? preset->installPath
                    : preset->installPath.substr(pos + 1));
            } else if (!presetRef.empty()) {
                album = "theme-" + presetRef;
            }
            std::error_code musicDirEc;
            if (!musicDir.empty() && std::filesystem::is_directory(musicDir, musicDirEc)
                && !m_presetChangePending && !m_themeMusicMergePending) {
                DebugLog::log("[theme-apply] merge theme music: dir=%s album=%s play=%d",
                              safeLogPath(musicDir),
                              album.c_str(),
                              (m_config.themeMusicPlayOnApply && m_config.musicEnabled) ? 1 : 0);
                m_themeMusicMergePending = true;
                m_themeMusicMergePlay = m_config.themeMusicPlayOnApply && m_config.musicEnabled;
                m_themeMusicMergeFirstIndex = m_audio.trackCount();
                m_audioFuture = m_threadPool.submit([this, musicDir, album]() {
                    const size_t n = appendMp3TracksFromDir(m_audio, musicDir, album);
                    DebugLog::log("[audio] merged %zu theme track(s) from %s",
                                  n, safeLogPath(musicDir));
                });
            } else {
                DebugLog::log("[theme-apply] merge theme music skipped: dir=%s pending=%d",
                              safeLogPath(musicDir),
                              (m_presetChangePending || m_themeMusicMergePending) ? 1 : 0);
            }
        } else {
            DebugLog::log("[theme-apply] bundled sound skipped by Options (override off, add off): preset=%s",
                          presetRef.c_str());
        }
    } else {
        DebugLog::log("[theme-apply] bundled sound skipped: preset=%s apply=%d soundPreset=%s",
                      presetRef.c_str(),
                      applyBundledSound ? 1 : 0,
                      safeLogPath(preset->soundPreset));
    }

    // Light shop refresh: update Applied badges / tints without rebuildCurrentTab.
    DebugLog::log("[theme-apply] refreshThemeShopState start: preset=%s (light)", presetRef.c_str());
    refreshThemeShopState(/*rebuildTab=*/false);
    DebugLog::log("[theme-apply] refreshThemeShopState done: preset=%s", presetRef.c_str());
    m_themeRenderDebugFrames = 6;
    if (m_themeShop)
        m_themeShop->requestRenderDiagnostics(6);
    m_audio.playSfx(Sfx::ThemeToggle);
    DebugLog::log("[theme-apply] complete preset=%s", presetRef.c_str());
}

// Troca a trilha pela do tema. Fica separado de changeSoundPreset porque as
// duas coisas nao andam juntas: o preset governa os efeitos e continua valendo,
// enquanto as faixas passam a ser propriedade do tema.
void WiiUMenuApp::applyThemeMusic(const std::vector<std::string>& tracks) {
    if (m_audioFuture.valid())
        m_audioFuture.get();   // o audio carrega em outra linha; sem isto as
                               // faixas do preset chegariam depois destas
    if (tracks.empty()) {
        DebugLog::log("[audio] theme declared music but none of it exists");
        return;
    }

    m_themeMusicTracks = tracks;
    reloadMusicTracks();
}

void WiiUMenuApp::applyAutoThemeConfig() {
    using switchu::services::AutoThemeMode;
    using switchu::services::AutoThemeSettings;

    AutoThemeSettings settings;
    if (m_config.autoThemeMode == "manual")
        settings.mode = AutoThemeMode::ManualHours;
    else if (m_config.autoThemeMode == "geo")
        settings.mode = AutoThemeMode::Geolocation;
    else
        settings.mode = AutoThemeMode::Off;

    settings.dayPreset = m_config.autoThemeDayPreset;
    settings.nightPreset = m_config.autoThemeNightPreset;
    settings.dayStartHour = m_config.autoThemeDayStartHour;
    settings.nightStartHour = m_config.autoThemeNightStartHour;

    m_autoTheme.configure(settings);

    // Seed the geolocation provider with the cached position, if any.
    if (m_config.autoThemeGeoResolved)
        m_autoTheme.setGeoLocation(switchu::services::GeoLocation{
            m_config.autoThemeGeoLat, m_config.autoThemeGeoLon});
    else
        m_autoTheme.setGeoLocation(std::nullopt);

    DebugLog::log("[auto-theme] configured: mode=%s day=%s night=%s dayStart=%d nightStart=%d",
                  m_config.autoThemeMode.c_str(),
                  safeLogPath(settings.dayPreset),
                  safeLogPath(settings.nightPreset),
                  settings.dayStartHour,
                  settings.nightStartHour);
}

void WiiUMenuApp::evaluateAutoTheme(bool force) {
    if (force)
        m_autoTheme.reset();

    const auto snapshot = m_clockService.refresh();
    auto desired = m_autoTheme.evaluate(snapshot);
    if (!desired)
        return;

    if (*desired == m_activePresetName)
        return;

    ThemePreset* preset = findPresetPtr(*desired);
    if (!preset) {
        DebugLog::log("[auto-theme] target preset not found: %s", desired->c_str());
        return;
    }

    DebugLog::log("[auto-theme] switching to preset=%s (hour=%u)",
                  desired->c_str(),
                  static_cast<unsigned>(snapshot.calendar.hour));
    // Auto switching keeps the current sound preset (applyBundledSound=false).
    activateThemePreset(preset, false);
}

void WiiUMenuApp::confirmSendLogs() {
    auto& i18n = nxui::I18n::instance();
    if (m_logUploadFuture.valid()) {
        if (m_settings)
            m_settings->requestToast(
                i18n.tr("settings.system.send_logs_sending", "Sending logs..."), 2.5f);
        return;
    }
    if (!m_dialog) {
        startLogUpload();
        return;
    }

    // The logs name the installed games, how long each was played and the
    // profile nicknames. That leaves the console only after the player has
    // been told so and agreed.
    m_audio.playSfx(Sfx::ModalShow);
    raiseOverlay(m_dialog);
    m_dialog->show(
        i18n.tr("settings.system.send_logs", "Send logs to the developer"),
        i18n.tr("settings.system.send_logs_confirm",
                "The logs list your games, play times and user nicknames.\n\n"
                "Send them over the internet to the SwitchU developer?"),
        {
            {i18n.tr("button.cancel", "Cancel"), []() {}, true},
            {i18n.tr("settings.system.send_logs_button", "Send"), [this]() {
                m_audio.playSfx(Sfx::ConfirmPositive);
                startLogUpload();
            }, true}
        });
}

void WiiUMenuApp::startLogUpload() {
    if (m_logUploadFuture.valid())
        return;

    // The live logs are held open and cannot be read; rotating leaves closed
    // copies, which are what gets sent.
    const Result rc = m_launcher.rotateLogs();
    switchu::commitSdCard("log rotation");
    DebugLog::log("[log-upload] requested (daemon rotate rc=0x%X)", rc);

    if (m_settings)
        m_settings->requestToast(
            nxui::I18n::instance().tr("settings.system.send_logs_sending", "Sending logs..."),
            3.0f);

    m_logUploadOutcome = std::make_shared<switchu::log_upload::Outcome>();
    auto outcome = m_logUploadOutcome;
    m_logUploadFuture = m_threadPool.submit([outcome]() {
        *outcome = switchu::log_upload::send();
    });
}

void WiiUMenuApp::pollLogUpload() {
    if (!m_logUploadFuture.valid())
        return;
    if (m_logUploadFuture.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
        return;

    m_logUploadFuture.get();
    const switchu::log_upload::Outcome outcome =
        m_logUploadOutcome ? *m_logUploadOutcome : switchu::log_upload::Outcome{};
    m_logUploadOutcome.reset();

    if (!m_settings)
        return;
    auto& i18n = nxui::I18n::instance();
    if (outcome.ok) {
        m_settings->requestToast(
            i18n.tr("settings.system.send_logs_done", "Logs sent. Thank you!"), 4.0f);
    } else if (outcome.empty) {
        m_settings->requestToast(
            i18n.tr("settings.system.send_logs_empty", "There are no logs to send."), 4.0f);
    } else {
        m_settings->requestToast(
            i18n.tr("settings.system.send_logs_failed",
                    "Could not send the logs. Check the internet connection, or use "
                    "\"Save logs for copying\" instead."),
            6.0f);
    }
}

void WiiUMenuApp::maybeFetchGeoLocation() {
    if (!m_autoTheme.needsGeoLocation() || m_geoFetchInFlight)
        return;

    m_geoFetchInFlight = true;
    m_geoFetchResult = std::make_shared<GeoFetchResult>();
    DebugLog::log("[auto-theme][geo] starting IP geolocation lookup");
    auto result = m_geoFetchResult;
    m_geoFetchFuture = m_threadPool.submit([result]() {
        try {
            // The Switch has no GPS; approximate the position from the public IP.
            const std::string body = themeshop::http::getText(
                "http://ip-api.com/json/?fields=status,message,lat,lon,city");
            const auto j = nlohmann::json::parse(body);
            if (j.value("status", std::string()) == "success") {
                result->lat = j.value("lat", 0.0);
                result->lon = j.value("lon", 0.0);
                result->city = j.value("city", std::string());
                result->ok = true;
            } else {
                DebugLog::log("[auto-theme][geo] api status not success: %s",
                              j.value("message", std::string("?")).c_str());
            }
        } catch (const std::exception& ex) {
            DebugLog::log("[auto-theme][geo] lookup failed: %s", ex.what());
        } catch (...) {
            DebugLog::log("[auto-theme][geo] lookup failed: unknown error");
        }
    });
}

void WiiUMenuApp::pollGeoLocationFetch() {
    if (!m_geoFetchInFlight || !m_geoFetchFuture.valid())
        return;
    if (m_geoFetchFuture.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
        return;

    m_geoFetchFuture.get();
    const GeoFetchResult r = m_geoFetchResult ? *m_geoFetchResult : GeoFetchResult{};
    m_geoFetchResult.reset();
    m_geoFetchInFlight = false;

    if (!r.ok) {
        DebugLog::log("[auto-theme][geo] no location resolved (offline or API error)");
        return;
    }

    DebugLog::log("[auto-theme][geo] resolved lat=%.4f lon=%.4f city=%s",
                  r.lat, r.lon, r.city.c_str());

    m_config.autoThemeGeoResolved = true;
    m_config.autoThemeGeoLat = r.lat;
    m_config.autoThemeGeoLon = r.lon;
    m_config.autoThemeGeoCity = r.city;
    m_autoTheme.setGeoLocation(switchu::services::GeoLocation{r.lat, r.lon});

    // Persist the resolved location so future launches skip the network request.
    if (m_configSaveFuture.valid())
        m_configSaveFuture.wait();
    m_configSaveFuture = m_threadPool.submit([cfg = m_config]() { cfg.save(); });

    evaluateAutoTheme(true);
    pushGeoDisplayToScreen();
}

void WiiUMenuApp::pushGeoDisplayToScreen() {
    if (!m_autoThemeScreen)
        return;

    const bool resolved = m_config.autoThemeGeoResolved;
    std::string city, sunrise, sunset;

    if (resolved) {
        city = m_config.autoThemeGeoCity.empty()
            ? fmt::format("{:.2f}, {:.2f}", m_config.autoThemeGeoLat, m_config.autoThemeGeoLon)
            : m_config.autoThemeGeoCity;

        auto& i18n = nxui::I18n::instance();
        const auto snap = m_clockService.refresh();
        const auto sun = m_autoTheme.geoSunTimes(snap);
        if (sun.valid) {
            if (sun.polarDay) {
                sunrise = sunset = i18n.tr("autotheme.geo_midnight_sun", "Midnight sun");
            } else if (sun.polarNight) {
                sunrise = sunset = i18n.tr("autotheme.geo_polar_night", "Polar night");
            } else {
                auto fmtHour = [this](double h) {
                    int hh = static_cast<int>(std::floor(h));
                    int mm = static_cast<int>(std::llround((h - hh) * 60.0));
                    if (mm >= 60) { mm -= 60; ++hh; }
                    hh = ((hh % 24) + 24) % 24;
                    if (m_config.clockUse12Hour) {
                        int h12 = hh % 12;
                        if (h12 == 0) h12 = 12;
                        return fmt::format("{}:{:02d} {}", h12, mm, hh >= 12 ? "PM" : "AM");
                    }
                    return fmt::format("{:02d}:{:02d}", hh, mm);
                };
                sunrise = fmtHour(sun.sunriseLocalHours);
                sunset = fmtHour(sun.sunsetLocalHours);
            }
        }
    }

    m_autoThemeScreen->updateGeoDisplay(resolved, city, sunrise, sunset);
}

void WiiUMenuApp::applyUiLanguage() {
    auto& i18n = nxui::I18n::instance();
    if (m_config.uiLanguageOverride == "auto" || m_config.uiLanguageOverride.empty())
        i18n.setLanguageAuto();
    else
        i18n.setLanguage(m_config.uiLanguageOverride);
    m_accessibility.setVoiceForLanguageTag(i18n.activeLanguageTag());
}

std::string WiiUMenuApp::resolveThemeAssetPath(const ThemePreset& preset, const std::string& rawPath) const {
    if (rawPath.empty())
        return {};
    if (isAbsoluteThemePath(rawPath))
        return rawPath;
    if ((preset.source == ThemePresetSource::InstalledPackage || preset.source == ThemePresetSource::BuiltIn)
        && !preset.installPath.empty())
        return joinPath(preset.installPath, rawPath);
    return joinPath(SD_ASSETS, rawPath);
}

ThemePreset WiiUMenuApp::buildEffectiveThemePreset() {
    ThemePreset effective;
    if (ThemePreset* preset = findPresetPtr(m_activePresetName))
        effective = *preset;

    effective.mode = m_activeMode;
    effective.colors = m_activeColors;
    return effective;
}

void WiiUMenuApp::applyThemeResources(const ThemePreset& preset) {
    // A trilha do tema decidida aqui, e nao no caminho de aplicar manualmente.
    // Este e o unico ponto por onde os dois passam: no boot o tema e carregado
    // direto por aqui, entao a musica so trocava quando alguem aplicava a mao,
    // e reiniciar o console devolvia a trilha do preset.
    //
    // Definida sempre, inclusive vazia: um tema sem trilha precisa devolver a
    // musica ao preset, e isso tem de estar resolvido antes de changeSoundPreset
    // ser disparado, porque ele carrega em outra thread e le este valor.
    m_themeMusicTracks.clear();
    if (!preset.music.empty() && !m_config.safeMode()) {
        for (const auto& track : preset.music) {
            std::string path = resolveThemeAssetPath(preset, track);
            std::error_code ec;
            if (!path.empty() && std::filesystem::exists(path, ec))
                m_themeMusicTracks.push_back(std::move(path));
            else
                DebugLog::log("[theme-apply] theme track missing: %s", safeLogPath(path));
        }
        applyThemeMusic(m_themeMusicTracks);
    }

    auto& gpu = app().gpu();
    auto& ren = app().renderer();
    const bool forceResourceReload = m_forceThemeResourceReload;

    const std::string fallbackFontPath = std::string(SD_ASSETS) + "/fonts/DejaVuSans.ttf";
    const std::string regularFontPath = resolveThemeAssetPath(preset, preset.fonts.regularPath);
    const std::string smallFontPath = resolveThemeAssetPath(
        preset,
        !preset.fonts.smallPath.empty() ? preset.fonts.smallPath : preset.fonts.regularPath);
    const std::string themeIconsBase = resolveThemeAssetPath(preset, preset.icons.basePath);
    const std::string backgroundImagePath = resolveThemeAssetPath(preset, preset.background.imagePath);

    const std::string presetRef = preset.id.empty() ? preset.name : preset.id;
    DebugLog::log("[theme-apply] resources start: preset=%s regularFont=%s smallFont=%s iconsBase=%s bgImage=%s",
                  presetRef.c_str(),
                  safeLogPath(regularFontPath),
                  safeLogPath(smallFontPath),
                  safeLogPath(themeIconsBase),
                  safeLogPath(backgroundImagePath));

    bool settingsLayoutNeedsRebuild = false;

    const bool regularFontExists = !regularFontPath.empty() && pathExists(regularFontPath);
    const std::string desiredRegularFontPath = regularFontExists ? regularFontPath : fallbackFontPath;
    const bool regularFontNeedsReload = forceResourceReload || m_loadedRegularFontPath != desiredRegularFontPath;

    const bool smallFontExists = !smallFontPath.empty() && pathExists(smallFontPath);
    const std::string desiredSmallFontPath = smallFontExists ? smallFontPath : fallbackFontPath;
    const bool smallFontNeedsReload = forceResourceReload || m_loadedSmallFontPath != desiredSmallFontPath;

    const std::string defaultGameCardPath = std::string(SD_ASSETS) + "/icons/default/gamecard.png";
    const std::string gameCardPath = !themeIconsBase.empty() ? joinPath(themeIconsBase, "gamecard.png") : std::string();
    const bool gameCardExists = !gameCardPath.empty() && pathExists(gameCardPath);
    const std::string desiredGameCardPath = gameCardExists ? gameCardPath : defaultGameCardPath;
    const bool gameCardNeedsReload = forceResourceReload || m_loadedGameCardPath != desiredGameCardPath;

    const bool imageExists = !backgroundImagePath.empty() && pathExists(backgroundImagePath);
    const bool wantsBackgroundImage = imageExists;
    // A frames-only theme has no imagePath, so both sides of the path check are
    // empty and this decided nothing needed loading -- the frames never loaded
    // and the wallpaper came up blank. The gate has to know about the sequence
    // as well as the single image.
    const bool wantsFrames = !preset.background.imageFrames.empty();
    const bool hasFramesLoaded = m_background && m_background->hasAnimatedBackground();
    const bool backgroundImageNeedsReload = forceResourceReload
        || m_loadedBackgroundImagePath != backgroundImagePath
        || m_backgroundImageLoaded != wantsBackgroundImage
        || wantsFrames != hasFramesLoaded;

    const bool needsGpuResourceReload = regularFontNeedsReload
        || smallFontNeedsReload
        || gameCardNeedsReload
        || backgroundImageNeedsReload;
    if (needsGpuResourceReload) {
        DebugLog::log("[theme-apply] waiting for GPU idle before reloading theme resources");
        gpu.waitIdle();
    }

    if (regularFontNeedsReload) {
        bool regularFontLoaded = regularFontExists && m_fontNormal.load(gpu, ren, regularFontPath, 24);
        DebugLog::log("[theme-apply] regular font: path=%s exists=%d reloaded=%d loaded=%d",
                      safeLogPath(regularFontPath),
                      regularFontExists ? 1 : 0,
                      1,
                      regularFontLoaded ? 1 : 0);
        if (!regularFontLoaded) {
            regularFontLoaded = m_fontNormal.load(gpu, ren, fallbackFontPath, 24);
            DebugLog::log("[theme-apply] regular font fallback: path=%s loaded=%d",
                          fallbackFontPath.c_str(),
                          regularFontLoaded ? 1 : 0);
        }
        if (regularFontLoaded) {
            m_loadedRegularFontPath = desiredRegularFontPath;
            settingsLayoutNeedsRebuild = true;
        }
    } else {
        DebugLog::log("[theme-apply] regular font: path=%s exists=%d reloaded=%d loaded=%d",
                      safeLogPath(regularFontPath),
                      regularFontExists ? 1 : 0,
                      0,
                      1);
    }

    if (smallFontNeedsReload) {
        bool smallFontLoaded = smallFontExists && m_fontSmall.load(gpu, ren, smallFontPath, 18);
        DebugLog::log("[theme-apply] small font: path=%s exists=%d reloaded=%d loaded=%d",
                      safeLogPath(smallFontPath),
                      smallFontExists ? 1 : 0,
                      1,
                      smallFontLoaded ? 1 : 0);
        if (!smallFontLoaded) {
            smallFontLoaded = m_fontSmall.load(gpu, ren, fallbackFontPath, 18);
            DebugLog::log("[theme-apply] small font fallback: path=%s loaded=%d",
                          fallbackFontPath.c_str(),
                          smallFontLoaded ? 1 : 0);
        }
        if (smallFontLoaded) {
            m_loadedSmallFontPath = desiredSmallFontPath;
            settingsLayoutNeedsRebuild = true;
        }
    } else {
        DebugLog::log("[theme-apply] small font: path=%s exists=%d reloaded=%d loaded=%d",
                      safeLogPath(smallFontPath),
                      smallFontExists ? 1 : 0,
                      0,
                      1);
    }

    if (gameCardNeedsReload) {
        bool gameCardLoaded = gameCardExists && m_gameCardTex.loadFromFile(gpu, ren, gameCardPath);
        DebugLog::log("[theme-apply] gamecard icon: path=%s exists=%d reloaded=%d loaded=%d",
                      safeLogPath(gameCardPath),
                      gameCardExists ? 1 : 0,
                      1,
                      gameCardLoaded ? 1 : 0);
        if (!gameCardLoaded) {
            gameCardLoaded = m_gameCardTex.loadFromFile(gpu, ren, defaultGameCardPath);
            DebugLog::log("[theme-apply] gamecard fallback: path=%s loaded=%d",
                          defaultGameCardPath.c_str(),
                          gameCardLoaded ? 1 : 0);
        }
        if (gameCardLoaded)
            m_loadedGameCardPath = desiredGameCardPath;
    } else {
        DebugLog::log("[theme-apply] gamecard icon: path=%s exists=%d reloaded=%d loaded=%d",
                      safeLogPath(gameCardPath),
                      gameCardExists ? 1 : 0,
                      0,
                      1);
    }

    if (m_background) {
        DebugLog::log("[theme-apply] background config start: preset=%s", presetRef.c_str());
        WaraWaraBackground::Config backgroundConfig;
        backgroundConfig.layout = toBackgroundLayout(preset.background.layout);
        backgroundConfig.shapeSet = toBackgroundShapeSet(preset.background.shapeSet);
        backgroundConfig.symmetry = toBackgroundSymmetry(preset.background.symmetry);
        backgroundConfig.shapeCount = preset.background.shapeCount;
        backgroundConfig.gridColumns = preset.background.gridColumns;
        backgroundConfig.gridRows = preset.background.gridRows;
        backgroundConfig.spacingX = preset.background.spacingX;
        backgroundConfig.spacingY = preset.background.spacingY;
        backgroundConfig.sizeMin = preset.background.sizeMin;
        backgroundConfig.sizeMax = preset.background.sizeMax;
        backgroundConfig.speedMin = preset.background.speedMin;
        backgroundConfig.speedMax = preset.background.speedMax;
        backgroundConfig.wobble = preset.background.wobble;
        backgroundConfig.opacity = preset.background.opacity;
        backgroundConfig.rotationSpeed = preset.background.rotationSpeed;
        backgroundConfig.fixedOrientation = preset.background.fixedOrientation;
        backgroundConfig.orientationDegrees = preset.background.orientationDegrees;
        backgroundConfig.cornerRoundness = preset.background.cornerRoundness;
        backgroundConfig.imageOpacity = preset.background.imageOpacity;
        backgroundConfig.imageCover = preset.background.imageCover;
        m_background->setConfig(backgroundConfig);
        m_background->setBlurStrength(m_config.backgroundBlur);
        m_background->setSpeedScale(m_config.backgroundSpeed * 2.f);
        // 0.35 was picked for drifting shapes, which have no speed of their own
        // to be wrong about. A frame sequence does: it was filmed at a rate, and
        // 0.35 plays a ten second loop over twenty-nine. So an animated theme
        // starts at the speed of its clip, until somebody moves the slider --
        // after which their choice is theirs, and applies to any theme.
        m_background->setWallpaperSpeedScale(
            (wantsFrames && !m_config.backgroundSpeedChosen) ? 1.f
                                                            : m_config.backgroundSpeed);
        // Theme config regenerates shapes — re-assert lite suppress if FT/Battery
        // Saver currently holds the light path.
        if (m_frameTapLightActive)
            applyFrameTapLiteBackground(true);

        if (backgroundImageNeedsReload) {
            // A theme that lists frames gets the moving wallpaper; one that
            // names a single image keeps the still, and pays exactly what it
            // paid before. Frames are resolved against the theme's own folder,
            // the same way its single image is.
            bool imageLoaded = false;
            if (!preset.background.imageFrames.empty()) {
                std::vector<std::string> framePaths;
                framePaths.reserve(preset.background.imageFrames.size());
                for (const auto& rel : preset.background.imageFrames) {
                    std::string abs = resolveThemeAssetPath(preset, rel);
                    if (!abs.empty() && pathExists(abs))
                        framePaths.push_back(std::move(abs));
                }
                imageLoaded = !framePaths.empty() &&
                              m_background->loadImageSequence(gpu, ren, framePaths,
                                                              preset.background.imageFps);
                DebugLog::log("[theme-apply] background frames: listed=%zu found=%zu loaded=%d",
                              preset.background.imageFrames.size(),
                              framePaths.size(), imageLoaded ? 1 : 0);
            }
            if (!imageLoaded)
                imageLoaded = imageExists && m_background->loadImage(gpu, ren, backgroundImagePath);
            DebugLog::log("[theme-apply] background image: path=%s exists=%d reloaded=%d loaded=%d",
                          safeLogPath(backgroundImagePath),
                          imageExists ? 1 : 0,
                          1,
                          imageLoaded ? 1 : 0);
            if (!imageLoaded) {
                if (m_backgroundImageLoaded)
                    m_background->clearImage();
                m_backgroundImageLoaded = false;
                m_loadedBackgroundImagePath.clear();
                DebugLog::log("[theme-apply] background image cleared");
            } else {
                m_backgroundImageLoaded = true;
                m_loadedBackgroundImagePath = backgroundImagePath;
            }
        } else {
            DebugLog::log("[theme-apply] background image: path=%s exists=%d reloaded=%d loaded=%d",
                          safeLogPath(backgroundImagePath),
                          imageExists ? 1 : 0,
                          0,
                          m_backgroundImageLoaded ? 1 : 0);
        }
    } else {
        DebugLog::log("[theme-apply] background widget missing");
    }

    if (settingsLayoutNeedsRebuild && m_settings && m_settings->isActive()) {
        m_settings->rebuildCurrentTab();
        DebugLog::log("[theme-apply] settings current tab rebuilt for font change");
    }

    DebugLog::log("[theme-apply] resources done: preset=%s", presetRef.c_str());
}


void WiiUMenuApp::applyHomeChrome() {
    const nxui::Theme& chrome = m_homeChrome;

    if (m_clock) {
        m_clock->setBaseColor(chrome.panelBase);
        m_clock->setBorderColor(chrome.panelBorder);
        m_clock->setHighlightColor(chrome.panelHighlight);
        m_clock->setTextColor(chrome.textPrimary);
        m_clock->setSecondaryTextColor(chrome.textSecondary);
    }

    if (m_battery) {
        m_battery->setBaseColor(chrome.panelBase);
        m_battery->setBorderColor(chrome.panelBorder);
        m_battery->setHighlightColor(chrome.panelHighlight);
        m_battery->setTextColor(chrome.textPrimary);
    }

    if (m_titlePill) {
        m_titlePill->setBaseColor(chrome.panelBase.withAlpha(
            chrome.mode == nxui::ThemeMode::Dark ? 0.90f : 0.86f));
        m_titlePill->setBorderColor(chrome.panelBorder.withAlpha(0.84f));
        m_titlePill->setHighlightColor(chrome.panelHighlight.withAlpha(0.42f));
        m_titlePill->setTextColor(chrome.textPrimary);
    }

    if (m_pageIndicator) {
        m_pageIndicator->setBaseColor(chrome.panelBase);
        m_pageIndicator->setBorderColor(chrome.panelBorder);
        m_pageIndicator->setHighlightColor(chrome.panelHighlight);
        m_pageIndicator->setTheme(&m_homeChrome);
    }
    if (m_folderHeader) {
        m_folderHeader->setBaseColor(chrome.panelBase);
        m_folderHeader->setBorderColor(chrome.panelBorder);
        m_folderHeader->setHighlightColor(chrome.panelHighlight);
    }
    if (m_folderHeaderLabel)
        m_folderHeaderLabel->setTextColor(chrome.textPrimary);
    if (m_layoutFlip)
        m_layoutFlip->setTheme(&m_homeChrome);

    m_sidebar.applyTheme(m_homeChrome);
    syncColourIconContrast();
}

void WiiUMenuApp::updateHeroAwareChrome(bool force) {
    HeroChromeAdapt desired = HeroChromeAdapt::Off;
    // Full-opacity heroes are single-row only; dimmed grid heroes leave chrome alone.
    const bool fullHeroContext =
        presentationLayoutMode() == AppLayoutMode::DynamicLine;

    if (fullHeroContext && m_steamGridDbBackdrop
        && m_steamGridDbBackdrop->hasDisplayedHero()) {
        const float luma = m_steamGridDbBackdrop->displayedHeroLuminance();
        if (luma >= 0.f) {
            // Hysteresis avoids flicker when focus skims near-threshold heroes.
            const float darkEnter = 0.42f;
            const float darkExit = 0.50f;
            const float lightEnter = 0.58f;
            const float lightExit = 0.50f;
            if (m_theme.mode == nxui::ThemeMode::Light) {
                const float limit = (m_heroChromeAdapt == HeroChromeAdapt::ForceDark)
                    ? darkExit : darkEnter;
                if (luma < limit)
                    desired = HeroChromeAdapt::ForceDark;
            } else if (m_theme.mode == nxui::ThemeMode::Dark) {
                const float limit = (m_heroChromeAdapt == HeroChromeAdapt::ForceLight)
                    ? lightExit : lightEnter;
                if (luma > limit)
                    desired = HeroChromeAdapt::ForceLight;
            }
        }
    }

    // Colour-icon contrast must track every hero change even when chrome mode
    // stays put (otherwise adapt never releases after the first hero).
    if (!force && desired == m_heroChromeAdapt) {
        syncColourIconContrast();
        return;
    }

    m_heroChromeAdapt = desired;
    if (desired == HeroChromeAdapt::ForceDark) {
        m_homeChrome = nxui::Theme::dark();
    } else if (desired == HeroChromeAdapt::ForceLight) {
        m_homeChrome = nxui::Theme::light();
    } else {
        m_homeChrome = m_theme;
    }
    // Keep the user's cursor brand while chrome contrast flips.
    m_homeChrome.cursorNormal = m_theme.cursorNormal;
    m_homeChrome.cursorGlow = m_theme.cursorGlow;
    m_homeChrome.cursorCornerRadius = m_theme.cursorCornerRadius;
    m_homeChrome.cursorBorderWidth = m_theme.cursorBorderWidth;
    applyHomeChrome();
}

void WiiUMenuApp::syncHeroBackdropPresentation() {
    if (!m_steamGridDbBackdrop)
        return;
    m_steamGridDbBackdrop->setLayoutMode(presentationLayoutMode());
    updateHeroAwareChrome(true);
}

void WiiUMenuApp::applyTheme() {
    DebugLog::log("[theme-apply] widget recolor start");
    m_homeChrome = m_theme;
    m_heroChromeAdapt = HeroChromeAdapt::Off;
    m_background->setAccentColor(m_theme.backgroundAccent);
    m_background->setSecondaryColor(m_theme.background);
    m_background->setShapeColor(m_theme.shapeColor);
    DebugLog::log("[theme-apply] widget recolor background done");

    for (auto& icon : m_grid->allIcons()) {
        icon->setBaseColor(m_theme.iconDefault);
        icon->setBorderColor(m_theme.panelBorder);
        icon->setHighlightColor(m_theme.panelHighlight);
        icon->setCornerRadius(m_theme.iconCornerRadius);
        icon->setLoadingColor(m_theme.cursorNormal);
        icon->setThemeMode(m_theme.mode);
    }
    DebugLog::log("[theme-apply] widget recolor grid icons done");

    m_cursor->setColor(m_theme.cursorNormal);
    m_cursor->setCornerRadius(m_theme.cursorCornerRadius);
    m_cursor->setBorderWidth(m_theme.cursorBorderWidth);
    if (m_pointerCursor) {
        m_pointerCursor->setColor(m_theme.cursorNormal);
        m_pointerCursor->setCornerRadius(15.f);
        m_pointerCursor->setBorderWidth(2.5f);
    }
    DebugLog::log("[theme-apply] widget recolor cursors done");

    // Clock, battery, title pill and page indicator follow the hero-aware HOME
    // chrome (PoloNX); the fork's avatar buttons keep the theme panel colours.
    applyHomeChrome();
    for (auto& avatar : m_userAvatarButtons) {
        avatar->setBaseColor(m_theme.panelBase);
        avatar->setBorderColor(m_theme.panelBorder);
        avatar->setHighlightColor(m_theme.panelHighlight);
        avatar->setCornerRadius(28.f);
        avatar->setChromeEnabled(false);
    }
    DebugLog::log("[theme-apply] widget recolor HUD done");

    m_userSelect->setTheme(&m_theme);
    m_userSelect->cursor().setColor(m_theme.cursorNormal);

    if (m_dialog) {
        m_dialog->setTheme(&m_theme);
        m_dialog->setBaseColor(m_theme.panelBase);
        m_dialog->setBorderColor(m_theme.panelBorder);
        m_dialog->setHighlightColor(m_theme.panelHighlight);
        m_dialog->cursor().setColor(m_theme.cursorNormal);
    }
    DebugLog::log("[theme-apply] widget recolor overlays done");

    if (m_settings)
        m_settings->setTheme(&m_theme);
    if (m_themeShop)
        m_themeShop->setTheme(&m_theme);
    if (m_gameGallery)
        m_gameGallery->setTheme(&m_theme);
    if (m_gameMods)
        m_gameMods->setTheme(&m_theme);
    if (m_gameCheats)
        m_gameCheats->setTheme(&m_theme);
    if (m_gameDetails)
        m_gameDetails->setTheme(&m_theme);
    // The 1.2 overlays were left out of the recolor pass and kept the previous
    // theme's colours until the menu process was recreated.
    if (m_contextMenu)
        m_contextMenu->setTheme(&m_theme);
    if (m_gameOptions)
        m_gameOptions->setTheme(&m_theme);
    if (m_folderOptions)
        m_folderOptions->setTheme(&m_theme);
    if (m_controllerTest)
        m_controllerTest->setTheme(&m_theme);
    if (m_steamGridDbPicker)
        m_steamGridDbPicker->setTheme(&m_theme);
    if (m_platformPicker)
        m_platformPicker->setTheme(&m_theme);

    // Re-evaluate hero contrast after a theme change (may re-apply chrome).
    updateHeroAwareChrome(true);
    DebugLog::log("[theme-apply] widget recolor complete");
}

void WiiUMenuApp::rebuildThemeFromColors() {
    DebugLog::log("[theme-apply] rebuild start: activePreset=%s", m_activePresetName.c_str());
    m_effectivePreset = buildEffectiveThemePreset();
    DebugLog::log("[theme-apply] effective preset resolved: id=%s name=%s source=%s installPath=%s",
                  safeLogPath(m_effectivePreset.id),
                  m_effectivePreset.name.c_str(),
                  themeSourceName(m_effectivePreset.source),
                  safeLogPath(m_effectivePreset.installPath));
    m_theme = m_effectivePreset.toTheme();
    DebugLog::log("[theme-apply] theme object rebuilt");

    DebugLog::log("[theme-apply] applyThemeResources start: preset=%s",
                  safeLogPath(m_effectivePreset.id.empty() ? m_effectivePreset.name : m_effectivePreset.id));
    applyThemeResources(m_effectivePreset);
    DebugLog::log("[theme-apply] applyThemeResources done: preset=%s",
                  safeLogPath(m_effectivePreset.id.empty() ? m_effectivePreset.name : m_effectivePreset.id));

    DebugLog::log("[theme-apply] sidebar.reloadAssets start: pack=%s",
                  m_config.iconPack.empty() ? switchu::icons::kDefaultPackId
                                            : m_config.iconPack.c_str());
    reloadSidebarIcons();
    DebugLog::log("[theme-apply] sidebar.reloadAssets done");

    DebugLog::log("[theme-apply] applyTheme start");
    applyTheme();
    m_forceThemeResourceReload = false;
    DebugLog::log("[theme-apply] rebuild complete: activePreset=%s", m_activePresetName.c_str());
}

ThemePreset* WiiUMenuApp::findPresetPtr(const std::string& name) {
    for (auto& p : m_allPresets)
        if (p.id == name || p.name == name) return &p;
    return nullptr;
}

void WiiUMenuApp::deletePreset(const std::string& presetId) {
    ThemePreset* preset = findPresetPtr(presetId);
    if (!preset) return;

    ThemePreset* activePreset = findPresetPtr(m_activePresetName);
    std::string activeId = activePreset ? (activePreset->id.empty() ? activePreset->name : activePreset->id)
                                        : m_activePresetName;
    std::string idToDelete = preset->id.empty() ? preset->name : preset->id;
    std::string nameToDelete = preset->name;
    std::string installPath = preset->installPath;
    std::string soundPreset = preset->soundPreset;
    ThemePresetSource source = preset->source;
    bool deletingActive = (activeId == idToDelete);

    m_allPresets.erase(
        std::remove_if(m_allPresets.begin(), m_allPresets.end(),
            [&](const ThemePreset& p) { return p.id == idToDelete || p.name == nameToDelete; }),
        m_allPresets.end());

    auto userPresets = ThemePreset::loadUserPresets();
    userPresets.erase(
        std::remove_if(userPresets.begin(), userPresets.end(),
            [&](const ThemePreset& p) { return p.id == idToDelete || p.name == nameToDelete; }),
        userPresets.end());
    ThemePreset::saveUserPresets(userPresets);

    if (source == ThemePresetSource::InstalledPackage && !installPath.empty()) {
        // The result is checked now. It was discarded before, which is why a
        // failure to delete looked exactly like a success from the player's
        // side: the theme left the list either way.
        std::string failedPath;
        if (removeDirectoryRecursive(installPath, &failedPath)) {
            DebugLog::log("[theme] removed %s", installPath.c_str());
            // The deletion has to reach the card, not just the cache: a reboot
            // before that and the folder is back with the theme gone from the
            // list, which is the same symptom by another route.
            switchu::commitSdCard("theme removed");
        } else {
            DebugLog::log("[theme] could not remove %s (stopped at %s)",
                          installPath.c_str(), failedPath.c_str());
        }
    }

    if (!soundPreset.empty() && m_config.soundPreset == soundPreset) {
        m_config.soundPreset = "wiiu";
        changeSoundPreset(m_config.soundPreset);
    }

    {
        std::string folder = idToDelete;
        if (!installPath.empty()) {
            const auto pos = installPath.find_last_of("/\\");
            folder = (pos == std::string::npos) ? installPath : installPath.substr(pos + 1);
        }
        // package:slug ids store the folder under installPath; strip prefix for pack ids.
        if (folder.rfind("package:", 0) == 0)
            folder = folder.substr(8);
        const std::string themePackId = switchu::icons::makeThemePackId(folder);
        const std::string legacyPackId = switchu::icons::makeThemePackId(idToDelete);
        std::string companionPackId;
        if (const ThemePreset* dying = findPresetPtr(idToDelete))
            companionPackId = dying->icons.packId;
        // Snapshot before preset list changes: folder-named extract is orphaned
        // when the theme goes away (unless it is a builtin like atmos).
        if (!folder.empty() && !isBuiltinIconPack(folder)) {
            const std::string orphan =
                switchu::icons::IconPackInstaller::destinationRootFor(folder);
            if (pathExists(orphan)) {
                switchu::icons::IconPackInstaller::remove(folder);
                DebugLog::log("[icon-pack] removed theme orphan pack=%s", folder.c_str());
            }
        }
        if ((!themePackId.empty() && m_config.iconPack == themePackId)
            || (!legacyPackId.empty() && m_config.iconPack == legacyPackId)
            || (!folder.empty() && m_config.iconPack == folder)
            || (switchu::icons::isThemePackId(m_config.iconPack)
                && switchu::icons::themeFolderFromPackId(m_config.iconPack) == folder)) {
            m_config.iconPack = switchu::icons::kDefaultPackId;
            m_config.save();
            DebugLog::log("[icon-pack] theme removed; pack reset to default");
        }
        (void)companionPackId; // builtins referenced via packId are kept
    }

    // Leave the package BEFORE deleting files — otherwise open fonts/textures can
    // lock the folder, remove_all fails, and Community still thinks it's installed.
    if (deletingActive) {
        m_activePresetName = defaultThemeRef();
        m_config.themePreset = defaultThemeRef();

        ThemePreset* fallback = findPresetPtr(defaultThemeRef());
        if (!fallback)
            fallback = findPresetPtr("Default Light");
        if (fallback) {
            m_activePresetName = fallback->id.empty() ? fallback->name : fallback->id;
            m_config.themePreset = m_activePresetName;
            m_activeColors = fallback->colors;
            m_activeMode = fallback->mode;
            m_forceThemeResourceReload = true;
        }
        rebuildThemeFromColors();
    }

    if (source == ThemePresetSource::InstalledPackage && !installPath.empty()) {
        if (m_themeDeleteFuture.valid()) {
            m_themeDeleteFuture.wait();
            m_themeDeleteFuture = {};
        }
        // Sync remove so Installed → Community reinstall does not race an orphan folder.
        const bool removed = removeDirectoryRecursive(installPath);
        DebugLog::log("[theme-delete] removal %s: %s",
                      removed ? "complete" : "failed", safeLogPath(installPath));
        if (!removed || pathExists(installPath)) {
            // One more pass after resources have switched away.
            const bool retry = removeDirectoryRecursive(installPath);
            DebugLog::log("[theme-delete] retry %s: %s",
                          retry ? "complete" : "failed", safeLogPath(installPath));
        }
    }

    refreshThemeShopState();
    // Removing from the product sheet used to leave detail open on the next
    // theme (selection falls through after the list rebuild). Return to shelf.
    if (m_themeShop && m_themeShop->detailOpen())
        m_themeShop->closeDetailView();
    m_audio.playSfx(Sfx::ConfirmPositive);
}

void WiiUMenuApp::rematchIconPackTintsToTheme(bool syncSidebar) {
    float accentH = m_activeColors.accentH;
    float accentS = m_activeColors.accentS;
    float accentL = m_activeColors.accentL;
    if (accentS < 0.05f && accentL < 0.08f
        && (m_activeColors.cursorS >= 0.05f || m_activeColors.cursorL >= 0.08f)) {
        accentH = m_activeColors.cursorH;
        accentS = std::max(0.f, m_activeColors.cursorS);
        accentL = std::max(0.f, m_activeColors.cursorL);
    }
    if (m_themeShop)
        m_themeShop->setThemeMatchAccent(accentH, accentS, accentL);

    // Rematch every pack that already has Match on. Do NOT move/clear Match
    // when a theme switches the active icon pack (e.g. iiSU) - Atmos keeps Auto.
    bool any = false;
    for (auto& [packId, tint] : m_config.iconPackTints) {
        if (!tint.matchTheme)
            continue;
        float meanH = 0.f, meanS = 0.45f, meanL = 0.55f;
        const std::string sample =
            switchu::icons::resolveGlyphPath(SD_ASSETS, packId, "album.png");
        if (!sample.empty())
            switchu::icons::probeIconMeanHsl(sample, meanH, meanS, meanL);
        const auto pivots = switchu::icons::computeMatchThemePivots(
            meanH, meanS, meanL, accentH, accentS, accentL);
        tint.enabled = true;
        tint.grayscale = false;
        tint.matchTheme = true;
        tint.hue = pivots.hue;
        tint.sat = pivots.sat;
        tint.light = pivots.light;
        any = true;
    }
    if (!any)
        return;

    m_config.save();
    if (syncSidebar)
        syncColourIconContrast();
    // Push rematched tints into Theme Shop before invalidating so hslKeys match.
    if (m_themeShop) {
        std::unordered_map<std::string, ThemeShopScreen::IconPackTintState> tints;
        for (const auto& [packId, tint] : m_config.iconPackTints) {
            ThemeShopScreen::IconPackTintState state;
            state.enabled = tint.enabled;
            state.flatten = tint.flatten;
            state.grayscale = tint.grayscale;
            state.matchTheme = tint.matchTheme;
            state.hue = tint.hue;
            state.sat = tint.sat;
            state.light = tint.light;
            tints[packId] = state;
        }
        m_themeShop->setIconPackTintState(tints);
        m_themeShop->setThemeMatchAccent(accentH, accentS, accentL);
        m_themeShop->invalidateIconPackPreviews();
    }
}

void WiiUMenuApp::syncColourIconContrast() {
    // Single-row + hero: colour icons follow HOME chrome (same light/dark flip
    // as mono glyphs). No hero / grid: authored colour, optionally HSL-shifted
    // when custom tint is on. Theme > Options arms the feature.
    const bool fullHero =
        presentationLayoutMode() == AppLayoutMode::DynamicLine
        && m_steamGridDbBackdrop
        && m_steamGridDbBackdrop->hasDisplayedHero();
    const bool adaptColour = m_config.iconContrastAdapt && fullHero;

    const std::string activePackId = resolvedIconPackId();
    const AppConfig::IconPackTint packTint = m_config.iconPackTintFor(activePackId);
    switchu::icons::HslAdjust hsl;
    hsl.enabled = packTint.enabled;
    hsl.flatten = packTint.flatten;
    hsl.grayscale = packTint.grayscale;
    hsl.hue = std::clamp(packTint.hue, 0.f, 1.f);
    hsl.sat = std::clamp(packTint.sat, 0.f, 1.f);
    hsl.light = std::clamp(packTint.light, 0.f, 1.f);

    const auto& prev = m_sidebar.iconHslAdjust();
    const bool hslChanged =
        prev.enabled != hsl.enabled
        || prev.flatten != hsl.flatten
        || prev.grayscale != hsl.grayscale
        || std::fabs(prev.hue - hsl.hue) > 0.0005f
        || std::fabs(prev.sat - hsl.sat) > 0.0005f
        || std::fabs(prev.light - hsl.light) > 0.0005f;

    m_sidebar.setIconHslAdjust(hsl);
    if (m_layoutFlip)
        m_layoutFlip->setIconHslAdjust(hsl);
    m_sidebar.setColourIconContrast(adaptColour);
    if (m_layoutFlip)
        m_layoutFlip->setColourIconContrast(adaptColour);

    if (hslChanged) {
        // Rebake colour glyphs (and Change View) with the new HSL adjust.
        // Do not log the unchanged path — syncColourIconContrast runs every
        // frame from updateHeroAwareChrome; SD log spam soft-locks the console.
        reloadSidebarIcons();
        m_sidebar.setColourIconContrast(adaptColour);
        if (m_layoutFlip)
            m_layoutFlip->setColourIconContrast(adaptColour);
    }
}

// ---------------------------------------------------------------------------
// PoloNX #167 / #172 SteamGridDB helpers that the fork had no counterpart for.
// The fork's own startSteamGridDbScrape / openSteamGridDbPicker /
// editSteamGridDbPickerQuery / applySteamGridDbCandidate / syncSteamGridDb /
// showFocusedSteamGridDbArtwork live in WiiUMenuAppUpstreamSettings.cpp and
// keep the fork's rules (gallery service without a key, image-pin pickers).

void WiiUMenuApp::raiseProgressDialogAboveOverlays() {
    if (!m_overlayLayer || !m_progressDialog)
        return;
    // Picker show() re-parents itself to the top of the overlay stack, which
    // buries the download dialog. Keep progress on top while it is active.
    m_overlayLayer->removeChild(m_progressDialog.get());
    m_overlayLayer->addChild(m_progressDialog);
}

void WiiUMenuApp::cancelSteamGridDbApply() {
    if (!m_steamGridDbApplyProgress)
        return;
    m_steamGridDbApplyProgress->cancelRequested.store(true);
    {
        std::lock_guard<std::mutex> lock(m_steamGridDbApplyProgress->mutex);
        m_steamGridDbApplyProgress->message = nxui::I18n::instance().tr(
            "settings.steamgriddb.cancelling", "Cancelling...");
        m_steamGridDbApplyProgress->progress01 = -1.f;
        ++m_steamGridDbApplyProgress->revision;
    }
    if (m_progressDialog)
        m_progressDialog->updateState(nxui::I18n::instance().tr(
            "settings.steamgriddb.cancelling", "Cancelling..."), -1.f);
}

// Folder Options (custom icon / poster) and any caller that is not the open
// Game Options panel. Same browse path as openSteamGridDbPicker, with the
// fork's rules: no key needed except for logos (the fork's gallery service
// serves the rest), and an in-flight browse is refused rather than parked --
// the fork's syncSteamGridDb does not drain parked browses.
void WiiUMenuApp::openSteamGridDbPickerForTitle(std::uint64_t titleId,
                                                const std::string& title,
                                                GameOptionsScreen::ArtworkKind kind,
                                                const std::string& requestedQuery) {
    if (titleId == 0 || !m_steamGridDbPicker) return;
    auto toast = [&](const std::string& msg) {
        if (m_gameOptions && m_gameOptions->isActive()) m_gameOptions->requestToast(msg);
        else if (m_folderOptions && m_folderOptions->isActive()) m_folderOptions->requestToast(msg);
        else if (m_settings) m_settings->requestToast(msg);
    };
    if (kind == GameOptionsScreen::ArtworkKind::Logo && m_config.steamGridDbApiKey.empty()) {
        toast(nxui::I18n::instance().tr(
            "settings.steamgriddb.need_key_logo",
            "Logos need a SteamGridDB API key. Configure one in Settings."));
        return;
    }
    SteamGridDbManager::ArtworkKind managerKind = SteamGridDbManager::ArtworkKind::Hero;
    if (kind == GameOptionsScreen::ArtworkKind::Logo)
        managerKind = SteamGridDbManager::ArtworkKind::Logo;
    else if (kind == GameOptionsScreen::ArtworkKind::Icon)
        managerKind = SteamGridDbManager::ArtworkKind::Icon;
    else if (kind == GameOptionsScreen::ArtworkKind::Poster) // == Cover
        managerKind = SteamGridDbManager::ArtworkKind::Poster;
    else if (kind == GameOptionsScreen::ArtworkKind::PosterWide)
        managerKind = SteamGridDbManager::ArtworkKind::GridWide;

    if (m_steamGridDbBrowseFuture.valid() || m_steamGridDbApplyFuture.valid()
        || m_steamGridDb.running()) {
        toast(nxui::I18n::instance().tr(
            "settings.steamgriddb.already_running", "A SteamGridDB operation is already running."));
        return;
    }
    std::string query = requestedQuery;
    if (query.empty())
        query = SteamGridDbManager::savedSearchQuery(titleId);
    if (query.empty())
        query = title;
    if (query.empty()) {
        toast(nxui::I18n::instance().tr(
            "settings.steamgriddb.need_query", "Enter a search name first."));
        return;
    }
    SteamGridDbManager::saveSearchQuery(titleId, query);
    m_gameOptionsTitleId = titleId;
    if (m_overlayLayer) {
        m_overlayLayer->removeChild(m_steamGridDbPicker.get());
        m_overlayLayer->addChild(m_steamGridDbPicker);
    }
    m_steamGridDbPicker->showLoading(titleId, title, query, managerKind);
    focusManager().setFocus(m_steamGridDbPicker.get());
    m_steamGridDbBrowseFuture = std::async(std::launch::async,
        [apiKey = m_config.steamGridDbApiKey, titleId, title, query, managerKind]() {
            return SteamGridDbManager::browse(apiKey, titleId, title, query, managerKind);
        });
}

void WiiUMenuApp::clearSteamGridDbArtwork(std::uint64_t titleId,
                                          GameOptionsScreen::ArtworkKind kind) {
    if (titleId == 0) return;
    SteamGridDbManager::ArtworkKind managerKind = SteamGridDbManager::ArtworkKind::Hero;
    if (kind == GameOptionsScreen::ArtworkKind::Logo)
        managerKind = SteamGridDbManager::ArtworkKind::Logo;
    else if (kind == GameOptionsScreen::ArtworkKind::Icon)
        managerKind = SteamGridDbManager::ArtworkKind::Icon;
    else if (kind == GameOptionsScreen::ArtworkKind::Poster) // == Cover
        managerKind = SteamGridDbManager::ArtworkKind::Poster;
    else if (kind == GameOptionsScreen::ArtworkKind::PosterWide)
        managerKind = SteamGridDbManager::ArtworkKind::GridWide;

    const bool removed = SteamGridDbManager::clearArtwork(titleId, managerKind);
    auto& i18n = nxui::I18n::instance();
    const std::string msg = removed
        ? i18n.tr("game.steamgriddb.cleared", "SteamGridDB artwork cleared.")
        : i18n.tr("game.steamgriddb.nothing_to_clear", "No SteamGridDB artwork to clear.");

    if ((managerKind == SteamGridDbManager::ArtworkKind::Icon
         || managerKind == SteamGridDbManager::ArtworkKind::Poster) && m_grid) {
        m_folderCoverCache.erase(titleId);
        m_folderPeekCache.erase(titleId);
        m_folderPeekBorrowed.erase(titleId);
        if (titleId >= kFolderTitleIdPrefix)
            invalidateFolderPeekComposite(static_cast<std::uint32_t>(
                titleId & 0xFFFFFFFFu));
        else
            invalidateFolderPeekCompositesForTitle(titleId);
        const bool preferPoster = preferSingleRowPosters();
        const bool isFolderArt = titleId >= kFolderTitleIdPrefix;
        if (managerKind == SteamGridDbManager::ArtworkKind::Poster) {
            for (const auto& icon : m_grid->allIcons()) {
                if (!icon || icon->titleId() != titleId) continue;
                // Folders stay tall in single-row poster mode even without art.
                if (icon->entryKind() == GridEntryKind::Folder)
                    icon->setPosterFrame(preferPoster);
                else
                    icon->setPosterFrame(false);
            }
            m_grid->invalidateLineLayout();
        }
        if (!isFolderArt) {
            m_iconStreamer.reloadTitle(titleId, m_grid->currentPage(),
                                       m_grid->iconsPerPage(),
                                       app().gpu(), app().renderer(), m_grid->allIcons());
        }
        applyFolderCoversToIcons();
        if (m_gameOptions && m_gameOptions->isActive()
            && titleId < kFolderTitleIdPrefix)
            m_gameOptions->setGameIcon(folderCoverTexture(titleId));
        else if (m_gameOptions)
            m_gameOptions->setGameIcon(nullptr);
    }
    if (managerKind == SteamGridDbManager::ArtworkKind::GridWide
        || managerKind == SteamGridDbManager::ArtworkKind::Hero
        || managerKind == SteamGridDbManager::ArtworkKind::Logo) {
        m_gameArtwork.erase(titleId);
        const auto size = gameGridSize(titleId, AppLayoutMode::Grid);
        if (switchu::widgets::isLandscapeWideGameTile(size))
            ensureGameArtwork(titleId);
    }
    showFocusedSteamGridDbArtwork(true);
    m_audio.playSfx(removed ? Sfx::ConfirmPositive : Sfx::ModalHide);
    if (m_gameOptions && m_gameOptions->isActive()) {
        std::error_code ec;
        const bool hasHero = std::filesystem::exists(
            SteamGridDbManager::heroPath(titleId), ec);
        ec.clear();
        const bool hasLogo = std::filesystem::exists(
            SteamGridDbManager::logoPath(titleId), ec);
        m_gameOptions->refreshArtworkPresence(
            hasHero, hasLogo, SteamGridDbManager::hasIcon(titleId),
            SteamGridDbManager::hasPoster(titleId),
            SteamGridDbManager::hasGridWide(titleId));
        m_gameOptions->requestToast(msg, 2.6f);
    } else if (m_folderOptions && m_folderOptions->isActive())
        m_folderOptions->requestToast(msg, 2.6f);
}