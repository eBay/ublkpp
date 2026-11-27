cmake_minimum_required (VERSION 3.11)

add_library(ublk_metrics OBJECT)
target_sources(ublk_metrics PRIVATE
    ublk_io_metrics.cpp
    ublk_raid_metrics.cpp
    ublk_craft_metrics.cpp
    ublk_fsdisk_metrics.cpp
)
target_link_libraries(ublk_metrics
    sisl::sisl
    ublksrv::ublksrv
)
