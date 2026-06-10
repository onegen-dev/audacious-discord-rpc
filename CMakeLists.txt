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

# === COMPILER OPTS === #

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

add_compile_options(-Wall -Wextra -Wpedantic)
set(CMAKE_CXX_FLAGS_RELEASE "-DNDEBUG -Ofast -march=x86-64-v2 -mtune=generic")
set(CMAKE_CXX_FLAGS_DEBUG "-g -Og -fsanitize=undefined -march=x86-64-v2 -mtune=generic")

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

# == N. Lohmann's C++ JSON library == #

FetchContent_Declare(
  nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3
    GIT_PROGRESS TRUE
)

FetchContent_MakeAvailable(nlohmann_json)

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
  "${nlohmann_json_SOURCE_DIR}/include"
)

target_link_libraries(audacious-discord-rpc PRIVATE
  discord-rpc
  nlohmann_json::nlohmann_json
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
