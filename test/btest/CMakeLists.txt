# SPDX-License-Identifier: Apache-2.0
# Copyright © 2026 Norvitex OÜ (AltaStor.com)
#
# AltaStor backup test utility

add_executable(btest
    btest.cpp
)

target_link_libraries(btest
    PRIVATE
        backuplib
)
