// SPDX-License-Identifier: Apache-2.0
//
// MPRIS (org.mpris.MediaPlayer2) on the session bus: shows what the Sendspin server plays (title,
// artist, album, cover, position) in the desktop's media controls, and turns their play/pause/
// next/previous/seek/volume/loop/shuffle into controller@v1 commands.

#pragma once

#include <sendspin/controller_role.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace pwsendspin {

/// Everything MPRIS shows, as one snapshot.
struct MprisState {
    bool connected{false};
    bool playing{false};

    std::optional<std::string> title, artist, album, album_artist, art_url;
    uint32_t duration_ms{0};  // 0 = unknown / live
    uint32_t progress_ms{0};
    int64_t progress_at_us{0};  // CLOCK_MONOTONIC time progress_ms was sampled at

    bool has_controller{false};
    std::vector<sendspin::SendspinControllerCommand> supported;
    uint8_t volume{100};
    bool muted{false};
    sendspin::SendspinRepeatMode repeat{sendspin::SendspinRepeatMode::OFF};
    bool shuffle{false};
    std::optional<uint32_t> seek_max_ms;
};

class Mpris {
public:
    /// Sends a controller command; called on the MPRIS thread.
    using CommandFn = std::function<bool(const sendspin::ClientCommandControllerObject&)>;

    Mpris();
    ~Mpris();

    /// Whether MPRIS support was compiled in.
    static bool available();

    /// Connects to the session bus and starts serving. `instance` distinguishes several modules
    /// on one bus when the plain name is taken.
    bool start(const std::string& instance, CommandFn command);
    void stop();

    /// Publishes a new state (any thread).
    void update(MprisState state);

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

}  // namespace pwsendspin
