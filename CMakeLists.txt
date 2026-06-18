# -*- coding: utf-8 -*-
# @brief Build file for Audacious Discord RPC plugin
#
# @author onegen <onegen@onegen.dev>
# @author Derzsi Dániel <daniel@tohka.us>
# @author Sturmlilie
#
# @code{.sh}
# cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
# cmake --build build -j
# @endcode
#

cmake_minimum_required(VERSION 3.10)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)

if(WIN32)
  execute_process(COMMAND uname OUTPUT_VARIABLE uname)
  if (NOT (uname MATCHES "^MSYS" OR uname MATCHES "^MINGW"))
    message(FATAL_ERROR "Use MINGW64 for Windows compilation.")
  endif()
endif()

include(FetchContent)
include(FindPkgConfig)
include(ExternalProject)
find_package(Git REQUIRED)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()


# === DEFINITION === #

project(
  audacious-discord-rpc
    VERSION 2.4
    HOMEPAGE_URL https://github.com/onegen-dev/audacious-discord-rpc
    LANGUAGES CXX
)

file(GLOB_RECURSE SOURCES "src/*.cpp")
add_library(audacious-discord-rpc SHARED ${SOURCES})
target_include_directories(audacious-discord-rpc PRIVATE "include")
target_compile_features(audacious-discord-rpc PUBLIC cxx_std_23)

# == Compilation Options == #
target_compile_options(audacious-discord-rpc
  PRIVATE
    # Warnings
    -Wall
    -Wextra
    -Wpedantic

    # Generic x86_64_v2 microarchitecture as minimum (#10#issuecomment-4596109645)
    -march=x86-64-v2
    -mtune=generic

    # Release
    # @note Safety hardening flags from systemshardening.com
    # @see https://www.systemshardening.com/articles/linux/linux-memory-protections
    $<$<CONFIG:Release>:
      -DNDEBUG                    # Do Not DEBUG?
      -Ofast                      # Gotta go fast!
      -fstack-protector-strong    # Protecting stacks with buffers
      -fstack-clash-protection    # Probing stacks to prevent clashes
      -fcf-protection=full        # Intel CET control-flow integrity
      # This should remove paths from the source code
      -fdebug-prefix-map=${CMAKE_SOURCE_DIR}=..
      -ffile-prefix-map=${CMAKE_SOURCE_DIR}=..
      $<$<NOT:$<BOOL:${WIN32}>>:  # Linux-only flags
        -D_FORTIFY_SOURCE=2       # Safety-hardened buffers, hardened libc bounds
      >
    >

    # Debug (all the debug symbols!)
    $<$<CONFIG:Debug>:
      -g
      -Og                   # Optimise for debugging
      -fsanitize=undefined  # UBSan
      $<$<NOT:$<BOOL:${WIN32}>>:  # Linux-only flags
        -D_GLIBCXX_ASSERTIONS     # C++ STL boundary checks
      >
    >

  -fPIC # Position-independant code
)

target_link_options(audacious-discord-rpc
  PRIVATE
    -pie  # Position-independant executable

    $<$<AND:$<CONFIG:Release>,$<NOT:$<BOOL:${WIN32}>>>:
      -Wl,-z,relro,-z,now   # Full RELRO
      -Wl,-z,noexecstack    # Non-executable stack
      -Wl,-z,separate-code  # Separate data and code
    >
)

# === DEPENDENCIES === #

# == Audacious (audcore) == #
#
# If this fails, make sure your system has the Audacious
# development package installed, e.g. `audacious-dev` (DEB)
# or `audacious-devel` (RPM).
#
# There is no such package on Windows, so this process involves
# compiling Audacious for its headers and linkage.
# The whole process is somewhat experimental.
#

if(WIN32)
  set(audacious_VERSION "4.6.1")
  FILE(DOWNLOAD "https://distfiles.audacious-media-player.org/audacious-${audacious_VERSION}.tar.bz2"
    "${CMAKE_BINARY_DIR}/_deps/audacious.tar.bz2"
    SHOW_PROGRESS
  )
  FILE(ARCHIVE_EXTRACT
    INPUT "${CMAKE_BINARY_DIR}/_deps/audacious.tar.bz2"
    DESTINATION "_deps/audacious"
  )
  FILE(COPY "${CMAKE_BINARY_DIR}/_deps/audacious/audacious-${audacious_VERSION}/"
    DESTINATION "_deps/audacious-src"
    FOLLOW_SYMLINK_CHAIN
  )
  FILE(REMOVE_RECURSE "${CMAKE_BINARY_DIR}/_deps/audacious")
  FILE(REMOVE "${CMAKE_BINARY_DIR}/_deps/audacious.tar.bz2")
  ExternalProject_Add(audacious
    SOURCE_DIR "${CMAKE_BINARY_DIR}/_deps/audacious-src"
    CONFIGURE_COMMAND bash -c "./configure --disable-qt --disable-gtk"
    BUILD_COMMAND make
    INSTALL_COMMAND ""
    BUILD_IN_SOURCE TRUE
  )
else()
  PKG_SEARCH_MODULE(AUDACIOUS REQUIRED audacious>=4.6)
  PKG_GET_VARIABLE(AUDACIOUS_PLUGIN_DIR audacious plugin_dir)
endif()

# == Glaze (for JSON deserialization) == #

FetchContent_Declare(
  glaze
    GIT_REPOSITORY https://github.com/stephenberry/glaze.git
    GIT_TAG "v7.8.1"
    GIT_PROGRESS TRUE
    GIT_SHALLOW TRUE
)

FetchContent_MakeAvailable(glaze)

# == O. Nemeš’s Discord RPC == #

if(WIN32)
  # NOMINMAX is defined by MinGW libstdc++ headers
  # and Discord RPC library redefines it in windows.hpp.
  # This is a patch that lets it compile anyway.
  add_definitions("-DNOMINMAX")
endif()

FetchContent_Declare(
  discord-rpc
    GIT_REPOSITORY https://github.com/EclipseMenu/discord-presence.git
    GIT_TAG 3b50b0854ec24f9b2d4cc8e28f9145f276bb998e
    GIT_PROGRESS TRUE
    UPDATE_DISCONNECTED ON
)

FetchContent_MakeAvailable(discord-rpc)
if(IS_DIRECTORY "${discord-rpc_SOURCE_DIR}")
  set_property(
    DIRECTORY ${discord-rpc_SOURCE_DIR}
    PROPERTY EXCLUDE_FROM_ALL YES
  )
endif()

# == cURL == #
#
# cURL is not included in the Windows installed package, so
# on most environments it would fail to link. Thus,
# cURL requiring capability of cover art fetching is disabled there.
#

if(WIN32)
  set(DISABLE_RPC_CAF FALSE)
  set(CURL_FOUND FALSE)
  message(STATUS "WIN build: WinHTTP will be used for cover art fetching.")
else()
  find_package(CURL QUIET)
  if(NOT CURL_FOUND)
    set(DISABLE_RPC_CAF TRUE)
    target_compile_definitions(audacious-discord-rpc PRIVATE DISABLE_RPC_CAF=1)
    message(WARNING "LIN build, no cURL: cover art fetching disabled.")
  else()
    set(DISABLE_RPC_CAF FALSE)
    message(STATUS "LIN build, found cURL: cURL will be used for cover art fetching.")
  endif()
endif()

# === LINKING === #

if(WIN32)
  # Self-built Audacious on Windows
  add_dependencies(audacious-discord-rpc audacious)
  target_include_directories(audacious-discord-rpc PRIVATE "${CMAKE_BINARY_DIR}/_deps/audacious-src/src")
  target_link_directories(audacious-discord-rpc PRIVATE "${CMAKE_BINARY_DIR}/_deps/audacious-src/src/libaudcore")
  target_link_libraries(audacious-discord-rpc PRIVATE audcore)
endif()

target_include_directories(audacious-discord-rpc PRIVATE
  "${discord-rpc_SOURCE_DIR}/include"
  "${glaze_SOURCE_DIR}/include"
)

target_link_libraries(audacious-discord-rpc PRIVATE
  glaze::glaze
  discord-rpc
)

if(NOT DISABLE_RPC_CAF)
  if(WIN32)
    target_link_libraries(audacious-discord-rpc PRIVATE winhttp)
  elseif(CURL_FOUND)
    target_link_libraries(audacious-discord-rpc PRIVATE CURL::libcurl)
  endif()
endif()

if(CURL_FOUND AND NOT DISABLE_RPC_CAF)
endif()

set_target_properties(audacious-discord-rpc
  PROPERTIES
    PREFIX ""
    OUTPUT_NAME "discord-rpc"
)

# === INSTALL OPTIONS === #

if(WIN32)
  # No simple app location exposure on Windows -> use default install dir
  set(INSTALL_DIR "C:/Program Files (x86)/Audacious/lib/audacious/General")
else()
  # pkg-config should have this info
  set(INSTALL_DIR "${AUDACIOUS_PLUGIN_DIR}/General")
endif()

install(
  TARGETS audacious-discord-rpc
  LIBRARY DESTINATION ${INSTALL_DIR}
)
