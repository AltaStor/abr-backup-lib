// SPDX-License-Identifier: Apache-2.0
// Copyright © 2026 Norvitex OÜ
//
// Shared helpers for the unit tests
//

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace ut
{
    namespace fs = std::filesystem;

    // Whether files carry POSIX permission bits. Windows has none: std::filesystem
    // makes them up from the read-only attribute, so a test about the modes the
    // file backend sets has nothing to check there.
#ifdef _WIN32
    constexpr bool kPosixModes = false;
#else
    constexpr bool kPosixModes = true;
#endif

    inline long processId()
    {
#ifdef _WIN32
        return static_cast<long>(::_getpid());
#else
        return static_cast<long>(::getpid());
#endif
    }

    //
    // Redirects std::cerr for as long as it is alive, so a test can assert on the
    // diagnostic the library produced. It saves and restores whatever buffer was
    // installed before, which lets it nest inside the runner's own silencer.
    //
    class CerrCapture
    {
    public:
        CerrCapture()
            : m_saved(std::cerr.rdbuf(m_buffer.rdbuf()))
        {}

        ~CerrCapture()
        {
            std::cerr.rdbuf(m_saved);
        }

        CerrCapture(const CerrCapture &) = delete;
        CerrCapture &operator=(const CerrCapture &) = delete;

        std::string text() const
        {
            return m_buffer.str();
        }

        bool contains(const std::string &needle) const
        {
            return m_buffer.str().find(needle) != std::string::npos;
        }

        void clear()
        {
            m_buffer.str(std::string());
        }

    private:
        std::ostringstream m_buffer;
        std::streambuf *m_saved;
    };

    //
    // A unique directory under the system temp directory, removed again when the
    // test ends. It is created with the 0750 the file backup manager insists on,
    // so it can be used as a repository without the manager having to fix it up.
    //
    class TempDir
    {
    public:
        static constexpr fs::perms REPOSITORY_PERMS =
            fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec;

        explicit TempDir(fs::perms perms = REPOSITORY_PERMS)
        {
            static std::atomic<unsigned> counter{ 0 };

            m_path = fs::temp_directory_path() /
                     ("abr-unit-" + std::to_string(processId()) + "-" +
                      std::to_string(counter++));

            std::error_code ec;
            fs::remove_all(m_path, ec);
            fs::create_directories(m_path, ec);
            fs::permissions(m_path, perms, fs::perm_options::replace, ec);
        }

        ~TempDir()
        {
            std::error_code ec;

            // A test may have dropped permissions to make an operation fail, so
            // put enough of them back for the tree to be removable.
            fs::permissions(m_path, fs::perms::owner_all, fs::perm_options::add, ec);

            for (auto it = fs::recursive_directory_iterator(
                     m_path, fs::directory_options::skip_permission_denied, ec);
                 !ec && it != fs::recursive_directory_iterator(); ++it) {
                std::error_code entryEc;
                if (it->is_directory(entryEc)) {
                    fs::permissions(it->path(), fs::perms::owner_all, fs::perm_options::add, entryEc);
                }
            }

            fs::remove_all(m_path, ec);
        }

        TempDir(const TempDir &) = delete;
        TempDir &operator=(const TempDir &) = delete;

        const fs::path &path() const
        {
            return m_path;
        }

        std::string str() const
        {
            return m_path.string();
        }

        fs::path operator/(const std::string &child) const
        {
            return m_path / child;
        }

    private:
        fs::path m_path;
    };

    inline std::vector<char> readWholeFile(const fs::path &path)
    {
        std::ifstream file(path, std::ios::in | std::ios::binary);
        return std::vector<char>((std::istreambuf_iterator<char>(file)),
                                 std::istreambuf_iterator<char>());
    }

    inline bool writeWholeFile(const fs::path &path, const std::vector<char> &data)
    {
        std::ofstream file(path, std::ios::out | std::ios::binary | std::ios::trunc);
        file.write(data.data(), static_cast<std::streamsize>(data.size()));
        return file.good();
    }

    // The permission bits of a path, as the three octal digits a mode is usually
    // written in.
    inline unsigned modeOf(const fs::path &path)
    {
        std::error_code ec;
        const fs::perms perms = fs::status(path, ec).permissions();
        return ec ? 0u : (static_cast<unsigned>(perms) & static_cast<unsigned>(fs::perms::all));
    }

#ifndef _WIN32
    inline bool runningAsRoot()
    {
        return ::geteuid() == 0;
    }
#endif

    // Offset of the first byte that is not zero, or -1 when the block is empty.
    inline long firstNonZero(const void *data, size_t length)
    {
        const auto *bytes = static_cast<const unsigned char *>(data);

        for (size_t i = 0; i < length; i++) {
            if (bytes[i] != 0) {
                return static_cast<long>(i);
            }
        }

        return -1;
    }

    // Pseudo-random bytes from a fixed seed, so an incompressible payload is
    // reproducible from run to run.
    inline std::vector<char> randomBuffer(size_t length, uint32_t seed)
    {
        std::vector<char> buf(length, 0);

        // A small xorshift keeps this independent of the standard library's
        // distribution implementations.
        uint32_t state = seed ? seed : 1;
        for (size_t i = 0; i < length; i++) {
            state ^= state << 13;
            state ^= state >> 17;
            state ^= state << 5;
            buf[i] = static_cast<char>(state & 0xFF);
        }

        return buf;
    }

    // A buffer whose first `dataLength` bytes hold a recognisable pattern and
    // whose remainder is zero, which is what the empty block elimination is meant
    // to drop.
    inline std::vector<char> patternBuffer(size_t totalLength, size_t dataLength, char seed = 'A')
    {
        std::vector<char> buf(totalLength, 0);

        for (size_t i = 0; i < dataLength && i < totalLength; i++) {
            buf[i] = static_cast<char>(seed + static_cast<char>(i % 26));
        }

        return buf;
    }
}
