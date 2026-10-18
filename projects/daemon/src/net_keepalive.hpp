#pragma once
#include <switch.h>
#include <switchu/file_log.hpp>

// Keeps a persistent NIFM request open, as the stock HOME menu does.
//
// The console only holds a network connection while some process has a
// request submitted for it. qlaunch keeps one up for the whole session, so the
// stock console reconnects on its own after the network drops. This daemon
// replaces qlaunch and never submitted one: once the connection was lost with
// no game asking for the network, nothing asked to bring it back, and the
// owner had to reconnect by hand every time (reported on 2.13.0).
//
// A persistent request (IRequest::SetPersistent, cmd 12) is not dropped when
// the connection goes: switchbrew documents that it moves to OnHold and NIFM
// retries bringing the network back up. libnx creates and submits requests but
// has no wrapper for that command, so it goes through the raw IPC call.
//
// Every state change is logged with the connection status, so the next console
// log shows when the network dropped and whether it came back by itself. If
// the request ever ends up Free again (persistence refused, or an error) it is
// resubmitted with a back-off. A failure here only logs: nothing in the daemon
// depends on it.
namespace switchu::daemon::net_keepalive {

namespace detail {
inline bool g_nifmOpen = false;
inline bool g_requestOpen = false;
inline NifmRequest g_request{};
inline NifmRequestState g_lastState = NifmRequestState_Invalid;
inline s32 g_lastStatus = -2;
inline u64 g_nextPollTick = 0;
inline u64 g_nextResubmitTick = 0;
inline u32 g_resubmits = 0;

inline s32 connectionStatus(Result* rcOut) {
    NifmInternetConnectionType type{};
    u32 strength = 0;
    NifmInternetConnectionStatus status{};
    const Result rc = nifmGetInternetConnectionStatus(&type, &strength, &status);
    if (rcOut) *rcOut = rc;
    return R_SUCCEEDED(rc) ? static_cast<s32>(status) : -1;
}

inline Result setPersistent(bool persistent) {
    const u8 value = persistent ? 1 : 0;
    return serviceDispatchIn(&g_request.s, 12, value);
}
} // namespace detail

inline void start() {
    using namespace detail;
    Result rc = nifmInitialize(NifmServiceType_System);
    if (R_FAILED(rc)) {
        switchu::FileLog::log("[net] nifm:s init failed rc=0x%X; no keep-alive request", rc);
        return;
    }
    g_nifmOpen = true;
    rc = nifmCreateRequest(&g_request, true);
    if (R_FAILED(rc)) {
        switchu::FileLog::log("[net] CreateRequest failed rc=0x%X; no keep-alive request", rc);
        return;
    }
    g_requestOpen = true;
    const Result persistRc = setPersistent(true);
    const Result submitRc = nifmRequestSubmit(&g_request);
    Result statusRc = 0;
    const s32 status = connectionStatus(&statusRc);
    switchu::FileLog::log("[net] keep-alive request persistent rc=0x%X submit rc=0x%X status=%d (rc=0x%X)",
                          persistRc, submitRc, status, statusRc);
    g_nextPollTick = armGetSystemTick();
}

// Cheap: two IPC reads every two seconds, and a log line only on a change.
inline void tick() {
    using namespace detail;
    if (!g_requestOpen) return;
    const u64 now = armGetSystemTick();
    if (now < g_nextPollTick) return;
    g_nextPollTick = now + armNsToTicks(2'000'000'000ULL);

    NifmRequestState state = NifmRequestState_Invalid;
    const Result stateRc = nifmGetRequestState(&g_request, &state);
    Result statusRc = 0;
    const s32 status = connectionStatus(&statusRc);
    if (state != g_lastState || status != g_lastStatus) {
        const Result result = nifmGetResult(&g_request);
        switchu::FileLog::log("[net] request state %d->%d (rc=0x%X) result=0x%X connection %d->%d (rc=0x%X)",
                              static_cast<int>(g_lastState), static_cast<int>(state), stateRc,
                              result, g_lastStatus, status, statusRc);
        g_lastState = state;
        g_lastStatus = status;
    }

    // 1 is Free: the request is no longer asking for the network. A persistent
    // request should never land here; if it does, ask again, backing off from
    // 15 s to 2 min so a console with no reachable network is not hammered.
    if (state == NifmRequestState_Unknown1 && now >= g_nextResubmitTick) {
        const Result submitRc = nifmRequestSubmit(&g_request);
        ++g_resubmits;
        const u64 delaySec = g_resubmits < 4 ? 15ULL * g_resubmits : 120ULL;
        g_nextResubmitTick = now + armNsToTicks(delaySec * 1'000'000'000ULL);
        switchu::FileLog::log("[net] request was free; resubmitted #%u rc=0x%X next in %llus",
                              g_resubmits, submitRc, static_cast<unsigned long long>(delaySec));
    } else if (state == NifmRequestState_Available) {
        g_resubmits = 0;
    }
}

} // namespace switchu::daemon::net_keepalive
