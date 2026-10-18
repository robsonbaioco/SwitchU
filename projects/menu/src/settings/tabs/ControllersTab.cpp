#include "TabBuilders.hpp"
#include "core/DebugLog.hpp"
#include <nxui/core/I18n.hpp>
#include <switch.h>
#include <algorithm>
#include <string>

namespace {

// Rumbles every connected controller for half a second and logs, per player,
// which style it reports, how many vibration devices hid gave for it and what
// sending to them returned. Reported as "player 2 vibrates in a game, player 1
// does not, with vibration switched on": the log tells a controller with no
// motor (or a third-party pad that ignores HD rumble) from a send that failed.
// Returns the players that accepted the command, e.g. "1 2".
std::string runVibrationTest() {
    struct Target {
        HidNpadIdType id;
        int player;
    };
    static const Target kTargets[] = {
        {HidNpadIdType_Handheld, 0}, {HidNpadIdType_No1, 1}, {HidNpadIdType_No2, 2},
        {HidNpadIdType_No3, 3}, {HidNpadIdType_No4, 4}, {HidNpadIdType_No5, 5},
        {HidNpadIdType_No6, 6}, {HidNpadIdType_No7, 7}, {HidNpadIdType_No8, 8},
    };
    // One style per pad, the one hid would pick to drive its motors.
    static const HidNpadStyleTag kStyles[] = {
        HidNpadStyleTag_NpadFullKey, HidNpadStyleTag_NpadHandheld,
        HidNpadStyleTag_NpadJoyDual, HidNpadStyleTag_NpadJoyLeft,
        HidNpadStyleTag_NpadJoyRight,
    };

    bool permitted = false;
    hidIsVibrationPermitted(&permitted);
    float volume = -1.f;
    setsysGetVibrationMasterVolume(&volume);
    DebugLog::log("[vibration-test] start permitted=%d masterVolume=%.2f",
                  permitted ? 1 : 0, volume);

    HidVibrationDeviceHandle sent[18];
    int sentCount = 0;
    std::string accepted;
    for (const auto& target : kTargets) {
        const u32 styleSet = hidGetNpadStyleSet(target.id);
        if (styleSet == 0)
            continue;
        HidNpadStyleTag style = HidNpadStyleTag_NpadFullKey;
        bool found = false;
        for (HidNpadStyleTag candidate : kStyles) {
            if (styleSet & candidate) { style = candidate; found = true; break; }
        }
        if (!found) {
            DebugLog::log("[vibration-test] player=%d style=0x%X: no vibrating style",
                          target.player, styleSet);
            continue;
        }
        const int handleCount =
            (style == HidNpadStyleTag_NpadJoyLeft || style == HidNpadStyleTag_NpadJoyRight) ? 1 : 2;
        HidVibrationDeviceHandle handles[2]{};
        Result rc = hidInitializeVibrationDevices(handles, handleCount, target.id, style);
        HidVibrationValue values[2]{};
        for (auto& v : values) {
            v.amp_low = 0.6f;
            v.freq_low = 160.f;
            v.amp_high = 0.6f;
            v.freq_high = 320.f;
        }
        Result sendRc = R_FAILED(rc) ? rc : hidSendVibrationValues(handles, values, handleCount);
        HidVibrationDeviceInfo info{};
        const Result infoRc = R_SUCCEEDED(rc) ? hidGetVibrationDeviceInfo(handles[0], &info) : rc;
        DebugLog::log("[vibration-test] player=%d style=0x%X used=0x%X init=0x%X send=0x%X "
                      "device type=%u position=%u infoRc=0x%X",
                      target.player, styleSet, static_cast<u32>(style), rc, sendRc,
                      static_cast<unsigned>(info.type), static_cast<unsigned>(info.position), infoRc);
        if (R_SUCCEEDED(sendRc)) {
            for (int i = 0; i < handleCount && sentCount < 18; ++i)
                sent[sentCount++] = handles[i];
            if (!accepted.empty()) accepted += ' ';
            accepted += target.player == 0 ? "H" : std::to_string(target.player);
        }
    }

    if (sentCount > 0) {
        svcSleepThread(500'000'000ULL);
        HidVibrationValue stop[18]{};
        for (auto& v : stop) {
            v.freq_low = 160.f;
            v.freq_high = 320.f;
        }
        hidSendVibrationValues(sent, stop, sentCount);
    }
    DebugLog::log("[vibration-test] done accepted='%s'", accepted.c_str());
    return accepted;
}

} // namespace

SettingsScreen::Tab settings::tabs::ControllersTab::build(SettingsScreen& screen) {

    using Tab = SettingsScreen::Tab;
    using SettingItem = SettingsScreen::SettingItem;
    using ItemType = SettingsScreen::ItemType;
    auto& i18n = nxui::I18n::instance();
    Tab t;
    t.name = i18n.tr("settings.tabs.controllers", "Controllers");

    {
        SettingItem it;
        it.label = i18n.tr("settings.controllers.test", "Test Input Devices");
        it.buttonLabel = i18n.tr("button.test", "Test");
        it.description = i18n.tr(
            "settings.controllers.test_desc",
            "Test joysticks, buttons, and the touch screen.");
        it.type = ItemType::Action;
        it.onChange = [&screen](SettingItem&) {
            if (screen.m_controllerTestCb)
                screen.m_controllerTestCb();
        };
        t.items.push_back(std::move(it));
    }

    {
        SettingItem it;
        it.label = i18n.tr("settings.controllers.pairing", "Change Grip/Order");
        it.buttonLabel = i18n.tr("button.open", "Open");
        it.description = i18n.tr(
            "settings.controllers.pairing_desc",
            "Pair controllers and change their player order.");
        it.type = ItemType::Action;
        it.onChange = [&screen](SettingItem&) {
            if (screen.m_controllerPairingCb)
                screen.m_controllerPairingCb();
        };
        t.items.push_back(std::move(it));
    }

    {
        SettingItem it;
        it.label = i18n.tr("settings.controllers.remapping", "Change Button Mapping");
        it.buttonLabel = i18n.tr("button.open", "Open");
        it.description = i18n.tr(
            "settings.controllers.remapping_desc",
            "Open the system controller button mapping tool.");
        it.type = ItemType::Action;
        it.onChange = [&screen](SettingItem&) {
            if (screen.m_controllerRemappingCb)
                screen.m_controllerRemappingCb();
        };
        t.items.push_back(std::move(it));
    }

    {
        // The console's own "Controller Vibration" switch is hid's vibration
        // permission, not the setsys master volume this used to show: the
        // volume stays at 1.0 when vibration is switched off elsewhere, so the
        // launcher said "on" while no game could vibrate. Read and write the
        // permission; the volume is only raised from 0 so that switching on
        // here can never leave the motors silent.
        SettingItem it; it.label = i18n.tr("settings.controllers.vibration", "Vibration"); it.type = ItemType::Toggle;
        bool permitted = true;
        const Result permittedRc = hidIsVibrationPermitted(&permitted);
        float vol = 1.f;
        setsysGetVibrationMasterVolume(&vol);
        DebugLog::log("[settings-vibration] permitted=%d rc=0x%X masterVolume=%.2f",
                      permitted ? 1 : 0, permittedRc, vol);
        it.boolVal = permitted;
        it.anim01 = permitted ? 1.f : 0.f;
        it.onChange = [](SettingItem& self) {
            Result rc = hidPermitVibration(self.boolVal);
            float volume = 1.f;
            if (R_SUCCEEDED(rc) && self.boolVal
                && R_SUCCEEDED(setsysGetVibrationMasterVolume(&volume)) && volume <= 0.f)
                setsysSetVibrationMasterVolume(1.f);
            bool after = self.boolVal;
            hidIsVibrationPermitted(&after);
            DebugLog::log("[settings-vibration] set=%d rc=0x%X now=%d",
                          self.boolVal ? 1 : 0, rc, after ? 1 : 0);
            if (R_FAILED(rc))
                self.boolVal = !self.boolVal;
        };
        t.items.push_back(std::move(it));
    }

    {
        SettingItem it;
        it.label = i18n.tr("settings.controllers.vibration_test", "Test Vibration");
        it.buttonLabel = i18n.tr("button.test", "Test");
        it.description = i18n.tr(
            "settings.controllers.vibration_test_desc",
            "Vibrates every connected controller for half a second. One that does not move has no motor or ignores the command.");
        it.type = ItemType::Action;
        it.onChange = [&screen](SettingItem&) {
            auto& tr = nxui::I18n::instance();
            const std::string accepted = runVibrationTest();
            screen.requestToast(accepted.empty()
                ? tr.tr("settings.controllers.vibration_test_none",
                        "No controller accepted the vibration command.")
                : tr.tr("settings.controllers.vibration_test_sent",
                        "Vibration sent to players: ") + accepted, 3.5f);
        };
        t.items.push_back(std::move(it));
    }

    {
        SettingItem it; it.label = i18n.tr("settings.controllers.usb_wired", "USB Wired Communication"); it.type = ItemType::Toggle;
        it.description = i18n.tr("settings.controllers.usb_wired_desc", "Use USB wired mode to reduce latency on compatible controllers.");
        bool val = false;
        setsysGetUsbFullKeyEnableFlag(&val);
        it.boolVal = val;
        it.anim01 = val ? 1.f : 0.f;
        it.onChange = [](SettingItem& self) {
            setsysSetUsbFullKeyEnableFlag(self.boolVal);
        };
        t.items.push_back(std::move(it));
    }

    {
        SettingItem it; it.label = i18n.tr("settings.controllers.nfc", "NFC (amiibo)"); it.type = ItemType::Toggle;
        bool val = true;
        setsysGetNfcEnableFlag(&val);
        it.boolVal = val;
        it.anim01 = val ? 1.f : 0.f;
        it.onChange = [](SettingItem& self) {
            setsysSetNfcEnableFlag(self.boolVal);
        };
        t.items.push_back(std::move(it));
    }

    return t;
}
