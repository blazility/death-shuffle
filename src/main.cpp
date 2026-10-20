// Death Shuffle: every death switches the level music to a random song
// you downloaded with Jukebox (fleym.nongd) for the level you're playing.
//
// Jukebox has no public C++ API for other mods, so this reads the song list
// Jukebox saves to disk (<Jukebox save dir>/manifest/<songID>.json). It only
// reads those files and never changes them.

#include <Geode/Geode.hpp>
#include <Geode/modify/FMODAudioEngine.hpp>
#include <Geode/modify/PlayLayer.hpp>

#include <algorithm>
#include <filesystem>
#include <optional>
#include <random>
#include <unordered_set>
#include <vector>

using namespace geode::prelude;

namespace {

struct ShuffleSong {
    std::filesystem::path path;
    std::string name;
    std::string artist;
    int offset = 0;  // the start offset set for this song in Jukebox
};

std::vector<ShuffleSong> g_pool;
std::optional<ShuffleSong> g_current;  // song currently replacing the level music
bool g_startPending = false;           // a new song was rolled and still needs to start
bool g_restarting = false;             // GD is (re)starting the level music right now
bool g_ownSeek = false;                // we're seeking the music ourselves
Ref<Notification> g_notification;      // the "Now playing" popup, if one is up
std::mt19937 g_rng{std::random_device{}()};

constexpr size_t MAX_POPUP_CHARS = 40;

bool setting(std::string_view key) {
    return Mod::get()->getSettingValue<bool>(key);
}

void addSong(matjson::Value const& song, std::unordered_set<std::string>& seen) {
    if (!song.isObject() || !song["path"].isString()) return;

    auto pathRes = song["path"].as<std::filesystem::path>();
    if (pathRes.isErr()) return;
    std::filesystem::path path = pathRes.unwrap();

    // YouTube/hosted songs that were never downloaded have no file yet.
    std::error_code ec;
    if (path.empty() || !std::filesystem::is_regular_file(path, ec)) return;

    std::string key = utils::string::pathToString(path);
    if (!seen.insert(key).second) return;  // same file listed twice

    ShuffleSong s;
    s.path = std::move(path);
    s.name = song["name"].asString().unwrapOr("Unknown song");
    s.artist = song["artist"].asString().unwrapOr("");
    s.offset = static_cast<int>(song["offset"].asInt().unwrapOr(0));
    g_pool.push_back(std::move(s));
}

// The IDs Jukebox uses to name its manifest files (<id>.json) for every song
// this level uses. Jukebox stores RobTop's official songs as -(track + 1)
// so they don't clash with Newgrounds IDs.
std::vector<int> levelSongIDs(GJGameLevel* level) {
    std::vector<int> ids;
    if (!level) return ids;

    if (level->m_songID != 0) {
        ids.push_back(level->m_songID);
    } else {
        ids.push_back(-level->m_audioTrack - 1);
    }

    // Extra songs used by song triggers in the level, comma separated.
    for (auto const& part : utils::string::split(std::string(level->m_songIDs), ",")) {
        auto id = utils::numFromString<int>(part);
        if (id.isOk() && id.unwrap() > 0) ids.push_back(id.unwrap());
    }
    return ids;
}

void loadSongPool(GJGameLevel* level) {
    g_pool.clear();

    Mod* jukebox = Loader::get()->getLoadedMod("fleym.nongd");
    if (!jukebox) {
        log::warn("Jukebox is not loaded, nothing to shuffle");
        return;
    }

    std::filesystem::path manifestDir = jukebox->getSaveDir() / "manifest";
    bool includeDefaults = setting("include-default-songs");
    std::unordered_set<std::string> seen;

    for (int id : levelSongIDs(level)) {
        std::filesystem::path file = manifestDir / fmt::format("{}.json", id);
        std::error_code ec;
        if (!std::filesystem::is_regular_file(file, ec)) continue;  // no NONGs for this song

        auto jsonRes = utils::file::readJson(file);
        if (jsonRes.isErr()) {
            log::warn("Skipping {}: {}", file.filename(), jsonRes.unwrapErr());
            continue;
        }
        matjson::Value json = jsonRes.unwrap();

        for (char const* list : {"locals", "youtube", "hosted"}) {
            if (!json[list].isArray()) continue;
            for (auto const& song : json[list]) addSong(song, seen);
        }
        if (includeDefaults) addSong(json["default"], seen);
    }

    log::info("Death Shuffle found {} downloaded songs for this level", g_pool.size());
}

void rollSong() {
    if (g_pool.empty()) return;

    std::uniform_int_distribution<size_t> dist(0, g_pool.size() - 1);
    size_t index = dist(g_rng);

    // Avoid getting the same song twice in a row when there's a choice.
    if (g_current && g_pool.size() > 1) {
        while (g_pool[index].path == g_current->path) index = dist(g_rng);
    }

    g_current = g_pool[index];
    g_startPending = true;
}

bool activeIn(PlayLayer* layer) {
    if (!layer || !setting("enabled")) return false;
    if (layer->m_isPracticeMode && !setting("in-practice")) return false;
    return true;
}

// ---- Popup ----

// Shorten text to fit the popup without cutting a UTF-8 character in half.
std::string shorten(std::string const& text, size_t maxChars) {
    size_t chars = 0;
    for (size_t i = 0; i < text.size(); ++i) {
        if ((static_cast<unsigned char>(text[i]) & 0xC0) == 0x80) continue;  // continuation byte
        if (chars == maxChars - 3) {
            // Only cut if there's actually more than fits.
            size_t rest = 0;
            for (size_t j = i; j < text.size(); ++j) {
                if ((static_cast<unsigned char>(text[j]) & 0xC0) != 0x80) ++rest;
            }
            if (rest > 3) return text.substr(0, i) + "...";
            return text;
        }
        ++chars;
    }
    return text;
}

void hidePopup() {
    if (g_notification && g_notification->isShowing()) g_notification->hide();
    g_notification = nullptr;
}

void announce(ShuffleSong const& song) {
    if (!setting("show-song-name")) return;
    hidePopup();

    std::string text = song.artist.empty() ? song.name : fmt::format("{} - {}", song.name, song.artist);
    g_notification = Notification::create(shorten(text, MAX_POPUP_CHARS), NotificationIcon::Info, 2.f);
    g_notification->show();
}

// ---- Timing ----

// Jukebox adds the start offset of the level's own NONG to every music start
// and seek while a level is open. That offset belongs to the level's song, not
// to ours, so switch it off before we start or seek a shuffled song. Asking for
// the path of song 0 (which never has NONGs) makes Jukebox clear it.
void stopJukeboxOffset() {
    (void)MusicDownloadManager::sharedState()->pathForSong(0);
}

// How far into the level the player's spawn point is (start pos or checkpoint).
int spawnElapsedMs(PlayLayer* layer) {
    if (!layer || !layer->m_player1) return 0;
    float seconds = layer->timeForPos(layer->m_player1->getPosition(), 0, 0, false, 0);
    return std::max(0, static_cast<int>(seconds * 1000.f));
}

// The level's own "song offset" (levels that start partway through their song).
int levelSongOffsetMs(PlayLayer* layer) {
    if (!layer || !layer->m_levelSettings) return 0;
    return std::max(0, static_cast<int>(layer->m_levelSettings->m_songOffset * 1000.f));
}

int shuffledStartMs(ShuffleSong const& song, PlayLayer* layer) {
    int start = 0;
    if (setting("use-song-offset")) start += song.offset;
    if (setting("use-level-offset")) start += levelSongOffsetMs(layer);
    if (setting("sync-start-pos")) start += spawnElapsedMs(layer);
    return std::max(0, start);
}

// Fallback for when GD doesn't start the music through queueStartMusic.
void startCurrentSong(PlayLayer* layer) {
    if (!g_current) return;
    g_startPending = false;

    auto engine = FMODAudioEngine::get();
    stopJukeboxOffset();
    engine->playMusic(utils::string::pathToString(g_current->path), true, 0.f, 0);

    stopJukeboxOffset();
    g_ownSeek = true;
    engine->setMusicTimeMS(shuffledStartMs(*g_current, layer), true, 0);
    g_ownSeek = false;

    announce(*g_current);
}

}  // namespace

class $modify(DeathShufflePlayLayer, PlayLayer) {
    struct Fields {
        bool rolledThisDeath = false;
    };

    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        g_current.reset();
        g_startPending = false;
        g_restarting = false;
        hidePopup();
        loadSongPool(level);

        if (setting("enabled") && setting("shuffle-first-attempt")) rollSong();

        return PlayLayer::init(level, useReplay, dontCreateObjects);
    }

    // The level actually begins (first attempt).
    void startGame() {
        PlayLayer::startGame();
        if (g_startPending && activeIn(this)) this->startMusic();
    }

    void startMusic() {
        g_restarting = true;
        PlayLayer::startMusic();
        g_restarting = false;

        // GD didn't go through queueStartMusic, so start our song directly.
        if (g_startPending && activeIn(this)) startCurrentSong(this);
    }

    void destroyPlayer(PlayerObject* player, GameObject* object) {
        PlayLayer::destroyPlayer(player, object);

        // GD "kills" the player once at level start as an anticheat check.
        if (object == m_anticheatSpike) return;
        // Noclip and similar don't actually kill you.
        if (!m_playerDied && !(player && player->m_isDead)) return;
        // In dual mode both players can die on the same frame.
        if (m_fields->rolledThisDeath) return;

        m_fields->rolledThisDeath = true;
        hidePopup();
        if (activeIn(this)) rollSong();
    }

    void resetLevel() {
        m_fields->rolledThisDeath = false;
        g_restarting = true;
        PlayLayer::resetLevel();
        g_restarting = false;

        // Let GD restart the music so our hooks swap in the new song.
        if (g_startPending && activeIn(this)) this->startMusic();
    }

    void onQuit() {
        hidePopup();
        g_current.reset();
        g_startPending = false;
        PlayLayer::onQuit();
    }
};

class $modify(DeathShuffleAudio, FMODAudioEngine) {
    // Whenever GD (re)starts the level's main music, play our song instead.
    void queueStartMusic(gd::string audioFilename, float pitch, float unknown, float volume, bool loop, int start,
                         int end, int fadeIn, int fadeOut, int musicID, bool p10, int channelID, bool noPrepare,
                         bool dontReset) {
        auto layer = PlayLayer::get();
        // Channel 0 is the main level song; leave song triggers on other channels alone.
        if (g_current && channelID == 0 && activeIn(layer)) {
            if (g_startPending) {
                g_startPending = false;
                announce(*g_current);
            }
            int ms = shuffledStartMs(*g_current, layer);
            stopJukeboxOffset();
            FMODAudioEngine::queueStartMusic(utils::string::pathToString(g_current->path), pitch, unknown, volume,
                                             true, ms, 0, fadeIn, fadeOut, musicID, p10, channelID, noPrepare,
                                             dontReset);
            return;
        }

        FMODAudioEngine::queueStartMusic(audioFilename, pitch, unknown, volume, loop, start, end, fadeIn, fadeOut,
                                         musicID, p10, channelID, noPrepare, dontReset);
    }

    // When GD jumps the level music to a time on respawn, jump our song to
    // the right spot instead.
    void setMusicTimeMS(unsigned int ms, bool dontWait, int channel) {
        auto layer = PlayLayer::get();
        if (!g_ownSeek && g_restarting && g_current && channel == 0 && activeIn(layer)) {
            stopJukeboxOffset();
            FMODAudioEngine::setMusicTimeMS(shuffledStartMs(*g_current, layer), dontWait, channel);
            return;
        }
        FMODAudioEngine::setMusicTimeMS(ms, dontWait, channel);
    }
};
