// SPDX-License-Identifier: Apache-2.0

#include "mpris.h"

#include <pipewire/log.h>

#ifdef HAVE_SDBUS

#include <systemd/sd-bus.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <ctime>
#include <poll.h>
#include <sys/eventfd.h>
#include <unistd.h>

namespace pwsendspin {
namespace {

constexpr const char* PATH = "/org/mpris/MediaPlayer2";
constexpr const char* ROOT_IFACE = "org.mpris.MediaPlayer2";
constexpr const char* PLAYER_IFACE = "org.mpris.MediaPlayer2.Player";
constexpr const char* BUS_NAME = "org.mpris.MediaPlayer2.Sendspin";
constexpr const char* NO_TRACK = "/org/mpris/MediaPlayer2/TrackList/NoTrack";
// A position that differs from the extrapolated one by more than this was a seek.
constexpr int64_t SEEK_DETECT_MS = 2000;

using Cmd = sendspin::SendspinControllerCommand;

int64_t monotonic_us() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000000 + ts.tv_nsec / 1000;
}

}  // namespace

struct Mpris::Impl {
    CommandFn command;
    sd_bus* bus{nullptr};
    sd_bus_slot* slot_root{nullptr};
    sd_bus_slot* slot_player{nullptr};
    int wake_fd{-1};
    std::thread thread;
    std::atomic<bool> running{false};

    std::mutex mutex;
    std::optional<MprisState> pending;  // guarded by mutex

    // MPRIS thread only
    MprisState state;
    uint64_t track_serial{0};
    std::string track_id{NO_TRACK};

    bool supports(Cmd c) const {
        return state.has_controller &&
               std::find(state.supported.begin(), state.supported.end(), c) !=
                   state.supported.end();
    }
    bool send(Cmd c) {
        sendspin::ClientCommandControllerObject o{};
        o.command = c;
        return send(o);
    }
    bool send(const sendspin::ClientCommandControllerObject& o) {
        if (!command || !command(o)) {
            pw_log_info("sendspin: MPRIS command %u not sent", static_cast<unsigned>(o.command));
            return false;
        }
        return true;
    }

    /// Current position, extrapolated from the last sample while playing.
    int64_t position_ms() const {
        int64_t pos = state.progress_ms;
        if (state.playing) pos += (monotonic_us() - state.progress_at_us) / 1000;
        if (state.duration_ms > 0) pos = std::min<int64_t>(pos, state.duration_ms);
        return std::max<int64_t>(pos, 0);
    }

    const char* playback_status() const {
        if (!state.connected) return "Stopped";
        if (state.playing) return "Playing";
        // Sendspin has no paused state: a stopped group that still shows a track is paused.
        return state.title ? "Paused" : "Stopped";
    }

    void loop();
    void apply(MprisState next);
};

// ---- property getters ------------------------------------------------------------------------

namespace {

Mpris::Impl* impl_of(void* userdata) { return static_cast<Mpris::Impl*>(userdata); }

int get_bool_false(sd_bus*, const char*, const char*, const char*, sd_bus_message* reply, void*,
                   sd_bus_error*) {
    return sd_bus_message_append(reply, "b", 0);
}
int get_bool_true(sd_bus*, const char*, const char*, const char*, sd_bus_message* reply, void*,
                  sd_bus_error*) {
    return sd_bus_message_append(reply, "b", 1);
}
int get_identity(sd_bus*, const char*, const char*, const char*, sd_bus_message* reply, void*,
                 sd_bus_error*) {
    return sd_bus_message_append(reply, "s", "Sendspin");
}
int get_empty_strv(sd_bus*, const char*, const char*, const char*, sd_bus_message* reply, void*,
                   sd_bus_error*) {
    return sd_bus_message_append(reply, "as", 0);
}
int get_rate(sd_bus*, const char*, const char*, const char*, sd_bus_message* reply, void*,
             sd_bus_error*) {
    return sd_bus_message_append(reply, "d", 1.0);
}

int get_status(sd_bus*, const char*, const char*, const char*, sd_bus_message* reply,
               void* userdata, sd_bus_error*) {
    return sd_bus_message_append(reply, "s", impl_of(userdata)->playback_status());
}

int get_loop(sd_bus*, const char*, const char*, const char*, sd_bus_message* reply, void* userdata,
             sd_bus_error*) {
    const auto r = impl_of(userdata)->state.repeat;
    return sd_bus_message_append(reply, "s",
                                 r == sendspin::SendspinRepeatMode::ONE   ? "Track"
                                 : r == sendspin::SendspinRepeatMode::ALL ? "Playlist"
                                                                          : "None");
}

int get_shuffle(sd_bus*, const char*, const char*, const char*, sd_bus_message* reply,
                void* userdata, sd_bus_error*) {
    return sd_bus_message_append(reply, "b", impl_of(userdata)->state.shuffle ? 1 : 0);
}

int get_volume(sd_bus*, const char*, const char*, const char*, sd_bus_message* reply,
               void* userdata, sd_bus_error*) {
    const auto& s = impl_of(userdata)->state;
    return sd_bus_message_append(reply, "d", s.muted ? 0.0 : s.volume / 100.0);
}

int get_position(sd_bus*, const char*, const char*, const char*, sd_bus_message* reply,
                 void* userdata, sd_bus_error*) {
    return sd_bus_message_append(reply, "x", impl_of(userdata)->position_ms() * 1000);
}

int get_can(sd_bus*, const char*, const char*, const char* property, sd_bus_message* reply,
            void* userdata, sd_bus_error*) {
    const auto* impl = impl_of(userdata);
    bool v = false;
    if (strcmp(property, "CanGoNext") == 0) v = impl->supports(Cmd::NEXT);
    else if (strcmp(property, "CanGoPrevious") == 0) v = impl->supports(Cmd::PREVIOUS);
    else if (strcmp(property, "CanPlay") == 0) v = impl->supports(Cmd::PLAY);
    else if (strcmp(property, "CanPause") == 0) v = impl->supports(Cmd::PAUSE);
    else if (strcmp(property, "CanSeek") == 0)
        v = impl->supports(Cmd::SEEK) && impl->state.seek_max_ms.has_value();
    return sd_bus_message_append(reply, "b", v ? 1 : 0);
}

int append_entry(sd_bus_message* m, const char* key, char type, const void* value) {
    const char sig[2] = {type, '\0'};
    int r = sd_bus_message_open_container(m, 'e', "sv");
    if (r >= 0) r = sd_bus_message_append(m, "s", key);
    if (r >= 0) r = sd_bus_message_open_container(m, 'v', sig);
    if (r >= 0) r = sd_bus_message_append_basic(m, type, value);
    if (r >= 0) r = sd_bus_message_close_container(m);
    if (r >= 0) r = sd_bus_message_close_container(m);
    return r;
}

int append_strv_entry(sd_bus_message* m, const char* key, const std::string& value) {
    int r = sd_bus_message_open_container(m, 'e', "sv");
    if (r >= 0) r = sd_bus_message_append(m, "s", key);
    if (r >= 0) r = sd_bus_message_open_container(m, 'v', "as");
    if (r >= 0) r = sd_bus_message_append(m, "as", 1, value.c_str());
    if (r >= 0) r = sd_bus_message_close_container(m);
    if (r >= 0) r = sd_bus_message_close_container(m);
    return r;
}

int get_metadata(sd_bus*, const char*, const char*, const char*, sd_bus_message* reply,
                 void* userdata, sd_bus_error*) {
    const auto* impl = impl_of(userdata);
    const auto& s = impl->state;
    int r = sd_bus_message_open_container(reply, 'a', "{sv}");
    if (r < 0) return r;
    r = append_entry(reply, "mpris:trackid", 'o', impl->track_id.c_str());
    if (r >= 0 && s.duration_ms > 0) {
        const int64_t len = static_cast<int64_t>(s.duration_ms) * 1000;
        r = append_entry(reply, "mpris:length", 'x', &len);
    }
    const auto str = [&](const char* key, const std::optional<std::string>& v) {
        if (r >= 0 && v && !v->empty()) r = append_entry(reply, key, 's', v->c_str());
    };
    const auto strv = [&](const char* key, const std::optional<std::string>& v) {
        if (r >= 0 && v && !v->empty()) r = append_strv_entry(reply, key, *v);
    };
    if (s.connected) {
        str("xesam:title", s.title);
        strv("xesam:artist", s.artist);
        str("xesam:album", s.album);
        strv("xesam:albumArtist", s.album_artist);
        str("mpris:artUrl", s.art_url);
    }
    if (r < 0) return r;
    return sd_bus_message_close_container(reply);
}

// ---- property setters ------------------------------------------------------------------------

int set_loop(sd_bus*, const char*, const char*, const char*, sd_bus_message* value,
             void* userdata, sd_bus_error* error) {
    auto* impl = impl_of(userdata);
    const char* mode = nullptr;
    int r = sd_bus_message_read(value, "s", &mode);
    if (r < 0) return r;
    Cmd c;
    if (strcmp(mode, "None") == 0) c = Cmd::REPEAT_OFF;
    else if (strcmp(mode, "Track") == 0) c = Cmd::REPEAT_ONE;
    else if (strcmp(mode, "Playlist") == 0) c = Cmd::REPEAT_ALL;
    else return sd_bus_error_set(error, SD_BUS_ERROR_INVALID_ARGS, "unknown LoopStatus");
    impl->send(c);
    return 0;
}

int set_shuffle(sd_bus*, const char*, const char*, const char*, sd_bus_message* value,
                void* userdata, sd_bus_error*) {
    int on = 0;
    int r = sd_bus_message_read(value, "b", &on);
    if (r < 0) return r;
    impl_of(userdata)->send(on ? Cmd::SHUFFLE : Cmd::UNSHUFFLE);
    return 0;
}

int set_volume(sd_bus*, const char*, const char*, const char*, sd_bus_message* value,
               void* userdata, sd_bus_error*) {
    double v = 0;
    int r = sd_bus_message_read(value, "d", &v);
    if (r < 0) return r;
    sendspin::ClientCommandControllerObject o{};
    o.command = Cmd::VOLUME;
    o.volume = static_cast<uint8_t>(std::clamp(std::lround(v * 100.0), 0L, 100L));
    impl_of(userdata)->send(o);
    return 0;
}

// ---- methods ---------------------------------------------------------------------------------

int method_noop(sd_bus_message* m, void*, sd_bus_error*) {
    return sd_bus_reply_method_return(m, "");
}

int method_simple(sd_bus_message* m, void* userdata, sd_bus_error*) {
    auto* impl = impl_of(userdata);
    const char* member = sd_bus_message_get_member(m);
    if (strcmp(member, "Next") == 0) impl->send(Cmd::NEXT);
    else if (strcmp(member, "Previous") == 0) impl->send(Cmd::PREVIOUS);
    else if (strcmp(member, "Pause") == 0) impl->send(Cmd::PAUSE);
    else if (strcmp(member, "Play") == 0) impl->send(Cmd::PLAY);
    else if (strcmp(member, "Stop") == 0) impl->send(Cmd::STOP);
    else if (strcmp(member, "PlayPause") == 0)
        impl->send(impl->state.playing ? Cmd::PAUSE : Cmd::PLAY);
    return sd_bus_reply_method_return(m, "");
}

int method_seek(sd_bus_message* m, void* userdata, sd_bus_error*) {
    int64_t offset_us = 0;
    int r = sd_bus_message_read(m, "x", &offset_us);
    if (r < 0) return r;
    auto* impl = impl_of(userdata);
    if (impl->supports(Cmd::SEEK_RELATIVE)) {
        sendspin::ClientCommandControllerObject o{};
        o.command = Cmd::SEEK_RELATIVE;
        o.offset_ms = static_cast<int32_t>(
            std::clamp<int64_t>(offset_us / 1000, INT32_MIN, INT32_MAX));
        impl->send(o);
    } else if (impl->supports(Cmd::SEEK) && impl->state.seek_max_ms) {
        sendspin::ClientCommandControllerObject o{};
        o.command = Cmd::SEEK;
        o.position_ms = static_cast<uint32_t>(std::clamp<int64_t>(
            impl->position_ms() + offset_us / 1000, 0, *impl->state.seek_max_ms));
        impl->send(o);
    }
    return sd_bus_reply_method_return(m, "");
}

int method_set_position(sd_bus_message* m, void* userdata, sd_bus_error*) {
    const char* track = nullptr;
    int64_t pos_us = 0;
    int r = sd_bus_message_read(m, "ox", &track, &pos_us);
    if (r < 0) return r;
    auto* impl = impl_of(userdata);
    // The spec says to ignore a SetPosition for another track or outside the track.
    if (impl->track_id == track && pos_us >= 0 && impl->supports(Cmd::SEEK) &&
        impl->state.seek_max_ms && pos_us / 1000 <= *impl->state.seek_max_ms) {
        sendspin::ClientCommandControllerObject o{};
        o.command = Cmd::SEEK;
        o.position_ms = static_cast<uint32_t>(pos_us / 1000);
        impl->send(o);
    }
    return sd_bus_reply_method_return(m, "");
}

int method_open_uri(sd_bus_message* m, void*, sd_bus_error* error) {
    (void)m;
    return sd_bus_error_set(error, SD_BUS_ERROR_NOT_SUPPORTED, "OpenUri is not supported");
}

constexpr uint64_t CHANGE = SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE;
constexpr uint64_t CONST = SD_BUS_VTABLE_PROPERTY_CONST;

const sd_bus_vtable root_vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD("Raise", "", "", method_noop, 0),
    SD_BUS_METHOD("Quit", "", "", method_noop, 0),
    SD_BUS_PROPERTY("CanQuit", "b", get_bool_false, 0, CONST),
    SD_BUS_PROPERTY("CanRaise", "b", get_bool_false, 0, CONST),
    SD_BUS_PROPERTY("HasTrackList", "b", get_bool_false, 0, CONST),
    SD_BUS_PROPERTY("Identity", "s", get_identity, 0, CONST),
    SD_BUS_PROPERTY("SupportedUriSchemes", "as", get_empty_strv, 0, CONST),
    SD_BUS_PROPERTY("SupportedMimeTypes", "as", get_empty_strv, 0, CONST),
    SD_BUS_VTABLE_END,
};

const sd_bus_vtable player_vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD("Next", "", "", method_simple, 0),
    SD_BUS_METHOD("Previous", "", "", method_simple, 0),
    SD_BUS_METHOD("Pause", "", "", method_simple, 0),
    SD_BUS_METHOD("PlayPause", "", "", method_simple, 0),
    SD_BUS_METHOD("Stop", "", "", method_simple, 0),
    SD_BUS_METHOD("Play", "", "", method_simple, 0),
    SD_BUS_METHOD("Seek", "x", "", method_seek, 0),
    SD_BUS_METHOD("SetPosition", "ox", "", method_set_position, 0),
    SD_BUS_METHOD("OpenUri", "s", "", method_open_uri, 0),
    SD_BUS_SIGNAL("Seeked", "x", 0),
    SD_BUS_PROPERTY("PlaybackStatus", "s", get_status, 0, CHANGE),
    SD_BUS_WRITABLE_PROPERTY("LoopStatus", "s", get_loop, set_loop, 0, CHANGE),
    SD_BUS_PROPERTY("Rate", "d", get_rate, 0, CONST),
    SD_BUS_PROPERTY("MinimumRate", "d", get_rate, 0, CONST),
    SD_BUS_PROPERTY("MaximumRate", "d", get_rate, 0, CONST),
    SD_BUS_WRITABLE_PROPERTY("Shuffle", "b", get_shuffle, set_shuffle, 0, CHANGE),
    SD_BUS_PROPERTY("Metadata", "a{sv}", get_metadata, 0, CHANGE),
    SD_BUS_WRITABLE_PROPERTY("Volume", "d", get_volume, set_volume, 0, CHANGE),
    SD_BUS_PROPERTY("Position", "x", get_position, 0, 0),
    SD_BUS_PROPERTY("CanGoNext", "b", get_can, 0, CHANGE),
    SD_BUS_PROPERTY("CanGoPrevious", "b", get_can, 0, CHANGE),
    SD_BUS_PROPERTY("CanPlay", "b", get_can, 0, CHANGE),
    SD_BUS_PROPERTY("CanPause", "b", get_can, 0, CHANGE),
    SD_BUS_PROPERTY("CanSeek", "b", get_can, 0, CHANGE),
    SD_BUS_PROPERTY("CanControl", "b", get_bool_true, 0, CONST),
    SD_BUS_VTABLE_END,
};

}  // namespace

void Mpris::Impl::apply(MprisState next) {
    // Work out what changed against what MPRIS last showed, then switch to the new state.
    const MprisState& prev = state;
    const bool track_changed = next.connected != prev.connected || next.title != prev.title ||
                               next.artist != prev.artist || next.album != prev.album;
    const bool meta_changed = track_changed || next.album_artist != prev.album_artist ||
                              next.art_url != prev.art_url ||
                              next.duration_ms != prev.duration_ms;
    const int64_t predicted = position_ms();
    const std::string old_status = playback_status();
    const bool caps_changed = next.has_controller != prev.has_controller ||
                              next.supported != prev.supported ||
                              next.seek_max_ms.has_value() != prev.seek_max_ms.has_value();
    const bool loop_changed = next.repeat != prev.repeat;
    const bool shuffle_changed = next.shuffle != prev.shuffle;
    const bool volume_changed = next.volume != prev.volume || next.muted != prev.muted;

    state = std::move(next);
    if (track_changed) {
        track_id = state.connected && state.title
                       ? "/org/sendspin/track/" + std::to_string(++track_serial)
                       : std::string(NO_TRACK);
    }
    const bool status_changed = old_status != playback_status();

    std::vector<const char*> names;
    if (status_changed) names.push_back("PlaybackStatus");
    if (meta_changed) names.push_back("Metadata");
    if (loop_changed) names.push_back("LoopStatus");
    if (shuffle_changed) names.push_back("Shuffle");
    if (volume_changed) names.push_back("Volume");
    if (caps_changed) {
        for (const char* n : {"CanGoNext", "CanGoPrevious", "CanPlay", "CanPause", "CanSeek"})
            names.push_back(n);
    }
    if (!names.empty()) {
        names.push_back(nullptr);
        int r = sd_bus_emit_properties_changed_strv(bus, PATH, PLAYER_IFACE,
                                                    const_cast<char**>(names.data()));
        if (r < 0) pw_log_warn("sendspin: MPRIS PropertiesChanged: %s", strerror(-r));
    }
    // Position is not signalled; a jump that playback alone does not explain is a seek.
    if (!track_changed && state.connected &&
        std::llabs(position_ms() - predicted) > SEEK_DETECT_MS) {
        sd_bus_emit_signal(bus, PATH, PLAYER_IFACE, "Seeked", "x", position_ms() * 1000);
    }
}

void Mpris::Impl::loop() {
    while (running.load()) {
        int r;
        while ((r = sd_bus_process(bus, nullptr)) > 0) {
        }
        if (r < 0) {
            pw_log_warn("sendspin: MPRIS bus error: %s", strerror(-r));
            break;
        }
        std::optional<MprisState> next;
        {
            std::lock_guard<std::mutex> lock(mutex);
            next.swap(pending);
        }
        if (next) {
            apply(std::move(*next));
            continue;  // flush what apply() queued
        }

        pollfd fds[2] = {};
        fds[0].fd = sd_bus_get_fd(bus);
        fds[0].events = static_cast<short>(sd_bus_get_events(bus));
        fds[1].fd = wake_fd;
        fds[1].events = POLLIN;
        int timeout_ms = 1000;
        uint64_t until = 0;
        if (sd_bus_get_timeout(bus, &until) >= 0 && until != UINT64_MAX) {
            const int64_t left = (static_cast<int64_t>(until) - monotonic_us()) / 1000;
            timeout_ms = static_cast<int>(std::clamp<int64_t>(left, 0, 1000));
        }
        if (poll(fds, 2, timeout_ms) > 0 && (fds[1].revents & POLLIN)) {
            uint64_t v;
            (void)!read(wake_fd, &v, sizeof(v));
        }
    }
}

Mpris::Mpris() : impl_(std::make_unique<Impl>()) {}
Mpris::~Mpris() { stop(); }

bool Mpris::available() { return true; }

bool Mpris::start(const std::string& instance, CommandFn command) {
    auto& i = *impl_;
    i.command = std::move(command);
    int r = sd_bus_open_user(&i.bus);
    if (r < 0) {
        pw_log_warn("sendspin: MPRIS: no session bus: %s", strerror(-r));
        return false;
    }
    r = sd_bus_add_object_vtable(i.bus, &i.slot_root, PATH, ROOT_IFACE, root_vtable, &i);
    if (r >= 0)
        r = sd_bus_add_object_vtable(i.bus, &i.slot_player, PATH, PLAYER_IFACE, player_vtable, &i);
    if (r >= 0) {
        r = sd_bus_request_name(i.bus, BUS_NAME, 0);
        if (r < 0) {
            // Another Sendspin module owns the plain name: MPRIS allows an instance suffix.
            std::string name = std::string(BUS_NAME) + ".instance" + std::to_string(getpid());
            std::string suffix;
            for (char c : instance)
                suffix += (isalnum(static_cast<unsigned char>(c)) || c == '_') ? c : '_';
            if (!suffix.empty()) name += "_" + suffix;
            r = sd_bus_request_name(i.bus, name.c_str(), 0);
            if (r >= 0) pw_log_info("sendspin: MPRIS name %s", name.c_str());
        }
    }
    if (r >= 0) {
        i.wake_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
        if (i.wake_fd < 0) r = -errno;
    }
    if (r < 0) {
        pw_log_warn("sendspin: MPRIS setup failed: %s", strerror(-r));
        stop();
        return false;
    }
    i.running.store(true);
    i.thread = std::thread([&i] { i.loop(); });
    return true;
}

void Mpris::stop() {
    auto& i = *impl_;
    if (i.thread.joinable()) {
        i.running.store(false);
        const uint64_t one = 1;
        (void)!write(i.wake_fd, &one, sizeof(one));
        i.thread.join();
    }
    i.slot_player = sd_bus_slot_unref(i.slot_player);
    i.slot_root = sd_bus_slot_unref(i.slot_root);
    if (i.bus != nullptr) {
        sd_bus_flush(i.bus);
        i.bus = sd_bus_flush_close_unref(i.bus);
    }
    if (i.wake_fd >= 0) {
        close(i.wake_fd);
        i.wake_fd = -1;
    }
}

void Mpris::update(MprisState state) {
    auto& i = *impl_;
    if (!i.running.load()) return;
    {
        std::lock_guard<std::mutex> lock(i.mutex);
        i.pending = std::move(state);
    }
    const uint64_t one = 1;
    (void)!write(i.wake_fd, &one, sizeof(one));
}

}  // namespace pwsendspin

#else  // !HAVE_SDBUS

namespace pwsendspin {

struct Mpris::Impl {};
Mpris::Mpris() : impl_(std::make_unique<Impl>()) {}
Mpris::~Mpris() = default;
bool Mpris::available() { return false; }
bool Mpris::start(const std::string&, CommandFn) { return false; }
void Mpris::stop() {}
void Mpris::update(MprisState) {}

}  // namespace pwsendspin

#endif
