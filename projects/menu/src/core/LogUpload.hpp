#pragma once

#include <cstddef>
#include <string>

// Sends the archived logs, plus the newest Atmosphère crash and fatal reports,
// to the developer's own endpoint, which mails them.
//
// The endpoint and its key are given at build time and are absent from a build
// made without them, so a fork compiled from this repository does not send its
// players' logs to somebody else's mailbox: available() is false there and the
// setting is not offered.
namespace switchu::log_upload {

struct Outcome {
    bool ok = false;
    // No archived log could be read; nothing was sent.
    bool empty = false;
    std::size_t files = 0;
    // Of those, Atmosphère crash/fatal reports.
    std::size_t reports = 0;
    std::size_t zipBytes = 0;
    std::string error;
};

bool available();

// Blocking: reads, compresses and posts. Call it off the UI thread, after the
// current logs were rotated -- the live ones are held open and cannot be read.
Outcome send();

} // namespace switchu::log_upload
