# pipewire-module-sendspin

A PipeWire module that turns a Linux computer into a **Sendspin** client for
Music Assistant, built on [sendspin-cpp](https://github.com/Sendspin/sendspin-cpp). The roles can be
switched on and off separately:

| Role | Key | Default | What it does |
|---|---|---|---|
| `player@v1` | `sendspin.player` | `true` | Music Assistant plays to this computer. Audio is decoded (FLAC/Opus/PCM), clock-synchronised with the other players in the group and played through a **playback stream** (`sendspin.player`) in the PipeWire graph. |
| `metadata@v1` | | with `sendspin.player` or `sendspin.mpris` | Title / artist / album of the current track are set as `media.*` properties on that playback stream (pavucontrol, `wpctl status`, desktop mixers) and shown in the media controls. |
| `source@v1` | `sendspin.source` | `true` | A virtual output device that applications play to (default), or this computer's input (default input, a chosen node, the monitor of the default sink) becomes a **Live Input** in Music Assistant. It always reports whether the input has a signal; detected from the level (`sendspin.source.signal-detect`), that lets MA start and stop playback automatically. |

At least one of `sendspin.player` / `sendspin.source` must be enabled.

With `sendspin.mpris` (on by default) the module also registers `controller@v1` and
appears in the desktop's media controls (MPRIS): title, artist, album, cover and
position of what the server's group plays, with play/pause, next/previous, seek,
volume, shuffle and repeat sent back to the server.

This project was created with the assistance of Claude.

## How it works

* The module runs a Sendspin WebSocket server on `sendspin.port` (default
  8928) and advertises it as `_sendspin._tcp` via Avahi. Music Assistant
  discovers it and connects in (server-initiated). The connection uses
  Sendspin's Noise-encrypted handshake.
* **Player:** the playback stream is created when the server starts a
  stream, in exactly the stream's format (PipeWire converts to the device),
  and closed when it ends. Playback progress is reported from
  `pw_stream_get_time_n()`, so graph and device latency are part of the
  synchronisation. Volume and mute go both ways: server changes set the
  stream's volume (cubic curve, like pavucontrol), and changing the stream
  in a mixer is reported back to the server. Volume and mute are restored
  after a restart.
* **Source:** captures 48 kHz / 16-bit / stereo (what Music Assistant Live
  Inputs use); sendspin-cpp sends it in 20 ms chunks (PCM, or FLAC/Opus with
  `sendspin.source.codec`) timestamped on the synchronised server clock. The capture stream only exists while the server has the input
  started, so the microphone/line-in is not held open otherwise. With
  `sendspin.source.signal-detect` it stays open to watch the level. It shows up in
  pavucontrol's **Recording** tab only while it exists.
* **Virtual sink** (`sendspin.source.virtual-sink`, on by default): the source is an
  output device, *"&lt;server name&gt; [Sendspin]"* (e.g. *"Music Assistant [Sendspin]"*;
  just *"Sendspin"* until a server has connected once), listed with your speakers.
  Send any application (or the whole desktop) to it, and that audio is the
  Live Input. The device is always present; it only keeps processing while
  Music Assistant plays the input (or signal detection is on), so an idle sink lets
  the graph suspend.
* **Pairing:** a paired server is always admitted; an unpaired one only while
  `sendspin.unpaired-access` is on. The Noise identity key (from which the
  client id is derived), the Pairing PSK and the pairing records are stored in
  the state file so they survive restarts. The pairing token is neither logged
  nor written to a file: it is the property `sendspin.pairing-token` of the
  module's PipeWire client "Sendspin", so any PipeWire tool of the same user
  can read it:

  ```bash
  pw-dump | grep -o '"sendspin.pairing-token": "[^"]*"'
  ```

* **Media controls (MPRIS):** the module registers `org.mpris.MediaPlayer2.Sendspin`
  on the session bus (GNOME/KDE media controls, lock screen, `playerctl`). A
  stopped group that still has a track shows as *Paused*. The buttons only offer
  what the server supports. Requires libsystemd (sd-bus) at build time and a
  session bus at run time; a PipeWire daemon started by systemd --user has one.
* Realtime-safety: the PipeWire process callbacks only touch wait-free
  ring buffers. All networking, decoding and protocol work runs on separate
  threads.

## Dependencies

```bash
# Debian / Ubuntu
sudo apt install libpipewire-0.3-dev libspa-0.2-dev libavahi-client-dev \
  libsystemd-dev cmake ninja-build pkg-config g++ git
# Fedora
sudo dnf install pipewire-devel avahi-devel systemd-devel cmake ninja-build gcc-c++ git
# Arch
sudo pacman -S pipewire avahi systemd-libs cmake ninja pkgconf gcc git
```

The remaining dependencies are git submodules under `external/`, pinned to
the versions sendspin-cpp expects:

| Submodule | Version |
|---|---|
| [sendspin-cpp](https://github.com/Sendspin/sendspin-cpp) | `96e002d` from the fork [meiser79/sendspin-cpp](https://github.com/meiser79/sendspin-cpp): main after v0.10.0 plus [#287](https://github.com/Sendspin/sendspin-cpp/pull/287) (no `source@v1_support` in `client/hello`, Sendspin/spec#335) |
| [ArduinoJson](https://github.com/bblanchon/ArduinoJson) | v7.4.1 |
| [IXWebSocket](https://github.com/machinezone/IXWebSocket) | v12.0.1 |
| [micro-flac](https://github.com/esphome-libs/micro-flac) (+ micro-ogg-demuxer) | v0.3.1 |
| [micro-opus](https://github.com/esphome-libs/micro-opus) (+ opus, micro-ogg-demuxer) | v0.3.5 |
| [noise-c](https://github.com/esphome-libs/noise-c) | v0.1.13 |

CMake builds from these checkouts, so no network access is needed at build
time. A submodule that is not checked out is downloaded by FetchContent as a
fallback. Everything is linked statically into the module; it exports only
`pipewire__module_init`.

`-DSENDSPIN_SOURCE_DIR=/path` uses a different sendspin-cpp tree. Without the
submodule, the build falls back to a sibling checkout at `../sendspin-cpp`.

## Build & install

```bash
git clone --recurse-submodules https://github.com/meiser79/pipewire-module-sendspin.git
cd pipewire-module-sendspin
# (in an existing clone: git submodule update --init --recursive)
cmake -B build -G Ninja
ninja -C build
sudo cmake --install build     # .so → PipeWire module dir, example conf → share/doc
```

## Configuration

### Inside the PipeWire daemon (normal use)

```bash
mkdir -p ~/.config/pipewire/pipewire.conf.d
cp pipewire.conf.d/sendspin.conf ~/.config/pipewire/pipewire.conf.d/
systemctl --user restart pipewire
```

Edit the `args` in that file; every key is documented there. Short version:

```
args = {
  sendspin.name   = "Living Room PC"
  sendspin.player = true          # player@v1
  sendspin.source = true          # source@v1
  sendspin.source.capture-sink = false
  sendspin.source.signal-detect = true
  #player.props = { target.object = "alsa_output.…" }
  #source.props = { target.object = "alsa_input.…" }
}
```

### As a separate process

`sendspin-client.conf` runs the same module in its own PipeWire client
process. That keeps the network code out of the daemon, and lets you run
several clients with different settings (give each its own `sendspin.port`
and `sendspin.client-id`):

```bash
pipewire -c "$PWD/sendspin-client.conf"
```

### All keys

| Key | Default | |
|---|---|---|
| `sendspin.name` | hostname | Friendly name |
| `sendspin.client-id` | `pipewire-<machine-id[0:12]>` | Instance id, names the default state file (the protocol client id comes from the identity key) |
| `sendspin.port` | `8928` | WebSocket listen port |
| `sendspin.mdns` | `true` | Advertise via Avahi |
| `sendspin.state-file` | `$XDG_STATE_HOME/pipewire-module-sendspin/<id>.state` | Identity key, pairings, volume, output delay. Empty = keep in memory only |
| `sendspin.unpaired-access` | `= sendspin.player` | Let unpaired servers play / activate roles |
| `sendspin.log-level` | `info` | Log level (`none`…`verbose`, printed to the daemon's stderr). `info` logs start-up and the server connection only; per-track, volume and stream events are `debug`. sendspin-cpp itself logs errors only below `debug`. |
| `sendspin.player` | `true` | Enable player@v1 |
| `sendspin.mpris` | `true` | Desktop media controls (MPRIS) with `controller@v1` |
| `sendspin.player.codecs` | `flac,opus,pcm` | Advertised codecs, in preference order |
| `sendspin.player.static-delay-ms` | `0` | Initial output delay; a value the server sets later is remembered |
| `player.props` | | Extra properties for the playback stream |
| `sendspin.source` | `true` | Enable source@v1 |
| `sendspin.source.codec` | `pcm` | Input stream codec: `pcm`, `flac` or `opus` (PCM if the server does not accept it) |
| `sendspin.source.capture-sink` | `false` | Capture the default sink's monitor instead (turns the virtual sink off unless it is set explicitly) |
| `sendspin.source.virtual-sink` | `true` (`false` with `capture-sink`) | Create an output device that applications play to; that audio is the Live Input. `false` records an input instead |
| `sendspin.source.signal-detect` | `false` | Detect the input's signal from its level. The signal is always reported; without detection it is absent, so the server starts the input only when asked to |
| `sendspin.source.signal.threshold-db` | `-50` | RMS threshold, dBFS |
| `sendspin.source.signal.attack-ms` | `300` | Signal must be above the threshold this long before it counts as present |
| `sendspin.source.signal.release-ms` | `5000` | Signal must be below the threshold this long before it counts as absent |
| `source.props` | | Extra properties for the capture stream |

## Limitations

* The module waits for the server to connect (server-initiated). Without
  Avahi, add the client in Music Assistant manually (`ws://host:8928/sendspin`).
* Players advertise stereo formats only.
* One client per port: two users on one machine, or two module instances,
  need different `sendspin.port` and `sendspin.client-id` values.

## License

Apache-2.0
