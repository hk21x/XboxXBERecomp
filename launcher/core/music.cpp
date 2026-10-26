#include "core/music.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <system_error>

#include "core/util.h"

namespace fs = std::filesystem;

namespace launcher {

namespace {

constexpr uint32_t kBlock = 0x200, kSoundtrackMagic = 0x21371, kGroupMagic = 0x31073, kGroupBase = 0x65;
constexpr size_t kNameUnits = 32;  // UTF-16 units, the last one the terminator

std::string Lowered(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// names in the order Explorer shows them, near enough: case folded
bool ByName(const fs::path& a, const fs::path& b) { return Lowered(U8(a.filename())) < Lowered(U8(b.filename())); }

void Put32(std::string& out, size_t at, uint32_t v) {
    for (int i = 0; i < 4; i++) out[at + i] = char(v >> (i * 8) & 0xFF);
}

// UTF-8 into at most 31 UTF-16 units and a terminator, at a block offset
void PutName(std::string& out, size_t at, const std::string& utf8) {
    std::u16string u;
    for (size_t i = 0; i < utf8.size();) {
        unsigned char c = static_cast<unsigned char>(utf8[i]);
        uint32_t cp = c, n = 0;
        if (c >= 0xF0) cp = c & 7, n = 3;
        else if (c >= 0xE0) cp = c & 15, n = 2;
        else if (c >= 0xC0) cp = c & 31, n = 1;
        i++;
        for (uint32_t k = 0; k < n && i < utf8.size(); k++, i++) cp = cp << 6 | (static_cast<unsigned char>(utf8[i]) & 63);
        if (cp >= 0x10000) {
            if (u.size() + 2 > kNameUnits - 1) break;
            cp -= 0x10000;
            u += char16_t(0xD800 + (cp >> 10));
            u += char16_t(0xDC00 + (cp & 0x3FF));
        } else {
            if (u.size() + 1 > kNameUnits - 1) break;
            u += char16_t(cp);
        }
    }
    for (size_t k = 0; k < u.size(); k++) {
        out[at + k * 2] = char(u[k] & 0xFF);
        out[at + k * 2 + 1] = char(u[k] >> 8);
    }
}

void Collect(const fs::path& dir, std::vector<fs::path>& files) {
    std::error_code ec;
    std::vector<fs::path> here, subs;
    for (const auto& e : fs::directory_iterator(dir, fs::directory_options::skip_permission_denied, ec)) {
        if (e.is_directory(ec)) subs.push_back(e.path());
        else if (e.is_regular_file(ec) && IsAudioFile(e.path())) here.push_back(e.path());
    }
    std::sort(here.begin(), here.end(), ByName);
    std::sort(subs.begin(), subs.end(), ByName);
    files.insert(files.end(), here.begin(), here.end());
    for (const fs::path& s : subs) Collect(s, files);
}

}  // namespace

bool IsAudioFile(const fs::path& path) {
    static const char* kTypes[] = {".mp3", ".flac", ".ogg", ".opus", ".m4a", ".aac", ".wav", ".wma", ".aiff", ".aif"};
    std::string ext = Lowered(U8(path.extension()));
    return std::any_of(std::begin(kTypes), std::end(kTypes), [&](const char* t) { return ext == t; });
}

std::vector<MusicSoundtrack> ScanMusic(const fs::path& folder, std::vector<std::string>& notes) {
    std::vector<MusicSoundtrack> out;
    std::error_code ec;
    std::vector<fs::path> loose, subs;
    for (const auto& e : fs::directory_iterator(folder, fs::directory_options::skip_permission_denied, ec)) {
        if (e.is_directory(ec)) subs.push_back(e.path());
        else if (e.is_regular_file(ec) && IsAudioFile(e.path())) loose.push_back(e.path());
    }
    std::sort(loose.begin(), loose.end(), ByName);
    std::sort(subs.begin(), subs.end(), ByName);
    auto add = [&](const std::string& name, const std::vector<fs::path>& files) {
        if (files.empty()) return;
        if (out.size() == kMaxSoundtracks) {
            notes.push_back("\"" + name + "\" left out: an Xbox holds " + std::to_string(kMaxSoundtracks) + " soundtracks");
            return;
        }
        MusicSoundtrack st;
        st.name = name;
        for (const fs::path& f : files) {
            if (st.songs.size() == kMaxSongs) {
                notes.push_back("\"" + name + "\": songs past " + std::to_string(kMaxSongs) + " left out");
                break;
            }
            st.songs.push_back({f, U8(f.stem()), 0});
        }
        out.push_back(std::move(st));
    };
    add(U8(folder.filename()), loose);
    for (const fs::path& s : subs) {
        std::vector<fs::path> files;
        Collect(s, files);
        add(U8(s.filename()), files);
    }
    return out;
}

std::string SongFile(uint32_t soundtrack, uint32_t song) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%04x/%08x.WMA", soundtrack, soundtrack << 16 | song);
    return buf;
}

std::string SoundtrackDatabase(const std::vector<MusicSoundtrack>& soundtracks) {
    size_t groups_total = 0;
    for (const auto& st : soundtracks) groups_total += (st.songs.size() + 5) / 6;
    std::string out((kGroupBase + groups_total) * kBlock, '\0');
    size_t count = std::min(soundtracks.size(), kMaxSoundtracks);
    Put32(out, 0, 1);
    Put32(out, 4, uint32_t(count));
    Put32(out, 8, uint32_t(count + 1));
    uint32_t group = 0;
    for (uint32_t i = 0; i < count; i++) {
        const MusicSoundtrack& st = soundtracks[i];
        uint32_t id = i + 1;
        size_t b = (1 + i) * kBlock;
        Put32(out, 12 + i * 4, id);
        Put32(out, b, kSoundtrackMagic);
        Put32(out, b + 4, id);
        Put32(out, b + 8, uint32_t(st.songs.size()));
        uint32_t total = 0;
        for (size_t first = 0; first < st.songs.size(); first += 6, group++) {
            size_t g = (kGroupBase + group) * kBlock;
            Put32(out, b + 0x0C + (first / 6) * 4, group);
            Put32(out, g, kGroupMagic);
            Put32(out, g + 4, id);
            Put32(out, g + 8, uint32_t(first / 6));
            for (size_t k = 0; k < 6 && first + k < st.songs.size(); k++) {
                const MusicSong& s = st.songs[first + k];
                Put32(out, g + 0x10 + k * 4, id << 16 | uint32_t(first + k));
                Put32(out, g + 0x28 + k * 4, s.ms);
                PutName(out, g + 0x40 + k * 0x40, s.name);
                total += s.ms;
            }
        }
        Put32(out, b + 0x15C, total);
        PutName(out, b + 0x160, st.name);
    }
    return out;
}

}  // namespace launcher
