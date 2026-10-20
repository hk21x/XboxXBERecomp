// Achievements: GameInfo::achievements' conditions on guest memory, checked once a frame, unlocks kept beside the exe.
//
// Conditions use RetroAchievements' syntax, so a set written here can be submitted there as it is. Supported:
// - memory: 0xH (8 bit), 0x (16), 0xW (24), 0xX (32), 0xM..0xT (bit 0..7), 0xL / 0xU (low / high nibble), 0xK (bits
//   set); with d (the value last frame) or p (the value before it last changed) in front; numbers in decimal or
//   with h in front (hex)
// - comparisons = != < <= > >=, hit counts as .N. at the end
// - flags: A: add source, B: sub source, I: add address (pointer), N: and next, O: or next, R: reset if, P: pause if,
//   M: measured, T: trigger (both as plain conditions here)
// - groups: the core, then alternatives after S; the achievement is met when the core and any alternative are
// An achievement must be seen unmet once after start before it can unlock, as RetroAchievements does, so loading a
// finished save does not award everything at once.
#include "core/achievements.h"

#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <ctime>
#include <fstream>
#include <memory>
#include <mutex>

#include "app/app.h"
#include "core/log.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"

namespace achievements {

namespace {

enum class Size { k8, k16, k24, k32, kBit, kLow, kHigh, kBitCount };
enum class Kind { kValue, kMem, kDelta, kPrior };

struct Operand {
    Kind kind = Kind::kValue;
    Size size = Size::k32;
    uint32_t bit = 0;
    uint32_t value = 0;  // the number, or the address
    // per operand history
    uint32_t last = 0, prior = 0, current = 0;
    bool seen = false;
};

enum class Op { kNone, kEq, kNe, kLt, kLe, kGt, kGe };

struct Cond {
    char flag = 0;  // 0, 'A', 'B', 'I', 'N', 'O', 'R', 'P', 'M', 'T'
    Operand left, right;
    Op op = Op::kNone;
    uint32_t target = 0;  // hits needed, 0 none
    uint32_t hits = 0;
};

struct Group {
    std::vector<Cond> conds;
};

struct Ach {
    const Def* def;
    std::vector<Group> groups;  // [0] core
    bool valid = false, armed = false, unlocked = false;
    std::string when;
};

std::mutex g_lock;
std::vector<Ach> g_list;
bool g_loaded = false;
std::string g_recent;
uint32_t g_recent_points = 0;
std::chrono::steady_clock::time_point g_recent_at;

std::filesystem::path File() { return app::ExeDir() / "achievements.toml"; }

uint32_t Read(uint32_t va, Size s, uint32_t bit) {
    // a page that is not there reads 0
    if (!mem::IsCommitted(va) || ((s == Size::k16 || s == Size::k24 || s == Size::k32) && !mem::IsCommitted(va + 3)))
        return 0;
    switch (s) {
        case Size::k8: return kernel::G<uint8_t>(va);
        case Size::k16: return kernel::R16(va);
        case Size::k24: return kernel::R32(va) & 0xFFFFFF;
        case Size::k32: return kernel::R32(va);
        case Size::kBit: return (kernel::G<uint8_t>(va) >> bit) & 1;
        case Size::kLow: return kernel::G<uint8_t>(va) & 15;
        case Size::kHigh: return kernel::G<uint8_t>(va) >> 4;
        case Size::kBitCount: {
            uint8_t b = kernel::G<uint8_t>(va);
            uint32_t n = 0;
            for (; b; b &= b - 1) n++;
            return n;
        }
    }
    return 0;
}

// p at an operand; false when it is not one
bool ParseOperand(const char*& p, Operand& o) {
    o = {};
    if (*p == 'd' || *p == 'p') {
        o.kind = *p == 'd' ? Kind::kDelta : Kind::kPrior;
        p++;
    }
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        p += 2;
        if (o.kind == Kind::kValue) o.kind = Kind::kMem;
        o.size = Size::k16;
        char c = char(std::toupper(static_cast<unsigned char>(*p)));
        if (c == ' ') {
            p++;
        } else if (std::strchr("HWXMNOPQRSTLUK", c) && c) {
            p++;
            switch (c) {
                case 'H': o.size = Size::k8; break;
                case 'W': o.size = Size::k24; break;
                case 'X': o.size = Size::k32; break;
                case 'L': o.size = Size::kLow; break;
                case 'U': o.size = Size::kHigh; break;
                case 'K': o.size = Size::kBitCount; break;
                default: o.size = Size::kBit, o.bit = uint32_t(c - 'M');
            }
        }
        char* end;
        o.value = uint32_t(std::strtoul(p, &end, 16));
        if (end == p) return false;
        p = end;
        return true;
    }
    if (o.kind != Kind::kValue) return false;  // d or p before a number
    char* end;
    if (*p == 'h' || *p == 'H') o.value = uint32_t(std::strtoul(p + 1, &end, 16));
    else o.value = uint32_t(std::strtoul(p, &end, 10));
    if (end == p || end == p + 1 && (*p == 'h' || *p == 'H')) return false;
    p = end;
    return true;
}

std::string Parse(const char* text, std::vector<Group>& groups) {
    groups.assign(1, {});
    const char* p = text;
    while (*p) {
        Cond c;
        if (p[0] && p[1] == ':') {
            c.flag = char(std::toupper(static_cast<unsigned char>(p[0])));
            if (!std::strchr("ABINORPMT", c.flag)) return std::string("unsupported flag ") + p[0];
            p += 2;
        }
        if (!ParseOperand(p, c.left)) return std::string("bad operand at ") + p;
        static const struct {
            const char* s;
            Op op;
        } kOps[] = {{"!=", Op::kNe}, {"<=", Op::kLe}, {">=", Op::kGe}, {"==", Op::kEq}, {"=", Op::kEq}, {"<", Op::kLt}, {">", Op::kGt}};
        for (auto& o : kOps)
            if (std::strncmp(p, o.s, std::strlen(o.s)) == 0) {
                c.op = o.op;
                p += std::strlen(o.s);
                break;
            }
        if (c.op != Op::kNone && !ParseOperand(p, c.right)) return std::string("bad operand at ") + p;
        bool source = c.flag == 'A' || c.flag == 'B' || c.flag == 'I';
        if (c.op == Op::kNone && !source) return "a condition without a comparison";
        if (*p == '.') {
            char* end;
            c.target = uint32_t(std::strtoul(p + 1, &end, 10));
            if (*end != '.') return "bad hit count";
            p = end + 1;
        }
        groups.back().conds.push_back(c);
        if (*p == '_') {
            p++;
        } else if (*p == 'S') {
            p++;
            groups.push_back({});
        } else if (*p) {
            return std::string("unexpected ") + p;
        }
    }
    return {};
}

uint32_t Value(Operand& o, uint32_t offset) {
    if (o.kind == Kind::kValue) return o.value;
    uint32_t v = Read(o.value + offset, o.size, o.bit);
    // history per operand: the value last frame, and the one before the last change
    if (!o.seen) o.last = o.prior = o.current = v, o.seen = true;
    if (v != o.current) o.prior = o.current;
    o.last = o.current;
    o.current = v;
    return o.kind == Kind::kDelta ? o.last : o.kind == Kind::kPrior ? o.prior : v;
}

bool Compare(uint32_t a, Op op, uint32_t b) {
    switch (op) {
        case Op::kEq: return a == b;
        case Op::kNe: return a != b;
        case Op::kLt: return a < b;
        case Op::kLe: return a <= b;
        case Op::kGt: return a > b;
        case Op::kGe: return a >= b;
        case Op::kNone: return false;
    }
    return false;
}

// one frame of a group: whether it holds; a reset if that held is reported through reset
bool Evaluate(Group& g, bool& reset) {
    bool all = true, paused = false;
    uint32_t add = 0, offset = 0;
    bool chain = true, chain_set = false;
    char chain_op = 0;
    // pause ifs first, as RetroAchievements does: a paused group keeps its hits
    for (Cond& c : g.conds) {
        if (c.flag != 'P') continue;
        if (Compare(Value(c.left, 0), c.op, Value(c.right, 0))) paused = true;
    }
    if (paused) return false;
    for (Cond& c : g.conds) {
        if (c.flag == 'P') continue;
        if (c.flag == 'I') {
            offset = Value(c.left, offset);
            continue;
        }
        uint32_t left = Value(c.left, offset), right = Value(c.right, offset);
        offset = 0;
        if (c.flag == 'A') {
            add += left;
            continue;
        }
        if (c.flag == 'B') {
            add -= left;
            continue;
        }
        bool met = Compare(add + left, c.op, right);
        add = 0;
        if (chain_set) met = chain_op == 'N' ? chain && met : chain || met;
        chain_set = false;
        if (c.flag == 'N' || c.flag == 'O') {
            chain = met;
            chain_op = c.flag;
            chain_set = true;
            continue;
        }
        if (c.flag == 'R') {
            if (met) reset = true;
            continue;
        }
        if (c.target) {
            if (met && c.hits < c.target) c.hits++;
            met = c.hits >= c.target;
        }
        all = all && met;
    }
    return all;
}

void Load() {
    g_loaded = true;
    for (const Def& d : app::kGame.achievements) {
        Ach a{&d};
        std::string err = Parse(d.condition, a.groups);
        a.valid = err.empty();
        if (!a.valid) LOGE("achievements: %s: %s", d.id, err.c_str());
        g_list.push_back(std::move(a));
    }
    // achievements.toml: [unlocked] then id = "when" lines, written by Save
    std::ifstream f(File());
    for (std::string line; std::getline(f, line);) {
        size_t eq = line.find(" = \"");
        if (eq == std::string::npos || line.back() != '"') continue;
        std::string id = line.substr(0, eq), when = line.substr(eq + 4, line.size() - eq - 5);
        for (Ach& a : g_list)
            if (id == a.def->id) a.unlocked = true, a.when = when;
    }
}

void Save() {
    std::ofstream f(File());
    f << "# achievements unlocked, and when\n[unlocked]\n";
    for (const Ach& a : g_list)
        if (a.unlocked) f << a.def->id << " = \"" << a.when << "\"\n";
}

std::string Now() {
    std::time_t now = std::time(nullptr);
    char b[32];
    std::strftime(b, sizeof b, "%Y-%m-%d %H:%M", std::localtime(&now));
    return b;
}

}  // namespace

std::string Check(const char* condition) {
    std::vector<Group> g;
    return Parse(condition, g);
}

void Frame() {
    if (app::kGame.achievements.empty()) return;
    std::lock_guard lock(g_lock);
    if (!g_loaded) Load();
    bool changed = false;
    for (Ach& a : g_list) {
        if (!a.valid || a.unlocked) continue;
        bool reset = false;
        bool core = Evaluate(a.groups[0], reset);
        bool alt = a.groups.size() == 1;
        for (size_t i = 1; i < a.groups.size(); i++) alt = Evaluate(a.groups[i], reset) || alt;
        if (reset)
            for (Group& g : a.groups)
                for (Cond& c : g.conds) c.hits = 0;
        bool met = !reset && core && alt;
        if (!met) {
            a.armed = true;
            continue;
        }
        if (!a.armed) continue;  // met since start: waits until it is unmet once
        a.unlocked = true;
        a.when = Now();
        g_recent = a.def->title;
        g_recent_points = a.def->points;
        g_recent_at = std::chrono::steady_clock::now();
        LOGR("achievement unlocked: %s (%u points)", a.def->title, a.def->points);
        changed = true;
    }
    if (changed) Save();
}

std::vector<Status> List() {
    std::lock_guard lock(g_lock);
    if (!g_loaded) Load();
    std::vector<Status> out;
    for (const Ach& a : g_list) out.push_back({a.def, a.unlocked, a.when});
    return out;
}

bool Recent(std::string& title, uint32_t& points, double& age) {
    std::lock_guard lock(g_lock);
    if (g_recent.empty()) return false;
    title = g_recent;
    points = g_recent_points;
    age = std::chrono::duration<double>(std::chrono::steady_clock::now() - g_recent_at).count();
    return true;
}

}  // namespace achievements
