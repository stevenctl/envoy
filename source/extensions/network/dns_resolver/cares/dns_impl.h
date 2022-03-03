#pragma once

#include <cstdint>
#include <string>

#include "envoy/common/platform.h"
#include "envoy/event/dispatcher.h"
#include "envoy/event/file_event.h"
#include "envoy/network/dns.h"
#include "envoy/registry/registry.h"

#include "source/common/common/linked_object.h"
#include "source/common/common/logger.h"
#include "source/common/common/utility.h"
#include "source/common/network/dns_resolver/dns_factory_util.h"

#include "absl/container/node_hash_map.h"
#include "ares.h"

namespace Envoy {
namespace Network {

class DnsResolverImplPeer;

/**
 * Implementation of DnsResolver that uses c-ares. All calls and callbacks are assumed to
 * happen on the thread that owns the creating dispatcher.
 */
class DnsResolverImpl : public DnsResolver, protected Logger::Loggable<Logger::Id::dns> {
public:
  DnsResolverImpl(
      const envoy::extensions::network::dns_resolver::cares::v3::CaresDnsResolverConfig& config,
      Event::Dispatcher& dispatcher,
      const std::vector<Network::Address::InstanceConstSharedPtr>& resolvers);
  ~DnsResolverImpl() override;

  // Network::DnsResolver
  ActiveDnsQuery* resolve(const std::string& dns_name, DnsLookupFamily dns_lookup_family,
                          ResolveCb callback) override;

private:
  friend class DnsResolverImplPeer;
  class PendingResolution : public ActiveDnsQuery {
  public:
    void cancel(CancelReason reason) override {
      // c-ares only supports channel-wide cancellation, so we just allow the
      // network events to continue but don't invoke the callback on completion.
      // TODO(mattklein123): Potentially use timeout to destroy and recreate the channel.
      cancelled_ = true;
      cancel_reason_ = reason;
    }
    // Does the object own itself? Resource reclamation occurs via self-deleting
    // on query completion or error.
    bool owned_ = false;
    // Has the query completed? Only meaningful if !owned_;
    bool completed_ = false;

  protected:
    // Network::ActiveDnsQuery
    PendingResolution(DnsResolverImpl& parent, ResolveCb callback, Event::Dispatcher& dispatcher,
                      ares_channel channel, const std::string& dns_name)
        : parent_(parent), callback_(callback), dispatcher_(dispatcher), channel_(channel),
          dns_name_(dns_name) {}

    void finishResolve();

    DnsResolverImpl& parent_;
    // Caller supplied callback to invoke on query completion or error.
    const ResolveCb callback_;
    // Dispatcher to post any callback_ exceptions to.
    Event::Dispatcher& dispatcher_;
    // Was the query cancelled via cancel()?
    bool cancelled_ = false;
    const ares_channel channel_;
    const std::string dns_name_;
    CancelReason cancel_reason_;

    // Small wrapping struct to accumulate addresses from firings of the
    // onAresGetAddrInfoCallback callback.
    struct PendingResponse {
      ResolutionStatus status_;
      std::list<DnsResponse> address_list_;
    };

    // Note: pending_response_ is constructed with ResolutionStatus::Failure by default and
    // __only__ changed to ResolutionStatus::Success if there is an ARES_SUCCESS reply.
    // In the dual_resolution case __any__ ARES_SUCCESS reply will result in a
    // ResolutionStatus::Success callback.
    PendingResponse pending_response_{ResolutionStatus::Failure, {}};
  };

  class AddrInfoPendingResolution final : public PendingResolution {
  public:
    AddrInfoPendingResolution(DnsResolverImpl& parent, ResolveCb callback,
                              Event::Dispatcher& dispatcher, ares_channel channel,
                              const std::string& dns_name, DnsLookupFamily dns_lookup_family);

    /**
     * ares_getaddrinfo query callback.
     * @param status return status of call to ares_getaddrinfo.
     * @param timeouts the number of times the request timed out.
     * @param addrinfo structure to store address info.
     */
    void onAresGetAddrInfoCallback(int status, int timeouts, ares_addrinfo* addrinfo);

    /**
     * wrapper function of call to ares_getaddrinfo.
     */
    void startResolution();

  private:
    void startResolutionImpl(int family);

    // Holds the availability of non-loopback network interfaces for the system.
    struct AvailableInterfaces {
      bool v4_available_;
      bool v6_available_;
    };

    // Return the currently available network interfaces.
    // Note: this call uses syscalls.
    AvailableInterfaces availableInterfaces();

    // Perform a second resolution under certain conditions. If dns_lookup_family_ is V4Preferred
    // or Auto: perform a second resolution if the first one fails. If dns_lookup_family_ is All:
    // perform resolutions on both families concurrently.
    bool dual_resolution_ = false;
    // Whether or not to lookup both V4 and V6 address.
    bool lookup_all_ = false;
    int family_ = AF_INET;
    const DnsLookupFamily dns_lookup_family_;
    // Queried for at construction time.
    const AvailableInterfaces available_interfaces_;
  };

  struct AresOptions {
    ares_options options_;
    int optmask_;
  };

  static absl::optional<std::string>
  maybeBuildResolversCsv(const std::vector<Network::Address::InstanceConstSharedPtr>& resolvers);

  // Callback for events on sockets tracked in events_.
  void onEventCallback(os_fd_t fd, uint32_t events);
  // c-ares callback when a socket state changes, indicating that libevent
  // should listen for read/write events.
  void onAresSocketStateChange(os_fd_t fd, int read, int write);
  // Initialize the channel.
  void initializeChannel(ares_options* options, int optmask);
  // Check if the only nameserver available is the c-ares default.
  bool isCaresDefaultTheOnlyNameserver();
  // Update timer for c-ares timeouts.
  void updateAresTimer();
  // Return default AresOptions.
  AresOptions defaultAresOptions();

  Event::Dispatcher& dispatcher_;
  Event::TimerPtr timer_;
  ares_channel channel_;
  bool dirty_channel_{};
  envoy::config::core::v3::DnsResolverOptions dns_resolver_options_;

  absl::node_hash_map<int, Event::FileEventPtr> events_;
  const bool use_resolvers_as_fallback_;
  const absl::optional<std::string> resolvers_csv_;
  const bool filter_unroutable_families_;
};

DECLARE_FACTORY(CaresDnsResolverFactory);

} // namespace Network
} // namespace Envoy
