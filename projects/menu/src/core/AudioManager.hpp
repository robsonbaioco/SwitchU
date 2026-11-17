#pragma once
#include <SDL2/SDL_mixer.h>
#include <string>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <atomic>
#include <thread>

enum class Sfx {
    Navigate,
    Activate,
    PageChange,
    ModalShow,
    ModalHide,
    LaunchGame,
    ThemeToggle,
    ToggleOff,
    SliderUp,
    SliderDown,
    ConfirmPositive,
    Volume,
};

struct TrackInfo {
    std::string path;
    std::string title;
};

class AudioManager {
public:
    AudioManager() = default;
    ~AudioManager();

    bool initialize();
    void shutdown();

    void loadTrack(const std::string& path, const std::string& title = "");
    void clearTracks();
    void play();
    void playTrack(int index);
    void pause();
    void resume();
    bool isPaused() const { return m_paused.load(); }
    void togglePlayPause();
    void stop();
    /// Silence music and active sound effects without unloading audio assets.
    void stopAll();
    void nextTrack();
    void previousTrack();
    void setVolume(float vol);
    float volume() const { return m_volume; }
    bool  isPlaying() const { return m_playing; }

    void setMusicFade(float fade);
    float musicFade() const { return m_musicFade; }

    void setShuffle(bool shuffle);
    bool isShuffle() const;

    int currentTrackIndex() const;
    size_t trackCount() const;
    std::string currentTrackTitle() const;
    std::string currentTrackPath() const;
    std::vector<TrackInfo> tracks() const;

    /// dt feeds the playback clock used by mid-track resume. SDL_mixer 2.0
    /// has no Mix_GetMusicPosition, so the position is counted here.
    void update(float dt = 0.f);

    // Mid-track resume across a game launch (PoloNX #130 / 2e5a6d6). The
    // menu process exits for the title; WiiUMenuApp saves currentTrackPath()
    // and positionSeconds() with the launch and hands them back on HOME.
    void setResumePositionEnabled(bool enabled) { m_resumePositionEnabled = enabled; }
    bool resumePositionEnabled() const { return m_resumePositionEnabled; }
    float positionSeconds() const { return m_positionSeconds; }
    /// Starts the playlist entry whose path is trackPath. With resume on, the
    /// track starts muted and Mix_SetMusicPosition runs on a worker thread a
    /// few frames later (an MP3 seek decodes from the file start: 57 s cost a
    /// 436 ms frame on the UI thread upstream), then fades in. Returns false
    /// when the path is no longer in the playlist; nothing is started then.
    bool resumeTrack(const std::string& trackPath, float positionSeconds);

    void loadSfx(Sfx id, const std::string& path);
    void clearSfx();
    void playSfx(Sfx id);
    void loadNamedSfx(const std::string& id, const std::string& path, float volumeScale = 1.f);
    void playNamedSfx(const std::string& id);
    void setSfxVolume(float vol);
    float sfxVolume() const { return m_sfxVolume; }

private:
    static std::atomic<AudioManager*> s_instance;
    static void onTrackFinished();

    void applyMusicVolume();
    void rebuildShuffleOrderLocked();
    void playCurrentTrackLocked();
    /// Reject MP4/AAC (ftyp…) and other non-decodable-on-Switch containers that
    /// are often mislabeled .mp3 — Mix_PlayMusic can soft-lock the console.
    static bool audioContainerLooksSafe(const std::string& path);
    /// Drops any pending or running resume seek and restores full gain.
    /// Blocks until a running seek returns: the worker holds the mixer, and
    /// the music it is seeking must not be halted or freed under it.
    void resetResumeSeekLocked();
    void joinSeekWorker();
    void finishSeekWorkerIfReady();
    void startResumeSeekIfDue();

    mutable std::mutex m_trackMutex;
    std::vector<TrackInfo> m_tracks;
    std::vector<int> m_shuffleOrder;
    int   m_current = -1;
    int   m_shufflePos = 0;
    bool  m_shuffle = false;
    float m_volume  = 0.5f;
    float m_musicFade = 1.f;
    std::atomic<bool> m_playing{false};
    std::atomic<bool> m_paused{false};
    std::atomic<bool> m_trackFinished{false};
    bool  m_initialized = false;
    Mix_Music* m_activeMusic = nullptr;
    // Last value handed to Mix_VolumeMusic. setMusicFade runs every frame and
    // Mix_VolumeMusic takes the mixer lock, which a resume seek holds.
    int   m_appliedMusicVolume = -1;

    // Mid-track resume state. The seek worker never takes m_trackMutex.
    bool  m_resumePositionEnabled = true;
    float m_positionSeconds = 0.f;
    float m_pendingSeekSeconds = -1.f;
    int   m_seekWarmupFrames = 0;
    float m_resumeGain = 1.f;
    bool  m_resumeFadeIn = false;
    std::thread m_seekWorker;
    std::atomic<bool> m_seekWorkerActive{false};
    std::atomic<bool> m_seekWorkerDone{false};
    float m_seekLandedSeconds = 0.f; // written by the worker before Done

    std::mutex m_sfxMutex;
    std::unordered_map<int, Mix_Chunk*> m_sfx;
    std::unordered_map<std::string, Mix_Chunk*> m_namedSfx;
    std::unordered_map<std::string, float> m_namedSfxVolumeScales;
    // Effects decode on first play instead of all at start-up (issue #2
    // item 8): these keep the registered path until that moment.
    std::unordered_map<int, std::string> m_sfxPaths;
    std::unordered_map<std::string, std::string> m_namedSfxPaths;
    float m_sfxVolume = 0.7f;
};
