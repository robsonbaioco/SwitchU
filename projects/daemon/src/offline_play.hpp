#pragma once
#include <switch.h>
#include <switchu/file_log.hpp>
#include <switchu/offline_titles.hpp>
#include <switchu/sd_commit.hpp>
#include <cstdio>
#include <cstring>

// Per-title "play offline": the daemon switches NIFM's wireless communication
// off before a listed title gets its process and back on when it exits or
// another title replaces it. That is the NIFM half of Airplane Mode only --
// Bluetooth and NFC stay on, so wireless controllers keep working.
//
// Only a state this module turned off is ever turned back on: if the owner
// already had Airplane Mode on, the launch changes nothing and the exit leaves
// it alone. The marker file carries that across a crash or power loss mid
// game, and the daemon restores it on its next boot.
//
// Offline is for while the title is played, not while it sits suspended under
// the menu: HOME turns the wireless back on and resuming turns it off again.
// A game left suspended for days otherwise kept the menu offline for days --
// updates, the theme shop and Send logs all failed on 2.13.2.
namespace switchu::daemon::offline_play {

inline constexpr const char* kMarkerPath = "sdmc:/config/SwitchU/offline_session.state";

// nifm:s IGeneralService, the same session the menu's Airplane Mode toggle
// opens. libnx only wraps nifm:u, which cannot write this switch.
class NifmSystemSession {
public:
    ~NifmSystemSession() {
        serviceClose(&m_general);
        serviceClose(&m_static);
    }

    Result open() {
        Result rc = smGetService(&m_static, "nifm:s");
        if (R_SUCCEEDED(rc))
            rc = serviceConvertToDomain(&m_static);
        if (R_SUCCEEDED(rc)) {
            const u64 reserved = 0;
            serviceAssumeDomain(&m_static);
            rc = serviceDispatchIn(&m_static, 5, reserved,
                .in_send_pid = true,
                .out_num_objects = 1,
                .out_objects = &m_general,
            );
        }
        return rc;
    }

    Result getEnabled(bool* out) {
        u8 value = 0;
        serviceAssumeDomain(&m_general);
        Result rc = serviceDispatchOut(&m_general, 17, value);
        if (R_SUCCEEDED(rc) && out)
            *out = (value & 1) != 0;
        return rc;
    }

    Result setEnabled(bool enabled) {
        const u8 value = enabled ? 1 : 0;
        serviceAssumeDomain(&m_general);
        return serviceDispatchIn(&m_general, 16, value);
    }

private:
    Service m_static{};
    Service m_general{};
};

inline bool markerExists() {
    FILE* f = std::fopen(kMarkerPath, "r");
    if (!f) return false;
    std::fclose(f);
    return true;
}

// The marker holds the title and, on a second line, "paused" while the title
// is suspended and the wireless was handed back.
inline bool readMarker(u64* titleId, bool* paused) {
    FILE* f = std::fopen(kMarkerPath, "r");
    if (!f) return false;
    unsigned long id = 0;
    char state[16] = {};
    const int got = std::fscanf(f, "%lx %15s", &id, state);
    std::fclose(f);
    if (titleId) *titleId = got >= 1 ? id : 0;
    if (paused) *paused = got >= 2 && std::strcmp(state, "paused") == 0;
    return true;
}

inline void writeMarker(u64 titleId, bool paused, const char* reason) {
    if (FILE* f = std::fopen(kMarkerPath, "w")) {
        std::fprintf(f, "%016lX\n%s", titleId, paused ? "paused\n" : "");
        std::fclose(f);
        switchu::commitSdCard(reason);
    }
}

// Turns the wireless back on if this module turned it off.
inline void restore(const char* reason) {
    if (!markerExists()) return;
    NifmSystemSession session;
    Result rc = session.open();
    if (R_SUCCEEDED(rc))
        rc = session.setEnabled(true);
    switchu::FileLog::log("[offline] restore (%s) rc=0x%X", reason, rc);
    if (R_SUCCEEDED(rc)) {
        std::remove(kMarkerPath);
        switchu::commitSdCard("offline restore");
    }
}

// Called right before a title gets its process. A title that is not listed
// still restores a session left by the previous one.
inline void beforeLaunch(u64 titleId) {
    if (!switchu::offline_titles::contains(titleId)) {
        restore("unlisted launch");
        return;
    }
    bool paused = false;
    if (readMarker(nullptr, &paused) && !paused) {
        switchu::FileLog::log("[offline] title=0x%016lX already offline", titleId);
        return;
    }
    NifmSystemSession session;
    bool enabled = false;
    Result rc = session.open();
    if (R_SUCCEEDED(rc))
        rc = session.getEnabled(&enabled);
    if (R_FAILED(rc) || !enabled) {
        switchu::FileLog::log("[offline] title=0x%016lX left as is: rc=0x%X wireless=%d",
                              titleId, rc, enabled ? 1 : 0);
        return;
    }
    // Marker first: a power cut between the two writes must still be undone
    // at the next boot, and restoring an untouched switch is harmless.
    writeMarker(titleId, false, "offline marker");
    rc = session.setEnabled(false);
    switchu::FileLog::log("[offline] title=0x%016lX wireless off rc=0x%X", titleId, rc);
    if (R_FAILED(rc))
        std::remove(kMarkerPath);
}

// HOME (or a power action) took the foreground from a title played offline:
// give the wireless back while it waits under the menu.
inline void onSuspend() {
    u64 titleId = 0;
    bool paused = false;
    if (!readMarker(&titleId, &paused) || paused) return;
    NifmSystemSession session;
    Result rc = session.open();
    if (R_SUCCEEDED(rc))
        rc = session.setEnabled(true);
    switchu::FileLog::log("[offline] title=0x%016lX suspended: wireless on rc=0x%X",
                          titleId, rc);
    if (R_SUCCEEDED(rc))
        writeMarker(titleId, true, "offline pause");
}

// Called right before a suspended title gets the foreground back.
inline void onResume() {
    u64 titleId = 0;
    bool paused = false;
    if (!readMarker(&titleId, &paused) || !paused) return;
    NifmSystemSession session;
    bool enabled = false;
    Result rc = session.open();
    if (R_SUCCEEDED(rc))
        rc = session.getEnabled(&enabled);
    if (R_FAILED(rc) || !enabled) {
        // The owner turned Airplane Mode on from the menu: that state is
        // theirs now, so the exit must not turn the wireless back on.
        switchu::FileLog::log("[offline] title=0x%016lX resume left as is: rc=0x%X wireless=%d",
                              titleId, rc, enabled ? 1 : 0);
        if (R_SUCCEEDED(rc)) {
            std::remove(kMarkerPath);
            switchu::commitSdCard("offline handed over");
        }
        return;
    }
    // Marker first, as in beforeLaunch.
    writeMarker(titleId, false, "offline resume");
    rc = session.setEnabled(false);
    switchu::FileLog::log("[offline] title=0x%016lX resumed: wireless off rc=0x%X", titleId, rc);
    if (R_FAILED(rc))
        writeMarker(titleId, true, "offline resume failed");
}

} // namespace switchu::daemon::offline_play
