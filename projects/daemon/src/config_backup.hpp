#pragma once
#include <cstdint>

namespace switchu::daemon::config_backup {

// Keeps a copy of config/SwitchU in /backup/SwitchU, and puts it back when the
// configuration has disappeared.
//
// Updating the CNX pack empties /config except for its own keep list, which
// does not name SwitchU; a clean install deletes every root folder except a
// fixed list, and /backup is on both (CostelaCNX/CNX-Updater, survey of
// 2026-11-26). A console that went through one came back with SwitchU at its
// first-run tutorial: settings, SteamGridDB key, folders, themes, artwork gone.
//
// Called once at boot, before the menu is launched.
// - Configuration present: the backup is brought up to date here, except the
//   downloaded artwork (steamgriddb/, game_art/), which a background thread
//   copies some seconds after the menu is up.
// - Configuration missing and a backup present: the whole restore runs on that
//   thread instead. The menu is launched at once with kStatusFlagRestoring,
//   shows the progress the daemon reports (progress()), and waits for
//   config.json, which is copied last; the artwork follows behind the menu.
//   On 2026-11-25 this ran synchronously: 1 GB in 212 s on a black screen.
// Caches, logs and per-boot state are left out: they rebuild themselves.
// shouldAbort ends the thread for good (power sequence); shouldPause holds it
// while a game has the foreground. A restore cut short resumes next boot.
void begin(bool (*shouldAbort)(), bool (*shouldPause)());

// True until config.json is back during a restore.
bool restoringEssentials();

struct Progress {
    uint32_t phase = 0;   // 0 none, 1 essentials, 2 artwork
    uint32_t filesDone = 0;
    uint32_t filesTotal = 0;
    uint64_t bytesDone = 0;
    uint64_t bytesTotal = 0;
};
Progress progress();

// Bits (1 << smi::RestoreStage) reached since the last call.
uint32_t takeStageEvents();

// Asks the thread to stop between two files and waits for it.
void stopDeferred();

} // namespace switchu::daemon::config_backup
