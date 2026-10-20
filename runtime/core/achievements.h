#pragma once
#include <cstdint>
#include <string>
#include <vector>

// Achievements a game defines as conditions on guest memory, in RetroAchievements' condition syntax (a subset, see
// achievements.cpp), checked once a frame. Unlocks are kept in achievements.toml beside the exe.
namespace achievements {

struct Def {
    const char* id;           // stable key in achievements.toml
    const char* title;
    const char* description;
    uint32_t points;
    const char* condition;    // RetroAchievements syntax, e.g. "0xX366ebc>=10"
};

struct Status {
    const Def* def;
    bool unlocked;
    std::string when;  // local time of the unlock
};

void Frame();                     // the title's thread, once a presented frame
std::vector<Status> List();       // every achievement, in the game's order
// the newest unlock's title and how long ago it came (seconds), for the pop-up; false when none is showing
bool Recent(std::string& title, uint32_t& points, double& age);

// a condition's parse, for tests: empty when it is valid, else what is wrong
std::string Check(const char* condition);

}  // namespace achievements
