// SPDX-License-Identifier: Apache-2.0
//
// Advertises the Sendspin client as _sendspin._tcp over Avahi so that a Sendspin server
// (Music Assistant) discovers it and connects in. Runs on Avahi's own threaded poll, never on
// the PipeWire loops.

#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace pwsendspin {

class MdnsAdvertiser {
public:
    MdnsAdvertiser();
    ~MdnsAdvertiser();
    MdnsAdvertiser(const MdnsAdvertiser&) = delete;
    MdnsAdvertiser& operator=(const MdnsAdvertiser&) = delete;

    /// Starts advertising. Returns false only if mDNS is unavailable in this build or Avahi's
    /// poll could not be created; a missing avahi-daemon is retried in the background.
    bool start(const std::string& instance_name, uint16_t port, const std::string& path,
               const std::string& friendly_name);
    void stop();

    static bool available();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace pwsendspin
