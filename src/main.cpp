// Death Shuffle: every death switches the level music to a random song
// you downloaded with Jukebox (fleym.nongd).
//
// Jukebox has no public C++ API for other mods, so this reads the song list
// Jukebox saves to disk (<Jukebox save dir>/manifest/<songID>.json). It only
// reads those files and never changes them.

#include <Geode/Geode.hpp>
#include <Geode/modify/FMODAudioEngine.hpp>
#include <Geode/modify/PlayLayer.hpp>

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
    int offset = 0;
};

std::vector<ShuffleSong> g_pool;
std::optional<ShuffleSong> g_current;  // song currently replacing the level music
bool g_startPending = false;           // a new song was rolled and still needs to start
std::mt19937 g_rng{std::random_device{}()};

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

int startOffsetFor(ShuffleSong const& song) {
    return setting("use-song-offset") ? song.offset : 0;
}

bool activeIn(PlayLayer* layer) {
    if (!layer || !setting("enabled")) return false;
    if (layer->m_isPracticeMode && !setting("in-practice")) return false;
    return true;
}

void announce(ShuffleSong const& song) {
    if (!setting("show-song-name")) return;
    std::string text = song.artist.empty() ? song.name : fmt::format("{} - {}", song.name, song.artist);
    Notification::create(fmt::format("Now playing: {}", text), NotificationIcon::Info, 2.f)->show();
}

void startCurrentSong() {
    if (!g_current) return;
    g_startPending = false;

    auto engine = FMODAudioEngine::get();
    // The queueStartMusic hook below swaps the file and offset for us.
    engine->playMusic(utils::string::pathToString(g_current->path), true, 0.f, 0);
    engine->setMusicTimeMS(startOffsetFor(*g_current), true, 0);
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
        loadSongPool(level);

        if (setting("enabled") && setting("shuffle-first-attempt")) rollSong();

        return PlayLayer::init(level, useReplay, dontCreateObjects);
    }

    void destroyPlayer(PlayerObject* player, GameObject* object) {
        PlayLayer::destroyPlayer(player, object);

        // GD "kills" the player once at level start as an anticheat check.
        if (object == m_anticheatSpike) return;
        // Noclip and similar don't actually kill you.
        if (!m_playerDied && !(player && player->m_isDead)) return;
        // In dual mode both players can die on the same frame.
        if (m_fields->rolledThisDeath) return;
        if (!activeIn(this)) return;

        m_fields->rolledThisDeath = true;
        rollSong();
    }

    void resetLevel() {
        m_fields->rolledThisDeath = false;
        PlayLayer::resetLevel();

        if (g_startPending && activeIn(this)) startCurrentSong();
    }

    void onQuit() {
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
            FMODAudioEngine::queueStartMusic(utils::string::pathToString(g_current->path), pitch, unknown, volume,
                                             true, startOffsetFor(*g_current), 0, fadeIn, fadeOut, musicID, p10,
                                             channelID, noPrepare, dontReset);
            return;
        }

        FMODAudioEngine::queueStartMusic(audioFilename, pitch, unknown, volume, loop, start, end, fadeIn, fadeOut,
                                         musicID, p10, channelID, noPrepare, dontReset);
    }
};
