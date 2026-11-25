// SPDX-License-Identifier: Apache-2.0
//
// Persistent key/value state: sendspin-cpp's blobs (the Noise identity key, the Pairing PSK, the
// pairing records, the output delay, the last server) plus the module's own settings (player
// volume/mute, the server name the virtual sink is named after). Without it every restart of
// PipeWire would present a new identity and lose all pairings.

#pragma once

#include <sendspin/client.h>

#include <map>
#include <mutex>
#include <optional>
#include <string>

namespace pwsendspin {

class StateStore final : public sendspin::SendspinPersistenceProvider {
public:
    /// An empty path disables persistence (everything is kept in memory only).
    explicit StateStore(std::string path);

    const std::string& path() const { return path_; }
    bool persistent() const { return !path_.empty(); }

    /// The module's own values (any thread).
    std::optional<std::string> get(const std::string& key) const;
    bool set(const std::string& key, const std::string& value);

    // --- sendspin::SendspinPersistenceProvider ---
    std::optional<size_t> read_blob(std::string_view key, std::span<uint8_t> out) override;
    bool write_blob(std::string_view key, std::span<const uint8_t> data) override;

private:
    void load();
    bool flush_locked();

    std::string path_;
    mutable std::mutex mutex_;
    std::map<std::string, std::string> values_;
};

}  // namespace pwsendspin
