#pragma once

namespace switchu::daemon::config_backup {

// Keeps a copy of config/SwitchU in /backup/SwitchU, and puts it back when the
// configuration has disappeared.
//
// CFW packs update Atmosphere with a "clean install" option that deletes every
// folder at the root of the card except a short list, and /backup is on that
// list (the CNX Updater's RCM payload, for one, spares /backup, /emummc,
// /emutendo, /jksv, /nintendo, /retroarch and /roms). A console that went
// through one came back with SwitchU at its first-run tutorial: settings,
// folders, widgets, themes and artwork all gone.
//
// Called once at boot, before the menu is launched, so the menu either finds
// its configuration restored or leaves behind a copy of what it had. When
// config/SwitchU/settings.json is missing and the backup has one, the backup
// is restored; otherwise the backup is brought up to date with what changed.
// Caches, logs and update staging are left out: they rebuild themselves.
void restoreOrBackUp();

} // namespace switchu::daemon::config_backup
