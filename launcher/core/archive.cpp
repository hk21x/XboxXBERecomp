#include "core/archive.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <fstream>
#include <memory>
#include <vector>

#include "miniz.h"
extern "C" {
#include "xz.h"
}

#include "core/util.h"

namespace fs = std::filesystem;

namespace launcher {

namespace {

// a relative path that stays inside the destination: no root, no drive, no ".."
bool SafeName(const std::string& name) {
    if (name.empty() || name[0] == '/' || name[0] == '\\' || name.find(':') != std::string::npos) return false;
    size_t at = 0;
    while (at <= name.size()) {
        size_t end = name.find_first_of("/\\", at);
        if (end == std::string::npos) end = name.size();
        if (name.compare(at, end - at, "..") == 0 && end - at == 2) return false;
        at = end + 1;
    }
    return true;
}

fs::path Under(const fs::path& dest, const std::string& rel) { return dest / PathOf(rel); }

bool Cancelled(const std::atomic<bool>* cancel) { return cancel && cancel->load(); }

}  // namespace

std::string StripComponents(const std::string& name, int n) {
    size_t at = 0;
    for (int i = 0; i < n; i++) {
        size_t slash = name.find('/', at);
        if (slash == std::string::npos) return {};
        at = slash + 1;
    }
    return name.substr(at);
}

bool Unzip(const fs::path& zip, const fs::path& dest, const EntryFilter& filter, const Progress& progress,
           const std::atomic<bool>* cancel, std::string& error) {
    mz_zip_archive z{};
    std::string zpath = U8(zip);
    if (!mz_zip_reader_init_file(&z, zpath.c_str(), 0)) {
        error = "cannot open " + zpath + " as a zip";
        return false;
    }
    std::unique_ptr<mz_zip_archive, void (*)(mz_zip_archive*)> closer(&z, [](mz_zip_archive* a) { mz_zip_reader_end(a); });
    mz_uint count = mz_zip_reader_get_num_files(&z);
    uint64_t total = 0, done = 0;
    for (mz_uint i = 0; i < count; i++) {
        mz_zip_archive_file_stat st;
        if (mz_zip_reader_file_stat(&z, i, &st)) total += st.m_uncomp_size;
    }
    for (mz_uint i = 0; i < count; i++) {
        if (Cancelled(cancel)) {
            error = "cancelled";
            return false;
        }
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&z, i, &st)) continue;
        std::string name = st.m_filename;
        for (char& c : name)
            if (c == '\\') c = '/';
        std::string rel = filter ? filter(name) : name;
        if (rel.empty() || st.m_is_directory) continue;
        if (!SafeName(rel)) {
            error = "unsafe path in " + zpath + ": " + name;
            return false;
        }
        fs::path out = Under(dest, rel);
        std::error_code ec;
        fs::create_directories(out.parent_path(), ec);
        std::string opath = U8(out);
        if (!mz_zip_reader_extract_to_file(&z, i, opath.c_str(), 0)) {
            error = "cannot write " + opath + " from " + zpath;
            return false;
        }
        done += st.m_uncomp_size;
        if (progress) progress(done, total);
    }
    return true;
}

namespace {

// a tar stream fed in pieces: ustar, GNU long names (L) and pax paths (x); files, folders, and links as copies of
// what they name (Windows has no links without rights)
class TarReader {
public:
    TarReader(const fs::path& dest, const EntryFilter& filter) : dest_(dest), filter_(filter) {}

    bool Feed(const uint8_t* p, size_t n, std::string& error) {
        while (n) {
            if (in_data_) {
                size_t take = std::min<uint64_t>(n, remaining_);
                if (keep_) {
                    if (meta_) meta_buf_.append(reinterpret_cast<const char*>(p), take);
                    else out_.write(reinterpret_cast<const char*>(p), std::streamsize(take));
                }
                remaining_ -= take, p += take, n -= take;
                if (remaining_ == 0) {
                    in_data_ = false;
                    skip_ = pad_;
                    if (!EndEntry(error)) return false;
                }
                continue;
            }
            if (skip_) {
                size_t take = std::min<uint64_t>(n, skip_);
                skip_ -= take, p += take, n -= take;
                continue;
            }
            size_t take = std::min<size_t>(n, 512 - header_len_);
            std::memcpy(header_ + header_len_, p, take);
            header_len_ += take, p += take, n -= take;
            if (header_len_ == 512) {
                header_len_ = 0;
                if (!Header(error)) return false;
            }
        }
        return true;
    }
    bool Ended() const { return ended_; }

private:
    static uint64_t Number(const char* f, size_t len) {
        if (uint8_t(f[0]) & 0x80) {  // base-256
            uint64_t v = 0;
            for (size_t i = 1; i < len; i++) v = v << 8 | uint8_t(f[i]);
            return v;
        }
        uint64_t v = 0;
        for (size_t i = 0; i < len && f[i]; i++)
            if (f[i] >= '0' && f[i] <= '7') v = v * 8 + uint64_t(f[i] - '0');
        return v;
    }
    static std::string Field(const char* f, size_t len) { return std::string(f, strnlen(f, len)); }

    bool Header(std::string& error) {
        bool zero = std::all_of(header_, header_ + 512, [](char c) { return c == 0; });
        if (zero) {
            ended_ = true;
            return true;
        }
        char type = header_[156];
        uint64_t size = Number(header_ + 124, 12);
        std::string name = Field(header_, 100), prefix = Field(header_ + 345, 155);
        if (std::memcmp(header_ + 257, "ustar", 5) == 0 && !prefix.empty()) name = prefix + "/" + name;
        if (!long_name_.empty()) name = long_name_, long_name_.clear();
        if (!pax_path_.empty()) name = pax_path_, pax_path_.clear();
        std::string link = Field(header_ + 157, 100);
        if (!long_link_.empty()) link = long_link_, long_link_.clear();

        remaining_ = size;
        pad_ = (512 - size % 512) % 512;
        meta_ = type == 'L' || type == 'K' || type == 'x' || type == 'g';
        meta_type_ = type;
        keep_ = false;
        if (meta_) {
            keep_ = true;
            meta_buf_.clear();
        } else if (type == '0' || type == '\0' || type == '7') {
            rel_ = filter_ ? filter_(name) : name;
            if (!rel_.empty()) {
                if (!SafeName(rel_)) {
                    error = "unsafe path in the archive: " + name;
                    return false;
                }
                fs::path out = Under(dest_, rel_);
                std::error_code ec;
                fs::create_directories(out.parent_path(), ec);
                out_.open(out, std::ios::binary | std::ios::trunc);
                if (!out_) {
                    error = "cannot write " + U8(out);
                    return false;
                }
                keep_ = true;
                written_[name] = rel_;
            }
        } else if (type == '1' || type == '2') {
            // a link: a copy of what it names, once that was written (a symbolic one relative to its own folder)
            std::string rel = filter_ ? filter_(name) : name;
            if (!rel.empty() && SafeName(rel)) {
                std::string target = link;
                if (type == '2') {
                    size_t slash = name.rfind('/');
                    target = Normal((slash == std::string::npos ? "" : name.substr(0, slash + 1)) + link);
                }
                auto it = written_.find(target);
                if (it != written_.end()) {
                    std::error_code ec;
                    fs::path out = Under(dest_, rel);
                    fs::create_directories(out.parent_path(), ec);
                    fs::copy_file(Under(dest_, it->second), out, fs::copy_options::overwrite_existing, ec);
                    if (!ec) written_[name] = rel;
                }
            }
        }
        if (remaining_) in_data_ = true;
        else {
            skip_ = 0;
            return EndEntry(error);
        }
        return true;
    }

    bool EndEntry(std::string& /*error*/) {
        if (out_.is_open()) out_.close();
        if (meta_) {
            if (meta_type_ == 'L') long_name_ = meta_buf_.c_str();
            else if (meta_type_ == 'K') long_link_ = meta_buf_.c_str();
            else if (meta_type_ == 'x') {
                // records "len key=value\n"
                size_t at = 0;
                while (at < meta_buf_.size()) {
                    size_t sp = meta_buf_.find(' ', at);
                    if (sp == std::string::npos) break;
                    size_t len = std::strtoul(meta_buf_.c_str() + at, nullptr, 10);
                    if (len == 0 || at + len > meta_buf_.size()) break;
                    std::string rec = meta_buf_.substr(sp + 1, at + len - sp - 2);
                    if (rec.rfind("path=", 0) == 0) pax_path_ = rec.substr(5);
                    if (rec.rfind("linkpath=", 0) == 0) long_link_ = rec.substr(9);
                    at += len;
                }
            }
            meta_ = false;
        }
        return true;
    }

    static std::string Normal(const std::string& path) {
        std::vector<std::string> parts;
        size_t at = 0;
        while (at <= path.size()) {
            size_t end = path.find('/', at);
            if (end == std::string::npos) end = path.size();
            std::string part = path.substr(at, end - at);
            if (part == "..") {
                if (!parts.empty()) parts.pop_back();
            } else if (!part.empty() && part != ".") parts.push_back(part);
            at = end + 1;
        }
        std::string out;
        for (const std::string& p : parts) out += (out.empty() ? "" : "/") + p;
        return out;
    }

    fs::path dest_;
    EntryFilter filter_;
    char header_[512];
    size_t header_len_ = 0;
    bool in_data_ = false, keep_ = false, meta_ = false, ended_ = false;
    char meta_type_ = 0;
    uint64_t remaining_ = 0, pad_ = 0, skip_ = 0;
    std::ofstream out_;
    std::string rel_, meta_buf_, long_name_, long_link_, pax_path_;
    std::map<std::string, std::string> written_;  // archive name -> where it was written
};

}  // namespace

bool UntarXz(const fs::path& file, const fs::path& dest, const EntryFilter& filter, const Progress& progress,
             const std::atomic<bool>* cancel, std::string& error) {
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        error = "cannot open " + U8(file);
        return false;
    }
    std::error_code ec;
    uint64_t total = fs::file_size(file, ec), done = 0;
    static const bool crc_ready = [] {
        xz_crc32_init();
        xz_crc64_init();
        return true;
    }();
    (void)crc_ready;
    std::unique_ptr<xz_dec, void (*)(xz_dec*)> dec(xz_dec_init(XZ_DYNALLOC, 1u << 30), xz_dec_end);
    if (!dec) {
        error = "out of memory for the xz decoder";
        return false;
    }
    TarReader tar(dest, filter);
    std::vector<uint8_t> inbuf(1 << 20), outbuf(4 << 20);
    xz_buf b{inbuf.data(), 0, 0, outbuf.data(), 0, outbuf.size()};
    for (;;) {
        if (Cancelled(cancel)) {
            error = "cancelled";
            return false;
        }
        if (b.in_pos == b.in_size) {
            in.read(reinterpret_cast<char*>(inbuf.data()), std::streamsize(inbuf.size()));
            b.in_size = size_t(in.gcount());
            b.in_pos = 0;
            done += b.in_size;
            if (progress) progress(done, total);
        }
        xz_ret r = xz_dec_run(dec.get(), &b);
        if (b.out_pos) {
            if (!tar.Feed(outbuf.data(), b.out_pos, error)) return false;
            b.out_pos = 0;
        }
        if (r == XZ_STREAM_END) break;
        if (r != XZ_OK && r != XZ_UNSUPPORTED_CHECK) {
            error = "the xz data is damaged (" + std::to_string(int(r)) + ") in " + U8(file);
            return false;
        }
        if (b.in_size == 0 && b.in_pos == 0 && !in) {
            error = "the xz file ends early: " + U8(file);
            return false;
        }
    }
    return true;
}

}  // namespace launcher
