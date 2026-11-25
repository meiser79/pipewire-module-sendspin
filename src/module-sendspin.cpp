// SPDX-License-Identifier: Apache-2.0
//
// libpipewire-module-sendspin
//
// Runs a Sendspin client (https://github.com/Sendspin) inside the PipeWire daemon. Each role can
// be enabled on its own:
//
//   player@v1    Audio a Sendspin server (Music Assistant) sends is decoded, clock-synchronised
//                and played through a playback stream in the PipeWire graph. Server volume/mute
//                and the stream's volume/mute in the mixer are kept in sync both ways.
//   metadata@v1  Title/artist/album of what the server plays are published as media.* properties
//                on that playback stream (pavucontrol, wpctl, desktop mixers show them) and in the
//                media controls. On whenever the player or MPRIS is.
//   source@v1    A capture stream (default input, a chosen node, or the default sink's monitor)
//                is offered to the server as a Live Input. Optional line-sense reports when a
//                signal is present so the server can start playback automatically.
//
// Threads:
//   PipeWire main loop    module init/destroy, stream creation/destruction, property updates,
//                         volume changes coming from the graph.
//   PipeWire data loop    the two process() callbacks. Never locks, never allocates.
//   sendspin thread       owns the SendspinClient: loop(), role callbacks, sending source audio,
//                         reporting playback progress.
//   library threads       protocol task, WebSocket I/O, the player's sync task (on_audio_output())
//                         and the source task that chunks/encodes captured audio.
//
// Work crosses from the sendspin thread to the main loop through a mutex-protected task list and
// a pw_loop event, and the other way through a second task list drained by the sendspin thread.
// Audio crosses to/from the data loop through wait-free SPSC rings (spsc.h).

#include "mdns.h"
#include "mpris.h"
#include "spsc.h"
#include "state_store.h"

#include <sendspin/client.h>
#include <sendspin/controller_role.h>
#include <sendspin/metadata_role.h>
#include <sendspin/player_role.h>
#include <sendspin/source_role.h>

#include <pipewire/impl.h>
#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/props.h>
#include <spa/utils/keys.h>
#include <spa/utils/result.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <cstdarg>
#include <fcntl.h>
#include <unistd.h>

PW_LOG_TOPIC_STATIC(mod_topic, "mod.sendspin");
#define PW_LOG_TOPIC_DEFAULT mod_topic

namespace pwsendspin {
namespace {

// ----------------------------------------------------------------------------------------------
// Constants
// ----------------------------------------------------------------------------------------------

constexpr const char* NAME = "sendspin";
constexpr const char* SENDSPIN_PATH = "/sendspin";

constexpr int LOOP_INTERVAL_MS = 5;
constexpr int64_t MPRIS_INTERVAL_US = 250000;

// Player PCM ring. Writes from the decoder are cut at multiples of 24 bytes when they have to be
// split: 24 is a multiple of every frame size this module advertises (1-2 channels x 2/3/4 bytes),
// so a partial write always ends on a frame boundary, whatever the current stream format is.
constexpr size_t PCM_ALIGN = 24;
constexpr size_t PLAYER_RING_CAPACITY = PCM_ALIGN * 43690;  // ~1 MiB
constexpr uint32_t PLAYER_RING_MS = 250;

// Source capture: 48 kHz / 16-bit / stereo, what Music Assistant Live Inputs use. The library cuts
// it into 20 ms chunks.
constexpr uint32_t SOURCE_RATE = 48000;
constexpr uint8_t SOURCE_CHANNELS = 2;
constexpr uint8_t SOURCE_BITS = 16;
constexpr size_t SOURCE_BPF = SOURCE_CHANNELS * (SOURCE_BITS / 8);
constexpr uint32_t SOURCE_SLOT_FRAMES = 8192;  // largest PipeWire quantum
constexpr int64_t SOURCE_REANCHOR_US = 50000;

// ----------------------------------------------------------------------------------------------
// Module arguments
// ----------------------------------------------------------------------------------------------

constexpr const char* KEY_NAME = "sendspin.name";
constexpr const char* KEY_CLIENT_ID = "sendspin.client-id";
constexpr const char* KEY_PORT = "sendspin.port";
constexpr const char* KEY_MDNS = "sendspin.mdns";
constexpr const char* KEY_STATE_FILE = "sendspin.state-file";
constexpr const char* KEY_UNPAIRED = "sendspin.unpaired-access";
constexpr const char* KEY_LOG_LEVEL = "sendspin.log-level";
constexpr const char* PROP_PAIRING_TOKEN = "sendspin.pairing-token";
constexpr const char* KEY_PLAYER = "sendspin.player";
constexpr const char* KEY_PLAYER_CODECS = "sendspin.player.codecs";
constexpr const char* KEY_PLAYER_DELAY = "sendspin.player.static-delay-ms";
constexpr const char* KEY_PLAYER_PROPS = "player.props";
constexpr const char* KEY_SOURCE = "sendspin.source";
constexpr const char* KEY_SOURCE_CODEC = "sendspin.source.codec";
constexpr const char* KEY_SOURCE_CAPTURE_SINK = "sendspin.source.capture-sink";
constexpr const char* KEY_SOURCE_VIRTUAL_SINK = "sendspin.source.virtual-sink";
constexpr const char* KEY_SOURCE_LINE_SENSE = "sendspin.source.line-sense";
constexpr const char* KEY_SOURCE_THRESHOLD = "sendspin.source.line-sense.threshold-db";
constexpr const char* KEY_SOURCE_ATTACK = "sendspin.source.line-sense.attack-ms";
constexpr const char* KEY_SOURCE_RELEASE = "sendspin.source.line-sense.release-ms";
constexpr const char* KEY_SOURCE_PROPS = "source.props";
constexpr const char* KEY_MPRIS = "sendspin.mpris";

const spa_dict_item module_info[] = {
    {PW_KEY_MODULE_AUTHOR, "pipewire-module-sendspin contributors"},
    {PW_KEY_MODULE_DESCRIPTION,
     "Sendspin client: player@v1/metadata@v1 playback and source@v1 capture"},
    {PW_KEY_MODULE_USAGE,
     "( sendspin.name=<friendly name> ) "
     "( sendspin.client-id=<stable id> ) "
     "( sendspin.port=<listen port, default 8928> ) "
     "( sendspin.mdns=<bool, default true> ) "
     "( sendspin.state-file=<path> ) "
     "( sendspin.unpaired-access=<bool> ) "
     "( sendspin.log-level=none|error|warn|info|debug|verbose ) "
     "( sendspin.player=<bool, default true> ) "
     "( sendspin.mpris=<bool, default true> ) "
     "( sendspin.player.codecs=<flac,opus,pcm> ) "
     "( sendspin.player.static-delay-ms=<ms> ) "
     "( player.props={ ... } ) "
     "( sendspin.source=<bool, default true> ) "
     "( sendspin.source.codec=pcm|flac|opus ) "
     "( sendspin.source.capture-sink=<bool, default false> ) "
     "( sendspin.source.virtual-sink=<bool, default true unless capture-sink> ) "
     "( sendspin.source.line-sense=<bool, default false> ) "
     "( sendspin.source.line-sense.threshold-db=<dBFS, default -50> ) "
     "( sendspin.source.line-sense.attack-ms=<ms, default 300> ) "
     "( sendspin.source.line-sense.release-ms=<ms, default 5000> ) "
     "( source.props={ ... } )"},
    {PW_KEY_MODULE_VERSION, MODULE_VERSION},
};

// ----------------------------------------------------------------------------------------------
// Helpers
// ----------------------------------------------------------------------------------------------

__attribute__((format(printf, 2, 3))) void ss_log(sendspin::LogLevel level, const char* fmt, ...);

/// The module's own log level (sendspin.log-level). sendspin-cpp has a separate, library-wide
/// one: see library_log_level().
std::atomic<sendspin::LogLevel> module_log_level{sendspin::LogLevel::INFO};

/// sendspin-cpp logs straight to stderr and reports routine events at info and warning level, so
/// below debug it gets errors only.
sendspin::LogLevel library_log_level(sendspin::LogLevel level) {
    using L = sendspin::LogLevel;
    if (static_cast<int>(level) >= static_cast<int>(L::DEBUG)) return level;
    return static_cast<int>(level) < static_cast<int>(L::ERROR) ? level : L::ERROR;
}

/// CLOCK_MONOTONIC in microseconds: the clock pw_time::now is expressed in.
int64_t monotonic_us() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000000 + ts.tv_nsec / 1000;
}

/// Converts a CLOCK_MONOTONIC time to sendspin-cpp's clock (CLOCK_MONOTONIC_RAW on Linux). The two
/// run at slightly different rates (NTP slews only CLOCK_MONOTONIC), so their offset drifts and is
/// measured afresh at every conversion. Both are vDSO reads, safe on the data loop.
int64_t to_client_time(int64_t mono_us) {
    return mono_us + (sendspin::SendspinClient::now_us() - monotonic_us());
}

bool parse_bool(const char* v, bool fallback) {
    if (v == nullptr) return fallback;
    return pw_properties_parse_bool(v);
}

template <typename T>
T parse_num(const char* v, T fallback, T lo, T hi, const char* key) {
    if (v == nullptr) return fallback;
    char* end = nullptr;
    errno = 0;
    const double d = strtod(v, &end);
    if (errno != 0 || end == v || *end != '\0' || d < static_cast<double>(lo) ||
        d > static_cast<double>(hi)) {
        pw_log_warn("invalid %s \"%s\", using default", key, v);
        return fallback;
    }
    return static_cast<T>(d);
}

std::string read_machine_id() {
    for (const char* p : {"/etc/machine-id", "/var/lib/dbus/machine-id"}) {
        FILE* f = fopen(p, "re");
        if (f == nullptr) continue;
        char buf[64] = {};
        const bool ok = fgets(buf, sizeof(buf), f) != nullptr;
        fclose(f);
        if (!ok) continue;
        std::string s(buf);
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
        if (!s.empty()) return s;
    }
    return {};
}

std::string hostname() {
    char buf[256] = {};
    if (gethostname(buf, sizeof(buf) - 1) == 0 && buf[0] != '\0') return buf;
    return "PipeWire";
}

std::string sanitize_filename(const std::string& in) {
    std::string out;
    for (char c : in) {
        out += (isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == '.') ? c
                                                                                           : '_';
    }
    return out.empty() ? "default" : out;
}

std::string default_state_path(const std::string& client_id) {
    std::string base;
    if (const char* x = getenv("XDG_STATE_HOME"); x != nullptr && x[0] == '/') {
        base = x;
    } else if (const char* h = getenv("HOME"); h != nullptr && h[0] == '/') {
        base = std::string(h) + "/.local/state";
    } else {
        return {};
    }
    return base + "/pipewire-module-sendspin/" + sanitize_filename(client_id) + ".state";
}

// Sendspin volume is 0..100 on a perceptual scale; PipeWire stream volumes are linear gains.
// Use the same cubic mapping as pavucontrol/wpctl so "50 %" means the same in both places.
float volume_to_linear(uint8_t v) {
    const float x = static_cast<float>(std::min<uint8_t>(v, 100)) / 100.0f;
    return x * x * x;
}
uint8_t linear_to_volume(float lin) {
    if (!(lin > 0.0f)) return 0;
    const float v = std::cbrt(lin) * 100.0f;
    return static_cast<uint8_t>(std::clamp(std::lround(v), 0L, 100L));
}

spa_audio_format spa_format_for_bits(uint8_t bits) {
    switch (bits) {
        case 16: return SPA_AUDIO_FORMAT_S16_LE;
        case 24: return SPA_AUDIO_FORMAT_S24_LE;  // packed 3-byte, as the decoder emits it
        case 32: return SPA_AUDIO_FORMAT_S32_LE;
        default: return SPA_AUDIO_FORMAT_UNKNOWN;
    }
}

const spa_pod* build_format(spa_pod_builder* b, spa_audio_format fmt, uint32_t rate,
                            uint32_t channels) {
    spa_audio_info_raw info{};
    info.format = fmt;
    info.rate = rate;
    info.channels = channels;
    if (channels == 1) {
        info.position[0] = SPA_AUDIO_CHANNEL_MONO;
    } else if (channels == 2) {
        info.position[0] = SPA_AUDIO_CHANNEL_FL;
        info.position[1] = SPA_AUDIO_CHANNEL_FR;
    } else {
        info.flags |= SPA_AUDIO_FLAG_UNPOSITIONED;
    }
    return spa_format_audio_raw_build(b, SPA_PARAM_EnumFormat, &info);
}

/// Microseconds until the next sample handed to (playback) or the last sample taken from
/// (capture) the stream reaches/left the device, from pw_time.
int64_t pw_time_delay_us(const pw_time& t) {
    if (t.rate.denom == 0) return 0;
    return static_cast<int64_t>(t.delay) * 1000000LL * static_cast<int64_t>(t.rate.num) /
           static_cast<int64_t>(t.rate.denom);
}

// ----------------------------------------------------------------------------------------------
// Data structures
// ----------------------------------------------------------------------------------------------

struct Impl;

struct PlaybackStream {
    Impl* impl{nullptr};
    pw_stream* stream{nullptr};
    spa_hook listener{};
    uint32_t rate{0};
    uint8_t channels{0};
    uint8_t bits{0};
    size_t bpf{0};
    bool volume_applied{false};  // ignore the graph's volume until ours has been set once
};

struct CaptureStream {
    Impl* impl{nullptr};
    pw_stream* stream{nullptr};
    spa_hook listener{};
};

struct PlayedEvent {
    uint32_t frames;
    int64_t finish_us;
};

struct CaptureSlot {
    uint32_t frames;
    bool discontinuity;
    int64_t first_us;
    uint8_t data[SOURCE_SLOT_FRAMES * SOURCE_BPF];
};

struct Metadata {
    std::optional<std::string> title, artist, album, album_artist, artwork_url;
};

struct Config {
    std::string name;
    std::string client_id;
    uint16_t port{sendspin::SendspinClientConfig::DEFAULT_SERVER_PORT};
    bool mdns{true};
    std::string state_file;
    std::optional<bool> unpaired_access;
    sendspin::LogLevel log_level{sendspin::LogLevel::INFO};

    bool player{true};
    bool metadata{true};
    bool mpris{true};
    std::vector<sendspin::SendspinCodecFormat> codecs{sendspin::SendspinCodecFormat::FLAC,
                                                      sendspin::SendspinCodecFormat::OPUS,
                                                      sendspin::SendspinCodecFormat::PCM};
    uint16_t static_delay_ms{0};
    std::string player_props;

    bool source{true};
    sendspin::SendspinCodecFormat source_codec{sendspin::SendspinCodecFormat::PCM};
    bool capture_sink{false};
    bool virtual_sink{true};
    bool line_sense{false};
    double threshold_db{-50.0};
    uint32_t attack_ms{300};
    uint32_t release_ms{5000};
    std::string source_props;
};

// ----------------------------------------------------------------------------------------------
// Listeners handed to sendspin-cpp
// ----------------------------------------------------------------------------------------------

struct PlayerListener final : sendspin::PlayerRoleListener {
    Impl* impl;
    explicit PlayerListener(Impl* i) : impl(i) {}
    size_t on_audio_output(std::span<const uint8_t> pcm, const sendspin::AudioFormat& format,
                           uint32_t timeout_ms) override;
    void on_stream_start() override;
    void on_stream_end() override;
    void on_volume_changed(uint8_t volume) override;
    void on_mute_changed(bool muted) override;
    void on_output_delay_changed(uint16_t delay_ms) override;

    // Sync task only: the format the playback stream was last requested in.
    sendspin::AudioFormat out_format{};
};

struct MetadataListener final : sendspin::MetadataRoleListener {
    Impl* impl;
    explicit MetadataListener(Impl* i) : impl(i) {}
    void on_track_metadata(const sendspin::TrackMetadata& m) override;
    void on_metadata_clear() override;
};

struct SourceListener final : sendspin::SourceRoleListener {
    Impl* impl;
    explicit SourceListener(Impl* i) : impl(i) {}
    void on_streaming_started() override;
    void on_streaming_stopped() override;
};

struct NetworkProvider final : sendspin::SendspinNetworkProvider {
    bool is_network_ready() override { return true; }
};

// ----------------------------------------------------------------------------------------------
// Module state
// ----------------------------------------------------------------------------------------------

struct Impl {
    Config cfg;

    pw_impl_module* module{nullptr};
    pw_context* context{nullptr};
    pw_loop* main_loop{nullptr};
    pw_core* core{nullptr};
    spa_hook core_listener{};
    spa_hook core_proxy_listener{};
    bool core_gone{false};
    bool own_core{false};    // we connected it, so we disconnect it
    bool core_hooks{false};  // core_listener/core_proxy_listener are linked
    spa_hook module_listener{};

    // main-loop task queue
    spa_source* main_event{nullptr};
    std::mutex main_mutex;
    std::deque<std::function<void()>> main_tasks;

    // sendspin-thread task queue
    std::mutex ss_mutex;
    std::deque<std::function<void()>> ss_tasks;

    std::thread ss_thread;
    std::atomic<bool> running{false};

    std::unique_ptr<StateStore> store;
    std::unique_ptr<MdnsAdvertiser> mdns;
    std::unique_ptr<Mpris> mpris;
    sendspin::ControllerRole* controller{nullptr};  // used from the MPRIS thread (send_command)
    sendspin::MetadataRole* metadata_role{nullptr};
    int64_t ss_mpris_due_us{0};  // sendspin thread
    NetworkProvider network;
    std::unique_ptr<PlayerListener> player_listener;
    std::unique_ptr<MetadataListener> metadata_listener;
    std::unique_ptr<SourceListener> source_listener;
    std::unique_ptr<sendspin::SendspinClient> client;
    sendspin::PlayerRole* player{nullptr};
    sendspin::SourceRole* source{nullptr};

    // ---- player ----
    ByteRing player_ring{PLAYER_RING_CAPACITY};
    std::atomic<size_t> player_ring_limit{PLAYER_RING_CAPACITY};
    std::atomic<uint64_t> player_flush_upto{0};
    std::atomic<bool> player_format_reset{true};  // next write (re)creates the playback stream
    SpscQueue<PlayedEvent, 512> played;
    PlaybackStream* playback{nullptr};  // main loop only
    std::atomic<uint8_t> volume{100};
    std::atomic<bool> muted{false};

    // ---- metadata (written on sendspin thread, read on main loop) ----
    std::mutex meta_mutex;
    Metadata meta;

    // ---- source ----
    CaptureStream* capture{nullptr};  // main loop only
    std::atomic<bool> source_streaming{false};
    std::string server_name;      // main loop: names the virtual sink
    std::string ss_server_name;   // sendspin thread: last name seen from the server
    std::string ss_connection_summary;  // sendspin thread: last logged connection state
    std::unique_ptr<SpscQueue<CaptureSlot, 32>> capture_slots;
    std::atomic<bool> capture_overflow{false};
    // sendspin-thread only:
    bool src_anchored{false};
    int64_t src_anchor_us{0};
    uint64_t src_frames_since_anchor{0};  // frames written since the anchor
    bool signal_present{false};
    int candidate_signal{-1};
    int64_t candidate_since_us{0};

    // --------------------------------------------------------------------------------------
    void post_main(std::function<void()> fn) {
        {
            std::lock_guard<std::mutex> lock(main_mutex);
            main_tasks.push_back(std::move(fn));
        }
        if (main_event != nullptr) pw_loop_signal_event(main_loop, main_event);
    }

    void post_ss(std::function<void()> fn) {
        std::lock_guard<std::mutex> lock(ss_mutex);
        ss_tasks.push_back(std::move(fn));
    }

    // main loop
    void run_main_tasks() {
        std::deque<std::function<void()>> tasks;
        {
            std::lock_guard<std::mutex> lock(main_mutex);
            tasks.swap(main_tasks);
        }
        for (auto& t : tasks) t();
    }

    // sendspin thread
    void run_ss_tasks() {
        std::deque<std::function<void()>> tasks;
        {
            std::lock_guard<std::mutex> lock(ss_mutex);
            tasks.swap(ss_tasks);
        }
        for (auto& t : tasks) t();
    }

    // ---- main loop: playback stream ----
    void ensure_playback(uint32_t rate, uint8_t channels, uint8_t bits);
    void destroy_playback();
    void apply_volume_to_stream();
    void apply_metadata_to_stream();
    void on_graph_volume(uint8_t vol, std::optional<bool> mute);

    // ---- main loop: capture stream ----
    void update_capture();
    void create_capture();
    void destroy_capture();
    std::string sink_description() const;
    void set_server_name(const std::string& name);

    // ---- sendspin thread ----
    void ss_main();
    void drain_played();
    void log_connection_changes();
    void publish_mpris();
    void pump_source();
    void line_sense(const int16_t* samples, size_t count);
};

// ==============================================================================================
// Player: sendspin side
// ==============================================================================================

size_t PlayerListener::on_audio_output(std::span<const uint8_t> pcm,
                                       const sendspin::AudioFormat& format, uint32_t timeout_ms) {
    // Runs on the library's sync task. `pcm` is a whole number of frames in `format`.
    const uint8_t bits = format.bits_per_sample;
    if (spa_format_for_bits(bits) == SPA_AUDIO_FORMAT_UNKNOWN || format.channels == 0 ||
        format.channels > 2 || format.sample_rate == 0) {
        // Not playable here (the advertised formats exclude it): count it as played so the
        // library's timeline does not stall on it.
        return pcm.size();
    }
    if (impl->player_format_reset.exchange(false) || format != out_format) {
        // The output follows the writes, not stream/start: a format change applies from the
        // first audio after it. Anything still queued in the old format is dropped by
        // ensure_playback().
        out_format = format;
        const size_t bpf = static_cast<size_t>(format.channels) * (bits / 8);
        size_t limit = static_cast<size_t>(format.sample_rate) * bpf * PLAYER_RING_MS / 1000;
        limit = std::clamp<size_t>(limit / PCM_ALIGN * PCM_ALIGN, PCM_ALIGN * 64,
                                   PLAYER_RING_CAPACITY);
        impl->player_ring_limit.store(limit);
        impl->post_main([i = impl, f = format] {
            i->ensure_playback(f.sample_rate, f.channels, f.bits_per_sample);
        });
    }

    const int64_t deadline = monotonic_us() + static_cast<int64_t>(timeout_ms) * 1000;
    for (;;) {
        const size_t avail = impl->player_ring.writable(impl->player_ring_limit.load());
        size_t n = 0;
        if (pcm.size() <= avail) {
            n = pcm.size();
        } else {
            n = (avail / PCM_ALIGN) * PCM_ALIGN;
        }
        if (n > 0) {
            impl->player_ring.write(pcm.data(), n);
            return n;
        }
        if (!impl->running.load(std::memory_order_relaxed) || monotonic_us() >= deadline) return 0;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

void PlayerListener::on_stream_start() {
    if (const auto info = impl->player->stream_info()) {
        static constexpr const char* CODECS[] = {"flac", "opus", "pcm", "?"};
        ss_log(sendspin::LogLevel::DEBUG, "player: stream started, %s %u Hz / %u ch / %u bit",
               CODECS[std::min<size_t>(static_cast<size_t>(info->codec), 3)], info->sample_rate,
               info->channels, info->bit_depth);
    }
}

void PlayerListener::on_stream_end() {
    ss_log(sendspin::LogLevel::DEBUG, "player: stream ended");
    // Everything still queued belongs to the stream that just ended.
    impl->player_flush_upto.store(impl->player_ring.write_position());
    impl->player_format_reset.store(true);
    impl->post_main([i = impl] { i->destroy_playback(); });
}

void PlayerListener::on_volume_changed(uint8_t volume) {
    ss_log(sendspin::LogLevel::DEBUG, "player: server set volume %u", volume);
    impl->volume.store(volume);
    impl->store->set("player.volume", std::to_string(volume));
    impl->post_main([i = impl] { i->apply_volume_to_stream(); });
}

void PlayerListener::on_mute_changed(bool muted) {
    ss_log(sendspin::LogLevel::DEBUG, "player: server set mute %s", muted ? "on" : "off");
    impl->muted.store(muted);
    impl->store->set("player.muted", muted ? "1" : "0");
    impl->post_main([i = impl] { i->apply_volume_to_stream(); });
}

void PlayerListener::on_output_delay_changed(uint16_t delay_ms) {
    ss_log(sendspin::LogLevel::DEBUG, "player: output delay %u ms", delay_ms);
}

// ==============================================================================================
// Player: PipeWire side (main loop + data loop)
// ==============================================================================================

void playback_process(void* data) {
    // Realtime data thread.
    auto* ps = static_cast<PlaybackStream*>(data);
    Impl* impl = ps->impl;
    pw_buffer* b = pw_stream_dequeue_buffer(ps->stream);
    if (b == nullptr) return;
    spa_data& d = b->buffer->datas[0];
    auto* dst = static_cast<uint8_t*>(d.data);
    if (dst == nullptr) {
        pw_stream_queue_buffer(ps->stream, b);
        return;
    }

    impl->player_ring.discard_until(impl->player_flush_upto.load(std::memory_order_acquire));

    size_t frames = d.maxsize / ps->bpf;
    if (b->requested != 0) frames = std::min<size_t>(frames, b->requested);
    const size_t avail = impl->player_ring.readable() / ps->bpf;
    const size_t real = std::min(frames, avail);
    if (real > 0) impl->player_ring.read(dst, real * ps->bpf);
    if (real < frames) memset(dst + real * ps->bpf, 0, (frames - real) * ps->bpf);

    if (real > 0) {
        // When will the last frame of this buffer have been played? `delay` covers the graph and
        // device latency ahead of the next sample we hand over; `queued`/`buffered` are frames
        // already ahead of this buffer in the stream.
        pw_time t{};
        if (pw_stream_get_time_n(ps->stream, &t, sizeof(t)) == 0 && t.rate.denom != 0) {
            const int64_t now_us = t.now > 0 ? t.now / 1000 : monotonic_us();
            const int64_t ahead_us =
                pw_time_delay_us(t) +
                static_cast<int64_t>(t.queued + t.buffered + real) * 1000000LL / ps->rate;
            impl->played.push(
                PlayedEvent{static_cast<uint32_t>(real), to_client_time(now_us + ahead_us)});
        }
    }

    d.chunk->offset = 0;
    d.chunk->stride = static_cast<int32_t>(ps->bpf);
    d.chunk->size = static_cast<uint32_t>(frames * ps->bpf);
    b->size = frames;
    pw_stream_queue_buffer(ps->stream, b);
}

void playback_state_changed(void* data, pw_stream_state old, pw_stream_state state,
                            const char* error) {
    (void)old;
    (void)data;
    pw_log_debug("player: stream %s", pw_stream_state_as_string(state));
    if (state == PW_STREAM_STATE_ERROR)
        pw_log_warn("player: stream error: %s", error ? error : "unknown");
}

void playback_control_info(void* data, uint32_t id, const pw_stream_control* control) {
    auto* ps = static_cast<PlaybackStream*>(data);
    if (control == nullptr) return;
    if (!ps->volume_applied) {
        // The stream's controls exist from the first control_info on: set the server's volume
        // then, and take the graph's values as the user's only after that.
        if (id == SPA_PROP_channelVolumes) {
            ps->volume_applied = true;
            ps->impl->apply_volume_to_stream();
        }
        return;
    }
    if (id == SPA_PROP_channelVolumes && control->n_values > 0) {
        float maxv = 0.0f;
        for (uint32_t i = 0; i < control->n_values; ++i) maxv = std::max(maxv, control->values[i]);
        ps->impl->on_graph_volume(linear_to_volume(maxv), std::nullopt);
    } else if (id == SPA_PROP_mute && control->n_values > 0) {
        ps->impl->on_graph_volume(ps->impl->volume.load(), control->values[0] >= 0.5f);
    }
}

const pw_stream_events playback_events = [] {
    pw_stream_events e{};
    e.version = PW_VERSION_STREAM_EVENTS;
    e.state_changed = playback_state_changed;
    e.control_info = playback_control_info;
    e.process = playback_process;
    return e;
}();

void Impl::ensure_playback(uint32_t rate, uint8_t channels, uint8_t bits) {
    if (core == nullptr || core_gone) return;
    if (playback != nullptr && playback->rate == rate && playback->channels == channels &&
        playback->bits == bits) {
        return;
    }
    if (playback != nullptr) {
        // Format change without a stream end: anything still queued is in the old format.
        player_flush_upto.store(player_ring.write_position());
        destroy_playback();
    }

    pw_properties* props = pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Playback", PW_KEY_MEDIA_ROLE, "Music",
        PW_KEY_APP_NAME, "Sendspin", PW_KEY_APP_ICON_NAME, "audio-x-generic", PW_KEY_NODE_NAME,
        "sendspin.player", PW_KEY_NODE_DESCRIPTION, (cfg.name + " (Sendspin)").c_str(),
        PW_KEY_MEDIA_NAME, "Sendspin", PW_KEY_NODE_LATENCY, "1024/48000",
        // The server owns this stream's volume; don't let a restore policy override it.
        "state.restore-props", "false", nullptr);
    if (!cfg.player_props.empty()) pw_properties_update_string(props, cfg.player_props.c_str(),
                                                               cfg.player_props.size());

    auto* ps = new PlaybackStream();
    ps->impl = this;
    ps->rate = rate;
    ps->channels = channels;
    ps->bits = bits;
    ps->bpf = static_cast<size_t>(channels) * (bits / 8);
    ps->stream = pw_stream_new(core, "sendspin-player", props);
    if (ps->stream == nullptr) {
        pw_log_error("player: cannot create stream: %m");
        delete ps;
        return;
    }
    pw_stream_add_listener(ps->stream, &ps->listener, &playback_events, ps);

    uint8_t buffer[1024];
    spa_pod_builder b{};
    spa_pod_builder_init(&b, buffer, sizeof(buffer));
    const spa_pod* params[1] = {build_format(&b, spa_format_for_bits(bits), rate, channels)};
    const int res = pw_stream_connect(
        ps->stream, PW_DIRECTION_OUTPUT, PW_ID_ANY,
        static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS |
                                     PW_STREAM_FLAG_RT_PROCESS),
        params, 1);
    if (res < 0) {
        pw_log_error("player: cannot connect stream: %s", spa_strerror(res));
        pw_stream_destroy(ps->stream);
        delete ps;
        return;
    }
    playback = ps;
    apply_metadata_to_stream();
}

void Impl::destroy_playback() {
    if (playback == nullptr) return;
    if (!core_gone) pw_stream_destroy(playback->stream);
    delete playback;
    playback = nullptr;
}

void Impl::apply_volume_to_stream() {
    if (playback == nullptr || core_gone || !playback->volume_applied) return;
    float vols[SPA_AUDIO_MAX_CHANNELS];
    const float lin = volume_to_linear(volume.load());
    const uint32_t n = std::max<uint32_t>(1, playback->channels);
    for (uint32_t i = 0; i < n; ++i) vols[i] = lin;
    float mute = muted.load() ? 1.0f : 0.0f;
    pw_stream_set_control(playback->stream, SPA_PROP_channelVolumes, n, vols, SPA_PROP_mute, 1,
                          &mute, 0);
}

void Impl::on_graph_volume(uint8_t vol, std::optional<bool> mute) {
    // A user changed the stream's volume or mute in the mixer: report it to the server.
    if (vol != volume.load()) {
        volume.store(vol);
        store->set("player.volume", std::to_string(vol));
        ss_log(sendspin::LogLevel::DEBUG, "player: mixer set volume %u", vol);
        post_ss([this, vol] {
            if (player) player->update_volume(vol);
        });
    }
    if (mute.has_value() && *mute != muted.load()) {
        muted.store(*mute);
        store->set("player.muted", *mute ? "1" : "0");
        ss_log(sendspin::LogLevel::DEBUG, "player: mixer set mute %s", *mute ? "on" : "off");
        post_ss([this, m = *mute] {
            if (player) player->update_muted(m);
        });
    }
}

// ==============================================================================================
// Metadata
// ==============================================================================================

void MetadataListener::on_track_metadata(const sendspin::TrackMetadata& m) {
    // The views are only valid during the callback: copy.
    const auto copy = [](const std::optional<std::string_view>& v) -> std::optional<std::string> {
        if (!v) return std::nullopt;
        return std::string(*v);
    };
    Metadata md{copy(m.title), copy(m.artist), copy(m.album), copy(m.album_artist),
                copy(m.artwork_url)};
    ss_log(sendspin::LogLevel::DEBUG, "metadata: %s - %s", md.artist.value_or("").c_str(),
           md.title.value_or("").c_str());
    {
        std::lock_guard<std::mutex> lock(impl->meta_mutex);
        impl->meta = std::move(md);
    }
    impl->post_main([i = impl] { i->apply_metadata_to_stream(); });
}

void MetadataListener::on_metadata_clear() {
    {
        std::lock_guard<std::mutex> lock(impl->meta_mutex);
        impl->meta = Metadata{};
    }
    impl->post_main([i = impl] { i->apply_metadata_to_stream(); });
}

void Impl::apply_metadata_to_stream() {
    if (!cfg.metadata || playback == nullptr || core_gone) return;
    Metadata m;
    {
        std::lock_guard<std::mutex> lock(meta_mutex);
        m = meta;
    }
    std::string media_name;
    if (m.title) {
        media_name = m.artist && !m.artist->empty() ? *m.artist + " - " + *m.title : *m.title;
    } else {
        media_name = "Sendspin";
    }
    // A NULL value removes the key, so stale fields of the previous track disappear.
    auto val = [](const std::optional<std::string>& s) -> const char* {
        return s && !s->empty() ? s->c_str() : nullptr;
    };
    const spa_dict_item items[] = {
        {PW_KEY_MEDIA_NAME, media_name.c_str()},
        {PW_KEY_MEDIA_TITLE, val(m.title)},
        {PW_KEY_MEDIA_ARTIST, val(m.artist)},
        {"media.album", val(m.album)},
        {"media.album-artist", val(m.album_artist)},
        {"media.artwork-url", val(m.artwork_url)},
    };
    const spa_dict dict = SPA_DICT_INIT(items, SPA_N_ELEMENTS(items));
    pw_stream_update_properties(playback->stream, &dict);
}

// ==============================================================================================
// Source: sendspin side
// ==============================================================================================

void SourceListener::on_streaming_started() {
    ss_log(sendspin::LogLevel::DEBUG, "source: server started the input stream");
    impl->source_streaming.store(true);
    impl->post_main([i = impl] { i->update_capture(); });
}

void SourceListener::on_streaming_stopped() {
    ss_log(sendspin::LogLevel::DEBUG, "source: server stopped the input stream");
    impl->source_streaming.store(false);
    impl->post_main([i = impl] { i->update_capture(); });
}

void Impl::line_sense(const int16_t* samples, size_t count) {
    if (count == 0) return;
    double sum = 0.0;
    for (size_t i = 0; i < count; ++i) {
        const double x = samples[i] / 32768.0;
        sum += x * x;
    }
    const double rms = std::sqrt(sum / static_cast<double>(count));
    const double threshold = std::pow(10.0, cfg.threshold_db / 20.0);
    const int measured = rms >= threshold ? 1 : 0;
    const int current = signal_present ? 1 : 0;
    if (measured == current) {
        candidate_signal = -1;
        return;
    }
    const int64_t now = monotonic_us();
    if (candidate_signal != measured) {
        candidate_signal = measured;
        candidate_since_us = now;
        return;
    }
    const uint32_t required_ms = measured ? cfg.attack_ms : cfg.release_ms;
    if (now - candidate_since_us < static_cast<int64_t>(required_ms) * 1000) return;
    signal_present = measured != 0;
    candidate_signal = -1;
    ss_log(sendspin::LogLevel::DEBUG, "source: line signal %s", signal_present ? "present" : "absent");
    source->set_signal(signal_present ? sendspin::SourceSignal::PRESENT
                                      : sendspin::SourceSignal::ABSENT);
}

void Impl::pump_source() {
    // Sendspin thread (the one producer write_audio() allows). Drain what the capture stream
    // produced: feed line-sense, and while the input stream is open hand the audio to the library,
    // which chunks it. Each write is stamped on one sample-count timeline that is slowly slewed
    // toward the graph clock, so the stream's timestamps are continuous and free of the jitter of
    // the individual graph cycles.
    const bool streaming = source->is_streaming();
    if (!streaming) src_anchored = false;
    while (CaptureSlot* slot = capture_slots->front()) {
        const size_t bytes = static_cast<size_t>(slot->frames) * SOURCE_BPF;
        if (cfg.line_sense) {
            line_sense(reinterpret_cast<const int16_t*>(slot->data),
                       static_cast<size_t>(slot->frames) * SOURCE_CHANNELS);
        }
        if (streaming && slot->frames > 0) {
            const int64_t predicted =
                src_anchor_us +
                static_cast<int64_t>(src_frames_since_anchor * 1000000ULL / SOURCE_RATE);
            const int64_t err = slot->first_us - predicted;
            if (!src_anchored || slot->discontinuity || std::llabs(err) > SOURCE_REANCHOR_US) {
                if (src_anchored)
                    pw_log_debug("source: re-anchoring capture timeline (error %lld us%s)",
                                 static_cast<long long>(err),
                                 slot->discontinuity ? ", overflow" : "");
                src_anchor_us = slot->first_us;
                src_frames_since_anchor = 0;
                src_anchored = true;
            } else {
                // Slew toward the measured clock: absorbs device/system clock drift without
                // ever producing a timestamp jump.
                src_anchor_us += err / 256;
            }
            const int64_t ts =
                src_anchor_us +
                static_cast<int64_t>(src_frames_since_anchor * 1000000ULL / SOURCE_RATE);
            src_frames_since_anchor += slot->frames;
            if (!source->write_audio(slot->data, bytes, to_client_time(ts))) {
                // Stream closed meanwhile, or the capture buffer is full (network stall): the
                // library resumes from live audio, so restart the timeline with it.
                src_anchored = false;
            }
        }
        capture_slots->pop();
    }
}

// ==============================================================================================
// Source: PipeWire side
// ==============================================================================================

void capture_process(void* data) {
    // Realtime data thread.
    auto* cs = static_cast<CaptureStream*>(data);
    Impl* impl = cs->impl;
    pw_buffer* b = pw_stream_dequeue_buffer(cs->stream);
    if (b == nullptr) return;
    const spa_data& d = b->buffer->datas[0];
    if (d.data == nullptr || d.chunk == nullptr) {
        pw_stream_queue_buffer(cs->stream, b);
        return;
    }
    const uint32_t offset = std::min(d.chunk->offset, d.maxsize);
    const uint32_t size = std::min(d.chunk->size, d.maxsize - offset);
    uint32_t frames = size / SOURCE_BPF;

    if (frames > 0) {
        CaptureSlot* slot = impl->capture_slots->reserve();
        if (slot == nullptr) {
            impl->capture_overflow.store(true, std::memory_order_relaxed);
        } else {
            frames = std::min(frames, SOURCE_SLOT_FRAMES);
            memcpy(slot->data, static_cast<const uint8_t*>(d.data) + offset, frames * SOURCE_BPF);
            slot->frames = frames;
            slot->discontinuity = impl->capture_overflow.exchange(false, std::memory_order_relaxed);
            // First sample of this buffer was captured `delay` (device + graph latency) plus the
            // buffer's own length before the cycle started.
            pw_time t{};
            int64_t now_us = monotonic_us();
            int64_t delay_us = 0;
            if (pw_stream_get_time_n(cs->stream, &t, sizeof(t)) == 0) {
                if (t.now > 0) now_us = t.now / 1000;
                delay_us = pw_time_delay_us(t);
            }
            slot->first_us = now_us - delay_us -
                             static_cast<int64_t>(frames) * 1000000LL / SOURCE_RATE;
            impl->capture_slots->commit_push();
        }
    }
    pw_stream_queue_buffer(cs->stream, b);
}

void capture_state_changed(void* data, pw_stream_state old, pw_stream_state state,
                           const char* error) {
    (void)data;
    (void)old;
    pw_log_debug("source: stream %s", pw_stream_state_as_string(state));
    if (state == PW_STREAM_STATE_ERROR)
        pw_log_warn("source: stream error: %s", error ? error : "unknown");
}

const pw_stream_events capture_events = [] {
    pw_stream_events e{};
    e.version = PW_VERSION_STREAM_EVENTS;
    e.state_changed = capture_state_changed;
    e.process = capture_process;
    return e;
}();

void Impl::update_capture() {
    // Recording from an existing input, the capture stream exists only while it is needed: while
    // the server streams the input, or permanently when line-sense has to watch the signal.
    // Otherwise nothing holds the microphone/line-in open.
    //
    // As a virtual sink it must always exist, so applications can pick it as an output. It is
    // only kept processing (node.always-process) while the server streams the input: then a
    // silent sink still delivers a continuous (silent) timeline instead of going quiet on the
    // wire, and an idle one lets the graph suspend.
    const bool needed =
        cfg.source && (cfg.virtual_sink || source_streaming.load() || cfg.line_sense);
    if (needed && capture == nullptr) create_capture();
    if (!needed && capture != nullptr) destroy_capture();
    if (capture != nullptr && cfg.virtual_sink && !core_gone) {
        const bool process = source_streaming.load() || cfg.line_sense;
        const spa_dict_item item = {PW_KEY_NODE_ALWAYS_PROCESS, process ? "true" : "false"};
        const spa_dict dict = SPA_DICT_INIT(&item, 1);
        pw_stream_update_properties(capture->stream, &dict);
    }
}

void Impl::create_capture() {
    if (core == nullptr || core_gone) return;
    pw_properties* props;
    if (cfg.virtual_sink) {
        // A capture stream with media.class Audio/Sink is presented as an output device
        // (the same construction module-loopback uses): whatever applications play to it is
        // what this stream captures.
        props = pw_properties_new(
            PW_KEY_MEDIA_CLASS, "Audio/Sink", PW_KEY_NODE_NAME, "sendspin_input",
            PW_KEY_NODE_DESCRIPTION, sink_description().c_str(), PW_KEY_NODE_NICK, "Sendspin",
            PW_KEY_DEVICE_ICON_NAME,
            "audio-speakers", SPA_KEY_AUDIO_POSITION, "FL,FR", PW_KEY_NODE_LATENCY,
            "960/48000", PW_KEY_NODE_ALWAYS_PROCESS,
            (source_streaming.load() || cfg.line_sense) ? "true" : "false", nullptr);
    } else {
        props = pw_properties_new(
            PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Capture", PW_KEY_MEDIA_ROLE,
            "Music", PW_KEY_APP_NAME, "Sendspin", PW_KEY_APP_ICON_NAME, "audio-input-microphone",
            PW_KEY_NODE_NAME, "sendspin.source", PW_KEY_NODE_DESCRIPTION,
            (cfg.name + " (Sendspin input)").c_str(), PW_KEY_MEDIA_NAME, "Sendspin Live Input",
            PW_KEY_NODE_LATENCY, "960/48000", nullptr);
        if (cfg.capture_sink) pw_properties_set(props, PW_KEY_STREAM_CAPTURE_SINK, "true");
    }
    if (!cfg.source_props.empty()) pw_properties_update_string(props, cfg.source_props.c_str(),
                                                               cfg.source_props.size());

    auto* cs = new CaptureStream();
    cs->impl = this;
    cs->stream = pw_stream_new(core, "sendspin-source", props);
    if (cs->stream == nullptr) {
        pw_log_error("source: cannot create stream: %m");
        delete cs;
        return;
    }
    pw_stream_add_listener(cs->stream, &cs->listener, &capture_events, cs);

    uint8_t buffer[1024];
    spa_pod_builder b{};
    spa_pod_builder_init(&b, buffer, sizeof(buffer));
    const spa_pod* params[1] = {
        build_format(&b, SPA_AUDIO_FORMAT_S16_LE, SOURCE_RATE, SOURCE_CHANNELS)};
    const int res = pw_stream_connect(
        cs->stream, PW_DIRECTION_INPUT, PW_ID_ANY,
        static_cast<pw_stream_flags>((cfg.virtual_sink ? 0 : PW_STREAM_FLAG_AUTOCONNECT) |
                                     PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS),
        params, 1);
    if (res < 0) {
        pw_log_error("source: cannot connect stream: %s", spa_strerror(res));
        pw_stream_destroy(cs->stream);
        delete cs;
        return;
    }
    capture_overflow.store(true);  // first slot starts a new timeline
    capture = cs;
    pw_log_info("source: capture stream created%s",
                cfg.virtual_sink ? " (virtual sink)" : cfg.capture_sink ? " (sink monitor)" : "");
}

std::string Impl::sink_description() const {
    // "<server name> [Sendspin]": where the audio goes, and what kind of device it is. Before
    // any server has ever connected, just "Sendspin".
    return server_name.empty() ? std::string("Sendspin") : server_name + " [Sendspin]";
}

void Impl::set_server_name(const std::string& name) {
    // Main loop. Renames the virtual sink; node.name stays "sendspin_input" so routing that
    // WirePlumber remembered for it keeps working.
    if (name.empty() || name == server_name) return;
    server_name = name;
    store->set("source.server_name", name);
    if (capture != nullptr && cfg.virtual_sink && !core_gone) {
        const std::string desc = sink_description();
        const spa_dict_item item = {PW_KEY_NODE_DESCRIPTION, desc.c_str()};
        const spa_dict dict = SPA_DICT_INIT(&item, 1);
        pw_stream_update_properties(capture->stream, &dict);
        ss_log(sendspin::LogLevel::DEBUG, "source: virtual sink renamed to \"%s\"", desc.c_str());
    }
}

void Impl::destroy_capture() {
    if (capture == nullptr) return;
    if (!core_gone) pw_stream_destroy(capture->stream);
    delete capture;
    capture = nullptr;
    pw_log_info("source: capture stream closed");
}

// ==============================================================================================
// Sendspin thread
// ==============================================================================================

void Impl::drain_played() {
    uint32_t frames = 0;
    int64_t finish = 0;
    while (PlayedEvent* e = played.front()) {
        frames += e->frames;
        finish = e->finish_us;
        played.pop();
    }
    if (frames > 0 && player != nullptr) player->notify_audio_played(frames, finish);
}

void Impl::log_connection_changes() {
    // Sendspin thread. Says which server is connected and whether the session is paired, so "the
    // server never connected" can be told apart from "connected, but not set up".
    std::string summary;
    if (client->is_connected()) {
        const auto info = client->server_information();
        summary = "connected to \"" + (info ? std::string(info->name) : std::string("?")) + "\" (" +
                  (client->is_paired() ? "paired" : "unpaired") + ")";
    } else {
        summary = "not connected to a server";
    }
    if (summary != ss_connection_summary) {
        ss_connection_summary = summary;
        ss_log(sendspin::LogLevel::INFO, "%s", summary.c_str());
    }
}

void Impl::publish_mpris() {
    // Sendspin thread: hand MPRIS a snapshot a few times a second; it signals only what changed.
    const int64_t now = monotonic_us();
    if (now < ss_mpris_due_us) return;
    ss_mpris_due_us = now + MPRIS_INTERVAL_US;

    MprisState st;
    st.connected = client->is_connected();
    if (st.connected) {
        st.playing = client->group_state().playback_state == sendspin::SendspinPlaybackState::PLAYING;
        {
            std::lock_guard<std::mutex> lock(meta_mutex);
            st.title = meta.title;
            st.artist = meta.artist;
            st.album = meta.album;
            st.album_artist = meta.album_artist;
            st.art_url = meta.artwork_url;
        }
        if (metadata_role != nullptr) {
            st.duration_ms = metadata_role->get_track_duration_ms();
            st.progress_ms = metadata_role->get_track_progress_ms();
        }
        st.progress_at_us = now;
        if (controller != nullptr) {
            const auto& cs = controller->get_controller_state();
            st.has_controller = !cs.supported_commands.empty();
            st.supported = cs.supported_commands;
            st.volume = cs.volume;
            st.muted = cs.muted;
            st.repeat = cs.repeat;
            st.shuffle = cs.shuffle;
            st.seek_max_ms = cs.seek_max_ms;
        }
    }
    mpris->update(std::move(st));
}

void Impl::ss_main() {
    pw_log_debug("sendspin thread running");
    while (running.load()) {
        client->loop();
        run_ss_tasks();
        log_connection_changes();
        if (cfg.virtual_sink) {
            // The sink is named after the server: pick up its name once it has said hello.
            if (auto info = client->server_information();
                info.has_value() && !info->name.empty() && info->name != ss_server_name) {
                ss_server_name = std::string(info->name);
                post_main([this, n = ss_server_name] { set_server_name(n); });
            }
        }
        if (player != nullptr) drain_played();
        if (source != nullptr) pump_source();
        if (mpris) publish_mpris();
        std::this_thread::sleep_for(std::chrono::milliseconds(LOOP_INTERVAL_MS));
    }

    // Goodbye every peer, join the library's threads and deliver the clear callbacks. stop() is
    // synchronous and bounded. The data loop keeps running meanwhile, so a sync task blocked in
    // on_audio_output() always gets unstuck.
    client->stop();
    // Destroy the client on this thread, which has been the only one driving it.
    player = nullptr;
    source = nullptr;
    controller = nullptr;
    metadata_role = nullptr;
    client.reset();
    pw_log_debug("sendspin thread finished");
}

// ==============================================================================================
// Module lifecycle
// ==============================================================================================

void on_main_event(void* data, uint64_t count) {
    (void)count;
    static_cast<Impl*>(data)->run_main_tasks();
}

void on_core_error(void* data, uint32_t id, int seq, int res, const char* message) {
    (void)data;
    (void)seq;
    pw_log_error("core error id:%u res:%d (%s): %s", id, res, spa_strerror(res), message);
}

const pw_core_events core_events = [] {
    pw_core_events e{};
    e.version = PW_VERSION_CORE_EVENTS;
    e.error = on_core_error;
    return e;
}();

void on_core_proxy_destroy(void* data) {
    auto* impl = static_cast<Impl*>(data);
    if (impl->core_hooks) {
        spa_hook_remove(&impl->core_listener);
        spa_hook_remove(&impl->core_proxy_listener);
        impl->core_hooks = false;
    }
    // The streams went down with the core; forget them without touching them.
    impl->core_gone = true;
    impl->core = nullptr;
    pw_log_warn("connection to PipeWire lost");
}

const pw_proxy_events core_proxy_events = [] {
    pw_proxy_events e{};
    e.version = PW_VERSION_PROXY_EVENTS;
    e.destroy = on_core_proxy_destroy;
    return e;
}();

void teardown(Impl* impl) {
    // 0. MPRIS first: its thread sends controller commands into the client.
    if (impl->mpris) impl->mpris->stop();
    // 1. Stop the sendspin thread; it disconnects and destroys the client. The data loop keeps
    //    running meanwhile, so a decoder blocked in on_audio_write() always gets unstuck.
    if (impl->ss_thread.joinable()) {
        impl->running.store(false);
        impl->ss_thread.join();
    }
    impl->client.reset();
    if (impl->mdns) impl->mdns->stop();

    // 2. No more cross-thread work can arrive; drop what is pending.
    if (impl->main_event != nullptr) {
        pw_loop_destroy_source(impl->main_loop, impl->main_event);
        impl->main_event = nullptr;
    }
    impl->main_tasks.clear();

    // 3. Streams and the core connection.
    impl->destroy_playback();
    impl->destroy_capture();
    if (impl->core_hooks) {
        spa_hook_remove(&impl->core_listener);
        spa_hook_remove(&impl->core_proxy_listener);
        impl->core_hooks = false;
    }
    if (impl->core != nullptr && !impl->core_gone && impl->own_core) pw_core_disconnect(impl->core);
    impl->core = nullptr;
    delete impl;
}

void on_module_destroy(void* data) {
    auto* impl = static_cast<Impl*>(data);
    spa_hook_remove(&impl->module_listener);
    pw_log_info("unloading");
    teardown(impl);
}

const pw_impl_module_events module_events = [] {
    pw_impl_module_events e{};
    e.version = PW_VERSION_IMPL_MODULE_EVENTS;
    e.destroy = on_module_destroy;
    return e;
}();

bool parse_config(Config& c, const pw_properties* props) {
    const char* v;
    c.name = (v = pw_properties_get(props, KEY_NAME)) ? v : hostname();
    if ((v = pw_properties_get(props, KEY_CLIENT_ID)) != nullptr && v[0] != '\0') {
        c.client_id = v;
    } else {
        const std::string mid = read_machine_id();
        c.client_id = mid.empty() ? std::string{} : "pipewire-" + mid.substr(0, 12);
    }
    c.port = parse_num<uint16_t>(pw_properties_get(props, KEY_PORT), c.port, 1, 65535, KEY_PORT);
    c.mdns = parse_bool(pw_properties_get(props, KEY_MDNS), true);
    if ((v = pw_properties_get(props, KEY_STATE_FILE)) != nullptr) {
        c.state_file = v;  // empty string disables persistence
    } else {
        c.state_file = default_state_path(c.client_id.empty() ? "default" : c.client_id);
    }
    if ((v = pw_properties_get(props, KEY_LOG_LEVEL)) != nullptr) {
        const std::string l = v;
        using L = sendspin::LogLevel;
        c.log_level = l == "none"    ? L::NONE
                      : l == "error" ? L::ERROR
                      : l == "warn"  ? L::WARN
                      : l == "debug" ? L::DEBUG
                      : l == "verbose" ? L::VERBOSE
                                       : L::INFO;
    }

    c.player = parse_bool(pw_properties_get(props, KEY_PLAYER), true);
    c.mpris = parse_bool(pw_properties_get(props, KEY_MPRIS), true);
    c.metadata = c.player || c.mpris;  // the playback stream's media.* and MPRIS both need it
    if ((v = pw_properties_get(props, KEY_PLAYER_CODECS)) != nullptr) {
        c.codecs.clear();
        std::string s = v;
        size_t pos = 0;
        while (pos <= s.size()) {
            const size_t comma = s.find(',', pos);
            std::string tok = s.substr(pos, comma == std::string::npos ? std::string::npos
                                                                       : comma - pos);
            tok.erase(0, tok.find_first_not_of(" \t"));
            tok.erase(tok.find_last_not_of(" \t") + 1);
            if (tok == "flac") c.codecs.push_back(sendspin::SendspinCodecFormat::FLAC);
            else if (tok == "opus") c.codecs.push_back(sendspin::SendspinCodecFormat::OPUS);
            else if (tok == "pcm") c.codecs.push_back(sendspin::SendspinCodecFormat::PCM);
            else if (!tok.empty()) pw_log_warn("ignoring unknown codec \"%s\"", tok.c_str());
            if (comma == std::string::npos) break;
            pos = comma + 1;
        }
        if (c.codecs.empty()) {
            pw_log_error("%s lists no usable codec", KEY_PLAYER_CODECS);
            return false;
        }
    }
    c.static_delay_ms = parse_num<uint16_t>(pw_properties_get(props, KEY_PLAYER_DELAY), 0, 0,
                                            5000, KEY_PLAYER_DELAY);
    if ((v = pw_properties_get(props, KEY_PLAYER_PROPS)) != nullptr) c.player_props = v;

    c.source = parse_bool(pw_properties_get(props, KEY_SOURCE), true);
    if ((v = pw_properties_get(props, KEY_SOURCE_CODEC)) != nullptr) {
        const std::string codec = v;
        if (codec == "pcm") c.source_codec = sendspin::SendspinCodecFormat::PCM;
        else if (codec == "flac") c.source_codec = sendspin::SendspinCodecFormat::FLAC;
        else if (codec == "opus") c.source_codec = sendspin::SendspinCodecFormat::OPUS;
        else pw_log_warn("invalid %s \"%s\", using pcm", KEY_SOURCE_CODEC, v);
    }
    c.capture_sink = parse_bool(pw_properties_get(props, KEY_SOURCE_CAPTURE_SINK), false);
    // The virtual sink is the default source; asking for the sink monitor turns it off unless it
    // is requested explicitly as well.
    const char* vs = pw_properties_get(props, KEY_SOURCE_VIRTUAL_SINK);
    c.virtual_sink = parse_bool(vs, !c.capture_sink);
    if (c.virtual_sink && c.capture_sink) {
        pw_log_warn("%s overrides %s", KEY_SOURCE_VIRTUAL_SINK, KEY_SOURCE_CAPTURE_SINK);
        c.capture_sink = false;
    }
    c.line_sense = parse_bool(pw_properties_get(props, KEY_SOURCE_LINE_SENSE), false);
    c.threshold_db = parse_num<double>(pw_properties_get(props, KEY_SOURCE_THRESHOLD), -50.0,
                                       -120.0, 0.0, KEY_SOURCE_THRESHOLD);
    c.attack_ms = parse_num<uint32_t>(pw_properties_get(props, KEY_SOURCE_ATTACK), 300, 0, 600000,
                                      KEY_SOURCE_ATTACK);
    c.release_ms = parse_num<uint32_t>(pw_properties_get(props, KEY_SOURCE_RELEASE), 5000, 0,
                                       3600000, KEY_SOURCE_RELEASE);
    if ((v = pw_properties_get(props, KEY_SOURCE_PROPS)) != nullptr) c.source_props = v;

    if ((v = pw_properties_get(props, KEY_UNPAIRED)) != nullptr)
        c.unpaired_access = pw_properties_parse_bool(v);

    if (!c.player && !c.source) {
        pw_log_error("neither %s nor %s is enabled: nothing to do", KEY_PLAYER, KEY_SOURCE);
        return false;
    }
    return true;
}

std::vector<sendspin::AudioSupportedFormatObject> advertised_formats(const Config& c) {
    // PipeWire converts any rate/depth to the device, so advertise a broad set; the server picks
    // the first one it can serve. Stereo only (see PCM_ALIGN).
    static constexpr uint32_t RATES[] = {48000, 44100, 96000, 88200};
    static constexpr uint8_t DEPTHS[] = {24, 16};
    std::vector<sendspin::AudioSupportedFormatObject> out;
    for (auto codec : c.codecs) {
        if (codec == sendspin::SendspinCodecFormat::OPUS) {
            out.push_back({codec, 2, 48000, 16});
            continue;
        }
        for (uint8_t depth : DEPTHS)
            for (uint32_t rate : RATES) out.push_back({codec, 2, rate, depth});
    }
    return out;
}

/// Logs like sendspin-cpp does: to stderr (the journal for a systemd-run PipeWire), gated by
/// sendspin.log-level, and additionally through pw_log at debug level for PipeWire's own log
/// targets (debug, so the journal does not get every line twice).
void ss_log(sendspin::LogLevel level, const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (static_cast<int>(module_log_level.load()) >= static_cast<int>(level)) {
        static constexpr char LETTERS[] = "-EWIDV";
        fprintf(stderr, "%c sendspin.module: %s\n", LETTERS[static_cast<int>(level) % 6], buf);
    }
    pw_log_debug("%s", buf);
}

/// Publishes the pairing token as the property "sendspin.pairing-token" of this module's
/// PipeWire client ("Sendspin"), readable with pw-dump / pw-cli by the user's PipeWire clients.
/// It is neither logged nor written to a file.
void publish_pairing_token(Impl* impl, const std::string& token) {
    if (token.empty()) {
        ss_log(sendspin::LogLevel::WARN, "no pairing token available");
        return;
    }
    const spa_dict_item items[] = {SPA_DICT_ITEM_INIT(PROP_PAIRING_TOKEN, token.c_str())};
    const spa_dict dict = SPA_DICT_INIT_ARRAY(items);
    const int res = pw_core_update_properties(impl->core, &dict);
    if (res < 0) {
        pw_log_warn("cannot publish the pairing token: %s", spa_strerror(res));
        return;
    }
    ss_log(sendspin::LogLevel::INFO,
           "pairing token: property \"%s\" of PipeWire client \"Sendspin\" (pw-dump)",
           PROP_PAIRING_TOKEN);
}

int module_init(pw_impl_module* module, const char* args) {
    auto* impl = new Impl();
    impl->module = module;
    impl->context = pw_impl_module_get_context(module);
    impl->main_loop = pw_context_get_main_loop(impl->context);

    {
        pw_properties* props = args ? pw_properties_new_string(args) : pw_properties_new(nullptr,
                                                                                         nullptr);
        if (props == nullptr) {
            delete impl;
            return -errno;
        }
        const bool ok = parse_config(impl->cfg, props);
        pw_properties_free(props);
        if (!ok) {
            delete impl;
            return -EINVAL;
        }
    }
    const Config& cfg = impl->cfg;
    module_log_level.store(cfg.log_level);
    sendspin::SendspinClient::set_log_level(library_log_level(cfg.log_level));

    int res = 0;
    impl->main_event = pw_loop_add_event(impl->main_loop, on_main_event, impl);
    if (impl->main_event == nullptr) {
        res = -errno;
        pw_log_error("cannot add loop event: %s", spa_strerror(res));
        teardown(impl);
        return res;
    }

    // A connection of our own, named "Sendspin": mixers (pavucontrol, desktop volume applets)
    // label streams with their client's application name, which for the daemon's or a client
    // process's shared connection is just "pipewire".
    impl->core = pw_context_connect(
        impl->context,
        pw_properties_new(PW_KEY_APP_NAME, "Sendspin", PW_KEY_APP_ICON_NAME, "audio-x-generic",
                          PW_KEY_APP_ID, "sendspin", PW_KEY_CLIENT_NAME, "Sendspin", nullptr),
        0);
    impl->own_core = true;
    if (impl->core == nullptr) {
        res = -errno;
        pw_log_error("cannot connect to PipeWire: %s", spa_strerror(res));
        teardown(impl);
        return res;
    }
    pw_core_add_listener(impl->core, &impl->core_listener, &core_events, impl);
    pw_proxy_add_listener(reinterpret_cast<pw_proxy*>(impl->core), &impl->core_proxy_listener,
                          &core_proxy_events, impl);
    impl->core_hooks = true;

    impl->store = std::make_unique<StateStore>(cfg.state_file);
    if (!impl->store->persistent())
        pw_log_warn("no state file: pairings and the identity key will not survive a restart");

    // ---- client ----
    sendspin::SendspinClientConfig cc;
    // The protocol client id is derived from the persisted Noise identity key; cfg.client_id only
    // names the state file.
    cc.name = cfg.name;
    cc.product_name = "PipeWire Sendspin";
    cc.manufacturer = "PipeWire";
    cc.software_version = MODULE_VERSION;
    cc.server_port = cfg.port;

    impl->client = std::make_unique<sendspin::SendspinClient>(std::move(cc));
    // Guest (unpaired) playback is handy for a player; a source-only endpoint does not admit
    // unpaired servers unless asked to. Set before start() so the first hello carries it.
    impl->client->set_unpaired_access_enabled(cfg.unpaired_access.value_or(cfg.player));
    // Before start(): it loads the identity and pairings through it.
    impl->client->set_persistence_provider(impl->store.get());
    impl->client->set_network_provider(&impl->network);

    if (cfg.player) {
        sendspin::PlayerRoleConfig pc;
        pc.audio_formats = advertised_formats(cfg);
        // Finish timestamps reported from the process callback already include graph and device
        // latency, so no fixed delay must be added on top.
        pc.fixed_delay_us = 0;
        pc.initial_output_delay_ms = cfg.static_delay_ms;
        impl->player = &impl->client->add_player(std::move(pc));
        impl->player_listener = std::make_unique<PlayerListener>(impl);
        impl->player->set_listener(impl->player_listener.get());
        impl->player->set_output_delay_adjustable(true);

        uint8_t vol = 100;
        if (auto v = impl->store->get("player.volume")) vol = static_cast<uint8_t>(
            std::clamp(atoi(v->c_str()), 0, 100));
        const bool mute = impl->store->get("player.muted").value_or("0") == "1";
        impl->volume.store(vol);
        impl->muted.store(mute);
        impl->player->update_volume(vol);
        impl->player->update_muted(mute);
    }
    if (cfg.metadata) {
        auto& md = impl->client->add_metadata();
        impl->metadata_listener = std::make_unique<MetadataListener>(impl);
        md.set_listener(impl->metadata_listener.get());
        impl->metadata_role = &md;
    }
    if (cfg.mpris && Mpris::available()) {
        // controller@v1 carries the media keys / play-pause / seek of the desktop's media
        // controls to the server's group.
        impl->controller = &impl->client->add_controller();
    }
    if (cfg.source) {
        sendspin::SourceRoleConfig sc;
        sc.codec = cfg.source_codec;
        sc.sample_rate = SOURCE_RATE;
        sc.channels = SOURCE_CHANNELS;
        sc.bit_depth = SOURCE_BITS;
        sc.line_sense = cfg.line_sense;
        // Room for one PipeWire quantum of up to SOURCE_SLOT_FRAMES per write (a write may take
        // at most half the buffer) and for network stalls.
        sc.capture_buffer_ms = 1000;
        impl->capture_slots = std::make_unique<SpscQueue<CaptureSlot, 32>>();
        impl->source = &impl->client->add_source(std::move(sc));
        impl->source_listener = std::make_unique<SourceListener>(impl);
        impl->source->set_listener(impl->source_listener.get());
        if (cfg.line_sense) impl->source->set_signal(sendspin::SourceSignal::ABSENT);
    }

    if (!impl->client->start()) {
        pw_log_error("cannot start the Sendspin server on port %u", cfg.port);
        teardown(impl);
        return -EADDRINUSE;
    }

    std::string roles;
    if (cfg.player) roles += "player@v1 ";
    if (cfg.metadata) roles += "metadata@v1 ";
    if (impl->controller != nullptr) roles += "controller@v1 ";
    if (cfg.source) roles += "source@v1 ";
    // These lines are what a user needs to set the client up, so they go out the way sendspin-cpp
    // logs (stderr, gated by sendspin.log-level) rather than through pw_log, which PipeWire's
    // default log level (warnings only) would hide.
    ss_log(sendspin::LogLevel::INFO, "Sendspin client \"%s\" listening on port %u, roles: %s",
           cfg.name.c_str(), cfg.port, roles.c_str());
    ss_log(sendspin::LogLevel::DEBUG, "client id: %s", impl->client->client_id().c_str());
    ss_log(sendspin::LogLevel::DEBUG, "state file: %s",
           cfg.state_file.empty() ? "<none>" : cfg.state_file.c_str());
    {
        const auto token = impl->client->client_pairing_token();
        publish_pairing_token(impl, token ? std::string(token->data(), token->size()) : std::string{});
    }

    if (cfg.mdns) {
        impl->mdns = std::make_unique<MdnsAdvertiser>();
        if (!MdnsAdvertiser::available())
            pw_log_warn("built without Avahi: not advertised over mDNS, add the client manually");
        else if (!impl->mdns->start(cfg.name, cfg.port, SENDSPIN_PATH, cfg.name))
            pw_log_warn("mDNS advertisement failed; the server must be pointed at port %u",
                        cfg.port);
    }

    if (cfg.mpris) {
        impl->mpris = std::make_unique<Mpris>();
        if (!Mpris::available()) {
            pw_log_warn("built without libsystemd: no MPRIS media controls");
            impl->mpris.reset();
        } else if (!impl->mpris->start(cfg.client_id, [impl](const auto& cmd) {
                       return impl->controller != nullptr && impl->controller->send_command(cmd);
                   })) {
            ss_log(sendspin::LogLevel::WARN, "MPRIS unavailable (no session bus?)");
            impl->mpris.reset();
        }
    }

    if (auto n = impl->store->get("source.server_name")) {
        impl->server_name = *n;
        impl->ss_server_name = *n;
    }
    if (cfg.source) impl->update_capture();  // opens the input now if line-sense needs it

    impl->running.store(true);
    try {
        impl->ss_thread = std::thread([impl] { impl->ss_main(); });
    } catch (const std::system_error& e) {
        pw_log_error("cannot start sendspin thread: %s", e.what());
        impl->running.store(false);
        teardown(impl);
        return -EAGAIN;
    }

    pw_impl_module_add_listener(module, &impl->module_listener, &module_events, impl);
    const spa_dict info_dict = SPA_DICT_INIT_ARRAY(module_info);
    pw_impl_module_update_properties(module, &info_dict);
    return 0;
}

}  // namespace
}  // namespace pwsendspin

extern "C" SPA_EXPORT int pipewire__module_init(struct pw_impl_module* module, const char* args) {
    PW_LOG_TOPIC_INIT(mod_topic);
    try {
        return pwsendspin::module_init(module, args);
    } catch (const std::exception& e) {
        pw_log_error("sendspin: initialisation failed: %s", e.what());
        return -EIO;
    } catch (...) {
        pw_log_error("sendspin: initialisation failed");
        return -EIO;
    }
}
