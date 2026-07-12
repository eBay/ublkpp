cmake_minimum_required (VERSION 3.11)

add_library(fs_disk OBJECT)
target_sources(fs_disk PRIVATE
    fs_disk.cpp
)
target_link_libraries(fs_disk
    sisl::cache
    ublksrv::ublksrv
)

# craft_disk: a CRAFT-backed leaf disk. Links craft_reference (the whole craft_client chain) for the in-process
# make_local_cluster factory; the TCP factory only needs craft_client, which reference pulls in.
add_library(craft_disk OBJECT)
target_sources(craft_disk PRIVATE
    craft_disk.cpp
)
target_link_libraries(craft_disk
    sisl::cache
    ublksrv::ublksrv
    craft_client::craft_reference
)


include_directories(BEFORE ${CMAKE_CURRENT_SOURCE_DIR})

if ((DEFINED ENABLE_TESTS) AND (${ENABLE_TESTS}))
add_subdirectory (tests)
endif()
