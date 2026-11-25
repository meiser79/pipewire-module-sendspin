// SPDX-License-Identifier: Apache-2.0

#include "state_store.h"

#include <cerrno>
#include <charconv>
#include <cstdio>
#include <cstring>
#include <vector>
#include <algorithm>
#include <array>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <sendspin/persistence_codec.h>

#include <pipewire/log.h>

namespace pwsendspin {
namespace {

// sendspin-cpp's blobs are stored base64url-encoded under "blob.<library key>".
constexpr const char* PREFIX_BLOB = "blob.";


// Values are written one per line as key=value. Backslash, newline and carriage return are
// escaped so arbitrary strings round-trip.
std::string escape(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (char c : in) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            default: out += c;
        }
    }
    return out;
}

std::string unescape(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] == '\\' && i + 1 < in.size()) {
            ++i;
            out += in[i] == 'n' ? '\n' : in[i] == 'r' ? '\r' : in[i];
        } else {
            out += in[i];
        }
    }
    return out;
}

// mkdir -p with 0700 for directories we create (the file holds secrets).
bool make_dirs(const std::string& dir) {
    if (dir.empty()) return true;
    std::string cur;
    size_t pos = 0;
    while (pos != std::string::npos) {
        pos = dir.find('/', pos + 1);
        cur = dir.substr(0, pos);
        if (cur.empty()) continue;
        if (mkdir(cur.c_str(), 0700) < 0 && errno != EEXIST) return false;
    }
    return true;
}

}  // namespace

StateStore::StateStore(std::string path) : path_(std::move(path)) {
    if (!path_.empty()) {
        const size_t slash = path_.rfind('/');
        if (slash != std::string::npos && !make_dirs(path_.substr(0, slash))) {
            pw_log_warn("sendspin: cannot create state directory for %s: %s", path_.c_str(),
                        strerror(errno));
        }
        load();
    }
}

void StateStore::load() {
    FILE* f = fopen(path_.c_str(), "re");
    if (f == nullptr) {
        if (errno != ENOENT)
            pw_log_warn("sendspin: cannot read %s: %s", path_.c_str(), strerror(errno));
        return;
    }
    char* line = nullptr;
    size_t cap = 0;
    ssize_t n;
    while ((n = getline(&line, &cap, f)) > 0) {
        std::string s(line, static_cast<size_t>(n));
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
        if (s.empty() || s[0] == '#') continue;
        const size_t eq = s.find('=');
        if (eq == std::string::npos) continue;
        values_[s.substr(0, eq)] = unescape(s.substr(eq + 1));
    }
    free(line);
    fclose(f);
}

bool StateStore::flush_locked() {
    if (path_.empty()) return true;
    const std::string tmp = path_ + ".tmp";
    int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) {
        pw_log_warn("sendspin: cannot write %s: %s", tmp.c_str(), strerror(errno));
        return false;
    }
    std::string out = "# pipewire-module-sendspin state. Contains secrets; keep private.\n";
    for (const auto& [k, v] : values_) out += k + "=" + escape(v) + "\n";
    const char* p = out.data();
    size_t left = out.size();
    while (left > 0) {
        ssize_t w = write(fd, p, left);
        if (w < 0) {
            if (errno == EINTR) continue;
            pw_log_warn("sendspin: write %s failed: %s", tmp.c_str(), strerror(errno));
            close(fd);
            unlink(tmp.c_str());
            return false;
        }
        p += w;
        left -= static_cast<size_t>(w);
    }
    fsync(fd);
    close(fd);
    if (rename(tmp.c_str(), path_.c_str()) < 0) {
        pw_log_warn("sendspin: rename to %s failed: %s", path_.c_str(), strerror(errno));
        unlink(tmp.c_str());
        return false;
    }
    return true;
}

std::optional<std::string> StateStore::get(const std::string& key) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = values_.find(key);
    if (it == values_.end()) return std::nullopt;
    return it->second;
}

bool StateStore::set(const std::string& key, const std::string& value) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = values_.find(key);
    if (it != values_.end() && it->second == value) return true;
    values_[key] = value;
    return flush_locked();
}

std::optional<size_t> StateStore::read_blob(std::string_view key, std::span<uint8_t> out) {
    const auto encoded = get(PREFIX_BLOB + std::string(key));
    if (!encoded) return std::nullopt;
    const auto bytes = sendspin::base64url_decode(*encoded);
    if (!bytes || bytes->size() > out.size()) return std::nullopt;
    std::copy(bytes->begin(), bytes->end(), out.begin());
    return bytes->size();
}

bool StateStore::write_blob(std::string_view key, std::span<const uint8_t> data) {
    return set(PREFIX_BLOB + std::string(key), sendspin::base64url_encode(data.data(), data.size()));
}

}  // namespace pwsendspin
