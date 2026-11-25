// SPDX-License-Identifier: Apache-2.0

#include "mdns.h"

#include <pipewire/log.h>

#ifdef HAVE_AVAHI
#include <avahi-client/client.h>
#include <avahi-client/publish.h>
#include <avahi-common/alternative.h>
#include <avahi-common/error.h>
#include <avahi-common/malloc.h>
#include <avahi-common/thread-watch.h>
#endif

namespace pwsendspin {

namespace {
constexpr const char* SERVICE_TYPE = "_sendspin._tcp";
}

#ifdef HAVE_AVAHI

struct MdnsAdvertiser::Impl {
    AvahiThreadedPoll* poll{nullptr};
    AvahiClient* client{nullptr};
    AvahiEntryGroup* group{nullptr};
    char* name{nullptr};  // avahi_strdup'd; replaced on collision
    uint16_t port{0};
    std::string path;
    std::string friendly;

    // All callbacks below run on the Avahi poll thread (or with its lock held).
    static void group_cb(AvahiEntryGroup* g, AvahiEntryGroupState state, void* userdata) {
        auto* self = static_cast<Impl*>(userdata);
        self->group = g;
        switch (state) {
            case AVAHI_ENTRY_GROUP_ESTABLISHED:
                pw_log_info("sendspin: mDNS service '%s' (%s) established", self->name,
                            SERVICE_TYPE);
                break;
            case AVAHI_ENTRY_GROUP_COLLISION: {
                char* alt = avahi_alternative_service_name(self->name);
                pw_log_warn("sendspin: mDNS name '%s' collides, renaming to '%s'", self->name,
                            alt);
                avahi_free(self->name);
                self->name = alt;
                self->create_services(avahi_entry_group_get_client(g));
                break;
            }
            case AVAHI_ENTRY_GROUP_FAILURE:
                pw_log_warn("sendspin: mDNS entry group failure: %s",
                            avahi_strerror(avahi_client_errno(avahi_entry_group_get_client(g))));
                break;
            default:
                break;
        }
    }

    void create_services(AvahiClient* c) {
        if (group == nullptr) {
            group = avahi_entry_group_new(c, group_cb, this);
            if (group == nullptr) {
                pw_log_warn("sendspin: avahi_entry_group_new failed: %s",
                            avahi_strerror(avahi_client_errno(c)));
                return;
            }
        }
        avahi_entry_group_reset(group);
        const std::string txt_path = "path=" + path;
        const std::string txt_name = "name=" + friendly;
        AvahiStringList* txt = avahi_string_list_new(txt_path.c_str(), nullptr);
        if (!friendly.empty()) txt = avahi_string_list_add(txt, txt_name.c_str());
        int ret = avahi_entry_group_add_service_strlst(
            group, AVAHI_IF_UNSPEC, AVAHI_PROTO_UNSPEC, static_cast<AvahiPublishFlags>(0), name,
            SERVICE_TYPE, nullptr, nullptr, port, txt);
        avahi_string_list_free(txt);
        if (ret == AVAHI_ERR_COLLISION) {
            char* alt = avahi_alternative_service_name(name);
            avahi_free(name);
            name = alt;
            create_services(c);
            return;
        }
        if (ret < 0) {
            pw_log_warn("sendspin: cannot add mDNS service: %s", avahi_strerror(ret));
            return;
        }
        if ((ret = avahi_entry_group_commit(group)) < 0)
            pw_log_warn("sendspin: cannot commit mDNS service: %s", avahi_strerror(ret));
    }

    static void client_cb(AvahiClient* c, AvahiClientState state, void* userdata) {
        auto* self = static_cast<Impl*>(userdata);
        switch (state) {
            case AVAHI_CLIENT_S_RUNNING:
                self->create_services(c);
                break;
            case AVAHI_CLIENT_S_COLLISION:
            case AVAHI_CLIENT_S_REGISTERING:
                if (self->group) avahi_entry_group_reset(self->group);
                break;
            case AVAHI_CLIENT_FAILURE:
                pw_log_warn("sendspin: Avahi client failure: %s",
                            avahi_strerror(avahi_client_errno(c)));
                // The client is dead (daemon went away). AVAHI_CLIENT_NO_FAIL normally keeps it
                // alive and waiting; anything reaching here is unrecoverable for this run.
                break;
            case AVAHI_CLIENT_CONNECTING:
                pw_log_info("sendspin: waiting for avahi-daemon");
                break;
            default:
                break;
        }
    }
};

MdnsAdvertiser::MdnsAdvertiser() : impl_(std::make_unique<Impl>()) {}
MdnsAdvertiser::~MdnsAdvertiser() { stop(); }

bool MdnsAdvertiser::available() { return true; }

bool MdnsAdvertiser::start(const std::string& instance_name, uint16_t port,
                           const std::string& path, const std::string& friendly_name) {
    stop();
    impl_->poll = avahi_threaded_poll_new();
    if (impl_->poll == nullptr) {
        pw_log_warn("sendspin: cannot create Avahi poll");
        return false;
    }
    impl_->name = avahi_strdup(instance_name.c_str());
    impl_->port = port;
    impl_->path = path;
    impl_->friendly = friendly_name;

    int error = 0;
    impl_->client = avahi_client_new(avahi_threaded_poll_get(impl_->poll), AVAHI_CLIENT_NO_FAIL,
                                     Impl::client_cb, impl_.get(), &error);
    if (impl_->client == nullptr) {
        pw_log_warn("sendspin: cannot create Avahi client: %s", avahi_strerror(error));
        stop();
        return false;
    }
    if (avahi_threaded_poll_start(impl_->poll) < 0) {
        pw_log_warn("sendspin: cannot start Avahi poll thread");
        stop();
        return false;
    }
    return true;
}

void MdnsAdvertiser::stop() {
    if (impl_->poll != nullptr) avahi_threaded_poll_stop(impl_->poll);
    if (impl_->client != nullptr) {
        // Frees the entry group too, withdrawing the service.
        avahi_client_free(impl_->client);
        impl_->client = nullptr;
        impl_->group = nullptr;
    }
    if (impl_->poll != nullptr) {
        avahi_threaded_poll_free(impl_->poll);
        impl_->poll = nullptr;
    }
    if (impl_->name != nullptr) {
        avahi_free(impl_->name);
        impl_->name = nullptr;
    }
}

#else  // !HAVE_AVAHI

struct MdnsAdvertiser::Impl {};
MdnsAdvertiser::MdnsAdvertiser() : impl_(std::make_unique<Impl>()) {}
MdnsAdvertiser::~MdnsAdvertiser() = default;
bool MdnsAdvertiser::available() { return false; }
bool MdnsAdvertiser::start(const std::string&, uint16_t, const std::string&, const std::string&) {
    return false;
}
void MdnsAdvertiser::stop() {}

#endif

}  // namespace pwsendspin
