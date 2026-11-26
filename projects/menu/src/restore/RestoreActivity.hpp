#pragma once

#include <nxui/Activity.hpp>
#include <nxui/Theme.hpp>
#include <nxui/core/Font.hpp>
#include <functional>
#include <memory>
#include <string>

// Shown when the daemon is restoring config/SwitchU from /backup/SwitchU at
// boot (a CNX pack update empties /config). The configuration is not there to
// read yet, so this stands in for the menu: it says what is happening, shows
// the progress the daemon reports, and asks for the console to stay on. When
// config.json is back it hands over to whatever the configuration calls for,
// the menu or (if it was never completed) the tutorial.
//
// Before this existed the restore ran before the menu was launched at all, and
// a 1 GB backup meant 212 s of black screen -- long enough to look hung.
class RestoreActivity : public nxui::Activity {
public:
    using ActivityFactory = std::function<std::unique_ptr<nxui::Activity>()>;

    explicit RestoreActivity(ActivityFactory next);
    ~RestoreActivity() override = default;

    bool onCreate() override;
    void onUpdate(float dt) override;
    void onRender(nxui::Renderer& ren) override;
    nxui::Widget* focusRoot() override { return nullptr; }

private:
    void pumpDaemonMessages();
    void refreshText();
    void finish(const char* why);

    ActivityFactory m_next;
    nxui::Theme m_theme;
    nxui::Font m_fontTitle;
    nxui::Font m_fontBody;
    nxui::Font m_fontSmall;

    std::string m_title;
    std::string m_body;
    std::string m_detail;

    uint32_t m_filesDone = 0;
    uint32_t m_filesTotal = 0;
    uint64_t m_kibDone = 0;
    uint64_t m_kibTotal = 0;
    bool m_haveProgress = false;
    float m_shownFraction = 0.f;

    float m_time = 0.f;
    float m_pollTimer = 0.f;
    float m_textTimer = 0.f;
    bool m_finished = false;
};
