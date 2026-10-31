#pragma once

#include "ThemeCatalogClient.hpp"
#include "YouTubeClient.hpp"
#include <unordered_set>
#include "settings/TabbedOverlayScreen.hpp"
#include "core/AppLayoutMode.hpp"
#include "widgets/AdaptiveIconLoad.hpp"

#include <nxui/core/Input.hpp>
#include <nxui/core/Texture.hpp>

#include <cstdint>
#include <algorithm>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace themeshop::tabs {
class InstalledTab;
class CommunityTab;
class AnimatedTab;
class MusicTab;
class SteamGridDbTab;
class IconsTab;
class SoundsTab;
class InterfaceTab;
class UpdateTab;
class UninstallTab;
}

namespace nxui {
class GpuDevice;
class Renderer;
}

class ThemeShopScreen : public TabbedOverlayScreen {
public:
    struct ThemeShopEntry {
        std::string id;
        std::string name;
        std::string author;
        std::string version;
        std::string source;
        std::string soundPreset;
        std::string coverPath;
        // De onde o tema foi instalado. Vazio para os embutidos, que vivem no
        // romfs do proprio build e nao ocupam nada do cartao.
        std::string installPath;
        bool active = false;
        bool removable = false;
        // Colour glyph pack (spectrum badge + custom tint controls in detail).
        bool colourPack = false;
        // Theme ships sidebar/chrome icons (Community/Installed badge).
        bool hasIcons = false;
        // Theme ships bundled BGM / SFX folders (Music / Sounds badges).
        bool hasMusic = false;
        bool hasSounds = false;
        // Icon packs: present on the console / shipped with SwitchU / online.
        bool installed = true;
        bool builtin = false;
        bool catalog = false;
        // Remote preview for catalog packs that are not installed yet.
        std::string coverUrl;
        std::string description;
    };

    ThemeShopScreen();
    ~ThemeShopScreen() override = default;

    void onMusicEnabledChange(BoolCb cb) { m_musicEnabledCb = std::move(cb); }
    void onUpdateCheck(std::function<void()> cb)   { m_updateCheckCb = std::move(cb); }
    void onUpdateInstall(std::function<void()> cb) { m_updateInstallCb = std::move(cb); }
    void onUpdateRestart(std::function<void()> cb) { m_updateRestartCb = std::move(cb); }
    void onSelfUninstall(std::function<void()> cb) { m_selfUninstallCb = std::move(cb); }
    // Called by the app whenever the update state changes, so the tab redraws
    // with the answer instead of polling for it.
    void setUpdateState(std::string installed, std::string available,
                        std::string status, bool busy,
                        std::string latestVersion, std::string latestNotes,
                        bool restartPending) {
        m_updateInstalledVersion = std::move(installed);
        m_updateAvailableVersion = std::move(available);
        m_updateStatusText = std::move(status);
        m_updateBusy = busy;
        m_updateLatestVersion = std::move(latestVersion);
        m_updateLatestNotes = std::move(latestNotes);
        m_updateRestartPending = restartPending;
        refreshState();
    }
    void beginStateRefreshBatch();
    void endStateRefreshBatch();
    void refreshState();
    void onReleaseNotes(std::function<void()> cb) { m_releaseNotesCb = std::move(cb); }
    void onMusicVolumeChange(FloatCb cb) { m_musicVolumeCb = std::move(cb); }
    void onSfxVolumeChange(FloatCb cb)   { m_sfxVolumeCb = std::move(cb); }
    // The three appearance controls also live under Settings > Display. They
    // are repeated here because this is the screen someone is on while judging
    // how a theme looks, and walking back out to change them breaks that.
    void onGlassSharpnessChange(FloatCb cb)  { m_glassSharpnessCb = std::move(cb); }
    void onBackgroundSpeedChange(FloatCb cb) { m_backgroundSpeedCb = std::move(cb); }
    void onBackgroundBlurChange(FloatCb cb)  { m_backgroundBlurCb = std::move(cb); }
    void onSteamGridDbEnabledChange(BoolCb cb) { m_steamGridDbEnabledCb = std::move(cb); }
    void onSteamGridDbApiKeyRequest(VoidCb cb) { m_steamGridDbApiKeyCb = std::move(cb); }
    void onSteamGridDbScrapeRequest(VoidCb cb) { m_steamGridDbScrapeCb = std::move(cb); }
    void setSteamGridDbState(bool enabled, bool hasApiKey) {
        m_steamGridDbEnabled = enabled;
        m_steamGridDbHasApiKey = hasApiKey;
        refreshState();
    }
    void onSteamGridDbOpacityChange(FloatCb cb) { m_steamGridDbOpacityCb = std::move(cb); }
    // The dossier's RAWG and IGDB keys. They moved here with the SteamGridDB
    // tab, which is where Settings used to keep them.
    void onMetadataKeyRequest(IntCb cb) { m_metadataKeyCb = std::move(cb); }
    void setMetadataKeyState(bool rawg, bool igdbId, bool igdbSecret) {
        m_metadataHasKey[0] = rawg;
        m_metadataHasKey[1] = igdbId;
        m_metadataHasKey[2] = igdbSecret;
        refreshState();
    }
    void onGridColumnsChange(IntCb cb)   { m_gridColumnsCb = std::move(cb); }
    void onGridRowsChange(IntCb cb)      { m_gridRowsCb = std::move(cb); }
    void onDynamicPagesChange(BoolCb cb) { m_dynamicPagesCb = std::move(cb); }
    // Launcher view selector (OmniLaunch views port). setLayoutModeState keeps
    // the 9-option selector in sync when R cycles the view outside this screen.
    using LayoutModeCb = std::function<void(AppLayoutMode)>;
    void onLayoutModeChange(LayoutModeCb cb) { m_layoutModeCb = std::move(cb); }
    void setLayoutModeState(AppLayoutMode mode) {
        m_layoutMode = mode;
        const int val = mode == AppLayoutMode::Metro ? 8
                      : mode == AppLayoutMode::List  ? 7
                      : mode == AppLayoutMode::Xmb   ? 6
                      : mode == AppLayoutMode::Cover ? 5
                      : mode == AppLayoutMode::Deck  ? 4
                      : mode == AppLayoutMode::Shelf ? 3
                      : mode == AppLayoutMode::Flow  ? 2
                      : mode == AppLayoutMode::DynamicLine ? 1 : 0;
        for (auto& tab : m_tabs) {
            for (auto& item : tab.items) {
                if (item.type == ItemType::Selector && item.options.size() == 9) {
                    item.intVal = val;
                }
            }
        }
    }
    void onActionHintStyleChange(IntCb cb) { m_actionHintStyleCb = std::move(cb); }
    void onSortLargeTilesChange(BoolCb cb) { m_sortLargeTilesCb = std::move(cb); }
    void onBatterySaverChange(BoolCb cb) { m_batterySaverCb = std::move(cb); }
    void onIconContrastAdaptChange(BoolCb cb) { m_iconContrastAdaptCb = std::move(cb); }
    void onThemeMusicOverrideChange(BoolCb cb) { m_themeMusicOverrideCb = std::move(cb); }
    void onThemeMusicAddToPlaylistChange(BoolCb cb) { m_themeMusicAddToPlaylistCb = std::move(cb); }
    void onThemeMusicPlayOnApplyChange(BoolCb cb) { m_themeMusicPlayOnApplyCb = std::move(cb); }
    struct IconPackTintState {
        bool enabled = false;
        bool flatten = false;
        bool grayscale = false;
        bool matchTheme = false;
        float hue = 0.5f;
        float sat = 0.5f;
        float light = 0.5f;
    };
    using IconPackTintCb = std::function<void(const std::string& packId, const IconPackTintState& tint)>;
    void onIconPackTintChange(IconPackTintCb cb) { m_iconPackTintCb = std::move(cb); }
    bool detailOpen() const { return m_detailOpen; }
    // Leave the product sheet and return focus to the shelf grid.
    void closeDetailView() { closeDetail(); }
    void setInputSource(const nxui::Input* input) { m_input = input; }
    void onAutoThemeOpen(VoidCb cb) { m_autoThemeOpenCb = std::move(cb); }
    void onNextTrack(VoidCb cb)          { m_nextTrackCb = std::move(cb); }
    void onThemeShopApply(StringCb cb)   { m_themeShopApplyCb = std::move(cb); }
    void onIconPackApply(StringCb cb)    { m_iconPackApplyCb = std::move(cb); }
    void onIconPackDownload(StringCb cb) { m_iconPackDownloadCb = std::move(cb); }
    void onIconPackDownloadApply(StringCb cb) { m_iconPackDownloadApplyCb = std::move(cb); }
    void onIconPackDelete(StringCb cb)   { m_iconPackDeleteCb = std::move(cb); }
    // Catalog icons finished (re)loading; the app rebuilds the pack list.
    void onIconCatalogUpdated(VoidCb cb) { m_iconCatalogUpdatedCb = std::move(cb); }
    void onThemeShopDelete(StringCb cb)  { m_themeShopDeleteCb = std::move(cb); }
    void onThemeShopDownload(StringCb cb) { m_themeShopDownloadCb = std::move(cb); }
    void onThemeShopDownloadInstall(StringCb cb) { m_themeShopDownloadInstallCb = std::move(cb); }
    void onNetConnectRequest(VoidCb cb)  { m_netConnectCb = std::move(cb); }
    // Recarrega a grade: relê os títulos instalados e descarta nomes e ícones
    // em cache. É a saída para um atalho cujo id foi reaproveitado, que não
    // parece novo para nenhuma verificação automática.
    void onReloadCatalog(VoidCb cb)      { m_reloadCatalogCb = std::move(cb); }
    void onRebuildControlCache(VoidCb cb) { m_rebuildControlCacheCb = std::move(cb); }

    void setMusicState(bool enabled, float musicVol, float sfxVol) {
        m_musicEnabled = enabled;
        m_musicVolume = musicVol;
        m_sfxVolume = sfxVol;
    }
    void setAppearanceState(float glassSharpness, float backgroundSpeed, float backgroundBlur) {
        m_glassSharpness = std::clamp(glassSharpness, 0.f, 1.f);
        m_backgroundSpeed = std::clamp(backgroundSpeed, 0.f, 1.f);
        m_backgroundBlur = std::clamp(backgroundBlur, 0.f, 1.f);
    }
    void setGridLayoutState(int columns, int rows) {
        m_gridColumns = std::clamp(columns, 3, 8);
        m_gridRows = std::clamp(rows, 2, 5);
    }
    void setDynamicPagesState(bool enabled) {
        m_dynamicPages = enabled;
    }
    void setSteamGridDbOpacity(float v) {
        m_steamGridDbOpacity = std::clamp(v, 0.f, 1.f);
    }
    void setSortLargeTilesState(bool enabled) { m_sortLargeTiles = enabled; }
    void setBatterySaverState(bool enabled) { m_batterySaver = enabled; }
    void setFrameTapLiteEngaged(bool engaged) { m_frameTapLiteEngaged = engaged; }
    void setIconContrastAdaptState(bool enabled) { m_iconContrastAdapt = enabled; }
    void setThemeMusicOverrideState(bool enabled) { m_themeMusicOverride = enabled; }
    void setThemeMusicAddToPlaylistState(bool enabled) { m_themeMusicAddToPlaylist = enabled; }
    void setThemeMusicPlayOnApplyState(bool enabled) { m_themeMusicPlayOnApply = enabled; }
    void setIconPackTintState(const std::unordered_map<std::string, IconPackTintState>& tints) {
        m_iconPackTints = tints;
    }
    // Theme accent used by Match theme (app refreshes on theme apply / shop open).
    void setThemeMatchAccent(float h, float s, float l) {
        m_themeMatchAccentH = h;
        m_themeMatchAccentS = s;
        m_themeMatchAccentL = l;
        m_themeMatchAccentValid = true;
    }
    // Soft-invalidate adaptive icon previews (optional cover-path filter).
    void invalidateIconPackPreviews(const std::string& coverPathFilter = {});
    // Cancel in-flight installed/icon decodes so Community can use the pool.
    void pauseInstalledPreviewDecodes();
    // Stop in-flight preview workers without destroying GPU textures (sleep-safe).
    void pausePreviewWorkForSleep();
    // Persist draft tint (Theme Shop → config/HOME). Call before Apply.
    void commitDraftIconPackTint();
    void setActionHintStyleState(const std::string& style) {
        m_actionHintStyle = style == "panel" ? 0 : 1;
    }

    void setAutoThemeSummary(std::string summary) {
        m_autoThemeSummary = std::move(summary);
    }

    void setThreadPool(nxui::ThreadPool* pool);
    void setRenderContext(nxui::GpuDevice* gpu, nxui::Renderer* renderer);
    void show() override;
    void refreshCommunityCatalog(bool forceNetwork = false);

    // Vira a página do catálogo, de onde quer que o foco esteja.
    //
    // Chegar até os botões Anterior e Próxima é descer a grade inteira, e num
    // catálogo de dezenas de temas virar página é o gesto mais repetido da
    // tela; os gatilhos L e R fazem isso sem tirar o foco do lugar. Devolve
    // true quando a página mudou de fato, para o som tocar só aí.
    //
    // Recusa com o detalhe aberto: ali L e R já percorrem as capturas.
    bool stepCataloguePage(int delta);
    void requestRenderDiagnostics(int frames = 6) {
        if (frames > m_renderDebugFrames)
            m_renderDebugFrames = frames;
    }
    enum class ShelfMode {
        Installed = 0,
        Shop = 1,
    };

    bool showsCatalogRefresh() const;
    bool showsShelfBranch() const;
    ShelfMode activeShelfMode() const;
    void setActiveShelfMode(ShelfMode mode);

    void setThemeShopState(const std::vector<ThemeShopEntry>& entries,
                           const std::string& activeId);
    void setIconPackState(const std::vector<ThemeShopEntry>& entries,
                          const std::string& activeId);
    void setPackageTransferState(const ThemeTransferState& state,
                                 std::string themeId,
                                 bool installMode);
    const ThemeShopEntry* selectedThemeShopEntry() const;
    // O tema do catálogo que já está instalado, ou nullptr. Um pacote baixado
    // vira um preset com id "package:<id do catálogo>", e é essa a ligação
    // entre as duas listas.
    const ThemeShopEntry* installedEntryForCatalogue(const std::string& catalogueId) const;
    using TextEntryRequestCb = std::function<void(const std::string& title,
                                                   const std::string& guide,
                                                   const std::string& initial,
                                                   std::function<void(std::string)> onAccept)>;
    using ProgressShowCb = std::function<void(const std::string& title, const std::string& message, float progress01)>;
    using ProgressUpdateCb = std::function<void(const std::string& message, float progress01)>;
    using ProgressHideCb = std::function<void()>;

    using PlayMusicCb = std::function<void(const std::string& path, const std::string& title)>;

    void onRequestTextEntry(TextEntryRequestCb cb) { m_requestTextEntryCb = std::move(cb); }
    void onProgressShow(ProgressShowCb cb) { m_progressShowCb = std::move(cb); }
    void onProgressUpdate(ProgressUpdateCb cb) { m_progressUpdateCb = std::move(cb); }
    void onProgressHide(ProgressHideCb cb) { m_progressHideCb = std::move(cb); }
    void onMusicDownloaded(VoidCb cb) { m_musicDownloadedCb = std::move(cb); }
    void onPlayMusic(PlayMusicCb cb) { m_playMusicCb = std::move(cb); }
    bool isMusicTab() const;
    void pollMusicDownloads();
    void searchYouTube(const std::string& query);
    void resetMusicTabState();
    YouTubeClient& youTubeClient() { return m_youTubeClient; }
    const YouTubeClient::TrackItem* selectedMusicTrack() const;

    const ThemeCatalogClient::Entry* selectedCommunityThemeEntry() const;
    const ThemeCatalogClient::Entry* findCommunityThemeEntry(const std::string& themeId) const;
    const std::vector<ThemeCatalogClient::IconEntry>& catalogIconEntries() const {
        return m_catalogIconEntries;
    }
    const std::string& communityCatalogUrl() const {
        return m_catalogClient.catalogUrl();
    }

protected:
    void buildTabs() override;
    bool usesCustomContentLayout() const override { return m_tabIndex <= kMusicTabIndex; }
    bool consumeRenderDiagnosticsFrame() override;
    void drawCustomContent(nxui::Renderer& ren, const nxui::Rect& panel, const nxui::Rect& content, float opacity) override;
    void updateCustomContent(float dt) override;
    bool handleCustomPressA() override;
    bool handleCustomPressB() override;
    bool handleCustomPressX() override;
    bool handleCustomNavUp() override;
    bool handleCustomNavDown() override;
    bool handleCustomNavLeft() override;
    bool handleCustomNavRight() override;
    bool handleCustomTouch(nxui::Input& input, const nxui::Rect& panel, const nxui::Rect& tabs, const nxui::Rect& content) override;
    float tabBranchExtraHeight(int tabIndex) const override;
    void drawTabBranch(nxui::Renderer& ren, int tabIndex, const nxui::Rect& branchRect, float opacity) override;
    bool tabBranchConsumesFocus() const override;
    bool tabFocusCursorRect(nxui::Rect& outRect) const override;
    void onTabRailParentActivated(int tabIndex) override;
    bool onTabRailBranchActivated(int parentTabIndex) override;
    void currentAccessibilityParts(std::string& context,
                                   std::string& position,
                                   std::string& summary,
                                   bool& forceRepeat) const override;
    std::string currentAccessibilitySummary() const override;

private:
    friend class themeshop::tabs::InstalledTab;
    friend class themeshop::tabs::CommunityTab;
    friend class themeshop::tabs::AnimatedTab;
    friend class themeshop::tabs::MusicTab;
    friend class themeshop::tabs::IconsTab;
    friend class themeshop::tabs::SoundsTab;
    friend class themeshop::tabs::InterfaceTab;
    friend class themeshop::tabs::UpdateTab;
    friend class themeshop::tabs::UninstallTab;
    friend class themeshop::tabs::SteamGridDbTab;

    // Sidebar order. Themes and Icons carry an Installed | Shop branch; the
    // first four are custom-drawn (see usesCustomContentLayout), the rest are
    // settings rows: Sounds, Interface, SteamGridDB, Update, Uninstall.
    static constexpr int kThemesTabIndex = 0;
    static constexpr int kAnimatedTabIndex = 1;
    static constexpr int kIconsTabIndex = 2;
    static constexpr int kMusicTabIndex = 3;

    enum class PreviewPhase {
        Idle,
        Loading,
        Downloaded,
        Ready,
        Failed,
    };

    struct PreviewImageState {
        std::mutex mutex;
        PreviewPhase phase = PreviewPhase::Idle;
        int failureCount = 0;
        bool cancelled = false;
        bool adaptiveRequested = false; // load via AdaptiveIconLoad
        bool adaptive = false;          // mono result -> tint with chrome
        bool hasContrastMask = false;   // colour pack coverage twin for chrome follow
        std::uint32_t hslKey = 0;       // custom-tint fingerprint for cache reuse
        int maxSide = 0;                // decode budget used for this slot
        int pixelW = 0;
        int pixelH = 0;
        std::string url;
        std::vector<std::uint8_t> bytes;       // compressed community download
        std::vector<std::uint8_t> pixels;      // decoded RGBA (installed/icons)
        std::vector<std::uint8_t> maskPixels;  // optional contrast twin
        std::future<void> future;
        nxui::Texture texture;
        nxui::Texture contrastMask;
    };

    enum class ThemeTouchTarget {
        None,
        Search,
        Refresh,
        PagePrev,
        PageNext,
        GridCard,
        DetailPreviewControl,
        DetailButton,
        DetailTintToggle,
        DetailMatchThemeToggle,
        DetailFlattenToggle,
        DetailGrayscaleToggle,
        DetailTintHue,
        DetailTintSat,
        DetailTintLight,
        FullscreenPreview,
        DetailBackdrop,
    };

    enum class DetailFocusArea {
        Preview,
        TintToggle,
        MatchThemeToggle,
        FlattenToggle,
        GrayscaleToggle,
        TintHue,
        TintSat,
        TintLight,
        Buttons,
    };

    enum class ContentFocusArea {
        Grid,
        Header,
        Pager,
    };

    bool pollCommunityCatalog();
    void syncCommunityCatalog(const ThemeCatalogClient::Snapshot& snapshot);
    // Warm catalog + shelf covers while still on Installed so Community
    // first-focus / page flips do not pay disk/HTTP bootstrap on the focus frame.
    void ensureCommunityCatalogWarm();
    // First shelf + last Community scroll neighborhood (±1 page).
    void prefetchCommunityShelfNeighborhood();
    bool isThemesTab() const;
    bool isCommunityTab() const; // Themes tab + Shop shelf
    bool isIconsTab() const;
    bool isInstalledTab() const; // Themes tab + Installed shelf
    bool isIconsShopMode() const;
    bool isIconsInstalledMode() const;
    bool isAnimatedTab() const;
    // One line summarising the open tab's whole catalogue: how many themes it
    // has, and what they cost to download and to keep.
    std::string communityCatalogueTotals() const;

    // O mesmo resumo para o que já está no console: quantos temas e quanto do
    // cartão eles ocupam.
    std::string installedThemeTotals() const;
    std::string installedMusicTotals() const;
    // O que um tema instalado ocupa, ou 0 enquanto a medição não voltou.
    std::uint64_t installedThemeBytes(const std::string& installPath) const;
    // Põe na fila a medição dos temas que ainda não têm tamanho. Percorrer a
    // pasta de um tema animado é centenas de arquivos no cartão, então isso
    // nunca acontece na thread que desenha.
    void measureInstalledThemes();
    int currentEntryCount() const;
    int currentSelectedIndex() const;
    void setCurrentSelectedIndex(int idx);
    int currentPage() const;
    int pageCount() const;
    void setCurrentPage(int page);
    void stepPage(int delta);
    void applySearchFilter();
    bool promptSearchQuery();
    int detailScreenshotCount() const;
    std::string currentDetailCommunityPreviewPath() const;
    void clampDetailScreenshotIndex();
    void stepDetailScreenshot(int delta);
    int& currentScrollRowRef();
    void ensureSelectionVisible();
    void openDetail();
    void closeDetail();
    int detailButtonCount() const;
    void activateDetailButton(int buttonIndex);
    int hitTestGridCard(const nxui::Rect& content, float x, float y) const;
    std::string resolveCommunityPreviewUrl(const std::string& previewPath) const;
    std::string resolveCommunityPreviewUrl(const ThemeCatalogClient::Entry& entry) const;
    std::shared_ptr<PreviewImageState> communityPreviewState(const std::string& previewPath) const;
    std::shared_ptr<PreviewImageState> communityPreviewState(const ThemeCatalogClient::Entry& entry) const;
    PreviewPhase communityPreviewPhase(const std::string& previewPath) const;
    PreviewPhase communityPreviewPhase(const ThemeCatalogClient::Entry& entry) const;
    const nxui::Texture* communityPreviewTexture(const std::string& previewPath) const;
    const nxui::Texture* communityPreviewTexture(const ThemeCatalogClient::Entry& entry) const;
    std::string installedPreviewCacheKey(const std::string& previewPath,
                                         bool adaptiveLoad,
                                         const std::string& iconPackId,
                                         const switchu::icons::HslAdjust* hslOverride = nullptr) const;
    PreviewPhase installedPreviewPhase(const std::string& previewPath,
                                       bool adaptiveLoad = false,
                                       const std::string& iconPackId = {},
                                       const switchu::icons::HslAdjust* hslOverride = nullptr) const;
    const nxui::Texture* installedPreviewTexture(const std::string& previewPath,
                                                 bool adaptiveLoad = false,
                                                 const std::string& iconPackId = {},
                                                 const switchu::icons::HslAdjust* hslOverride = nullptr) const;
    bool installedPreviewAdaptive(const std::string& previewPath,
                                  bool adaptiveLoad = false,
                                  const std::string& iconPackId = {},
                                  const switchu::icons::HslAdjust* hslOverride = nullptr) const;
    const nxui::Texture* installedPreviewContrastMask(const std::string& previewPath,
                                                      bool adaptiveLoad = false,
                                                      const std::string& iconPackId = {},
                                                      const switchu::icons::HslAdjust* hslOverride = nullptr) const;
    void primeInstalledPreview(const std::string& previewPath,
                               bool adaptiveLoad = false,
                               const std::string& iconPackId = {},
                               const switchu::icons::HslAdjust* hslOverride = nullptr,
                               int maxSide = 0);
    void syncFinishedInstalledPreviewLoads();
    void trimInstalledPreviewCache();
    // Drop all installed/Icons GPU frames + CPU pixels (Frame Tap Applet headroom).
    void clearInstalledPreviewCache();
    bool selectedIconPackIsColour() const;
    // Installed Icons-tab pack: Custom tint / Flatten / Grayscale controls.
    bool showSelectedIconTintControls() const;
    bool selectedIconShowsHueSat() const;
    IconPackTintState iconPackTintFor(const std::string& packId) const;
    IconPackTintState& selectedIconPackTint();
    const IconPackTintState& selectedIconPackTint() const;
    bool selectedIconCustomTint() const { return selectedIconPackTint().enabled; }
    void beginIconPackTintDraft();
    void discardIconPackTintDraft();
    void setIconCustomTintEnabled(bool enabled);
    void setIconMatchThemeEnabled(bool enabled);
    void setIconFlattenEnabled(bool enabled);
    void setIconGrayscaleEnabled(bool enabled);
    void setIconTintHue(float hue01);
    void setIconTintSat(float sat01);
    void setIconTintLight(float light01);
    bool applyMatchThemePivotsToTint(IconPackTintState& tint,
                                     const std::string& meanSamplePath) const;
    void nudgeIconTintHue(float delta);
    void nudgeIconTintSat(float delta);
    void nudgeIconTintLight(float delta);
    void emitIconPackTint(const std::string& packId, const IconPackTintState& tint);
    switchu::icons::HslAdjust hslAdjustForPack(const std::string& packId) const;
    switchu::icons::HslAdjust hslAdjustFromTint(const IconPackTintState& tint) const;
    void maybeSeedFlattenLightDefault();
    void updateTintSliderAnalog(float dt);
    void primeCommunityPreview(const std::string& previewPath);
    void primeCommunityPreview(const ThemeCatalogClient::Entry& entry);
    void primeVisibleCommunityPreviews();
    void primeVisibleInstalledPreviews();
    void syncFinishedCommunityPreviewLoads();
    void clearCommunityPreviewCache();
    // Hard-drop community + installed preview GPU on hide so Frame Tap can
    // remount (~30–40 MB Applet free). Catalog/disk cache stay warm.
    void releasePreviewCachesOnHide();
    /* pressure=true: reclaim screenshots / excess covers while browsing under
     * GPU pressure. Hide path uses releasePreviewCachesOnHide() instead. */
    void trimCommunityPreviewCache(bool pressure = false);
    int previewUploadsBudgetThisFrame() const;

    BoolCb m_musicEnabledCb;
    FloatCb m_musicVolumeCb;
    FloatCb m_sfxVolumeCb;
    FloatCb m_glassSharpnessCb;
    FloatCb m_backgroundSpeedCb;
    FloatCb m_backgroundBlurCb;
    FloatCb m_steamGridDbOpacityCb;
    BoolCb m_steamGridDbEnabledCb;
    VoidCb m_steamGridDbApiKeyCb;
    VoidCb m_steamGridDbScrapeCb;
    IntCb m_metadataKeyCb;
    bool m_metadataHasKey[3] = {false, false, false};
    IntCb m_gridColumnsCb;
    IntCb m_gridRowsCb;
    BoolCb m_dynamicPagesCb;
    LayoutModeCb m_layoutModeCb;
    AppLayoutMode m_layoutMode = AppLayoutMode::Grid;
    IntCb m_actionHintStyleCb;
    BoolCb m_sortLargeTilesCb;
    BoolCb m_batterySaverCb;
    BoolCb m_iconContrastAdaptCb;
    BoolCb m_themeMusicOverrideCb;
    BoolCb m_themeMusicAddToPlaylistCb;
    BoolCb m_themeMusicPlayOnApplyCb;
    IconPackTintCb m_iconPackTintCb;
    VoidCb m_autoThemeOpenCb;
    VoidCb m_nextTrackCb;
    StringCb m_themeShopApplyCb;
    StringCb m_iconPackApplyCb;
    StringCb m_iconPackDownloadCb;
    StringCb m_iconPackDownloadApplyCb;
    StringCb m_iconPackDeleteCb;
    VoidCb m_iconCatalogUpdatedCb;
    StringCb m_themeShopDeleteCb;
    StringCb m_themeShopDownloadCb;
    StringCb m_themeShopDownloadInstallCb;
    VoidCb m_netConnectCb;

    bool m_musicEnabled = true;
    // What the Update tab shows. The app owns the check itself and pushes the
    // outcome here, so the tab stays a view rather than a second client.
    std::string m_updateInstalledVersion;
    std::string m_updateAvailableVersion;
    std::string m_updateStatusText;
    bool  m_updateBusy = false;
    // Um pacote já baixado e conferido, esperando o reinício que o aplica. Nesse
    // estado não há o que baixar de novo, e era justamente isso que fazia a aba
    // oferecer a mesma atualização sem fim.
    bool  m_updateRestartPending = false;
    std::function<void()> m_updateCheckCb;
    std::function<void()> m_updateInstallCb;
    std::function<void()> m_updateRestartCb;
    std::function<void()> m_selfUninstallCb;
    VoidCb m_reloadCatalogCb;
    VoidCb m_rebuildControlCacheCb;
    std::function<void()> m_releaseNotesCb;
    std::string m_updateLatestVersion;
    std::string m_updateLatestNotes;
    float m_musicVolume = 0.4f;
    float m_sfxVolume = 0.7f;
    float m_glassSharpness = 0.4f;
    float m_backgroundSpeed = 0.5f;
    float m_backgroundBlur = 0.f;
    float m_steamGridDbOpacity = 0.50f;
    bool m_steamGridDbEnabled = true;
    bool m_steamGridDbHasApiKey = false;
    int m_gridColumns = 5;
    int m_gridRows = 3;
    bool m_dynamicPages = true;
    int m_actionHintStyle = 1;
    bool m_sortLargeTiles = true;
    bool m_batterySaver = false;
    bool m_frameTapLiteEngaged = false;
    bool m_iconContrastAdapt = true;
    bool m_themeMusicOverride = true;
    bool m_themeMusicAddToPlaylist = true;
    bool m_themeMusicPlayOnApply = true;
    bool m_themeMatchAccentValid = false;
    float m_themeMatchAccentH = 0.f;
    float m_themeMatchAccentS = 0.55f;
    float m_themeMatchAccentL = 0.55f;
    // Committed tints (match config / HOME / Icons grid cards).
    std::unordered_map<std::string, IconPackTintState> m_iconPackTints;
    // Live HSL edit while the detail sheet is open - preview only until Apply.
    bool m_draftTintActive = false;
    std::string m_draftTintPackId;
    IconPackTintState m_draftTint;
    float m_tintSliderHoldTime = 0.f;
    const nxui::Input* m_input = nullptr;
    std::string m_autoThemeSummary;
    std::string m_searchQuery;
    std::vector<ThemeShopEntry> m_allThemeShopEntries;
    std::vector<ThemeShopEntry> m_themeShopEntries;
    std::string m_themeShopSelectedId;
    std::vector<ThemeShopEntry> m_allIconPackEntries;
    std::vector<ThemeShopEntry> m_iconPackEntries;
    std::string m_iconPackSelectedId;

    // Tamanhos medidos por caminho de instalação. Compartilhado com as threads
    // de trabalho que fazem a medição, daí o mutex; a leitura acontece durante
    // o desenho e é só uma consulta.
    struct InstalledSizes {
        mutable std::mutex mutex;
        std::unordered_map<std::string, std::uint64_t> bytes;
        std::unordered_set<std::string> inFlight;
    };
    std::shared_ptr<InstalledSizes> m_installedSizes = std::make_shared<InstalledSizes>();

    nxui::ThreadPool* m_threadPool = nullptr;
    nxui::GpuDevice* m_gpu = nullptr;
    nxui::Renderer* m_renderer = nullptr;
    ThemeCatalogClient m_catalogClient;
    ThemeTransferState m_communityTransferState;
    ThemeTransferState m_packageTransferState;
    std::unordered_map<std::string, std::shared_ptr<PreviewImageState>> m_communityPreviewCache;
    std::unordered_map<std::string, std::shared_ptr<PreviewImageState>> m_installedPreviewCache;
    std::vector<ThemeCatalogClient::Entry> m_allCommunityEntries;
    std::vector<ThemeCatalogClient::Entry> m_communityEntries;
    std::vector<ThemeCatalogClient::IconEntry> m_catalogIconEntries;
    std::string m_communitySelectedId;
    std::string m_packageTransferThemeId;
    bool m_packageTransferInstallMode = false;
    bool m_hasPendingCommunityPreviewWork = false;
    bool m_hasPendingInstalledPreviewWork = false;
    std::uint64_t m_communityRevision = 0;
    // Set on each Theme Shop open so Community/Icons re-pull index.json even
    // when a disk cache already exists (renames, new themes, badge flags).
    bool m_catalogNeedsNetworkRefresh = true;
    bool m_detailOpen = false;
    bool m_detailFullscreen = false;
    std::string m_lastLoggedDetailPath;
    // Snap blue ring to grid selection after detail close (no Apply->card lerp).
    bool m_snapGridFocus = false;
    ContentFocusArea m_contentFocusArea = ContentFocusArea::Grid;
    DetailFocusArea m_detailFocusArea = DetailFocusArea::Buttons;
    int m_headerButtonIndex = 0;
    int m_pageButtonIndex = 1;
    int m_detailButtonIndex = 0;
    int m_detailPreviewButtonIndex = 0;
    int m_detailScreenshotIndex = 0;
    nxui::AnimatedFloat m_detailSheetAnim{0.f};
    nxui::AnimatedFloat m_detailFullscreenAnim{0.f};
    // Soft eShop page flip: 1 → 0 after stepPage (sign in m_pageSlideFrom).
    nxui::AnimatedFloat m_pageSlideAnim{0.f};
    float m_pageSlideFrom = 0.f;
    bool m_previewPausedWhileHidden = false;
    int m_installedScrollRow = 0;
    int m_communityScrollRow = 0;
    int m_musicScrollRow = 0;
    int m_musicSelectedIndex = 0;
    std::string m_youtubeSearchQuery;
    YouTubeClient m_youTubeClient;
    TextEntryRequestCb m_requestTextEntryCb;
    ProgressShowCb m_progressShowCb;
    ProgressUpdateCb m_progressUpdateCb;
    ProgressHideCb m_progressHideCb;
    VoidCb m_musicDownloadedCb;
    PlayMusicCb m_playMusicCb;
    int m_iconPackScrollRow = 0;
    ShelfMode m_themesShelfMode = ShelfMode::Installed;
    ShelfMode m_iconsShelfMode = ShelfMode::Installed;
    bool m_shopBranchFocused = false;
    nxui::Rect m_shopBranchRect{};
    nxui::AnimatedFloat m_shopBranchReveal{0.f};
    int m_lastCustomTabIndex = -1;
    std::string m_lastPreviewPrimeKey;
    float m_previewTrimTimer = 0.f;
    unsigned m_stateRefreshBatchDepth = 0;
    bool m_stateRefreshPending = false;
    ThemeTouchTarget m_themeTouchTarget = ThemeTouchTarget::None;
    int m_themeTouchIndex = -1;
    float m_themeTouchStartX = 0.f;
    float m_themeTouchStartY = 0.f;
    int m_renderDebugFrames = 0;
    bool m_renderDiagnosticsActive = false;
};
