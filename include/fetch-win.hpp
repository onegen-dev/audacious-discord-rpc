/**
 * @file fetch-win.hpp
 * @brief WinHTTP-based fetcher for use on Windows 10+.
 * @note Made for Audacious-Discord-RPC project.
 * @author onegen <onegen@onegen.dev>
 * @date 2026-06-29 (last modified)
 *
 * @license MIT
 * @copyright Copyright (c) 2025–2026 onegen
 *
 */

#pragma once

#include <windows.h>

#include <winhttp.h>

#include <codecvt>
#include <optional>
#include <string>

#ifndef AUDDBG
#     define AUDDBG(...) ((void)0)
#endif
#ifndef AUDINFO
#     define AUDINFO(...) ((void)0)
#endif
#ifndef AUDERR
#     define AUDERR(...) ((void)0)
#endif

constexpr DWORD FETCH_TIMEO = 15000;  // [ms]

/* === Helpers === */

/**
 * @brief Converts UTF-8 std::string to Windows std::wstring
 * @cite https://stackoverflow.com/questions/38672719
 */
std::wstring wstringify(const std::string& str) {
     if (str.empty()) return std::wstring();
     size_t size
         = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), str.size(), NULL, 0);
     std::wstring wstr(size, 0);
     MultiByteToWideChar(CP_UTF8, 0, str.c_str(), str.size(), &wstr[0], size);
     return wstr;
}

/**
 * @brief Gets last Windows API error as a string
 * @cite https://stackoverflow.com/a/17387176
 */
static std::string GetLastErrorAsString() {
     auto err = GetLastError();
     if (err == 0) return std::string("NONE");

     LPSTR buf;
     size_t size = FormatMessageA(
         FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM
             | FORMAT_MESSAGE_IGNORE_INSERTS,
         NULL, err, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), (LPSTR)&buf, 0,
         NULL);

     std::string msg(buf, size);
     if (buf) LocalFree(buf);
     return msg;
}

/* === Exported Function === */

/** @brief User-Agent */
static const wchar_t* ua
    = L"Audacious-Discord-RPC/master "
      "(+https://github.com/onegen-dev/audacious-discord-rpc)";

static std::optional<std::string> fetch(const std::string& url) noexcept {
     std::wstring wurl = wstringify(url);

     /** @cite
      * https://learn.microsoft.com/en-us/windows/win32/api/winhttp/nf-winhttp-winhttpcrackurl#examples
      */
     URL_COMPONENTS url_parts;
     ZeroMemory(&url_parts, sizeof(url_parts));
     url_parts.dwStructSize = sizeof(url_parts);
     url_parts.dwSchemeLength = -1;
     url_parts.dwHostNameLength = -1;
     url_parts.dwUrlPathLength = -1;
     url_parts.dwExtraInfoLength = -1;
     if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &url_parts)) {
          AUDINFO("Discord RPC WinHTTP fetch failed: %s\r\n",
                  GetLastErrorAsString().c_str());
          return std::nullopt;
     }

     std::wstring host(url_parts.lpszHostName, url_parts.dwHostNameLength);
     std::wstring path(url_parts.lpszUrlPath, url_parts.dwUrlPathLength);
     path.append(url_parts.lpszExtraInfo, url_parts.dwExtraInfoLength);

     HINTERNET sesh
         = WinHttpOpen(ua, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, NULL, NULL, 0);
     if (!sesh) return std::nullopt;
     HINTERNET conn
         = WinHttpConnect(sesh, host.c_str(), INTERNET_DEFAULT_PORT, 0);
     if (!conn) return std::nullopt;
     unsigned long req_flags = WINHTTP_FLAG_ESCAPE_PERCENT;
     if (url_parts.nScheme == INTERNET_SCHEME_HTTPS)
          req_flags |= WINHTTP_FLAG_SECURE;
     HINTERNET req = WinHttpOpenRequest(conn, NULL, path.c_str(), NULL,
                                        WINHTTP_NO_REFERER, NULL, req_flags);
     auto cleanup = [&](void) -> void {
          if (req) WinHttpCloseHandle(req);
          if (conn) WinHttpCloseHandle(conn);
          if (sesh) WinHttpCloseHandle(sesh);
     };

     if (!req) {
          AUDINFO("Discord RPC WinHTTP fetch failed: %s\r\n",
                  GetLastErrorAsString().c_str());
          cleanup();
          return std::nullopt;
     }

     if (!WinHttpSetTimeouts(req, FETCH_TIMEO, FETCH_TIMEO, FETCH_TIMEO,
                             FETCH_TIMEO)
         || !WinHttpSendRequest(req, NULL, 0, NULL, 0, 0, 0)
         || !WinHttpReceiveResponse(req, NULL)) {
          AUDINFO("Discord RPC WinHTTP fetch failed: %s\r\n",
                  GetLastErrorAsString().c_str());
          cleanup();
          return std::nullopt;
     }

     std::string data;
     unsigned long n_read = 0;
     unsigned long n_available = 0;

     do {
          if (!WinHttpQueryDataAvailable(req, &n_available)) {
               AUDINFO("Discord RPC WinHTTP fetch failed: %s\r\n",
                       GetLastErrorAsString().c_str());
               cleanup();
               return std::nullopt;
          }

          if (!n_available) break;
          char* buf = new char[n_available];
          if (!buf) {
               AUDINFO("Discord RPC WinHTTP fetch failed: %s\r\n",
                       GetLastErrorAsString().c_str());
               cleanup();
               return std::nullopt;
          }

          if (!WinHttpReadData(req, buf, n_available, &n_read)) {
               AUDINFO("Discord RPC WinHTTP fetch failed: %s\r\n",
                       GetLastErrorAsString().c_str());
               cleanup();
               delete[] buf;
               return std::nullopt;
          }

          data.append(buf, n_read);
          delete[] buf;

          if (n_read == 0) break;  // Shouldn’t happen
     } while (n_available > 0);
     cleanup();

     return data;
}

static std::optional<std::string> uri_encode(const std::string& str) noexcept {
     return str;  // This is handled by WinHttpOpenRequest in fetch()
}