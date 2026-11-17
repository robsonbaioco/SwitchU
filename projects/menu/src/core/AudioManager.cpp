#include "AudioManager.hpp"
#include <SDL2/SDL.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>

namespace {

// Let Mix_FadeInMusic settle a few frames before jumping into the stream
// (PoloNX #130).
constexpr int kResumeSeekWarmupFrames = 20;
// Shorter than this is not worth a seek.
constexpr float kMinResumeSeekSeconds = 0.25f;
// Deep Mix_SetMusicPosition into MP3 has frozen the menu on Switch (SDL_mixer /
// mpg123). Shallow resume seeks are fine; multi-minute jumps are not
// (PoloNX 2e5a6d6 keeps this ceiling with the seek off the UI thread).
constexpr float kMaxSafeMp3ResumeSeekSeconds = 90.f;
// Gain ramp once the seek lands (PoloNX restore fade-in).
constexpr float kResumeFadeInSeconds = 0.45f;

bool pathIsMp3(const std::string& path) {
    if (path.size() < 4)
        return false;
    std::string ext = path.substr(path.size() - 4);
    for (char& c : ext) {
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
    }
    return ext == ".mp3";
}

int mpegSampleRate(int version, int rateIndex) {
    static const int rates[4][4] = {
        {11025, 12000, 8000, 0},  // MPEG2.5
        {0, 0, 0, 0},
        {22050, 24000, 16000, 0}, // MPEG2
        {44100, 48000, 32000, 0}, // MPEG1
    };
    if (version < 0 || version > 3 || rateIndex < 0 || rateIndex > 3) return 0;
    return rates[version][rateIndex];
}

// Track length from the Xing/Info frame count, or 0 when the file has none.
// A bitrate x size guess is not a safe Mix_SetMusicPosition target for mpg123
// (PoloNX #130 seekReliable), so it is not attempted. Runs on the seek worker.
float mp3XingDurationSeconds(const std::string& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return 0.f;
    const long long fileSize = static_cast<long long>(f.tellg());
    f.seekg(0, std::ios::beg);
    unsigned char head[10] = {};
    f.read(reinterpret_cast<char*>(head), sizeof(head));
    if (f.gcount() < 10) return 0.f;
    long long audioStart = 0;
    if (std::memcmp(head, "ID3", 3) == 0) {
        audioStart = 10 + (((head[6] & 0x7F) << 21) | ((head[7] & 0x7F) << 14) |
                           ((head[8] & 0x7F) << 7) | (head[9] & 0x7F));
        if (head[5] & 0x10)
            audioStart += 10; // ID3v2.4 footer
    }
    if (audioStart + 4 >= fileSize) return 0.f;

    f.clear();
    f.seekg(audioStart, std::ios::beg);
    unsigned char buf[4096] = {};
    f.read(reinterpret_cast<char*>(buf), sizeof(buf));
    const int n = static_cast<int>(f.gcount());

    int frameAt = -1;
    for (int i = 0; i + 3 < n; ++i) {
        if (buf[i] == 0xFF && (buf[i + 1] & 0xE0) == 0xE0
            && ((buf[i + 1] >> 3) & 0x3) != 1 && ((buf[i + 1] >> 1) & 0x3) != 0) {
            frameAt = i;
            break;
        }
    }
    if (frameAt < 0) return 0.f;

    const int version = (buf[frameAt + 1] >> 3) & 0x3;
    const int sampleRate = mpegSampleRate(version, (buf[frameAt + 2] >> 2) & 0x3);
    if (sampleRate <= 0) return 0.f;
    const bool mpeg1 = (version == 3);
    const bool mono = ((buf[frameAt + 3] >> 6) & 0x3) == 3;
    const int sideInfo = mpeg1 ? (mono ? 17 : 32) : (mono ? 9 : 17);
    const int samplesPerFrame = mpeg1 ? 1152 : 576;

    const int xingAt = frameAt + 4 + sideInfo;
    if (xingAt + 12 >= n) return 0.f;
    const char* tag = reinterpret_cast<const char*>(buf + xingAt);
    if (std::memcmp(tag, "Xing", 4) != 0 && std::memcmp(tag, "Info", 4) != 0)
        return 0.f;
    if (!(buf[xingAt + 7] & 0x01)) // frame count present
        return 0.f;
    const unsigned int frames =
        (static_cast<unsigned int>(buf[xingAt + 8]) << 24) |
        (static_cast<unsigned int>(buf[xingAt + 9]) << 16) |
        (static_cast<unsigned int>(buf[xingAt + 10]) << 8) |
        static_cast<unsigned int>(buf[xingAt + 11]);
    if (frames == 0) return 0.f;
    return static_cast<float>(frames) * static_cast<float>(samplesPerFrame)
           / static_cast<float>(sampleRate);
}

// Where a resume may safely jump to, or 0 to keep playing from the start.
float safeResumeSeekSeconds(const std::string& path, float seconds) {
    if (!(seconds > kMinResumeSeekSeconds))
        return 0.f;
    if (!pathIsMp3(path)) {
        // OGG/WAV: a jump past the end just fails and the track plays on.
        return seconds;
    }
    if (seconds > kMaxSafeMp3ResumeSeekSeconds)
        return 0.f;
    const float duration = mp3XingDurationSeconds(path);
    if (!(duration > 1.f) || seconds >= duration - 1.f)
        return 0.f;
    return seconds;
}

} // namespace

AudioManager::~AudioManager() { shutdown(); }

bool AudioManager::initialize() {
    if (SDL_Init(SDL_INIT_AUDIO) < 0) {
        std::fprintf(stderr, "[Audio] SDL_Init(AUDIO) failed: %s\n", SDL_GetError());
        return false;
    }
    if (Mix_OpenAudio(44100, MIX_DEFAULT_FORMAT, 2, 4096) < 0) {
        std::fprintf(stderr, "[Audio] Mix_OpenAudio failed: %s\n", Mix_GetError());
        return false;
    }
    Mix_AllocateChannels(8);
    m_initialized = true;
    m_appliedMusicVolume = -1;
    setVolume(m_volume);
    return true;
}

void AudioManager::shutdown() {
    if (!m_initialized) return;

    {
        std::lock_guard<std::mutex> lk(m_trackMutex);
        resetResumeSeekLocked();
    }
    Mix_HookMusicFinished(nullptr);
    s_instance.store(nullptr);
    Mix_HaltMusic();
    Mix_HaltChannel(-1);
    Mix_CloseAudio();
    m_initialized = false;
    m_playing.store(false);
    m_paused.store(false);
    m_trackFinished.store(false);

    if (m_activeMusic) {
        Mix_FreeMusic(m_activeMusic);
        m_activeMusic = nullptr;
    }
    m_tracks.clear();
    m_shuffleOrder.clear();
    m_current = -1;
    m_shufflePos = 0;
    m_appliedMusicVolume = -1;

    for (auto& [id, chunk] : m_sfx) Mix_FreeChunk(chunk);
    m_sfx.clear();
    for (auto& [id, chunk] : m_namedSfx) Mix_FreeChunk(chunk);
    m_namedSfx.clear();
    m_namedSfxVolumeScales.clear();
}

void AudioManager::rebuildShuffleOrderLocked() {
    m_shuffleOrder.resize(m_tracks.size());
    for (size_t i = 0; i < m_tracks.size(); ++i) {
        m_shuffleOrder[i] = static_cast<int>(i);
    }
    if (m_shuffleOrder.size() > 1) {
        std::random_device rd;
        std::mt19937 g(rd());
        std::shuffle(m_shuffleOrder.begin(), m_shuffleOrder.end(), g);
    }
    m_shufflePos = 0;
}

bool AudioManager::audioContainerLooksSafe(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return false;
    unsigned char head[16] = {};
    in.read(reinterpret_cast<char*>(head), sizeof(head));
    const auto got = static_cast<size_t>(in.gcount());
    if (got < 4)
        return false;

    // ISO BMFF (MP4/M4A/AAC) mislabeled as .mp3 — Mix_PlayMusic has soft-locked
    // the Switch on these (BOTW Sheikah Tower OST shipped as ftypdash).
    if (got >= 8 && std::memcmp(head + 4, "ftyp", 4) == 0)
        return false;

    auto lowerExt = [](std::string p) {
        for (char& c : p) {
            if (c >= 'A' && c <= 'Z')
                c = static_cast<char>(c - 'A' + 'a');
        }
        const auto slash = p.find_last_of("/\\");
        const auto base = (slash == std::string::npos) ? p : p.substr(slash + 1);
        const auto dot = base.find_last_of('.');
        return (dot == std::string::npos) ? std::string{} : base.substr(dot);
    };
    const std::string ext = lowerExt(path);

    if (ext == ".mp3") {
        if (got >= 3 && std::memcmp(head, "ID3", 3) == 0)
            return true;
        // MPEG frame sync (1111 1111 111x xxxx)
        if (head[0] == 0xFF && (head[1] & 0xE0) == 0xE0)
            return true;
        return false;
    }
    if (ext == ".ogg")
        return got >= 4 && std::memcmp(head, "OggS", 4) == 0;
    if (ext == ".wav")
        return got >= 4 && std::memcmp(head, "RIFF", 4) == 0;
    // Other extensions: let Mix decide (romfs may use uncommon names).
    return true;
}

void AudioManager::loadTrack(const std::string& path, const std::string& title) {
    // Checked here, not when the track is opened: this fork decodes a track
    // only when it starts playing, and by then a bad file has already been
    // handed to Mix_LoadMUS (PoloNX 727435a).
    if (!audioContainerLooksSafe(path)) {
        std::fprintf(stderr,
                     "[Audio] Rejected unsafe/mismatched audio container: %s "
                     "(need real MP3/OGG/WAV — MP4/AAC .mp3 renames freeze playback on Switch)\n",
                     path.c_str());
        return;
    }
    std::string effTitle = title;
    if (effTitle.empty()) {
        std::filesystem::path p(path);
        effTitle = p.stem().string();
    }
    std::lock_guard<std::mutex> lk(m_trackMutex);
    m_tracks.push_back({path, effTitle});
    rebuildShuffleOrderLocked();
}

void AudioManager::clearTracks() {
    std::lock_guard<std::mutex> lk(m_trackMutex);
    resetResumeSeekLocked();
    Mix_HookMusicFinished(nullptr);
    Mix_HaltMusic();
    if (m_activeMusic) {
        Mix_FreeMusic(m_activeMusic);
        m_activeMusic = nullptr;
    }
    m_tracks.clear();
    m_shuffleOrder.clear();
    m_current = -1;
    m_shufflePos = 0;
    m_playing.store(false);
    m_paused.store(false);
    m_trackFinished.store(false);
}

std::atomic<AudioManager*> AudioManager::s_instance{nullptr};

void AudioManager::onTrackFinished() {
    AudioManager* inst = s_instance.load();
    if (inst) {
        inst->m_trackFinished.store(true);
    }
}

void AudioManager::playCurrentTrackLocked() {
    if (m_current < 0 || m_current >= static_cast<int>(m_tracks.size())) {
        m_playing.store(false);
        return;
    }

    // A new start always plays from 0; resumeTrack arms its seek after this.
    resetResumeSeekLocked();
    Mix_HookMusicFinished(nullptr);
    Mix_HaltMusic();
    if (m_activeMusic) {
        Mix_FreeMusic(m_activeMusic);
        m_activeMusic = nullptr;
    }

    const auto& track = m_tracks[m_current];
    m_activeMusic = Mix_LoadMUS(track.path.c_str());
    if (!m_activeMusic) {
        std::fprintf(stderr, "[Audio] Failed to load %s: %s\n", track.path.c_str(), Mix_GetError());
        m_playing.store(false);
        return;
    }

    s_instance.store(this);
    Mix_HookMusicFinished(onTrackFinished);
    applyMusicVolume();

    // 250ms fade-in for smooth acoustic transition
    if (Mix_FadeInMusic(m_activeMusic, 1, 250) < 0) {
        Mix_PlayMusic(m_activeMusic, 1);
    }
    m_playing.store(true);
    m_paused.store(false);
    m_trackFinished.store(false);
}

void AudioManager::play() {
    std::lock_guard<std::mutex> lk(m_trackMutex);
    if (m_tracks.empty()) return;
    // Unlock and settings paths call play() defensively; restarting a track
    // that is already playing made an audible skip (PoloNX #133).
    // A running resume seek holds the mixer lock (Mix_PlayingMusic would
    // wait for it) and means the music is already playing.
    if (m_playing.load() && (m_seekWorkerActive.load() || Mix_PlayingMusic()))
        return;
    if (m_current < 0 || m_current >= static_cast<int>(m_tracks.size())) {
        if (m_shuffle && !m_shuffleOrder.empty()) {
            m_shufflePos = 0;
            m_current = m_shuffleOrder[0];
        } else {
            m_current = 0;
        }
    }
    playCurrentTrackLocked();
}

void AudioManager::playTrack(int index) {
    std::lock_guard<std::mutex> lk(m_trackMutex);
    if (index < 0 || index >= static_cast<int>(m_tracks.size())) return;
    m_current = index;
    if (m_shuffle) {
        for (size_t i = 0; i < m_shuffleOrder.size(); ++i) {
            if (m_shuffleOrder[i] == m_current) {
                m_shufflePos = static_cast<int>(i);
                break;
            }
        }
    }
    playCurrentTrackLocked();
}

void AudioManager::pause() {
    std::lock_guard<std::mutex> lk(m_trackMutex);
    if (m_playing.load() && !m_paused.load()) {
        // Let a running resume seek land first (sleep handoff, launch); the
        // fade-in continues after resume(). A seek still in warm-up waits.
        if (m_seekWorkerActive.load()) {
            joinSeekWorker();
            m_positionSeconds = m_seekLandedSeconds;
            m_resumeFadeIn = true;
        }
        Mix_PauseMusic();
        m_paused.store(true);
    }
}

void AudioManager::resume() {
    std::lock_guard<std::mutex> lk(m_trackMutex);
    if (m_paused.load()) {
        Mix_ResumeMusic();
        m_paused.store(false);
        m_playing.store(true);
    } else if (!m_playing.load()) {
        if (!m_tracks.empty()) {
            if (m_current < 0 || m_current >= static_cast<int>(m_tracks.size())) {
                m_current = 0;
            }
            playCurrentTrackLocked();
        }
    }
}

void AudioManager::togglePlayPause() {
    if (isPaused()) {
        resume();
    } else if (isPlaying()) {
        pause();
    } else {
        play();
    }
}

void AudioManager::stop() {
    std::lock_guard<std::mutex> lk(m_trackMutex);
    resetResumeSeekLocked();
    Mix_HookMusicFinished(nullptr);
    Mix_HaltMusic();
    m_playing.store(false);
    m_paused.store(false);
    m_trackFinished.store(false);
}

void AudioManager::stopAll() {
    stop();
    Mix_HaltChannel(-1);
}

void AudioManager::nextTrack() {
    std::lock_guard<std::mutex> lk(m_trackMutex);
    if (m_tracks.empty()) return;
    if (m_tracks.size() == 1) {
        m_current = 0;
        playCurrentTrackLocked();
        return;
    }
    if (m_shuffle) {
        m_shufflePos++;
        if (m_shufflePos >= static_cast<int>(m_shuffleOrder.size())) {
            int last = m_current;
            rebuildShuffleOrderLocked();
            if (m_shuffleOrder.size() > 1 && m_shuffleOrder[0] == last) {
                std::swap(m_shuffleOrder[0], m_shuffleOrder[1]);
            }
            m_shufflePos = 0;
        }
        m_current = m_shuffleOrder[m_shufflePos];
    } else {
        m_current = (m_current + 1) % static_cast<int>(m_tracks.size());
    }
    playCurrentTrackLocked();
}

void AudioManager::previousTrack() {
    std::lock_guard<std::mutex> lk(m_trackMutex);
    if (m_tracks.empty()) return;
    if (m_tracks.size() == 1) {
        m_current = 0;
        playCurrentTrackLocked();
        return;
    }
    if (m_shuffle) {
        m_shufflePos--;
        if (m_shufflePos < 0) {
            m_shufflePos = static_cast<int>(m_shuffleOrder.size()) - 1;
        }
        m_current = m_shuffleOrder[m_shufflePos];
    } else {
        m_current = (m_current - 1 + static_cast<int>(m_tracks.size())) % static_cast<int>(m_tracks.size());
    }
    playCurrentTrackLocked();
}

void AudioManager::setShuffle(bool shuffle) {
    std::lock_guard<std::mutex> lk(m_trackMutex);
    if (m_shuffle != shuffle) {
        m_shuffle = shuffle;
        if (m_shuffle) {
            rebuildShuffleOrderLocked();
            if (m_current >= 0) {
                for (size_t i = 0; i < m_shuffleOrder.size(); ++i) {
                    if (m_shuffleOrder[i] == m_current) {
                        m_shufflePos = static_cast<int>(i);
                        break;
                    }
                }
            }
        }
    }
}

bool AudioManager::isShuffle() const {
    std::lock_guard<std::mutex> lk(m_trackMutex);
    return m_shuffle;
}

int AudioManager::currentTrackIndex() const {
    std::lock_guard<std::mutex> lk(m_trackMutex);
    return m_current;
}

size_t AudioManager::trackCount() const {
    std::lock_guard<std::mutex> lk(m_trackMutex);
    return m_tracks.size();
}

std::string AudioManager::currentTrackTitle() const {
    std::lock_guard<std::mutex> lk(m_trackMutex);
    if (m_current >= 0 && m_current < static_cast<int>(m_tracks.size())) {
        return m_tracks[m_current].title;
    }
    return {};
}

std::string AudioManager::currentTrackPath() const {
    std::lock_guard<std::mutex> lk(m_trackMutex);
    if (m_current >= 0 && m_current < static_cast<int>(m_tracks.size())) {
        return m_tracks[m_current].path;
    }
    return {};
}

std::vector<TrackInfo> AudioManager::tracks() const {
    std::lock_guard<std::mutex> lk(m_trackMutex);
    return m_tracks;
}

void AudioManager::update(float dt) {
    if (m_trackFinished.exchange(false)) {
        if (m_playing.load()) {
            nextTrack();
        }
    }

    finishSeekWorkerIfReady();
    startResumeSeekIfDue();

    std::lock_guard<std::mutex> lk(m_trackMutex);
    const bool audible = m_playing.load() && !m_paused.load();
    if (m_resumeFadeIn && audible && dt > 0.f) {
        m_resumeGain = std::min(1.f, m_resumeGain + dt / kResumeFadeInSeconds);
        if (m_resumeGain >= 0.999f) {
            m_resumeGain = 1.f;
            m_resumeFadeIn = false;
        }
        applyMusicVolume();
    }
    // Counted only while the track really advances from a known point.
    if (audible && dt > 0.f && m_pendingSeekSeconds < 0.f
        && !m_seekWorkerActive.load())
        m_positionSeconds += dt;
}

bool AudioManager::resumeTrack(const std::string& trackPath, float positionSeconds) {
    std::lock_guard<std::mutex> lk(m_trackMutex);
    if (trackPath.empty())
        return false;
    int index = -1;
    for (size_t i = 0; i < m_tracks.size(); ++i) {
        if (m_tracks[i].path == trackPath) {
            index = static_cast<int>(i);
            break;
        }
    }
    // The saved track was removed or renamed: never seek into whatever now
    // sits at its slot (PoloNX #130). The caller starts the playlist instead.
    if (index < 0)
        return false;

    m_current = index;
    if (m_shuffle) {
        for (size_t i = 0; i < m_shuffleOrder.size(); ++i) {
            if (m_shuffleOrder[i] == m_current) {
                m_shufflePos = static_cast<int>(i);
                break;
            }
        }
    }
    playCurrentTrackLocked();
    if (!m_playing.load() || !m_resumePositionEnabled
        || !(positionSeconds > kMinResumeSeekSeconds))
        return true;

    // Silent from 0 until the jump lands; update() ramps the gain back.
    m_resumeGain = 0.f;
    applyMusicVolume();
    m_pendingSeekSeconds = positionSeconds;
    m_seekWarmupFrames = kResumeSeekWarmupFrames;
    return true;
}

void AudioManager::resetResumeSeekLocked() {
    joinSeekWorker();
    m_pendingSeekSeconds = -1.f;
    m_seekWarmupFrames = 0;
    m_positionSeconds = 0.f;
    m_resumeFadeIn = false;
    if (m_resumeGain != 1.f) {
        m_resumeGain = 1.f;
        applyMusicVolume();
    }
}

void AudioManager::joinSeekWorker() {
    if (m_seekWorker.joinable())
        m_seekWorker.join();
    m_seekWorkerActive.store(false, std::memory_order_release);
    m_seekWorkerDone.store(false, std::memory_order_release);
}

void AudioManager::finishSeekWorkerIfReady() {
    if (!m_seekWorkerActive.load(std::memory_order_acquire))
        return;
    if (!m_seekWorkerDone.load(std::memory_order_acquire))
        return;
    std::lock_guard<std::mutex> lk(m_trackMutex);
    if (!m_seekWorkerActive.load())
        return; // pause/stop joined it in between
    joinSeekWorker();
    m_positionSeconds = m_seekLandedSeconds;
    m_resumeFadeIn = true;
    std::fprintf(stderr, "[Audio] Resume seek landed at %.2fs\n",
                 static_cast<double>(m_seekLandedSeconds));
}

void AudioManager::startResumeSeekIfDue() {
    std::lock_guard<std::mutex> lk(m_trackMutex);
    if (m_pendingSeekSeconds < 0.f || m_seekWorkerActive.load())
        return;
    if (!m_playing.load() || m_paused.load())
        return; // a paused track keeps its seek for resume()
    if (m_seekWarmupFrames > 0) {
        --m_seekWarmupFrames;
        return;
    }
    const float target = m_pendingSeekSeconds;
    m_pendingSeekSeconds = -1.f;
    if (!m_resumePositionEnabled || m_current < 0
        || m_current >= static_cast<int>(m_tracks.size())) {
        m_positionSeconds = 0.f;
        m_resumeFadeIn = true;
        return;
    }

    // The safety checks read the file and Mix_SetMusicPosition decodes an MP3
    // from its start, so both stay off the HOME frame (PoloNX 2e5a6d6).
    // Mix_SetMusicPosition takes the mixer lock itself; the music it seeks
    // is not halted or freed until joinSeekWorker() returns.
    const std::string path = m_tracks[m_current].path;
    m_seekLandedSeconds = 0.f;
    m_seekWorkerDone.store(false, std::memory_order_release);
    m_seekWorkerActive.store(true, std::memory_order_release);
    m_seekWorker = std::thread([this, path, target]() {
        float landed = 0.f;
        const float safe = safeResumeSeekSeconds(path, target);
        if (safe > 0.f && Mix_SetMusicPosition(static_cast<double>(safe)) == 0)
            landed = safe;
        m_seekLandedSeconds = landed;
        m_seekWorkerDone.store(true, std::memory_order_release);
    });
}

void AudioManager::applyMusicVolume() {
    // Mix_VolumeMusic waits on the mixer lock a running resume seek holds;
    // the volume lands when the seek finishes and the fade-in starts.
    if (m_seekWorkerActive.load(std::memory_order_acquire))
        return;
    const int vol = static_cast<int>(m_volume * m_musicFade * m_resumeGain * MIX_MAX_VOLUME);
    if (vol == m_appliedMusicVolume)
        return;
    m_appliedMusicVolume = vol;
    Mix_VolumeMusic(vol);
}

void AudioManager::setVolume(float vol) {
    m_volume = vol;
    applyMusicVolume();
}

void AudioManager::setMusicFade(float fade) {
    m_musicFade = std::clamp(fade, 0.f, 1.f);
    applyMusicVolume();
}

// Registers the path only. The wav decodes on the first play (issue #2
// item 8): a preset ships dozens of effects and a session touches a few.
void AudioManager::loadSfx(Sfx id, const std::string& path) {
    std::lock_guard<std::mutex> lk(m_sfxMutex);
    auto it = m_sfx.find(static_cast<int>(id));
    if (it != m_sfx.end()) {
        Mix_FreeChunk(it->second);
        m_sfx.erase(it);
    }
    m_sfxPaths[static_cast<int>(id)] = path;
}

void AudioManager::clearSfx() {
    std::lock_guard<std::mutex> lk(m_sfxMutex);
    for (auto& [id, chunk] : m_sfx) Mix_FreeChunk(chunk);
    m_sfx.clear();
    m_sfxPaths.clear();
    for (auto& [id, chunk] : m_namedSfx) Mix_FreeChunk(chunk);
    m_namedSfx.clear();
    m_namedSfxPaths.clear();
    m_namedSfxVolumeScales.clear();
}

void AudioManager::playSfx(Sfx id) {
    // Mix_PlayChannel waits on the mixer lock a resume seek holds (under a
    // second, right after HOME): drop the effect rather than stall the frame.
    if (m_seekWorkerActive.load(std::memory_order_acquire))
        return;
    std::lock_guard<std::mutex> lk(m_sfxMutex);
    auto it = m_sfx.find(static_cast<int>(id));
    if (it == m_sfx.end()) {
        auto path = m_sfxPaths.find(static_cast<int>(id));
        if (path == m_sfxPaths.end())
            return;
        Mix_Chunk* chunk = Mix_LoadWAV(path->second.c_str());
        if (!chunk) {
            std::fprintf(stderr, "[Audio] Failed to load SFX %s: %s\n",
                         path->second.c_str(), Mix_GetError());
            m_sfxPaths.erase(path);   // do not retry every press
            return;
        }
        Mix_VolumeChunk(chunk, (int)(m_sfxVolume * MIX_MAX_VOLUME));
        it = m_sfx.emplace(static_cast<int>(id), chunk).first;
    }
    if (it->second)
        Mix_PlayChannel(-1, it->second, 0);
}

void AudioManager::loadNamedSfx(const std::string& id, const std::string& path, float volumeScale) {
    Mix_Chunk* chunk = Mix_LoadWAV(path.c_str());
    if (!chunk) {
        std::fprintf(stderr, "[Audio] Failed to load named SFX %s (%s): %s\n", id.c_str(), path.c_str(), Mix_GetError());
        return;
    }
    const float scale = std::clamp(volumeScale, 0.f, 2.f);
    Mix_VolumeChunk(chunk, (int)(m_sfxVolume * scale * MIX_MAX_VOLUME));
    std::lock_guard<std::mutex> lk(m_sfxMutex);
    auto it = m_namedSfx.find(id);
    if (it != m_namedSfx.end()) {
        Mix_FreeChunk(it->second);
        it->second = chunk;
    } else {
        m_namedSfx[id] = chunk;
    }
    m_namedSfxVolumeScales[id] = scale;
}

void AudioManager::playNamedSfx(const std::string& id) {
    if (m_seekWorkerActive.load(std::memory_order_acquire))
        return; // see playSfx
    std::lock_guard<std::mutex> lk(m_sfxMutex);
    auto it = m_namedSfx.find(id);
    if (it == m_namedSfx.end()) {
        auto path = m_namedSfxPaths.find(id);
        if (path == m_namedSfxPaths.end())
            return;
        Mix_Chunk* chunk = Mix_LoadWAV(path->second.c_str());
        if (!chunk) {
            std::fprintf(stderr, "[Audio] Failed to load named SFX %s (%s): %s\n",
                         id.c_str(), path->second.c_str(), Mix_GetError());
            m_namedSfxPaths.erase(path);
            return;
        }
        float scale = 1.f;
        auto sc = m_namedSfxVolumeScales.find(id);
        if (sc != m_namedSfxVolumeScales.end()) scale = sc->second;
        Mix_VolumeChunk(chunk, (int)(m_sfxVolume * scale * MIX_MAX_VOLUME));
        it = m_namedSfx.emplace(id, chunk).first;
    }
    if (it->second)
        Mix_PlayChannel(-1, it->second, 0);
}

void AudioManager::setSfxVolume(float vol) {
    m_sfxVolume = vol;
    std::lock_guard<std::mutex> lk(m_sfxMutex);
    for (auto& [id, chunk] : m_sfx) {
        if (chunk) Mix_VolumeChunk(chunk, (int)(m_sfxVolume * MIX_MAX_VOLUME));
    }
    for (auto& [id, chunk] : m_namedSfx) {
        if (chunk) {
            float scale = 1.f;
            auto it = m_namedSfxVolumeScales.find(id);
            if (it != m_namedSfxVolumeScales.end()) scale = it->second;
            Mix_VolumeChunk(chunk, (int)(m_sfxVolume * scale * MIX_MAX_VOLUME));
        }
    }
}
