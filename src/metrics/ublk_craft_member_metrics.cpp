#include "ublk_craft_member_metrics.hpp"

#include <fmt/format.h>

namespace ublkpp {

UblkCraftMemberMetrics::UblkCraftMemberMetrics(std::string const& parent_id, std::string const& craft_disk_id,
                                               std::size_t slot) :
        sisl::MetricsGroup{fmt::format("{}:m{}", craft_disk_id, slot), fmt::format("{}:m{}", craft_disk_id, slot)} {
    REGISTER_HISTOGRAM(craft_member_write_latency_us, "CRAFT member write latency in microseconds",
                       "ublk_craft_member_write_latency_us", {"parent_id", parent_id},
                       HistogramBucketsType(ExponentialOfTwoBuckets));
    REGISTER_HISTOGRAM(craft_member_read_latency_us, "CRAFT member read latency in microseconds",
                       "ublk_craft_member_read_latency_us", {"parent_id", parent_id},
                       HistogramBucketsType(ExponentialOfTwoBuckets));
    REGISTER_HISTOGRAM(craft_member_keepalive_latency_us, "CRAFT member keep_alive latency in microseconds",
                       "ublk_craft_member_keepalive_latency_us", {"parent_id", parent_id},
                       HistogramBucketsType(ExponentialOfTwoBuckets));
    REGISTER_COUNTER(craft_member_errors_total, "CRAFT member legs that failed (error, timeout or unreachable)",
                     "ublk_craft_member_errors_total", {"parent_id", parent_id});
    register_me_to_farm();
}

UblkCraftMemberMetrics::~UblkCraftMemberMetrics() { deregister_me_from_farm(); }

void UblkCraftMemberMetrics::record(op o, uint64_t usec, bool ok) {
    if (!ok) {
        COUNTER_INCREMENT(*this, craft_member_errors_total, 1);
        return; // a failed leg's duration is mostly the timeout: keep it out of the latency
    }
    switch (o) {
    case op::write:
        HISTOGRAM_OBSERVE(*this, craft_member_write_latency_us, usec);
        break;
    case op::read:
        HISTOGRAM_OBSERVE(*this, craft_member_read_latency_us, usec);
        break;
    case op::keep_alive:
        HISTOGRAM_OBSERVE(*this, craft_member_keepalive_latency_us, usec);
        break;
    }
}

} // namespace ublkpp
