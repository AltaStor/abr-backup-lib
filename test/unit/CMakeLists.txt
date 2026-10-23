# AltaStor backup library unit tests

# Prefer a GoogleTest that is already installed; fall back to a pinned release,
# which needs network access at configure time.
find_package(GTest QUIET)

if(NOT TARGET GTest::gtest)
    message(STATUS "GoogleTest not found, fetching v${ABR_GTEST_VERSION}")

    include(FetchContent)

    # GoogleTest must not join the install set of this project, and on Windows it
    # has to use the same CRT as the rest of the build.
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)

    FetchContent_Declare(googletest
        URL      https://github.com/google/googletest/archive/refs/tags/v${ABR_GTEST_VERSION}.tar.gz
        URL_HASH SHA256=${ABR_GTEST_SHA256}
        DOWNLOAD_EXTRACT_TIMESTAMP FALSE
    )

    FetchContent_MakeAvailable(googletest)
endif()

add_executable(abr_unit_tests
    unit_main.cpp
    test_join_backup_path.cpp
    test_index_format.cpp
    test_backup_core.cpp
    test_restore_core.cpp
    test_compression.cpp
    test_encryption.cpp
    test_file_backup_manager.cpp
    test_backup_manager_factory.cpp
    test_immutability.cpp
)

target_include_directories(abr_unit_tests
    PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}
        # backup_core.h is internal to the library, the core tests drive it directly
        ${PROJECT_SOURCE_DIR}/src
)

if(CRC32C_INCLUDE_DIR)
    target_include_directories(abr_unit_tests PRIVATE ${CRC32C_INCLUDE_DIR})
endif()

target_link_libraries(abr_unit_tests
    PRIVATE
        backuplib
        GTest::gtest
        # The tests recompute the checksums the library stored, and decrypt a
        # stored chunk by hand, instead of trusting the library to agree with
        # itself.
        ${CRC32C_LINK_TARGET}
        OpenSSL::Crypto
)

include(GoogleTest)

# DISCOVERY_TIMEOUT covers the first run of the binary on a cold build. The
# per-test TIMEOUT matters more than it looks: a defect that stops the library
# from making progress shows up as a test that never returns, and this turns
# that into a reported failure instead of a build that hangs.
gtest_discover_tests(abr_unit_tests
    DISCOVERY_TIMEOUT 60
    PROPERTIES
        LABELS unit
        TIMEOUT 60
)
