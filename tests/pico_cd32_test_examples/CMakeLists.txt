
cmake_minimum_required(VERSION 3.16)
project(cd32_emulator_tests)

set(CMAKE_CXX_STANDARD 17)

add_executable(cd32_tests
    tests/test_command_parser.cpp
    tests/test_sector_builder.cpp
    tests/test_qsubcode.cpp
    tests/test_ringbuffer.cpp
    tests/test_dma_sim.cpp
    tests/test_trace_replay.cpp
    tests/test_fuzz.cpp
)

target_include_directories(cd32_tests PRIVATE
    include
    third_party
)
