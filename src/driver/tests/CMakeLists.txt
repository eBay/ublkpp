cmake_minimum_required (VERSION 3.11)

enable_testing()
find_package(GTest QUIET REQUIRED)

add_executable(test_fsdisk)
target_sources(test_fsdisk PRIVATE
   test_fsdisk.cpp test_fs_disk_impl.cpp
  $<TARGET_OBJECTS:fs_disk>
  $<TARGET_OBJECTS:ublk_disk>
  $<TARGET_OBJECTS:ublk_metrics>
  $<TARGET_OBJECTS:logging>
)
target_link_libraries (test_fsdisk
  GTest::gmock
  mock_ublksrv
  sisl::cache
  ublksrv::ublksrv
)
add_test(NAME FSDiskTest COMMAND test_fsdisk -cv warning)

add_executable(test_craftdisk)
target_sources(test_craftdisk PRIVATE
   test_craftdisk.cpp
  $<TARGET_OBJECTS:craft_disk>
  $<TARGET_OBJECTS:ublk_disk>
  $<TARGET_OBJECTS:logging>
)
target_link_libraries (test_craftdisk
  GTest::gmock
  sisl::cache
  ublksrv::ublksrv
  craft_client::craft_reference
  mock_ublksrv
)
add_test(NAME CraftDiskTest COMMAND test_craftdisk -cv warning)
