/**
 * @file fetch-noop.cpp
 * @brief NOOP (no operation) HTTP fetched for Audacious-Discord-RPC
 *        to use when no HTTP executing library is found/selected.
 * @author onegen <onegen@onegen.dev>
 * @date 2026-11-17 (last modified)
 *
 * @license MIT
 * @copyright Copyright (c) 2025–2026 onegen
 *
 */

#include "fetch.hpp"

FetchRes fetch(const std::string& url, const Headers& headers,
               const std::string& method, const std::string& body,
               const std::stop_token& stopper) {
     return {0, 1};
}