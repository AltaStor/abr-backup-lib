cmake_minimum_required(VERSION 3.20)

project(abr-backup-lib LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# If no build type is specified for single-config generators, default to Debug
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    message(STATUS "No build type specified. Defaulting to Debug.")
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

option(ABR_BACKUP_BUILD_TESTS "Build the btest test utility" ON)
option(ABR_BACKUP_BUILD_UNIT_TESTS "Build the GoogleTest unit tests" ON)

# GoogleTest release used when the build has to fetch it, see test/unit
set(ABR_GTEST_VERSION "1.16.0" CACHE STRING "GoogleTest version to fetch when none is installed")
set(ABR_GTEST_SHA256 "78c676fc63881529bf97bf9d45948d905a66833fbfa5318ea2cd7478cb98f399"
    CACHE STRING "SHA256 of the fetched GoogleTest release archive")

find_package(AWSSDK CONFIG REQUIRED COMPONENTS core s3)
find_package(OpenSSL REQUIRED)
find_package(zstd CONFIG QUIET)
find_package(Crc32c CONFIG QUIET)

set(AWS_S3_LINK_TARGETS "")
if(AWSSDK_LINK_LIBRARIES)
    list(APPEND AWS_S3_LINK_TARGETS ${AWSSDK_LINK_LIBRARIES})
elseif(TARGET aws-cpp-sdk-s3)
    list(APPEND AWS_S3_LINK_TARGETS aws-cpp-sdk-s3)
elseif(TARGET AWS::aws-cpp-sdk-s3)
    list(APPEND AWS_S3_LINK_TARGETS AWS::aws-cpp-sdk-s3)
endif()

# zstd ships either a CMake package (vcpkg) or a plain library (distro packages)
if(TARGET zstd::libzstd_shared)
    set(ZSTD_LINK_TARGET zstd::libzstd_shared)
elseif(TARGET zstd::libzstd_static)
    set(ZSTD_LINK_TARGET zstd::libzstd_static)
elseif(TARGET zstd::libzstd)
    set(ZSTD_LINK_TARGET zstd::libzstd)
else()
    find_library(ZSTD_LIBRARY NAMES zstd REQUIRED)
    set(ZSTD_LINK_TARGET ${ZSTD_LIBRARY})
endif()

# crc32c likewise: imported target when available, then a plain system
# lookup, and finally the bundled third_party/crc32c submodule as a last
# resort so the build works without any crc32c installed on the system.
if(TARGET Crc32c::crc32c)
    set(CRC32C_LINK_TARGET Crc32c::crc32c)
else()
    find_library(CRC32C_LIBRARY NAMES crc32c)
    find_path(CRC32C_INCLUDE_DIR NAMES crc32c/crc32c.h)
    if(CRC32C_LIBRARY AND CRC32C_INCLUDE_DIR)
        set(CRC32C_LINK_TARGET ${CRC32C_LIBRARY})
    else()
        if(NOT EXISTS "${CMAKE_CURRENT_LIST_DIR}/third_party/crc32c/CMakeLists.txt")
            message(FATAL_ERROR
                "No crc32c installation was found and the bundled "
                "third_party/crc32c submodule is empty. Run "
                "'git submodule update --init --recursive' and reconfigure.")
        endif()

        # Only build crc32c's own library; skip its tests/benchmarks, whose
        # nested submodules (googletest, glog, benchmark) are not checked out.
        set(CRC32C_BUILD_TESTS OFF CACHE BOOL "" FORCE)
        set(CRC32C_BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)
        set(CRC32C_USE_GLOG OFF CACHE BOOL "" FORCE)
        set(CRC32C_INSTALL OFF CACHE BOOL "" FORCE)
        add_subdirectory(third_party/crc32c EXCLUDE_FROM_ALL)
        set(CRC32C_LINK_TARGET crc32c)
    endif()
endif()

add_library(backuplib STATIC
    src/file_backup.cpp
    src/s3_backup.cpp
    src/backup_core.cpp
    src/abr_backup.cpp
)

target_include_directories(backuplib
    PUBLIC
        ${CMAKE_CURRENT_LIST_DIR}/inc
    PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}/src
)

if(CRC32C_INCLUDE_DIR)
    target_include_directories(backuplib PRIVATE ${CRC32C_INCLUDE_DIR})
endif()

target_link_libraries(backuplib
    PUBLIC
        ${AWS_S3_LINK_TARGETS}
    PRIVATE
        ${ZSTD_LINK_TARGET}
        ${CRC32C_LINK_TARGET}
        OpenSSL::Crypto
)

if(ABR_BACKUP_BUILD_TESTS OR ABR_BACKUP_BUILD_UNIT_TESTS)
    enable_testing()
    add_subdirectory(test)
endif()
