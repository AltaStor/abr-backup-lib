# SPDX-License-Identifier: Apache-2.0
# Copyright © 2026 Norvitex OÜ (AltaStor.com)
#
# AltaStor backup test utilities

if(ABR_BACKUP_BUILD_TESTS)
    add_subdirectory(btest)
endif()

if(ABR_BACKUP_BUILD_UNIT_TESTS)
    add_subdirectory(unit)
endif()
