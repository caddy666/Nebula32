# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file Copyright.txt or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION 3.5)

file(MAKE_DIRECTORY
  "/home/caddy/.pico-sdk/sdk/2.2.0/tools/pioasm"
  "/home/caddy/pico-claude/Nebula32/build-core2350b/pioasm"
  "/home/caddy/pico-claude/Nebula32/build-core2350b/pioasm-install"
  "/home/caddy/pico-claude/Nebula32/build-core2350b/pico-sdk/src/rp2_common/pico_cyw43_driver/pioasm/tmp"
  "/home/caddy/pico-claude/Nebula32/build-core2350b/pico-sdk/src/rp2_common/pico_cyw43_driver/pioasm/src/pioasmBuild-stamp"
  "/home/caddy/pico-claude/Nebula32/build-core2350b/pico-sdk/src/rp2_common/pico_cyw43_driver/pioasm/src"
  "/home/caddy/pico-claude/Nebula32/build-core2350b/pico-sdk/src/rp2_common/pico_cyw43_driver/pioasm/src/pioasmBuild-stamp"
)

set(configSubDirs )
foreach(subDir IN LISTS configSubDirs)
    file(MAKE_DIRECTORY "/home/caddy/pico-claude/Nebula32/build-core2350b/pico-sdk/src/rp2_common/pico_cyw43_driver/pioasm/src/pioasmBuild-stamp/${subDir}")
endforeach()
if(cfgdir)
  file(MAKE_DIRECTORY "/home/caddy/pico-claude/Nebula32/build-core2350b/pico-sdk/src/rp2_common/pico_cyw43_driver/pioasm/src/pioasmBuild-stamp${cfgdir}") # cfgdir has leading slash
endif()
