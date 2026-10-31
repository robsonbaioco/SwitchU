#include "ThemeShopScreen.hpp"
#include "ThemeShopTabBuilders.hpp"
#include "core/DebugLog.hpp"

void ThemeShopScreen::buildTabs() {
    DebugLog::log("[themeshop] buildTabs() start");
    // Replace, don't append — refreshTranslations / rebuild can call this again.
    // Also size the settings-row widget cache: Themes/Icons are custom-drawn, but
    // Sounds/Interface use m_cachedTabContentWidgets. setTheme() often runs while
    // m_tabs is still empty (cache resized to 0); Icons → Icon Shop → Down into
    // Sounds then indexed a null cache (Data Abort at null+0x30).
    m_tabs.clear();
    DebugLog::log("[themeshop]   ThemesTab...");
    m_tabs.push_back(themeshop::tabs::InstalledTab::build(*this));
    // Sidebar order matches ThemeShopScreen::k*TabIndex: Themes (Installed |
    // Theme Shop) first, then the fork's Animated catalogue -- ours, and what
    // most people come for -- Icons (Installed | Icon Shop), Music (YouTube),
    // then the settings tabs.
    DebugLog::log("[themeshop]   AnimatedTab...");
    m_tabs.push_back(themeshop::tabs::AnimatedTab::build(*this));
    DebugLog::log("[themeshop]   IconsTab...");
    m_tabs.push_back(themeshop::tabs::IconsTab::build(*this));
    DebugLog::log("[themeshop]   MusicTab...");
    m_tabs.push_back(themeshop::tabs::MusicTab::build(*this));
    DebugLog::log("[themeshop]   SoundsTab...");
    m_tabs.push_back(themeshop::tabs::SoundsTab::build(*this));
    DebugLog::log("[themeshop]   InterfaceTab...");
    m_tabs.push_back(themeshop::tabs::InterfaceTab::build(*this));
    DebugLog::log("[themeshop]   SteamGridDbTab...");
    m_tabs.push_back(themeshop::tabs::SteamGridDbTab::build(*this));
    DebugLog::log("[themeshop]   UpdateTab...");
    m_tabs.push_back(themeshop::tabs::UpdateTab::build(*this));
    DebugLog::log("[themeshop]   UninstallTab...");
    m_tabs.push_back(themeshop::tabs::UninstallTab::build(*this));
    DebugLog::log("[themeshop] buildTabs() done (%d tabs)", (int)m_tabs.size());
    // Quanto do orcamento de imagem sobra para as previas. Sem este numero,
    // uma previa que nao aparece nao se distingue de uma previa que nao coube.
    if (m_gpu)
        DebugLog::log("[themeshop] imagem em %.1f de %.1f MB ao abrir",
                      m_gpu->imageMemoryUsed() / 1048576.0,
                      nxui::GpuDevice::imageBudget() / 1048576.0);

    m_cachedTabContentWidgets.clear();
    m_cachedTabContentWidgets.resize(m_tabs.size());

    // TabbedOverlayScreen owns the presentation refresh after every build:
    // warmup(), refreshTranslations(), and rebuildCurrentTab() all restore the
    // selected tab before rebuilding its widgets.  Keep this method to
    // data/cache construction only.
}
