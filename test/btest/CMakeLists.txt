# AltaStor backup test utility

add_executable(btest
    btest.cpp
)

target_link_libraries(btest
    PRIVATE
        backuplib
)
