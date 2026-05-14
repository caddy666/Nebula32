# Raspberry Pi Pico Interfaces

This library includes the interfaces used by some other libraries, for example the mcp23017 and latching-switch

# Usage

## Include library

Check the module out as a submodule of your project to say: modules/pico-mcp23017 then
add the sub directory and link the library in your CMakeLists.txt:
```
add_subdirectory(modules/pico-interfaces)
add_subdirectory(modules/pico-mcp23017)
add_subdirectory(modules/pico-latching-switch)

add_dependencies(pico_latching_switch pico_interfaces)

target_link_libraries( ... pico_mcp23017 pico_latching_switch)
```
