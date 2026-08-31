// SPDX-License-Identifier: Apache-2.0
// Copyright © 2026 Norvitex OÜ
//
// Unit test entry point
//

#include <gtest/gtest.h>

#include <iostream>
#include <sstream>
#include <string>

namespace
{
    //
    // The library reports every error on std::cerr, and a large part of this
    // suite drives it into exactly those paths on purpose. Their diagnostics are
    // held back per test and printed only for a test that failed, so a passing
    // run stays readable. --abr_show_stderr prints them for every test.
    //
    class CerrSilencer : public testing::EmptyTestEventListener
    {
    public:
        explicit CerrSilencer(bool alwaysShow)
            : m_alwaysShow(alwaysShow)
        {}

        void OnTestStart(const testing::TestInfo &) override
        {
            m_buffer.str(std::string());
            m_saved = std::cerr.rdbuf(m_buffer.rdbuf());
        }

        void OnTestEnd(const testing::TestInfo &testInfo) override
        {
            if (m_saved) {
                std::cerr.rdbuf(m_saved);
                m_saved = nullptr;
            }

            const std::string text = m_buffer.str();
            if (text.empty()) {
                return;
            }

            if (m_alwaysShow || testInfo.result()->Failed()) {
                std::cout << "--- library diagnostics ---\n" << text
                          << "---------------------------" << std::endl;
            }
        }

    private:
        std::ostringstream m_buffer;
        std::streambuf *m_saved = nullptr;
        bool m_alwaysShow;
    };
}

int main(int argc, char **argv)
{
    testing::InitGoogleTest(&argc, argv);

    bool showStderr = false;

    for (int i = 1; i < argc; i++) {
        if (std::string(argv[i]) == "--abr_show_stderr") {
            showStderr = true;
        }
    }

    testing::UnitTest::GetInstance()->listeners().Append(new CerrSilencer(showStderr));

    return RUN_ALL_TESTS();
}
