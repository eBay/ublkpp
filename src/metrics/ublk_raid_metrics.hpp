#pragma once

#include <cstdint>
#include <string>

#include <sisl/metrics/metrics.hpp>

namespace ublkpp {
//test-copilot
// RAID-level metrics - tracks RAID device operations
//
// Constructor parameters:
//   parent_id: The ID of the parent device that contains this RAID array. This is used as a label
//              to correlate metrics across the device hierarchy (e.g., volume -> RAID -> disks).
//              For standalone RAID arrays, you can use the RAID's own ID.
//   raid_device_id: The unique identifier for this specific RAID instance.
struct UblkRaidMetrics : public sisl::MetricsGroup {
    UblkRaidMetrics(std::string const& parent_id, std::string const& raid_device_id);
    ~UblkRaidMetrics();

    void record_device_degraded(std::string const& device_name);
    void record_device_swap();

    // RAID1 resync metrics
    void record_resync_start();
    void record_resync_progress(uint64_t bytes);
    void record_resync_complete(uint64_t duration_seconds);
    void record_active_resyncs(uint64_t count);
    void record_dirty_pages(uint64_t pages, uint64_t remaining_bytes);
    void record_last_resync_size(uint64_t bytes);
    void record_resync_initial_size(uint64_t bytes);
    void record_degraded_state(bool is_degraded);
};

} // namespace ublkpp
