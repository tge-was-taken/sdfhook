cmake_minimum_required(VERSION 3.16...3.25)

# PCSX2's LTO policy, set before pulling the emulator in below.
cmake_policy(SET CMP0069 NEW)
set(CMAKE_POLICY_DEFAULT_CMP0069 NEW)

project(sdfhook C CXX)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

# So an editor's language server can see this project's own sources, not just the vendored tree.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON CACHE BOOL "" FORCE)

# The runner: a PCSX2 fork exposing a C API for hooks and traces. Point PCSX2HOOK_TREE at a checkout
# elsewhere if it is not vendored beside this one.
set(PCSX2HOOK_TREE "${CMAKE_SOURCE_DIR}/vendor/pcsx2-hook" CACHE PATH "pcsx2-hook checkout")
if(NOT EXISTS "${PCSX2HOOK_TREE}/pcsx2-hook/include/pcsx2hook/pcsx2hook.h")
    message(FATAL_ERROR "pcsx2-hook not found at ${PCSX2HOOK_TREE}. Clone it there or set PCSX2HOOK_TREE.")
endif()

# Headless: no Qt front end, and the library rather than the emulator binary.
set(ENABLE_QT_UI OFF CACHE BOOL "" FORCE)
set(PCSX2HOOK_LIBRARY ON CACHE BOOL "" FORCE)
add_subdirectory("${PCSX2HOOK_TREE}" "${CMAKE_BINARY_DIR}/pcsx2-hook")
add_subdirectory(src)
