#include "TabBuilders.hpp"

#include <nxui/core/I18n.hpp>

#include <algorithm>

SettingsScreen::Tab settings::tabs::SteamGridDbTab::build(SettingsScreen& screen) {
    using ItemType = SettingsScreen::ItemType;
    using SettingItem = SettingsScreen::SettingItem;
    auto& i18n = nxui::I18n::instance();

    SettingsScreen::Tab tab;
    tab.name = i18n.tr("settings.tabs.steamgriddb", "SteamGridDB");

    SettingItem section;
    section.type = ItemType::Section;
    section.label = i18n.tr("settings.steamgriddb.section", "Game artwork");
    tab.items.push_back(std::move(section));

    SettingItem enabled;
    enabled.type = ItemType::Toggle;
    enabled.label = i18n.tr("settings.steamgriddb.enabled", "Display SteamGridDB artwork");
    enabled.description = i18n.tr("settings.steamgriddb.enabled_desc",
        "Show heroes and logos behind the application menu.");
    enabled.boolVal = screen.m_steamGridDbEnabled;
    enabled.anim01 = enabled.boolVal ? 1.f : 0.f;
    enabled.onChange = [&screen](SettingItem& item) {
        screen.m_steamGridDbEnabled = item.boolVal;
        if (screen.m_steamGridDbEnabledCb)
            screen.m_steamGridDbEnabledCb(item.boolVal);
    };
    tab.items.push_back(std::move(enabled));

    SettingItem apiKey;
    apiKey.type = ItemType::Action;
    apiKey.label = i18n.tr("settings.steamgriddb.api_key", "API key");
    apiKey.buttonLabel = i18n.tr("button.configure", "Configure");
    apiKey.description = screen.m_steamGridDbHasApiKey
        ? i18n.tr("settings.steamgriddb.api_key_set", "Configured (hidden)")
        : i18n.tr("settings.steamgriddb.api_key_optional",
                  "Optional. Heroes and icons work without it; logos need one.");
    apiKey.onChange = [&screen](SettingItem&) {
        if (screen.m_steamGridDbApiKeyCb) screen.m_steamGridDbApiKeyCb();
    };
    tab.items.push_back(std::move(apiKey));

    SettingItem scan;
    scan.type = ItemType::Action;
    scan.label = i18n.tr("settings.steamgriddb.scan", "Search artwork for missing assets");
    scan.buttonLabel = i18n.tr("button.search", "Search");
    scan.description = i18n.tr("settings.steamgriddb.scan_desc",
        "Downloads a hero and logo only for applications that do not already have artwork.");
    scan.onChange = [&screen](SettingItem&) {
        if (screen.m_steamGridDbRunning) {
            screen.requestToast(nxui::I18n::instance().tr(
                "settings.steamgriddb.already_running", "A scan is already running."));
            return;
        }
        if (screen.m_steamGridDbScrapeCb) screen.m_steamGridDbScrapeCb();
    };
    tab.items.push_back(std::move(scan));

    // The dossier's metascore and time to beat come from services that need a
    // key of the player's own. Kept next to the SteamGridDB key, the other one.
    SettingItem detailsSection;
    detailsSection.type = ItemType::Section;
    detailsSection.label = i18n.tr("settings.metadata.section", "Game details");
    tab.items.push_back(std::move(detailsSection));

    struct KeyField { const char* labelKey; const char* label; const char* hintKey; const char* hint; };
    static constexpr KeyField kKeyFields[] = {
        {"settings.metadata.rawg_key", "RAWG API key",
         "settings.metadata.rawg_hint", "Optional. Adds the metascore. Free at rawg.io/apidocs."},
        {"settings.metadata.igdb_id", "IGDB Client ID",
         "settings.metadata.igdb_hint", "Optional. Adds time to beat. Create an application at dev.twitch.tv."},
        {"settings.metadata.igdb_secret", "IGDB Client Secret",
         "settings.metadata.igdb_hint", "Optional. Adds time to beat. Create an application at dev.twitch.tv."},
    };
    const auto keyDescription = [](const SettingsScreen& owner, int index) {
        auto& tr = nxui::I18n::instance();
        return owner.m_metadataHasKey[index]
            ? tr.tr("settings.steamgriddb.api_key_set", "Configured (hidden)")
            : tr.tr(kKeyFields[index].hintKey, kKeyFields[index].hint);
    };
    for (int index = 0; index < 3; ++index) {
        SettingItem field;
        field.type = ItemType::Action;
        field.label = i18n.tr(kKeyFields[index].labelKey, kKeyFields[index].label);
        field.buttonLabel = i18n.tr("button.configure", "Configure");
        field.description = keyDescription(screen, index);
        field.onChange = [&screen, index](SettingItem&) {
            if (screen.m_metadataKeyCb) screen.m_metadataKeyCb(index);
        };
        tab.items.push_back(std::move(field));
    }

    tab.onUpdate = [keyDescription](SettingsScreen::Tab& current, TabbedOverlayScreen& base) {
        auto& owner = static_cast<SettingsScreen&>(base);
        if (current.items.size() < 4) return;

        auto& key = current.items[2];
        key.description = owner.m_steamGridDbHasApiKey
            ? nxui::I18n::instance().tr("settings.steamgriddb.api_key_set", "Configured (hidden)")
            : nxui::I18n::instance().tr("settings.steamgriddb.api_key_optional",
                  "Optional. Heroes and icons work without it; logos need one.");
        if (current.items.size() < 8) return;
        for (int index = 0; index < 3; ++index)
            current.items[static_cast<std::size_t>(5 + index)].description =
                keyDescription(owner, index);
    };

    return tab;
}
