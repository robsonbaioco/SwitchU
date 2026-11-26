#include "config_backup.hpp"

#include <switchu/file_log.hpp>
#include <switchu/fs_remove.hpp>
#include <switchu/sd_commit.hpp>
#include <switchu/smi_protocol.hpp>
#include <switch.h>

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <utility>
#include <vector>

// Written against POSIX rather than std::filesystem for the reason given in
// switchu/fs_remove.hpp: fsdev has no openat/fdopendir, and the directory
// walkers in libstdc++ fail quietly without them.
namespace switchu::daemon::config_backup {
namespace {

constexpr const char* kConfigParent = "sdmc:/config";
constexpr const char* kConfigRoot   = "sdmc:/config/SwitchU";
constexpr const char* kBackupParent = "sdmc:/backup";
constexpr const char* kBackupRoot   = "sdmc:/backup/SwitchU";
// Its presence is what says a configuration exists. Copied last in either
// direction, so an interrupted copy is retried in full at the next boot.
constexpr const char* kConfigFile   = "config.json";
constexpr const char* kPartSuffix   = ".switchu-part";
// Written into config/SwitchU while the deferred half of a restore is still to
// do, so a restore cut short by a power-off is finished at the next boot even
// though config.json is already back.
constexpr const char* kRestorePendingFile = "backup_restore_pending";
static_assert(std::string_view(smi::kRestorePendingPath) ==
              std::string_view("sdmc:/config/SwitchU/backup_restore_pending"));

// Files up to this size are compared byte for byte, which catches an edit that
// kept the size. Larger ones -- artwork, theme media -- are compared by size.
constexpr off_t kCompareContentsBelow = 256 * 1024;

struct Stats {
    unsigned copied = 0;
    unsigned unchanged = 0;
    unsigned failed = 0;
    unsigned long long bytes = 0;
};

// Static so the copy costs nothing from the daemon's heap, which shares a pool
// with every other sysmodule on the card.
unsigned char g_bufferA[32 * 1024];
unsigned char g_bufferB[32 * 1024];

bool endsWith(const char* value, const char* suffix) {
    const std::size_t valueLength = std::strlen(value);
    const std::size_t suffixLength = std::strlen(suffix);
    return valueLength >= suffixLength
        && std::strcmp(value + valueLength - suffixLength, suffix) == 0;
}

// Rebuilt by SwitchU on its own, or only meaningful for the boot that wrote
// them: the title cache, update and uninstall staging, logs, half-written files,
// and the preview/composite caches the menu regenerates the first time it needs
// them. On 2026-11-25 a restore copied 1 GB in 212 s with the console on a
// black screen; a good part of that was these.
bool excludedDirectory(const std::string& relative, const char* name) {
    if (relative.empty()
        && (std::strcmp(name, "update") == 0
            || std::strcmp(name, "uninstall") == 0
            || std::strcmp(name, "control_cache") == 0
            || std::strcmp(name, "community_preview_cache") == 0
            || std::strcmp(name, "icon_pack_previews") == 0
            || std::strcmp(name, "folder_peeks") == 0
            || std::strcmp(name, "frame_dumps") == 0))
        return true;
    // A theme install that never finished, or the copy kept while one is
    // replaced. The menu deletes these at start (ThemePreset.cpp), so restoring
    // them only to have them removed a second later is pure cost.
    return endsWith(name, ".installing") || endsWith(name, ".installing.part")
        || endsWith(name, ".previous");
}

bool excludedFile(const std::string& relative, const char* name) {
    // leave_frame.bin (PoloNX #104) is a 3.6 MB screenshot rewritten at every
    // launch; copying it would cost that at almost every boot, for a picture
    // that is useless once the config it belongs to is gone.
    if (std::strcmp(name, "leave_frame.bin") == 0) return true;
    if (relative.empty()
        && (std::strcmp(name, kRestorePendingFile) == 0
            // Rewritten by the daemon at every boot (catalogue, sizes), or the
            // state of the boot that wrote it (guard, gfx failure marker,
            // memory probe). A restored copy would only be stale, and the
            // restore now runs while the daemon is already writing these.
            || std::strncmp(name, "applist.", 8) == 0
            || std::strcmp(name, "appsize.bin") == 0
            || std::strncmp(name, "boot_guard.state", 16) == 0
            || std::strcmp(name, "last_gfx_fail.txt") == 0
            || std::strncmp(name, "mem_inventory", 13) == 0))
        return true;
    // Decoded SteamGridDB artwork (about 900 KB each), rebuilt from the
    // downloaded image beside it in a few tens of milliseconds.
    if (endsWith(name, ".rgba-cache")) return true;
    return endsWith(name, ".log") || std::strncmp(name, "log.txt", 7) == 0
        || endsWith(name, ".tmp") || endsWith(name, ".part")
        || endsWith(name, kPartSuffix);
}

// Downloaded artwork: the bulk of the copy, and nothing the menu needs before
// its first frame -- it draws the grid's own icons and fetches art lazily. These
// are left to a background thread started once the menu is up.
bool deferredDirectory(const std::string& relative, const char* name) {
    return relative.empty()
        && (std::strcmp(name, "steamgriddb") == 0 || std::strcmp(name, "game_art") == 0);
}

bool fileSize(const std::string& path, off_t& size) {
    struct stat info {};
    if (stat(path.c_str(), &info) != 0 || !S_ISREG(info.st_mode))
        return false;
    size = info.st_size;
    return true;
}

bool ensureDirectory(const std::string& path) {
    if (mkdir(path.c_str(), 0777) == 0 || errno == EEXIST)
        return true;
    struct stat info {};
    return stat(path.c_str(), &info) == 0 && S_ISDIR(info.st_mode);
}

bool sameContents(const std::string& a, const std::string& b) {
    std::FILE* fa = std::fopen(a.c_str(), "rb");
    std::FILE* fb = fa ? std::fopen(b.c_str(), "rb") : nullptr;
    bool same = fa && fb;
    while (same) {
        const std::size_t ra = std::fread(g_bufferA, 1, sizeof(g_bufferA), fa);
        const std::size_t rb = std::fread(g_bufferB, 1, sizeof(g_bufferB), fb);
        if (ra != rb || std::memcmp(g_bufferA, g_bufferB, ra) != 0)
            same = false;
        else if (ra == 0)
            break;
    }
    if (fa) std::fclose(fa);
    if (fb) std::fclose(fb);
    return same;
}

bool upToDate(const std::string& source, off_t sourceSize, const std::string& destination) {
    off_t destinationSize = 0;
    if (!fileSize(destination, destinationSize) || destinationSize != sourceSize)
        return false;
    return sourceSize > kCompareContentsBelow || sameContents(source, destination);
}

// Written beside the target and renamed over it, so a copy cut short by a
// power loss leaves the previous version rather than half of the new one.
bool copyFile(const std::string& source, const std::string& destination, Stats& stats) {
    const std::string part = destination + kPartSuffix;
    std::FILE* in = std::fopen(source.c_str(), "rb");
    std::FILE* out = in ? std::fopen(part.c_str(), "wb") : nullptr;
    bool ok = in && out;
    unsigned long long written = 0;
    while (ok) {
        const std::size_t read = std::fread(g_bufferA, 1, sizeof(g_bufferA), in);
        if (read == 0) {
            ok = !std::ferror(in);
            break;
        }
        if (std::fwrite(g_bufferA, 1, read, out) != read)
            ok = false;
        written += read;
    }
    if (in) std::fclose(in);
    if (out && std::fclose(out) != 0) ok = false;

    if (ok) {
        std::remove(destination.c_str());
        ok = std::rename(part.c_str(), destination.c_str()) == 0;
    }
    if (!ok) {
        std::remove(part.c_str());
        ++stats.failed;
        switchu::FileLog::log("[backup] copy failed: %s -> %s errno=%d",
                              source.c_str(), destination.c_str(), errno);
        return false;
    }
    ++stats.copied;
    stats.bytes += written;
    return true;
}

// Which part of the tree one pass covers. The split is only at the top level:
// the immediate pass takes the files there and every folder but the deferred
// ones, the deferred pass takes only those.
enum class Scope { Immediate, Deferred };

// Set for the background pass; checked between files so a power-off is never
// raced by a copy in flight, and a game in front is never competing with it
// for the card.
bool (*g_shouldAbort)() = nullptr;
bool (*g_shouldPause)() = nullptr;
std::atomic<bool> g_stopRequested{false};

// What a restore has done so far, read by the main loop to tell the menu.
std::atomic<uint32_t> g_phase{0};
std::atomic<uint32_t> g_filesDone{0};
std::atomic<uint32_t> g_filesTotal{0};
std::atomic<uint64_t> g_bytesDone{0};
std::atomic<uint64_t> g_bytesTotal{0};
std::atomic<bool> g_restoringEssentials{false};
std::atomic<uint32_t> g_stageEvents{0};
bool g_reportProgress = false;

bool aborted() {
    if (g_stopRequested.load()) return true;
    if (g_shouldAbort && g_shouldAbort()) return true;
    while (g_shouldPause && g_shouldPause()) {
        if (g_stopRequested.load() || (g_shouldAbort && g_shouldAbort()))
            return true;
        svcSleepThread(500'000'000ULL);
    }
    return false;
}

void countDone(off_t size) {
    if (!g_reportProgress) return;
    g_filesDone.fetch_add(1);
    g_bytesDone.fetch_add(static_cast<uint64_t>(size));
}

// The same walk as mirror(), only adding up what it will look at, so the
// progress screen has a total to count towards.
void count(const std::string& sourceDir, const std::string& relative, Scope scope,
           uint32_t& files, uint64_t& bytes) {
    DIR* dir = opendir(sourceDir.c_str());
    if (!dir)
        return;
    while (const dirent* entry = readdir(dir)) {
        const char* name = entry->d_name;
        if (std::strcmp(name, ".") == 0 || std::strcmp(name, "..") == 0)
            continue;
        const std::string source = sourceDir + "/" + name;
        struct stat info {};
        if (stat(source.c_str(), &info) != 0)
            continue;
        if (S_ISDIR(info.st_mode)) {
            if (excludedDirectory(relative, name))
                continue;
            if (relative.empty() && (scope == Scope::Deferred) != deferredDirectory(relative, name))
                continue;
            count(source, relative + name + "/", scope, files, bytes);
            continue;
        }
        if (relative.empty() && scope == Scope::Deferred)
            continue;
        if (!S_ISREG(info.st_mode) || excludedFile(relative, name))
            continue;
        ++files;
        bytes += static_cast<uint64_t>(info.st_size);
    }
    closedir(dir);
}

// Copies what changed from sourceDir into destinationDir. Nothing is ever
// deleted on the destination side: a file removed from the configuration
// stays in the backup, which is the safe way round for a copy whose job is
// to outlive a wipe. Excluded leftovers are the one exception; see prune().
void mirror(const std::string& sourceDir, const std::string& destinationDir,
            const std::string& relative, Scope scope, Stats& stats) {
    if (aborted()) {
        ++stats.failed;
        return;
    }
    if (!ensureDirectory(destinationDir)) {
        ++stats.failed;
        switchu::FileLog::log("[backup] cannot create %s errno=%d",
                              destinationDir.c_str(), errno);
        return;
    }
    DIR* dir = opendir(sourceDir.c_str());
    if (!dir) {
        ++stats.failed;
        switchu::FileLog::log("[backup] cannot open %s errno=%d", sourceDir.c_str(), errno);
        return;
    }
    while (const dirent* entry = readdir(dir)) {
        const char* name = entry->d_name;
        if (std::strcmp(name, ".") == 0 || std::strcmp(name, "..") == 0)
            continue;
        if (aborted()) {
            ++stats.failed;
            break;
        }
        const std::string source = sourceDir + "/" + name;
        const std::string destination = destinationDir + "/" + name;
        struct stat info {};
        if (stat(source.c_str(), &info) != 0)
            continue;
        if (S_ISDIR(info.st_mode)) {
            if (excludedDirectory(relative, name))
                continue;
            if (!relative.empty()) {
                mirror(source, destination, relative + name + "/", scope, stats);
                continue;
            }
            if ((scope == Scope::Deferred) != deferredDirectory(relative, name))
                continue;
            // Per top-level folder, so the next slow copy says where the time
            // went instead of only how much there was.
            const u64 dirStart = armGetSystemTick();
            const unsigned long long bytesBefore = stats.bytes;
            const unsigned copiedBefore = stats.copied;
            mirror(source, destination, std::string(name) + "/", scope, stats);
            const u64 dirMs = armTicksToNs(armGetSystemTick() - dirStart) / 1'000'000ULL;
            if (dirMs >= 500)
                switchu::FileLog::log("[backup]   %s: copied=%u (%llu bytes) in %llu ms",
                                      name, stats.copied - copiedBefore,
                                      stats.bytes - bytesBefore,
                                      static_cast<unsigned long long>(dirMs));
            continue;
        }
        // Files at the top level belong to the immediate pass.
        if (relative.empty() && scope == Scope::Deferred)
            continue;
        if (!S_ISREG(info.st_mode) || excludedFile(relative, name))
            continue;
        if (relative.empty() && std::strcmp(name, kConfigFile) == 0)
            continue;   // the caller copies it last
        if (upToDate(source, info.st_size, destination))
            ++stats.unchanged;
        else
            copyFile(source, destination, stats);
        countDone(info.st_size);
    }
    closedir(dir);
}

// The backup only ever grew, so caches and half-finished theme installs that
// were copied before they were excluded stayed there and came back with every
// restore. Removes those from the backup, and nothing else.
void prune(const std::string& dirPath, const std::string& relative, unsigned& removed) {
    DIR* dir = opendir(dirPath.c_str());
    if (!dir)
        return;
    std::vector<std::string> doomed;
    std::vector<std::pair<std::string, std::string>> children;
    while (const dirent* entry = readdir(dir)) {
        const char* name = entry->d_name;
        if (std::strcmp(name, ".") == 0 || std::strcmp(name, "..") == 0)
            continue;
        const std::string path = dirPath + "/" + name;
        struct stat info {};
        if (stat(path.c_str(), &info) != 0)
            continue;
        if (S_ISDIR(info.st_mode)) {
            if (excludedDirectory(relative, name))
                doomed.push_back(path);
            else
                children.emplace_back(path, relative + name + "/");
        } else if (S_ISREG(info.st_mode) && excludedFile(relative, name)) {
            doomed.push_back(path);
        }
    }
    closedir(dir);
    for (const std::string& path : doomed) {
        if (aborted()) return;
        if (switchu::removeRecursive(path))
            ++removed;
    }
    for (const auto& child : children) {
        if (aborted()) return;
        prune(child.first, child.second, removed);
    }
}

void copyConfigFileLast(const std::string& sourceRoot, const std::string& destinationRoot,
                        Stats& stats) {
    const std::string source = sourceRoot + "/" + kConfigFile;
    const std::string destination = destinationRoot + "/" + kConfigFile;
    off_t size = 0;
    if (!fileSize(source, size))
        return;
    if (upToDate(source, size, destination))
        ++stats.unchanged;
    else
        copyFile(source, destination, stats);
    countDone(size);
}

bool exists(const std::string& path) {
    struct stat info {};
    return stat(path.c_str(), &info) == 0;
}

std::string pendingMarkerPath() {
    return std::string(kConfigRoot) + "/" + kRestorePendingFile;
}

bool writePendingMarker() {
    std::FILE* f = std::fopen(pendingMarkerPath().c_str(), "wb");
    if (!f)
        return false;
    std::fputs(kBackupRoot, f);
    return std::fclose(f) == 0;
}

void logResult(const char* action, const Stats& stats, u64 start) {
    const u64 elapsedMs = armTicksToNs(armGetSystemTick() - start) / 1'000'000ULL;
    switchu::FileLog::log("[backup] %s: copied=%u (%llu bytes) unchanged=%u failed=%u in %llu ms",
                          action, stats.copied, stats.bytes, stats.unchanged, stats.failed,
                          static_cast<unsigned long long>(elapsedMs));
}

void startPhase(uint32_t phase, Scope scope) {
    uint32_t files = 0;
    uint64_t bytes = 0;
    count(kBackupRoot, "", scope, files, bytes);
    if (scope == Scope::Immediate)
        ++files;   // config.json, copied last
    g_filesDone.store(0);
    g_bytesDone.store(0);
    g_filesTotal.store(files);
    g_bytesTotal.store(bytes);
    g_phase.store(phase);
    switchu::FileLog::log("[backup] restore phase %u: %u files, %llu bytes",
                          phase, files, static_cast<unsigned long long>(bytes));
}

// What the background thread does, decided at boot before the menu starts.
enum class Job { None, RestoreAll, RestoreArtwork, BackUpArtwork };
Job g_job = Job::None;

Thread g_thread = {};
bool g_threadStarted = false;
// Static for the same reason as the copy buffers: nothing from the daemon heap.
alignas(0x1000) unsigned char g_threadStack[0x8000];

// Everything but the artwork, while the menu waits on its progress screen.
void restoreEssentials() {
    const u64 start = armGetSystemTick();
    Stats stats;
    startPhase(1, Scope::Immediate);
    mirror(kBackupRoot, kConfigRoot, "", Scope::Immediate, stats);
    if (aborted()) {
        // Powering off: config.json stays missing, so the next boot starts over.
        logResult("restore interrupted; retried next boot", stats, start);
        return;
    }
    // config.json goes back even when some files failed. Holding it back to
    // retry did not work out: the menu then starts from scratch, the tutorial
    // writes a fresh config.json, and the next boot backs that up over the
    // real one. The marker goes in first: once config.json exists this card no
    // longer looks wiped, and only the marker says the artwork is still owed.
    writePendingMarker();
    copyConfigFileLast(kBackupRoot, kConfigRoot, stats);
    logResult(stats.failed ? "restored with failures" : "restored", stats, start);
    if (stats.copied > 0)
        switchu::commitSdCard("config restore");
    g_restoringEssentials.store(false);
    g_stageEvents.fetch_or(1u << static_cast<uint32_t>(
        stats.failed ? smi::RestoreStage::Failed : smi::RestoreStage::EssentialsDone));
}

void restoreArtwork() {
    const u64 start = armGetSystemTick();
    Stats stats;
    startPhase(2, Scope::Deferred);
    mirror(kBackupRoot, kConfigRoot, "", Scope::Deferred, stats);
    if (stats.failed == 0) {
        std::remove(pendingMarkerPath().c_str());
        logResult("restored artwork (background)", stats, start);
    } else {
        logResult("artwork restore interrupted; resumed next boot", stats, start);
    }
    if (stats.copied > 0 && !aborted())
        switchu::commitSdCard("artwork restore");
    g_phase.store(0);
    g_stageEvents.fetch_or(1u << static_cast<uint32_t>(smi::RestoreStage::ArtworkDone));
}

void backUpArtwork() {
    // Well behind the menu's own start-up reads: nothing here is urgent.
    for (int i = 0; i < 40; ++i) {
        if (aborted()) return;
        svcSleepThread(500'000'000ULL);
    }
    const u64 start = armGetSystemTick();
    Stats stats;
    mirror(kConfigRoot, kBackupRoot, "", Scope::Deferred, stats);
    unsigned removed = 0;
    if (!aborted())
        prune(kBackupRoot, "", removed);
    logResult("backed up artwork (background)", stats, start);
    if (removed > 0)
        switchu::FileLog::log("[backup] pruned %u cache/leftover entries from %s",
                              removed, kBackupRoot);
    if (stats.copied > 0 && !aborted())
        switchu::commitSdCard("artwork backup");
}

void backgroundThread(void*) {
    switch (g_job) {
    case Job::RestoreAll:
        restoreEssentials();
        if (!aborted())
            restoreArtwork();
        break;
    case Job::RestoreArtwork:
        restoreArtwork();
        break;
    case Job::BackUpArtwork:
        backUpArtwork();
        break;
    case Job::None:
        break;
    }
}

bool startThread() {
    Result rc = threadCreate(&g_thread, backgroundThread, nullptr, g_threadStack,
                             sizeof(g_threadStack), 0x3B, 3);
    if (R_SUCCEEDED(rc)) {
        rc = threadStart(&g_thread);
        if (R_FAILED(rc))
            threadClose(&g_thread);
    }
    if (R_FAILED(rc)) {
        switchu::FileLog::log("[backup] background thread failed rc=0x%X", rc);
        return false;
    }
    g_threadStarted = true;
    return true;
}

} // namespace

void begin(bool (*shouldAbort)(), bool (*shouldPause)()) {
    g_shouldAbort = shouldAbort;
    g_shouldPause = shouldPause;
    g_stopRequested.store(false);

    const u64 start = armGetSystemTick();
    const std::string configFile = std::string(kConfigRoot) + "/" + kConfigFile;
    const std::string backupFile = std::string(kBackupRoot) + "/" + kConfigFile;
    const bool haveConfig = exists(configFile);
    const bool haveBackup = exists(backupFile);
    const bool restorePending = haveConfig && exists(pendingMarkerPath());

    if (!haveConfig && haveBackup) {
        // The whole restore runs behind the menu, which shows its progress and
        // waits for config.json. On 2026-11-25 it ran here instead, 212 s on a
        // black screen that looked like a hung console.
        switchu::FileLog::log("[backup] configuration missing; restoring from %s behind the progress screen",
                              kBackupRoot);
        ensureDirectory(kConfigParent);
        ensureDirectory(kConfigRoot);
        g_job = Job::RestoreAll;
        g_reportProgress = true;
        g_restoringEssentials.store(true);
        if (!startThread()) {
            // No thread: do the essentials here, as before, rather than not at all.
            restoreEssentials();
            g_restoringEssentials.store(false);
        }
        return;
    }
    if (!haveConfig) {
        switchu::FileLog::log("[backup] no configuration and no backup yet");
        return;
    }

    Stats stats;
    ensureDirectory(kBackupParent);
    mirror(kConfigRoot, kBackupRoot, "", Scope::Immediate, stats);
    if (stats.failed == 0)
        copyConfigFileLast(kConfigRoot, kBackupRoot, stats);
    logResult("backed up", stats, start);
    if (stats.copied > 0)
        switchu::commitSdCard("config backup");

    // Artwork still owed by an interrupted restore comes back first; backing
    // up half-restored folders would gain nothing.
    if (restorePending && haveBackup) {
        switchu::FileLog::log("[backup] artwork restore unfinished last boot; resuming behind the menu");
        g_job = Job::RestoreArtwork;
        g_reportProgress = true;
    } else {
        if (restorePending)
            std::remove(pendingMarkerPath().c_str());
        g_job = Job::BackUpArtwork;
    }
    startThread();
}

bool restoringEssentials() {
    return g_restoringEssentials.load();
}

Progress progress() {
    Progress p;
    p.phase = g_phase.load();
    p.filesDone = g_filesDone.load();
    p.filesTotal = g_filesTotal.load();
    p.bytesDone = g_bytesDone.load();
    p.bytesTotal = g_bytesTotal.load();
    return p;
}

uint32_t takeStageEvents() {
    return g_stageEvents.exchange(0);
}

void stopDeferred() {
    if (!g_threadStarted)
        return;
    g_stopRequested.store(true);
    threadWaitForExit(&g_thread);
    threadClose(&g_thread);
    g_threadStarted = false;
}

} // namespace switchu::daemon::config_backup
