#include "source/extensions/transport_sockets/internal_upstream/internal_upstream.h"
#include "source/common/network/address_impl.h"

namespace Envoy {
namespace Extensions {
namespace TransportSockets {
namespace InternalUpstream {

InternalSocket::InternalSocket(
    Network::TransportSocketPtr inner_socket,
    std::unique_ptr<envoy::config::core::v3::Metadata> metadata,
    std::unique_ptr<IoSocket::UserSpace::FilterStateObjects> filter_state_objects)
    : PassthroughSocket(std::move(inner_socket)), metadata_(std::move(metadata)),
      filter_state_objects_(std::move(filter_state_objects)) {


  auto fmd = metadata->mutable_filter_metadata();
  auto iter = fmd->find("tunnel"); // TODO use the explicitly named sources
  if (iter != fmd->end()) {
    auto address_it = iter->second.fields().find("target");
    if (address_it != iter->second.fields().end() && address_it->second.has_string_value()) {
      ENVOY_LOG_MISC(trace, "create tunnel info with target address {}",
                     address_it->second.string_value());
      // Alternatively, create it via configurable metadata name or from config itself.
      tunnel_info_ = std::make_shared<TunnelInfoImpl>(
          std::make_shared<Network::Address::EnvoyInternalInstance>(
              address_it->second.string_value()));
    }
  }

}

void InternalSocket::setTransportSocketCallbacks(Network::TransportSocketCallbacks& callbacks) {
  transport_socket_->setTransportSocketCallbacks(callbacks);
  auto* io_handle = dynamic_cast<IoSocket::UserSpace::IoHandle*>(&callbacks.ioHandle());
  if (io_handle != nullptr && io_handle->passthroughState()) {
    io_handle->passthroughState()->initialize(std::move(metadata_),
                                              std::move(filter_state_objects_));
  }
  metadata_ = nullptr;
  filter_state_objects_ = nullptr;
}

} // namespace InternalUpstream
} // namespace TransportSockets
} // namespace Extensions
} // namespace Envoy
