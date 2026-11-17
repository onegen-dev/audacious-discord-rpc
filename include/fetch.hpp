/**
 * @file fetch.hpp
 * @brief HTTP fetcher header for Audacious-Discord-RPC.
 * @author onegen <onegen@onegen.dev>
 * @date 2026-11-17 (last modified)
 *
 * @note This header provides a unified header for ‘fetch-{}.cpp’.
 *       The intent is that multiple ways of fetching with different
 *       executing frameworks can be implemented, like WinHTTP
 *       for Windows, then cURL, Boost or something on Linux etc, and
 *       CMake selects one based on given rules at its linking step
 *       (aka link-time polymorphism; for actual implementations
 *       see CMakeLists.txt).
 *
 * @license MIT
 * @copyright Copyright (c) 2025–2026 onegen
 *
 */

#pragma once

#include <stop_token>
#include <string>
#include <utility>
#include <vector>

/** Timeout */
constexpr unsigned long FETCH_TIMEO = 15000;  // [ms]
/** User-Agent */
inline constexpr const char* UA
    = "Audacious-Discord-RPC/master "
      "(+https://github.com/onegen-dev/audacious-discord-rpc)";

/** Headers type (a vector of string×string pairs) */
using Headers = std::vector<std::pair<std::string, std::string>>;

/** Struct encapsulating the fetch response */
struct FetchRes {
     unsigned int status_http = 0;  // HTTP status code
     unsigned int status_exe = 0;   // Executor status code
     std::string data;              // Response data, if applicable
};

FetchRes fetch(const std::string& url, const Headers& headers = {},
               const std::string& method = "GET", const std::string& body = "",
               const std::stop_token& stopper = {});
