#include <filesystem>
#include <future>
#include <ostream>
#include <system_error>
#include <vector>

#include <boost/uuid/random_generator.hpp>
#include <boost/uuid/string_generator.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <sisl/http/http_server.hpp>
#include <sisl/logging/logging.h>
#include <sisl/options/options.h>

#include <ublkpp/craft_disk.hpp>
#include <ublkpp/drivers.hpp>
#include <ublkpp/raid.hpp>
#include <ublkpp/target.hpp>

SISL_OPTION_GROUP(ublkpp_disk,
                  (uuid, "", "vol_id", "Volume UUID to use (else random)", ::cxxopts::value< std::string >(), ""),
                  (loop, "", "loop", "Attach a single device 1-to-1", ::cxxopts::value< std::string >(), "<path>"),
                  (raid0, "", "raid0", "Devices for RAID0 device", ::cxxopts::value< std::vector< std::string > >(),
                   "<path>[,<path>,...]"),
                  (raid1, "", "raid1", "Devices for RAID1 device", ::cxxopts::value< std::vector< std::string > >(),
                   "<path>[,<path>,...]"),
                  (raid10, "", "raid10", "Devices for RAID10 device", ::cxxopts::value< std::vector< std::string > >(),
                   "<path>[,<path>,...]"),
                  (stripe_size, "", "stripe_size", "RAID-0 Stripe Size",
                   ::cxxopts::value< uint32_t >()->default_value("131072"), ""),
                  (craft, "", "craft", "CRAFT in-process reference volume of the given size (MiB)",
                   ::cxxopts::value< uint64_t >(), "<size_mb>"),
                  (craft_tcp, "", "craft_tcp",
                   "CRAFT over TCP: comma-separated host:port replica endpoints (run a "
                   "craft_reference_tcp_srv per endpoint first)",
                   ::cxxopts::value< std::string >(), "<host:port,...>"),
                  (device_id, "", "device_id", "Recover existing device",
                   cxxopts::value< int32_t >()->default_value("-1"), "<ublkid>"),
                  (assume_clean, "", "assume_clean",
                   "RAID1: assert a new leg reads zero where unallocated; its rebuild then skips "
                   "all-zero regions (thin-preserving)",
                   cxxopts::value< bool >()->default_value("false")->implicit_value("true"), ""))

#define ENABLED_OPTIONS logging, ublkpp_tgt, raid1, ublkpp_disk

SISL_OPTIONS_ENABLE(ENABLED_OPTIONS)

SISL_LOGGING_INIT(ublksrv, UBLKPP_LOG_MODS)

///
// Clean shutdown
static std::promise< int > s_stop_code;
static void handle(int signal);
///

using Result = std::expected< std::filesystem::path, std::error_condition >;
static auto k_target = std::unique_ptr< ublkpp::ublkpp_tgt >();

static Result _run_target(boost::uuids::uuid const& vol_id, std::shared_ptr< ublkpp::ublk_disk > dev) {

    // Wait for initialization to complete
    auto res = ublkpp::ublkpp_tgt::run(vol_id, std::move(dev), SISL_OPTIONS["device_id"].as< int32_t >());
    if (!res) { return std::unexpected(res.error()); }
    k_target = std::move(res.value());
    return k_target->device_path();
}

// Return a device based on the format of the input
// Optional: pass in a unique identifier for metrics tracking
static std::shared_ptr< ublkpp::ublk_disk > get_driver(std::string const& resource,
                                                       std::string const& metrics_id = "") {
    if (auto path = std::filesystem::path(resource); std::filesystem::exists(path)) {
        return ublkpp::make_fs_disk(path, metrics_id);
    }
    return ublkpp::make_missing_disk();
}

Result create_loop(boost::uuids::uuid const& id, std::string const& path) {
    auto dev = std::shared_ptr< ublkpp::ublk_disk >();
    try {
        auto loop_id = fmt::format("loop_{}", boost::uuids::to_string(id).substr(0, 8));
        dev = get_driver(path, loop_id);
    } catch (std::runtime_error const& e) {}
    if (!dev) return std::unexpected(std::make_error_condition(std::errc::operation_not_permitted));
    return _run_target(id, std::move(dev));
}

Result create_raid0(boost::uuids::uuid const& id, std::vector< std::string > const& layout) {
    auto dev = std::shared_ptr< ublkpp::ublk_disk >();
    try {
        auto devices = std::vector< std::shared_ptr< ublkpp::ublk_disk > >();
        auto raid_uuid = boost::uuids::to_string(id);

        // Create stripe devices with RAID0 UUID for correlation
        for (auto const& disk : layout) {
            devices.push_back(get_driver(disk, raid_uuid));
        }

        if (0 < devices.size())
            dev = ublkpp::make_raid0_disk(id, SISL_OPTIONS["stripe_size"].as< uint32_t >(), std::move(devices));
    } catch (std::runtime_error const& e) {}
    if (!dev) return std::unexpected(std::make_error_condition(std::errc::operation_not_permitted));
    return _run_target(id, std::move(dev));
}

Result create_raid1(boost::uuids::uuid const& id, std::vector< std::string > const& layout) {
    auto dev = std::shared_ptr< ublkpp::ublk_disk >();
    auto raid_uuid = boost::uuids::to_string(id);

    try {
        // Create file-backed devices with RAID1 UUID for correlation
        auto dev_a = get_driver(*layout.begin(), raid_uuid);
        auto dev_b = get_driver(*(layout.begin() + 1), raid_uuid);

        auto const assume_clean = SISL_OPTIONS["assume_clean"].as< bool >();
        dev = ublkpp::make_raid1_disk(id, std::move(dev_a), std::move(dev_b), raid_uuid, assume_clean);
    } catch (std::runtime_error const& e) {}
    if (!dev) return std::unexpected(std::make_error_condition(std::errc::operation_not_permitted));
    return _run_target(id, std::move(dev));
}

Result create_raid10(boost::uuids::uuid const& id, std::vector< std::string > const& layout) {
    if (1 > layout.size()) {
        LOGERROR("Zero mirrors in Array [uuid:{}]!", to_string(id))
        return std::unexpected(std::make_error_condition(std::errc::invalid_argument));
    }

    auto dev = std::shared_ptr< ublkpp::ublk_disk >();
    auto raid10_uuid_str = boost::uuids::to_string(id);
    auto const assume_clean = SISL_OPTIONS["assume_clean"].as< bool >();
    try {
        auto devices = std::vector< std::shared_ptr< ublkpp::ublk_disk > >();
        auto name_gen = boost::uuids::name_generator(id);

        // Process disks in pairs to create RAID1 mirrors
        for (size_t i = 0; i + 1 < layout.size(); i += 2) {
            // Generate partition UUID for this RAID1 mirror
            auto partition_uuid = name_gen(fmt::format("partition_{}", i / 2));
            auto partition_uuid_str = boost::uuids::to_string(partition_uuid);

            auto dev_a = get_driver(layout[i], partition_uuid_str);
            auto dev_b = get_driver(layout[i + 1], partition_uuid_str);

            // Create RAID1 mirror and add to devices
            devices.push_back(ublkpp::make_raid1_disk(partition_uuid, std::move(dev_a), std::move(dev_b),
                                                      raid10_uuid_str, assume_clean));
        }

        dev = ublkpp::make_raid0_disk(id, SISL_OPTIONS["stripe_size"].as< uint32_t >(), std::move(devices));
    } catch (std::runtime_error const& e) {}
    if (!dev) return std::unexpected(std::make_error_condition(std::errc::operation_not_permitted));
    return _run_target(id, std::move(dev));
}

// A self-contained CRAFT device: an in-process 3-replica reference cluster (no servers, no wire) behind a ublk
// block device. The disk logs in and self-sizes from what the cluster reports; `size_mb` sets the volume size.
Result create_craft(boost::uuids::uuid const& id, uint64_t size_mb) {
    auto dev = std::shared_ptr< ublkpp::ublk_disk >();
    try {
        dev = ublkpp::make_craft_disk_local(id, /*n=*/3, /*page_size=*/4096, /*capacity=*/size_mb << 20);
    } catch (std::runtime_error const& e) { LOGERROR("craft disk create failed: {}", e.what()) }
    if (!dev) return std::unexpected(std::make_error_condition(std::errc::operation_not_permitted));
    return _run_target(id, std::move(dev));
}

// CRAFT over TCP: connect to standalone craft_reference_tcp_srv replicas at the given host:port endpoints. The
// disk logs in to the first (leader), self-sizes from its login geometry (capacity / block size / max transfer),
// and drives writes/reads over the ON-RING transport at QD>1. Start one craft_reference_tcp_srv per endpoint.
Result create_craft_tcp(boost::uuids::uuid const& id, std::string const& endpoints) {
    std::vector< craft::replica_endpoint > members;
    for (std::size_t start = 0; start <= endpoints.size();) {
        auto const comma = endpoints.find(',', start);
        auto const addr = endpoints.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
        if (!addr.empty()) {
            craft::replica_endpoint m;
            m.id = boost::uuids::random_generator()(); // cosmetic: the server routes by index / fences by term, not id
            m.addr = addr;                             // "host:port"; tcp_cluster splits on the last ':'
            members.push_back(std::move(m));
        }
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    if (members.empty()) {
        LOGERROR("craft_tcp: no endpoints given")
        return std::unexpected(std::make_error_condition(std::errc::invalid_argument));
    }
    auto dev = std::shared_ptr< ublkpp::ublk_disk >();
    try {
        dev = ublkpp::make_craft_disk_tcp(std::move(members), id);
    } catch (std::runtime_error const& e) { LOGERROR("craft_tcp disk create failed: {}", e.what()) }
    if (!dev) return std::unexpected(std::make_error_condition(std::errc::operation_not_permitted));
    return _run_target(id, std::move(dev));
}

int main(int argc, char* argv[]) {
    SISL_OPTIONS_LOAD(argc, argv, ENABLED_OPTIONS);
    sisl::logging::SetLogger(std::string(argv[0]),
                             BOOST_PP_STRINGIZE(PACKAGE_NAME), BOOST_PP_STRINGIZE(PACKAGE_VERSION));
    spdlog::set_pattern("[%D %T] [%^%l%$] [%n] [%t] %v");

    signal(SIGINT, handle);
    signal(SIGTERM, handle);

    auto exit_future = s_stop_code.get_future();

    auto vol_id = (0 < SISL_OPTIONS["vol_id"].count())
        ? boost::uuids::string_generator()(SISL_OPTIONS["vol_id"].as< std::string >())
        : boost::uuids::random_generator()();
    Result res;
    if (0 < SISL_OPTIONS["loop"].count()) {
        res = create_loop(vol_id, SISL_OPTIONS["loop"].as< std::string >());
    } else if (0 < SISL_OPTIONS["raid0"].count()) {
        res = create_raid0(vol_id, SISL_OPTIONS["raid0"].as< std::vector< std::string > >());
    } else if (0 < SISL_OPTIONS["raid1"].count()) {
        res = create_raid1(vol_id, SISL_OPTIONS["raid1"].as< std::vector< std::string > >());
    } else if (0 < SISL_OPTIONS["raid10"].count()) {
        res = create_raid10(vol_id, SISL_OPTIONS["raid10"].as< std::vector< std::string > >());
    } else if (0 < SISL_OPTIONS["craft"].count()) {
        res = create_craft(vol_id, SISL_OPTIONS["craft"].as< uint64_t >());
    } else if (0 < SISL_OPTIONS["craft_tcp"].count()) {
        res = create_craft_tcp(vol_id, SISL_OPTIONS["craft_tcp"].as< std::string >());
    } else
        std::cout << SISL_PARSER.help({}) << std::endl;

    auto http_server = sisl::HttpServer{};
    if (res) {
        try {
            http_server.register_metrics_endpoint();
            http_server.start();
        } catch (std::runtime_error const& e) { LOGERROR("setup routes failed, {}", e.what()) }

        exit_future.wait();
        http_server.stop();
        ublkpp::ublkpp_tgt::remove(std::move(k_target));
    } else
        s_stop_code.set_value(EIO);
    return exit_future.get();
}

static void handle(int signal) {
    switch (signal) {
    case SIGINT:
        [[fallthrough]];
    case SIGTERM: {
        // if multiple SIGTERM are received, the set_value will throw an exception
        try {
            LOGWARN("SIGNAL: {}", strsignal(signal));
            s_stop_code.set_value(signal);
        } catch (std::future_error const& e) { LOGERROR("Failed to set stop code: {}", e.what()); }
    } break;
        ;
    default:
        LOGERROR("Unhandled SIGNAL: {}", strsignal(signal));
        break;
    }
}
