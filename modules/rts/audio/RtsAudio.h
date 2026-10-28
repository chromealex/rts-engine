// Sound for the generic RTS module: turns simulation events into sounds from the game's
// `audio.json`. Presentation only — it reads the sim and never writes into it.
//
// Besides "sounds" and "music" (see engine/audio/Audio.h) the bank maps gameplay to sounds:
//
//   "events":    { "died": "death", "underAttack": "alarm", "notEnough.gold": "need_gold", ... },
//   "types":     { "footman": { "select": "footman_what", "move": "footman_yes", "died": "death_human",
//                               "weapons": { "sword": { "attack": "swing", "hit": "clang" } } } },
//   "weapons":   { "bow": { "attack": "bow_release", "hit": "arrow_hit" } },   // by weapon id, any type
//   "abilities": { "heal": "heal_cast" }                                         // by ability id
//
// Lookup order for a unit: its type entry, then weapons/abilities by id, then "events".
// Cue names: see kCueNames in RtsAudio.cpp (attack, hit, died, ready, buildDone, underAttack, ...).
// Music playlists used automatically: "match", "battle" (heavy fighting nearby), "victory", "defeat".
#pragma once
#include <array>
#include <memory>
#include <string>
#include <vector>
#include "../../../engine/audio/Audio.h"
#include "../../../engine/sim/Simulation.h"
#include "../RtsData.h"

namespace rts::game {

enum class Cue : uint8_t {
    Attack, Hit, Miss, Died, Spawned, TrainStarted, Ready, BuildPlaced, BuildDone, Gathered,
    Captured, Lost, PlayerDefeated, Victory, Defeat, NotEnough, NotEnoughSupply, NoRoom,
    Researched, LevelUp, Ability, ReadyToPlace, Sold, RegionClaimed, Wave, LowPower, Upgraded,
    Message, UnderAttack, BaseUnderAttack, Select, Move, AttackOrder, BuildOrder, Click, Error,
    Count
};
const char* cueName(Cue c);

class RtsAudio {
public:
    // `bank` is the parsed audio.json whose sounds are already loaded into `engine`.
    // Returns false on malformed mapping; unknown sound names are warnings (that cue stays silent).
    bool init(audio::AudioEngine& engine, const Rules& rules, const Json& bank, std::string* error = nullptr);
    const std::vector<std::string>& warnings() const { return warnings_; }

    // Start listening to a match. `viewer` = the local player (0 = spectator: world sounds only).
    void attach(Simulation& sim, uint8_t viewer);
    void detach();
    // Once per frame after the simulation advanced: plays everything that happened since.
    void update(Simulation& sim, double dt);

    // Acknowledgements from the client's input.
    void onSelect(Simulation& sim, Entity unit);
    void onCommand(Simulation& sim, Entity firstUnit, uint16_t commandType);
    void onMatchEnd(bool won);

    // Listener for an RTS camera (eye looking at a ground target). The ear hangs on the view ray
    // above the target: zooming out raises it, so the battle sounds farther (quieter, duller,
    // roomier), while hearing widens with the square root of the zoom so the visible
    // battlefield never goes silent. `referenceDistance` = camera distance of "normal" zoom.
    static audio::ListenerPose cameraListener(const float target[3], const float eye[3], float referenceDistance = 24.0f);
    void play(Cue c);   // UI cues (click, error) and anything without a position

    // How many sounds update() started since attach (tests, debug overlay).
    int played() const { return played_; }

private:
    struct WeaponSounds { audio::SoundId attack = audio::kNoSound, hit = audio::kNoSound; };
    struct TypeSounds {
        std::array<audio::SoundId, size_t(Cue::Count)> cue;
        std::vector<WeaponSounds> weapons;      // by weapon index
        std::vector<audio::SoundId> abilities;  // by ability index
    };
    struct Collector;

    audio::SoundId resolve(const Json& name, const std::string& where);
    audio::SoundId cueFor(int type, Cue c) const;
    bool playAt(audio::SoundId s, const Vec3& pos, bool positional, float height = 0);
    // `worldSounds` = false still updates alerts and battle intensity, without playing the hit itself.
    void handle(Simulation& sim, const SimEvent& ev, bool worldSounds);

    audio::AudioEngine* engine_ = nullptr;
    const Rules* rules_ = nullptr;
    std::array<audio::SoundId, size_t(Cue::Count)> global_{};
    std::vector<TypeSounds> types_;
    std::vector<audio::SoundId> notEnoughRes_;   // by resource index
    std::vector<std::string> warnings_;
    std::shared_ptr<Collector> collector_;
    uint8_t viewer_ = 0;
    double time_ = 0, lastAlert_ = -1e9;
    float intensity_ = 0;
    double calmSince_ = 0;
    bool inBattle_ = false;
    int played_ = 0;
};

} // namespace rts::game
