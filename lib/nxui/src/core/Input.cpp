#include <nxui/core/Input.hpp>
#include <algorithm>
#include <cmath>

namespace nxui {

namespace {

constexpr float kScreenWidth = 1280.f;
constexpr float kScreenHeight = 720.f;
constexpr float kPointerCenterX = kScreenWidth * 0.5f;
constexpr float kPointerCenterY = kScreenHeight * 0.5f;
constexpr float kGyroDeadzone = 0.002f;
constexpr float kNavStickDisableThreshold = 0.30f;

float applyGyroDeadzone(float value) {
    if (std::abs(value) <= kGyroDeadzone)
        return 0.f;

    return value;
}

// A single Joy-Con arrives as an upright controller (its state is reported
// raw, whatever the hold type). Held sideways - the only way HOME is used with
// one - its four buttons act as A/B/X/Y by where they sit, SL/SR as L/R, and
// its stick is turned a quarter: counter-clockwise for the left Joy-Con,
// clockwise for the right one.
struct ButtonMap { uint64_t from, to; };

constexpr ButtonMap kLeftJoyConSideways[] = {
    {HidNpadButton_Down,  HidNpadButton_A},
    {HidNpadButton_Left,  HidNpadButton_B},
    {HidNpadButton_Right, HidNpadButton_X},
    {HidNpadButton_Up,    HidNpadButton_Y},
    {HidNpadButton_LeftSL, HidNpadButton_L},
    {HidNpadButton_LeftSR, HidNpadButton_R},
};

constexpr ButtonMap kRightJoyConSideways[] = {
    {HidNpadButton_X, HidNpadButton_A},
    {HidNpadButton_A, HidNpadButton_B},
    {HidNpadButton_Y, HidNpadButton_X},
    {HidNpadButton_B, HidNpadButton_Y},
    {HidNpadButton_RightSL, HidNpadButton_L},
    {HidNpadButton_RightSR, HidNpadButton_R},
};

constexpr uint64_t kStickDirectionBits =
    HidNpadButton_StickLLeft | HidNpadButton_StickLUp
    | HidNpadButton_StickLRight | HidNpadButton_StickLDown
    | HidNpadButton_StickRLeft | HidNpadButton_StickRUp
    | HidNpadButton_StickRRight | HidNpadButton_StickRDown;

// Half deflection, like the pseudo-buttons HID derives from an upright stick.
constexpr int32_t kStickDirectionThreshold = 16384;

uint64_t leftStickDirections(const HidAnalogStickState& stick) {
    uint64_t bits = 0;
    if (stick.x <= -kStickDirectionThreshold) bits |= HidNpadButton_StickLLeft;
    if (stick.x >=  kStickDirectionThreshold) bits |= HidNpadButton_StickLRight;
    if (stick.y >=  kStickDirectionThreshold) bits |= HidNpadButton_StickLUp;
    if (stick.y <= -kStickDirectionThreshold) bits |= HidNpadButton_StickLDown;
    return bits;
}

// Buttons of a sideways Joy-Con; its stick becomes the left stick.
uint64_t mapSidewaysJoyCon(const HidNpadCommonState& state, bool left,
                           HidAnalogStickState& stick) {
    const auto& table = left ? kLeftJoyConSideways : kRightJoyConSideways;
    uint64_t buttons = state.buttons & ~kStickDirectionBits;
    for (const auto& map : table)
        buttons &= ~map.from;
    for (const auto& map : table)
        if (state.buttons & map.from)
            buttons |= map.to;

    const HidAnalogStickState& raw = left ? state.analog_stick_l : state.analog_stick_r;
    stick.x = left ? -raw.y : raw.y;
    stick.y = left ? raw.x : -raw.x;
    return buttons | leftStickDirections(stick);
}

size_t readNpadState(HidNpadIdType id, uint32_t style, HidNpadCommonState& state) {
    // GameCube uses a sibling state (same button/stick lifo as FullKey plus
    // analog triggers). Prefer it when the pad is in Gc style so adapters that
    // only advertise NpadGc are not missed.
    if (style & HidNpadStyleTag_NpadGc) {
        HidNpadGcState gc{};
        const size_t n = hidGetNpadStatesGc(id, &gc, 1);
        if (n == 0)
            return 0;
        state.sampling_number = gc.sampling_number;
        state.buttons = gc.buttons;
        state.analog_stick_l = gc.analog_stick_l;
        state.analog_stick_r = gc.analog_stick_r;
        state.attributes = gc.attributes;
        state.reserved = 0;
        return n;
    }
    if (style & HidNpadStyleTag_NpadFullKey)   return hidGetNpadStatesFullKey(id, &state, 1);
    if (style & HidNpadStyleTag_NpadHandheld)  return hidGetNpadStatesHandheld(id, &state, 1);
    if (style & HidNpadStyleTag_NpadJoyDual)   return hidGetNpadStatesJoyDual(id, &state, 1);
    if (style & HidNpadStyleTag_NpadJoyLeft)   return hidGetNpadStatesJoyLeft(id, &state, 1);
    if (style & HidNpadStyleTag_NpadJoyRight)  return hidGetNpadStatesJoyRight(id, &state, 1);
    if (style & HidNpadStyleTag_NpadSystemExt) return hidGetNpadStatesSystemExt(id, &state, 1);
    return 0;
}

// Standard Joy/Pro set plus GameCube adapters and generic external pads, so
// player 2+ and a USB GameCube adapter can drive HOME. NpadStandard alone
// drops Gc.
constexpr uint32_t kSupportedNpadStyles =
    HidNpadStyleSet_NpadStandard
    | HidNpadStyleTag_NpadGc
    | HidNpadStyleTag_NpadSystemExt;

constexpr HidNpadIdType kNpadIds[] = {
    HidNpadIdType_Handheld,
    HidNpadIdType_No1, HidNpadIdType_No2, HidNpadIdType_No3, HidNpadIdType_No4,
    HidNpadIdType_No5, HidNpadIdType_No6, HidNpadIdType_No7, HidNpadIdType_No8,
};

} // namespace

void Input::initialize() {
    padConfigureInput(8, kSupportedNpadStyles);
    // A home menu answers to whichever controller is in the player's hands,
    // not just to player 1. Buttons and sticks are read per npad in update();
    // the merged pad is kept for the style set the six-axis pointer needs.
    padInitializeAny(&m_pad);
    // Single Joy-Con are held sideways here, the way qlaunch does it. Without
    // this the system reports a lone left Joy-Con in its upright layout, where
    // it has a d-pad and no action buttons at all, so nothing can be confirmed.
    hidSetNpadJoyHoldType(HidNpadJoyHoldType_Horizontal);
    hidInitializeTouchScreen();

    recenterVirtualPointer();
    m_prevUpdateTick = armGetSystemTick();

}

void Input::shutdown() {
    stopVirtualPointerSensors();
    m_virtualPointerEnabled = false;
}

void Input::startVirtualPointerSensors() {
    if (m_sixAxisRunning)
        return;

    m_hasHandheldSixAxis = R_SUCCEEDED(hidGetSixAxisSensorHandles(
        &m_handheldSixAxisHandle, 1, HidNpadIdType_Handheld, HidNpadStyleTag_NpadHandheld));
    m_hasFullKeySixAxis = R_SUCCEEDED(hidGetSixAxisSensorHandles(
        &m_fullKeySixAxisHandle, 1, HidNpadIdType_No1, HidNpadStyleTag_NpadFullKey));
    m_hasJoyDualSixAxis = R_SUCCEEDED(hidGetSixAxisSensorHandles(
        m_joyDualSixAxisHandles, 2, HidNpadIdType_No1, HidNpadStyleTag_NpadJoyDual));

    if (m_hasHandheldSixAxis)
        hidStartSixAxisSensor(m_handheldSixAxisHandle);
    if (m_hasFullKeySixAxis)
        hidStartSixAxisSensor(m_fullKeySixAxisHandle);
    if (m_hasJoyDualSixAxis) {
        hidStartSixAxisSensor(m_joyDualSixAxisHandles[0]);
        hidStartSixAxisSensor(m_joyDualSixAxisHandles[1]);
    }
    m_sixAxisRunning = m_hasHandheldSixAxis || m_hasFullKeySixAxis || m_hasJoyDualSixAxis;
}

void Input::stopVirtualPointerSensors() {
    if (!m_sixAxisRunning)
        return;
    if (m_hasHandheldSixAxis) {
        hidStopSixAxisSensor(m_handheldSixAxisHandle);
        m_hasHandheldSixAxis = false;
    }
    if (m_hasFullKeySixAxis) {
        hidStopSixAxisSensor(m_fullKeySixAxisHandle);
        m_hasFullKeySixAxis = false;
    }
    if (m_hasJoyDualSixAxis) {
        hidStopSixAxisSensor(m_joyDualSixAxisHandles[0]);
        hidStopSixAxisSensor(m_joyDualSixAxisHandles[1]);
        m_hasJoyDualSixAxis = false;
    }
    m_sixAxisRunning = false;
}

void Input::setVirtualPointerEnabled(bool enabled) {
    if (m_virtualPointerEnabled == enabled)
        return;
    m_virtualPointerEnabled = enabled;
    if (enabled)
        startVirtualPointerSensors();
    else
        stopVirtualPointerSensors();
}

void Input::setVirtualPointerShortcutsEnabled(bool enabled) {
    m_virtualPointerShortcutsEnabled = enabled;
    if (!enabled)
        setVirtualPointerEnabled(false);
}

void Input::recenterVirtualPointer() {
    m_virtualPointerX = kPointerCenterX;
    m_virtualPointerY = kPointerCenterY;
}

bool Input::readActiveSixAxisState(HidSixAxisSensorState& out) const {
    u32 styleSet = padGetStyleSet(&m_pad);
    if ((styleSet & HidNpadStyleTag_NpadHandheld) && m_hasHandheldSixAxis) {
        return hidGetSixAxisSensorStates(m_handheldSixAxisHandle, &out, 1) > 0;
    }

    if ((styleSet & HidNpadStyleTag_NpadFullKey) && m_hasFullKeySixAxis) {
        return hidGetSixAxisSensorStates(m_fullKeySixAxisHandle, &out, 1) > 0;
    }

    if ((styleSet & HidNpadStyleTag_NpadJoyDual) && m_hasJoyDualSixAxis) {
        u32 attributes = padGetAttributes(&m_pad);
        if ((attributes & HidNpadAttribute_IsRightConnected)
            && hidGetSixAxisSensorStates(m_joyDualSixAxisHandles[1], &out, 1) > 0) {
            return true;
        }
        if ((attributes & HidNpadAttribute_IsLeftConnected)
            && hidGetSixAxisSensorStates(m_joyDualSixAxisHandles[0], &out, 1) > 0) {
            return true;
        }
    }

    return false;
}

void Input::update() {
    uint64_t nowTick = armGetSystemTick();
    float dt = m_prevUpdateTick
        ? static_cast<float>(nowTick - m_prevUpdateTick) / static_cast<float>(armGetSystemTickFreq())
        : (1.f / 60.f);
    m_prevUpdateTick = nowTick;
    if (dt > 0.05f) dt = 1.f / 60.f;

    // Still updated for padGetStyleSet/padGetAttributes in the six-axis path.
    padUpdate(&m_pad);

    // Every controller is read on its own: padUpdate merges them raw, which
    // left a lone sideways Joy-Con with turned-around buttons and stick.
    uint64_t held = 0;
    HidAnalogStickState ls{}, rs{};
    auto takeMoving = [](HidAnalogStickState& out, const HidAnalogStickState& stick) {
        if (stick.x != 0 || stick.y != 0)
            out = stick;
    };
    for (const HidNpadIdType id : kNpadIds) {
        const uint32_t style = hidGetNpadStyleSet(id);
        if (style == 0)
            continue;
        HidNpadCommonState state{};
        if (readNpadState(id, style, state) == 0
            || !(state.attributes & HidNpadAttribute_IsConnected))
            continue;

        const bool single = !(style & (HidNpadStyleTag_NpadFullKey
                                       | HidNpadStyleTag_NpadHandheld
                                       | HidNpadStyleTag_NpadJoyDual
                                       | HidNpadStyleTag_NpadGc
                                       | HidNpadStyleTag_NpadSystemExt));
        const bool leftJoyCon  = single && (style & HidNpadStyleTag_NpadJoyLeft);
        const bool rightJoyCon = single && (style & HidNpadStyleTag_NpadJoyRight);
        if (leftJoyCon || rightJoyCon) {
            HidAnalogStickState stick{};
            held |= mapSidewaysJoyCon(state, leftJoyCon, stick);
            takeMoving(ls, stick);
            continue;
        }
        held |= state.buttons;
        takeMoving(ls, state.analog_stick_l);
        takeMoving(rs, state.analog_stick_r);
    }
    m_kDown = held & ~m_kHeld;
    m_kUp   = m_kHeld & ~held;
    m_kHeld = held;

    // Analog sticks
    m_lx = ls.x / 32767.f;
    m_ly = ls.y / 32767.f;
    m_rx = rs.x / 32767.f;
    m_ry = rs.y / 32767.f;

    HidTouchScreenState tState = {};
    bool hardwareTouching = hidGetTouchScreenStates(&tState, 1) && tState.count > 0;
    float hardwareTouchX = hardwareTouching ? tState.touches[0].x : m_touchX;
    float hardwareTouchY = hardwareTouching ? tState.touches[0].y : m_touchY;

    bool navInputUsed =
        isDown(Button::DLeft)  || isDown(Button::DRight) ||
        isDown(Button::DUp)    || isDown(Button::DDown)  ||
        isDown(Button::LStickL)|| isDown(Button::LStickR)||
        isDown(Button::LStickU)|| isDown(Button::LStickD)||
        isDown(Button::RStickL)|| isDown(Button::RStickR)||
        isDown(Button::RStickU)|| isDown(Button::RStickD)||
        std::abs(m_lx) >= kNavStickDisableThreshold ||
        std::abs(m_ly) >= kNavStickDisableThreshold ||
        std::abs(m_rx) >= kNavStickDisableThreshold ||
        std::abs(m_ry) >= kNavStickDisableThreshold;

    if (hardwareTouching || navInputUsed) {
        setVirtualPointerEnabled(false);
    }

    if (m_virtualPointerShortcutsEnabled && isDown(Button::LStick)) {
        setVirtualPointerEnabled(!m_virtualPointerEnabled);
    }
    if (m_virtualPointerShortcutsEnabled && isDown(Button::RStick)) {
        recenterVirtualPointer();
    }

    if (m_virtualPointerEnabled) {
        HidSixAxisSensorState sixAxis = {};
        if (readActiveSixAxisState(sixAxis)) {
            float roll = applyGyroDeadzone(sixAxis.angular_velocity.z);
            float pitch = applyGyroDeadzone(sixAxis.angular_velocity.x);

            m_virtualPointerX = std::clamp(
                m_virtualPointerX - roll * m_virtualPointerSensitivity * dt,
                0.f, kScreenWidth - 1.f);
            m_virtualPointerY = std::clamp(
                m_virtualPointerY - pitch * m_virtualPointerSensitivity * dt,
                0.f, kScreenHeight - 1.f);
        }
    }

    m_wasTouching = m_touching;
    if (hardwareTouching) {
        m_touchX = hardwareTouchX;
        m_touchY = hardwareTouchY;
    } else if (m_virtualPointerEnabled) {
        m_touchX = m_virtualPointerX;
        m_touchY = m_virtualPointerY;
    }

    bool virtualTouching = !hardwareTouching
        && m_virtualPointerEnabled
        && (m_kHeld & static_cast<uint64_t>(Button::A));

    m_touching = hardwareTouching || virtualTouching;

    m_touchDown = (m_touching && !m_wasTouching);
    m_touchUp   = (!m_touching && m_wasTouching);

    if (m_touchDown) {
        m_touchStartX = m_touchX;
        m_touchStartY = m_touchY;
        m_touchStartTick = armGetSystemTick();
    }
}

float Input::touchDuration() const {
    uint64_t now = armGetSystemTick();
    return static_cast<float>(now - m_touchStartTick) / static_cast<float>(armGetSystemTickFreq());
}

bool Input::isDown(Button b) const { return m_kDown & static_cast<uint64_t>(b); }
bool Input::isUp(Button b)   const { return m_kUp   & static_cast<uint64_t>(b); }
bool Input::isHeld(Button b) const { return m_kHeld & static_cast<uint64_t>(b); }

bool Input::hasUserActivity() const {
    constexpr float kActivityStickThreshold = 0.08f;
    return m_kDown != 0 || m_kUp != 0 || m_touching || m_touchDown || m_touchUp
        || m_virtualPointerEnabled
        || std::abs(m_lx) >= kActivityStickThreshold
        || std::abs(m_ly) >= kActivityStickThreshold
        || std::abs(m_rx) >= kActivityStickThreshold
        || std::abs(m_ry) >= kActivityStickThreshold;
}

} // namespace nxui
