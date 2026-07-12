cmake_minimum_required(VERSION 3.11)

find_package(fio QUIET)
# FIO_EXECUTABLE may be injected by conanfile.py (pointing to the Conan-provided
# binary that matches the headers we compiled against). Fall back to find_program.
if (NOT FIO_EXECUTABLE)
    find_program(FIO_EXECUTABLE fio)
endif()

if (NOT fio_FOUND OR NOT FIO_EXECUTABLE)
    message(STATUS "fio package/binary not found — skipping functional tests")
    return()
endif()

# ThreadSanitizer instruments the engine .so but fio is not a TSAN binary.
# TSAN requires being the very first library in the process and cannot be
# loaded late via dlopen, so fio would fail to open the .so at runtime.
# The functional tests are already covered by Debug and ASan builds.
if (DEFINED THREAD_SANITIZER_ON AND THREAD_SANITIZER_ON)
    message(STATUS "ThreadSanitizer build — skipping functional tests (TSAN cannot be dlopen'd into a non-TSAN fio binary)")
    return()
endif()

# Locate fio headers: Conan sets fio_INCLUDE_DIRS; fall back to a direct search
if (NOT fio_INCLUDE_DIRS)
    find_path(FIO_INCLUDE_BASE NAMES fio/ioengines.h)
    if (NOT FIO_INCLUDE_BASE)
        message(STATUS "fio/ioengines.h not found — skipping functional tests")
        return()
    endif()
    set(fio_INCLUDE_DIRS ${FIO_INCLUDE_BASE})
endif()

# ---------------------------------------------------------------------------
# Shared library: libublkpp_fio_engine.so
# ---------------------------------------------------------------------------
add_library(ublkpp_fio_engine SHARED)
target_sources(ublkpp_fio_engine PRIVATE
    ublkpp_fio_engine.cpp
)
target_link_libraries(ublkpp_fio_engine PRIVATE
    mock_ublksrv
    ublkpp
    ublksrv::ublksrv
    sisl::cache
    craft_client::craft_reference # make_craft_disk_local -> make_local_cluster (in-process CRAFT reference)
    $<$<PLATFORM_ID:Linux>:atomic>
)
target_include_directories(ublkpp_fio_engine PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${fio_INCLUDE_DIRS}
)
# Suppress warnings from fio headers and allow C-style casts common in fio APIs
target_compile_options(ublkpp_fio_engine PRIVATE
    -Wno-error
    -Wno-pedantic
)
if (DEFINED BUILD_COVERAGE AND BUILD_COVERAGE)
    target_compile_definitions(ublkpp_fio_engine PRIVATE BUILD_COVERAGE)
endif()
set_target_properties(ublkpp_fio_engine PROPERTIES
    PREFIX ""   # produce libublkpp_fio_engine.so (fio uses the full filename)
)

# ---------------------------------------------------------------------------
# CTest functional tests
# ---------------------------------------------------------------------------
set(ENGINE_SO $<TARGET_FILE:ublkpp_fio_engine>)
set(TEST_DIR ${CMAKE_CURRENT_BINARY_DIR}/test_data)
file(MAKE_DIRECTORY ${TEST_DIR})

# Under ASan the engine .so carries the ASan runtime as a direct dependency
# (it was compiled with -fsanitize=address), so fio's dynamic linker loads
# libasan.so when it dlopen's our .so.  At that point libstdc++ is already
# present, so the __cxa_throw interceptor can resolve the real symbol.
# We only need to suppress ASan's "runtime not first" link-order warning.
set(FUNCTIONAL_ENV "")
if (DEFINED ADDRESS_SANITIZER_ON AND ADDRESS_SANITIZER_ON)
    list(APPEND FUNCTIONAL_ENV "ASAN_OPTIONS=verify_asan_link_order=0")
endif()

# tcmalloc (used by sisl in Release builds) is statically linked into our .so
# with the initial-exec TLS model (~88 KiB of per-thread cache storage).
# When fio dlopen's our .so at runtime the static TLS block is already
# finalised, so glibc reports "cannot allocate memory in static TLS block".
# Work-around: LD_PRELOAD the engine .so so the dynamic linker allocates its
# TLS in the initial (startup) block before fio's own code runs.  When fio
# later dlopen's the same path the linker returns the already-loaded handle
# with no second TLS allocation needed.
if (DEFINED TCMALLOC_ON AND TCMALLOC_ON)
    list(APPEND FUNCTIONAL_ENV "LD_PRELOAD=${ENGINE_SO}")
endif()

# Setup fixture: remove stale backing images before each functional test so
# a fresh superblock is written with the current deterministic UUID.
add_test(NAME FunctionalCleanup
    COMMAND ${CMAKE_COMMAND} -E rm -f
        ${TEST_DIR}/fsdisk.img
        ${TEST_DIR}/raid0_a.img ${TEST_DIR}/raid0_b.img
        ${TEST_DIR}/raid1_a.img ${TEST_DIR}/raid1_b.img
        ${TEST_DIR}/raid10_a.img ${TEST_DIR}/raid10_b.img
        ${TEST_DIR}/raid10_c.img ${TEST_DIR}/raid10_d.img
        ${TEST_DIR}/raid0xs_a.img ${TEST_DIR}/raid0xs_b.img
        ${TEST_DIR}/raid10xs_a.img ${TEST_DIR}/raid10xs_b.img
        ${TEST_DIR}/raid10xs_c.img ${TEST_DIR}/raid10xs_d.img
)
set_tests_properties(FunctionalCleanup PROPERTIES
    FIXTURES_SETUP   FunctionalImages
    LABELS           "Functional"
)

set(FIO_BASE_ARGS
    --output-format=normal
    --allow_file_create=0
)

# --- FSDisk ---
add_test(NAME FunctionalFSDisk
    COMMAND ${FIO_EXECUTABLE} ${FIO_BASE_ARGS}
        ${CMAKE_CURRENT_SOURCE_DIR}/jobs/fsdisk_rw.fio
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
)
set(FSDISK_ENV  "ENGINE_SO=${ENGINE_SO}" "TEST_FILE=${TEST_DIR}/fsdisk.img"                 ${FUNCTIONAL_ENV})
set(RAID0_ENV   "ENGINE_SO=${ENGINE_SO}" "TEST_FILE_A=${TEST_DIR}/raid0_a.img"
                "TEST_FILE_B=${TEST_DIR}/raid0_b.img" ${FUNCTIONAL_ENV})
set(RAID1_ENV   "ENGINE_SO=${ENGINE_SO}" "TEST_FILE_A=${TEST_DIR}/raid1_a.img"
                "TEST_FILE_B=${TEST_DIR}/raid1_b.img" ${FUNCTIONAL_ENV})
set(RAID10_ENV  "ENGINE_SO=${ENGINE_SO}" "TEST_FILE_A=${TEST_DIR}/raid10_a.img"
                "TEST_FILE_B=${TEST_DIR}/raid10_b.img" "TEST_FILE_C=${TEST_DIR}/raid10_c.img"
                "TEST_FILE_D=${TEST_DIR}/raid10_d.img" ${FUNCTIONAL_ENV})
set(RAID0XS_ENV  "ENGINE_SO=${ENGINE_SO}" "TEST_FILE_A=${TEST_DIR}/raid0xs_a.img"
                 "TEST_FILE_B=${TEST_DIR}/raid0xs_b.img" ${FUNCTIONAL_ENV})
set(RAID10XS_ENV "ENGINE_SO=${ENGINE_SO}" "TEST_FILE_A=${TEST_DIR}/raid10xs_a.img"
                 "TEST_FILE_B=${TEST_DIR}/raid10xs_b.img" "TEST_FILE_C=${TEST_DIR}/raid10xs_c.img"
                 "TEST_FILE_D=${TEST_DIR}/raid10xs_d.img" ${FUNCTIONAL_ENV})

set_tests_properties(FunctionalFSDisk PROPERTIES
    ENVIRONMENT "${FSDISK_ENV}"
    FIXTURES_REQUIRED FunctionalImages
    TIMEOUT 120
    LABELS "Functional"
)

# --- RAID0 ---
add_test(NAME FunctionalRAID0
    COMMAND ${FIO_EXECUTABLE} ${FIO_BASE_ARGS}
        ${CMAKE_CURRENT_SOURCE_DIR}/jobs/raid0_rw.fio
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
)
set_tests_properties(FunctionalRAID0 PROPERTIES
    ENVIRONMENT "${RAID0_ENV}"
    FIXTURES_REQUIRED FunctionalImages
    TIMEOUT 120
    LABELS "Functional"
)

# --- RAID1 ---
add_test(NAME FunctionalRAID1
    COMMAND ${FIO_EXECUTABLE} ${FIO_BASE_ARGS}
        ${CMAKE_CURRENT_SOURCE_DIR}/jobs/raid1_rw.fio
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
)
set_tests_properties(FunctionalRAID1 PROPERTIES
    ENVIRONMENT "${RAID1_ENV}"
    FIXTURES_REQUIRED FunctionalImages
    TIMEOUT 180
    LABELS "Functional"
)

# --- RAID10 ---
add_test(NAME FunctionalRAID10
    COMMAND ${FIO_EXECUTABLE} ${FIO_BASE_ARGS}
        ${CMAKE_CURRENT_SOURCE_DIR}/jobs/raid10_rw.fio
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
)
set_tests_properties(FunctionalRAID10 PROPERTIES
    ENVIRONMENT "${RAID10_ENV}"
    FIXTURES_REQUIRED FunctionalImages
    TIMEOUT 180
    LABELS "Functional"
)

# --- RAID0 cross-stripe (bs=96k forces iovec accumulation in __distribute) ---
add_test(NAME FunctionalRAID0CrossStripe
    COMMAND ${FIO_EXECUTABLE} ${FIO_BASE_ARGS}
        ${CMAKE_CURRENT_SOURCE_DIR}/jobs/raid0_cross_stripe.fio
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
)
set_tests_properties(FunctionalRAID0CrossStripe PROPERTIES
    ENVIRONMENT "${RAID0XS_ENV}"
    FIXTURES_REQUIRED FunctionalImages
    TIMEOUT 180
    LABELS "Functional"
)

# --- RAID10 cross-stripe (same pattern through RAID0+RAID1 stack) ---
add_test(NAME FunctionalRAID10CrossStripe
    COMMAND ${FIO_EXECUTABLE} ${FIO_BASE_ARGS}
        ${CMAKE_CURRENT_SOURCE_DIR}/jobs/raid10_cross_stripe.fio
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
)
set_tests_properties(FunctionalRAID10CrossStripe PROPERTIES
    ENVIRONMENT "${RAID10XS_ENV}"
    FIXTURES_REQUIRED FunctionalImages
    TIMEOUT 180
    LABELS "Functional"
)

# --- CRAFT (in-process reference cluster: no backing files, no fixture) — the on-ring transport at depth ---
add_test(NAME FunctionalCraft
    COMMAND ${FIO_EXECUTABLE} ${FIO_BASE_ARGS}
        ${CMAKE_CURRENT_SOURCE_DIR}/jobs/craft_verify.fio
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
)
set(CRAFT_ENV "ENGINE_SO=${ENGINE_SO}" ${FUNCTIONAL_ENV})
set_tests_properties(FunctionalCraft PROPERTIES
    ENVIRONMENT "${CRAFT_ENV}"
    TIMEOUT 180
    LABELS "Functional"
)

# ---------------------------------------------------------------------------
# Convenience target: cmake --build <dir> --target functional
# Runs only Functional-labelled tests without re-invoking Conan.
# ---------------------------------------------------------------------------
add_custom_target(functional
    COMMAND ${CMAKE_CTEST_COMMAND}
        --test-dir ${CMAKE_BINARY_DIR}
        -L Functional
        --output-on-failure
        --verbose
    USES_TERMINAL
    COMMENT "Running functional fio tests"
)
add_dependencies(functional ublkpp_fio_engine)
