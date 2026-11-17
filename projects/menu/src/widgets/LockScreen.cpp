#include "LockScreen.hpp"
#include "core/DebugLog.hpp"
#include <nxui/core/Renderer.hpp>
#include <nxui/core/Font.hpp>
#include <nxui/core/Input.hpp>
#include <nxui/core/I18n.hpp>
#include <nxui/core/Animation.hpp>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>

namespace {

// Every button the sequence accepts, and the same set that counts as activity.
// The list is deliberately wide: whichever button the owner reaches for first
// is the one they will press three times.
constexpr nxui::Button kButtons[] = {
    nxui::Button::A,      nxui::Button::B,      nxui::Button::X,      nxui::Button::Y,
    nxui::Button::L,      nxui::Button::R,      nxui::Button::ZL,     nxui::Button::ZR,
    nxui::Button::Plus,   nxui::Button::Minus,  nxui::Button::DUp,    nxui::Button::DDown,
    nxui::Button::DLeft,  nxui::Button::DRight, nxui::Button::LStick, nxui::Button::RStick,
};

// A touch counts as its own button, so the sequence still works handheld with
// no controller attached. The value sits outside the HID button mask.
constexpr std::uint64_t kTouchButton = 1ull << 63;

// The stick has to be pushed rather than breathed on. A drifting stick must not
// hold the lock off forever, which is the failure that makes an idle timer
// useless on worn hardware.
constexpr float kStickActivity = 0.35f;

// Dissolve, the fork's original fade. The other transitions take a little
// longer than PoloNX's would make sense for here: isLocked() stays true until
// the exit finishes, so a long exit is time the menu ignores input.
constexpr float kFadeInDur  = 0.45f;
constexpr float kFadeOutDur = 0.26f;
constexpr float kCurtainEnterDur = 0.55f;
constexpr float kCurtainExitDur  = 0.50f;
constexpr float kZoomEnterDur    = 0.50f;
constexpr float kZoomExitDur     = 0.42f;
constexpr float kWipeEnterDur    = 0.50f;
constexpr float kWipeExitDur     = 0.42f;
// Zoom scale endpoints (PoloNX): Out grows away, In shrinks in.
constexpr float kZoomOutEnter = 1.48f;
constexpr float kZoomInEnter  = 0.52f;
constexpr float kZoomOutExit  = 1.58f;
constexpr float kZoomInExit   = 0.48f;
// Wrap for the atmosphere clock so a lock left up for days keeps its precision.
constexpr float kAtmosWrap = 3600.f;
// How long a half-finished sequence is remembered. Long enough to be unhurried,
// short enough that a bag pressing one button twice never adds up to three.
constexpr float kSequenceLife = 2.5f;
constexpr float kBeadPopDur   = 0.2f;
constexpr float kWakeDisplayDuration = 10.f;
// How long input is ignored after the screen locks, so the press that asked for
// the lock cannot also count as the first press asking to leave it.
constexpr float kInputGrace = 0.4f;
// If a suspend request does not actually put the console under, do not spend the
// next frame asking again. One try, then a pause long enough to be noticed.
constexpr float kSleepRetryCooldown = 30.f;
// One full sweep of the slow drift, in seconds, and how far it travels.
constexpr float kDriftPeriod = 47.f;
constexpr float kDriftRange  = 7.f;

} // namespace

void LockScreen::setSleepDelaySeconds(float seconds) {
    if (seconds < 0.f) seconds = -1.f;
    if (m_sleepDelay == seconds) return;
    DebugLog::log("[lock] suspend plan is now %.0fs", (double)seconds);
    m_sleepDelay = seconds;
    m_sleepIdle = 0.f;
}

void LockScreen::resetSleepCountdown() {
    m_sleepIdle = 0.f;
    m_sleepCooldown = 0.f;
}

void LockScreen::setTransition(Transition mode, int zoomDir, int wipeDir) {
    m_transition = mode;
    m_zoomDir = std::clamp(zoomDir, 0, 1);
    m_wipeDir = std::clamp(wipeDir, 0, 3);
}

float LockScreen::enterDuration() const {
    switch (m_transition) {
        case Transition::None:    return 0.f;
        case Transition::Curtain: return kCurtainEnterDur;
        case Transition::Zoom:    return kZoomEnterDur;
        case Transition::Wipe:    return kWipeEnterDur;
        case Transition::Dissolve:
        default:                  return kFadeInDur;
    }
}

float LockScreen::exitDuration() const {
    switch (m_transition) {
        case Transition::None:    return 0.f;
        case Transition::Curtain: return kCurtainExitDur;
        case Transition::Zoom:    return kZoomExitDur;
        case Transition::Wipe:    return kWipeExitDur;
        case Transition::Dissolve:
        default:                  return kFadeOutDur;
    }
}

void LockScreen::lockNow(bool instant) {
    if (m_state == State::Locked) {
        // Already locked, but maybe mid-enter or showing the wake reveal. A
        // sleep handoff must still reach the black frame at once, or the menu
        // keeps presenting for up to kWakeDisplayDuration into the sleep.
        if (instant && (m_fade < 1.f || m_wakeDisplaySeconds > 0.f)) {
            DebugLog::log("[lock] already locked, snapping to the low-power frame");
            m_fade = 1.f;
            m_wakeDisplaySeconds = 0.f;
        }
        return;
    }
    DebugLog::log("[lock] locked%s", instant ? " (instant)" : "");
    m_inputGrace = kInputGrace;
    // Otherwise the enter transition runs from wherever m_fade is, so a lock
    // that comes back while its exit is still playing reverses smoothly.
    if (instant || enterDuration() <= 0.f)
        m_fade = 1.f;
    m_state = State::Locked;
    m_presses = 0;
    m_sequenceButton = 0;
    m_sequenceAge = 0.f;
    m_beadPop = 0.f;
    m_wakeDisplaySeconds = 0.f;
    m_clockTimer = 1.f;
    if (m_lockedCb) m_lockedCb();
}

void LockScreen::unlockNow() {
    beginRelease();
}

void LockScreen::showAfterSystemWake() {
    if (m_state != State::Locked)
        return;
    DebugLog::log("[lock] system wake reveal for %.0fs", (double)kWakeDisplayDuration);
    m_wakeDisplaySeconds = kWakeDisplayDuration;
}

void LockScreen::reset() {
    m_state = State::Open;
    m_fade = 0.f;
    m_inputGrace = 0.f;
    m_presses = 0;
    m_sequenceButton = 0;
    m_wakeDisplaySeconds = 0.f;
}

void LockScreen::beginRelease() {
    if (m_state != State::Locked) return;
    DebugLog::log("[lock] unlocking after %d press(es)", m_presses);
    m_state = State::Releasing;
    m_presses = 0;
    m_sequenceButton = 0;
    if (m_unlockedCb) m_unlockedCb();
}

bool LockScreen::consumeActivity(nxui::Input& input) {
    for (nxui::Button b : kButtons) {
        if (input.isHeld(b)) return true;
    }
    if (input.isTouching()) return true;
    return std::fabs(input.leftStickX())  > kStickActivity ||
           std::fabs(input.leftStickY())  > kStickActivity ||
           std::fabs(input.rightStickX()) > kStickActivity ||
           std::fabs(input.rightStickY()) > kStickActivity;
}

std::uint64_t LockScreen::pressedUnlockButton(nxui::Input& input) const {
    for (nxui::Button b : kButtons) {
        if (input.isDown(b))
            return static_cast<std::uint64_t>(b);
    }
    return input.touchDown() ? kTouchButton : 0;
}

void LockScreen::updateClock(float dt) {
    m_clockTimer += dt;
    if (m_clockTimer < 1.f && !m_timeStr.empty()) return;
    m_clockTimer = 0.f;

    std::time_t t = std::time(nullptr);
    std::tm* tm = std::localtime(&t);
    if (!tm) return;
    char buf[64];
    if (m_use12Hour) {
        int hour = tm->tm_hour % 12;
        if (hour == 0) hour = 12;
        std::snprintf(buf, sizeof(buf), "%d:%02d %s", hour, tm->tm_min,
                      tm->tm_hour >= 12 ? "PM" : "AM");
    } else {
        std::snprintf(buf, sizeof(buf), "%02d:%02d", tm->tm_hour, tm->tm_min);
    }
    m_timeStr = buf;
    std::snprintf(buf, sizeof(buf), "%02d/%02d/%04d",
                  tm->tm_mday, tm->tm_mon + 1, tm->tm_year + 1900);
    m_dateStr = buf;
}

void LockScreen::update(float dt, nxui::Input& input, bool sleepSuppressed) {
    if (m_beadPop > 0.f)
        m_beadPop = std::max(0.f, m_beadPop - dt);
    if (m_inputGrace > 0.f)
        m_inputGrace = std::max(0.f, m_inputGrace - dt);
    if (m_sleepCooldown > 0.f)
        m_sleepCooldown = std::max(0.f, m_sleepCooldown - dt);

    // The suspend countdown runs in every state. Locking the screen is not the
    // end of being idle: on a real console the lock screen is what the owner
    // walks away from, and the console still goes to sleep behind it.
    if (m_sleepDelay < 0.f || sleepSuppressed || consumeActivity(input) ||
        pressedUnlockButton(input) != 0) {
        m_sleepIdle = 0.f;
    } else {
        m_sleepIdle += dt;
        if (m_sleepIdle >= m_sleepDelay && m_sleepCooldown <= 0.f) {
            DebugLog::log("[lock] idle %.0fs reached suspend plan %.0fs, requesting suspend",
                          (double)m_sleepIdle, (double)m_sleepDelay);
            m_sleepIdle = 0.f;
            m_sleepCooldown = kSleepRetryCooldown;
            if (m_sleepCb) m_sleepCb();
        }
    }

    const float enterDur = enterDuration();
    const float exitDur = exitDuration();
    switch (m_state) {
        case State::Open:
            // Nothing counts down here any more. The screen locks when the
            // console goes under and when it comes back, not on a timer of
            // its own.
            m_fade = exitDur > 0.f ? std::max(0.f, m_fade - dt / exitDur) : 0.f;
            break;
        case State::Locked: {
            m_fade = enterDur > 0.f ? std::min(1.f, m_fade + dt / enterDur) : 1.f;
            m_driftPhase = std::fmod(m_driftPhase + dt, kDriftPeriod);
            m_atmosPhase = std::fmod(m_atmosPhase + dt, kAtmosWrap);
            updateClock(dt);

            // Wake press is deliberately not one of the three unlock presses.
            if (hidesScene()) {
                if (m_inputGrace <= 0.f && pressedUnlockButton(input) != 0)
                    m_wakeDisplaySeconds = kWakeDisplayDuration;
                break;
            }

            if (m_wakeDisplaySeconds > 0.f)
                m_wakeDisplaySeconds = std::max(0.f, m_wakeDisplaySeconds - dt);

            if (m_sequenceButton != 0) {
                m_sequenceAge += dt;
                if (m_sequenceAge > kSequenceLife) {
                    // Drain one step rather than resetting outright, so a slow
                    // hand is not punished as hard as a bumped pocket.
                    m_sequenceAge = 0.f;
                    if (--m_presses <= 0) {
                        m_presses = 0;
                        m_sequenceButton = 0;
                    }
                }
            }

            const std::uint64_t pressed = m_inputGrace > 0.f ? 0 : pressedUnlockButton(input);
            if (pressed == 0)
                break;

            if (pressed != m_sequenceButton) {
                // A different button starts its own count at one. The console
                // asks for the same button three times, and so does this.
                m_sequenceButton = pressed;
                m_presses = 1;
            } else {
                ++m_presses;
            }
            m_sequenceAge = 0.f;
            m_beadPop = kBeadPopDur;

            if (m_presses >= kUnlockPresses) {
                beginRelease();
            } else {
                DebugLog::log("[lock] unlock press %d/%d", m_presses, kUnlockPresses);
                if (m_progressCb) m_progressCb();
            }
            break;
        }
        case State::Releasing: {
            m_fade = exitDur > 0.f ? std::max(0.f, m_fade - dt / exitDur) : 0.f;
            m_atmosPhase = std::fmod(m_atmosPhase + dt, kAtmosWrap);
            if (m_fade <= 0.f)
                m_state = State::Open;
            break;
        }
    }
}

void LockScreen::render(nxui::Renderer& ren) {
    if (m_fade <= 0.001f || !m_theme)
        return;

    auto& i18n = nxui::I18n::instance();
    const float w = static_cast<float>(ren.width());
    const float h = static_cast<float>(ren.height());
    const float fade = std::clamp(m_fade, 0.f, 1.f);

    // Present one all-black frame before WiiUMenuApp stops rendering. This
    // removes all per-frame GPU work and lets OLED panels draw almost no power.
    // LCD backlight control belongs to Horizon, so physical panel sleep still
    // follows the console's normal auto-sleep setting.
    if (hidesScene()) {
        ren.drawRect({0.f, 0.f, w, h}, nxui::Color(0.f, 0.f, 0.f, 1.f));
        return;
    }

    // Map the plane's progress through the chosen transition (PoloNX 4b0c337).
    // The way out mirrors the way in; only Zoom picks different end scales.
    const bool leaving = m_state != State::Locked;
    float ease = 1.f;   // opacity of the whole plane
    float rise = 0.f;   // Dissolve: content rises into place
    float dy = 0.f;     // Curtain: the plane slides down from the top
    float zoom = 1.f;   // Zoom: content scales around the screen centre
    bool clipped = false;
    switch (m_transition) {
        case Transition::Curtain:
            dy = -(1.f - nxui::Easing::inOutCubic(fade)) * h;
            break;
        case Transition::Zoom: {
            const float e = nxui::Easing::inOutCubic(fade);
            const float z0 = leaving ? (m_zoomDir == 1 ? kZoomInExit : kZoomOutExit)
                                     : (m_zoomDir == 1 ? kZoomInEnter : kZoomOutEnter);
            ease = e;
            zoom = z0 + (1.f - z0) * e;
            break;
        }
        case Transition::Wipe: {
            // A straight edge crosses the screen. The direction names the way
            // the lock leaves; entering, the same edge comes back.
            const float r = nxui::Easing::inOutCubic(fade);
            if (r < 0.999f) {
                nxui::Rect clip{0.f, 0.f, w, h};
                switch (m_wipeDir) {
                    case 0:  clip = {0.f, 0.f, w, h * r}; break;             // Up
                    case 2:  clip = {0.f, h * (1.f - r), w, h * r}; break;   // Down
                    case 3:  clip = {0.f, 0.f, w * r, h}; break;             // Left
                    default: clip = {w * (1.f - r), 0.f, w * r, h}; break;   // Right
                }
                ren.pushClipRect(clip);
                clipped = true;
            }
            break;
        }
        case Transition::None:
        case Transition::Dissolve:
        default:
            ease = nxui::Easing::outCubic(fade);
            // Everything rises into place; on the way out it simply fades.
            rise = (1.f - ease) * 14.f;
            break;
    }

    const float cx = w * 0.5f;
    const float cy = h * 0.5f;
    auto mapPt = [&](float x, float y) {
        return nxui::Vec2{cx + (x - cx) * zoom, cy + (y - cy) * zoom + dy};
    };
    auto mapRect = [&](const nxui::Rect& r) {
        const nxui::Vec2 p = mapPt(r.x, r.y);
        return nxui::Rect{p.x, p.y, r.width * zoom, r.height * zoom};
    };

    // The scrim is the theme's own background rather than flat black, so a
    // light theme locks to a pale screen instead of a hole. It never scales:
    // on Zoom only what sits on it moves.
    ren.drawRect({0.f, dy, w, h}, m_theme->background.withAlpha(0.93f * ease));
    drawAtmosphere(ren, w, h, dy, ease);

    const float drift = std::sin(m_driftPhase / kDriftPeriod * 6.2831853f) * kDriftRange;

    if (m_clockFont && !m_timeStr.empty()) {
        // Left-anchored rather than centred. The clock is the one thing worth
        // reading here, and against the left margin it sits where the menu's own
        // headings sit instead of turning the screen into a poster.
        // Drawn at 1:1 at rest. The face behind this is loaded at the size it
        // appears at, so nothing here is an upscale of a smaller bitmap; only
        // a Zoom transition scales it, and only while it moves.
        const nxui::Vec2 timeSz = m_clockFont->measure(m_timeStr);
        const float x = w * 0.075f + drift;
        const float y = h * 0.34f + rise + drift * 0.4f;
        const nxui::Color ink = m_theme->textPrimary.withAlpha(ease);

        // PoloNX's colon pulse: the separator breathes, the digits hold still.
        const float colonPulse = 0.45f + 0.55f * (0.5f + 0.5f * std::sin(m_atmosPhase * 2.4f));
        const auto colon = m_timeStr.find(':');
        auto drawTimePass = [&](const nxui::Color& c) {
            const nxui::Vec2 o = mapPt(x, y);
            if (colon == std::string::npos) {
                ren.drawText(m_timeStr, o, m_clockFont, c, zoom);
                return;
            }
            const std::string left = m_timeStr.substr(0, colon);
            const std::string right = m_timeStr.substr(colon + 1);
            const float leftW = m_clockFont->measure(left).x;
            const float colonW = m_clockFont->measure(":").x;
            ren.drawText(left, o, m_clockFont, c, zoom);
            ren.drawText(":", {o.x + leftW * zoom, o.y}, m_clockFont,
                         c.withAlpha(c.a * colonPulse), zoom);
            ren.drawText(right, {o.x + (leftW + colonW) * zoom, o.y}, m_clockFont, c, zoom);
        };
        drawTimePass(ink);

        // PoloNX's clock wave: a soft highlight band drifting through the
        // digits, the same text drawn again lighter inside a moving clip.
        if (ease > 0.05f && timeSz.x > 1.f) {
            const float textW = timeSz.x * zoom;
            const float band = std::max(72.f, timeSz.x * 0.32f) * zoom;
            const float span = textW + band * 2.f;
            const float waveX = std::fmod(m_atmosPhase * 95.f, span) - band;
            const nxui::Vec2 o = mapPt(x, y);
            float bandX = o.x + waveX;
            float bandW = band;
            if (bandX < 0.f) { bandW += bandX; bandX = 0.f; }
            if (bandW > 1.f) {
                ren.pushClipRect({bandX, o.y - 8.f * zoom, bandW, (timeSz.y + 16.f) * zoom});
                const nxui::Color hi = nxui::Color::lerp(ink, nxui::Color::white(), 0.55f)
                                           .withAlpha(ink.a * 0.55f);
                drawTimePass(hi);
                ren.popClipRect();
            }
        }

        if (m_captionFont && !m_dateStr.empty()) {
            ren.drawText(m_dateStr, mapPt(x + 4.f, y + timeSz.y + 10.f),
                         m_captionFont,
                         m_theme->textSecondary.withAlpha(0.85f * ease), zoom);
        }
    }

    // The beads: one per press the sequence still owes. They are the only moving
    // part, and the only thing on screen that answers "what do I do now".
    const float beadW = 58.f, beadH = 12.f, beadGap = 16.f;
    const float totalW = kUnlockPresses * beadW + (kUnlockPresses - 1) * beadGap;
    const float beadY = h - 104.f + rise + drift * 0.4f;
    const float beadX0 = (w - totalW) * 0.5f + drift;
    const float pop = m_beadPop > 0.f ? (m_beadPop / kBeadPopDur) : 0.f;

    for (int i = 0; i < kUnlockPresses; ++i) {
        const bool filled = i < m_presses;
        const bool newest = filled && i == m_presses - 1;
        const float grow = newest ? nxui::Easing::outCubic(pop) * 3.f : 0.f;
        const nxui::Rect r = mapRect({beadX0 + i * (beadW + beadGap) - grow, beadY - grow * 0.5f,
                                      beadW + grow * 2.f, beadH + grow});
        if (filled) {
            ren.drawRoundedRect(r, m_theme->textPrimary.withAlpha(ease), r.height * 0.5f);
        } else {
            ren.drawRoundedRect(r, m_theme->panelBase.withAlpha(0.5f * ease), r.height * 0.5f);
            ren.drawRoundedRectOutline(r, m_theme->panelBorder.withAlpha(0.9f * ease),
                                       r.height * 0.5f, 2.f);
        }
    }

    if (m_captionFont) {
        const std::string hint = i18n.tr("lock_screen.hint",
                                         "Press the same button three times to unlock");
        const nxui::Vec2 sz = m_captionFont->measure(hint);
        ren.drawText(hint, mapPt((w - sz.x) * 0.5f + drift, beadY - sz.y - 22.f),
                     m_captionFont, m_theme->textSecondary.withAlpha(0.8f * ease), zoom);
    }

    if (clipped)
        ren.popClipRect();
}

void LockScreen::drawAtmosphere(nxui::Renderer& ren, float w, float h, float dy,
                                float alpha) const {
    if (alpha <= 0.01f)
        return;
    // PoloNX's vignette, top and bottom only, breathing slowly. Shade at low
    // alpha reads as depth on a dark theme and as a soft edge on a light one.
    const float breath = 0.5f + 0.5f * std::sin(m_atmosPhase * 0.7f);
    const float vig = 0.28f + 0.10f * breath;
    ren.drawGradientRect({0.f, dy, w, 210.f},
                         nxui::Color(0.f, 0.f, 0.f, vig * alpha),
                         nxui::Color(0.f, 0.f, 0.f, 0.f));
    ren.drawGradientRect({0.f, h - 240.f + dy, w, 240.f},
                         nxui::Color(0.f, 0.f, 0.f, 0.f),
                         nxui::Color(0.f, 0.f, 0.f, (vig + 0.12f) * alpha));
}
