# CMakeLists.txt for the Q Compiler Alpha Test Prototype.
#
# Builds compiler.cpp into a single executable named `qc`.
# Standard library only; no external dependencies.
#
# Build (developer):
#     cmake -S . -B build
#     cmake --build build --config Release
#
# The resulting binary is at build/Release/qc.exe (or build/qc.exe on some
# generators). Invoke as:
#     qc build <filename>.q

cmake_minimum_required(VERSION 3.15)
project(q_compiler_alpha CXX)

# C++17 is sufficient; we use only standard library features.
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Build the compiler executable.
add_executable(qc compiler.cpp)

# Release defaults: optimise for speed, strip nothing (the compiler itself
# is a developer tool; the .exe *it produces* is independent of this).
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# On MSVC: use UTF-8 source encoding and disable the security-CRT warnings
# that would otherwise fire on the raw memory/PE code above.
if(MSVC)
    target_compile_options(qc PRIVATE /utf-8 /W3 /D_CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(qc PRIVATE -Wall -Wextra -Wpedantic -O2)
endif()

# Install target (optional; convenient for local testing).
install(TARGETS qc RUNTIME DESTINATION bin)