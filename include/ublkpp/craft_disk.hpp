#pragma once

// craft_disk: a ublkpp leaf disk (ublkpp::ublk_disk) that exposes a CRAFT volume as a ublk block device
// (/dev/ublkbN). It drives a craft_client (login / dLSN-stamped quorum writes / horizon reads) over a set of
// replicas that the factory assembles for you -- either an in-process reference cluster (no servers, no wire)
// or a set of remote replica servers over TCP.
//
// The disk OWNS its transport + client and self-configures its geometry from what login reports (capacity /
// block size), so a consumer names only the transport + volume and gets a device back. Construction throws
// std::runtime_error on failure (bad geometry, login failure), matching ublkpp's other leaf-disk factories.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <ublkpp/drivers.hpp> // disk_handle

#include <craft/types.hpp> // craft::volume_id_t, craft::replica_endpoint
#include <craft/wire.hpp>  // craft::wire::k_default_max_tx (the single-sourced volume max-transfer default)

namespace ublkpp {

// A caller-chosen session-ownership token (see CRAFT's client_token). Any unique value; admission is the
// transport's job, so the token is just the owner identity.
inline constexpr uint64_t k_craft_default_token = 0xC0FFEEULL;

// In-process REFERENCE cluster: `n` replicas serving `vol_id` at `page_size` bytes/block, `capacity` bytes,
// entirely in this process (no servers, no wire). The self-contained path -- ideal for bringing up a device
// against the model. Links craft_reference. `max_tx` configures the reference volume's max transfer; the disk
// self-sizes its geometry from craft::max_tx(client) after login, so it is single-sourced from the volume.
disk_handle make_craft_disk_local(craft::volume_id_t vol_id, uint32_t n = 3, uint32_t page_size = 4096,
                                  uint64_t capacity = uint64_t{1} << 30,
                                  uint32_t max_tx = craft::wire::k_default_max_tx,
                                  uint64_t client_token = k_craft_default_token);

// REMOTE replica servers over TCP. `members` are the {peer id, "host:port"} endpoints (index 0 is the leader
// you first log in to); ALL geometry (capacity / block size / max transfer) comes from the server's login
// response, so there is no max_tx to pass -- the server's volume defines it.
disk_handle make_craft_disk_tcp(std::vector< craft::replica_endpoint > members, craft::volume_id_t vol_id,
                                uint64_t client_token = k_craft_default_token);

} // namespace ublkpp
