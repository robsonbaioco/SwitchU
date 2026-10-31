#include "SteamGridDbPickerScreen.hpp"

#include "themeshop/ThemeHttp.hpp"
#include "settings/SettingsGlassTuning.hpp"

#include <nxui/core/Input.hpp>
#include <nxui/core/Renderer.hpp>
#include <nxui/third_party/stb/stb_image.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

namespace {
// Picker panel and gallery geometry (PoloNX zoomable gallery).
constexpr float kPickerPanelX = 70.f;
constexpr float kPickerPanelY = 38.f;
constexpr float kPickerPanelW = 1140.f;
constexpr float kPickerPanelH = 644.f;
constexpr float kPickerGridPadX = 28.f;
constexpr float kPickerGridTop = 220.f;
constexpr float kPickerGridBottomPad = 58.f; // leave room for footer hints

float zoomScale(int level) {
    switch (std::clamp(level, 0, 4)) {
    case 0: return 0.72f;
    case 1: return 0.88f;
    case 2: return 1.00f;
    case 3: return 1.22f;
    default: return 1.48f;
    }
}
} // namespace

SteamGridDbPickerScreen::SteamGridDbPickerScreen(nxui::GpuDevice& gpu,
                                                 nxui::Renderer& renderer,
                                                 nxui::ThreadPool& threadPool)
    : m_gpu(gpu), m_renderer(renderer), m_threadPool(threadPool) {
    setRect({0.f, 0.f, 1280.f, 720.f});
    setVisible(false);
    setFocusable(true);
    setFrameworkTouchEnabled(false);

    auto addDirection = [this](nxui::Button button, int dx, int dy) {
        addAction(static_cast<std::uint64_t>(button), [this, dx, dy]() {
            moveSelection(dx, dy);
        });
    };
    addDirection(nxui::Button::DLeft, -1, 0);
    addDirection(nxui::Button::LStickL, -1, 0);
    addDirection(nxui::Button::DRight, 1, 0);
    addDirection(nxui::Button::LStickR, 1, 0);
    addDirection(nxui::Button::DUp, 0, -1);
    addDirection(nxui::Button::LStickU, 0, -1);
    addDirection(nxui::Button::DDown, 0, 1);
    addDirection(nxui::Button::LStickD, 0, 1);
    addAction(static_cast<std::uint64_t>(nxui::Button::A), [this]() { activateSelection(); });
    addAction(static_cast<std::uint64_t>(nxui::Button::X), [this]() {
        if (m_searchCb) m_searchCb();
    });
    addAction(static_cast<std::uint64_t>(nxui::Button::B), [this]() { hide(); });
    addAction(static_cast<std::uint64_t>(nxui::Button::ZL), [this]() { adjustZoom(-1); });
    addAction(static_cast<std::uint64_t>(nxui::Button::ZR), [this]() { adjustZoom(+1); });
}

void SteamGridDbPickerScreen::adjustZoom(int delta) {
    if (m_loading) return;
    const int previous = m_zoomLevel;
    m_zoomLevel = std::clamp(m_zoomLevel + delta, 0, 4);
    if (m_zoomLevel == previous) return;
    // Redecode at the new canvas size so zoom-in stays sharp.
    ++m_generation;
    schedulePreviews();
}

void SteamGridDbPickerScreen::previewCanvasSize(int& outW, int& outH) const {
    const float zoom = zoomScale(m_zoomLevel);
    using Kind = SteamGridDbManager::ArtworkKind;
    if (m_result.kind == Kind::Cover) { // == Poster
        outW = std::max(96, (int)(180.f * zoom));
        outH = std::max(144, (int)(270.f * zoom));
    } else if (m_result.kind == Kind::Icon) {
        outW = outH = std::max(96, (int)(180.f * zoom));
    } else if (m_result.kind == Kind::Logo) {
        outW = std::max(160, (int)(280.f * zoom));
        outH = std::max(48, (int)(80.f * zoom));
    } else {
        outW = std::max(160, (int)(280.f * zoom));
        outH = std::max(90, (int)(158.f * zoom));
    }
}

void SteamGridDbPickerScreen::cardMetrics(float& outW, float& outH, float& outGapX,
                                          float& outGapY, int& outColumns,
                                          int& outRows) const {
    const float zoom = zoomScale(m_zoomLevel);
    using Kind = SteamGridDbManager::ArtworkKind;
    outGapX = 14.f;
    outGapY = 12.f;
    if (m_result.kind == Kind::Cover) { // == Poster
        outColumns = m_zoomLevel >= 4 ? 2 : (m_zoomLevel >= 3 ? 3 : 4);
        outRows = m_zoomLevel >= 3 ? 1 : 2;
        outW = 168.f * zoom;
        outH = 252.f * zoom;
    } else if (m_result.kind == Kind::Icon) {
        outColumns = m_zoomLevel >= 4 ? 2 : (m_zoomLevel >= 3 ? 3 : 4);
        outRows = m_zoomLevel >= 3 ? 2 : 3;
        outW = outH = 168.f * zoom;
    } else if (m_result.kind == Kind::Logo) {
        outColumns = m_zoomLevel >= 4 ? 2 : (m_zoomLevel >= 3 ? 3 : 4);
        outRows = m_zoomLevel >= 3 ? 2 : 3;
        outW = 250.f * zoom;
        outH = 110.f * zoom;
    } else {
        outColumns = m_zoomLevel >= 4 ? 2 : (m_zoomLevel >= 3 ? 3 : 4);
        outRows = m_zoomLevel >= 3 ? 2 : 3;
        outW = 250.f * zoom;
        outH = 130.f * zoom;
    }

    // Fit inside the panel so zoom never spills past the rounded window.
    const float availW = kPickerPanelW - kPickerGridPadX * 2.f;
    const float availH = (kPickerPanelY + kPickerPanelH - kPickerGridBottomPad) - kPickerGridTop;
    if (outColumns > 1)
        outW = std::min(outW, (availW - outGapX * (outColumns - 1)) / outColumns);
    else
        outW = std::min(outW, availW);
    if (outRows > 1)
        outH = std::min(outH, (availH - outGapY * (outRows - 1)) / outRows);
    else
        outH = std::min(outH, availH);

    if (m_result.kind == Kind::Cover) {
        constexpr float kAspect = 2.f / 3.f; // w/h
        if (outW / std::max(1.f, outH) > kAspect)
            outW = outH * kAspect;
        else
            outH = outW / kAspect;
    } else if (m_result.kind == Kind::Icon) {
        const float side = std::min(outW, outH);
        outW = outH = side;
    }
}

int SteamGridDbPickerScreen::perPage() const {
    float w, h, gx, gy;
    int cols = 4, rows = 3;
    cardMetrics(w, h, gx, gy, cols, rows);
    return std::max(1, cols * rows);
}

int SteamGridDbPickerScreen::artworkCount() const {
    return static_cast<int>(std::min(m_result.candidates.size(), m_slots.size()));
}

void SteamGridDbPickerScreen::showLoading(std::uint64_t titleId,
                                          const std::string& title,
                                          const std::string& query,
                                          SteamGridDbManager::ArtworkKind kind) {
    ++m_generation;
    for (auto& slot : m_slots) {
        if (slot.state) slot.state->cancelled.store(true);
        slot.ready = false;
        slot.failed = false;
    }
    // Keep the search string across a re-search so X Search and the header
    // always show what was typed, even when a result payload omits it.
    if (!query.empty())
        m_searchQuery = query;
    m_result = {};
    m_result.titleId = titleId;
    m_result.title = title;
    m_result.query = m_searchQuery;
    m_result.kind = kind;
    m_selected = 0;
    m_loading = true;
    m_message = "Searching SteamGridDB...";
    m_backdropCacheValid = false;
    m_active = true;
    setVisible(true);
}

void SteamGridDbPickerScreen::setResult(BrowseResult result) {
    // Never drop the active search string when a payload omits it.
    if (!result.query.empty())
        m_searchQuery = result.query;
    else
        result.query = m_searchQuery;
    m_result = std::move(result);
    m_result.query = m_searchQuery;
    m_loading = false;
    m_selected = 0;
    m_message = m_result.success
        ? std::to_string(m_result.candidates.size()) + " choices for " + m_result.gameName
        : m_result.error;
    ++m_generation;
    schedulePreviews();
}

void SteamGridDbPickerScreen::setMessage(const std::string& message, bool loading) {
    m_message = message;
    m_loading = loading;
}

void SteamGridDbPickerScreen::hide() {
    if (!m_active) return;
    m_active = false;
    m_backdropCacheValid = false;
    m_searchQuery.clear();
    setVisible(false);
    for (auto& slot : m_slots)
        if (slot.state) slot.state->cancelled.store(true);
    if (m_closedCb) m_closedCb();
}

void SteamGridDbPickerScreen::wait() {
    for (auto& slot : m_slots) {
        if (slot.state) slot.state->cancelled.store(true);
        if (slot.future.valid()) slot.future.wait();
    }
    for (auto& future : m_retiredFutures)
        if (future.valid()) future.wait();
    m_retiredFutures.clear();
}

SteamGridDbPickerScreen::DecodedImage SteamGridDbPickerScreen::decodePreview(
    const std::vector<std::uint8_t>& bytes, int maxW, int maxH) {
    DecodedImage out;
    if (bytes.empty() || maxW <= 0 || maxH <= 0) return out;
    int width = 0, height = 0, channels = 0;
    stbi_uc* raw = stbi_load_from_memory(bytes.data(), static_cast<int>(bytes.size()),
                                         &width, &height, &channels, 4);
    if (!raw || width <= 0 || height <= 0) {
        if (raw) stbi_image_free(raw);
        return out;
    }
    // Contain into a kind-sized canvas so posters stay portrait and fill the slot.
    out.width = maxW;
    out.height = maxH;
    out.rgba.assign(static_cast<std::size_t>(out.width) * out.height * 4, 0);
    const float scale = std::min(static_cast<float>(out.width) / width,
                                 static_cast<float>(out.height) / height);
    const int drawnWidth = std::max(1, static_cast<int>(width * scale));
    const int drawnHeight = std::max(1, static_cast<int>(height * scale));
    const int offsetX = (out.width - drawnWidth) / 2;
    const int offsetY = (out.height - drawnHeight) / 2;
    for (int y = 0; y < drawnHeight; ++y) {
        const int sourceY = y * height / drawnHeight;
        for (int x = 0; x < drawnWidth; ++x) {
            const int sourceX = x * width / drawnWidth;
            std::memcpy(out.rgba.data()
                            + (static_cast<std::size_t>(y + offsetY) * out.width
                               + x + offsetX) * 4,
                        raw + (static_cast<std::size_t>(sourceY) * width + sourceX) * 4,
                        4);
        }
    }
    stbi_image_free(raw);
    return out;
}

void SteamGridDbPickerScreen::schedulePreviews() {
    const std::size_t count = std::min(m_result.candidates.size(), m_slots.size());
    for (std::size_t i = 0; i < m_slots.size(); ++i) {
        auto& slot = m_slots[i];
        if (slot.state) slot.state->cancelled.store(true);
        if (slot.future.valid())
            m_retiredFutures.push_back(std::move(slot.future));
        slot.state.reset();
        slot.ready = false;
        slot.failed = i >= count;
        if (i >= count) continue;
        slot.candidate = m_result.candidates[i];
    }
}

void SteamGridDbPickerScreen::schedulePreview(std::size_t index) {
    if (index >= m_slots.size()) return;
    auto& slot = m_slots[index];
    if (slot.ready || slot.failed || slot.future.valid() || slot.state) return;
    slot.generation = m_generation;
    slot.state = std::make_shared<DecodeState>();
    auto state = slot.state;
    // Always prefer SGDB thumbs for the gallery — full art is only fetched on Apply.
    const std::string url = slot.candidate.thumbnailUrl.empty()
        ? slot.candidate.url
        : slot.candidate.thumbnailUrl;
    int canvasW = 240, canvasH = 112;
    previewCanvasSize(canvasW, canvasH);
    slot.future = m_threadPool.submit([state, url, canvasW, canvasH]() {
        try {
            if (!state->cancelled.load())
                state->image = decodePreview(themeshop::http::getBytes(url),
                                             canvasW, canvasH);
        } catch (...) {
        }
    });
}

void SteamGridDbPickerScreen::onUpdate(float dt) {
    if (!m_active) return;
    m_spinner += dt;

    for (std::size_t i = 0; i < m_retiredFutures.size();) {
        if (m_retiredFutures[i].wait_for(std::chrono::seconds(0))
            != std::future_status::ready) {
            ++i;
            continue;
        }
        try { m_retiredFutures[i].get(); } catch (...) {}
        m_retiredFutures[i] = std::move(m_retiredFutures.back());
        m_retiredFutures.pop_back();
    }

    int uploads = 0;
    for (auto& slot : m_slots) {
        if (uploads >= 1 || slot.ready || slot.failed || !slot.future.valid()) continue;
        if (slot.future.wait_for(std::chrono::seconds(0)) != std::future_status::ready) continue;
        try { slot.future.get(); } catch (...) {}
        if (!slot.state || slot.state->cancelled.load() || slot.generation != m_generation) {
            slot.failed = true;
            continue;
        }
        auto& image = slot.state->image;
        slot.ready = !image.rgba.empty()
            && slot.texture.loadFromPixels(m_gpu, m_renderer, image.rgba.data(),
                                           image.width, image.height);
        slot.failed = !slot.ready;
        image.rgba.clear();
        ++uploads;
    }

    // Network/decode work is intentionally limited. Launching all eighteen
    // previews at once saturated the shared worker pool and caused audible
    // underruns while navigating the gallery.
    int activeJobs = 0;
    for (const auto& slot : m_slots)
        if (slot.future.valid()) ++activeJobs;
    const int count = artworkCount();
    const int pageSize = perPage();
    const int pageStart = count > 0 ? (m_selected / pageSize) * pageSize : 0;
    for (int offset = 0; offset < pageSize && activeJobs < 2; ++offset) {
        const int index = pageStart + offset;
        if (index >= count) break;
        auto& slot = m_slots[(size_t)index];
        if (!slot.ready && !slot.failed && !slot.future.valid() && !slot.state) {
            schedulePreview((size_t)index);
            ++activeJobs;
        }
    }
}

nxui::Rect SteamGridDbPickerScreen::containRect(const nxui::Texture& texture,
                                                const nxui::Rect& area) {
    if (!texture.valid()) return area;
    const float scale = std::min(area.width / texture.width(), area.height / texture.height());
    const float width = texture.width() * scale;
    const float height = texture.height() * scale;
    return {area.x + (area.width - width) * 0.5f,
            area.y + (area.height - height) * 0.5f, width, height};
}

std::string SteamGridDbPickerScreen::kindLabel() const {
    return m_result.kind == SteamGridDbManager::ArtworkKind::Hero ? "Hero"
         : m_result.kind == SteamGridDbManager::ArtworkKind::Logo ? "Logo"
         : m_result.kind == SteamGridDbManager::ArtworkKind::Cover ? "Cover" // == Poster
         : m_result.kind == SteamGridDbManager::ArtworkKind::GridWide ? "Poster wide"
         : "Icon";
}

void SteamGridDbPickerScreen::onRender(nxui::Renderer& renderer) {
    if (!m_active || !m_theme) return;

    // Translucent backdrop scrim matching default window style (Settings / Quick Settings / Dialogs)
    nxui::Rect screen = {0.f, 0.f, (float)renderer.width(), (float)renderer.height()};
    nxui::Color scrim = nxui::Color::lerp(m_theme->background, nxui::Color::black(),
                                          m_theme->mode == nxui::ThemeMode::Dark ? 0.72f : 0.28f)
        .withAlpha(m_theme->mode == nxui::ThemeMode::Dark ? 0.25f : 0.14f);
    renderer.drawRect(screen, scrim);

    const nxui::Rect panel{70.f, 38.f, 1140.f, 644.f};
    constexpr float kPanelRadius = 28.f;

    const auto& tuning = settings::debug::settingsGlassTuning();
    bool needsBackdropRefresh = !m_backdropCacheValid
        || std::abs(m_cachedPreBlurRadius - tuning.preBlurRadius) > 0.001f
        || m_cachedBlurIterations != tuning.blurIterations;

    if (needsBackdropRefresh) {
        renderer.captureToOffscreenSharp();
        if (tuning.blurIterations > 0 && tuning.preBlurRadius > 0.001f) {
            renderer.applyBlur(tuning.preBlurRadius, tuning.blurIterations);
        }
        renderer.copyOffscreen(nxui::GpuDevice::OFF_SHARP_A, kBackdropCacheTarget);
        m_backdropCacheValid = true;
        m_cachedPreBlurRadius = tuning.preBlurRadius;
        m_cachedBlurIterations = tuning.blurIterations;
    }

    nxui::LiquidGlassSettings savedGlass = renderer.liquidGlassSettings();
    auto& glass = renderer.liquidGlassSettings();
    glass.refractionIntensity = std::clamp(tuning.refractionIntensity, 0.0f, 1.5f);
    glass.blurIntensity = std::max(0.0f, tuning.shaderBlurIntensity);
    glass.noiseIntensity = 0.0f;
    glass.glowIntensity = std::max(0.0f, tuning.glowIntensity);
    glass.saturation = std::max(0.0f, tuning.saturation);
    glass.opacityMultiplier = 1.0f;
    glass.roughness = std::max(0.0f, tuning.roughness);
    glass.powerFactor = std::max(1.001f, tuning.powerFactor);

    nxui::Color glassTint = m_theme->panelBase.withAlpha(m_theme->mode == nxui::ThemeMode::Dark
        ? std::clamp(tuning.tintAlphaDark, 0.0f, 1.0f)
        : std::clamp(tuning.tintAlphaLight, 0.0f, 1.0f));
    nxui::Rect glassRect = panel.shrunk(std::max(0.0f, tuning.inset));
    float glassRadius = std::max(12.0f, kPanelRadius - std::max(0.0f, tuning.inset) * 0.5f);

    renderer.drawLiquidGlass(kBackdropCacheTarget,
                             glassRect,
                             glassRadius,
                             glassTint,
                             1.0f,
                             std::clamp(tuning.shade, 0.0f, 1.0f));
    renderer.drawRoundedRectOutline(glassRect,
                                   m_theme->panelBorder.withAlpha(std::clamp(m_theme->panelBorder.a * 0.90f, 0.14f, 0.34f)),
                                   glassRadius,
                                   1.2f);
    renderer.drawRoundedRectOutline(glassRect.shrunk(1.5f),
                                   m_theme->panelHighlight.withAlpha(std::clamp(m_theme->panelHighlight.a * 0.90f, 0.04f, 0.10f)),
                                   std::max(0.0f, glassRadius - 1.5f),
                                   1.0f);
    renderer.liquidGlassSettings() = savedGlass;

    if (m_font)
        renderer.drawText("SteamGridDB - " + kindLabel(), {108.f, 68.f}, m_font,
                          m_theme->textPrimary, 1.f);
    if (m_smallFont) {
        renderer.drawText(m_result.title, {108.f, 110.f}, m_smallFont,
                          m_theme->textSecondary, 0.78f);
        renderer.drawText("Search: " + m_searchQuery + "   [X] Change name",
                          {108.f, 139.f}, m_smallFont, m_theme->textSecondary, 0.72f);
        renderer.drawText(m_message, {108.f, 169.f}, m_smallFont,
                          m_theme->textPrimary, 0.72f);
    }

    if (m_loading) {
        const float pulse = 0.45f + 0.35f * std::sin(m_spinner * 5.f);
        renderer.drawRoundedRect({570.f, 340.f, 140.f, 12.f},
                                 m_theme->cursorNormal.withAlpha(pulse), 6.f);
        return;
    }

    const int count = artworkCount();
    float cardW = 250.f, cardH = 130.f, gapX = 20.f, gapY = 18.f;
    int columns = 4, rows = 3;
    cardMetrics(cardW, cardH, gapX, gapY, columns, rows);
    const int pageSize = std::max(1, columns * rows);
    const int page = count > 0 ? m_selected / pageSize : 0;
    const int start = page * pageSize;
    const float gridW = columns * cardW + (columns - 1) * gapX;
    const float gridLeft = kPickerPanelX + kPickerGridPadX;
    const float gridAvailW = kPickerPanelW - kPickerGridPadX * 2.f;
    const float originX = gridLeft + std::max(0.f, (gridAvailW - gridW) * 0.5f);
    const float originY = kPickerGridTop;
    // Mask the gallery to the panel interior (focus ring included).
    const nxui::Rect clip{
        kPickerPanelX + 12.f,
        kPickerGridTop - 8.f,
        kPickerPanelW - 24.f,
        (kPickerPanelY + kPickerPanelH - kPickerGridBottomPad) - (kPickerGridTop - 8.f)};
    renderer.pushClipRect(clip);
    for (int local = 0; local < pageSize; ++local) {
        const int index = start + local;
        const nxui::Rect card{originX + (local % columns) * (cardW + gapX),
                              originY + (local / columns) * (cardH + gapY), cardW, cardH};
        if (local < (int)m_visibleRects.size())
            m_visibleRects[(size_t)local] = card;
        if (index >= count) continue;
        renderer.drawRoundedRect(card, m_theme->panelBorder.withAlpha(0.20f), 16.f);
        renderer.drawRoundedRectOutline(card, m_theme->panelBorder.withAlpha(0.35f), 16.f, 1.f);
        const auto& slot = m_slots[(size_t)index];
        if (slot.ready && slot.texture.valid()) {
            const nxui::Rect imageArea = card.shrunk(6.f);
            renderer.drawTextureRounded(&slot.texture, containRect(slot.texture, imageArea), 10.f,
                                 nxui::Color::white());
        } else {
            renderer.drawRoundedRect(card.shrunk(12.f),
                                     m_theme->panelHighlight.withAlpha(0.08f), 12.f);
        }
        if (index == m_selected)
            renderer.drawRoundedRectOutline(card.expanded(3.f), m_theme->cursorNormal,
                                            18.f, 3.f);
    }
    renderer.popClipRect();
    if (m_smallFont && count > 0) {
        const int pages = (count + pageSize - 1) / pageSize;
        renderer.drawText("A Apply   B Back   X Search   ZL/ZR Zoom "
                          + std::to_string(m_zoomLevel + 1) + "/5   Page "
                          + std::to_string(page + 1) + "/" + std::to_string(pages),
                          {108.f, 651.f}, m_smallFont, m_theme->textSecondary, 0.72f);
    }
}

void SteamGridDbPickerScreen::moveSelection(int dx, int dy) {
    const int count = artworkCount();
    if (count <= 0 || m_loading) return;
    float cardW, cardH, gapX, gapY;
    int columns = 4, rows = 3;
    cardMetrics(cardW, cardH, gapX, gapY, columns, rows);
    int target = m_selected + dx + dy * columns;
    if (target >= 0 && target < count) m_selected = target;
}

void SteamGridDbPickerScreen::activateSelection() {
    const int count = static_cast<int>(std::min(m_result.candidates.size(), m_slots.size()));
    if (m_loading || m_selected < 0 || m_selected >= count) return;
    m_loading = true;
    m_message = "Applying selection...";
    if (m_applyCb) m_applyCb(m_result, m_result.candidates[(size_t)m_selected]);
}

void SteamGridDbPickerScreen::handleTouch(nxui::Input& input) {
    if (!m_active || m_loading || !input.touchUp()) return;
    const int count = artworkCount();
    const int pageSize = perPage();
    const int page = count > 0 ? m_selected / pageSize : 0;
    for (int local = 0; local < pageSize; ++local) {
        const int index = page * pageSize + local;
        if (index >= count || local >= (int)m_visibleRects.size()
            || !m_visibleRects[(size_t)local].contains(input.touchX(), input.touchY()))
            continue;
        if (m_selected == index) activateSelection();
        else m_selected = index;
        return;
    }
}
