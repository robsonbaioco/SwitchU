#pragma once

#include <nxui/widgets/Widget.hpp>
#include <nxui/core/Types.hpp>
#include <nxui/core/Font.hpp>
#include <nxui/core/Input.hpp>
#include <nxui/core/Texture.hpp>
#include <nxui/Theme.hpp>
#include "widgets/SelectionCursor.hpp"

#include <string>
#include <vector>
#include <functional>
#include <cstdint>

namespace nxui {
class Renderer;
}

class QuickSettingsOverlay : public nxui::Widget {
public:
    enum class ItemIndex {
        Brightness = 0,
        BgmVolume,
        SfxVolume,
        BatterySaver,
        AirplaneMode,
        Wifi,
        Vibration,
        ActivityLog,
        PowerActions,
        Count
    };

    enum class PowerAction {
        Sleep = 0,
        Reboot,
        Shutdown,
        Count
    };

    struct Callbacks {
        std::function<void(float)> onBrightnessChanged;
        std::function<void(float)> onBgmVolumeChanged;
        std::function<void(float)> onSfxVolumeChanged;
        std::function<void(bool)>  onAirplaneModeToggled;
        std::function<void(bool)>  onWifiToggled;
        std::function<void()>      onActivityLogRequested;
        // PoloNX #177 Lite Mode+ / Battery Saver row.
        std::function<void(bool)>  onBatterySaverToggled;
        std::function<void()>      onSleepRequested;
        std::function<void()>      onRebootRequested;
        std::function<void()>      onShutdownRequested;
        std::function<void()>      onClose;
        std::function<void()>      onNavigateSfx;
        std::function<void()>      onActivateSfx;
        std::function<void()>      onToggleOffSfx;
    };

    QuickSettingsOverlay();
    ~QuickSettingsOverlay() override;

    void setFont(nxui::Font* f)      { m_font = f; }
    void setSmallFont(nxui::Font* f) { m_smallFont = f; }
    void setIconFont(nxui::Font* f)  { m_iconFont = f; }
    void setTheme(const nxui::Theme* t);
    void setInput(nxui::Input* input) { m_input = input; }
    void setCallbacks(const Callbacks& cb) { m_callbacks = cb; }
    void setInstantCursorMotion(bool instant) { m_cursor.setInstantMotion(instant); }
    // Resolve toggle glyphs (leaf, airplane) from the active icon pack, with
    // the default pack as fallback. The name is PoloNX's, whose panel also
    // carries music transport icons; this fork's panel has none.
    void setIconGlyphResolver(std::function<std::string(const char* fileName)> resolve) {
        m_iconGlyphResolve = std::move(resolve);
        invalidateMusicControlIcons();
    }
    void invalidateMusicControlIcons();

    void show();
    void hide();
    bool isActive() const { return m_active || m_animating; }
    bool isFullyVisible() const { return m_active && !m_animating; }

    // External real-time status update from daemon or system messages
    void setBatteryStatus(int percent, bool charging);
    // PoloNX #177 Battery Saver. When Frame Tap's viewer holds lite, the row is
    // locked ON and relabelled; the preference underneath is unchanged.
    void setBatterySaver(bool enabled);
    bool batterySaver() const { return m_batterySaver; }
    void setFrameTapLiteEngaged(bool engaged);
    bool frameTapLiteEngaged() const { return m_frameTapLiteEngaged; }

    // Initial state setters before showing
    void setInitialValues(float brightness, float bgmVolume, float sfxVolume,
                          bool airplaneMode, bool wifiEnabled);

    void handleTouch(nxui::Input& input);
    void update(float dt) override;
    void render(nxui::Renderer& ren) override;

private:
    void setupNavigationActions();
    void refreshHardwareStatus();
    void adjustSlider(ItemIndex item, float delta);
    void toggleItem(ItemIndex item);
    void triggerPowerAction(PowerAction action);
    void updateCursorTarget();

    nxui::Rect computePanelRect() const;
    nxui::Rect computeItemRect(ItemIndex item) const;
    nxui::Rect computePowerButtonRect(PowerAction action) const;
    nxui::Rect computeSliderTrackRect(ItemIndex item) const;
    void ensureToggleIcons(nxui::Renderer& ren);
    bool drawToggleGlyph(nxui::Renderer& ren, ItemIndex idx, nxui::Vec2 center,
                         const nxui::Color& color, float edge) const;

    nxui::Font*        m_font = nullptr;
    nxui::Font*        m_smallFont = nullptr;
    nxui::Font*        m_iconFont = nullptr;
    const nxui::Theme* m_theme = nullptr;
    nxui::Input*       m_input = nullptr;
    Callbacks          m_callbacks;

    bool  m_active = false;
    bool  m_animating = false;
    float m_animProgress = 0.f; // 0.f = closed, 1.f = open

    ItemIndex   m_selectedItem = ItemIndex::Brightness;
    PowerAction m_selectedPower = PowerAction::Sleep;

    SelectionCursor m_cursor;

    // Offscreen backdrop blur caching
    bool m_backdropCacheValid = false;

    // Live state values
    float m_brightness = 0.5f;
    float m_bgmVolume = 0.5f;
    float m_sfxVolume = 0.5f;
    bool  m_airplaneMode = false;
    bool  m_wifiEnabled = true;
    bool  m_vibrationEnabled = true;
    bool  m_batterySaver = false;
    bool  m_frameTapLiteEngaged = false;

    // Leaf / airplane glyphs from the active icon pack (PoloNX); drawn
    // fallbacks when a pack has none.
    nxui::Texture m_iconLeaf;
    nxui::Texture m_iconAirplane;
    bool  m_toggleIconsLoadAttempted = false;
    std::function<std::string(const char*)> m_iconGlyphResolve;

    // Real-time battery & thermal status
    int   m_batteryPercent = -1;
    bool  m_batteryCharging = false;
    float m_socTemp = -1.f;
    float m_pcbTemp = -1.f;
    bool  m_hasSocTemp = false;
    bool  m_hasPcbTemp = false;
    float m_statusPollTimer = 0.f;

    // Touch interaction tracking
    bool      m_draggingSlider = false;
    ItemIndex m_draggedSlider = ItemIndex::Brightness;
};
