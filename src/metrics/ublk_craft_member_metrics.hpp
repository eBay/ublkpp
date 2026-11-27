#pragma once

#include <cstdint>
#include <string>

#include <sisl/metrics/metrics.hpp>

namespace ublkpp {

// Per-member latency of a CRAFT disk: one group per member slot, fed by craft::set_leg_observer. A leg is one
// member's write, read or keep_alive, timed from issue to reply; a write's slower legs are still reported after the
// quorum ack, so a slow member shows up here even while the volume's own latency looks fine.
//
// Constructor parameters:
//   parent_id:     the volume (a label, as for the other ublk metrics).
//   craft_disk_id: the CRAFT disk's id (its partition id).
//   slot:          the member slot; the entity is "<craft_disk_id>:m<slot>". A replaced member keeps its slot.
struct UblkCraftMemberMetrics : public sisl::MetricsGroup {
    UblkCraftMemberMetrics(std::string const& parent_id, std::string const& craft_disk_id, std::size_t slot);
    ~UblkCraftMemberMetrics();

    enum class op : uint8_t { write, read, keep_alive };
    void record(op o, uint64_t usec, bool ok);
};

} // namespace ublkpp
