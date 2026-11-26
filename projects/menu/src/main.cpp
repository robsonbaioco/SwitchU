
#include "core/WiiUMenuApp.hpp"
#include "core/DebugLog.hpp"
#include "core/Config.hpp"
#include "core/NsService.hpp"
#include "tutorial/TutorialActivity.hpp"
#include "restore/RestoreActivity.hpp"
#include "services/NtpClient.hpp"
#include "services/SystemTime.hpp"
#include "usb/Mtp.hpp"
#include <nxui/Application.hpp>
#include <fmt/format.h>
#ifdef SWITCHU_MENU
#include <nxui/core/GpuDevice.hpp>
#include <nxui/core/Renderer.hpp>
#include <switchu/smi_protocol.hpp>
#include <switchu/file_log.hpp>
#endif
#include <switch.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#ifdef SWITCHU_MENU
#include <cstring>
#endif
#include <memory>

namespace {
constexpr size_t kMenuAppletHeapSize = 224u * 1024u * 1024u;
}

extern "C" {
#ifdef SWITCHU_HOMEBREW
    u32 __nx_applet_type = AppletType_Application;

    size_t __nx_heap_size = 0xD000000;
#else
    u32 __nx_applet_type = AppletType_LibraryApplet;
    u32 __nx_fs_num_sessions = 1;

    // Ignorado enquanto __libnx_initheap existir logo abaixo; fica como o
    // ultimo degrau da escada de recuo, que e o valor que sempre funcionou.
    size_t __nx_heap_size = kMenuAppletHeapSize;

    TimeServiceType __nx_time_service_type = TimeServiceType_Menu;

    void __libnx_init_time(void);

    void __nx_win_init(void);
    void __nx_win_exit(void);

    // Weak in libnx (0): __nx_win_init opens this layer instead of creating one.
    extern u64 __nx_vi_layer_id;
#endif
}

#ifndef SWITCHU_HOMEBREW
// Quanto do que o console concede o menu de fato pede.
//
// Eram 224 MB fixos, e tudo sai dali: memoria de imagem, framebuffers, fontes,
// icones, espeak, o heap do C++. O kernel concede 458 MB a este processo -- o
// [mem] do arranque mede isso -- entao havia mais de 200 MB nunca pedidos, e o
// muro de ~108 MB de imagem que derrubou o menu era este teto, nao o hardware.
//
// Em degraus, e nao um valor unico, porque o menu e um applet: a memoria vem de
// um bolso compartilhado, e ele sobe com um jogo suspenso na memoria. Pedir
// demais numa hora ruim faz svcSetHeapSize falhar, e uma falha aqui e o menu
// nao abrir. Descendo degrau a degrau, o pior caso e exatamente o que se tinha
// antes.
namespace {
// Degraus acima de 352 acrescentados depois de medir no console: o processo
// relata 457,9 MB no total e o teto anterior da escada, 352, era concedido na
// primeira tentativa -- ou seja, ela parava de pedir antes de o sistema recusar.
// Nao ha custo em tentar: svcSetHeapSize devolve erro e o degrau seguinte entra.
// Quanto o sistema concede de fato aparece no log como "heap concedido".
// O topo caiu de 416 para 384 (issue #2 item 2, mesmo corte do OmniLaunch):
// os ~32 MB que o menu deixava de pegar ficam no pool de applets para o
// sistema, e nenhum tema medido chegou perto do orcamento de imagem no teto
// antigo. O log "heap concedido" continua dizendo o que o console deu.
constexpr size_t kHeapLadder[] = {
    384u * 1024u * 1024u,
    368u * 1024u * 1024u,
    352u * 1024u * 1024u,
    320u * 1024u * 1024u,
    288u * 1024u * 1024u,
    256u * 1024u * 1024u,
    kMenuAppletHeapSize,
};

}  // namespace

// Preenchido no arranque, lido depois: nao da para registrar daqui, porque isto
// roda antes de existir heap, log ou cartao montado.
extern "C" size_t g_switchuHeapSize = 0;

extern "C" void __libnx_initheap(void) {
    extern char* fake_heap_start;
    extern char* fake_heap_end;

    void*  addr = nullptr;
    size_t size = 0;
    for (size_t want : kHeapLadder) {
        if (R_SUCCEEDED(svcSetHeapSize(&addr, want))) {
            size = want;
            break;
        }
    }

    g_switchuHeapSize = size;
    fake_heap_start = static_cast<char*>(addr);
    fake_heap_end   = static_cast<char*>(addr) + size;
}
#endif

#ifdef SWITCHU_HOMEBREW
extern "C" void userAppInit(void) {
    timeInitialize();
    setInitialize();
    setsysInitialize();
    accountInitialize(AccountServiceType_Application);
    psmInitialize();
    lblInitialize();
    romfsInit();
}

extern "C" void userAppExit(void) {
    lblExit();
    psmExit();
    accountExit();
    setsysExit();
    setExit();
    timeExit();
    romfsExit();
}
#else
namespace switchu::menu {
namespace {
u64 g_managedLayerId = 0;
}

// Sizes the menu's managed VI layer for an operation mode (PoloNX d4385bd /
// 6900c31). Docked the layer covers the 1920x1080 output and the 1280x720
// framebuffer is scaled to it (FitToLayer); in handheld it goes back to
// 1280x720 at (0,0), the size libnx leaves it at, so a dock earlier in the
// session cannot leave the LCD on a TV-sized layer. Underscan is not applied
// here: the console applies its own TV setting to every layer.
//
// operationMode < 0 asks libnx. The menu is a library applet and never
// processes AppletMessage_OperationModeChanged itself, so after boot its libnx
// cache goes stale; live changes pass the mode the daemon forwarded instead.
bool fitManagedLayerToMode(int operationMode) {
    if (g_managedLayerId == 0)
        return false;

    const int mode = operationMode >= 0
        ? operationMode
        : static_cast<int>(appletGetOperationMode());
    const bool docked = mode == AppletOperationMode_Console;
    const s32 w = docked ? 1920 : 1280;
    const s32 h = docked ? 1080 : 720;

    ViLayer layer{};
    layer.initialized = true;
    layer.layer_id = g_managedLayerId;
    const Result rc = viSetLayerSize(&layer, w, h);
    if (R_SUCCEEDED(rc))
        viSetLayerPosition(&layer, 0.f, 0.f);
    DebugLog::log("[display] mode=%d (%s) layer %llu %dx%d rc=0x%X",
                  mode, docked ? "docked" : "handheld",
                  (unsigned long long)g_managedLayerId, w, h, rc);
    return R_SUCCEEDED(rc);
}
} // namespace switchu::menu

extern "C" void __appInit(void) {
    Result rc;

    svcOutputDebugString("[SwitchU-menu] __appInit start", 30);

    rc = smInitialize();
    if (R_FAILED(rc)) diagAbortWithResult(MAKERESULT(Module_Libnx, 500));

    rc = fsInitialize();
    if (R_FAILED(rc)) diagAbortWithResult(MAKERESULT(Module_Libnx, 501));

    rc = appletInitialize();
    if (R_FAILED(rc)) {
        svcOutputDebugString("[SwitchU-menu] appletInitialize FAIL", 37);
        diagAbortWithResult(MAKERESULT(Module_Libnx, 502));
    }
    svcOutputDebugString("[SwitchU-menu] appletInitialize OK", 34);

    rc = hidInitialize();
    if (R_FAILED(rc)) svcOutputDebugString("[SwitchU-menu] hidInitialize FAIL", 33);

    // On a HOME return this can fail or answer before the clocks are set;
    // services/SystemTime recovers both later (logged once the SD log is open).
    const Result timeRc = timeInitialize();
    __libnx_init_time();
    setsysInitialize();
    setInitialize();
    lblInitialize();
    splInitialize();
    accountInitialize(AccountServiceType_System);

    {
        SetSysFirmwareVersion fw = {};
        if (R_SUCCEEDED(setsysGetFirmwareVersion(&fw)))
            hosversionSet(MAKEHOSVERSION(fw.major, fw.minor, fw.micro) | BIT(31));
    }

    rc = fsdevMountSdmc();
    if (R_FAILED(rc)) {
        svcOutputDebugString("[SwitchU-menu] fsdevMountSdmc FAIL, retry", 42);
        svcSleepThread(100'000'000ULL);
        rc = fsdevMountSdmc();
    }

    // Appended to across menu restarts, not rotated on each one: the menu
    // starts again every time a game is closed, and rotating there threw away
    // the morning to keep the afternoon. The explicit "rotate logs" action
    // still rotates, which is what it is for.
    switchu::FileLog::open("menu", switchu::FileLog::MENU_ROTATE_BYTES);
    DebugLog::openFileLog();
    DebugLog::log("[menu] __appInit complete (sd mount: 0x%X)", rc);
    DebugLog::log("[time] timeInitialize rc=0x%X", timeRc);
    switchu::services::system_time::logBootState();

    // ns is opened lazily by core/NsService (this fork), not here.

    // Single VI/layer probe before libnx's fatal BadGfxInit path.
    // Retrying viInitialize after a failure (e.g. 0xF601) was observed to panic VI
    // itself. Prefer an immediate clean exit; daemon delays at most one relaunch.
    {
        auto failGfxProbe = [](const char* step, Result rc) {
            // Sticky marker so daemon.log can correlate after a hard reboot.
            if (FILE* f = std::fopen("sdmc:/config/SwitchU/last_gfx_fail.txt", "w")) {
                std::fprintf(f, "step=%s rc=0x%X\n", step, rc);
                std::fflush(f);
                std::fclose(f);
                fsdevCommitDevice("sdmc");
            }
            switchu::FileLog::logCommit(
                "[menu] gfx probe %s fail rc=0x%X; clean exit (daemon may delay/skip relaunch)",
                step, rc);
            DebugLog::closeFileLog();
            switchu::FileLog::close();
            svcExitProcess();
        };

        Result lastRc = viInitialize(ViServiceType_Default);
        if (R_FAILED(lastRc))
            failGfxProbe("viInitialize", lastRc);

        ViDisplay display{};
        ViLayer layer{};
        bool gfxReady = false;
        bool haveDisplay = false;
        bool haveLayer = false;
        lastRc = viOpenDefaultDisplay(&display);
        if (R_FAILED(lastRc)) {
            viExit();
            failGfxProbe("viOpenDefaultDisplay", lastRc);
        }
        haveDisplay = true;
        lastRc = viCreateLayer(&display, &layer);
        if (R_SUCCEEDED(lastRc)) {
            haveLayer = true;
            gfxReady = true;
        }

        if (haveLayer)
            viCloseLayer(&layer);
        if (haveDisplay)
            viCloseDisplay(&display);
        viExit();

        if (!gfxReady) {
            // Avoid LibnxError_BadGfxInit (and the vi/omm cascade it caused).
            failGfxProbe("viCreateLayer", lastRc);
        }
    }

    // A library applet's managed layer is left at 1280x720 by libnx, so docked
    // the UI sat unscaled in the middle of the 1080p output. Open the layer
    // ourselves (its id is needed) and size it to the output of the current
    // mode: docked the 1280x720 framebuffer is then scaled to 1920x1080
    // (FitToLayer). Live dock/undock resizes it again from the daemon's
    // MenuMessage::OperationModeChanged.
    u64 managedLayerId = 0;
    if (R_SUCCEEDED(appletCreateManagedDisplayLayer(&managedLayerId)))
        __nx_vi_layer_id = managedLayerId;

    __nx_win_init();
    DebugLog::log("[menu] __nx_win_init done");

    switchu::menu::g_managedLayerId = managedLayerId;
    // The libnx mode cache is fresh here: appletInitialize just read it.
    switchu::menu::fitManagedLayerToMode(-1);

    svcOutputDebugString("[SwitchU-menu] __appInit done", 29);
}

extern "C" void __appExit(void) {
    switchu::menu::shutdownNsService();
    DebugLog::closeFileLog();
    switchu::FileLog::close();

    __nx_win_exit();

    accountExit();
    splExit();
    lblExit();

    hidExit();
    appletExit();

    setExit();
    setsysExit();
    timeExit();

    fsdevUnmountAll();
    fsExit();
    smExit();
}

static switchu::smi::MenuStartMode readStartMode() {
    LibAppletArgs args{};
    AppletStorage stor{};
    if (R_SUCCEEDED(appletPopInData(&stor))) {
        s64 sz = 0;
        appletStorageGetSize(&stor, &sz);
        if (sz >= (s64)sizeof(LibAppletArgs)) {
            appletStorageRead(&stor, 0, &args, sizeof(args));
        }
        appletStorageClose(&stor);
    }
    return static_cast<switchu::smi::MenuStartMode>(args.LaVersion);
}

static switchu::smi::SystemStatus readSystemStatus() {
    switchu::smi::SystemStatus status{};
    AppletStorage stor{};
    if (R_SUCCEEDED(appletPopInData(&stor))) {
        s64 sz = 0;
        appletStorageGetSize(&stor, &sz);
        if (sz >= (s64)sizeof(switchu::smi::SystemStatus)) {
            appletStorageRead(&stor, 0, &status, sizeof(status));
        }
        appletStorageClose(&stor);
    }
    return status;
}
#endif

int main(int argc, char* argv[]) {
    (void)argc; (void)argv;

#ifdef SWITCHU_MENU
    const uint64_t menuMainTick = armGetSystemTick();
    const uint32_t menuMainCore = svcGetCurrentProcessorNumber();
#endif

    std::srand(static_cast<unsigned>(std::time(nullptr)));

#ifdef SWITCHU_HOMEBREW
    DebugLog::openFileLog();
    DebugLog::log("[hb] main() entry");
    DebugLog::log("[main] applet config...");
#else
    DebugLog::log("[menu] main() entry");

    auto startMode = readStartMode();
    auto sysStatus = readSystemStatus();

    DebugLog::log("[menu] start mode=%d  suspended=0x%016lX running=%d",
                  (int)startMode, sysStatus.suspended_app_id, sysStatus.app_running);

    {
        std::string sdPath = fmt::format("{}/shaders/", SD_ASSETS);
        nxui::Renderer::setShaderBasePath(sdPath);
        DebugLog::log("[menu] shader path: %s", sdPath.c_str());
    }
#endif

    DebugLog::log("[main] SDL_Init");
    if (SDL_Init(SDL_INIT_AUDIO) < 0)
        DebugLog::log("[main] SDL_Init FAILED: %s", SDL_GetError());

    DebugLog::log("[main] TTF_Init");
    if (TTF_Init() < 0)
        DebugLog::log("[main] TTF_Init FAILED: %s", TTF_GetError());

    DebugLog::log("[main] creating app...");
    {
        nxui::Application app;
#ifdef SWITCHU_HOMEBREW
        AppConfig startupConfig;
        startupConfig.load();
        auto makeMenuActivity = [startupConfig](bool fromTutorial = false) -> std::unique_ptr<nxui::Activity> {
            auto activity = std::make_unique<WiiUMenuApp>();
            if (!fromTutorial)
                activity->setStartupConfig(startupConfig);
            activity->setTutorialStartupFade(fromTutorial);
            return activity;
        };
        if (startupConfig.tutorialCompleted)
            app.setActivity(makeMenuActivity(false));
        else
            app.setActivity(std::make_unique<TutorialActivity>(makeMenuActivity));
        DebugLog::log("[hb] app.initialize...");
        app.setLogSink([](const char* msg) { DebugLog::log("%s", msg); });
        nxui::GpuDevice::setDebugSink([](const char* msg) { DebugLog::log("%s", msg); });
        if (app.initialize()) {
            DebugLog::log("[hb] app.run...");
            app.run();
        } else {
            DebugLog::log("[hb] app.initialize FAILED");
        }
        DebugLog::log("[hb] app.shutdown...");
        app.shutdown();
#else
        // Mid-restore the configuration is not on the card yet: reading it now
        // would start the tutorial over a console that has one. The progress
        // screen waits for it, then makes the same choice as below.
        const bool restoring = (sysStatus.boot_flags & switchu::smi::kStatusFlagRestoring) != 0;
        auto firstActivity = [sysStatus, menuMainTick, menuMainCore]() -> std::unique_ptr<nxui::Activity> {
            AppConfig startupConfig;
            startupConfig.load();
            auto makeMenuActivity = [sysStatus, startupConfig, menuMainTick, menuMainCore](bool fromTutorial = false) -> std::unique_ptr<nxui::Activity> {
                auto activity = std::make_unique<WiiUMenuApp>();
                if (!fromTutorial)
                    activity->setStartupConfig(startupConfig);
                activity->setStartupStatus(sysStatus);
                activity->setMenuMainTrace(menuMainTick, menuMainCore);
                activity->setTutorialStartupFade(fromTutorial);
                return activity;
            };
            if (startupConfig.tutorialCompleted)
                return makeMenuActivity(false);
            return std::make_unique<TutorialActivity>(makeMenuActivity);
        };
        if (restoring) {
            DebugLog::log("[menu] daemon is restoring the configuration; progress screen first");
            app.setActivity(std::make_unique<RestoreActivity>(firstActivity));
        } else {
            app.setActivity(firstActivity());
        }
        DebugLog::log("[menu] app.initialize...");
        {
            u64 total = 0;
            u64 used = 0;
            const Result totalRc = svcGetInfo(
                &total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
            const Result usedRc = svcGetInfo(
                &used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
            DebugLog::log(
                "[mem-startup] heap_mib=%.1f process_mib=%.1f used_mib=%.1f "
                "free_mib=%.1f total_rc=0x%X used_rc=0x%X",
                g_switchuHeapSize / 1048576.0, total / 1048576.0,
                used / 1048576.0,
                total >= used ? (total - used) / 1048576.0 : 0.0,
                totalRc, usedRc);
        }
        app.setLogSink([](const char* msg) { DebugLog::log("%s", msg); });
        nxui::GpuDevice::setDebugSink([](const char* msg) { DebugLog::log("%s", msg); });
        if (app.initialize()) {
            DebugLog::log("[menu] starting USB MTP server...");
            switchu::usb::mtpStart();
            DebugLog::log("[menu] app.run...");
            app.run();
        } else {
            DebugLog::log("[menu] app.initialize FAILED");
        }
        DebugLog::log("[menu] stopping USB MTP server...");
        switchu::usb::mtpStop();
        DebugLog::log("[menu] app.shutdown...");
        app.shutdown();
        switchu::services::NtpClient::cleanup();
#endif
    }

    TTF_Quit();
    SDL_Quit();
#ifdef SWITCHU_HOMEBREW
    DebugLog::log("[hb] exit");
    DebugLog::closeFileLog();
#endif
#ifdef SWITCHU_MENU
    DebugLog::log("[menu] exit");
#endif
    return 0;
}
