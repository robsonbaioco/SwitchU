#pragma once

namespace switchu::daemon::update {

// Puts a staged launcher update in place, if the menu left one ready.
//
// Called once at boot, before the menu is launched: that is the only moment at
// which none of the files being replaced is open. Does nothing when there is no
// staged update, and gives up after a few failed attempts rather than delaying
// every boot.
//
// Returns true when the console must reboot before it can go on: files were
// replaced, and this daemon is one of them. The menu puts that half in place
// before the restart when it can, and leaves a marker saying so; on that path
// the daemon running here already came out of the archive and this returns
// false, which is what makes a single restart enough.
bool applyStagedUpdate();

} // namespace switchu::daemon::update
