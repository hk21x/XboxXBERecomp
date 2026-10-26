#pragma once
// Xbox custom soundtracks from a PC music folder. The dashboard rips CDs to E:\TDATA\FFFE0000\MUSIC: WMA files
// <soundtrack id>\<song id>.WMA (lower case hex, the song id's top 16 bits the soundtrack's) listed in ST.DB, which a
// game reads through XAPI's soundtrack calls and plays with its own WMA decoder. The launcher converts each file to
// WMA 2 with ffmpeg (the decoder takes ffmpeg's) and writes the database (core/builder.cpp ImportMusic).
//
// ST.DB, blocks of 0x200 bytes (read from PGR2's XAPI, 0x190014 to 0x19020C):
//   block 0         version 1, soundtrack count (100 at most), next soundtrack id, the soundtrack ids from +0x0C
//   blocks 1..100   a soundtrack each, in the header's order: magic 0x21371, id, song count, its song groups' block
//                   numbers from +0x0C (84), total length in ms at +0x15C, name (32 UTF-16 units) at +0x160
//   0x65 + n        song group n: magic 0x31073, soundtrack id, group number in the soundtrack, six song ids from
//                   +0x10, their lengths in ms from +0x28, their names (32 UTF-16 units each) from +0x40
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace launcher {

struct MusicSong {
    std::filesystem::path source;
    std::string name;  // "Artist / Title" from its tags, else its file name; UTF-8
    uint32_t ms = 0;
};

struct MusicSoundtrack {
    std::string name;  // the folder's name
    std::vector<MusicSong> songs;
};

constexpr size_t kMaxSoundtracks = 100, kMaxSongs = 84 * 6;

// a folder's soundtracks: each subfolder one (everything under it, in name order), the files directly in the folder
// one named after it; audio files only, at most 100 soundtracks of 504 songs. notes: what was left out
std::vector<MusicSoundtrack> ScanMusic(const std::filesystem::path& folder, std::vector<std::string>& notes);
bool IsAudioFile(const std::filesystem::path& path);

// the soundtrack's (1 on) song file under MUSIC: "0001/00010000.WMA"
std::string SongFile(uint32_t soundtrack, uint32_t song);
// ST.DB for these soundtracks, numbered 1 on in order
std::string SoundtrackDatabase(const std::vector<MusicSoundtrack>& soundtracks);

}  // namespace launcher
