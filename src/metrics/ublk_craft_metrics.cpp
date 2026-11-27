#include "ublk_craft_metrics.hpp"

#include <utility>

namespace ublkpp {

UblkCraftMetrics::UblkCraftMetrics(std::string const& parent_id, std::string const& craft_disk_id,
                                   std::function< snapshot() > sample) :
        sisl::MetricsGroup{craft_disk_id, craft_disk_id}, _sample(std::move(sample)) {
    REGISTER_GAUGE(craft_commit_lsn, "CRAFT client commit frontier (highest quorum-durable dLSN, -1 if none)",
                   "ublk_craft_commit_lsn", {"parent_id", parent_id});
    REGISTER_GAUGE(craft_all_committed_lsn, "Lowest commit_lsn across the CRAFT members (-1 if none)",
                   "ublk_craft_all_committed_lsn", {"parent_id", parent_id});
    REGISTER_GAUGE(craft_members, "Number of CRAFT members", "ublk_craft_members", {"parent_id", parent_id});
    REGISTER_GAUGE(craft_members_catching_up, "CRAFT members still catching up (reads route around them)",
                   "ublk_craft_members_catching_up", {"parent_id", parent_id});
    register_me_to_farm();
    attach_gather_cb([this] { refresh(); });
}

UblkCraftMetrics::~UblkCraftMetrics() {
    detach_gather_cb();
    deregister_me_from_farm();
}

void UblkCraftMetrics::refresh() {
    if (!_sample) return;
    auto const s = _sample();
    GAUGE_UPDATE(*this, craft_commit_lsn, s.commit_lsn);
    GAUGE_UPDATE(*this, craft_all_committed_lsn, s.all_committed_lsn);
    GAUGE_UPDATE(*this, craft_members, static_cast< int64_t >(s.members));
    GAUGE_UPDATE(*this, craft_members_catching_up, static_cast< int64_t >(s.members_catching_up));
}

} // namespace ublkpp
