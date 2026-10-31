#include "ThemeShopScreen.hpp"

#include "ThemeShopLayout.hpp"
#include "widgets/ActionButtonStyle.hpp"
#include "settings/SettingsGlassTuning.hpp"

#include "core/DebugLog.hpp"

#include <nxui/core/I18n.hpp>
#include <nxui/core/Renderer.hpp>
#include <nxui/core/ThreadPool.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cmath>
#include <filesystem>
#include <functional>
#include <system_error>
#include <vector>
#include <unordered_map>
#include <utility>

namespace {

// eShop-inspired shelf: wider store row, cover-first (art owns the card).
// Column/row counts live in ThemeShopLayout.hpp (shared with preview prefetch).
constexpr int kGridCols = themeshop::layout::kGridCols;
constexpr int kVisibleRows = themeshop::layout::kVisibleRows;
constexpr float kContentInsetX = 22.f;
constexpr float kContentInsetY = 12.f;
constexpr float kHeaderHeight = 58.f;
constexpr float kGridGapX = 16.f;
constexpr float kGridGapY = 16.f;
constexpr float kFooterHintHeight = 64.f;
constexpr float kCardCaptionH = 48.f;
constexpr float kPreviewAspect = 16.f / 9.f;
constexpr float kSearchButtonWidth = 208.f;
constexpr float kRefreshButtonWidth = 144.f;
constexpr float kHeaderButtonGap = 12.f;
// Match parent tab pitch (kTabRowHeight) so the Shop pill is the same size.
constexpr float kShopBranchRow = 58.f;
constexpr int kEntriesPerPage = themeshop::layout::kEntriesPerPage;
constexpr float kPageButtonWidth = 124.f;
constexpr float kPageCounterWidth = 96.f;
constexpr float kActionButtonRadius = 16.f;
constexpr int kThemeShopBackdropCacheTarget = 2;
constexpr size_t kTextMeasureCacheLimit = 1024;
constexpr size_t kEllipsizeCacheLimit = 1024;

std::unordered_map<std::string, nxui::Vec2> g_textMeasureCache;
std::unordered_map<std::string, std::string> g_ellipsizeCache;

std::string measureCacheKey(nxui::Font* font, const std::string& text) {
    return std::to_string((std::uintptr_t)font) + "\n"
         + std::to_string(font ? font->revision() : 0) + "\n"
         + text;
}

std::string ellipsizeCacheKey(nxui::Font* font, const std::string& text, float maxWidth, float scale) {
    return std::to_string((std::uintptr_t)font)
         + "\n" + std::to_string(font ? font->revision() : 0)
        + "\n" + std::to_string((int)std::lround(maxWidth * 4.f))
        + "\n" + std::to_string((int)std::lround(scale * 1000.f))
        + "\n" + text;
}

nxui::Vec2 measureTextCached(nxui::Font* font, const std::string& text) {
    if (!font || text.empty())
        return font ? font->measure(text) : nxui::Vec2{};

    std::string key = measureCacheKey(font, text);
    auto it = g_textMeasureCache.find(key);
    if (it != g_textMeasureCache.end())
        return it->second;

    nxui::Vec2 size = font->measure(text);
    if (g_textMeasureCache.size() >= kTextMeasureCacheLimit)
        g_textMeasureCache.clear();
    g_textMeasureCache.emplace(std::move(key), size);
    return size;
}

struct GridLayout {
    nxui::Rect header;
    nxui::Rect searchButton;
    nxui::Rect refreshButton;
    nxui::Rect pagePrevButton;
    nxui::Rect pageCounter;
    nxui::Rect pageNextButton;
    nxui::Rect grid;
    float cardW = 0.f;
    float cardH = 0.f;
};

GridLayout makeGridLayout(const nxui::Rect& content, bool showRefresh) {
    GridLayout layout;
    layout.header = {
        content.x + kContentInsetX,
        content.y + kContentInsetY,
        content.width - kContentInsetX * 2.f,
        kHeaderHeight
    };
    layout.searchButton = {
        layout.header.right() - kSearchButtonWidth,
        layout.header.y + 10.f,
        kSearchButtonWidth,
        46.f
    };
    float cursorX = layout.searchButton.x;
    if (showRefresh) {
        cursorX -= kHeaderButtonGap + kRefreshButtonWidth;
        layout.refreshButton = {
            cursorX,
            layout.header.y + 10.f,
            kRefreshButtonWidth,
            46.f
        };
    } else {
        layout.refreshButton = {cursorX, layout.header.y + 10.f, 0.f, 46.f};
    }
    layout.grid = {
        content.x + kContentInsetX,
        layout.header.bottom() + 12.f,
        content.width - kContentInsetX * 2.f,
        content.height - (layout.header.bottom() - content.y) - 12.f - kFooterHintHeight
    };
    const float pageControlsW = kPageButtonWidth * 2.f + kPageCounterWidth + kHeaderButtonGap * 2.f;
    const float pageControlsX = layout.grid.x + std::max(0.f, (layout.grid.width - pageControlsW) * 0.5f);
    const float pageControlsY = layout.grid.bottom() + 8.f;
    layout.pagePrevButton = {
        pageControlsX,
        pageControlsY,
        kPageButtonWidth,
        46.f
    };
    layout.pageCounter = {
        layout.pagePrevButton.right() + kHeaderButtonGap,
        pageControlsY,
        kPageCounterWidth,
        46.f
    };
    layout.pageNextButton = {
        layout.pageCounter.right() + kHeaderButtonGap,
        pageControlsY,
        kPageButtonWidth,
        46.f
    };
    layout.cardW = (layout.grid.width - kGridGapX * (kGridCols - 1)) / (float)kGridCols;
    layout.cardH = (layout.grid.height - kGridGapY * (kVisibleRows - 1)) / (float)kVisibleRows;
    return layout;
}

nxui::Rect gridCardRect(const GridLayout& layout, int localIndex) {
    int col = localIndex % kGridCols;
    int row = localIndex / kGridCols;
    return {
        layout.grid.x + col * (layout.cardW + kGridGapX),
        layout.grid.y + row * (layout.cardH + kGridGapY),
        layout.cardW,
        layout.cardH
    };
}

nxui::Rect detailDialogRect(const nxui::Rect& content) {
    // Product sheet: closer to full content — eShop product page, not a modal card.
    return {
        content.x + 22.f,
        content.y + 24.f,
        content.width - 44.f,
        content.height - 52.f
    };
}

std::vector<nxui::Rect> detailButtonRects(const nxui::Rect& dialog, int count) {
    std::vector<nxui::Rect> rects;
    if (count <= 0)
        return rects;

    float gap = 14.f;
    float width = count == 1 ? 258.f : 246.f;
    float totalWidth = count * width + (count - 1) * gap;
    float x = dialog.right() - 30.f - totalWidth;
    float y = dialog.bottom() - 68.f;
    rects.reserve((size_t)count);
    for (int i = 0; i < count; ++i) {
        rects.push_back({x + i * (width + gap), y, width, 46.f});
    }
    return rects;
}

std::vector<nxui::Rect> headerButtonRects(const GridLayout& layout, bool showRefresh) {
    std::vector<nxui::Rect> rects;
    if (showRefresh && layout.refreshButton.width > 0.f)
        rects.push_back(layout.refreshButton);
    rects.push_back(layout.searchButton);
    return rects;
}

std::string ellipsize(nxui::Font* font, const std::string& text, float maxWidth, float scale) {
    if (!font || text.empty() || maxWidth <= 0.f)
        return text;

    std::string cacheKey = ellipsizeCacheKey(font, text, maxWidth, scale);
    auto cached = g_ellipsizeCache.find(cacheKey);
    if (cached != g_ellipsizeCache.end())
        return cached->second;

    if (measureTextCached(font, text).x * scale <= maxWidth)
        return text;

    std::string out = text;
    while (!out.empty() && measureTextCached(font, out + "...").x * scale > maxWidth)
        out.pop_back();

    std::string result = out.empty() ? text : out + "...";
    if (g_ellipsizeCache.size() >= kEllipsizeCacheLimit)
        g_ellipsizeCache.clear();
    g_ellipsizeCache.emplace(std::move(cacheKey), result);
    return result;
}

nxui::Rect fitAspectRect(const nxui::Rect& bounds, float aspect) {
    if (bounds.width <= 0.f || bounds.height <= 0.f || aspect <= 0.f)
        return bounds;

    float width = std::min(bounds.width, bounds.height * aspect);
    float height = width / aspect;
    if (height > bounds.height) {
        height = bounds.height;
        width = height * aspect;
    }

    return {
        bounds.x + (bounds.width - width) * 0.5f,
        bounds.y + (bounds.height - height) * 0.5f,
        width,
        height
    };
}

nxui::Rect fitAspectRectTop(const nxui::Rect& bounds, float aspect) {
    if (bounds.width <= 0.f || bounds.height <= 0.f || aspect <= 0.f)
        return bounds;

    float width = std::min(bounds.width, bounds.height * aspect);
    float height = width / aspect;
    if (height > bounds.height) {
        height = bounds.height;
        width = height * aspect;
    }

    return {
        bounds.x + (bounds.width - width) * 0.5f,
        bounds.y,
        width,
        height
    };
}

nxui::Rect detailPreviewRect(const nxui::Rect& dialog) {
    // Leave a title band above the 16:9 screenshot (full-width name).
    // Preview sits on the right; meta text owns the left column.
    // Top-align so the shot sits under the title (no vertical centering gap).
    constexpr float kBodyTopInset = 78.f;
    constexpr float kBodyBottomInset = 92.f;
    const float previewW = dialog.width * 0.58f;
    nxui::Rect bounds = {
        dialog.right() - 28.f - previewW,
        dialog.y + kBodyTopInset,
        previewW,
        dialog.height - kBodyTopInset - kBodyBottomInset
    };
    return fitAspectRectTop(bounds, kPreviewAspect);
}

std::vector<std::string> wrapTextLines(nxui::Font* font,
                                       const std::string& text,
                                       float maxWidth,
                                       float scale,
                                       int maxLines) {
    std::vector<std::string> lines;
    if (!font || text.empty() || maxWidth <= 0.f || maxLines <= 0)
        return lines;

    auto fits = [&](const std::string& s) {
        return measureTextCached(font, s).x * scale <= maxWidth;
    };

    std::string remaining = text;
    while (!remaining.empty() && (int)lines.size() < maxLines) {
        if (fits(remaining) || (int)lines.size() + 1 >= maxLines) {
            if (!fits(remaining) && (int)lines.size() + 1 >= maxLines) {
                std::string clipped = remaining;
                while (!clipped.empty() && !fits(clipped + "..."))
                    clipped.pop_back();
                lines.push_back(clipped.empty() ? "..." : clipped + "...");
            } else {
                lines.push_back(remaining);
            }
            break;
        }

        // Prefer breaking on spaces; fall back to hard cut.
        std::string candidate;
        std::string lastFit;
        for (std::size_t i = 0; i < remaining.size(); ++i) {
            candidate.push_back(remaining[i]);
            if (fits(candidate)) {
                if (remaining[i] == ' ')
                    lastFit = candidate;
            } else {
                break;
            }
        }
        if (lastFit.empty()) {
            // Single long token — hard-cut to width.
            candidate.clear();
            for (char c : remaining) {
                candidate.push_back(c);
                if (!fits(candidate)) {
                    candidate.pop_back();
                    break;
                }
            }
            if (candidate.empty())
                candidate = remaining.substr(0, 1);
            lines.push_back(candidate);
            remaining = remaining.substr(candidate.size());
            while (!remaining.empty() && remaining.front() == ' ')
                remaining.erase(remaining.begin());
            continue;
        }
        while (!lastFit.empty() && lastFit.back() == ' ')
            lastFit.pop_back();
        lines.push_back(lastFit);
        remaining = remaining.substr(lastFit.size());
        while (!remaining.empty() && remaining.front() == ' ')
            remaining.erase(remaining.begin());
    }
    return lines;
}

float drawWrappedText(nxui::Renderer& ren,
                      nxui::Font* font,
                      const std::string& text,
                      nxui::Vec2 origin,
                      float maxWidth,
                      const nxui::Color& color,
                      float scale,
                      float lineGap,
                      int maxLines) {
    if (!font || text.empty())
        return 0.f;
    const auto lines = wrapTextLines(font, text, maxWidth, scale, maxLines);
    float y = origin.y;
    float total = 0.f;
    for (const auto& line : lines) {
        const float h = measureTextCached(font, line).y * scale;
        ren.drawText(line, {origin.x, y}, font, color, scale);
        y += h + lineGap;
        total += h + lineGap;
    }
    if (total > 0.f)
        total -= lineGap;
    return total;
}

struct DetailPreviewControls {
    nxui::Rect prev;
    nxui::Rect counter;
    nxui::Rect next;
};

DetailPreviewControls detailPreviewControls(const nxui::Rect& preview) {
    DetailPreviewControls controls;
    float gap = 10.f;
    float rowY = preview.bottom() + 18.f;
    float rowH = 42.f;
    float prevW = 118.f;
    float counterW = 82.f;
    float nextW = 118.f;
    float totalW = prevW + counterW + nextW + gap * 2.f;
    float startX = preview.x + std::max(0.f, (preview.width - totalW) * 0.5f);

    controls.prev = {startX, rowY, prevW, rowH};
    controls.counter = {controls.prev.right() + gap, rowY, counterW, rowH};
    controls.next = {controls.counter.right() + gap, rowY, nextW, rowH};
    return controls;
}

std::vector<nxui::Rect> detailPreviewControlRects(const DetailPreviewControls& controls,
                                                 const nxui::Rect& preview,
                                                 int screenshotCount) {
    std::vector<nxui::Rect> rects;
    if (screenshotCount <= 0)
        return rects;
    if (screenshotCount > 1)
        rects.push_back(controls.prev);
    rects.push_back(preview);
    if (screenshotCount > 1)
        rects.push_back(controls.next);
    return rects;
}

nxui::Rect lerpRect(const nxui::Rect& from, const nxui::Rect& to, float t) {
    return {
        from.x + (to.x - from.x) * t,
        from.y + (to.y - from.y) * t,
        from.width + (to.width - from.width) * t,
        from.height + (to.height - from.height) * t,
    };
}

nxui::Rect scaledRect(const nxui::Rect& rect, float scale) {
    if (std::abs(scale - 1.f) <= 0.001f) {
        return rect;
    }

    float width = rect.width * scale;
    float height = rect.height * scale;
    return {
        rect.x + (rect.width - width) * 0.5f,
        rect.y + (rect.height - height) * 0.5f,
        width,
        height
    };
}

struct FullscreenOverlayLayout {
    nxui::Rect preview;
    nxui::Rect prev;
    nxui::Rect next;
    nxui::Rect close;
    nxui::Rect counter;
};

FullscreenOverlayLayout makeFullscreenOverlayLayout(const nxui::Rect& content) {
    FullscreenOverlayLayout layout;
    nxui::Rect inner = content.shrunk(18.f);
    nxui::Rect previewBounds = {
        inner.x + 44.f,
        inner.y + 44.f,
        inner.width - 88.f,
        inner.height - 88.f
    };
    layout.preview = fitAspectRect(previewBounds, kPreviewAspect);
    layout.prev = {inner.x + 16.f, layout.preview.y + layout.preview.height * 0.5f - 24.f, 92.f, 48.f};
    layout.next = {inner.right() - 108.f, layout.preview.y + layout.preview.height * 0.5f - 24.f, 92.f, 48.f};
    layout.close = {inner.right() - 128.f, inner.y + 18.f, 112.f, 40.f};
    layout.counter = {inner.x + (inner.width - 84.f) * 0.5f, inner.y + 18.f, 84.f, 40.f};
    return layout;
}

nxui::Rect fitTextureRect(const nxui::Rect& bounds, const nxui::Texture* texture) {
    if (!texture || texture->width() <= 0 || texture->height() <= 0)
        return bounds;

    float texW = (float)texture->width();
    float texH = (float)texture->height();
    float scale = std::min(bounds.width / texW, bounds.height / texH);
    float width = texW * scale;
    float height = texH * scale;
    return {
        bounds.x + (bounds.width - width) * 0.5f,
        bounds.y + (bounds.height - height) * 0.5f,
        width,
        height
    };
}

void drawSpinner(nxui::Renderer& ren, const nxui::Vec2& center, float radius,
                 float time, const nxui::Color& color, float opacity) {
    constexpr int kSegments = 12;
    for (int i = 0; i < kSegments; ++i) {
        float angle = ((float)i / (float)kSegments) * 6.2831853f + time * 5.0f;
        float lead = std::fmod((float)i + time * 12.f, (float)kSegments) / (float)kSegments;
        float alpha = std::clamp(0.12f + lead * 0.88f, 0.f, 1.f) * opacity;
        nxui::Vec2 from = {center.x + std::cos(angle) * (radius - 7.f), center.y + std::sin(angle) * (radius - 7.f)};
        nxui::Vec2 to = {center.x + std::cos(angle) * radius, center.y + std::sin(angle) * radius};
        ren.drawLine(from, to, color.withAlpha(alpha), 3.f);
    }
}

// GameCube-ish tumble: diagonal roll + GIF-style breathe + soft ghost trail.
void drawCatalogLoadingCubePose(nxui::Renderer& ren,
                                const nxui::Vec2& center,
                                float size,
                                float yaw,
                                float pitch,
                                float roll,
                                const nxui::Color& color,
                                float opacity,
                                float lineScale = 1.f) {
    const float cy = std::cos(yaw), sy = std::sin(yaw);
    const float cp = std::cos(pitch), sp = std::sin(pitch);
    const float cr = std::cos(roll), sr = std::sin(roll);
    const float s = size * 0.40f;

    nxui::Vec2 projected[8];
    float depth[8];
    int nearest = 0;
    for (int i = 0; i < 8; ++i) {
        float x = (i & 1) ? 1.f : -1.f;
        float y = (i & 2) ? 1.f : -1.f;
        float z = (i & 4) ? 1.f : -1.f;
        // Roll → pitch → yaw (tumble over a corner).
        float x0 = x * cr - y * sr;
        float y0 = x * sr + y * cr;
        float y1 = y0 * cp - z * sp;
        float z1 = y0 * sp + z * cp;
        float x2 = x0 * cy + z1 * sy;
        float z2 = -x0 * sy + z1 * cy;
        depth[i] = z2;
        if (z2 > depth[nearest])
            nearest = i;
        projected[i] = {
            center.x + (x2 - z2) * s * 0.72f,
            center.y + (y1 * 0.92f + (x2 + z2) * 0.38f) * s * 0.72f
        };
    }

    struct Edge { int a; int b; float z; };
    Edge edges[12] = {
        {0, 1, 0}, {1, 3, 0}, {3, 2, 0}, {2, 0, 0},
        {4, 5, 0}, {5, 7, 0}, {7, 6, 0}, {6, 4, 0},
        {0, 4, 0}, {1, 5, 0}, {2, 6, 0}, {3, 7, 0},
    };
    for (auto& e : edges)
        e.z = 0.5f * (depth[e.a] + depth[e.b]);
    std::sort(edges, edges + 12, [](const Edge& lhs, const Edge& rhs) {
        return lhs.z < rhs.z;
    });

    const float zMin = edges[0].z;
    const float zMax = edges[11].z;
    const float zSpan = std::max(0.001f, zMax - zMin);

    for (const auto& e : edges) {
        const nxui::Vec2& a = projected[e.a];
        const nxui::Vec2& b = projected[e.b];
        const float t = (e.z - zMin) / zSpan;
        const bool touchesNear = (e.a == nearest || e.b == nearest);
        const float edgeAlpha = (0.28f + 0.72f * t) * opacity * (touchesNear ? 1.f : 0.82f);
        const float thick = (touchesNear ? 2.85f : 2.2f) * lineScale;
        ren.drawLine(a, b, nxui::Color(0.f, 0.f, 0.f, 0.20f * edgeAlpha), thick + 1.6f);
        ren.drawLine(a, b, color.withAlpha(edgeAlpha), thick);
    }

    // Soft highlight on the nearest corner (logo “apex” energy).
    if (opacity > 0.15f) {
        ren.drawCircle(projected[nearest], 2.6f * lineScale,
                       color.withAlpha(0.55f * opacity), 12);
    }
}

void drawCatalogLoadingCube(nxui::Renderer& ren,
                            const nxui::Vec2& center,
                            float size,
                            float time,
                            const nxui::Color& color,
                            float opacity) {
    // Slightly snappier breathe than the first tumble pass.
    const float breathe = 0.5f + 0.5f * std::sin(time * 2.65f);
    const float eased = breathe * breathe * (3.f - 2.f * breathe);
    const float pulse = 0.78f + 0.42f * eased;

    // Continuous corner-over-corner tumble (~25% faster).
    const float yaw = time * 1.35f;
    const float pitch = 0.55f + time * 0.92f;
    const float roll = time * 0.62f;

    // Ghost trail — slightly smaller / earlier pose.
    drawCatalogLoadingCubePose(ren, center, size * pulse * 0.96f,
                               yaw - 0.22f, pitch - 0.16f, roll - 0.12f,
                               color, opacity * 0.20f, 0.85f);
    drawCatalogLoadingCubePose(ren, center, size * pulse * 0.98f,
                               yaw - 0.10f, pitch - 0.07f, roll - 0.05f,
                               color, opacity * 0.38f, 0.92f);
    drawCatalogLoadingCubePose(ren, center, size * pulse,
                               yaw, pitch, roll,
                               color, opacity, 1.f);
}

void drawChip(nxui::Renderer& ren, nxui::Font* font, const nxui::Rect& rect,
              const std::string& text, const nxui::Color& fill, const nxui::Color& border,
              const nxui::Color& textColor, float opacity, float scale = 0.72f) {
    float borderWidth = rect.height >= 42.f ? 1.8f : (rect.height >= 34.f ? 1.5f : 1.3f);
    ren.drawRoundedRect(rect, fill.withAlpha(fill.a * opacity), rect.height * 0.5f);
    ren.drawRoundedRectOutline(rect, border.withAlpha(border.a * opacity), rect.height * 0.5f, borderWidth);
    if (font) {
        std::string fitted = ellipsize(font, text, rect.width - 16.f, scale);
        nxui::Vec2 size = measureTextCached(font, fitted);
        ren.drawText(fitted,
                     {rect.x + (rect.width - size.x * scale) * 0.5f,
                      rect.y + (rect.height - size.y * scale) * 0.5f},
                     font,
                     textColor.withAlpha(opacity),
                     scale);
    }
}

enum class ShelfBadgeKind { Icons, Music, Sounds };
enum class ShelfBadgeStyle { Icons, Music, Sounds, Tint, Auto };

struct ShelfBadgeColors {
    nxui::Color fill;
    nxui::Color border;
    nxui::Color ink;
};

// Dark: muted fills + light ink. Light: brighter fills + pure black ink so
// caption/detail/preview chips stay readable over panels and cover art.
ShelfBadgeColors shelfBadgeColors(ShelfBadgeStyle style, bool darkTheme) {
    const nxui::Color lightInk(0.f, 0.f, 0.f, 1.f);
    const nxui::Color darkInk(0.96f, 0.97f, 1.f, 1.f);
    if (darkTheme) {
        switch (style) {
        case ShelfBadgeStyle::Icons:
            return {nxui::Color(0.12f, 0.28f, 0.48f, 0.38f),
                    nxui::Color(0.45f, 0.72f, 1.f, 0.48f), darkInk};
        case ShelfBadgeStyle::Music:
            return {nxui::Color(0.32f, 0.16f, 0.28f, 0.40f),
                    nxui::Color(0.95f, 0.55f, 0.78f, 0.50f), darkInk};
        case ShelfBadgeStyle::Sounds:
            return {nxui::Color(0.18f, 0.30f, 0.16f, 0.40f),
                    nxui::Color(0.55f, 0.90f, 0.48f, 0.50f), darkInk};
        case ShelfBadgeStyle::Tint:
            return {nxui::Color(0.28f, 0.18f, 0.42f, 0.42f),
                    nxui::Color(0.82f, 0.55f, 1.f, 0.55f), darkInk};
        case ShelfBadgeStyle::Auto:
            return {nxui::Color(0.12f, 0.28f, 0.48f, 0.40f),
                    nxui::Color(0.45f, 0.78f, 1.f, 0.55f), darkInk};
        }
    }
    switch (style) {
    case ShelfBadgeStyle::Icons:
        return {nxui::Color(0.52f, 0.78f, 1.f, 0.90f),
                nxui::Color(0.12f, 0.40f, 0.82f, 0.88f), lightInk};
    case ShelfBadgeStyle::Music:
        return {nxui::Color(0.98f, 0.68f, 0.84f, 0.92f),
                nxui::Color(0.72f, 0.22f, 0.50f, 0.88f), lightInk};
    case ShelfBadgeStyle::Sounds:
        return {nxui::Color(0.62f, 0.92f, 0.58f, 0.92f),
                nxui::Color(0.16f, 0.52f, 0.22f, 0.88f), lightInk};
    case ShelfBadgeStyle::Tint:
        return {nxui::Color(0.86f, 0.72f, 1.f, 0.92f),
                nxui::Color(0.48f, 0.22f, 0.82f, 0.88f), lightInk};
    case ShelfBadgeStyle::Auto:
        return {nxui::Color(0.52f, 0.78f, 1.f, 0.90f),
                nxui::Color(0.12f, 0.40f, 0.82f, 0.88f), lightInk};
    }
    return {nxui::Color(0.75f, 0.78f, 0.82f, 0.90f),
            nxui::Color(0.35f, 0.40f, 0.48f, 0.80f), lightInk};
}

void drawShelfBadgeGlyph(nxui::Renderer& ren, ShelfBadgeKind kind,
                         nxui::Vec2 center, const nxui::Color& color) {
    const float s = 1.f;
    if (kind == ShelfBadgeKind::Icons) {
        // 2×2 tile grid — reads as "icons" at chip size.
        const float cell = 3.2f * s;
        const float gap = 1.6f * s;
        const float originX = center.x - cell - gap * 0.5f;
        const float originY = center.y - cell - gap * 0.5f;
        for (int row = 0; row < 2; ++row) {
            for (int col = 0; col < 2; ++col) {
                ren.drawRoundedRect(
                    {originX + col * (cell + gap), originY + row * (cell + gap), cell, cell},
                    color, 1.1f);
            }
        }
        return;
    }
    if (kind == ShelfBadgeKind::Music) {
        // Note head + stem (QS music-family weight).
        ren.drawCircle({center.x - 2.2f * s, center.y + 2.8f * s}, 2.6f * s, color);
        ren.drawLine({center.x + 0.2f * s, center.y + 2.8f * s},
                     {center.x + 0.2f * s, center.y - 5.5f * s}, color, 1.7f * s);
        ren.drawLine({center.x + 0.2f * s, center.y - 5.5f * s},
                     {center.x + 4.5f * s, center.y - 3.8f * s}, color, 1.5f * s);
        return;
    }
    // Sounds — mute speaker (body only; waves clipped the chip outline).
    const float cx = center.x - 0.6f * s;
    ren.drawLine({cx - 3.6f * s, center.y - 2.0f * s},
                 {cx - 1.2f * s, center.y - 2.0f * s}, color, 1.6f * s);
    ren.drawLine({cx - 3.6f * s, center.y + 2.0f * s},
                 {cx - 1.2f * s, center.y + 2.0f * s}, color, 1.6f * s);
    ren.drawLine({cx - 3.6f * s, center.y - 2.0f * s},
                 {cx - 3.6f * s, center.y + 2.0f * s}, color, 1.6f * s);
    ren.drawLine({cx - 1.2f * s, center.y - 2.0f * s},
                 {cx + 1.6f * s, center.y - 3.8f * s}, color, 1.6f * s);
    ren.drawLine({cx - 1.2f * s, center.y + 2.0f * s},
                 {cx + 1.6f * s, center.y + 3.8f * s}, color, 1.6f * s);
    ren.drawLine({cx + 1.6f * s, center.y - 3.8f * s},
                 {cx + 1.6f * s, center.y + 3.8f * s}, color, 1.6f * s);
}

void drawIconBadgeChip(nxui::Renderer& ren, const nxui::Rect& rect, ShelfBadgeKind kind,
                       const nxui::Color& fill, const nxui::Color& border,
                       const nxui::Color& glyph, float opacity) {
    const float radius = rect.height * 0.5f;
    ren.drawRoundedRect(rect, fill.withAlpha(fill.a * opacity), radius);
    ren.drawRoundedRectOutline(rect, border.withAlpha(border.a * opacity), radius, 1.3f);
    drawShelfBadgeGlyph(ren, kind,
                        {rect.x + rect.width * 0.5f, rect.y + rect.height * 0.5f},
                        glyph.withAlpha(opacity));
}

void drawActionButtonChip(nxui::Renderer& ren,
                          nxui::Font* font,
                          const nxui::Rect& rect,
                          const std::string& text,
                          const nxui::Theme* theme,
                          const nxui::Color& textColor,
                          float opacity,
                          float emphasis,
                          float accentMix = -1.f,
                          float scale = 0.72f) {
    auto style = switchu::ui::resolveActionButtonStyle(theme, opacity, emphasis, accentMix);
    nxui::Rect buttonRect = switchu::ui::scaledActionButtonRect(rect, style.scale);
    float radius = std::min(kActionButtonRadius, buttonRect.height * 0.5f);

    switchu::ui::drawActionButtonChrome(ren, rect, radius, style);

    if (font) {
        float textScale = scale * style.scale;
        std::string fitted = ellipsize(font, text, buttonRect.width - 24.f, textScale);
        nxui::Vec2 size = measureTextCached(font, fitted);
        ren.drawText(fitted,
                     {buttonRect.x + (buttonRect.width - size.x * textScale) * 0.5f,
                      buttonRect.y + (buttonRect.height - size.y * textScale) * 0.5f},
                     font,
                     textColor.withAlpha(textColor.a * opacity),
                     textScale);
    }
}

void drawLiquidGlassPanel(nxui::Renderer& ren,
                          const nxui::Theme* theme,
                          const nxui::Rect& rect,
                          float radius,
                          float opacity,
                          float borderBoost = 1.f) {
    if (!theme || opacity <= 0.01f)
        return;

    const auto& tuning = settings::debug::settingsGlassTuning();
    nxui::LiquidGlassSettings savedGlass = ren.liquidGlassSettings();
    auto& glass = ren.liquidGlassSettings();
    glass.refractionIntensity = std::clamp(tuning.refractionIntensity, 0.0f, 1.5f);
    glass.blurIntensity = std::max(0.0f, tuning.shaderBlurIntensity);
    glass.noiseIntensity = 0.0f;
    glass.glowIntensity = std::max(0.0f, tuning.glowIntensity);
    glass.saturation = std::max(0.0f, tuning.saturation);
    glass.opacityMultiplier = 1.0f;
    glass.roughness = std::max(0.0f, tuning.roughness);
    glass.powerFactor = std::max(1.001f, tuning.powerFactor);

    nxui::Rect glassRect = rect.shrunk(std::max(0.0f, tuning.inset));
    float glassRadius = std::max(12.0f, radius - std::max(0.0f, tuning.inset) * 0.5f);
    nxui::Color glassTint = theme->panelBase.withAlpha(theme->mode == nxui::ThemeMode::Dark
        ? std::clamp(tuning.tintAlphaDark, 0.0f, 1.0f)
        : std::clamp(tuning.tintAlphaLight, 0.0f, 1.0f));

    ren.drawLiquidGlass(kThemeShopBackdropCacheTarget,
                        glassRect,
                        glassRadius,
                        glassTint,
                        opacity,
                        std::clamp(tuning.shade, 0.0f, 1.0f));
    ren.drawRoundedRectOutline(glassRect,
                               theme->panelBorder.withAlpha(0.24f * borderBoost * opacity),
                               glassRadius,
                               1.2f);
    ren.drawRoundedRectOutline(glassRect.shrunk(1.5f),
                               theme->panelHighlight.withAlpha(0.08f * opacity),
                               std::max(0.f, glassRadius - 1.5f),
                               1.0f);
    ren.liquidGlassSettings() = savedGlass;
}

void drawPreviewPlaceholder(nxui::Renderer& ren,
                            nxui::Font* smallFont,
                            const nxui::Theme* theme,
                            const nxui::Rect& rect,
                            const std::string& label,
                            bool loading,
                            float time,
                            float opacity) {
    ren.drawRoundedRect(rect, theme->panelBase.withAlpha(0.16f * opacity), 14.f);
    ren.drawRoundedRectOutline(rect, theme->panelBorder.withAlpha(0.18f * opacity), 14.f, 1.f);

    nxui::Rect inner = {rect.x + 4.f, rect.y + 4.f, rect.width - 8.f, rect.height - 8.f};
    ren.drawRoundedRect(inner, theme->background.withAlpha(0.22f * opacity), 12.f);

    nxui::Vec2 center = {inner.x + inner.width * 0.5f, inner.y + inner.height * 0.5f};
    if (loading) {
        drawSpinner(ren, {center.x, center.y - 12.f}, 14.f, time, theme->cursorNormal, opacity);
    }

    if (smallFont && !label.empty()) {
        std::string fitted = ellipsize(smallFont, label, inner.width - 20.f, 0.64f);
        nxui::Vec2 size = measureTextCached(smallFont, fitted);
        float textY = center.y + (loading ? 12.f : -size.y * 0.32f);
        ren.drawText(fitted,
                     {inner.x + (inner.width - size.x * 0.64f) * 0.5f, textY},
                     smallFont,
                     theme->textSecondary.withAlpha(0.92f * opacity),
                     0.64f);
    }
}

void drawBuiltInThemePreview(nxui::Renderer& ren,
                             nxui::Font* smallFont,
                             const nxui::Rect& rect,
                             bool light,
                             float opacity) {
    // The original built-in themes have no wallpaper or screenshot asset.
    // Keep their preview honest and zero-byte: a miniature of SwitchU's own
    // chrome, painted from the dark/light palette rather than a fake photo.
    const nxui::Color background = light ? nxui::Color(0.79f, 0.85f, 0.91f, opacity)
                                        : nxui::Color(0.035f, 0.055f, 0.080f, opacity);
    const nxui::Color panel = light ? nxui::Color(0.94f, 0.97f, 1.00f, 0.72f * opacity)
                                    : nxui::Color(0.14f, 0.19f, 0.25f, 0.78f * opacity);
    const nxui::Color accent = light ? nxui::Color(0.13f, 0.46f, 0.89f, opacity)
                                     : nxui::Color(0.18f, 0.58f, 1.00f, opacity);
    const nxui::Color muted = light ? nxui::Color(0.32f, 0.42f, 0.53f, 0.78f * opacity)
                                    : nxui::Color(0.74f, 0.82f, 0.91f, 0.70f * opacity);

    ren.drawRoundedRect(rect, background, 14.f);
    ren.drawRoundedRectOutline(rect, accent.withAlpha(0.45f * opacity), 14.f, 1.f);
    const nxui::Rect inner = rect.shrunk(7.f);
    const nxui::Rect topBar = {inner.x, inner.y, inner.width, std::max(14.f, inner.height * 0.17f)};
    ren.drawRoundedRect(topBar, panel, 8.f);
    ren.drawRoundedRect({topBar.x + 8.f, topBar.y + topBar.height * 0.38f, topBar.width * 0.32f, 2.f}, accent, 1.f);
    const nxui::Rect sidebar = {inner.x, topBar.bottom() + 5.f, std::max(17.f, inner.width * 0.14f),
                                inner.bottom() - topBar.bottom() - 5.f};
    ren.drawRoundedRect(sidebar, panel, 8.f);
    const float iconSize = std::max(5.f, sidebar.width * 0.38f);
    for (int index = 0; index < 3; ++index) {
        const nxui::Rect icon = {sidebar.x + (sidebar.width - iconSize) * 0.5f,
                                 sidebar.y + 9.f + index * (iconSize + 7.f), iconSize, iconSize};
        ren.drawRoundedRect(icon, index == 0 ? accent : muted.withAlpha(0.48f * opacity), iconSize * 0.35f);
    }
    const nxui::Rect content = {sidebar.right() + 6.f, topBar.bottom() + 5.f,
                                inner.right() - sidebar.right() - 6.f, inner.bottom() - topBar.bottom() - 5.f};
    const nxui::Rect hero = {content.x, content.y, content.width, content.height * 0.58f};
    ren.drawRoundedRect(hero, panel, 9.f);
    ren.drawRoundedRect({hero.x + hero.width * 0.10f, hero.y + hero.height * 0.23f,
                         hero.width * 0.64f, 3.f}, muted, 1.f);
    ren.drawRoundedRect({hero.x + hero.width * 0.10f, hero.y + hero.height * 0.43f,
                         hero.width * 0.43f, 2.f}, muted.withAlpha(0.70f * opacity), 1.f);
    const float cardGap = 4.f;
    const float cardWidth = (content.width - cardGap * 2.f) / 3.f;
    for (int index = 0; index < 3; ++index) {
        const nxui::Rect card = {content.x + index * (cardWidth + cardGap), hero.bottom() + 5.f,
                                 cardWidth, std::max(8.f, content.bottom() - hero.bottom() - 5.f)};
        ren.drawRoundedRect(card, index == 0 ? accent.withAlpha(0.72f * opacity) : panel, 5.f);
    }
    if (smallFont) {
        ren.drawText("SwitchU", {inner.right() - 46.f, topBar.y + 4.f}, smallFont,
                     light ? nxui::Color(0.08f, 0.18f, 0.29f, opacity) : nxui::Color::white().withAlpha(opacity),
                     0.42f);
    }
}

void drawHueSpectrumTrack(nxui::Renderer& ren, const nxui::Rect& track, float hue01, float opacity,
                          bool focused) {
    constexpr int kBands = 24;
    const float bandW = track.width / static_cast<float>(kBands);
    for (int i = 0; i < kBands; ++i) {
        const float hue = static_cast<float>(i) / static_cast<float>(kBands);
        ren.drawRect({track.x + bandW * static_cast<float>(i), track.y, bandW + 0.6f, track.height},
                     nxui::Color::fromHSL(hue, 0.80f, 0.52f, opacity));
    }
    ren.drawRoundedRectOutline(track,
                               focused ? nxui::Color(1.f, 1.f, 1.f, 0.55f * opacity)
                                       : nxui::Color(0.f, 0.f, 0.f, 0.28f * opacity),
                               track.height * 0.5f,
                               focused ? 2.f : 1.f);
    const float knobX = track.x + std::clamp(hue01, 0.f, 1.f) * track.width;
    const float knobR = track.height * 0.72f;
    const nxui::Vec2 knobC = {knobX, track.y + track.height * 0.5f};
    ren.drawCircle(knobC, knobR + 2.6f, nxui::Color(0.f, 0.f, 0.f, 0.35f * opacity));
    ren.drawCircle(knobC, knobR + 1.4f, nxui::Color(1.f, 1.f, 1.f, 0.90f * opacity));
    ren.drawCircle(knobC, knobR,
                   nxui::Color::fromHSL(std::clamp(hue01, 0.f, 1.f), 0.78f, 0.55f, opacity));
}

void drawTintTogglePill(nxui::Renderer& ren,
                        nxui::Font* font,
                        const nxui::Rect& pill,
                        bool enabled,
                        float opacity,
                        bool focused) {
    const nxui::Color pillBg = enabled
        ? nxui::Color(0.20f, 0.82f, 0.45f, 0.90f * opacity)
        : nxui::Color(0.35f, 0.38f, 0.45f, 0.75f * opacity);
    ren.drawRoundedRect(pill, pillBg, pill.height * 0.5f);
    if (focused)
        ren.drawRoundedRectOutline(pill, nxui::Color(1.f, 1.f, 1.f, 0.55f * opacity),
                                   pill.height * 0.5f, 2.f);
    const float knobX = enabled ? (pill.x + pill.width - pill.height * 0.5f)
                                : (pill.x + pill.height * 0.5f);
    ren.drawCircle({knobX, pill.y + pill.height * 0.5f}, pill.height * 0.40f,
                   nxui::Color(1.f, 1.f, 1.f, opacity));
    if (font) {
        // Label drawn by caller beside the pill.
        (void)font;
    }
}

struct IconTintLayout {
    nxui::Rect tintLabelRow;
    nxui::Rect tintPill;
    nxui::Rect matchLabelRow;
    nxui::Rect matchPill;
    nxui::Rect flattenLabelRow;
    nxui::Rect flattenPill;
    nxui::Rect grayLabelRow;
    nxui::Rect grayPill;
    nxui::Rect hueLabel;
    nxui::Rect hueTrack;
    nxui::Rect satLabel;
    nxui::Rect satTrack;
    nxui::Rect lightLabel;
    nxui::Rect lightTrack;
};

// HSL above toggles so Apply is reached without brushing sliders (which break Match).
// Block is always vertically centered against the preview band — if HSL makes the
// stack taller than the art, scale gaps/rows so it still fits and can center.
IconTintLayout makeIconTintLayout(const nxui::Rect& band, bool showSliders, bool showHueSat) {
    IconTintLayout layout;
    constexpr float kPillW = 56.f;
    float rowH = 34.f;
    float pillH = 28.f;
    float toggleGap = 6.f;
    float labelH = 20.f;
    float trackH = 16.f;
    float labelGap = 5.f;
    float sectionGap = 14.f;
    float sliderToToggleGap = 12.f;

    auto measureBlockH = [&]() {
        const float togglesH = 4.f * rowH + 3.f * toggleGap;
        if (!showSliders)
            return togglesH;
        const int sliderCount = showHueSat ? 3 : 1;
        const float slidersH = static_cast<float>(sliderCount) * (labelH + labelGap + trackH)
            + static_cast<float>(std::max(0, sliderCount - 1)) * sectionGap
            + sliderToToggleGap;
        return togglesH + slidersH;
    };

    float blockH = measureBlockH();
    constexpr float kMinPad = 6.f;
    if (showSliders && band.height > 1.f && blockH + 2.f * kMinPad > band.height) {
        const float target = std::max(40.f, band.height - 2.f * kMinPad);
        const float s = std::clamp(target / blockH, 0.55f, 1.f);
        rowH *= s;
        pillH *= s;
        toggleGap *= s;
        labelH *= s;
        trackH *= s;
        labelGap *= s;
        sectionGap *= s;
        sliderToToggleGap *= s;
        blockH = measureBlockH();
    }
    float y = band.y + std::max(0.f, (band.height - blockH) * 0.5f);

    auto placeToggle = [&](nxui::Rect& labelRow, nxui::Rect& pill, float rowY) {
        labelRow = {band.x, rowY, band.width, rowH};
        pill = {band.x + band.width - kPillW - 2.f,
                rowY + std::max(1.f, (rowH - pillH) * 0.5f),
                kPillW,
                pillH};
    };

    if (showSliders) {
        if (showHueSat) {
            layout.hueLabel = {band.x, y, band.width, labelH};
            y = layout.hueLabel.bottom() + labelGap;
            layout.hueTrack = {band.x, y, band.width, trackH};
            y = layout.hueTrack.bottom() + sectionGap;
            layout.satLabel = {band.x, y, band.width, labelH};
            y = layout.satLabel.bottom() + labelGap;
            layout.satTrack = {band.x, y, band.width, trackH};
            y = layout.satTrack.bottom() + sectionGap;
            layout.lightLabel = {band.x, y, band.width, labelH};
            y = layout.lightLabel.bottom() + labelGap;
            layout.lightTrack = {band.x, y, band.width, trackH};
            y = layout.lightTrack.bottom() + sliderToToggleGap;
        } else {
            // Lightness alone reuses the hue slots (same as draw/touch path).
            layout.hueLabel = {band.x, y, band.width, labelH};
            y = layout.hueLabel.bottom() + labelGap;
            layout.hueTrack = {band.x, y, band.width, trackH};
            y = layout.hueTrack.bottom() + sliderToToggleGap;
            layout.satLabel = {};
            layout.satTrack = {};
            layout.lightLabel = {};
            layout.lightTrack = {};
        }
    } else {
        layout.hueLabel = {};
        layout.hueTrack = {};
        layout.satLabel = {};
        layout.satTrack = {};
        layout.lightLabel = {};
        layout.lightTrack = {};
    }

    placeToggle(layout.matchLabelRow, layout.matchPill, y);
    y = layout.matchLabelRow.bottom() + toggleGap;
    placeToggle(layout.tintLabelRow, layout.tintPill, y);
    y = layout.tintLabelRow.bottom() + toggleGap;
    placeToggle(layout.flattenLabelRow, layout.flattenPill, y);
    y = layout.flattenLabelRow.bottom() + toggleGap;
    placeToggle(layout.grayLabelRow, layout.grayPill, y);
    return layout;
}

// Large left preview + tint column. Icon studio is a glyph collage (not a 16:9
// screenshot) — fill the body box so HSL+toggles have a tall partner to center on.
nxui::Rect colourIconPreviewBounds(const nxui::Rect& dialog) {
    constexpr float kBodyTopInset = 78.f; // room for title above the studio preview
    constexpr float kBodyBottomInset = 92.f;
    return {
        dialog.x + 28.f,
        dialog.y + kBodyTopInset,
        dialog.width * 0.48f,
        dialog.height - kBodyTopInset - kBodyBottomInset
    };
}

nxui::Rect colourIconTintBand(const nxui::Rect& dialog, const nxui::Rect& preview, bool tintExpanded) {
    (void)tintExpanded;
    (void)dialog;
    // Same vertical span as the preview art — controls center against that tile
    // (Apply stays in the dialog footer and is not part of this band).
    return {
        preview.right() + 24.f,
        preview.y,
        std::max(160.f, dialog.right() - preview.right() - 46.f),
        std::max(80.f, preview.height)
    };
}

void drawGradientValueTrack(nxui::Renderer& ren,
                            const nxui::Rect& track,
                            float value01,
                            float opacity,
                            bool focused,
                            const std::function<nxui::Color(float)>& colorAt) {
    constexpr int kBands = 24;
    const float bandW = track.width / static_cast<float>(kBands);
    for (int i = 0; i < kBands; ++i) {
        const float t = (static_cast<float>(i) + 0.5f) / static_cast<float>(kBands);
        ren.drawRect({track.x + bandW * static_cast<float>(i), track.y, bandW + 0.6f, track.height},
                     colorAt(t).withAlpha(opacity));
    }
    ren.drawRoundedRectOutline(track,
                               focused ? nxui::Color(1.f, 1.f, 1.f, 0.55f * opacity)
                                       : nxui::Color(0.f, 0.f, 0.f, 0.28f * opacity),
                               track.height * 0.5f,
                               focused ? 2.f : 1.f);
    const float knobX = track.x + std::clamp(value01, 0.f, 1.f) * track.width;
    const float knobR = track.height * 0.72f;
    const nxui::Vec2 knobC = {knobX, track.y + track.height * 0.5f};
    ren.drawCircle(knobC, knobR + 2.6f, nxui::Color(0.f, 0.f, 0.f, 0.35f * opacity));
    ren.drawCircle(knobC, knobR + 1.4f, nxui::Color(1.f, 1.f, 1.f, 0.90f * opacity));
    ren.drawCircle(knobC, knobR, colorAt(std::clamp(value01, 0.f, 1.f)).withAlpha(opacity));
}

void drawThemePreview(nxui::Renderer& ren,
                      nxui::Font* smallFont,
                      const nxui::Theme* theme,
                      const nxui::Rect& rect,
                      const nxui::Texture* texture,
                      const std::string& placeholderLabel,
                      bool loading,
                      float time,
                      float opacity,
                      int sheetCols = 0,
                      int sheetRows = 0,
                      float sheetFps = 10.f,
                      bool builtInDefault = false,
                      bool builtInLight = false,
                      bool adaptiveTint = false,
                      bool iconPackGlass = false,
                      const nxui::Texture* contrastMask = nullptr,
                      const nxui::Color* customTint = nullptr,
                      bool shelfCover = false) {
    if (!theme)
        return;

    if (texture && texture->valid()) {
        nxui::Rect drawRect = rect;
        float corner = 14.f;
        if (shelfCover) {
            // eShop shelf / product hero: cover is the plane — no inset frame.
            ren.drawRoundedRect(rect, nxui::Color(0.f, 0.f, 0.f, 0.22f * opacity), 16.f);
            drawRect = rect;
            corner = 16.f;
        } else if (iconPackGlass) {
            // Little glass well inside the pack card - theme-coloured (no hard
            // #1c1e25 plate). Glyphs keep alpha and sit on this glass.
            const bool dark = theme->mode == nxui::ThemeMode::Dark;
            const nxui::Color glassBase = theme->panelBase.withAlpha((dark ? 0.58f : 0.78f) * opacity);
            const nxui::Color glassBorder = theme->panelBorder.withAlpha(0.36f * opacity);
            const nxui::Color glassHighlight = theme->panelHighlight.withAlpha(0.16f * opacity);
            ren.drawFrostedInset(rect, glassBase, glassBorder, glassHighlight, 14.f, opacity);
            drawRect = {rect.x + 10.f, rect.y + 10.f, rect.width - 20.f, rect.height - 20.f};
            corner = 10.f;
        } else {
            // Theme screenshots keep a framed inset well.
            ren.drawRoundedRect(rect, theme->panelBase.withAlpha(0.14f * opacity), 14.f);
            ren.drawRoundedRectOutline(rect, theme->panelBorder.withAlpha(0.18f * opacity), 14.f, 1.f);
            drawRect = {rect.x + 4.f, rect.y + 4.f, rect.width - 8.f, rect.height - 8.f};
            ren.drawRoundedRect(drawRect, nxui::Color(0.f, 0.f, 0.f, 0.14f * opacity), 12.f);
            corner = 12.f;
        }
        const nxui::Rect& inner = drawRect;
        if (sheetCols > 0 && sheetRows > 0) {
            // A folha traz os quadros em grade; o cartao percorre um por vez.
            // Anima sem baixar nada alem da propria imagem, e sem guardar um
            // laco inteiro de texturas para uma miniatura.
            const int total = sheetCols * sheetRows;
            const int frame = ((int)(time * sheetFps)) % total;
            const float cw = (float)texture->width() / (float)sheetCols;
            const float ch = (float)texture->height() / (float)sheetRows;
            const nxui::Rect src{(frame % sheetCols) * cw, (frame / sheetCols) * ch, cw, ch};
            // O destino usa a proporcao de uma celula, nao da folha inteira.
            const float cellAspect = cw / ch;
            float dw = inner.width, dh = inner.width / cellAspect;
            if (dh > inner.height) { dh = inner.height; dw = inner.height * cellAspect; }
            const nxui::Rect dest{inner.x + (inner.width - dw) * 0.5f,
                                  inner.y + (inner.height - dh) * 0.5f, dw, dh};
            ren.drawTextureRoundedSub(texture, src, dest, corner,
                                      nxui::Color(1.f, 1.f, 1.f, opacity));
            return;
        }
        const nxui::Texture* drawTex = texture;
        nxui::Color tint = adaptiveTint
            ? theme->textPrimary.withAlpha(theme->textPrimary.a * opacity)
            : nxui::Color(1.f, 1.f, 1.f, opacity);
        if (!adaptiveTint && customTint && contrastMask && contrastMask->valid()) {
            drawTex = contrastMask;
            tint = customTint->withAlpha(customTint->a * opacity);
        }
        ren.drawTextureRounded(drawTex,
                               fitTextureRect(drawRect, drawTex),
                               corner,
                               tint);
        return;
    }

    if (builtInDefault) {
        drawBuiltInThemePreview(ren, smallFont, rect, builtInLight, opacity);
        return;
    }

    drawPreviewPlaceholder(ren, smallFont, theme, rect, placeholderLabel, loading, time, opacity);
}

std::string transferLabel(const ThemeTransferState& state) {
    if (state.label().empty())
        return {};
    if (state.progress01() < 0.f || state.progress01() > 1.f)
        return state.label();

    int percent = (int)std::lround(state.progress01() * 100.f);
    return state.label() + " (" + std::to_string(percent) + "%)";
}

// Sizes a player can act on, not exact byte counts.
//
// The decimal is dropped above 100 of a unit: "1,2 GB" tells you whether the
// theme fits, "847,3 MB" only makes the line longer. The unit is binary, which
// is what the console's own storage screen counts in, so the two agree.
std::string formatBytes(std::uint64_t bytes) {
    if (bytes == 0)
        return {};
    const char* units[] = {"KB", "MB", "GB"};
    double value = (double)bytes / 1024.0;
    int unit = 0;
    while (value >= 1024.0 && unit < 2) {
        value /= 1024.0;
        ++unit;
    }
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), value >= 100.0 ? "%.0f %s" : "%.1f %s", value, units[unit]);
    return buffer;
}

std::string estimateDownloadSize(const std::string& durationStr) {
    if (durationStr.empty() || durationStr == "0:00")
        return "~4.5 MB";
    int seconds = 0;
    size_t firstColon = durationStr.find(':');
    if (firstColon != std::string::npos) {
        size_t secondColon = durationStr.find(':', firstColon + 1);
        if (secondColon != std::string::npos) {
            int h = std::atoi(durationStr.substr(0, firstColon).c_str());
            int m = std::atoi(durationStr.substr(firstColon + 1, secondColon - firstColon - 1).c_str());
            int s = std::atoi(durationStr.substr(secondColon + 1).c_str());
            seconds = h * 3600 + m * 60 + s;
        } else {
            int m = std::atoi(durationStr.substr(0, firstColon).c_str());
            int s = std::atoi(durationStr.substr(firstColon + 1).c_str());
            seconds = m * 60 + s;
        }
    }
    if (seconds <= 0)
        return "~4.5 MB";

    std::uint64_t bytes = static_cast<std::uint64_t>(seconds) * 24000ULL;
    return "~" + formatBytes(bytes);
}

void drawTransferProgressTrack(nxui::Renderer& ren,
                               const nxui::Rect& track,
                               float progress01,
                               float opacity,
                               const nxui::Color& accent) {
    if (track.width <= 1.f || track.height <= 1.f)
        return;
    const float radius = track.height * 0.5f;
    ren.drawRoundedRect(track, nxui::Color(0.f, 0.f, 0.f, 0.42f * opacity), radius);
    ren.drawRoundedRectOutline(track, nxui::Color(1.f, 1.f, 1.f, 0.14f * opacity), radius, 1.f);
    if (progress01 < 0.f)
        return;
    const float fillW = std::max(track.height,
                                 track.width * std::clamp(progress01, 0.f, 1.f));
    ren.drawRoundedRect({track.x, track.y, fillW, track.height},
                        accent.withAlpha(0.95f * opacity),
                        radius);
}

} // namespace

// Which custom-drawn tab is open. The sidebar is Themes (Installed | Theme
// Shop), the fork's Animated Themes catalogue, Icons (Installed | Icon Shop)
// and Music; the settings-row tabs (Sounds, Interface, SteamGridDB, Update,
// Uninstall) come after and are not custom content.
bool ThemeShopScreen::isThemesTab() const {
    return m_tabIndex == kThemesTabIndex;
}

bool ThemeShopScreen::isAnimatedTab() const {
    return m_tabIndex == kAnimatedTabIndex;
}

bool ThemeShopScreen::isIconsTab() const {
    return m_tabIndex == kIconsTabIndex;
}

bool ThemeShopScreen::isMusicTab() const {
    return m_tabIndex == kMusicTabIndex;
}

bool ThemeShopScreen::isInstalledTab() const {
    return isThemesTab() && m_themesShelfMode == ShelfMode::Installed;
}

// Both catalogue views render the same way and read the same list; what tells
// them apart is which entries survive the filter (PoloNX's index on the Theme
// Shop shelf, the animated mirror on the Animated tab).
bool ThemeShopScreen::isCommunityTab() const {
    return (isThemesTab() && m_themesShelfMode == ShelfMode::Shop) || isAnimatedTab();
}

bool ThemeShopScreen::isIconsInstalledMode() const {
    return isIconsTab() && m_iconsShelfMode == ShelfMode::Installed;
}

bool ThemeShopScreen::isIconsShopMode() const {
    return isIconsTab() && m_iconsShelfMode == ShelfMode::Shop;
}
bool ThemeShopScreen::showsCatalogRefresh() const {
    return isCommunityTab() || isIconsShopMode();
}

bool ThemeShopScreen::showsShelfBranch() const {
    return isThemesTab() || isIconsTab();
}

ThemeShopScreen::ShelfMode ThemeShopScreen::activeShelfMode() const {
    return isIconsTab() ? m_iconsShelfMode : m_themesShelfMode;
}

void ThemeShopScreen::setActiveShelfMode(ShelfMode mode) {
    ShelfMode& slot = isIconsTab() ? m_iconsShelfMode : m_themesShelfMode;
    const bool modeChanged = (slot != mode);
    slot = mode;
    m_shopBranchFocused = showsShelfBranch() && mode == ShelfMode::Shop
        && m_focusArea == FocusArea::Tabs;
    if (!modeChanged)
        return;
    closeDetail();
    currentScrollRowRef() = 0;
    applySearchFilter();
    if (isCommunityTab() || isIconsShopMode())
        ensureCommunityCatalogWarm();
    rebuildCurrentTab();
}

float ThemeShopScreen::tabBranchExtraHeight(int tabIndex) const {
    if (tabIndex != m_tabIndex || (tabIndex != kThemesTabIndex && tabIndex != kIconsTabIndex))
        return 0.f;
    return kShopBranchRow * std::clamp(m_shopBranchReveal.value(), 0.f, 1.f);
}

bool ThemeShopScreen::tabBranchConsumesFocus() const {
    return showsShelfBranch() && m_shopBranchFocused && m_focusArea == FocusArea::Tabs;
}

bool ThemeShopScreen::tabFocusCursorRect(nxui::Rect& outRect) const {
    if (!tabBranchConsumesFocus() || m_shopBranchRect.width <= 1.f)
        return false;
    outRect = m_shopBranchRect.expanded(1.f);
    return true;
}

void ThemeShopScreen::onTabRailParentActivated(int tabIndex) {
    if (tabIndex != kThemesTabIndex && tabIndex != kIconsTabIndex)
        return;
    m_shopBranchFocused = false;
    setActiveShelfMode(ShelfMode::Installed);
}

bool ThemeShopScreen::onTabRailBranchActivated(int parentTabIndex) {
    if (parentTabIndex != kThemesTabIndex && parentTabIndex != kIconsTabIndex)
        return false;
    m_focusArea = FocusArea::Tabs;
    m_shopBranchFocused = true;
    setActiveShelfMode(ShelfMode::Shop);
    return true;
}

void ThemeShopScreen::drawTabBranch(nxui::Renderer& ren,
                                    int tabIndex,
                                    const nxui::Rect& branchRect,
                                    float opacity) {
    if (!m_theme || !m_font || (tabIndex != kThemesTabIndex && tabIndex != kIconsTabIndex))
        return;
    const float reveal = std::clamp(m_shopBranchReveal.value(), 0.f, 1.f);
    if (reveal < 0.02f || branchRect.width <= 1.f || branchRect.height <= 1.f)
        return;

    auto& i18n = nxui::I18n::instance();
    const float slide = (1.f - reveal) * 12.f;
    nxui::Rect r = {
        branchRect.x,
        branchRect.y + slide,
        branchRect.width,
        branchRect.height
    };
    m_shopBranchRect = r;

    const bool shopActive = activeShelfMode() == ShelfMode::Shop;
    const bool focused = tabBranchConsumesFocus();
    const float alpha = opacity * reveal;

    // Elbow: gray at rest, theme accent while the Shop child is focused.
    const float elbowX = r.x - 12.f;
    const float elbowMidY = r.y + r.height * 0.5f;
    const nxui::Color stem = focused
        ? m_theme->cursorNormal.withAlpha(0.72f * alpha)
        : m_theme->panelBorder.withAlpha(0.36f * alpha);
    ren.drawLine({elbowX, r.y - 6.f}, {elbowX, elbowMidY}, stem, focused ? 2.0f : 1.5f);
    ren.drawLine({elbowX, elbowMidY}, {r.x + 2.f, elbowMidY}, stem, focused ? 2.0f : 1.5f);

    // Match parent tab pills: panel fill only. Focus ring comes from SelectionCursor.
    const nxui::Color fill = m_theme->panelBase.withAlpha((shopActive ? 0.94f : 0.88f) * alpha);
    // Keep the pill itself fill+soft border; the blue focus ring is SelectionCursor only.
    const nxui::Color border = shopActive
        ? m_theme->cursorNormal.withAlpha(0.46f * alpha)
        : m_theme->panelBorder.withAlpha(0.22f * alpha);
    ren.drawRoundedRect(r, fill, 18.f);
    ren.drawRoundedRectOutline(r, border, 18.f, shopActive ? 1.2f : 1.f);

    const std::string label = isIconsTab()
        ? i18n.tr("themeshop.icons.shop_item", "Icon Shop")
        : i18n.tr("themeshop.themes.shop_item", "Theme Shop");
    float scale = shopActive ? 0.91f : 0.87f;
    nxui::Vec2 size = measureTextCached(m_font, label);
    if (size.x > 0.f && r.width > 44.f)
        scale = std::max(0.55f, std::min(scale, (r.width - 44.f) / size.x));
    ren.drawText(label,
                 {r.x + 22.f, r.y + (r.height - size.y * scale) * 0.5f},
                 m_font,
                 (shopActive ? m_theme->textPrimary : m_theme->textSecondary).withAlpha(alpha),
                 scale);
}

bool ThemeShopScreen::stepCataloguePage(int delta) {
    if (!usesCustomContentLayout() || m_detailOpen || !isFullyVisible())
        return false;
    const int before = currentPage();
    stepPage(delta);
    return before != currentPage();
}

// "61 temas - 4,5 GB para baixar - 7,3 GB no cartao", for whichever catalogue
// the open tab is showing.
//
// Both figures are worth having and they are far apart: the frames compress
// hard, so the download is a little over half of what lands on the card. Only
// entries that declare a size contribute -- an older index that never carried
// the field is reported as the themes it has, without a size claim invented on
// its behalf.
std::uint64_t ThemeShopScreen::installedThemeBytes(const std::string& installPath) const {
    if (installPath.empty())
        return 0;
    std::lock_guard<std::mutex> lk(m_installedSizes->mutex);
    auto it = m_installedSizes->bytes.find(installPath);
    return it == m_installedSizes->bytes.end() ? 0 : it->second;
}

// Uma medição por tema, na thread de trabalho, e o resultado fica.
//
// Um tema animado é uma pasta com centenas de quadros; somar isso com o
// desenho parado é a mesma travada que a capa personalizada causava. O tamanho
// aparece um instante depois de a aba abrir, e é o preço certo a pagar.
void ThemeShopScreen::measureInstalledThemes() {
    if (!m_threadPool)
        return;

    std::vector<std::string> pending;
    {
        std::lock_guard<std::mutex> lk(m_installedSizes->mutex);
        for (const auto& entry : m_allThemeShopEntries) {
            if (entry.installPath.empty())
                continue;
            if (m_installedSizes->bytes.count(entry.installPath))
                continue;
            if (!m_installedSizes->inFlight.insert(entry.installPath).second)
                continue;
            pending.push_back(entry.installPath);
        }
    }
    if (pending.empty())
        return;

    auto sizes = m_installedSizes;
    m_threadPool->submit([sizes, pending]() {
        for (const auto& path : pending) {
            std::uint64_t total = 0;
            std::error_code ec;
            for (std::filesystem::recursive_directory_iterator it(path, ec), end;
                 it != end && !ec; it.increment(ec)) {
                if (it->is_regular_file(ec) && !ec)
                    total += (std::uint64_t)it->file_size(ec);
            }
            std::lock_guard<std::mutex> lk(sizes->mutex);
            sizes->bytes[path] = total;
            sizes->inFlight.erase(path);
        }
    });
}

// "4 temas - 512 MB no cartao". Só conta o que realmente ocupa espaço: os
// embutidos vêm dentro do próprio build e não têm caminho de instalação.
std::string ThemeShopScreen::installedThemeTotals() const {
    int measured = 0;
    std::uint64_t total = 0;
    for (const auto& entry : m_allThemeShopEntries) {
        const std::uint64_t bytes = installedThemeBytes(entry.installPath);
        if (bytes == 0)
            continue;
        ++measured;
        total += bytes;
    }
    if (measured == 0 || total == 0)
        return {};

    auto& i18n = nxui::I18n::instance();
    return std::to_string((int)m_allThemeShopEntries.size()) + " "
        + (m_allThemeShopEntries.size() == 1
               ? i18n.tr("themeshop.catalog.theme_one", "theme")
               : i18n.tr("themeshop.catalog.theme_many", "themes"))
        + " - " + formatBytes(total) + " " + i18n.tr("themeshop.catalog.on_card", "on the SD card");
}

std::string ThemeShopScreen::communityCatalogueTotals() const {
    const bool wantAnimated = isAnimatedTab();
    int themes = 0;
    std::uint64_t download = 0;
    std::uint64_t onCard = 0;
    for (const auto& entry : m_allCommunityEntries) {
        const bool fromOurs = entry.catalogUrl == ThemeCatalogClient::kDefaultCatalogUrl;
        if (fromOurs != wantAnimated)
            continue;
        ++themes;
        download += entry.packageBytes;
        onCard   += entry.installedBytes;
    }
    if (themes == 0)
        return {};

    auto& i18n = nxui::I18n::instance();
    std::string line = std::to_string(themes) + " "
        + (themes == 1 ? i18n.tr("themeshop.catalog.theme_one", "theme")
                       : i18n.tr("themeshop.catalog.theme_many", "themes"));
    if (download > 0)
        line += " - " + formatBytes(download) + " " + i18n.tr("themeshop.catalog.to_download", "to download");
    if (onCard > 0)
        line += " - " + formatBytes(onCard) + " " + i18n.tr("themeshop.catalog.on_card", "on the SD card");
    return line;
}

std::string ThemeShopScreen::installedMusicTotals() const {
    auto& i18n = nxui::I18n::instance();
    std::string musicDir = YouTubeClient::musicDirectory();
    std::error_code ec;
    int songCount = 0;
    std::uint64_t totalBytes = 0;
    if (std::filesystem::is_directory(musicDir, ec)) {
        for (const auto& entry : std::filesystem::directory_iterator(musicDir, ec)) {
            if (ec) { ec.clear(); continue; }
            if (!entry.is_regular_file(ec)) continue;
            std::string filename = entry.path().filename().string();
            if (filename.empty() || filename.front() == '.') continue;
            std::string ext = entry.path().extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (ext == ".mp3" || ext == ".ogg" || ext == ".wav") {
                ++songCount;
                std::error_code szEc;
                totalBytes += entry.file_size(szEc);
            }
        }
    }

    if (songCount == 0)
        return {};

    std::string countStr = std::to_string(songCount) + " "
        + (songCount == 1 ? i18n.tr("themeshop.music.song_one", "song")
                          : i18n.tr("themeshop.music.song_many", "songs"));
    return countStr + " - " + formatBytes(totalBytes) + " "
        + i18n.tr("themeshop.catalog.on_card", "on the SD card");
}

int ThemeShopScreen::currentEntryCount() const {
    if (isMusicTab())
        return (int)m_youTubeClient.trackCount();
    if (isCommunityTab())
        return (int)m_communityEntries.size();
    if (isIconsTab())
        return (int)m_iconPackEntries.size();
    return (int)m_themeShopEntries.size();}

int ThemeShopScreen::currentSelectedIndex() const {
    if (isMusicTab()) {
        int count = (int)m_youTubeClient.trackCount();
        if (count <= 0) return -1;
        return std::clamp(m_musicSelectedIndex, 0, count - 1);
    }

    if (isCommunityTab()) {
        for (int i = 0; i < (int)m_communityEntries.size(); ++i) {
            if (m_communityEntries[i].id == m_communitySelectedId)
                return i;
        }
        return m_communityEntries.empty() ? -1 : 0;
    }
    if (isIconsTab()) {
        for (int i = 0; i < (int)m_iconPackEntries.size(); ++i) {
            if (m_iconPackEntries[i].id == m_iconPackSelectedId)
                return i;
        }
        return m_iconPackEntries.empty() ? -1 : 0;
    }

    for (int i = 0; i < (int)m_themeShopEntries.size(); ++i) {
        if (m_themeShopEntries[i].id == m_themeShopSelectedId)
            return i;
    }
    return m_themeShopEntries.empty() ? -1 : 0;
}

void ThemeShopScreen::setCurrentSelectedIndex(int idx) {
    int count = currentEntryCount();
    if (count <= 0)
        return;

    idx = std::clamp(idx, 0, count - 1);
    if (isMusicTab())
        m_musicSelectedIndex = idx;
    else if (isCommunityTab())
        m_communitySelectedId = m_communityEntries[(size_t)idx].id;
    else if (isIconsTab())
        m_iconPackSelectedId = m_iconPackEntries[(size_t)idx].id;
    else
        m_themeShopSelectedId = m_themeShopEntries[(size_t)idx].id;
}

int ThemeShopScreen::pageCount() const {
    int count = currentEntryCount();
    return std::max(1, (count + kEntriesPerPage - 1) / kEntriesPerPage);
}

int ThemeShopScreen::currentPage() const {
    int count = currentEntryCount();
    if (count <= 0)
        return 0;
    int scrollRow = isMusicTab() ? m_musicScrollRow
                  : isCommunityTab() ? m_communityScrollRow
                  : (isIconsTab() ? m_iconPackScrollRow : m_installedScrollRow);
    return std::clamp(scrollRow / kVisibleRows, 0, pageCount() - 1);
}

void ThemeShopScreen::setCurrentPage(int page) {
    int count = currentEntryCount();
    if (count <= 0) {
        currentScrollRowRef() = 0;
        return;
    }

    page = std::clamp(page, 0, pageCount() - 1);
    int target = std::min(count - 1, page * kEntriesPerPage);
    setCurrentSelectedIndex(target);
    currentScrollRowRef() = page * kVisibleRows;
}

void ThemeShopScreen::stepPage(int delta) {
    int before = currentPage();
    setCurrentPage(before + delta);
    if (currentPage() == before || delta == 0)
        return;
    // Presence, not noise: short lateral settle when flipping shelves.
    m_pageSlideFrom = delta > 0 ? 1.f : -1.f;
    m_pageSlideAnim.setImmediate(1.f);
    m_pageSlideAnim.set(0.f, 0.20f, nxui::Easing::outCubic);
    // Kick decode for the landing page at slide start so cold flips recover faster.
    if (isCommunityTab())
        primeVisibleCommunityPreviews();
    else if (isInstalledTab() || isIconsTab())
        primeVisibleInstalledPreviews();
}

int& ThemeShopScreen::currentScrollRowRef() {
    if (isMusicTab())
        return m_musicScrollRow;
    if (isCommunityTab())
        return m_communityScrollRow;
    if (isIconsTab())
        return m_iconPackScrollRow;
    return m_installedScrollRow;

}

void ThemeShopScreen::ensureSelectionVisible() {
    int count = currentEntryCount();
    if (count <= 0) {
        currentScrollRowRef() = 0;
        return;
    }

    int& scrollRow = currentScrollRowRef();
    int maxPageScroll = std::max(0, (pageCount() - 1) * kVisibleRows);
    scrollRow = std::clamp(scrollRow, 0, maxPageScroll);

    int page = std::clamp(scrollRow / kVisibleRows, 0, pageCount() - 1);
    int pageStart = page * kEntriesPerPage;
    int pageEnd = std::min(count, pageStart + kEntriesPerPage);
    int selected = std::max(0, currentSelectedIndex());
    if (selected < pageStart || selected >= pageEnd)
        setCurrentSelectedIndex(std::min(count - 1, pageStart));
}

void ThemeShopScreen::openDetail() {
    if (currentEntryCount() <= 0)
        return;
    m_detailOpen = true;
    m_detailSheetAnim.setImmediate(0.f);
    m_detailSheetAnim.set(1.f, 0.22f, nxui::Easing::outCubic);
    m_detailFullscreen = false;
    m_detailFullscreenAnim.setImmediate(0.f);
    m_contentFocusArea = ContentFocusArea::Grid;
    m_detailButtonIndex = 0;
    m_detailScreenshotIndex = 0;
    m_detailPreviewButtonIndex = 0;
    // Always land on Apply. Icons: Up → tint, Left → preview. Installed /
    // Community: Up → preview. Down from preview/tint returns here.
    m_detailFocusArea = DetailFocusArea::Buttons;
    m_focusArea = FocusArea::Content;

    beginIconPackTintDraft();

    if (isCommunityTab())
        primeCommunityPreview(currentDetailCommunityPreviewPath());
}

void ThemeShopScreen::closeDetail() {
    m_detailOpen = false;
    m_detailSheetAnim.set(0.f, 0.16f, nxui::Easing::outCubic);
    m_detailFullscreen = false;
    m_detailFullscreenAnim.setImmediate(0.f);
    // Avoid lerping the blue ring from the detail Apply button through the
    // bottom-left card before settling on the real selection.
    m_snapGridFocus = true;
    m_detailFocusArea = DetailFocusArea::Buttons;
    m_detailButtonIndex = 0;
    m_detailPreviewButtonIndex = 0;
    m_detailScreenshotIndex = 0;
    if (!m_packageTransferState.isRunning()) {
        m_packageTransferState.reset();
        m_packageTransferThemeId.clear();
        m_packageTransferInstallMode = false;
    }
    // Keep Community covers warm while a package transfer runs in the background.
    discardIconPackTintDraft();
}

// Um pacote baixado vira um preset com id "package:<id do catálogo>". A pasta
// de instalação também usa o id do catálogo, então a ligação entre as duas
// listas é essa e não depende do nome, que o autor do tema pode mudar.
const ThemeShopScreen::ThemeShopEntry*
ThemeShopScreen::installedEntryForCatalogue(const std::string& catalogueId) const {
    if (catalogueId.empty())
        return nullptr;
    const std::string packaged = "package:" + catalogueId;
    for (const auto& entry : m_allThemeShopEntries) {
        if (entry.id == packaged || entry.id == catalogueId)
            return &entry;
    }
    return nullptr;
}

int ThemeShopScreen::detailButtonCount() const {
    if (isMusicTab()) {
        const auto* track = selectedMusicTrack();
        if (!track)
            return 0;
        return track->isDownloaded ? 2 : 1;
    }

    if (isCommunityTab())
        return selectedCommunityThemeEntry() ? 2 : 0;

    const auto* entry = selectedThemeShopEntry();
    if (!entry)
        return 0;
    if (isIconsTab()) {
        if (!entry->installed)
            return 2; // Install / Download + Apply
        return entry->removable ? 2 : 1; // Apply [/ Remove]
    }
    return entry->removable ? 2 : 1;
}

void ThemeShopScreen::activateDetailButton(int buttonIndex) {
    auto& i18n = nxui::I18n::instance();

    if (isMusicTab()) {
        const auto* track = selectedMusicTrack();
        if (!track)
            return;
        int trackIdx = currentSelectedIndex();
        if (trackIdx < 0)
            return;

        if (track->isDownloaded) {
            if (buttonIndex == 0) {
                // Play Track!
                closeDetail();
                if (m_playMusicCb) {
                    m_playMusicCb(track->localFilePath, track->title);
                }
                requestToast(i18n.tr("themeshop.music.playing", "Playing: ") + track->title, 2.5f);
                return;
            } else if (buttonIndex == 1) {
                // Delete Track!
                closeDetail();
                m_youTubeClient.deleteTrack(static_cast<size_t>(trackIdx));
                requestToast(i18n.tr("themeshop.music.deleted", "Track deleted from SD card."), 2.5f);
                if (m_musicDownloadedCb) {
                    m_musicDownloadedCb();
                }
                return;
            }
        }

        if (track->isDownloading) {
            requestToast(i18n.tr("themeshop.music.download_busy", "Download already in progress."), 2.5f);
            return;
        }

        closeDetail();
        std::string dlgTitle = i18n.tr("themeshop.music.download_title", "Downloading Music");
        std::string initialMsg = i18n.tr("themeshop.music.download_prep", "Preparing MP3 stream...");
        if (m_progressShowCb) {
            m_progressShowCb(dlgTitle, initialMsg, 0.05f);
        }

        auto onProgress = [this](const std::string& msg, float p) {
            if (m_progressUpdateCb) {
                m_progressUpdateCb(msg, p);
            }
        };

        std::string songTitle = track->title;
        auto onComplete = [this, songTitle](bool ok, const std::string& path, const std::string& err) {
            auto& i18nInner = nxui::I18n::instance();
            if (m_progressHideCb) {
                m_progressHideCb();
            }
            if (ok) {
                if (m_musicDownloadedCb) {
                    m_musicDownloadedCb();
                }
                std::string compTitle = i18nInner.tr("themeshop.music.finished_title", "Download Finished!");
                std::string compMsg = "'" + songTitle + "'\n\n"
                    + i18nInner.tr("themeshop.music.finished_msg", "was successfully downloaded to your music library!\n\nWould you like to play it now?");
                std::vector<DialogButtonDef> btns;
                btns.push_back({i18nInner.tr("themeshop.music.play_now", "Play Now"), [this, path, songTitle]() {
                    if (m_playMusicCb) {
                        m_playMusicCb(path, songTitle);
                    }
                }});
                btns.push_back({i18nInner.tr("button.ok", "OK"), [this]() {}});
                requestDialog(compTitle, compMsg, std::move(btns));
            } else {
                requestToast(i18nInner.tr("themeshop.music.download_error", "Download failed: ") + err, 4.0f);
            }
        };

        m_youTubeClient.downloadTrack(static_cast<size_t>(trackIdx), onProgress, onComplete);
        return;
    }

    if (isCommunityTab()) {
        const auto* entry = selectedCommunityThemeEntry();
        if (!entry)
            return;

        if (m_packageTransferState.isRunning()) {
            requestToast(i18n.tr("themeshop.community.transfer_busy",
                                 "Another theme transfer is already running."),
                         2.5f);
            return;
        }

        std::string themeId = entry->id;
        // Já instalado: baixar de novo não é o que alguém quer daqui, e era a
        // única coisa oferecida. As ações passam a ser as mesmas da aba de
        // instalados, sobre o preset que o pacote virou. (Covers stay warm, as
        // PoloNX #177 does for the download path.)
        if (const auto* installed = installedEntryForCatalogue(themeId)) {
            const std::string presetId = installed->id;
            closeDetail();
            if (buttonIndex == 0) {
                if (m_themeShopApplyCb) m_themeShopApplyCb(presetId);
            } else if (buttonIndex == 1) {
                if (m_themeShopDeleteCb) m_themeShopDeleteCb(presetId);
            }
            return;
        }

        if (buttonIndex == 0) {
            // Plain Install stays one-tap (Replace dialog still gates overwrites).
            if (m_themeShopDownloadCb) {
                m_themeShopDownloadCb(themeId);
            } else {
                requestToast(i18n.tr("themeshop.community.install_pending", "Install flow is not wired yet."), 2.5f);
            }
        } else if (buttonIndex == 1) {
            requestDialog(
                i18n.tr("themeshop.community.download_apply_confirm_title", "Download And Apply"),
                i18n.tr("themeshop.community.download_apply_confirm",
                        "Download this theme and apply it to HOME now?"),
                {{i18n.tr("button.cancel", "Cancel"), {}},
                 {i18n.tr("themeshop.community.download_apply", "Download + Apply"),
                  [this, themeId]() {
                      if (m_themeShopDownloadInstallCb)
                          m_themeShopDownloadInstallCb(themeId);
                      else
                          requestToast(nxui::I18n::instance().tr(
                                           "themeshop.community.download_apply_pending",
                                           "Download + apply flow is not wired yet."),
                                       2.5f);
                  }}});
        }
        return;
    }

    const auto* entry = selectedThemeShopEntry();
    if (!entry)
        return;

    std::string themeId = entry->id;
    bool removable = entry->removable;

    if (isIconsTab()) {
        if (!entry->installed) {
            if (buttonIndex == 0) {
                // Plain Install stays one-tap.
                closeDetail();
                if (m_iconPackDownloadCb)
                    m_iconPackDownloadCb(themeId);
                return;
            }
            requestDialog(
                i18n.tr("themeshop.icons.download_apply_confirm_title", "Download And Apply"),
                i18n.tr("themeshop.icons.download_apply_confirm",
                        "Download this icon pack and apply it now?"),
                {{i18n.tr("button.cancel", "Cancel"), {}},
                 {i18n.tr("themeshop.community.download_apply", "Download + Apply"),
                  [this, themeId]() {
                      closeDetail();
                      if (m_iconPackDownloadApplyCb)
                          m_iconPackDownloadApplyCb(themeId);
                  }}});
            return;
        }
        // Commit draft tint before closing so Apply publishes HSL + HOME chrome.
        if (buttonIndex == 0)
            commitDraftIconPackTint();
        closeDetail();
        if (buttonIndex == 0) {
            if (m_iconPackApplyCb)
                m_iconPackApplyCb(themeId);
        } else if (buttonIndex == 1 && removable && m_iconPackDeleteCb) {
            // App callback already shows a Remove confirm.
            m_iconPackDeleteCb(themeId);
        }
        return;
    }

    if (buttonIndex == 0) {
        closeDetail();
        if (m_themeShopApplyCb)
            m_themeShopApplyCb(themeId);
    } else if (buttonIndex == 1 && removable) {
        // Keep the product sheet up until Remove is confirmed in the app dialog.
        if (m_themeShopDeleteCb)
            m_themeShopDeleteCb(themeId);
    }
}

void ThemeShopScreen::updateCustomContent(float dt) {
    // Track tab changes even on Sounds/Interface. Those tabs early-out below, and
    // skipping this left m_lastCustomTabIndex stuck on Icons — Sounds→Icons then
    // looked like "no change" and kept Icon Shop content under a focused parent.
    if (m_lastCustomTabIndex != m_tabIndex) {
        m_lastCustomTabIndex = m_tabIndex;
        m_contentFocusArea = ContentFocusArea::Grid;
        closeDetail();
        if (isThemesTab() || isIconsTab()) {
            m_shopBranchReveal.setImmediate(0.f);
            m_shopBranchReveal.set(1.f, 0.22f, nxui::Easing::outCubic);
            m_shopBranchFocused = false;
            if (activeShelfMode() != ShelfMode::Installed)
                setActiveShelfMode(ShelfMode::Installed);
        } else {
            m_shopBranchReveal.set(0.f, 0.16f, nxui::Easing::outQuad);
            m_shopBranchFocused = false;
            m_shopBranchRect = {};
        }

        // The Theme Shop shelf and the Animated tab read one merged list and
        // are told apart by which index each entry came from -- and that split
        // happens in the filter. Without re-running it here the list stayed as
        // whatever the previous tab produced, so the two showed each other's
        // themes until somebody pressed refresh.
        applySearchFilter();
    }

    if (!usesCustomContentLayout())
        return;

    // ZL/ZR flip pages like HOME - Installed / Community / Icons. L/R already
    // do it through stepCataloguePage(), routed from WiiUMenuApp with its own
    // dialog / lock-screen gating and sound, so they are not polled here too
    // (both would step twice per press).
    if (m_input && m_showing && !m_detailOpen && pageCount() > 1) {
        if (m_input->isDown(nxui::Button::ZL))
            stepPage(-1);
        else if (m_input->isDown(nxui::Button::ZR))
            stepPage(1);
    }

    m_detailSheetAnim.update(std::min(dt, 0.03f));
    m_detailFullscreenAnim.update(std::min(dt, 0.03f));
    m_pageSlideAnim.update(std::min(dt, 0.03f));

    if (!m_showing) {
        closeDetail();
        // Soft-trim-only left ~10 MB Applet free — Frame Tap needs ~30 MB to
        // remount while SwitchU stays up. Hard-drop preview GPU once on hide;
        // catalog + SD disk cache stay warm for the next open.
        if (!m_previewPausedWhileHidden) {
            pausePreviewWorkForSleep();
            releasePreviewCachesOnHide();
            m_previewPausedWhileHidden = true;
        }
        return;
    }
    m_previewPausedWhileHidden = false;

    // Bootstrap Community catalog while on Installed; neighborhood prefetch only
    // off the Community tab (on-tab priming owns the visible shelf).
    ensureCommunityCatalogWarm();
    (void)pollCommunityCatalog();
    if (!isCommunityTab())
        prefetchCommunityShelfNeighborhood();

    if (isCommunityTab()) {
        if (m_hasPendingCommunityPreviewWork)
            syncFinishedCommunityPreviewLoads();
    } else if (isInstalledTab() || isIconsTab()) {
        if (!m_packageTransferState.isRunning())
            primeVisibleInstalledPreviews();
        syncFinishedInstalledPreviewLoads();
        // Drip one Community warm upload only when installed had nothing to do.
        if (!m_hasPendingInstalledPreviewWork && m_hasPendingCommunityPreviewWork)
            syncFinishedCommunityPreviewLoads();
    } else if (m_hasPendingCommunityPreviewWork) {
        syncFinishedCommunityPreviewLoads();
    }

    if (isMusicTab()) {
        m_youTubeClient.updateThumbnailTextures();
        m_youTubeClient.trimThumbnailCache();
    }

    const int headerButtonCount = (showsCatalogRefresh() ? 1 : 0) + 1;
    m_headerButtonIndex = std::clamp(m_headerButtonIndex, 0, std::max(0, headerButtonCount - 1));

    if (showsShelfBranch() && m_shopBranchReveal.value() < 0.999f) {
        // Keep reveal animating toward open while staying on Themes/Icons.
        if (m_shopBranchReveal.target() < 0.999f)
            m_shopBranchReveal.set(1.f, 0.22f, nxui::Easing::outCubic);
    }

    if (currentEntryCount() <= 0) {
        // An empty list used to push focus onto the tab bar, which put the
        // refresh button out of reach of the controller: the one screen where
        // refresh is the only useful action was the one where only touch could
        // press it. So focus is allowed to stay in the content area, landing on
        // the header instead of on a grid with nothing in it.
        //
        // Allowed, not forced. Forcing it stole focus back from the tab bar on
        // every frame while a catalogue was still loading, which read as the
        // selector being stuck between tabs until the download finished.
        if (m_focusArea == FocusArea::Content) {
            m_contentFocusArea = ContentFocusArea::Header;
            m_headerButtonIndex = 0;
        }
        currentScrollRowRef() = 0;
        closeDetail();
        return;
    }

    ensureSelectionVisible();

    if (isCommunityTab()) {
        // Keep covers during package install — only skip heavy neighbor primes
        // while the transfer is saturating the thread pool / network.
        clampDetailScreenshotIndex();
        int previewButtonCount = detailScreenshotCount() > 0 ? 1 : 0;
        if (previewButtonCount <= 0) {
            m_detailPreviewButtonIndex = 0;
            if (m_detailFocusArea == DetailFocusArea::Preview)
                m_detailFocusArea = DetailFocusArea::Buttons;
        } else {
            m_detailPreviewButtonIndex = 0;
        }
        if (!m_packageTransferState.isRunning()) {
            std::string primeKey = std::to_string(m_communityRevision)
                + "|" + m_communitySelectedId
                + "|" + std::to_string(m_communityScrollRow)
                + "|" + std::to_string(m_detailOpen ? 1 : 0)
                // A tela cheia pede outra imagem que o detalhe -- a folha grande no
                // lugar da pequena -- e nao constava aqui. Sem isso a chave nao
                // mudava ao expandir e a folha HD ficava em Idle: nunca pedida, e
                // por isso "captura indisponivel" para sempre.
                //
                // Medido no console: "tela cheia pede .../preview_sheet_hd.jpg
                // (estado=0 celulas=5x4)" -- estado 0 e Idle.
                + "|" + std::to_string(m_detailFullscreen ? 1 : 0)
                + "|" + std::to_string(m_detailScreenshotIndex);
            if (primeKey != m_lastPreviewPrimeKey) {
                // Never trim on page change — Texture dtor waitIdle hitch fought
                // cold flips (PoloNX #177). The timed trim below covers it.
                m_lastPreviewPrimeKey = std::move(primeKey);
            } else {
                // Tambem sem nada mudar. A poda so acontecia quando a chave mudava,
                // entao quem abria a aba e esperava as previas carregarem nunca
                // podava nada -- e e exatamente ai que elas chegam. A sessao que
                // derrubou o menu teve zero podas.
                m_previewTrimTimer += dt;
                if (m_previewTrimTimer >= 1.f) {
                    m_previewTrimTimer = 0.f;
                    trimCommunityPreviewCache();
                }
            }
            // Always re-attempt primes so Idle slots can start when a download slot frees.
            primeVisibleCommunityPreviews();
        }
    } else {
        // Away from Community: soft-trim at most occasionally — Texture dtor
        // waitIdle's, and rebuilding preferred sets every frame was hitchy.
        if (!m_communityPreviewCache.empty()
            && m_pageSlideAnim.value() <= 0.05f
            && m_communityPreviewCache.size() > 36u)
            trimCommunityPreviewCache();
        if (isInstalledTab() || isIconsTab())
            trimInstalledPreviewCache();
    }

    updateTintSliderAnalog(dt);
}

bool ThemeShopScreen::handleCustomPressA() {
    if (m_focusArea == FocusArea::Tabs) {
        if (currentEntryCount() > 0) {
            m_focusArea = FocusArea::Content;
            m_contentFocusArea = ContentFocusArea::Grid;
            if (m_navSfxCb) m_navSfxCb();
            return true;
        }

        {
            m_focusArea = FocusArea::Content;
            m_contentFocusArea = ContentFocusArea::Header;
            m_headerButtonIndex = 0;
            if (m_navSfxCb) m_navSfxCb();
            return true;
        }
    }

    if (!m_detailOpen && m_contentFocusArea == ContentFocusArea::Header) {
        const bool hasRefresh = showsCatalogRefresh();
        if (hasRefresh && m_headerButtonIndex == 0) {
            refreshCommunityCatalog(true);
        } else {
            promptSearchQuery();
        }
        if (m_activateSfxCb) m_activateSfxCb();
        return true;
    }

    if (!m_detailOpen && m_contentFocusArea == ContentFocusArea::Pager) {
        int before = currentPage();
        if (m_pageButtonIndex == 0)
            stepPage(-1);
        else
            stepPage(1);
        if (before != currentPage() && m_navSfxCb)
            m_navSfxCb();
        return true;
    }

    if (m_detailOpen) {
        if (m_detailFullscreen) {
            m_detailFullscreen = false;
            m_detailFullscreenAnim.set(0.f, 0.16f, nxui::Easing::outQuad);
            if (m_activateSfxCb) m_activateSfxCb();
            return true;
        }

        if (m_detailFocusArea == DetailFocusArea::Preview
            && (isCommunityTab() ? detailScreenshotCount() > 0
                                 : (selectedThemeShopEntry() && !selectedThemeShopEntry()->coverPath.empty()))) {
            m_detailFullscreen = true;
            m_detailFullscreenAnim.set(1.f, 0.18f, nxui::Easing::outCubic);
            if (m_activateSfxCb) m_activateSfxCb();
            return true;
        }

        if (m_detailFocusArea == DetailFocusArea::TintToggle) {
            setIconCustomTintEnabled(!selectedIconCustomTint());
            if (selectedIconCustomTint()) {
                m_detailFocusArea = selectedIconShowsHueSat()
                    ? DetailFocusArea::TintHue : DetailFocusArea::TintLight;
            }
            if (m_activateSfxCb) m_activateSfxCb();
            return true;
        }
        if (m_detailFocusArea == DetailFocusArea::MatchThemeToggle) {
            setIconMatchThemeEnabled(!selectedIconPackTint().matchTheme);
            if (m_activateSfxCb) m_activateSfxCb();
            return true;
        }
        if (m_detailFocusArea == DetailFocusArea::FlattenToggle) {
            setIconFlattenEnabled(!selectedIconPackTint().flatten);
            if (m_activateSfxCb) m_activateSfxCb();
            return true;
        }
        if (m_detailFocusArea == DetailFocusArea::GrayscaleToggle) {
            setIconGrayscaleEnabled(!selectedIconPackTint().grayscale);
            if (m_activateSfxCb) m_activateSfxCb();
            return true;
        }

        if (m_detailFocusArea == DetailFocusArea::TintHue
            || m_detailFocusArea == DetailFocusArea::TintSat
            || m_detailFocusArea == DetailFocusArea::TintLight) {
            if (m_activateSfxCb) m_activateSfxCb();
            return true;
        }

        activateDetailButton(m_detailButtonIndex);
        if (m_activateSfxCb) m_activateSfxCb();
        return true;
    }

    if (currentEntryCount() > 0) {
        openDetail();
        if (m_activateSfxCb) m_activateSfxCb();
        return true;
    }

    return false;
}

void ThemeShopScreen::currentAccessibilityParts(std::string& context,
                                                std::string& position,
                                                std::string& summary,
                                                bool& forceRepeat) const {
    if (!usesCustomContentLayout()) {
        TabbedOverlayScreen::currentAccessibilityParts(context, position, summary, forceRepeat);
        return;
    }

    context.clear();
    position.clear();
    summary.clear();
    forceRepeat = false;

    if (m_tabIndex < 0 || m_tabIndex >= (int)m_tabs.size())
        return;

    auto& i18n = nxui::I18n::instance();
    const std::string tabName = m_tabs[(size_t)m_tabIndex].name;
    if (m_focusArea == FocusArea::Tabs) {
        context = i18n.tr("accessibility.context.themes", "Themes");
        if (m_accessibilitySpeakPosition) {
            position = std::to_string(m_tabIndex + 1) + " "
                     + i18n.tr("accessibility.context.of", "of") + " "
                     + std::to_string((int)m_tabs.size());
        }
        if (tabBranchConsumesFocus()) {
            const std::string shopName = isIconsTab()
                ? i18n.tr("themeshop.icons.shop_item", "Icon Shop")
                : i18n.tr("themeshop.themes.shop_item", "Theme Shop");
            summary = shopName;
            if (m_accessibilitySpeakHints)
                summary += ". " + i18n.tr("accessibility.themeshop.shop_branch",
                    "A or right to enter shop. Up for installed. Down for next tab. B to close.");
        } else {
            summary = i18n.tr("accessibility.settings.tab_prefix", "Tab") + " " + tabName;
            if (showsShelfBranch() && m_accessibilitySpeakHints)
                summary += ". " + i18n.tr("accessibility.themeshop.shelf_branch",
                    "A or right to enter. Down for shop. Up and down to change tab. B to close.");
            else if (m_accessibilitySpeakHints)
                summary += ". " + i18n.tr("accessibility.settings.tab_actions",
                    "A or right to enter. Up and down to change tab. B to close.");
        }
        return;
    }

    context = tabName;

    if (m_detailOpen) {
        std::string themeName;
        std::string author;
        if (isMusicTab()) {
            if (const auto* track = selectedMusicTrack()) {
                themeName = track->title;
                author = track->author;
            }
        } else if (isCommunityTab()) {
            if (const auto* entry = selectedCommunityThemeEntry()) {
                themeName = entry->name;
                author = entry->author;
            }
        } else if (const auto* entry = selectedThemeShopEntry()) {
            themeName = entry->name;
            author = entry->author;
        }
        if (themeName.empty())
            themeName = i18n.tr("settings.tabs.theme", "Theme");

        if (m_detailFullscreen) {
            summary = i18n.tr("themeshop.accessibility.fullscreen_preview", "Fullscreen preview")
                    + ". " + themeName;
            if (m_accessibilitySpeakHints)
                summary += ". " + i18n.tr("themeshop.accessibility.fullscreen_actions", "B to close the preview.");
            return;
        }

        std::string area = (m_detailFocusArea == DetailFocusArea::Preview)
            ? i18n.tr("themeshop.accessibility.preview_area", "Preview")
            : i18n.tr("themeshop.accessibility.actions_area", "Actions");
        summary = i18n.tr("themeshop.accessibility.detail", "Detail")
                + ". " + themeName + ". " + area;
        if (!author.empty())
            summary += ". " + i18n.tr("themeshop.accessibility.author", "Author") + ": " + author;
        if (m_accessibilitySpeakHints)
            summary += ". " + i18n.tr("themeshop.accessibility.detail_actions", "Left and right to change control. A to confirm. B to go back.");
        return;
    }

    if (m_contentFocusArea == ContentFocusArea::Header) {
        summary = isCommunityTab()
            ? i18n.tr("themeshop.accessibility.search_refresh", "Search and refresh")
            : i18n.tr("themeshop.accessibility.search_bar", "Search bar");
        if (m_accessibilitySpeakHints) {
            summary += ". " + (isCommunityTab()
                ? i18n.tr("themeshop.accessibility.header_actions", "Left and right to choose. A to confirm. Down to the grid.")
                : i18n.tr("themeshop.accessibility.search_actions", "A to search. Down to the grid."));
        }
        return;
    }

    if (m_contentFocusArea == ContentFocusArea::Pager) {
        summary = i18n.tr("themeshop.accessibility.pagination", "Pagination");
        if (m_accessibilitySpeakHints)
            summary += ". " + i18n.tr("themeshop.accessibility.pagination_actions", "Left and right to change button. A to change page. Up to the grid.");
        return;
    }

    std::string themeName;
    std::string detail;
    if (isMusicTab()) {
        if (const auto* track = selectedMusicTrack()) {
            themeName = track->title;
            detail = track->author + (track->isDownloaded ? ", Downloaded" : "");
        }
    } else if (isCommunityTab()) {
        if (const auto* entry = selectedCommunityThemeEntry()) {
            themeName = entry->name;
            detail = entry->author;
        }
    } else if (const auto* entry = selectedThemeShopEntry()) {
        themeName = entry->name;
        detail = entry->source;
        if (entry->active)
            detail = detail.empty() ? "Actif" : detail + ", actif";
    }

    if (themeName.empty())
        themeName = i18n.tr("themeshop.accessibility.no_theme", "No theme");

    summary = themeName;
    if (!detail.empty())
        summary += ". " + detail;
    if (m_accessibilitySpeakPosition) {
        const int count = currentEntryCount();
        const int selected = currentSelectedIndex();
        if (selected >= 0 && count > 0) {
            position = std::to_string(selected + 1) + " "
                     + i18n.tr("accessibility.context.of", "of") + " "
                     + std::to_string(count);
        }
    }
    if (m_accessibilitySpeakHints)
        summary += ". " + i18n.tr("themeshop.accessibility.grid_actions", "Directional pad to navigate. A to open details. B to return to tabs.");
}

std::string ThemeShopScreen::currentAccessibilitySummary() const {
    std::string context;
    std::string position;
    std::string summary;
    bool forceRepeat = false;
    currentAccessibilityParts(context, position, summary, forceRepeat);
    std::string out = context;
    if (!summary.empty())
        out += (out.empty() ? "" : ". ") + summary;
    if (!position.empty())
        out += (out.empty() ? "" : ". ") + position;
    return out;
}

bool ThemeShopScreen::handleCustomPressB() {
    if (m_detailFullscreen) {
        m_detailFullscreen = false;
        m_detailFullscreenAnim.set(0.f, 0.16f, nxui::Easing::outQuad);
        if (m_closeSfxCb) m_closeSfxCb();
        return true;
    }

    if (m_detailOpen) {
        closeDetail();
        if (m_closeSfxCb) m_closeSfxCb();
        return true;
    }

    if (m_focusArea == FocusArea::Content) {
        m_focusArea = FocusArea::Tabs;
        if (m_navSfxCb) m_navSfxCb();
        return true;
    }

    return false;
}

bool ThemeShopScreen::handleCustomPressX() {
    bool handled = promptSearchQuery();
    if (handled && m_activateSfxCb)
        m_activateSfxCb();
    return handled;
}

bool ThemeShopScreen::handleCustomNavUp() {
    if (m_detailFullscreen)
        return true;
    if (m_detailOpen) {
        const bool canFocusPreview = isCommunityTab()
            ? detailScreenshotCount() > 0
            : (selectedThemeShopEntry() && !selectedThemeShopEntry()->coverPath.empty());
        if (showSelectedIconTintControls()) {
            // Apply Up → toggles → sliders (above) → Preview. Sliders sit above
            // toggles so the Apply path does not brush HSL and break Match.
            if (m_detailFocusArea == DetailFocusArea::Buttons) {
                m_detailFocusArea = DetailFocusArea::GrayscaleToggle;
                if (m_navSfxCb) m_navSfxCb();
            } else if (m_detailFocusArea == DetailFocusArea::GrayscaleToggle) {
                m_detailFocusArea = DetailFocusArea::FlattenToggle;
                if (m_navSfxCb) m_navSfxCb();
            } else if (m_detailFocusArea == DetailFocusArea::FlattenToggle) {
                m_detailFocusArea = DetailFocusArea::TintToggle;
                if (m_navSfxCb) m_navSfxCb();
            } else if (m_detailFocusArea == DetailFocusArea::TintToggle) {
                m_detailFocusArea = DetailFocusArea::MatchThemeToggle;
                if (m_navSfxCb) m_navSfxCb();
            } else if (m_detailFocusArea == DetailFocusArea::MatchThemeToggle) {
                if (selectedIconCustomTint())
                    m_detailFocusArea = DetailFocusArea::TintLight;
                else if (canFocusPreview)
                    m_detailFocusArea = DetailFocusArea::Preview;
                if (m_navSfxCb) m_navSfxCb();
            } else if (m_detailFocusArea == DetailFocusArea::TintLight) {
                m_detailFocusArea = selectedIconShowsHueSat()
                    ? DetailFocusArea::TintSat : (canFocusPreview ? DetailFocusArea::Preview
                                                                  : DetailFocusArea::MatchThemeToggle);
                if (m_navSfxCb) m_navSfxCb();
            } else if (m_detailFocusArea == DetailFocusArea::TintSat) {
                m_detailFocusArea = DetailFocusArea::TintHue;
                if (m_navSfxCb) m_navSfxCb();
            } else if (m_detailFocusArea == DetailFocusArea::TintHue && canFocusPreview) {
                m_detailFocusArea = DetailFocusArea::Preview;
                if (m_navSfxCb) m_navSfxCb();
            }
        } else if (canFocusPreview && m_detailFocusArea == DetailFocusArea::Buttons) {
            m_detailFocusArea = DetailFocusArea::Preview;
            if (m_navSfxCb) m_navSfxCb();
        }
        return true;
    }
    if (m_focusArea == FocusArea::Tabs && showsShelfBranch() && m_shopBranchFocused) {
        m_shopBranchFocused = false;
        setActiveShelfMode(ShelfMode::Installed);
        if (m_navSfxCb) m_navSfxCb();
        return true;
    }
    if (m_focusArea != FocusArea::Content)
        return false;

    if (m_contentFocusArea == ContentFocusArea::Header) {
        m_focusArea = FocusArea::Tabs;
        m_shopBranchFocused = showsShelfBranch() && activeShelfMode() == ShelfMode::Shop;
        if (m_navSfxCb) m_navSfxCb();
        return true;
    }

    if (m_contentFocusArea == ContentFocusArea::Pager) {
        if (currentEntryCount() > 0) {
            m_contentFocusArea = ContentFocusArea::Grid;
            int pageStart = currentPage() * kEntriesPerPage;
            int target = std::min(currentEntryCount() - 1,
                                  pageStart + (kVisibleRows - 1) * kGridCols + m_pageButtonIndex);
            setCurrentSelectedIndex(target);
            ensureSelectionVisible();
        }
        if (m_navSfxCb) m_navSfxCb();
        return true;
    }

    int count = currentEntryCount();
    if (count <= 0)
        return true;

    int selected = std::max(0, currentSelectedIndex());
    int pageStart = currentPage() * kEntriesPerPage;
    if (selected - kGridCols >= pageStart) {
        setCurrentSelectedIndex(selected - kGridCols);
        ensureSelectionVisible();
    } else {
        m_contentFocusArea = ContentFocusArea::Header;
        {
            const int headerButtonCount = (showsCatalogRefresh() ? 1 : 0) + 1;
            m_headerButtonIndex = std::clamp(selected % kGridCols, 0, headerButtonCount - 1);
        }
    }
    if (m_navSfxCb) m_navSfxCb();
    return true;
}

bool ThemeShopScreen::handleCustomNavDown() {
    if (m_focusArea == FocusArea::Tabs && showsShelfBranch()) {
        if (!m_shopBranchFocused) {
            m_shopBranchFocused = true;
            setActiveShelfMode(ShelfMode::Shop);
            if (m_navSfxCb) m_navSfxCb();
            return true;
        }
        m_shopBranchFocused = false;
        return false; // advance to the next main tab
    }

    if (m_detailFullscreen)
        return true;
    if (m_detailOpen) {
        // Preview → toggles → Apply. Sliders sit above toggles; reach them via
        // Custom-tint Right or Up from Match (not on the Apply path).
        if (m_detailFocusArea == DetailFocusArea::Preview) {
            if (showSelectedIconTintControls()) {
                m_detailFocusArea = DetailFocusArea::MatchThemeToggle;
                if (m_navSfxCb) m_navSfxCb();
                return true;
            }
            if (detailButtonCount() > 0) {
                m_detailFocusArea = DetailFocusArea::Buttons;
                if (m_navSfxCb) m_navSfxCb();
                return true;
            }
        }
        if (showSelectedIconTintControls()) {
            if (m_detailFocusArea == DetailFocusArea::TintHue) {
                m_detailFocusArea = selectedIconShowsHueSat()
                    ? DetailFocusArea::TintSat : DetailFocusArea::TintLight;
                if (m_navSfxCb) m_navSfxCb();
            } else if (m_detailFocusArea == DetailFocusArea::TintSat) {
                m_detailFocusArea = DetailFocusArea::TintLight;
                if (m_navSfxCb) m_navSfxCb();
            } else if (m_detailFocusArea == DetailFocusArea::TintLight) {
                m_detailFocusArea = DetailFocusArea::MatchThemeToggle;
                if (m_navSfxCb) m_navSfxCb();
            } else if (m_detailFocusArea == DetailFocusArea::MatchThemeToggle) {
                m_detailFocusArea = DetailFocusArea::TintToggle;
                if (m_navSfxCb) m_navSfxCb();
            } else if (m_detailFocusArea == DetailFocusArea::TintToggle) {
                m_detailFocusArea = DetailFocusArea::FlattenToggle;
                if (m_navSfxCb) m_navSfxCb();
            } else if (m_detailFocusArea == DetailFocusArea::FlattenToggle) {
                m_detailFocusArea = DetailFocusArea::GrayscaleToggle;
                if (m_navSfxCb) m_navSfxCb();
            } else if (m_detailFocusArea == DetailFocusArea::GrayscaleToggle) {
                if (detailButtonCount() > 0) {
                    m_detailFocusArea = DetailFocusArea::Buttons;
                    if (m_navSfxCb) m_navSfxCb();
                }
            }
        }
        return true;
    }
    if (m_focusArea != FocusArea::Content)
        return false;

    if (m_contentFocusArea == ContentFocusArea::Header) {
        if (currentEntryCount() > 0)
            m_contentFocusArea = ContentFocusArea::Grid;
        if (m_navSfxCb) m_navSfxCb();
        return true;
    }

    if (m_contentFocusArea == ContentFocusArea::Pager)
        return true;

    int count = currentEntryCount();
    if (count <= 0)
        return true;

    int selected = std::max(0, currentSelectedIndex());
    int pageEnd = std::min(count, (currentPage() + 1) * kEntriesPerPage);
    if (selected + kGridCols < pageEnd) {
        setCurrentSelectedIndex(selected + kGridCols);
        ensureSelectionVisible();
        if (m_navSfxCb) m_navSfxCb();
    } else {
        m_contentFocusArea = ContentFocusArea::Pager;
        m_pageButtonIndex = std::clamp(selected % kGridCols, 0, 1);
        if (m_navSfxCb) m_navSfxCb();
    }
    return true;
}

bool ThemeShopScreen::handleCustomNavLeft() {
    if (m_detailFullscreen) {
        int before = m_detailScreenshotIndex;
        stepDetailScreenshot(-1);
        if (before != m_detailScreenshotIndex && m_navSfxCb) m_navSfxCb();
        return true;
    }

    if (m_detailOpen) {
        if (isCommunityTab() && m_detailFocusArea == DetailFocusArea::Preview) {
            int before = m_detailScreenshotIndex;
            stepDetailScreenshot(-1);
            if (before != m_detailScreenshotIndex && m_navSfxCb) m_navSfxCb();
            return true;
        }

        if (m_detailFocusArea == DetailFocusArea::TintToggle
            || m_detailFocusArea == DetailFocusArea::MatchThemeToggle
            || m_detailFocusArea == DetailFocusArea::FlattenToggle
            || m_detailFocusArea == DetailFocusArea::GrayscaleToggle) {
            // Prefer returning to the preview; A toggles on/off.
            const bool canPreview = selectedThemeShopEntry()
                && !selectedThemeShopEntry()->coverPath.empty();
            if (canPreview)
                m_detailFocusArea = DetailFocusArea::Preview;
            if (m_navSfxCb) m_navSfxCb();
            return true;
        }
        if (m_detailFocusArea == DetailFocusArea::TintHue) {
            nudgeIconTintHue(-1.f / 36.f);
            if (m_navSfxCb) m_navSfxCb();
            return true;
        }
        if (m_detailFocusArea == DetailFocusArea::TintSat) {
            nudgeIconTintSat(-1.f / 20.f);
            if (m_navSfxCb) m_navSfxCb();
            return true;
        }
        if (m_detailFocusArea == DetailFocusArea::TintLight) {
            nudgeIconTintLight(-1.f / 20.f);
            if (m_navSfxCb) m_navSfxCb();
            return true;
        }

        // Icons: Left from Apply → pack preview.
        if (m_detailFocusArea == DetailFocusArea::Buttons
            && m_detailButtonIndex == 0
            && isIconsTab()
            && selectedThemeShopEntry()
            && !selectedThemeShopEntry()->coverPath.empty()) {
            m_detailFocusArea = DetailFocusArea::Preview;
            if (m_navSfxCb) m_navSfxCb();
            return true;
        }

        if (m_detailButtonIndex > 0) {
            --m_detailButtonIndex;
            if (m_navSfxCb) m_navSfxCb();
        }
        return true;
    }

    if (m_focusArea != FocusArea::Content)
        return false;

    if (m_contentFocusArea == ContentFocusArea::Header) {
        if (m_headerButtonIndex > 0) {
            --m_headerButtonIndex;
        } else {
            m_focusArea = FocusArea::Tabs;
            m_shopBranchFocused = showsShelfBranch() && activeShelfMode() == ShelfMode::Shop;
        }
        if (m_navSfxCb) m_navSfxCb();
        return true;
    }

    if (m_contentFocusArea == ContentFocusArea::Pager) {
        if (m_pageButtonIndex > 0)
            --m_pageButtonIndex;
        else {
            m_focusArea = FocusArea::Tabs;
            m_shopBranchFocused = showsShelfBranch() && activeShelfMode() == ShelfMode::Shop;
        }
        if (m_navSfxCb) m_navSfxCb();
        return true;
    }

    int count = currentEntryCount();
    if (count <= 0)
        return true;

    int selected = std::max(0, currentSelectedIndex());
    if (selected % kGridCols > 0) {
        setCurrentSelectedIndex(selected - 1);
    } else {
        m_focusArea = FocusArea::Tabs;
        m_shopBranchFocused = showsShelfBranch() && activeShelfMode() == ShelfMode::Shop;
    }
    if (m_navSfxCb) m_navSfxCb();
    return true;
}

bool ThemeShopScreen::handleCustomNavRight() {
    if (m_detailFullscreen) {
        int before = m_detailScreenshotIndex;
        stepDetailScreenshot(1);
        if (before != m_detailScreenshotIndex && m_navSfxCb) m_navSfxCb();
        return true;
    }

    if (m_detailOpen) {
        if (isCommunityTab() && m_detailFocusArea == DetailFocusArea::Preview) {
            int before = m_detailScreenshotIndex;
            stepDetailScreenshot(1);
            if (before != m_detailScreenshotIndex && m_navSfxCb) m_navSfxCb();
            return true;
        }
        // Icons: Preview Right -> Match theme (when the pack supports tint).
        if (isIconsTab() && m_detailFocusArea == DetailFocusArea::Preview
            && showSelectedIconTintControls()) {
            m_detailFocusArea = DetailFocusArea::MatchThemeToggle;
            if (m_navSfxCb) m_navSfxCb();
            return true;
        }
        // Icons: Apply Right stays on buttons; colour packs also allow
        // Apply → Match when only one primary action is focused.
        if (isIconsTab() && m_detailFocusArea == DetailFocusArea::Buttons
            && m_detailButtonIndex + 1 >= detailButtonCount()
            && showSelectedIconTintControls()) {
            m_detailFocusArea = DetailFocusArea::MatchThemeToggle;
            if (m_navSfxCb) m_navSfxCb();
            return true;
        }

        if (m_detailFocusArea == DetailFocusArea::MatchThemeToggle) {
            m_detailFocusArea = DetailFocusArea::TintToggle;
            if (m_navSfxCb) m_navSfxCb();
            return true;
        }
        if (m_detailFocusArea == DetailFocusArea::TintToggle) {
            setIconCustomTintEnabled(true);
            m_detailFocusArea = selectedIconShowsHueSat()
                ? DetailFocusArea::TintHue : DetailFocusArea::TintLight;
            if (m_navSfxCb) m_navSfxCb();
            return true;
        }
        if (m_detailFocusArea == DetailFocusArea::TintHue) {
            nudgeIconTintHue(1.f / 36.f);
            if (m_navSfxCb) m_navSfxCb();
            return true;
        }
        if (m_detailFocusArea == DetailFocusArea::TintSat) {
            nudgeIconTintSat(1.f / 20.f);
            if (m_navSfxCb) m_navSfxCb();
            return true;
        }
        if (m_detailFocusArea == DetailFocusArea::TintLight) {
            nudgeIconTintLight(1.f / 20.f);
            if (m_navSfxCb) m_navSfxCb();
            return true;
        }

        if (m_detailButtonIndex + 1 < detailButtonCount()) {
            ++m_detailButtonIndex;
            if (m_navSfxCb) m_navSfxCb();
        }
        return true;
    }

    if (m_focusArea == FocusArea::Tabs) {
        if (currentEntryCount() > 0) {
            m_focusArea = FocusArea::Content;
            m_contentFocusArea = ContentFocusArea::Grid;
            if (m_navSfxCb) m_navSfxCb();
            return true;
        }
        {
            m_focusArea = FocusArea::Content;
            m_contentFocusArea = ContentFocusArea::Header;
            m_headerButtonIndex = 0;
            if (m_navSfxCb) m_navSfxCb();
            return true;
        }
    }

    if (m_contentFocusArea == ContentFocusArea::Header) {
        const int headerButtonCount = (showsCatalogRefresh() ? 1 : 0) + 1;
        if (m_headerButtonIndex + 1 < headerButtonCount) {
            ++m_headerButtonIndex;
        } else if (currentEntryCount() > 0) {
            m_contentFocusArea = ContentFocusArea::Grid;
        }
        if (m_navSfxCb) m_navSfxCb();
        return true;
    }

    if (m_contentFocusArea == ContentFocusArea::Pager) {
        if (m_pageButtonIndex < 1)
            ++m_pageButtonIndex;
        if (m_navSfxCb) m_navSfxCb();
        return true;
    }

    int count = currentEntryCount();
    if (count <= 0)
        return true;

    int selected = std::max(0, currentSelectedIndex());
    if (selected + 1 < count && selected % kGridCols < kGridCols - 1) {
        setCurrentSelectedIndex(selected + 1);
        if (m_navSfxCb) m_navSfxCb();
    }
    return true;
}

int ThemeShopScreen::hitTestGridCard(const nxui::Rect& content, float x, float y) const {
    GridLayout layout = makeGridLayout(content, showsCatalogRefresh());
    int count = currentEntryCount();
    if (count <= 0 || !layout.grid.contains(x, y))
        return -1;

    int scrollRow = isCommunityTab() ? m_communityScrollRow
                  : (isIconsTab() ? m_iconPackScrollRow : m_installedScrollRow);
    int start = scrollRow * kGridCols;
    int end = std::min(count, start + kGridCols * kVisibleRows);
    for (int i = start; i < end; ++i) {
        int local = i - start;
        if (gridCardRect(layout, local).contains(x, y))
            return i;
    }

    return -1;
}

bool ThemeShopScreen::handleCustomTouch(nxui::Input& input, const nxui::Rect&, const nxui::Rect& tabs, const nxui::Rect& content) {
    auto searchHit = [&](float x, float y) {
        return makeGridLayout(content, showsCatalogRefresh())
            .searchButton.contains(x, y);
    };
    auto refreshHit = [&](float x, float y) {
        return showsCatalogRefresh()
            && makeGridLayout(content, showsCatalogRefresh())
                   .refreshButton.contains(x, y);
    };
    auto pagePrevHit = [&](float x, float y) {
        return pageCount() > 1
            && makeGridLayout(content, showsCatalogRefresh())
                   .pagePrevButton.contains(x, y);
    };
    auto pageNextHit = [&](float x, float y) {
        return pageCount() > 1
            && makeGridLayout(content, showsCatalogRefresh())
                   .pageNextButton.contains(x, y);
    };
    auto installedHasCover = [&]() {
        const auto* entry = selectedThemeShopEntry();
        return entry && !entry->coverPath.empty();
    };
    auto detailPreviewInteractCount = [&]() {
        if (isCommunityTab())
            return detailScreenshotCount();
        return installedHasCover() ? 1 : 0;
    };
    auto detailPreviewHitIndex = [&](float x, float y) {
        if (!m_detailOpen)
            return -1;
        const int count = detailPreviewInteractCount();
        if (count <= 0)
            return -1;

        nxui::Rect dialog = detailDialogRect(content);
        nxui::Rect previewRect = isIconsTab() && showSelectedIconTintControls()
            ? colourIconPreviewBounds(dialog)
            : detailPreviewRect(dialog);
        DetailPreviewControls controls = detailPreviewControls(previewRect);
        auto rects = detailPreviewControlRects(controls, previewRect, count);
        for (int i = 0; i < (int)rects.size(); ++i) {
            if (rects[(size_t)i].contains(x, y))
                return i;
        }
        return -1;
    };
    auto fullscreenHitIndex = [&](float x, float y) {
        FullscreenOverlayLayout overlay = makeFullscreenOverlayLayout(content);
        if (detailScreenshotCount() > 1 && overlay.prev.contains(x, y))
            return 0;
        if (detailScreenshotCount() > 1 && overlay.next.contains(x, y))
            return 1;
        if (overlay.close.contains(x, y) || overlay.preview.contains(x, y) || content.contains(x, y))
            return 2;
        return -1;
    };

    if (input.touchDown()) {
        float x = input.touchX();
        float y = input.touchY();
        m_themeTouchTarget = ThemeTouchTarget::None;
        m_themeTouchIndex = -1;
        m_themeTouchStartX = x;
        m_themeTouchStartY = y;

        if (tabs.contains(x, y))
            return false;
        if (!content.contains(x, y) && !m_detailOpen)
            return false;

        if (m_detailOpen) {
            if (m_detailFullscreen) {
                m_themeTouchTarget = ThemeTouchTarget::FullscreenPreview;
                m_themeTouchIndex = fullscreenHitIndex(x, y);
                return true;
            }

            nxui::Rect dialog = detailDialogRect(content);
            if (!dialog.contains(x, y)) {
                m_themeTouchTarget = ThemeTouchTarget::DetailBackdrop;
                return true;
            }

            int previewHit = detailPreviewHitIndex(x, y);
            if (previewHit >= 0) {
                m_themeTouchTarget = ThemeTouchTarget::DetailPreviewControl;
                m_themeTouchIndex = previewHit;
                return true;
            }

            if (showSelectedIconTintControls()) {
                nxui::Rect previewRect = colourIconPreviewBounds(dialog);
                IconTintLayout tintLayout = makeIconTintLayout(
                    colourIconTintBand(dialog, previewRect, selectedIconCustomTint()),
                    selectedIconCustomTint(),
                    selectedIconShowsHueSat());
                if (tintLayout.matchPill.expanded(8.f).contains(x, y)) {
                    m_themeTouchTarget = ThemeTouchTarget::DetailMatchThemeToggle;
                    return true;
                }
                if (tintLayout.tintPill.expanded(8.f).contains(x, y)) {
                    m_themeTouchTarget = ThemeTouchTarget::DetailTintToggle;
                    return true;
                }
                if (tintLayout.flattenPill.expanded(8.f).contains(x, y)) {
                    m_themeTouchTarget = ThemeTouchTarget::DetailFlattenToggle;
                    return true;
                }
                if (tintLayout.grayPill.expanded(8.f).contains(x, y)) {
                    m_themeTouchTarget = ThemeTouchTarget::DetailGrayscaleToggle;
                    return true;
                }
                if (selectedIconCustomTint()) {
                    auto hitTrack = [&](const nxui::Rect& track, ThemeTouchTarget target,
                                       auto setter) {
                        if (!track.expanded(8.f).contains(x, y))
                            return false;
                        m_themeTouchTarget = target;
                        const float t = std::clamp((x - track.x) / std::max(1.f, track.width), 0.f, 1.f);
                        setter(t);
                        return true;
                    };
                    if (selectedIconShowsHueSat()) {
                        if (hitTrack(tintLayout.hueTrack, ThemeTouchTarget::DetailTintHue,
                                     [this](float t) { setIconTintHue(t); }))
                            return true;
                        if (hitTrack(tintLayout.satTrack, ThemeTouchTarget::DetailTintSat,
                                     [this](float t) { setIconTintSat(t); }))
                            return true;
                        if (hitTrack(tintLayout.lightTrack, ThemeTouchTarget::DetailTintLight,
                                     [this](float t) { setIconTintLight(t); }))
                            return true;
                    } else if (hitTrack(tintLayout.hueTrack, ThemeTouchTarget::DetailTintLight,
                                        [this](float t) { setIconTintLight(t); })) {
                        return true;
                    }
                }
            }

            auto buttons = detailButtonRects(dialog, detailButtonCount());
            for (int i = 0; i < (int)buttons.size(); ++i) {
                if (buttons[(size_t)i].contains(x, y)) {
                    m_themeTouchTarget = ThemeTouchTarget::DetailButton;
                    m_themeTouchIndex = i;
                    return true;
                }
            }

            return true;
        }

        if (searchHit(x, y)) {
            m_themeTouchTarget = ThemeTouchTarget::Search;
            return true;
        }

        if (refreshHit(x, y)) {
            m_themeTouchTarget = ThemeTouchTarget::Refresh;
            return true;
        }

        if (pagePrevHit(x, y)) {
            m_themeTouchTarget = ThemeTouchTarget::PagePrev;
            return true;
        }

        if (pageNextHit(x, y)) {
            m_themeTouchTarget = ThemeTouchTarget::PageNext;
            return true;
        }

        int hit = hitTestGridCard(content, x, y);
        if (hit >= 0) {
            m_themeTouchTarget = ThemeTouchTarget::GridCard;
            m_themeTouchIndex = hit;
            return true;
        }

        return content.contains(x, y);
    }

    if (input.isTouching() && m_themeTouchTarget != ThemeTouchTarget::None) {
        if ((m_themeTouchTarget == ThemeTouchTarget::DetailTintHue
             || m_themeTouchTarget == ThemeTouchTarget::DetailTintSat
             || m_themeTouchTarget == ThemeTouchTarget::DetailTintLight)
            && showSelectedIconTintControls()) {
            nxui::Rect dialog = detailDialogRect(content);
            nxui::Rect previewRect = colourIconPreviewBounds(dialog);
            IconTintLayout tintLayout = makeIconTintLayout(
                colourIconTintBand(dialog, previewRect, selectedIconCustomTint()),
                selectedIconCustomTint(),
                selectedIconShowsHueSat());
            const nxui::Rect* track = &tintLayout.hueTrack;
            if (m_themeTouchTarget == ThemeTouchTarget::DetailTintSat)
                track = &tintLayout.satTrack;
            else if (m_themeTouchTarget == ThemeTouchTarget::DetailTintLight)
                track = &tintLayout.lightTrack;
            const float t = std::clamp((input.touchX() - track->x)
                                           / std::max(1.f, track->width),
                                       0.f, 1.f);
            if (m_themeTouchTarget == ThemeTouchTarget::DetailTintHue)
                setIconTintHue(t);
            else if (m_themeTouchTarget == ThemeTouchTarget::DetailTintSat)
                setIconTintSat(t);
            else
                setIconTintLight(t);
            return true;
        }
        float dx = std::abs(input.touchX() - m_themeTouchStartX);
        float dy = std::abs(input.touchY() - m_themeTouchStartY);
        if (dx > 18.f || dy > 18.f) {
            m_themeTouchTarget = ThemeTouchTarget::None;
            m_themeTouchIndex = -1;
        }
        return true;
    }

    if (input.touchUp()) {
        float x = input.touchX();
        float y = input.touchY();
        ThemeTouchTarget target = m_themeTouchTarget;
        int hitIndex = m_themeTouchIndex;
        m_themeTouchTarget = ThemeTouchTarget::None;
        m_themeTouchIndex = -1;

        if (target == ThemeTouchTarget::None)
            return content.contains(x, y);

        switch (target) {
            case ThemeTouchTarget::Search:
                if (searchHit(x, y)) {
                    promptSearchQuery();
                    if (m_activateSfxCb) m_activateSfxCb();
                }
                return true;
            case ThemeTouchTarget::Refresh:
                if (refreshHit(x, y)) {
                    refreshCommunityCatalog(true);
                    if (m_activateSfxCb) m_activateSfxCb();
                }
                return true;
            case ThemeTouchTarget::PagePrev:
                if (pagePrevHit(x, y)) {
                    m_contentFocusArea = ContentFocusArea::Pager;
                    m_pageButtonIndex = 0;
                    int before = currentPage();
                    stepPage(-1);
                    if (before != currentPage() && m_navSfxCb) m_navSfxCb();
                }
                return true;
            case ThemeTouchTarget::PageNext:
                if (pageNextHit(x, y)) {
                    m_contentFocusArea = ContentFocusArea::Pager;
                    m_pageButtonIndex = 1;
                    int before = currentPage();
                    stepPage(1);
                    if (before != currentPage() && m_navSfxCb) m_navSfxCb();
                }
                return true;
            case ThemeTouchTarget::GridCard:
                if (hitTestGridCard(content, x, y) == hitIndex) {
                    bool alreadyFocused = (m_focusArea == FocusArea::Content
                        && m_contentFocusArea == ContentFocusArea::Grid
                        && currentSelectedIndex() == hitIndex);
                    setCurrentSelectedIndex(hitIndex);
                    m_focusArea = FocusArea::Content;
                    m_contentFocusArea = ContentFocusArea::Grid;
                    ensureSelectionVisible();
                    if (alreadyFocused) {
                        openDetail();
                        if (m_activateSfxCb) m_activateSfxCb();
                    } else if (m_navSfxCb) {
                        m_navSfxCb();
                    }
                }
                return true;
            case ThemeTouchTarget::DetailPreviewControl:
                if (hitIndex == detailPreviewHitIndex(x, y)) {
                    const int shots = detailPreviewInteractCount();
                    if (hitIndex == 0) {
                        if (shots > 1) {
                            int before = m_detailScreenshotIndex;
                            stepDetailScreenshot(-1);
                            if (before != m_detailScreenshotIndex && m_navSfxCb) m_navSfxCb();
                        } else if (shots == 1) {
                            m_detailFocusArea = DetailFocusArea::Preview;
                            m_detailFullscreen = true;
                            m_detailFullscreenAnim.set(1.f, 0.18f, nxui::Easing::outCubic);
                            if (m_activateSfxCb) m_activateSfxCb();
                        }
                    } else if ((hitIndex == 1 && shots > 1)
                            || (hitIndex == 0 && shots == 1)) {
                        m_detailFocusArea = DetailFocusArea::Preview;
                        m_detailFullscreen = true;
                        m_detailFullscreenAnim.set(1.f, 0.18f, nxui::Easing::outCubic);
                        if (m_activateSfxCb) m_activateSfxCb();
                    } else if (hitIndex == 2 && shots > 1) {
                        int before = m_detailScreenshotIndex;
                        stepDetailScreenshot(1);
                        if (before != m_detailScreenshotIndex && m_navSfxCb) m_navSfxCb();
                    }
                }
                return true;
            case ThemeTouchTarget::DetailButton: {
                nxui::Rect dialog = detailDialogRect(content);
                auto buttons = detailButtonRects(dialog, detailButtonCount());
                if (hitIndex >= 0 && hitIndex < (int)buttons.size() && buttons[(size_t)hitIndex].contains(x, y)) {
                    m_detailButtonIndex = hitIndex;
                    activateDetailButton(hitIndex);
                    if (m_activateSfxCb) m_activateSfxCb();
                }
                return true;
            }
            case ThemeTouchTarget::DetailTintToggle: {
                if (showSelectedIconTintControls()) {
                    setIconCustomTintEnabled(!selectedIconCustomTint());
                    m_detailFocusArea = selectedIconCustomTint()
                        ? (selectedIconShowsHueSat() ? DetailFocusArea::TintHue
                                                     : DetailFocusArea::TintLight)
                        : DetailFocusArea::TintToggle;
                    if (m_activateSfxCb) m_activateSfxCb();
                }
                return true;
            }
            case ThemeTouchTarget::DetailMatchThemeToggle: {
                if (showSelectedIconTintControls()) {
                    setIconMatchThemeEnabled(!selectedIconPackTint().matchTheme);
                    m_detailFocusArea = DetailFocusArea::MatchThemeToggle;
                    if (m_activateSfxCb) m_activateSfxCb();
                }
                return true;
            }
            case ThemeTouchTarget::DetailFlattenToggle: {
                if (showSelectedIconTintControls()) {
                    setIconFlattenEnabled(!selectedIconPackTint().flatten);
                    m_detailFocusArea = DetailFocusArea::FlattenToggle;
                    if (m_activateSfxCb) m_activateSfxCb();
                }
                return true;
            }
            case ThemeTouchTarget::DetailGrayscaleToggle: {
                if (showSelectedIconTintControls()) {
                    setIconGrayscaleEnabled(!selectedIconPackTint().grayscale);
                    m_detailFocusArea = DetailFocusArea::GrayscaleToggle;
                    if (m_activateSfxCb) m_activateSfxCb();
                }
                return true;
            }
            case ThemeTouchTarget::DetailTintHue:
                m_detailFocusArea = DetailFocusArea::TintHue;
                return true;
            case ThemeTouchTarget::DetailTintSat:
                m_detailFocusArea = DetailFocusArea::TintSat;
                return true;
            case ThemeTouchTarget::DetailTintLight:
                m_detailFocusArea = DetailFocusArea::TintLight;
                return true;
            case ThemeTouchTarget::FullscreenPreview:
                if (hitIndex == fullscreenHitIndex(x, y)) {
                    if (hitIndex == 0) {
                        int before = m_detailScreenshotIndex;
                        stepDetailScreenshot(-1);
                        if (before != m_detailScreenshotIndex && m_navSfxCb) m_navSfxCb();
                    } else if (hitIndex == 1) {
                        int before = m_detailScreenshotIndex;
                        stepDetailScreenshot(1);
                        if (before != m_detailScreenshotIndex && m_navSfxCb) m_navSfxCb();
                    } else if (hitIndex == 2) {
                        m_detailFullscreen = false;
                        m_detailFullscreenAnim.set(0.f, 0.16f, nxui::Easing::outQuad);
                        if (m_closeSfxCb) m_closeSfxCb();
                    }
                }
                return true;
            case ThemeTouchTarget::DetailBackdrop:
                if (!detailDialogRect(content).contains(x, y)) {
                    closeDetail();
                    if (m_closeSfxCb) m_closeSfxCb();
                }
                return true;
            case ThemeTouchTarget::None:
                return false;
        }
    }

    return false;
}

void ThemeShopScreen::drawCustomContent(nxui::Renderer& ren, const nxui::Rect&, const nxui::Rect& content, float opacity) {
    auto& i18n = nxui::I18n::instance();
    GridLayout layout = makeGridLayout(content, showsCatalogRefresh());
    float slideT = std::clamp(m_contentSlideAnim.value(), 0.f, 1.f);
    float slideOffset = (1.f - slideT) * 24.f * (float)m_tabSwitchDir;
    float contentOpacity = opacity * slideT;
    int count = currentEntryCount();

    if (m_renderDiagnosticsActive) {
        const ThemeShopEntry* installed = selectedThemeShopEntry();
        const ThemeCatalogClient::Entry* community = selectedCommunityThemeEntry();
        DebugLog::log("[themeshop-render] tab=%d count=%d installedCount=%zu communityCount=%zu selectedInstalled=%s selectedCommunity=%s detail=%d fullscreen=%d focusArea=%d contentFocus=%d active=%d visible=%d opacity=%.2f contentOpacity=%.2f",
                      m_tabIndex,
                      count,
                      m_themeShopEntries.size(),
                      m_communityEntries.size(),
                      installed ? installed->id.c_str() : "<none>",
                      community ? community->id.c_str() : "<none>",
                      m_detailOpen ? 1 : 0,
                      m_detailFullscreen ? 1 : 0,
                      (int)m_focusArea,
                      (int)m_contentFocusArea,
                      isActive() ? 1 : 0,
                      isVisible() ? 1 : 0,
                      opacity,
                      contentOpacity);
    }

    layout.header.y += slideOffset;
    layout.refreshButton.y += slideOffset;
    layout.searchButton.y += slideOffset;
    layout.pagePrevButton.y += slideOffset;
    layout.pageCounter.y += slideOffset;
    layout.pageNextButton.y += slideOffset;
    layout.grid.y += slideOffset;

    // O catalogo da loja e a aba Animados compartilham a renderizacao, mas nao
    // o assunto: uma traz os temas animados deste projeto e a outra os
    // estaticos do PoloNX.
    std::string title, subtitle;
    if (isMusicTab()) {
        if (!m_youtubeSearchQuery.empty()) {
            title = i18n.tr("themeshop.music.results_title", "YouTube Search Results");
            subtitle = i18n.tr("themeshop.music.results_subtitle", "Online songs found for: ") + "'" + m_youtubeSearchQuery + "'";
        } else {
            title = i18n.tr("themeshop.music.installed_title", "Installed Music Library");
            subtitle = i18n.tr("themeshop.music.installed_subtitle", "Your local songs saved in sdmc:/config/SwitchU/music/");
        }
    } else if (isIconsTab()) {
        title = isIconsShopMode()
            ? i18n.tr("themeshop.icons.title_shop", "Icon Shop")
            : i18n.tr("themeshop.icons.title_installed", "Installed Icons");
        subtitle = isIconsShopMode()
            ? i18n.tr("themeshop.icons.subtitle_shop", "Download icon packs from the catalog.")
            : i18n.tr("themeshop.icons.subtitle_installed", "Icon packs on this console.");
    } else if (isAnimatedTab()) {
        title = i18n.tr("themeshop.animated.title", "Animated Themes");
        subtitle = i18n.tr("themeshop.animated.subtitle", "Moving wallpapers, with sound when the theme brings it.");
    } else if (isCommunityTab()) {
        title = i18n.tr("themeshop.themes.title_shop", "Theme Shop");
        subtitle = i18n.tr("themeshop.themes.subtitle_shop", "Download themes from the catalog.");
    } else {
        title = i18n.tr("themeshop.themes.title_installed", "Installed Themes");
        subtitle = i18n.tr("themeshop.themes.subtitle_installed", "Themes on this console.");
    }

    ren.drawText(title, {layout.header.x, layout.header.y + 0.f}, m_font,
                 m_theme->textPrimary.withAlpha(contentOpacity), 1.16f);
    ren.drawText(subtitle, {layout.header.x, layout.header.y + 34.f}, m_smallFont,
                 m_theme->textSecondary.withAlpha(0.78f * contentOpacity), 0.72f);

    const bool hasRefresh = showsCatalogRefresh();
    const int searchHeaderIndex = hasRefresh ? 1 : 0;
    const int refreshHeaderIndex = 0;

    // What the whole catalogue costs, before anything is chosen.
    //
    // Counted from the entries as they arrive, so a theme published tomorrow is
    // in this line the next time the console reads the index -- there is no
    // number to keep in step by hand. The search filter is deliberately not
    // applied: this answers "what is there", not "what am I looking at".
    {
        std::string totals;
        if (isMusicTab()) {
            if (m_youtubeSearchQuery.empty()) {
                totals = installedMusicTotals();
            }
        } else if (isCommunityTab()) {
            totals = communityCatalogueTotals();
        } else if (isInstalledTab()) {
            totals = installedThemeTotals();
        }
        if (!totals.empty()) {
            ren.drawText(totals, {layout.header.x, layout.header.y + 60.f}, m_smallFont,
                         m_theme->textSecondary.withAlpha(0.70f * contentOpacity), 0.68f);
        }
    }

    std::string searchLabel = isMusicTab()
        ? (!m_youtubeSearchQuery.empty() ? m_youtubeSearchQuery : i18n.tr("themeshop.music.search_button", "Search YouTube"))
        : (!m_searchQuery.empty() ? m_searchQuery : i18n.tr("themeshop.search.button", "Search"));
    bool searchSelected = !m_detailOpen && m_focusArea == FocusArea::Content && m_contentFocusArea == ContentFocusArea::Header
        && m_headerButtonIndex == searchHeaderIndex;
    drawActionButtonChip(ren,
                         m_smallFont,
                         layout.searchButton,
                         searchLabel,
                         m_theme,
                         m_theme->textPrimary,
                         contentOpacity,
                         searchSelected ? 1.f : 0.f,
                         searchSelected ? 1.f : (m_searchQuery.empty() && m_youtubeSearchQuery.empty() ? 0.f : 0.72f),
                         0.78f);

    std::string counterText;
    if (isMusicTab() && m_youTubeClient.isSearching()) {
        counterText = i18n.tr("themeshop.music.searching", "Searching");
    } else if (count > 0) {
        int selected = std::max(0, currentSelectedIndex()) + 1;
        counterText = std::to_string(selected) + " / " + std::to_string(count);
    } else if (isCommunityTab() && m_communityTransferState.isRunning()) {
        counterText = i18n.tr("themeshop.community.counter_loading", "Loading");
    } else {
        counterText = "0 / 0";
    }
    nxui::Vec2 countSize = measureTextCached(m_font, counterText);
    float countRight = layout.searchButton.x - 18.f;
    if (hasRefresh && layout.refreshButton.width > 0.f)
        countRight = layout.refreshButton.x - 18.f;
    ren.drawText(counterText,
                 {countRight - countSize.x * 0.90f, layout.header.y + 4.f},
                 m_font,
                 m_theme->textPrimary.withAlpha(contentOpacity),
                 0.90f);

    if (!m_packageTransferState.label().empty()) {
        nxui::Color statusFill = m_theme->panelBase.withAlpha(0.18f);
        nxui::Color statusBorder = m_theme->panelBorder.withAlpha(0.28f);
        if (m_packageTransferState.isRunning()) {
            statusFill = m_theme->cursorNormal.withAlpha(0.16f);
            statusBorder = m_theme->cursorNormal.withAlpha(0.42f);
        } else if (m_packageTransferState.isReady()) {
            statusFill = nxui::Color(0.18f, 0.50f, 0.28f, 0.18f);
            statusBorder = nxui::Color(0.34f, 0.92f, 0.52f, 0.42f);
        } else if (m_packageTransferState.hasFailed()) {
            statusFill = nxui::Color(0.52f, 0.18f, 0.16f, 0.20f);
            statusBorder = nxui::Color(0.98f, 0.34f, 0.30f, 0.42f);
        }

        nxui::Rect statusChip = {
            layout.header.x,
            layout.header.bottom() - 6.f,
            std::min(layout.grid.width * 0.64f, 430.f),
            28.f
        };
        drawChip(ren,
                 m_smallFont,
                 statusChip,
                 transferLabel(m_packageTransferState),
                 statusFill,
                 statusBorder,
                 m_theme->textPrimary,
                 contentOpacity,
                 0.70f);
        if (m_packageTransferState.isRunning()) {
            drawSpinner(ren,
                        {statusChip.x + 16.f, statusChip.y + statusChip.height * 0.5f},
                        7.f,
                        m_uiTime,
                        m_theme->cursorNormal,
                        contentOpacity);
            nxui::Rect track = {
                statusChip.x + 10.f,
                statusChip.bottom() + 4.f,
                statusChip.width - 20.f,
                6.f
            };
            drawTransferProgressTrack(ren,
                                      track,
                                      m_packageTransferState.progress01(),
                                      contentOpacity,
                                      m_theme->cursorNormal);
        }
    }

    if (hasRefresh) {
        std::string refreshLabel = m_communityTransferState.isRunning()
            ? i18n.tr("themeshop.community.refreshing", "Refreshing")
            : i18n.tr("themeshop.community.refresh", "Refresh");
        bool refreshSelected = !m_detailOpen && m_focusArea == FocusArea::Content && m_contentFocusArea == ContentFocusArea::Header
            && m_headerButtonIndex == refreshHeaderIndex;
        drawActionButtonChip(ren,
                             m_smallFont,
                             layout.refreshButton,
                             refreshLabel,
                             m_theme,
                             m_theme->textPrimary,
                             contentOpacity,
                             refreshSelected ? 1.f : 0.f,
                             refreshSelected ? 1.f : 0.68f,
                             0.78f);
        if (m_communityTransferState.isRunning()) {
            drawSpinner(ren,
                        {layout.refreshButton.x + 18.f, layout.refreshButton.y + layout.refreshButton.height * 0.5f},
                        8.f,
                        m_uiTime,
                        m_theme->cursorNormal,
                        contentOpacity);
        }
    }

    const int pages = pageCount();
    const int page = currentPage();
    const bool canPagePrev = page > 0;
    const bool canPageNext = page + 1 < pages;
    const bool pagerFocused = !m_detailOpen
        && m_focusArea == FocusArea::Content
        && m_contentFocusArea == ContentFocusArea::Pager;
    drawActionButtonChip(ren,
                         m_smallFont,
                         layout.pagePrevButton,
                         i18n.tr("themeshop.page.prev", "Previous"),
                         m_theme,
                         canPagePrev ? m_theme->textPrimary : m_theme->textSecondary,
                         contentOpacity,
                         pagerFocused && m_pageButtonIndex == 0 ? 1.f : 0.f,
                         canPagePrev ? (pagerFocused && m_pageButtonIndex == 0 ? 1.f : 0.18f) : 0.f,
                         0.74f);
    drawChip(ren,
             m_smallFont,
             layout.pageCounter,
             std::to_string(page + 1) + " / " + std::to_string(pages),
             m_theme->panelBase.withAlpha(0.16f),
             m_theme->panelBorder.withAlpha(0.24f),
             m_theme->textPrimary,
             contentOpacity,
             0.74f);
    drawActionButtonChip(ren,
                         m_smallFont,
                         layout.pageNextButton,
                         i18n.tr("themeshop.page.next", "Next"),
                         m_theme,
                         canPageNext ? m_theme->textPrimary : m_theme->textSecondary,
                         contentOpacity,
                         pagerFocused && m_pageButtonIndex == 1 ? 1.f : 0.f,
                         canPageNext ? (pagerFocused && m_pageButtonIndex == 1 ? 1.f : 0.18f) : 0.f,
                         0.74f);

    if (pagerFocused) {
        nxui::Rect target = m_pageButtonIndex == 0 ? layout.pagePrevButton : layout.pageNextButton;
        m_focusCursor.moveTo(target.expanded(2.f), 20.f, 0.08f);
    }

    if (isCommunityTab() && !m_communityTransferState.label().empty() && m_packageTransferState.label().empty()) {
        ren.drawText(m_communityTransferState.label(),
                     {layout.header.x, layout.header.bottom() - 6.f},
                     m_smallFont,
                     m_theme->textSecondary.withAlpha(0.78f * contentOpacity),
                     0.72f);
    }

    if (isCommunityTab() && m_communityTransferState.isRunning() && count == 0) {
        // High + roomy gap — breathe peaks ~1.2× and must clear the label.
        nxui::Vec2 center = {layout.grid.x + layout.grid.width * 0.5f,
                             layout.grid.y + layout.grid.height * 0.30f};
        drawCatalogLoadingCube(ren, center, 112.f, m_uiTime, m_theme->cursorNormal, contentOpacity);
        std::string loadingLabel = i18n.tr("themeshop.community.loading", "Loading theme catalog...");
        nxui::Vec2 size = measureTextCached(m_font, loadingLabel);
        ren.drawText(loadingLabel,
                     {center.x - size.x * 0.40f, center.y + 108.f},
                     m_font,
                     m_theme->textPrimary.withAlpha(contentOpacity),
                     0.88f);
        return;
    }

    if (count == 0) {
        nxui::Rect emptyBox = {
            layout.grid.x + layout.grid.width * 0.14f,
            layout.grid.y + layout.grid.height * 0.22f,
            layout.grid.width * 0.72f,
            170.f
        };
        ren.drawRoundedRect(emptyBox, m_theme->panelBase.withAlpha(0.16f * contentOpacity), 20.f);
        ren.drawRoundedRectOutline(emptyBox, m_theme->panelBorder.withAlpha(0.24f * contentOpacity), 20.f, 1.2f);
        std::string emptyTitle, emptySubtitle;
        if (isMusicTab()) {
            if (m_youTubeClient.isSearching()) {
                emptyTitle = i18n.tr("themeshop.music.searching_title", "Searching YouTube...");
                emptySubtitle = i18n.tr("themeshop.music.searching_hint", "Connecting to YouTube and fetching soundtracks...");
            } else if (!m_youtubeSearchQuery.empty()) {
                emptyTitle = i18n.tr("themeshop.music.no_results", "No songs found for this search.");
                emptySubtitle = i18n.tr("themeshop.music.no_results_hint", "Press Search (or X) to try different keywords.");
            } else {
                emptyTitle = i18n.tr("themeshop.music.empty_installed_title", "No Local Songs Found");
                emptySubtitle = i18n.tr("themeshop.music.empty_installed_hint", "Press Search (or X) to find and download songs from YouTube.");
            }
        } else {
            bool searchActive = !m_searchQuery.empty();
            emptyTitle = searchActive
                ? i18n.tr("themeshop.search.no_results", "No themes match this search.")
                : (isCommunityTab()
                    ? i18n.tr("themeshop.community.catalog_empty", "No published themes are listed in the catalog.")
                    : (isIconsShopMode()
                        ? i18n.tr("themeshop.icons.empty_shop", "No icon packs available to download.")
                        : (isIconsTab()
                            ? i18n.tr("themeshop.icons.empty", "No icon packs installed.")
                            : i18n.tr("themeshop.installed.empty", "No installed themes found."))));
            emptySubtitle = searchActive
                ? i18n.tr("themeshop.search.no_results_hint", "Press X or use Search to change or clear the filter.")
                : (isCommunityTab() && m_communityTransferState.hasFailed()
                    ? i18n.tr("themeshop.community.catalog_failed", "The catalog could not be fetched. Use Refresh to try again.")
                    : (isAnimatedTab()
                        ? i18n.tr("themeshop.community.empty_hint", "Add themes to the repository and they will appear here.")
                        : (isIconsShopMode()
                            ? i18n.tr("themeshop.icons.empty_shop_hint", "Refresh the catalog, or switch to Installed for packs on this console.")
                            : (isIconsTab()
                                ? i18n.tr("themeshop.icons.empty_hint", "Switch to Shop to download packs, or install a theme that includes icons.")
                                : (isCommunityTab()
                                    ? i18n.tr("themeshop.community.empty_hint", "Add themes to the repository and they will appear here.")
                                    : i18n.tr("themeshop.themes.empty_installed_hint", "Switch to Shop to browse and download themes."))))));
        }

        nxui::Vec2 titleSize = measureTextCached(m_font, emptyTitle);
        ren.drawText(emptyTitle,
                     {emptyBox.x + (emptyBox.width - titleSize.x * 0.90f) * 0.5f, emptyBox.y + 40.f},
                     m_font,
                     m_theme->textPrimary.withAlpha(contentOpacity),
                     0.90f);
        nxui::Vec2 subSize = measureTextCached(m_smallFont, emptySubtitle);
        ren.drawText(emptySubtitle,
                     {emptyBox.x + (emptyBox.width - subSize.x * 0.76f) * 0.5f, emptyBox.y + 94.f},
                     m_smallFont,
                     m_theme->textSecondary.withAlpha(0.90f * contentOpacity),
                     0.76f);
        if (isMusicTab() && m_youTubeClient.isSearching()) {
            drawSpinner(ren,
                        {emptyBox.x + emptyBox.width * 0.5f, emptyBox.y + 135.f},
                        10.f,
                        m_uiTime,
                        m_theme->cursorNormal,
                        contentOpacity);
        }
        if (!m_detailOpen && m_focusArea == FocusArea::Content && m_contentFocusArea == ContentFocusArea::Header) {
            auto headerButtons = headerButtonRects(layout, showsCatalogRefresh());
            if (!headerButtons.empty()) {
                int headerIndex = std::clamp(m_headerButtonIndex, 0, (int)headerButtons.size() - 1);
                m_focusCursor.moveTo(headerButtons[(size_t)headerIndex].expanded(2.f), 20.f, 0.08f);
            }
        }
        return;
    }

    int scrollRow = currentScrollRowRef();
    int start = scrollRow * kGridCols;
    int end = std::min(count, start + kGridCols * kVisibleRows);
    int selected = std::max(0, currentSelectedIndex());
    float reveal = std::clamp(m_tabReveal.value(), 0.f, 1.f);
    const float gridDetailT = std::clamp(m_detailSheetAnim.value(), 0.f, 1.f);
    const bool gridFocusLive = !m_detailOpen && gridDetailT <= 0.01f;
    const float pageSlideT = std::clamp(m_pageSlideAnim.value(), 0.f, 1.f);
    const float pageSlideX = m_pageSlideFrom * pageSlideT * (layout.grid.width * 0.18f);

    for (int globalIndex = start; globalIndex < end; ++globalIndex) {
        int localIndex = globalIndex - start;
        nxui::Rect card = gridCardRect(layout, localIndex);
        float delay = std::min(0.42f, localIndex * 0.03f);
        float localReveal = std::clamp((reveal - delay) / 0.30f, 0.f, 1.f);
        float rowOpacity = contentOpacity * localReveal * (1.f - 0.18f * pageSlideT);
        float rise = (1.f - localReveal) * 14.f;
        card.y += rise;
        card.x += pageSlideX;

        bool cardSelected = (globalIndex == selected);
        std::string titleText;
        std::string subtitleText;
        // Drawn at the right end of the author's line rather than appended to
        // it: the author is what gets ellipsized when the card is narrow, and
        // the size must not be the half that disappears.
        std::string sizeText;
        std::string versionText;
        const nxui::Texture* previewTexture = nullptr;
        PreviewPhase previewPhase = PreviewPhase::Failed;
        bool previewRequested = false;
        bool builtInDefaultPreview = false;
        bool builtInLightPreview = false;
        bool previewAdaptive = false;
        bool activeTheme = false;
        bool installedHere = false;
        int sheetCols = 0, sheetRows = 0;
        float sheetFps = 10.f;

        bool showIconsBadge = false;
        bool showMusicBadge = false;
        bool showSoundsBadge = false;
        if (isMusicTab()) {
            YouTubeClient::TrackItem track;
            if (m_youTubeClient.getTrack(static_cast<size_t>(globalIndex), track)) {
                titleText = track.title;
                subtitleText = track.author.empty() ? "YouTube" : track.author;
                versionText = track.duration;
                if (track.isDownloading) {
                    int pct = static_cast<int>(track.downloadProgress * 100.f);
                    sizeText = std::to_string(pct) + "%";
                } else if (track.isDownloaded) {
                    sizeText = "✓ " + i18n.tr("themeshop.music.downloaded", "Downloaded");
                    installedHere = true;
                } else {
                    sizeText = estimateDownloadSize(track.duration);
                }
                if (!track.thumbnailUrl.empty()) {
                    previewTexture = m_youTubeClient.thumbnailTexture(track.id);
                    auto phase = m_youTubeClient.thumbnailPhase(track.id);
                    previewPhase = (phase == YouTubeClient::PreviewPhase::Ready) ? PreviewPhase::Ready :
                                   (phase == YouTubeClient::PreviewPhase::Downloaded || phase == YouTubeClient::PreviewPhase::Loading) ? PreviewPhase::Loading :
                                   PreviewPhase::Idle;
                    if (previewPhase == PreviewPhase::Idle) {
                        m_youTubeClient.primeThumbnail(track.id, track.thumbnailUrl);
                        previewPhase = PreviewPhase::Loading;
                    }
                    previewRequested = true;
                } else {
                    previewRequested = false;
                    previewPhase = PreviewPhase::Idle;
                }
            }
        } else if (isCommunityTab()) {
            const auto& entry = m_communityEntries[(size_t)globalIndex];
            titleText = entry.name;
            subtitleText = entry.author.empty() ? i18n.tr("themeshop.community.author_unknown", "Unknown") : entry.author;
            sizeText = formatBytes(entry.packageBytes);
            versionText = entry.version;
            previewTexture = communityPreviewTexture(entry);
            previewPhase = communityPreviewPhase(entry);
            previewRequested = !entry.cover.empty() || !entry.thumbSheet.empty();
            sheetCols = entry.thumbCols;
            sheetRows = entry.thumbRows;
            sheetFps  = entry.thumbFps;
            // Marca o que já está no console, para não ser preciso abrir cada
            // tema só para descobrir que ele já foi baixado.
            installedHere = installedEntryForCatalogue(entry.id) != nullptr;
            showIconsBadge = entry.hasIcons;
            showMusicBadge = entry.hasMusic;
            showSoundsBadge = entry.hasSounds;
        } else {
            const auto& entry = isIconsTab()
                ? m_iconPackEntries[(size_t)globalIndex]
                : m_themeShopEntries[(size_t)globalIndex];
            titleText = entry.name;
            // Shelf caption: author only (source/version live as chips or detail).
            subtitleText = entry.author.empty()
                ? i18n.tr("themeshop.community.author_unknown", "Unknown")
                : entry.author;
            versionText = entry.version;
            activeTheme = entry.active;
            if (!isIconsTab()) {
                // What the theme takes on the card, measured off the render thread.
                sizeText = formatBytes(installedThemeBytes(entry.installPath));
                builtInDefaultPreview = entry.id == "builtin:Default Dark" || entry.id == "builtin:Default Light";
                builtInLightPreview = entry.id == "builtin:Default Light";
            }
            previewRequested = !entry.coverPath.empty() || !entry.coverUrl.empty();
            showIconsBadge = !isIconsTab() && entry.hasIcons;
            showMusicBadge = !isIconsTab() && entry.hasMusic;
            showSoundsBadge = !isIconsTab() && entry.hasSounds;
            if (previewRequested && !entry.coverUrl.empty()) {
                // Catalog pack that is not on the console yet: fetch the cover.
                primeCommunityPreview(entry.coverUrl);
                previewTexture = communityPreviewTexture(entry.coverUrl);
                previewPhase = communityPreviewPhase(entry.coverUrl);
            } else if (previewRequested) {
                const std::string packId = isIconsTab() ? entry.id : std::string{};
                primeInstalledPreview(entry.coverPath, isIconsTab(), packId, nullptr, 384); // grid
                previewTexture = installedPreviewTexture(entry.coverPath, isIconsTab(), packId);
                previewPhase = installedPreviewPhase(entry.coverPath, isIconsTab(), packId);
                previewAdaptive = isIconsTab()
                    && installedPreviewAdaptive(entry.coverPath, true, packId);
            }
        }

        // Shelf tile: soft plate + cover-forward art (caption lives under the cover).
        nxui::Color cardFill = m_theme->panelBase.withAlpha((cardSelected ? 0.16f : 0.10f) * rowOpacity);
        nxui::Color cardBorder = m_theme->panelBorder.withAlpha((cardSelected ? 0.28f : 0.14f) * rowOpacity);
        ren.drawRoundedRect(card, cardFill, 20.f);
        ren.drawRoundedRectOutline(card, cardBorder, 20.f, 1.0f);

        nxui::Rect previewBounds = {
            card.x + 8.f,
            card.y + 8.f,
            card.width - 16.f,
            card.height - kCardCaptionH - 10.f
        };
        nxui::Rect preview = isIconsTab()
            ? previewBounds
            : fitAspectRect(previewBounds, kPreviewAspect);
        // Idle means "queued but not started yet" (download cap) - still loading, not unavailable.
        bool previewLoading = previewPhase == PreviewPhase::Idle
            || previewPhase == PreviewPhase::Loading
            || previewPhase == PreviewPhase::Downloaded;
        std::string previewLabel = previewRequested
            ? (previewLoading
                ? (isIconsTab()
                    ? i18n.tr("themeshop.icons.preview_loading", "Loading preview...")
                    : i18n.tr("themeshop.preview.loading", "Loading screenshot..."))
                : (isIconsTab()
                    ? i18n.tr("themeshop.icons.preview_unavailable", "Preview unavailable")
                    : i18n.tr("themeshop.preview.unavailable", "Screenshot unavailable")))
            : (isIconsTab()
                ? i18n.tr("themeshop.icons.preview_missing", "No preview")
                : i18n.tr("themeshop.preview.missing", "No screenshot"));
        drawThemePreview(ren,
                         m_smallFont,
                         m_theme,
                         preview,
                         previewTexture,
                         previewLabel,
                         previewLoading,
                         m_uiTime,
                         rowOpacity,
                         sheetCols, sheetRows, sheetFps,
                         builtInDefaultPreview, builtInLightPreview,
                         previewAdaptive,
                         isIconsTab(),
                         nullptr,
                         nullptr,
                         !isIconsTab());

        // On-card install progress for the transferring Community / Icons pack.
        const bool cardTransferring = !m_packageTransferThemeId.empty()
            && !m_packageTransferState.label().empty()
            && ((isCommunityTab()
                    && m_communityEntries[(size_t)globalIndex].id == m_packageTransferThemeId)
                || (isIconsTab()
                    && m_iconPackEntries[(size_t)globalIndex].id == m_packageTransferThemeId));
        if (cardTransferring) {
            ren.drawRoundedRect({preview.x, preview.bottom() - 44.f, preview.width, 44.f},
                                nxui::Color(0.f, 0.f, 0.f, 0.55f * rowOpacity),
                                12.f);
            nxui::Rect track = {
                preview.x + 12.f,
                preview.bottom() - 18.f,
                preview.width - 24.f,
                8.f
            };
            drawTransferProgressTrack(ren,
                                      track,
                                      m_packageTransferState.progress01(),
                                      rowOpacity,
                                      m_theme->cursorNormal);
            if (m_packageTransferState.isRunning()) {
                drawSpinner(ren,
                            {preview.x + 18.f, preview.bottom() - 32.f},
                            6.f,
                            m_uiTime,
                            m_theme->cursorNormal,
                            rowOpacity);
            }
            const std::string status = ellipsize(m_smallFont,
                                                 transferLabel(m_packageTransferState),
                                                 preview.width - 48.f,
                                                 0.62f);
            ren.drawText(status,
                         {preview.x + 34.f, preview.bottom() - 40.f},
                         m_smallFont,
                         m_theme->textPrimary.withAlpha(rowOpacity),
                         0.62f);
        }

        if (!versionText.empty()) {
            float chipWidth = std::max(74.f, std::min(108.f, 32.f + measureTextCached(m_smallFont, versionText).x * 0.64f));
            nxui::Rect versionChip = {preview.right() - chipWidth - 10.f, preview.y + 10.f, chipWidth, 26.f};
            drawChip(ren,
                     m_smallFont,
                     versionChip,
                     versionText,
                     nxui::Color(0.f, 0.f, 0.f, 0.26f),
                     nxui::Color(1.f, 1.f, 1.f, 0.16f),
                     m_theme->textPrimary,
                     rowOpacity,
                     0.64f);
        }

        if (installedHere) {
            const std::string label = i18n.tr("themeshop.community.installed", "Installed");
            float chipWidth = std::max(92.f, std::min(126.f, 32.f + measureTextCached(m_smallFont, label).x * 0.66f));
            nxui::Rect chip = {preview.x + 10.f, preview.y + 10.f, chipWidth, 26.f};
            drawChip(ren,
                     m_smallFont,
                     chip,
                     label,
                     nxui::Color(0.10f, 0.22f, 0.38f, 0.36f),
                     nxui::Color(0.36f, 0.68f, 0.98f, 0.52f),
                     m_theme->textPrimary,
                     rowOpacity,
                     0.66f);
        }

        if (activeTheme) {
            const std::string activeLabel = i18n.tr("themeshop.installed.status_active", "Active");
            float chipWidth = std::max(82.f, std::min(114.f, 32.f + measureTextCached(m_smallFont, activeLabel).x * 0.66f));
            nxui::Rect activeChip = {preview.x + 10.f, preview.y + 10.f, chipWidth, 26.f};
            drawChip(ren,
                     m_smallFont,
                     activeChip,
                     activeLabel,
                     nxui::Color(0.10f, 0.34f, 0.14f, 0.34f),
                     nxui::Color(0.34f, 0.92f, 0.52f, 0.50f),
                     m_theme->textPrimary,
                     rowOpacity,
                     0.66f);
        }

        const auto shelfTint = (isIconsTab() && !isCommunityTab())
            ? iconPackTintFor(m_iconPackEntries[(size_t)globalIndex].id)
            : IconPackTintState{};
        // Tint/Auto chips (same language as the detail header) — no spectrum strip.
        const bool showTintBadge = isIconsTab() && !isCommunityTab() && shelfTint.enabled;
        const bool showAutoBadge = isIconsTab() && !isCommunityTab() && shelfTint.matchTheme;
        constexpr float kBadgeGap = 8.f;
        const std::string tintBadgeLabel = i18n.tr("themeshop.badge.tint", "Tint");
        const float tintBadgeW = showTintBadge
            ? std::max(48.f, std::min(72.f, 24.f + measureTextCached(m_smallFont, tintBadgeLabel).x * 0.62f))
            : 0.f;
        const std::string autoBadgeLabel = i18n.tr("themeshop.badge.auto", "Auto");
        const float autoBadgeW = showAutoBadge
            ? std::max(48.f, std::min(72.f, 24.f + measureTextCached(m_smallFont, autoBadgeLabel).x * 0.62f))
            : 0.f;
        constexpr float kAutoBadgeH = 22.f;
        // Icon-only chips so Icons + Music + Sounds fit on one caption row.
        constexpr float kGlyphBadge = 22.f;
        constexpr float kGlyphBadgeGap = 6.f;
        float titleReserve = 0.f;
        if (showTintBadge)
            titleReserve += tintBadgeW + kBadgeGap;
        if (showAutoBadge)
            titleReserve += autoBadgeW + kBadgeGap;
        if (showIconsBadge)
            titleReserve += kGlyphBadge + kGlyphBadgeGap;
        if (showMusicBadge)
            titleReserve += kGlyphBadge + kGlyphBadgeGap;
        if (showSoundsBadge)
            titleReserve += kGlyphBadge + kGlyphBadgeGap;
        const float titleMaxW = titleReserve > 0.f
            ? std::max(40.f, card.width - 18.f - titleReserve)
            : (card.width - 18.f);
        std::string titleFitted = ellipsize(m_font, titleText, titleMaxW, 0.82f);
        const float titleY = card.bottom() - kCardCaptionH + 6.f;
        ren.drawText(titleFitted,
                     {card.x + 12.f, titleY},
                     m_font,
                     m_theme->textPrimary.withAlpha(rowOpacity),
                     0.82f);
        float badgeX = card.x + 12.f + measureTextCached(m_font, titleFitted).x * 0.82f + kBadgeGap;
        const float titleH = measureTextCached(m_font, titleFitted).y * 0.82f;
        auto drawCaptionBadge = [&](float width, const std::string& label,
                                    const ShelfBadgeColors& colors) {
            nxui::Rect chip = {
                badgeX,
                titleY + std::max(0.f, (titleH - kAutoBadgeH) * 0.5f),
                width,
                kAutoBadgeH
            };
            drawChip(ren, m_smallFont, chip, label, colors.fill, colors.border,
                     colors.ink, rowOpacity, 0.62f);
            badgeX = chip.right() + kBadgeGap;
        };
        auto drawGlyphBadge = [&](ShelfBadgeKind kind, const ShelfBadgeColors& colors) {
            nxui::Rect chip = {
                badgeX,
                titleY + std::max(0.f, (titleH - kGlyphBadge) * 0.5f),
                kGlyphBadge,
                kGlyphBadge
            };
            drawIconBadgeChip(ren, chip, kind, colors.fill, colors.border,
                              colors.ink, rowOpacity);
            badgeX = chip.right() + kGlyphBadgeGap;
        };
        const bool darkBadges = m_theme->mode == nxui::ThemeMode::Dark;
        if (showTintBadge)
            drawCaptionBadge(tintBadgeW, tintBadgeLabel,
                             shelfBadgeColors(ShelfBadgeStyle::Tint, darkBadges));
        if (showAutoBadge)
            drawCaptionBadge(autoBadgeW, autoBadgeLabel,
                             shelfBadgeColors(ShelfBadgeStyle::Auto, darkBadges));
        if (showIconsBadge)
            drawGlyphBadge(ShelfBadgeKind::Icons,
                           shelfBadgeColors(ShelfBadgeStyle::Icons, darkBadges));
        if (showMusicBadge)
            drawGlyphBadge(ShelfBadgeKind::Music,
                           shelfBadgeColors(ShelfBadgeStyle::Music, darkBadges));
        if (showSoundsBadge)
            drawGlyphBadge(ShelfBadgeKind::Sounds,
                           shelfBadgeColors(ShelfBadgeStyle::Sounds, darkBadges));
        // Author only — eShop shelves don't dump source/version in the caption.
        // The size sits at the right end of that line rather than appended to
        // it: the author is what gets ellipsized, and the size must not be the
        // half that disappears.
        float subtitleWidth = card.width - 24.f;
        if (!sizeText.empty()) {
            const float sizeWidth = measureTextCached(m_smallFont, sizeText).x * 0.70f;
            ren.drawText(sizeText,
                         {card.right() - 12.f - sizeWidth, card.bottom() - 22.f},
                         m_smallFont,
                         m_theme->textSecondary.withAlpha(0.78f * rowOpacity),
                         0.70f);
            subtitleWidth -= sizeWidth + 10.f;
        }
        std::string subtitleFitted = ellipsize(m_smallFont, subtitleText, std::max(20.f, subtitleWidth), 0.70f);
        ren.drawText(subtitleFitted,
                     {card.x + 12.f, card.bottom() - 22.f},
                     m_smallFont,
                     m_theme->textSecondary.withAlpha(0.78f * rowOpacity),
                     0.70f);

        // Hold grid focus until the detail close anim finishes, then snap once.
        // Mid-close updates let the fading sheet steal the ring back to Apply.
        if (gridFocusLive
            && m_focusArea == FocusArea::Content && m_contentFocusArea == ContentFocusArea::Grid
            && cardSelected) {
            const float dur = m_snapGridFocus ? 0.f : 0.08f;
            m_focusCursor.moveTo(card.expanded(2.f), 22.f, dur);
            if (m_snapGridFocus)
                m_snapGridFocus = false;
        }
    }

    if (gridFocusLive
        && m_focusArea == FocusArea::Content && m_contentFocusArea == ContentFocusArea::Header) {
        auto headerButtons = headerButtonRects(layout, showsCatalogRefresh());
        if (!headerButtons.empty()) {
            int headerIndex = std::clamp(m_headerButtonIndex, 0, (int)headerButtons.size() - 1);
            m_focusCursor.moveTo(headerButtons[(size_t)headerIndex].expanded(2.f), 20.f, 0.08f);
        }
    }

    int firstVisible = start + 1;
    int lastVisible = end;
    std::string footer = std::to_string(firstVisible) + "-" + std::to_string(lastVisible) + " / " + std::to_string(count);
    ren.drawText(footer,
                 {layout.grid.x, layout.grid.bottom() + 15.f},
                 m_smallFont,
                 m_theme->textSecondary.withAlpha(0.84f * contentOpacity),
                 0.76f);

    float detailT = std::clamp(m_detailSheetAnim.value(), 0.f, 1.f);
    if (!m_detailOpen && detailT <= 0.01f)
        return;

    float detailOpacity = contentOpacity * detailT;
    // Soft store scrim — dim the shelf, lift the product sheet.
    ren.drawRoundedRect(content, nxui::Color(0.f, 0.f, 0.f, 0.52f * detailOpacity), 26.f);
    nxui::Rect dialog = detailDialogRect(content);
    // Scale + slight rise so the sheet feels like it settles onto the shelf.
    dialog = scaledRect(dialog, 0.97f + 0.03f * detailT);
    dialog.y += (1.f - detailT) * 26.f;
    // The fork keeps its liquid-glass product sheet (PoloNX moved to a flat
    // frosted panel); the scrim and settle motion are PoloNX's.
    drawLiquidGlassPanel(ren, m_theme, dialog, 26.f, detailOpacity, 1.4f);

    // Tint only applies to packs that live on the console.
    const auto* tintEntry = isIconsTab() ? selectedThemeShopEntry() : nullptr;
    const bool showIconTintControls = isIconsTab() && showSelectedIconTintControls()
        && tintEntry && tintEntry->installed;
    nxui::Rect preview = showIconTintControls
        ? colourIconPreviewBounds(dialog)
        : detailPreviewRect(dialog);
    int detailSheetCols = 0, detailSheetRows = 0;
    float detailSheetFps = 4.f;
    std::string detailTitle;
    std::string detailSubtitle;
    std::string detailInfoA;
    std::string detailInfoB;
    // O tamanho no cartao ganhou linha propria. Junto com o download numa linha
    // so, a frase passava da largura da coluna e as reticencias comiam
    // justamente a metade que interessa a quem esta decidindo se cabe.
    std::string detailInfoBExtra;
    std::string detailInfoC;
    // Row 1: status / version. Row 2: Icons / Music / Sounds (shelf colors).
    std::vector<std::string> detailStatusBadges;
    bool detailShowIconsBadge = false;
    bool detailShowMusicBadge = false;
    bool detailShowSoundsBadge = false;
    const nxui::Texture* detailPreviewTexture = nullptr;
    PreviewPhase detailPreviewPhase = PreviewPhase::Failed;
    bool detailPreviewRequested = false;
    bool detailBuiltInDefaultPreview = false;
    bool detailBuiltInLightPreview = false;
    int detailScreenshotTotal = 0;
    DetailPreviewControls previewControls = detailPreviewControls(preview);

    if (isMusicTab()) {
        const auto* track = selectedMusicTrack();
        if (!track)
            return;
        detailTitle = track->title;
        detailSubtitle = track->author.empty() ? "YouTube" : track->author;
        detailInfoA = track->isDownloaded ? (i18n.tr("themeshop.music.size", "Size: ") + track->duration)
                                          : (i18n.tr("themeshop.music.duration", "Duration: ") + track->duration + "  •  " + i18n.tr("themeshop.music.est_size", "Est. Size: ") + estimateDownloadSize(track->duration));
        detailInfoB = track->isDownloaded ? i18n.tr("themeshop.music.status_downloaded", "Status: Saved in sdmc:/config/SwitchU/music/")
                                          : (i18n.tr("themeshop.music.video_id", "Video ID: ") + track->id);
        detailInfoC = track->isDownloaded
            ? i18n.tr("themeshop.music.status_ready_play", "Status: Ready to play or delete")
            : i18n.tr("themeshop.music.status_ready", "Status: Ready to download (.mp3)");
        if (!track->thumbnailUrl.empty()) {
            detailPreviewTexture = m_youTubeClient.thumbnailTexture(track->id);
            auto phase = m_youTubeClient.thumbnailPhase(track->id);
            detailPreviewPhase = (phase == YouTubeClient::PreviewPhase::Ready) ? PreviewPhase::Ready :
                                 (phase == YouTubeClient::PreviewPhase::Downloaded || phase == YouTubeClient::PreviewPhase::Loading) ? PreviewPhase::Loading :
                                 PreviewPhase::Idle;
            if (detailPreviewPhase == PreviewPhase::Idle) {
                m_youTubeClient.primeThumbnail(track->id, track->thumbnailUrl);
                detailPreviewPhase = PreviewPhase::Loading;
            }
            detailPreviewRequested = true;
        } else {
            detailPreviewRequested = false;
            detailPreviewPhase = PreviewPhase::Idle;
        }
    } else if (isCommunityTab()) {
        const auto* entry = selectedCommunityThemeEntry();
        if (!entry)
            return;
        detailTitle = entry->name;
        detailSubtitle = entry->author.empty() ? i18n.tr("themeshop.community.author_unknown", "Unknown") : entry->author;
        if (!entry->version.empty())
            detailStatusBadges.push_back(entry->version);
        detailShowIconsBadge = entry->hasIcons;
        detailShowMusicBadge = entry->hasMusic;
        detailShowSoundsBadge = entry->hasSounds;
        detailInfoA = i18n.tr("themeshop.community.version", "Version") + std::string(": ")
            + (entry->version.empty() ? i18n.tr("themeshop.community.version_unknown", "Unknown") : entry->version);
        // Where the two sizes belong: the detail screen is where a theme is
        // accepted or refused, and it has room for both. The manifest path this
        // line used to carry is a catalogue implementation detail -- it stays
        // as the fallback for an index that declares no sizes.
        const std::string downloadSize = formatBytes(entry->packageBytes);
        const std::string cardSize = formatBytes(entry->installedBytes);
        if (!downloadSize.empty() || !cardSize.empty()) {
            detailInfoB = i18n.tr("themeshop.community.size", "Size") + std::string(": ");
            if (!downloadSize.empty())
                detailInfoB += downloadSize + " " + i18n.tr("themeshop.catalog.to_download", "to download");
            if (!cardSize.empty())
                detailInfoBExtra = cardSize + " " + i18n.tr("themeshop.catalog.on_card", "on the SD card");
        } else {
            detailInfoB = i18n.tr("themeshop.community.manifest", "Manifest") + std::string(": ")
                + (entry->manifest.empty() ? i18n.tr("themeshop.community.manifest_missing", "Not provided") : entry->manifest);
        }
        detailScreenshotTotal = std::max(0, (int)entry->screenshots.size());
        std::string detailPreviewPath = currentDetailCommunityPreviewPath();
        // A folha tambem no detalhe e na tela cheia. Ela e a unica coisa que se
        // move: as capturas do tema sao imagens paradas, e um tema animado
        // apresentado por uma foto parada nao mostra o que ele e.
        if (const auto* selected = selectedCommunityThemeEntry()) {
            // As duas folhas, nao so a pequena. A grande tem a mesma grade de
            // celulas -- muda o tamanho de cada uma -- e comparar so com
            // thumbSheet deixava a tela cheia sem colunas nem linhas, o que faz
            // o desenho tratar a folha inteira como uma imagem so e mostrar as
            // vinte celulas de uma vez.
            const bool isSheet =
                (!selected->thumbSheet.empty()   && detailPreviewPath == selected->thumbSheet)
             || (!selected->thumbSheetHd.empty() && detailPreviewPath == selected->thumbSheetHd);
            if (isSheet) {
                detailSheetCols = selected->thumbCols;
                detailSheetRows = selected->thumbRows;
                detailSheetFps  = selected->thumbFps;
            }
        }

        // Qual imagem a tela cheia pediu e em que estado ela esta. A previa
        // sumia sem deixar rastro: "captura indisponivel" e o log sem uma
        // palavra sobre qual arquivo foi pedido.
        if (m_detailFullscreen && detailPreviewPath != m_lastLoggedDetailPath) {
            m_lastLoggedDetailPath = detailPreviewPath;
            DebugLog::log("[themeshop] tela cheia pede %s (estado=%d celulas=%dx%d)",
                          detailPreviewPath.c_str(), (int)communityPreviewPhase(detailPreviewPath),
                          detailSheetCols, detailSheetRows);
        }
        detailPreviewTexture = communityPreviewTexture(detailPreviewPath);
        detailPreviewPhase = communityPreviewPhase(detailPreviewPath);
        detailPreviewRequested = !detailPreviewPath.empty();
        if (!detailPreviewRequested) {
            detailInfoC = i18n.tr("themeshop.community.cover_missing", "No screenshot has been declared for this theme yet.");
        } else if (detailPreviewTexture) {
            detailInfoC = detailScreenshotTotal > 1
                ? i18n.tr("themeshop.community.cover_present", "Use Left/Right to browse screenshots and A for fullscreen.")
                : i18n.tr("themeshop.community.cover_single", "Press A to open this screenshot in fullscreen.");
        } else if (detailPreviewPhase == PreviewPhase::Loading || detailPreviewPhase == PreviewPhase::Downloaded) {
            detailInfoC = i18n.tr("themeshop.community.cover_loading", "Loading theme screenshot...");
        } else {
            detailInfoC = i18n.tr("themeshop.community.cover_unavailable", "The screenshot could not be loaded. Placeholder shown instead.");
        }
    } else {
        const auto* entry = selectedThemeShopEntry();
        if (!entry)
            return;
        detailTitle = entry->name;
        std::string author = entry->author.empty()
            ? i18n.tr("themeshop.community.author_unknown", "Unknown")
            : entry->author;
        detailSubtitle = entry->source.empty() ? author : (author + " - " + entry->source);
        const std::string statusText = entry->active
            ? i18n.tr("themeshop.installed.status_active", "Active")
            : (entry->installed
                ? i18n.tr("themeshop.installed.status_available", "Available")
                : i18n.tr("themeshop.icons.status_online", "Available online"));
        detailStatusBadges.push_back(statusText);
        if (!entry->version.empty())
            detailStatusBadges.push_back(entry->version);
        if (!isIconsTab()) {
            detailShowIconsBadge = entry->hasIcons;
            detailShowMusicBadge = entry->hasMusic;
            detailShowSoundsBadge = entry->hasSounds;
        }
        detailInfoA = i18n.tr("themeshop.installed.status", "Status") + std::string(": ")
            + statusText;
        if (isIconsTab()) {
            detailInfoB = entry->description.empty()
                ? i18n.tr("themeshop.icons.fallback", "Missing icons fall back to Default.")
                : entry->description;
            if (!entry->installed) {
                detailInfoC = i18n.tr("themeshop.icons.download_hint",
                    "Shop pack: Download or Download + Apply to install it on this console.");
            } else if (entry->removable) {
                detailInfoC = i18n.tr("themeshop.icons.remove_hint",
                    "Installed pack: Apply to use it, or Remove to delete it from the console.");
            } else if (entry->builtin) {
                detailInfoC = i18n.tr("themeshop.icons.builtin_hint",
                    "Built-in pack: Apply to use it. Built-ins cannot be removed.");
            } else if (!entry->coverPath.empty()) {
                detailInfoC = i18n.tr("themeshop.icons.cover_single", "Press A for a closer look, then Apply to switch icons.");
            } else {
                detailInfoC = i18n.tr("themeshop.icons.apply_hint", "Apply to switch sidebar icons immediately.");
            }
        } else {
            detailInfoB = i18n.tr("themeshop.installed.sound", "Bundled Sound Pack") + std::string(": ")
                + (entry->soundPreset.empty() ? i18n.tr("themeshop.installed.sound_none", "None") : entry->soundPreset);
            detailInfoC = entry->removable
                ? i18n.tr("themeshop.installed.remove_hint", "This theme can be removed from the console.")
                : i18n.tr("themeshop.installed.builtin_hint", "This is part of the built-in theme set.");
            if (!entry->coverPath.empty())
                detailInfoC = i18n.tr("themeshop.installed.cover_single", "Press A to open this screenshot in fullscreen.");
        }
        detailPreviewRequested = !entry->coverPath.empty() || !entry->coverUrl.empty();
        if (!isIconsTab()) {
            detailBuiltInDefaultPreview = entry->id == "builtin:Default Dark" || entry->id == "builtin:Default Light";
            detailBuiltInLightPreview = entry->id == "builtin:Default Light";
        }
        if (detailPreviewRequested && !entry->coverUrl.empty()) {
            primeCommunityPreview(entry->coverUrl);
            detailPreviewTexture = communityPreviewTexture(entry->coverUrl);
            detailPreviewPhase = communityPreviewPhase(entry->coverUrl);
        } else if (detailPreviewRequested) {
            const std::string packId = isIconsTab() ? entry->id : std::string{};
            // Detail always uses the draft HSL slot while tint controls are up —
            // including when Custom/Flatten/Gray are all off, so the preview can
            // return to the pack's original look instead of the last Apply.
            // Icons grid keeps the committed preview (hslOverride=null).
            switchu::icons::HslAdjust draftHsl;
            const switchu::icons::HslAdjust* hslOverride = nullptr;
            if (isIconsTab() && showSelectedIconTintControls()) {
                draftHsl = hslAdjustFromTint(selectedIconPackTint());
                hslOverride = &draftHsl;
            }
            primeInstalledPreview(entry->coverPath, isIconsTab(), packId, hslOverride, 384); // match grid - avoid waitIdle reupload
            detailPreviewTexture =
                installedPreviewTexture(entry->coverPath, isIconsTab(), packId, hslOverride);
            detailPreviewPhase =
                installedPreviewPhase(entry->coverPath, isIconsTab(), packId, hslOverride);
        }
    }

    const bool detailPreviewAdaptive = [&]() {
        if (!isIconsTab() || !detailPreviewRequested || !selectedThemeShopEntry())
            return false;
        const std::string packId = selectedThemeShopEntry()->id;
        switchu::icons::HslAdjust draftHsl;
        const switchu::icons::HslAdjust* hslOverride = nullptr;
        if (showSelectedIconTintControls()) {
            draftHsl = hslAdjustFromTint(selectedIconPackTint());
            hslOverride = &draftHsl;
        }
        return installedPreviewAdaptive(selectedThemeShopEntry()->coverPath, true, packId,
                                        hslOverride);
    }();

    // Never flash the spinner over a still-valid draft frame (Idle / Loading /
    // Downloaded all keep the previous texture during HSL scrub).
    bool detailPreviewLoading = !detailPreviewTexture
        && (detailPreviewPhase == PreviewPhase::Idle
            || detailPreviewPhase == PreviewPhase::Loading
            || detailPreviewPhase == PreviewPhase::Downloaded);
    std::string detailPreviewLabel = detailPreviewRequested
        ? (detailPreviewLoading
            ? i18n.tr("themeshop.preview.loading", "Loading screenshot...")
            : i18n.tr("themeshop.preview.unavailable", "Screenshot unavailable"))
        : i18n.tr("themeshop.preview.missing", "No screenshot");
    // Custom tint is baked into the preview texture via HSL adjust on load.
    drawThemePreview(ren,
                     m_smallFont,
                     m_theme,
                     preview,
                     detailPreviewTexture,
                     detailPreviewLabel,
                     detailPreviewLoading,
                     m_uiTime,
                     detailOpacity,
                     detailSheetCols, detailSheetRows, detailSheetFps,
                     detailBuiltInDefaultPreview, detailBuiltInLightPreview,
                     detailPreviewAdaptive,
                     isIconsTab(),
                     nullptr,
                     nullptr,
                     !isIconsTab());

    // Tint / Auto chips on the product preview tile (same language as the shelf caption).
    if (showIconTintControls) {
        const bool tileTint = selectedIconCustomTint();
        const bool tileAuto = selectedIconPackTint().matchTheme;
        if (tileTint || tileAuto) {
            const bool darkBadges = m_theme->mode == nxui::ThemeMode::Dark;
            const std::string tintLabel = i18n.tr("themeshop.badge.tint", "Tint");
            const std::string autoLabel = i18n.tr("themeshop.badge.auto", "Auto");
            float chipX = preview.x + 10.f;
            const float chipY = preview.y + 10.f;
            if (tileTint) {
                const auto c = shelfBadgeColors(ShelfBadgeStyle::Tint, darkBadges);
                const float tintW = std::max(48.f,
                    std::min(72.f, 24.f + measureTextCached(m_smallFont, tintLabel).x * 0.62f));
                nxui::Rect tintChip = {chipX, chipY, tintW, 22.f};
                drawChip(ren, m_smallFont, tintChip, tintLabel,
                         c.fill, c.border, c.ink, detailOpacity, 0.62f);
                chipX = tintChip.right() + 8.f;
            }
            if (tileAuto) {
                const auto c = shelfBadgeColors(ShelfBadgeStyle::Auto, darkBadges);
                const float autoW = std::max(48.f,
                    std::min(72.f, 24.f + measureTextCached(m_smallFont, autoLabel).x * 0.62f));
                nxui::Rect autoChip = {chipX, chipY, autoW, 22.f};
                drawChip(ren, m_smallFont, autoChip, autoLabel,
                         c.fill, c.border, c.ink, detailOpacity, 0.62f);
            }
        }
    }

    if (detailPreviewRequested) {
        auto previewButtons = detailPreviewControlRects(previewControls, preview, detailScreenshotTotal);
        if (isCommunityTab() && detailScreenshotTotal > 1) {
            bool canGoPrev = m_detailScreenshotIndex > 0;
            bool canGoNext = m_detailScreenshotIndex + 1 < detailScreenshotTotal;
            drawActionButtonChip(ren,
                                 m_smallFont,
                                 previewControls.prev,
                                 i18n.tr("themeshop.preview.prev", "Prev"),
                                 m_theme,
                                 canGoPrev ? m_theme->textPrimary : m_theme->textSecondary,
                                 detailOpacity,
                                 0.f,
                                 canGoPrev ? 0.24f : 0.f,
                                 0.78f);
            drawActionButtonChip(ren,
                                 m_smallFont,
                                 previewControls.next,
                                 i18n.tr("themeshop.preview.next", "Next"),
                                 m_theme,
                                 canGoNext ? m_theme->textPrimary : m_theme->textSecondary,
                                 detailOpacity,
                                 0.f,
                                 canGoNext ? 0.24f : 0.f,
                                 0.78f);
            drawChip(ren,
                     m_smallFont,
                     previewControls.counter,
                     std::to_string(m_detailScreenshotIndex + 1) + " / " + std::to_string(detailScreenshotTotal),
                     nxui::Color(0.f, 0.f, 0.f, 0.22f),
                     m_theme->panelBorder.withAlpha(0.20f),
                     m_theme->textPrimary,
                     detailOpacity,
                     0.68f);
        }

        if (m_detailOpen && m_detailFocusArea == DetailFocusArea::Preview) {
            m_focusCursor.moveTo(preview.expanded(4.f), 20.f, 0.08f);
        }
    }

    if (showIconTintControls) {
        // Header above the preview; Tint/Auto live on the preview tile itself.
        const float headerX = dialog.x + 36.f;
        const float headerW = dialog.width - 72.f;
        ren.drawText(ellipsize(m_font, detailTitle, headerW * 0.62f, 1.10f),
                     {headerX, dialog.y + 22.f},
                     m_font,
                     m_theme->textPrimary.withAlpha(detailOpacity),
                     1.10f);
        ren.drawText(ellipsize(m_smallFont, detailSubtitle, headerW * 0.55f, 0.84f),
                     {headerX, dialog.y + 56.f},
                     m_smallFont,
                     m_theme->textSecondary.withAlpha(0.90f * detailOpacity),
                     0.84f);
        ren.drawText(ellipsize(m_smallFont, detailInfoA, headerW * 0.40f, 0.78f),
                     {headerX + headerW * 0.58f, dialog.y + 28.f},
                     m_smallFont,
                     m_theme->textPrimary.withAlpha(detailOpacity),
                     0.78f);

        IconTintLayout tintLayout = makeIconTintLayout(
            colourIconTintBand(dialog, preview, selectedIconCustomTint()),
            selectedIconCustomTint(),
            selectedIconShowsHueSat());
        auto drawToggleRow = [&](const nxui::Rect& labelRow, const nxui::Rect& pill,
                                 const std::string& label, bool enabled, bool focused) {
            ren.drawText(ellipsize(m_smallFont, label, pill.x - labelRow.x - 12.f, 0.86f),
                         {labelRow.x, labelRow.y + 6.f},
                         m_smallFont,
                         m_theme->textPrimary.withAlpha(detailOpacity),
                         0.86f);
            drawTintTogglePill(ren, m_smallFont, pill, enabled, detailOpacity, focused);
        };
        drawToggleRow(tintLayout.matchLabelRow, tintLayout.matchPill,
                      i18n.tr("themeshop.icons.match_theme", "Match theme"),
                      selectedIconPackTint().matchTheme,
                      m_detailFocusArea == DetailFocusArea::MatchThemeToggle);
        drawToggleRow(tintLayout.tintLabelRow, tintLayout.tintPill,
                      i18n.tr("themeshop.icons.custom_tint", "Custom tint"),
                      selectedIconCustomTint(),
                      m_detailFocusArea == DetailFocusArea::TintToggle);
        drawToggleRow(tintLayout.flattenLabelRow, tintLayout.flattenPill,
                      i18n.tr("themeshop.icons.flatten", "Solid"),
                      selectedIconPackTint().flatten,
                      m_detailFocusArea == DetailFocusArea::FlattenToggle);
        drawToggleRow(tintLayout.grayLabelRow, tintLayout.grayPill,
                      i18n.tr("themeshop.icons.grayscale", "Grayscale"),
                      selectedIconPackTint().grayscale,
                      m_detailFocusArea == DetailFocusArea::GrayscaleToggle);

        if (selectedIconCustomTint()) {
            constexpr float kLabelScale = 0.80f;
            if (selectedIconShowsHueSat()) {
                ren.drawText(i18n.tr("themeshop.icons.tint_hue", "Hue"),
                             {tintLayout.hueLabel.x, tintLayout.hueLabel.y},
                             m_smallFont,
                             m_theme->textPrimary.withAlpha(0.92f * detailOpacity),
                             kLabelScale);
                drawHueSpectrumTrack(ren,
                                     tintLayout.hueTrack,
                                     selectedIconPackTint().hue,
                                     detailOpacity,
                                     m_detailFocusArea == DetailFocusArea::TintHue);
                ren.drawText(i18n.tr("themeshop.icons.tint_saturation", "Saturation"),
                             {tintLayout.satLabel.x, tintLayout.satLabel.y},
                             m_smallFont,
                             m_theme->textPrimary.withAlpha(0.92f * detailOpacity),
                             kLabelScale);
                drawGradientValueTrack(ren, tintLayout.satTrack, selectedIconPackTint().sat, detailOpacity,
                                       m_detailFocusArea == DetailFocusArea::TintSat,
                                       [](float t) {
                                           return nxui::Color::fromHSL(0.58f, t, 0.55f, 1.f);
                                       });
            }
            // Lightness uses dedicated slots (or the hue slots when H/S are hidden).
            const nxui::Rect lightLabel = selectedIconShowsHueSat()
                ? tintLayout.lightLabel : tintLayout.hueLabel;
            const nxui::Rect lightTrack = selectedIconShowsHueSat()
                ? tintLayout.lightTrack : tintLayout.hueTrack;
            ren.drawText(i18n.tr("themeshop.icons.tint_lightness", "Lightness"),
                         {lightLabel.x, lightLabel.y},
                         m_smallFont,
                         m_theme->textPrimary.withAlpha(0.92f * detailOpacity),
                         kLabelScale);
            drawGradientValueTrack(ren, lightTrack, selectedIconPackTint().light, detailOpacity,
                                   m_detailFocusArea == DetailFocusArea::TintLight,
                                   [](float t) {
                                       return nxui::Color::fromHSL(0.58f, 0.35f, t, 1.f);
                                   });
            if (m_detailOpen && m_detailFocusArea == DetailFocusArea::TintLight)
                m_focusCursor.moveTo(lightTrack.expanded(8.f), 18.f, 0.08f);
        }

        if (m_detailOpen && m_detailFocusArea == DetailFocusArea::TintToggle) {
            m_focusCursor.moveTo(tintLayout.tintPill.expanded(6.f), 18.f, 0.08f);
        } else if (m_detailOpen && m_detailFocusArea == DetailFocusArea::MatchThemeToggle) {
            m_focusCursor.moveTo(tintLayout.matchPill.expanded(6.f), 18.f, 0.08f);
        } else if (m_detailOpen && m_detailFocusArea == DetailFocusArea::FlattenToggle) {
            m_focusCursor.moveTo(tintLayout.flattenPill.expanded(6.f), 18.f, 0.08f);
        } else if (m_detailOpen && m_detailFocusArea == DetailFocusArea::GrayscaleToggle) {
            m_focusCursor.moveTo(tintLayout.grayPill.expanded(6.f), 18.f, 0.08f);
        } else if (m_detailOpen && m_detailFocusArea == DetailFocusArea::TintHue
                   && selectedIconCustomTint() && selectedIconShowsHueSat()) {
            m_focusCursor.moveTo(tintLayout.hueTrack.expanded(8.f), 18.f, 0.08f);
        } else if (m_detailOpen && m_detailFocusArea == DetailFocusArea::TintSat
                   && selectedIconCustomTint() && selectedIconShowsHueSat()) {
            m_focusCursor.moveTo(tintLayout.satTrack.expanded(8.f), 18.f, 0.08f);
        } else if ((m_detailFocusArea == DetailFocusArea::TintHue
                    || m_detailFocusArea == DetailFocusArea::TintSat)
                   && !(selectedIconCustomTint() && selectedIconShowsHueSat())) {
            m_detailFocusArea = selectedIconCustomTint()
                ? DetailFocusArea::TintLight : DetailFocusArea::TintToggle;
        } else if (m_detailFocusArea == DetailFocusArea::TintLight
                   && !selectedIconCustomTint()) {
            m_detailFocusArea = DetailFocusArea::TintToggle;
        }
    } else {
        // Full-width title above the screenshot — long theme names fit here.
        const float titleX = dialog.x + 28.f;
        const float titleW = dialog.width - 56.f;
        ren.drawText(ellipsize(m_font, detailTitle, titleW, 1.18f),
                     {titleX, dialog.y + 22.f},
                     m_font,
                     m_theme->textPrimary.withAlpha(detailOpacity),
                     1.18f);

        // Meta column on the left: author, status/version, content badges, hint.
        nxui::Rect infoBounds = {
            dialog.x + 28.f,
            preview.y,
            std::max(80.f, preview.x - dialog.x - 56.f),
            std::max(80.f, preview.height)
        };
        ren.drawText(ellipsize(m_smallFont, detailSubtitle, infoBounds.width, 0.86f),
                     {infoBounds.x, infoBounds.y + 2.f},
                     m_smallFont,
                     m_theme->textSecondary.withAlpha(0.88f * detailOpacity),
                     0.86f);

        float badgeY = infoBounds.y + 42.f;
        float badgeX = infoBounds.x;
        constexpr float kBadgeH = 30.f;
        constexpr float kBadgeGap = 8.f;
        constexpr float kBadgeScale = 0.70f;
        auto placeChip = [&](float chipW) -> nxui::Rect {
            if (badgeX > infoBounds.x
                && badgeX + chipW > infoBounds.right() + 0.5f) {
                badgeX = infoBounds.x;
                badgeY += kBadgeH + kBadgeGap;
            }
            nxui::Rect chip = {badgeX, badgeY, chipW, kBadgeH};
            badgeX = chip.right() + kBadgeGap;
            return chip;
        };
        for (const auto& label : detailStatusBadges) {
            if (badgeY + kBadgeH > infoBounds.bottom() - 56.f)
                break;
            const float chipW = std::max(72.f,
                std::min(infoBounds.width,
                         28.f + measureTextCached(m_smallFont, label).x * kBadgeScale));
            drawChip(ren,
                     m_smallFont,
                     placeChip(chipW),
                     label,
                     m_theme->panelBase.withAlpha(0.22f),
                     m_theme->panelBorder.withAlpha(0.28f),
                     m_theme->textPrimary,
                     detailOpacity,
                     kBadgeScale);
        }
        // Icons / Music / Sounds on the next row — text labels, shelf colors
        // (shelf itself stays glyph-only for caption space).
        if (detailShowIconsBadge || detailShowMusicBadge || detailShowSoundsBadge) {
            badgeX = infoBounds.x;
            badgeY += kBadgeH + kBadgeGap;
            auto drawContentText = [&](const std::string& label,
                                       const ShelfBadgeColors& colors) {
                if (badgeY + kBadgeH > infoBounds.bottom() - 56.f)
                    return;
                const float chipW = std::max(72.f,
                    std::min(infoBounds.width,
                             28.f + measureTextCached(m_smallFont, label).x * kBadgeScale));
                drawChip(ren,
                         m_smallFont,
                         placeChip(chipW),
                         label,
                         colors.fill,
                         colors.border,
                         colors.ink,
                         detailOpacity,
                         kBadgeScale);
            };
            const bool darkBadges = m_theme->mode == nxui::ThemeMode::Dark;
            if (detailShowIconsBadge) {
                drawContentText(i18n.tr("themeshop.badge.icons", "Icons"),
                                shelfBadgeColors(ShelfBadgeStyle::Icons, darkBadges));
            }
            if (detailShowMusicBadge) {
                drawContentText(i18n.tr("themeshop.badge.music", "Music"),
                                shelfBadgeColors(ShelfBadgeStyle::Music, darkBadges));
            }
            if (detailShowSoundsBadge) {
                drawContentText(i18n.tr("themeshop.badge.sounds", "Sounds"),
                                shelfBadgeColors(ShelfBadgeStyle::Sounds, darkBadges));
            }
        }

        // The fork's facts that decide a download: the two sizes (to download /
        // on the card), the bundled sound pack, or the track's duration and
        // status on the Music tab. PoloNX's sheet only carries badges + hint.
        float metaY = badgeY + kBadgeH + 16.f;
        auto drawMetaLine = [&](const std::string& line) {
            if (line.empty() || metaY + 24.f > infoBounds.bottom() - 24.f)
                return;
            ren.drawText(ellipsize(m_smallFont, line, infoBounds.width, 0.76f),
                         {infoBounds.x, metaY},
                         m_smallFont,
                         m_theme->textPrimary.withAlpha(0.90f * detailOpacity),
                         0.76f);
            metaY += 30.f;
        };
        if (isMusicTab())
            drawMetaLine(detailInfoA);
        drawMetaLine(detailInfoB);
        drawMetaLine(detailInfoBExtra);

        const float hintY = std::max(metaY, infoBounds.y + 96.f);        const float hintMaxH = std::max(0.f, infoBounds.bottom() - hintY);
        const int hintLines = std::max(1, (int)(hintMaxH / 22.f));
        drawWrappedText(ren,
                        m_smallFont,
                        detailInfoC,
                        {infoBounds.x, hintY},
                        infoBounds.width,
                        m_theme->textSecondary.withAlpha(0.80f * detailOpacity),
                        0.76f,
                        4.f,
                        hintLines);
    }

    if ((isCommunityTab() || isIconsTab()) && !m_packageTransferState.label().empty()
        && (m_packageTransferThemeId.empty()
            || (isCommunityTab()
                    && selectedCommunityThemeEntry()
                    && selectedCommunityThemeEntry()->id == m_packageTransferThemeId)
            || (isIconsTab()
                    && selectedThemeShopEntry()
                    && selectedThemeShopEntry()->id == m_packageTransferThemeId))) {
        // Thin progress sheet under the product copy — stay on the eShop sheet
        // instead of a modal that nukes focus / covers.
        nxui::Rect sheet = {
            dialog.x + 28.f,
            dialog.bottom() - 132.f,
            std::max(120.f, preview.x - dialog.x - 56.f),
            50.f
        };
        ren.drawRoundedRect(sheet, m_theme->panelBase.withAlpha(0.22f * detailOpacity), 14.f);
        ren.drawRoundedRectOutline(sheet, m_theme->panelBorder.withAlpha(0.28f * detailOpacity), 14.f, 1.f);

        nxui::Color statusColor = m_theme->textSecondary;
        if (m_packageTransferState.isRunning())
            statusColor = m_theme->textPrimary;
        else if (m_packageTransferState.isReady())
            statusColor = nxui::Color(0.62f, 0.96f, 0.72f, 1.f);
        else if (m_packageTransferState.hasFailed())
            statusColor = nxui::Color(1.f, 0.56f, 0.52f, 1.f);

        if (m_packageTransferState.isRunning()) {
            drawSpinner(ren,
                        {sheet.x + 18.f, sheet.y + 18.f},
                        7.f,
                        m_uiTime,
                        m_theme->cursorNormal,
                        detailOpacity);
        }
        ren.drawText(ellipsize(m_smallFont, transferLabel(m_packageTransferState), sheet.width - 44.f, 0.74f),
                     {sheet.x + (m_packageTransferState.isRunning() ? 34.f : 14.f), sheet.y + 10.f},
                     m_smallFont,
                     statusColor.withAlpha(detailOpacity),
                     0.74f);
        drawTransferProgressTrack(ren,
                                  {sheet.x + 14.f, sheet.bottom() - 16.f, sheet.width - 28.f, 7.f},
                                  m_packageTransferState.progress01(),
                                  detailOpacity,
                                  m_theme->cursorNormal);
    }

    float fullscreenT = std::clamp(m_detailFullscreenAnim.value(), 0.f, 1.f);
    bool fullscreenVisible = m_detailFullscreen || fullscreenT > 0.01f;
    if (fullscreenVisible) {
        FullscreenOverlayLayout fullscreen = makeFullscreenOverlayLayout(content);
        nxui::Rect animatedPreview = lerpRect(preview, fullscreen.preview, fullscreenT);
        ren.drawRoundedRect(content, nxui::Color(0.f, 0.f, 0.f, 0.58f * detailOpacity * fullscreenT), 26.f);
        drawThemePreview(ren,
                         m_smallFont,
                         m_theme,
                         animatedPreview,
                         detailPreviewTexture,
                         detailPreviewLabel,
                         detailPreviewLoading,
                         m_uiTime,
                         detailOpacity,
                         detailSheetCols, detailSheetRows, detailSheetFps,
                         false, false,
                         detailPreviewAdaptive,
                         isIconsTab(),
                         nullptr,
                         nullptr,
                         !isIconsTab());

        drawActionButtonChip(ren,
                             m_smallFont,
                             fullscreen.close,
                             i18n.tr("button.close", "Close"),
                             m_theme,
                             m_theme->textPrimary,
                             detailOpacity * fullscreenT,
                             0.f,
                             0.38f,
                             0.84f);

        if (detailScreenshotTotal > 1) {
            drawActionButtonChip(ren,
                                 m_smallFont,
                                 fullscreen.prev,
                                 i18n.tr("themeshop.preview.prev", "Prev"),
                                 m_theme,
                                 m_theme->textPrimary,
                                 detailOpacity * fullscreenT,
                                 0.f,
                                 0.32f,
                                 0.84f);
            drawActionButtonChip(ren,
                                 m_smallFont,
                                 fullscreen.next,
                                 i18n.tr("themeshop.preview.next", "Next"),
                                 m_theme,
                                 m_theme->textPrimary,
                                 detailOpacity * fullscreenT,
                                 0.f,
                                 0.32f,
                                 0.84f);
            drawChip(ren,
                     m_smallFont,
                     fullscreen.counter,
                     std::to_string(m_detailScreenshotIndex + 1) + " / " + std::to_string(detailScreenshotTotal),
                     nxui::Color(0.f, 0.f, 0.f, 0.50f),
                     nxui::Color(1.f, 1.f, 1.f, 0.24f),
                     m_theme->textPrimary,
                     detailOpacity * fullscreenT,
                     0.70f);
        }

        if (m_detailOpen)
            m_focusCursor.moveTo(fullscreen.close.expanded(2.f), 20.f, 0.08f);
        return;
    }

    std::vector<std::string> buttonLabels;
    if (isMusicTab()) {
        const auto* track = selectedMusicTrack();
        if (track && track->isDownloading) {
            buttonLabels.push_back(i18n.tr("themeshop.music.downloading", "Downloading..."));
        } else if (track && track->isDownloaded) {
            buttonLabels.push_back(i18n.tr("themeshop.music.play_track", "Play Track"));
            buttonLabels.push_back(i18n.tr("themeshop.music.delete_track", "Delete Track"));
        } else {
            buttonLabels.push_back(i18n.tr("themeshop.music.download_action", "Download Track (.mp3)"));
        }
    } else if (isCommunityTab()) {
        const auto* catalogueEntry = selectedCommunityThemeEntry();
        const bool alreadyInstalled =
            catalogueEntry && installedEntryForCatalogue(catalogueEntry->id) != nullptr;
        if (alreadyInstalled) {
            buttonLabels.push_back(i18n.tr("themeshop.installed.apply", "Apply Theme"));
            buttonLabels.push_back(i18n.tr("themeshop.installed.remove", "Remove Theme"));
        } else {
            buttonLabels.push_back(i18n.tr("themeshop.community.install", "Install"));
            buttonLabels.push_back(i18n.tr("themeshop.community.download_apply", "Download + Apply"));
        }
    } else if (isIconsTab()) {
        const auto* entry = selectedThemeShopEntry();
        if (entry && !entry->installed) {
            buttonLabels.push_back(i18n.tr("themeshop.community.install", "Install"));
            buttonLabels.push_back(i18n.tr("themeshop.community.download_apply", "Download + Apply"));
        } else {
            buttonLabels.push_back(i18n.tr("themeshop.icons.apply", "Apply Icon Pack"));
            if (entry && entry->removable)
                buttonLabels.push_back(i18n.tr("themeshop.icons.remove", "Remove Icon Pack"));
        }
    } else {
        buttonLabels.push_back(i18n.tr("themeshop.installed.apply", "Apply Theme"));
        const auto* entry = selectedThemeShopEntry();
        if (entry && entry->removable)
            buttonLabels.push_back(i18n.tr("themeshop.installed.remove", "Remove Theme"));
    }

    auto buttons = detailButtonRects(dialog, (int)buttonLabels.size());
    bool disableCommunityButtons = isCommunityTab() && m_packageTransferState.isRunning();
    for (int i = 0; i < (int)buttons.size(); ++i) {
        bool selectedButton = !disableCommunityButtons && (i == m_detailButtonIndex)
            && (m_detailFocusArea == DetailFocusArea::Buttons);
        // Primary CTA (Apply / Install) reads stronger — eShop "Get" energy.
        const bool primary = (i == 0);
        nxui::Color textColor = disableCommunityButtons ? m_theme->textSecondary : m_theme->textPrimary;
        const float fill = disableCommunityButtons
            ? 0.f
            : (selectedButton ? 1.f : (primary ? 0.38f : 0.14f));
        drawActionButtonChip(ren,
                             m_smallFont,
                             buttons[(size_t)i],
                             buttonLabels[(size_t)i],
                             m_theme,
                             textColor,
                             contentOpacity,
                             selectedButton ? 1.f : 0.f,
                             fill,
                             primary ? 0.88f : 0.80f);
    }

    const bool tintFocus = m_detailFocusArea == DetailFocusArea::TintToggle
        || m_detailFocusArea == DetailFocusArea::MatchThemeToggle
        || m_detailFocusArea == DetailFocusArea::FlattenToggle
        || m_detailFocusArea == DetailFocusArea::GrayscaleToggle
        || m_detailFocusArea == DetailFocusArea::TintHue
        || m_detailFocusArea == DetailFocusArea::TintSat
        || m_detailFocusArea == DetailFocusArea::TintLight;
    // Only drive the ring while detail is actually open - during the close
    // anim the grid snap (above) owns focus so we don't jump Apply -> card.
    if (m_detailOpen
        && !(m_detailFocusArea == DetailFocusArea::Preview && detailPreviewRequested)
        && !tintFocus
        && !buttons.empty() && !disableCommunityButtons) {
        m_focusCursor.moveTo(buttons[(size_t)std::clamp(m_detailButtonIndex, 0, (int)buttons.size() - 1)].expanded(2.f),
                             20.f,
                             0.08f);
    }
}
