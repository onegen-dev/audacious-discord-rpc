/**
 * @file audacious-discord-rpc.hpp
 * @brief Discord Rich Presence plugin for Audacious (header)
 * @author onegen <onegen@onegen.dev>
 * @author Derzsi Dániel <daniel@tohka.us>
 * @date 2026-06-29 (last modified)
 *
 * @license MIT
 * @copyright Copyright (c) 2024–2026 onegen
 *                          2018–2022 Derzsi Dániel
 *
 */

#pragma once

#include <discord-rpc.hpp>

#include <libaudcore/audstrings.h>
#include <libaudcore/drct.h>
#include <libaudcore/hook.h>
#include <libaudcore/i18n.h>
#include <libaudcore/mainloop.h>
#include <libaudcore/plugin.h>
#include <libaudcore/preferences.h>
#include <libaudcore/runtime.h>
#include <libaudcore/tuple.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <string_view>
#include <thread>

#ifdef _WIN32
#     include <windows.h>

#     include <shellapi.h>
#else
#     include <cstdlib>
#endif

#if (!(defined(DISABLE_RPC_CAF)) && !(DISABLE_RPC_CAF))
#     include "covers.hpp"
#endif

#ifdef _WIN32
#     define EXPORT __declspec(dllexport)
#else
#     define EXPORT __attribute__((visibility("default")))
#endif

#define PLUGIN_NAME "Discord RPC"
#define PLUGIN_ID "discord-rpc"
#define PLUGIN_URL "https://github.com/onegen-dev/audacious-discord-rpc"
#define DISCORD_APP_ID "1428914566795890738"

/** Mutual access lock to presence data editing */
inline std::mutex rpc_lock;
/** "Is Discord connected?" atomic flag */
inline std::atomic<bool> is_connected{false};
/** Current cover fetching request number */
inline std::atomic<unsigned long long> req_id_now{0};
/** Checks if the req_id is stale */
inline bool cover_fetch_stop(unsigned long long req_id) {
     return req_id != req_id_now.load();
}

/* === Discord Functions === */

constexpr int DISCORD_DEFAULT_DISPLAY
    = static_cast<int>(discord::StatusDisplayType::Name);

void init_discord();
void clear_discord();
void cleanup_discord();
void update_presence();
void init_presence();

void playback_to_presence();  // Audacious metadata -> Discord RPC (main)
void cover_to_presence(
    const String &artist,
    const String &album);  // Attempts to fetch cover, if enabled

inline void on_playback_update_rpc(void *, void *) { playback_to_presence(); }

/* === Utilities === */

inline bool audstr_empty(const String &str) {
     const char *str_c = str;
     return (!str_c || !*str_c || strlen(str_c) == 0);
}

inline String field_sanitise(const String &field) {
     if (audstr_empty(field)) return String("[unknown]");

     const char *field_c = field;
     auto field_len = strlen(field_c);

     if (field_len < 2) return String(str_concat({field, " "}).settle());
     if (field_len <= 128) return field;

     // Attempt UTF-8-safe truncation
     unsigned int ofst_end = 124;
     while (ofst_end > 0 && ((unsigned char)field_c[ofst_end] & 0xC0) == 0x80)
          --ofst_end;
     if (ofst_end == 0) ofst_end = 124;  // Just in case

     return String(str_concat({str_copy(field_c, ofst_end), "..."}).settle());
}

inline void open_github() {
#ifdef _WIN32
     auto ret
         = ShellExecuteW(NULL, L"open", L"" PLUGIN_URL, NULL, NULL, SW_NORMAL);
     if ((intptr_t)ret <= 32) AUDERR("Failed to open URL: %s\r\n", PLUGIN_URL);
     /**
      * If the function succeeds, it returns a value greater than 32.
      *
      * @cite
      * https://learn.microsoft.com/en-us/windows/win32/api/shellapi/nf-shellapi-shellexecutew#return-value
      */
#else
     int ret = system("xdg-open " PLUGIN_URL);
     if (ret == -1 || ret == 127)
          AUDERR("Failed to open URL: %s\r\n", PLUGIN_URL);
     /**
      * -1 indicates fork/exec error and 127 means the command was not
      * found. Other errors are related to the user environment and are not
      * worth blocking over: “xdg-open inherits most of the flaws of its
      * configuration and the underlying opener”
      *
      * @cite
      * https://manpages.ubuntu.com/manpages/questing/en/man1/xdg-open.1.html
      */
#endif
}
