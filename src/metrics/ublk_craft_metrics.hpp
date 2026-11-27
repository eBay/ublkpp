#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include <sisl/metrics/metrics.hpp>

namespace ublkpp {

// CRAFT disk metrics: where the client's commit frontier is, and how many members are still catching up through
// server-side resync. All gauges are sampled when the metrics are gathered (a scrape), from `sample`, so nothing on
// the IO path updates them.
//
// Constructor parameters:
//   parent_id:     the volume this disk belongs to (a label, as for RAID metrics), to correlate across the device
//                  hierarchy (volume -> RAID0 -> CRAFT partition).
//   craft_disk_id: the CRAFT disk's own id (its partition id): the metrics entity.
//   sample:        reads the current state; called from the metrics gather thread, so it must be thread-safe.
struct UblkCraftMetrics : public sisl::MetricsGroup {
    struct snapshot {
        int64_t commit_lsn{-1};        // the client's commit frontier: every dLSN <= it is quorum-durable
        int64_t all_committed_lsn{-1}; // min commit_lsn across members (the journal reclaim floor)
        uint64_t members{0};
        uint64_t members_catching_up{0}; // members reads still route around (behind, or new after a replica move)
    };

    UblkCraftMetrics(std::string const& parent_id, std::string const& craft_disk_id,
                     std::function< snapshot() > sample);
    ~UblkCraftMetrics();

    void refresh(); // samples now (also what a gather does)

private:
    std::function< snapshot() > _sample;
};

} // namespace ublkpp
