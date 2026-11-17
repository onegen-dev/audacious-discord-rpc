/**
 * @file fetch-winhttp.cpp
 * @brief HTTP fetcher for Audacious-Discord-RPC
 *        implemented in WinHTTP for Windows.
 * @author onegen <onegen@onegen.dev>
 * @date 2026-11-17 (last modified)
 *
 * @license MIT
 * @copyright Copyright (c) 2025–2026 onegen
 *
 */

#include "fetch.hpp"

#include <windows.h>

#include <winerror.h>
#include <winhttp.h>

/**
 * @brief Converts UTF-8 std::string to Windows std::wstring
 * @cite https://stackoverflow.com/questions/38672719
 */
static std::wstring wstringify(const std::string& str) {
     if (str.empty()) return std::wstring();
     size_t size
         = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), str.size(), NULL, 0);
     std::wstring wstr(size, 0);
     MultiByteToWideChar(CP_UTF8, 0, str.c_str(), str.size(), &wstr[0], size);
     return wstr;
}

// Windows casts
constexpr DWORD FETCH_TIMEO_WIN = static_cast<DWORD>(FETCH_TIMEO);
inline wchar_t const* UA_WIN = wstringify(UA).c_str();
inline wchar_t const* DEFAULT_CONTENT_TYPE
    = L"Content-Type: application/x-www-form-urlencoded";

FetchRes fetch(const std::string& url, const Headers& headers,
               const std::string& method, const std::string& body,
               const std::stop_token& stopper) {
     if (stopper.stop_requested())
          return {.status_exe = ERROR_OPERATION_ABORTED};
     std::wstring wurl = wstringify(url);

     /** @cite
      * https://learn.microsoft.com/en-us/windows/win32/api/winhttp/nf-winhttp-winhttpcrackurl#examples
      * Most of this function is implemented copy-pasting Microsoft guides.
      */
     URL_COMPONENTS url_parts;
     ZeroMemory(&url_parts, sizeof(url_parts));
     url_parts.dwStructSize = sizeof(url_parts);
     url_parts.dwSchemeLength = -1;
     url_parts.dwHostNameLength = -1;
     url_parts.dwUrlPathLength = -1;
     url_parts.dwExtraInfoLength = -1;
     if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &url_parts))
          return {.status_exe = GetLastError()};

     std::wstring host(url_parts.lpszHostName, url_parts.dwHostNameLength);
     std::wstring path(url_parts.lpszUrlPath, url_parts.dwUrlPathLength);
     path.append(url_parts.lpszExtraInfo, url_parts.dwExtraInfoLength);

     // Obtain a session handle
     HINTERNET sesh
         = WinHttpOpen(UA_WIN, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                       WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
     if (!sesh) return {.status_exe = GetLastError()};

     // Specify host to connect to
     HINTERNET conn
         = WinHttpConnect(sesh, host.c_str(), INTERNET_DEFAULT_PORT, 0);
     if (!conn) return {.status_exe = GetLastError()};

     // Connection flags
     unsigned long req_flags = 0;
     if (url_parts.nScheme == INTERNET_SCHEME_HTTPS)
          req_flags |= WINHTTP_FLAG_SECURE;

     // Create a HTTP request handle
     HINTERNET req = WinHttpOpenRequest(
         conn, wstringify(method).c_str(), path.c_str(), NULL,
         WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, req_flags);

     /** Callback to use for cleaning up the connection */
     auto cleanup = [&](void) -> void {
          if (req) WinHttpCloseHandle(req);
          if (conn) WinHttpCloseHandle(conn);
          if (sesh) WinHttpCloseHandle(sesh);
     };

     if (!req)
          return {.status_exe = GetLastError()};
     else if (stopper.stop_requested())
          return {.status_exe = ERROR_OPERATION_ABORTED};

     // Set timeouts
     if (!WinHttpSetTimeouts(req, FETCH_TIMEO_WIN, FETCH_TIMEO_WIN,
                             FETCH_TIMEO_WIN, FETCH_TIMEO_WIN)) {
          cleanup();
          return {.status_exe = GetLastError()};
     }

     // Set headers
     const wchar_t* contentHeader = NULL;
     for (const auto& [k, v] : headers) {
          /** @cite
           * https://learn.microsoft.com/en-us/windows/win32/api/winhttp/nf-winhttp-winhttpaddrequestheaders#examples
           */
          if (k == "Content-Type") {
               contentHeader = wstringify(k + ": " + v).c_str();
               continue;
          }

          WinHttpAddRequestHeaders(req, wstringify(k + ": " + v).c_str(), -1L,
                                   WINHTTP_ADDREQ_FLAG_ADD);
     }
     if (!contentHeader && !body.empty()) contentHeader = DEFAULT_CONTENT_TYPE;

     // Send request
     if (!WinHttpSendRequest(req, contentHeader, wcslen(DEFAULT_CONTENT_TYPE),
                             static_cast<void*>(const_cast<char*>(body.data())),
                             static_cast<DWORD>(body.length()),
                             static_cast<DWORD>(body.length()), 0)) {
          cleanup();
          return {.status_exe = GetLastError()};
     }

     // Receive response
     if (!WinHttpReceiveResponse(req, NULL)) {
          cleanup();
          return {.status_exe = GetLastError()};
     }

     std::string data;
     unsigned long n_read = 0;
     unsigned long n_available = 0;

     do {
          if (stopper.stop_requested()) {
               cleanup();
               return {.status_exe = ERROR_OPERATION_ABORTED};
          }

          if (!WinHttpQueryDataAvailable(req, &n_available)) {
               cleanup();
               return {.status_exe = GetLastError()};
          }

          if (!n_available) break;
          char* buf = new (std::nothrow) char[n_available];
          if (!buf) {
               cleanup();
               return {.status_exe = GetLastError()};
          }

          if (!WinHttpReadData(req, buf, n_available, &n_read)) {
               delete[] buf;
               cleanup();
               return {.status_exe = GetLastError()};
          }

          data.append(buf, n_read);
          delete[] buf;

          if (n_read == 0) break;  // Shouldn’t happen
     } while (n_available > 0);

     // Extract status code
     // @cite https://stackoverflow.com/a/25795669
     unsigned int status = 0;
     unsigned long status_len = sizeof(status);
     WinHttpQueryHeaders(req,
                         WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                         WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_len,
                         WINHTTP_NO_HEADER_INDEX);

     cleanup();
     return {.status_http = status, .status_exe = 0, .data = data};
}