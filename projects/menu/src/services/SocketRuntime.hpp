#pragma once

#include <switch.h>

namespace switchu::services {

// libnx's socketInitialize/socketExit are not reference counted: a second
// initialize returns LibnxError_AlreadyInitialized and the first socketExit
// tears the service down for everyone. The menu has two independent users
// (the HTTP runtime and the NTP client), so both go through this counter.
// Before 2.11.0 the NTP poll only ran after the HTTP runtime was already up and
// silently piggybacked on it; once it ran first, every HTTP request failed with
// 0x00000F59 for the rest of the session.
Result acquireSocketRuntime();
void releaseSocketRuntime();

} // namespace switchu::services
