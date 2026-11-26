#include "RestoreActivity.hpp"
#include "core/DebugLog.hpp"
#include <nxui/Application.hpp>
#include <nxui/core/I18n.hpp>
#include <nxui/core/Renderer.hpp>
#include <switchu/smi_protocol.hpp>
#include <switch.h>
#include <fmt/format.h>
#include <algorithm>
#include <cmath>
#include <sys/stat.h>

#ifdef SWITCHU_HOMEBREW
static constexpr const char* RESTORE_ASSETS = "romfs:";
#else
static constexpr const char* RESTORE_ASSETS = "sdmc:/switch/SwitchU";
#endif

namespace switchu::menu {
bool fitManagedLayerToMode(int operationMode);
}

namespace {

constexpr const char* kConfigFile = "sdmc:/config/SwitchU/config.json";

bool configIsBack() {
    struct stat info {};
    return stat(kConfigFile, &info) == 0;
}

std::string trFormat(const char* key, const char* fallback, auto&&... args) {
    const std::string pattern = nxui::I18n::instance().tr(key, fallback);
    try {
        return fmt::format(fmt::runtime(pattern), args...);
    } catch (...) {
        // A translation with broken placeholders must not take the screen down.
        return fmt::format(fmt::runtime(std::string(fallback)), args...);
    }
}

std::string megabytes(uint64_t kib) {
    const double mb = static_cast<double>(kib) / 1024.0;
    return mb >= 100.0 ? fmt::format("{:.0f}", mb) : fmt::format("{:.1f}", mb);
}

} // namespace

RestoreActivity::RestoreActivity(ActivityFactory next) : m_next(std::move(next)) {}

bool RestoreActivity::onCreate() {
    DebugLog::log("[restore] progress screen up; waiting for the daemon to bring config.json back");
    // No configuration to take a language from: the console's own.
    auto& i18n = nxui::I18n::instance();
    i18n.initialize(std::string(RESTORE_ASSETS) + "/i18n", "en-US");
    i18n.setLanguageAuto();

    m_theme = nxui::Theme::dark();
    const std::string fontPath = std::string(RESTORE_ASSETS) + "/fonts/DejaVuSans.ttf";
    m_fontTitle.load(app().gpu(), app().renderer(), fontPath, 34);
    m_fontBody.load(app().gpu(), app().renderer(), fontPath, 22);
    m_fontSmall.load(app().gpu(), app().renderer(), fontPath, 18);

    m_title = i18n.tr("restore.title", "Restoring your settings");
    m_body = i18n.tr("restore.body",
                     "An update erased SwitchU's settings. They are coming back from the backup.");
    refreshText();
    return true;
}

void RestoreActivity::pumpDaemonMessages() {
#ifdef SWITCHU_MENU
    AppletStorage st;
    while (R_SUCCEEDED(appletPopInteractiveInData(&st))) {
        switchu::smi::DaemonNotification notif{};
        s64 size = 0;
        appletStorageGetSize(&st, &size);
        if (size >= (s64)sizeof(notif))
            appletStorageRead(&st, 0, &notif, sizeof(notif));
        appletStorageClose(&st);
        if (notif.magic != switchu::smi::kNotifyMagic)
            continue;

        switch (notif.msg) {
        case switchu::smi::MenuMessage::BackupRestoreProgress:
            if (switchu::smi::restoreProgressPhase(notif.payload) != 1)
                break;
            m_filesDone = switchu::smi::restoreProgressFilesDone(notif.payload);
            m_filesTotal = switchu::smi::restoreProgressFilesTotal(notif.payload);
            m_kibDone = notif.app_id >> 32;
            m_kibTotal = notif.app_id & 0xFFFFFFFFULL;
            m_haveProgress = true;
            break;
        case switchu::smi::MenuMessage::BackupRestoreStage:
            if (notif.payload == static_cast<uint32_t>(switchu::smi::RestoreStage::EssentialsDone) ||
                notif.payload == static_cast<uint32_t>(switchu::smi::RestoreStage::Failed))
                finish(notif.payload == static_cast<uint32_t>(switchu::smi::RestoreStage::Failed)
                           ? "daemon: done with failures" : "daemon: done");
            break;
        // The few the screen has to honour itself. Everything else is about
        // a menu that does not exist yet, and the daemon repeats what matters
        // (battery) once the restore is done.
        case switchu::smi::MenuMessage::SleepSequence:
            app().setRenderEnabled(false);
            break;
        case switchu::smi::MenuMessage::WakeUp:
            // The swapchain does not survive sleep (see WiiUMenuApp).
            app().gpu().recreateSwapchain();
            app().setRenderEnabled(true);
            break;
        case switchu::smi::MenuMessage::OperationModeChanged:
            switchu::menu::fitManagedLayerToMode(static_cast<int>(notif.payload));
            break;
        default:
            break;
        }
    }
#endif
}

void RestoreActivity::refreshText() {
    if (!m_haveProgress || m_filesTotal == 0) {
        m_detail = nxui::I18n::instance().tr("restore.preparing", "Preparing...");
        return;
    }
    m_detail = trFormat("restore.progress", "{} of {} files - {} of {} MB",
                        m_filesDone, m_filesTotal, megabytes(m_kibDone), megabytes(m_kibTotal));
}

void RestoreActivity::finish(const char* why) {
    if (m_finished)
        return;
    m_finished = true;
    DebugLog::log("[restore] configuration is back (%s) after %.1fs; starting the menu", why, m_time);
    if (m_next)
        app().requestActivity(m_next());
    else
        app().requestExit();
}

void RestoreActivity::onUpdate(float dt) {
    m_time += dt;
    pumpDaemonMessages();
    if (m_finished)
        return;

    // The file is the source of truth: a stage message can be missed (the
    // daemon only sends while this applet is active), config.json cannot.
    m_pollTimer += dt;
    if (m_pollTimer >= 0.5f) {
        m_pollTimer = 0.f;
        if (configIsBack()) {
            finish("config.json present");
            return;
        }
    }

    const float target = m_kibTotal > 0
        ? std::clamp(static_cast<float>(m_kibDone) / static_cast<float>(m_kibTotal), 0.f, 1.f)
        : 0.f;
    m_shownFraction += (target - m_shownFraction) * std::min(1.f, dt * 6.f);

    m_textTimer += dt;
    if (m_textTimer >= 0.25f) {
        m_textTimer = 0.f;
        refreshText();
    }

    // Each new progress string is a new cached texture; trim like the tutorial.
    nxui::Font* fonts[] = {&m_fontTitle, &m_fontBody, &m_fontSmall};
    bool trim = false;
    for (auto* font : fonts)
        trim = trim || font->maintenanceRequested();
    if (trim) {
        app().gpu().waitIdle();
        for (auto* font : fonts)
            font->trimCache(font->cacheEntryCount() / 2, font->cacheBytes() / 2);
        app().renderer().reclaimReleasedTextureSlotsAfterIdle();
    }
}

void RestoreActivity::onRender(nxui::Renderer& ren) {
    ren.drawRect({0.f, 0.f, 1280.f, 720.f}, m_theme.background);

    auto centered = [&](const std::string& text, nxui::Font& font, float y,
                        const nxui::Color& color) {
        const float w = font.measure(text).x;
        ren.drawText(text, {(1280.f - w) * 0.5f, y}, &font, color);
    };

    centered(m_title, m_fontTitle, 220.f, m_theme.textPrimary);
    centered(m_body, m_fontBody, 284.f, m_theme.textSecondary);

    // Progress bar; a sweeping highlight until the first numbers arrive, so
    // the screen visibly moves from the first frame.
    const nxui::Rect track{240.f, 360.f, 800.f, 14.f};
    ren.drawRoundedRect(track, m_theme.panelBase.withAlpha(0.85f), 7.f);
    if (m_haveProgress && m_kibTotal > 0) {
        const float w = std::max(14.f, track.width * m_shownFraction);
        ren.drawRoundedRect({track.x, track.y, w, track.height}, m_theme.cursorNormal, 7.f);
    } else {
        const float phase = std::fmod(m_time * 0.6f, 1.f);
        const float w = 160.f;
        const float x = track.x + (track.width - w) * phase;
        ren.drawRoundedRect({x, track.y, w, track.height}, m_theme.cursorNormal.withAlpha(0.8f), 7.f);
    }

    centered(m_detail, m_fontSmall, 392.f, m_theme.textSecondary);

    const std::string hint = nxui::I18n::instance().tr(
        "restore.hint", "This can take a few minutes. Please don't turn the console off.");
    centered(hint, m_fontSmall, 470.f, m_theme.textPrimary.withAlpha(0.85f));
}
