#include "ThemeShopTabBuilders.hpp"

#include <nxui/core/I18n.hpp>
#include <algorithm>
#include <cmath>

// PoloNX split the old Options tab into Sounds (music / SFX / theme music) and
// Interface. This is the Interface half, merged with everything the fork's
// Options tab carried: the appearance sliders, the 9-view launcher selector,
// the fork's grid ranges, Dynamic Pages and the grid reload actions. PoloNX's
// Cursor Movement / Page Transition selectors are not here: the fork never
// carried those modes (its grid has its own morph and paging).
ThemeShopScreen::Tab themeshop::tabs::InterfaceTab::build(ThemeShopScreen& screen) {
    using Tab = ThemeShopScreen::Tab;
    using SettingItem = ThemeShopScreen::SettingItem;
    using ItemType = ThemeShopScreen::ItemType;
    auto& i18n = nxui::I18n::instance();

    Tab t;
    t.name = i18n.tr("themeshop.tabs.interface", "Interface");

    // Must stay item 0: onUpdate below refreshes it in place.
    {
        SettingItem it;
        const bool ft = screen.m_frameTapLiteEngaged;
        it.label = ft
            ? i18n.tr("themeshop.options.frame_tap_engaged", "Frame Tap Engaged")
            : i18n.tr("themeshop.options.battery_saver", "Battery Saver");
        it.description = ft
            ? i18n.tr("themeshop.options.frame_tap_engaged_desc",
                      "Lite mode is held while Frame Tap is streaming. "
                      "Disconnect the viewer to change Battery Saver.")
            : i18n.tr("themeshop.options.battery_saver_desc",
                      "Lighten HOME memory and GPU use. Also available in Quick Settings.");
        it.type = ItemType::Toggle;
        it.boolVal = screen.m_batterySaver || ft;
        it.anim01 = it.boolVal ? 1.f : 0.f;
        it.onChange = [&screen](SettingItem& self) {
            if (screen.m_frameTapLiteEngaged) {
                // Held on while Frame Tap streams. The fork's SettingItem has no
                // disabled state, so the toggle snaps back instead.
                self.boolVal = true;
                self.anim01 = 1.f;
                return;
            }
            screen.m_batterySaver = self.boolVal;
            if (screen.m_batterySaverCb) screen.m_batterySaverCb(self.boolVal);
        };
        t.items.push_back(std::move(it));
    }

    t.onUpdate = [&screen](Tab& tab, TabbedOverlayScreen&) {
        if (tab.items.empty())
            return;
        auto& it = tab.items[0];
        auto& i18n = nxui::I18n::instance();
        const bool ft = screen.m_frameTapLiteEngaged;
        const bool wantOn = screen.m_batterySaver || ft;
        it.boolVal = wantOn;
        it.anim01 = wantOn ? 1.f : 0.f;
        it.label = ft
            ? i18n.tr("themeshop.options.frame_tap_engaged", "Frame Tap Engaged")
            : i18n.tr("themeshop.options.battery_saver", "Battery Saver");
        it.description = ft
            ? i18n.tr("themeshop.options.frame_tap_engaged_desc",
                      "Lite mode is held while Frame Tap is streaming. "
                      "Disconnect the viewer to change Battery Saver.")
            : i18n.tr("themeshop.options.battery_saver_desc",
                      "Lighten HOME memory and GPU use. Also available in Quick Settings.");
    };

    {
        SettingItem it;
        it.label = i18n.tr("settings.display.glass_sharpness", "Glass sharpness");
        it.description = i18n.tr("settings.display.glass_sharpness_desc",
                                 "How clearly the screen behind menus shows through them.");
        it.type = ItemType::Slider;
        it.floatVal = std::clamp(screen.m_glassSharpness, 0.f, 1.f);
        it.anim01 = it.floatVal;
        it.onChange = [&screen](SettingItem& self) {
            self.anim01 = self.floatVal;
            screen.m_glassSharpness = std::clamp(self.floatVal, 0.f, 1.f);
            if (screen.m_glassSharpnessCb) screen.m_glassSharpnessCb(screen.m_glassSharpness);
        };
        t.items.push_back(std::move(it));
    }

    {
        SettingItem it;
        it.label = i18n.tr("settings.display.background_speed", "Background animation speed");
        it.description = i18n.tr("settings.display.background_speed_desc",
                                 "How fast the shapes drift. Centre is the theme's own pace.");
        it.type = ItemType::Slider;
        it.floatVal = std::clamp(screen.m_backgroundSpeed, 0.f, 1.f);
        it.anim01 = it.floatVal;
        it.onChange = [&screen](SettingItem& self) {
            self.anim01 = self.floatVal;
            screen.m_backgroundSpeed = std::clamp(self.floatVal, 0.f, 1.f);
            if (screen.m_backgroundSpeedCb) screen.m_backgroundSpeedCb(screen.m_backgroundSpeed);
        };
        t.items.push_back(std::move(it));
    }

    {
        SettingItem it;
        it.label = i18n.tr("settings.display.background_blur", "Background blur");
        it.description = i18n.tr("settings.display.background_blur_desc",
                                 "Soften the wallpaper and the shapes drifting over it.");
        it.type = ItemType::Slider;
        it.floatVal = std::clamp(screen.m_backgroundBlur, 0.f, 1.f);
        it.anim01 = it.floatVal;
        it.onChange = [&screen](SettingItem& self) {
            self.anim01 = self.floatVal;
            screen.m_backgroundBlur = std::clamp(self.floatVal, 0.f, 1.f);
            if (screen.m_backgroundBlurCb) screen.m_backgroundBlurCb(screen.m_backgroundBlur);
        };
        t.items.push_back(std::move(it));
    }

    {
        SettingItem it;
        it.label = i18n.tr("settings.display.launcher_view", "Launcher View");
        it.description = i18n.tr("settings.display.launcher_view_desc",
                                 "Choose between standard Grid, Dynamic Line carousel, 3D Flow, 3D Shelf, 3D Deck, Fullscreen Cover, XMB, List (Niagara), or Metro (Live Tiles).");
        it.type = ItemType::Selector;
        it.options = {
            i18n.tr("settings.layout.grid", "Grid"),
            i18n.tr("settings.layout.dynamic_line", "Dynamic Line"),
            i18n.tr("settings.layout.flow", "Flow (3D)"),
            i18n.tr("settings.layout.shelf", "Shelf (3D)"),
            i18n.tr("settings.layout.deck", "Deck (3D)"),
            i18n.tr("settings.layout.cover", "Cover"),
            i18n.tr("settings.layout.xmb", "XMB"),
            i18n.tr("settings.layout.list", "List (Niagara)"),
            i18n.tr("settings.layout.metro", "Metro (Live Tiles)"),
        };
        it.intVal = screen.m_layoutMode == AppLayoutMode::Metro ? 8
                  : screen.m_layoutMode == AppLayoutMode::List  ? 7
                  : screen.m_layoutMode == AppLayoutMode::Xmb   ? 6
                  : screen.m_layoutMode == AppLayoutMode::Cover ? 5
                  : screen.m_layoutMode == AppLayoutMode::Deck  ? 4
                  : screen.m_layoutMode == AppLayoutMode::Shelf ? 3
                  : screen.m_layoutMode == AppLayoutMode::Flow  ? 2
                  : screen.m_layoutMode == AppLayoutMode::DynamicLine ? 1 : 0;
        it.onChange = [&screen](SettingItem& self) {
            AppLayoutMode mode = self.intVal == 8 ? AppLayoutMode::Metro
                               : self.intVal == 7 ? AppLayoutMode::List
                               : self.intVal == 6 ? AppLayoutMode::Xmb
                               : self.intVal == 5 ? AppLayoutMode::Cover
                               : self.intVal == 4 ? AppLayoutMode::Deck
                               : self.intVal == 3 ? AppLayoutMode::Shelf
                               : self.intVal == 2 ? AppLayoutMode::Flow
                               : self.intVal == 1 ? AppLayoutMode::DynamicLine
                                                  : AppLayoutMode::Grid;
            screen.m_layoutMode = mode;
            if (screen.m_layoutModeCb) screen.m_layoutModeCb(mode);
        };
        t.items.push_back(std::move(it));
    }

    // Fork ranges (3-8 columns, 2-5 rows): setGridLayoutState clamps to these.
    {
        SettingItem it;
        it.label = i18n.tr("settings.display.grid_columns", "Home Grid Columns");
        it.type = ItemType::Slider;
        it.sliderSteps = 5;
        int cols = std::clamp(screen.m_gridColumns, 3, 8);
        it.floatVal = (float)(cols - 3) / 5.f;
        it.anim01 = it.floatVal;
        it.infoText = std::to_string(cols);
        it.onChange = [&screen](SettingItem& self) {
            int cols = std::clamp(3 + (int)std::round(std::clamp(self.floatVal, 0.f, 1.f) * 5.f), 3, 8);
            self.floatVal = (float)(cols - 3) / 5.f;
            self.anim01 = self.floatVal;
            self.infoText = std::to_string(cols);
            screen.m_gridColumns = cols;
            if (screen.m_gridColumnsCb) screen.m_gridColumnsCb(cols);
        };
        t.items.push_back(std::move(it));
    }

    {
        SettingItem it;
        it.label = i18n.tr("settings.display.grid_rows", "Home Grid Rows");
        it.type = ItemType::Slider;
        it.sliderSteps = 3;
        int rows = std::clamp(screen.m_gridRows, 2, 5);
        it.floatVal = (float)(rows - 2) / 3.f;
        it.anim01 = it.floatVal;
        it.infoText = std::to_string(rows);
        it.onChange = [&screen](SettingItem& self) {
            int rows = std::clamp(2 + (int)std::round(std::clamp(self.floatVal, 0.f, 1.f) * 3.f), 2, 5);
            self.floatVal = (float)(rows - 2) / 3.f;
            self.anim01 = self.floatVal;
            self.infoText = std::to_string(rows);
            screen.m_gridRows = rows;
            if (screen.m_gridRowsCb) screen.m_gridRowsCb(rows);
        };
        t.items.push_back(std::move(it));
    }

    {
        SettingItem it;
        it.label = i18n.tr("settings.display.dynamic_pages", "Dynamic Pages");
        it.description = i18n.tr("settings.display.dynamic_pages_desc",
                                 "Adjust home pages to fit installed games and homebrew instead of keeping 8 fixed pages.");
        it.type = ItemType::Toggle;
        it.boolVal = screen.m_dynamicPages;
        it.anim01 = it.boolVal ? 1.f : 0.f;
        it.onChange = [&screen](SettingItem& self) {
            screen.m_dynamicPages = self.boolVal;
            if (screen.m_dynamicPagesCb) screen.m_dynamicPagesCb(self.boolVal);
        };
        t.items.push_back(std::move(it));
    }

    {
        SettingItem it;
        it.label = i18n.tr("themeshop.options.sort_large_tiles", "Sort Large Tiles Too");
        it.description = i18n.tr("themeshop.options.sort_large_tiles_desc",
                                 "A-Z and Recent also move applications that use a larger tile size.");
        it.type = ItemType::Toggle;
        it.boolVal = screen.m_sortLargeTiles;
        it.anim01 = it.boolVal ? 1.f : 0.f;
        it.onChange = [&screen](SettingItem& self) {
            screen.m_sortLargeTiles = self.boolVal;
            if (screen.m_sortLargeTilesCb) screen.m_sortLargeTilesCb(self.boolVal);
        };
        t.items.push_back(std::move(it));
    }

    {
        SettingItem it;
        it.label = i18n.tr("themeshop.options.icon_contrast", "Adaptive icon contrast");
        it.description = i18n.tr(
            "themeshop.options.icon_contrast_desc",
            "In single-row with a hero, colour HOME icons follow the interface light/dark chrome. Without a hero they keep authored colours (or your custom tint).");
        it.type = ItemType::Toggle;
        it.boolVal = screen.m_iconContrastAdapt;
        it.anim01 = it.boolVal ? 1.f : 0.f;
        it.onChange = [&screen](SettingItem& self) {
            screen.m_iconContrastAdapt = self.boolVal;
            if (screen.m_iconContrastAdaptCb)
                screen.m_iconContrastAdaptCb(self.boolVal);
        };
        t.items.push_back(std::move(it));
    }

    {
        SettingItem it;
        it.label = i18n.tr("themeshop.options.action_hints", "Action Hints");
        it.description = i18n.tr("themeshop.options.action_hints_desc",
                                 "Choose how contextual controls appear in the bottom-right corner.");
        it.type = ItemType::Selector;
        it.options = {
            i18n.tr("themeshop.options.hint_panel", "Panel"),
            i18n.tr("themeshop.options.hint_capsules", "Capsules")
        };
        it.intVal = std::clamp(screen.m_actionHintStyle, 0, 1);
        it.onChange = [&screen](SettingItem& self) {
            screen.m_actionHintStyle = std::clamp(self.intVal, 0, 1);
            if (screen.m_actionHintStyleCb)
                screen.m_actionHintStyleCb(screen.m_actionHintStyle);
        };
        t.items.push_back(std::move(it));
    }

    // --- Automatic day/night theme -----------------------------------------
    // A single entry opens a dedicated window with all the auto-theme settings.
    {
        SettingItem it;
        it.label = i18n.tr("themeshop.options.auto_theme", "Automatic Theme");
        it.description = screen.m_autoThemeSummary.empty()
            ? i18n.tr("themeshop.options.auto_theme_desc",
                      "Switch between a day theme and a night theme automatically.")
            : screen.m_autoThemeSummary;
        it.type = ItemType::Action;
        it.onChange = [&screen](SettingItem&) {
            if (screen.m_autoThemeOpenCb)
                screen.m_autoThemeOpenCb();
        };
        t.items.push_back(std::move(it));
    }

    // Asked for by a player who had just made a shortcut and could only get the
    // grid to show it by rebooting the console.
    {
        SettingItem it;
        it.label = i18n.tr("settings.display.reload_grid", "Reload games and shortcuts");
        it.description = i18n.tr("settings.display.reload_grid_desc",
                                 "Reads the installed titles again. Use it after creating or "
                                 "deleting a shortcut.");
        it.type = ItemType::Action;
        it.onChange = [&screen](SettingItem&) {
            if (screen.m_reloadCatalogCb) screen.m_reloadCatalogCb();
        };
        t.items.push_back(std::move(it));
    }

    // The expensive half of what "reload" used to do, on its own item. Reading
    // a title's control data costs about a second, so a hundred-title console
    // spends minutes on this -- worth it when a name or an icon is actually
    // wrong, and pure waste when a shortcut was merely added.
    {
        SettingItem it;
        it.label = i18n.tr("settings.display.rebuild_names",
                           "Rebuild names and icons");
        it.description = i18n.tr("settings.display.rebuild_names_desc",
                                 "Discards every cached name and icon and reads them from the "
                                 "titles again. Takes a few minutes on a full console.");
        it.type = ItemType::Action;
        it.onChange = [&screen](SettingItem&) {
            if (screen.m_rebuildControlCacheCb) screen.m_rebuildControlCacheCb();
        };
        t.items.push_back(std::move(it));
    }

    return t;
}
