#include "LogUpload.hpp"

#include "DebugLog.hpp"
#include "themeshop/ThemeHttp.hpp"

#include <switchu/file_log.hpp>

#include <switch.h>
#include <zlib.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <functional>
#include <filesystem>
#include <list>
#include <system_error>
#include <vector>

namespace switchu::log_upload {

#if defined(SWITCHU_LOG_UPLOAD_URL) && defined(SWITCHU_LOG_UPLOAD_KEY)

namespace {

// The endpoint refuses a body over 4 MB, and base64 grows the archive by a
// third. Logs compress to a tenth or less, so this holds every archived log in
// practice; when it does not, the oldest are the ones left out.
constexpr std::size_t kMaxZipBytes = 2800u * 1024u;
constexpr std::uintmax_t kMaxLogBytes = 8u * 1024u * 1024u;

// Atmosphère's own reports of a crashed process or a fatal error. They name
// the program and the faulting address, which the logs cannot: a process that
// dies mid-frame leaves its log a few seconds short and nothing more. Only the
// newest few go, under their folder name, and they get a slice of the archive
// of their own so the logs are never pushed out by an old crash.
struct ReportDir {
    const char* path;
    const char* zipFolder;
    const char* extension;
    std::size_t maxFiles;
};
constexpr ReportDir kReportDirs[] = {
    {"sdmc:/atmosphere/crash_reports", "crash_reports", ".log", 6},
    {"sdmc:/atmosphere/fatal_reports", "fatal_reports", ".bin", 4},
};
constexpr std::uintmax_t kMaxReportBytes = 1024u * 1024u;
constexpr std::size_t kMaxReportZipBytes = 900u * 1024u;

struct ZipEntry {
    std::string name;
    std::uint32_t crc = 0;
    std::uint32_t packedSize = 0;
    std::uint32_t size = 0;
    std::uint32_t offset = 0;
};

void put16(std::string& out, std::uint16_t value) {
    out.push_back(static_cast<char>(value & 0xFF));
    out.push_back(static_cast<char>((value >> 8) & 0xFF));
}

void put32(std::string& out, std::uint32_t value) {
    put16(out, static_cast<std::uint16_t>(value & 0xFFFF));
    put16(out, static_cast<std::uint16_t>((value >> 16) & 0xFFFF));
}

// 1980-01-01, the earliest date the format can hold. The file names carry the
// real timestamps.
constexpr std::uint16_t kDosDate = 0x0021;

bool readFile(const std::string& path, std::string& out) {
    std::FILE* file = std::fopen(path.c_str(), "rb");
    if (!file)
        return false;
    out.clear();
    char buffer[16 * 1024];
    std::size_t got = 0;
    while ((got = std::fread(buffer, 1, sizeof(buffer), file)) > 0)
        out.append(buffer, got);
    const bool ok = std::ferror(file) == 0;
    std::fclose(file);
    return ok;
}

bool deflateRaw(const std::string& in, std::string& out) {
    z_stream stream{};
    if (deflateInit2(&stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -MAX_WBITS, 8,
                     Z_DEFAULT_STRATEGY) != Z_OK)
        return false;
    out.resize(deflateBound(&stream, static_cast<uLong>(in.size())));
    stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(in.data()));
    stream.avail_in = static_cast<uInt>(in.size());
    stream.next_out = reinterpret_cast<Bytef*>(out.data());
    stream.avail_out = static_cast<uInt>(out.size());
    const int rc = deflate(&stream, Z_FINISH);
    const std::size_t produced = stream.total_out;
    deflateEnd(&stream);
    if (rc != Z_STREAM_END)
        return false;
    out.resize(produced);
    return true;
}

void appendLocalEntry(std::string& zip, ZipEntry& entry, const std::string& packed) {
    entry.offset = static_cast<std::uint32_t>(zip.size());
    put32(zip, 0x04034b50);
    put16(zip, 20);
    put16(zip, 0);
    put16(zip, 8);
    put16(zip, 0);
    put16(zip, kDosDate);
    put32(zip, entry.crc);
    put32(zip, entry.packedSize);
    put32(zip, entry.size);
    put16(zip, static_cast<std::uint16_t>(entry.name.size()));
    put16(zip, 0);
    zip.append(entry.name);
    zip.append(packed);
}

void appendDirectory(std::string& zip, const std::vector<ZipEntry>& entries) {
    const std::uint32_t start = static_cast<std::uint32_t>(zip.size());
    for (const auto& entry : entries) {
        put32(zip, 0x02014b50);
        put16(zip, 20);
        put16(zip, 20);
        put16(zip, 0);
        put16(zip, 8);
        put16(zip, 0);
        put16(zip, kDosDate);
        put32(zip, entry.crc);
        put32(zip, entry.packedSize);
        put32(zip, entry.size);
        put16(zip, static_cast<std::uint16_t>(entry.name.size()));
        put16(zip, 0);
        put16(zip, 0);
        put16(zip, 0);
        put16(zip, 0);
        put32(zip, 0);
        put32(zip, entry.offset);
        zip.append(entry.name);
    }
    const std::uint32_t size = static_cast<std::uint32_t>(zip.size()) - start;
    put32(zip, 0x06054b50);
    put16(zip, 0);
    put16(zip, 0);
    put16(zip, static_cast<std::uint16_t>(entries.size()));
    put16(zip, static_cast<std::uint16_t>(entries.size()));
    put32(zip, size);
    put32(zip, start);
    put16(zip, 0);
}

std::string base64(const std::string& in) {
    static constexpr char kAlphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((in.size() + 2) / 3 * 4);
    std::size_t i = 0;
    for (; i + 2 < in.size(); i += 3) {
        const std::uint32_t chunk = (static_cast<std::uint8_t>(in[i]) << 16)
                                  | (static_cast<std::uint8_t>(in[i + 1]) << 8)
                                  | static_cast<std::uint8_t>(in[i + 2]);
        out.push_back(kAlphabet[(chunk >> 18) & 63]);
        out.push_back(kAlphabet[(chunk >> 12) & 63]);
        out.push_back(kAlphabet[(chunk >> 6) & 63]);
        out.push_back(kAlphabet[chunk & 63]);
    }
    if (i < in.size()) {
        std::uint32_t chunk = static_cast<std::uint8_t>(in[i]) << 16;
        const bool two = i + 1 < in.size();
        if (two)
            chunk |= static_cast<std::uint8_t>(in[i + 1]) << 8;
        out.push_back(kAlphabet[(chunk >> 18) & 63]);
        out.push_back(kAlphabet[(chunk >> 12) & 63]);
        out.push_back(two ? kAlphabet[(chunk >> 6) & 63] : '=');
        out.push_back('=');
    }
    return out;
}

// menu-<timestamp>.log and daemon-<timestamp>.log, newest first across both.
std::vector<std::string> archivedLogNames() {
    std::vector<std::string> names;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(FileLog::LOG_DIR, ec)) {
        if (ec)
            break;
        const std::string name = entry.path().filename().string();
        if (log_detail::has_archived_log_name(name.c_str(), "menu", FileLog::LOG_EXTENSION)
            || log_detail::has_archived_log_name(name.c_str(), "daemon", FileLog::LOG_EXTENSION))
            names.push_back(name);
    }
    const auto stamp = [](const std::string& name) { return name.substr(name.find('-') + 1); };
    std::sort(names.begin(), names.end(), [&stamp](const std::string& a, const std::string& b) {
        return stamp(a) > stamp(b);
    });
    return names;
}

// Both name their files <posix time>_<program id>, so by name is newest first.
std::vector<std::string> reportNames(const ReportDir& dir) {
    std::vector<std::string> names;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(dir.path, ec)) {
        if (ec)
            break;
        if (!entry.is_regular_file(ec) || entry.path().extension() != dir.extension)
            continue;
        names.push_back(entry.path().filename().string());
    }
    std::sort(names.begin(), names.end(), std::greater<>());
    if (names.size() > dir.maxFiles)
        names.resize(dir.maxFiles);
    return names;
}

std::string buildArchive(Outcome& outcome) {
    std::string zip;
    std::vector<ZipEntry> entries;
    std::string raw;
    std::string packed;
    // False once the archive is full, so the caller stops offering files.
    const auto add = [&](const std::string& path, const std::string& name,
                         std::uintmax_t maxBytes, std::size_t zipLimit) {
        std::error_code ec;
        const std::uintmax_t size = std::filesystem::file_size(path, ec);
        if (ec || size == 0 || size > maxBytes)
            return true;
        if (!readFile(path, raw) || !deflateRaw(raw, packed))
            return true;
        if (zip.size() + packed.size() > zipLimit)
            return false;

        ZipEntry entry;
        entry.name = name;
        entry.crc = static_cast<std::uint32_t>(
            crc32(0L, reinterpret_cast<const Bytef*>(raw.data()), static_cast<uInt>(raw.size())));
        entry.packedSize = static_cast<std::uint32_t>(packed.size());
        entry.size = static_cast<std::uint32_t>(raw.size());
        appendLocalEntry(zip, entry, packed);
        entries.push_back(std::move(entry));
        return true;
    };

    for (const auto& dir : kReportDirs) {
        for (const auto& name : reportNames(dir)) {
            if (!add(std::string(dir.path) + "/" + name,
                     std::string(dir.zipFolder) + "/" + name, kMaxReportBytes,
                     kMaxReportZipBytes))
                break;
            ++outcome.reports;
        }
    }
    // Reports alone are not worth a mail; the logs say when they happened.
    const std::size_t reportEntries = entries.size();
    for (const auto& name : archivedLogNames()) {
        if (!add(std::string(FileLog::LOG_DIR) + "/" + name, name, kMaxLogBytes, kMaxZipBytes))
            break;
    }
    if (entries.size() == reportEntries)
        return {};
    appendDirectory(zip, entries);
    outcome.files = entries.size();
    outcome.zipBytes = zip.size();
    return zip;
}

std::string firmwareVersion() {
    const u32 version = hosversionGet();
    char text[32];
    std::snprintf(text, sizeof(text), "%u.%u.%u", HOSVER_MAJOR(version), HOSVER_MINOR(version),
                  HOSVER_MICRO(version));
    return text;
}

} // namespace

bool available() {
    return SWITCHU_LOG_UPLOAD_URL[0] != '\0' && SWITCHU_LOG_UPLOAD_KEY[0] != '\0';
}

Outcome send() {
    Outcome outcome;
    try {
        // The daemon rotates its log when the message reaches it, not when it
        // is sent. Give it the moment it needs, or its newest log is missed.
        svcSleepThread(700'000'000ULL);

        std::string body = base64(buildArchive(outcome));
        if (body.empty()) {
            outcome.empty = true;
            DebugLog::log("[log-upload] no archived log to send");
            return outcome;
        }

        DebugLog::log("[log-upload] sending files=%zu reports=%zu zip=%zu body=%zu",
                      outcome.files, outcome.reports, outcome.zipBytes, body.size());
        const std::list<std::string> headers = {
            std::string("X-SwitchU-Key: ") + SWITCHU_LOG_UPLOAD_KEY,
            std::string("X-SwitchU-Version: ") + SWITCHU_VERSION,
            "X-SwitchU-Firmware: " + firmwareVersion(),
            "Content-Type: text/plain",
        };
        themeshop::http::postText(SWITCHU_LOG_UPLOAD_URL, body, headers);
        outcome.ok = true;
        DebugLog::log("[log-upload] sent");
    } catch (const std::exception& ex) {
        outcome.error = ex.what();
        DebugLog::log("[log-upload] failed: %s", ex.what());
    } catch (...) {
        outcome.error = "unknown error";
        DebugLog::log("[log-upload] failed: unknown error");
    }
    return outcome;
}

#else

bool available() { return false; }

Outcome send() {
    Outcome outcome;
    outcome.error = "log upload is not configured in this build";
    return outcome;
}

#endif

} // namespace switchu::log_upload
