#include "SocketRuntime.hpp"

#include <mutex>

namespace switchu::services {

namespace {

std::mutex s_mutex;
int s_refCount = 0;

} // namespace

Result acquireSocketRuntime() {
    std::lock_guard<std::mutex> lock(s_mutex);
    if (s_refCount == 0) {
        Result rc = socketInitializeDefault();
        if (R_FAILED(rc))
            return rc;
    }
    ++s_refCount;
    return 0;
}

void releaseSocketRuntime() {
    std::lock_guard<std::mutex> lock(s_mutex);
    if (s_refCount == 0)
        return;
    if (--s_refCount == 0)
        socketExit();
}

} // namespace switchu::services
