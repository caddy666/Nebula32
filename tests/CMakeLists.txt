cmake_minimum_required(VERSION 3.13)

# Native host compiler — must be set before project() so CMake uses it for
# compiler detection.  This is the x86-64 GCC-15 found on this machine.
# The main CMakeLists.txt (Pico firmware) is unaffected — it uses the
# arm-none-eabi toolchain supplied by the Pico SDK.
set(CMAKE_C_COMPILER /usr/bin/x86_64-linux-gnu-gcc-15
    CACHE STRING "Native C compiler for host-side tests" FORCE)

project(cd32_ode_tests C)
set(CMAKE_C_STANDARD 11)

# Build:  cmake -B build -S tests/   (from repo root)
#         cmake --build tests/build
# Run:    ./tests/build/cd32_tests

# Native host build — not cross-compiled for RP2350.
# Only covers pure-C modules with no hardware dependencies.

include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/stubs          # stub headers override Pico SDK
    ${CMAKE_CURRENT_SOURCE_DIR}/../include     # project headers
)

add_executable(cd32_tests
    run_tests.c
    test_cd_types.c
    test_ecc.c
    test_subcode.c
    test_da_speed.c
    ../src/ecc.c
    ../src/subcode.c
    ../src/da_output.c
)

target_compile_options(cd32_tests PRIVATE
    -Wall -Wextra -Wno-unused-parameter
)
