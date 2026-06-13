# CMake generated Testfile for 
# Source directory: /home/caddy/pico-claude/Nebula32/libs/wolfssl-master
# Build directory: /home/caddy/pico-claude/Nebula32/build-core2350b/libs/wolfssl-master
# 
# This file includes the relevant testing commands required for 
# testing this directory and lists subdirectories to be tested as well.
add_test(unit_test "/home/caddy/pico-claude/Nebula32/build-core2350b/libs/wolfssl-master/tests/unit.test.elf")
set_tests_properties(unit_test PROPERTIES  WORKING_DIRECTORY "/home/caddy/pico-claude/Nebula32/libs/wolfssl-master" _BACKTRACE_TRIPLES "/home/caddy/pico-claude/Nebula32/libs/wolfssl-master/CMakeLists.txt;3014;add_test;/home/caddy/pico-claude/Nebula32/libs/wolfssl-master/CMakeLists.txt;0;")
add_test(wolfcrypttest "/home/caddy/pico-claude/Nebula32/build-core2350b/libs/wolfssl-master/wolfcrypt/test/testwolfcrypt.elf")
set_tests_properties(wolfcrypttest PROPERTIES  WORKING_DIRECTORY "/home/caddy/pico-claude/Nebula32/libs/wolfssl-master" _BACKTRACE_TRIPLES "/home/caddy/pico-claude/Nebula32/libs/wolfssl-master/CMakeLists.txt;3059;add_test;/home/caddy/pico-claude/Nebula32/libs/wolfssl-master/CMakeLists.txt;0;")
