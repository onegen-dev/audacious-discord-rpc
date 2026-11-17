/**
 * @file fetch-curl.cpp
 * @brief HTTP fetcher for Audacious-Discord-RPC
 *        implemented with cURL intended for Linux.
 * @author onegen <onegen@onegen.dev>
 * @date 2026-11-17 (last modified)
 *
 * @license MIT
 * @copyright Copyright (c) 2025–2026 onegen
 *
 */

#include "fetch.hpp"

#include <curl/curl.h>

/**
 * Callback for cURL used to apply the stop token.
 * @param stopper_p Stop token pointer
 * @return 0 to continue, 1 to stop
 */
static int stopper_check(void* stopper_p, curl_off_t, curl_off_t, curl_off_t,
                         curl_off_t) {
     auto* stopper = static_cast<const std::stop_token*>(stopper_p);
     return (stopper && stopper->stop_requested()) ? 1 : 0;
}

/**
 * Write callback for cURL.
 * @note For disclosure, I copied this from somewhere but
 *       I don’t recall where from sry...
 *
 * @param c Pointer to delivered data (non-terminated chunk)
 * @param s Chunk count? Initial read size? (always 1)
 * @param n Size of delivered data
 * @param u Pointer to whatever was passed in CURLOPT_WRITEDATA
 * @return size_t Number of bytes actually handled
 */
static size_t write_cb(void* c, size_t s, size_t n, void* u) {
     static_cast<std::string*>(u)->append(static_cast<char*>(c), s * n);
     return s * n;
}

FetchRes fetch(const std::string& url, const Headers& headers,
               const std::string& method, const std::string& body,
               const std::stop_token& stopper) {
     if (stopper.stop_requested())
          return {.status_exe = CURLE_ABORTED_BY_CALLBACK};

     // Initialise cURL API
     CURL* c = curl_easy_init();
     if (!c) return {.status_exe = CURLE_FAILED_INIT};
     std::string rx_buf;  // Reception buffer

     // URL, UA and METHOD
     curl_easy_setopt(c, CURLOPT_URL, url.c_str());
     curl_easy_setopt(c, CURLOPT_USERAGENT, UA);
     if (method == "HEAD") {
          curl_easy_setopt(c, CURLOPT_NOBODY, 1L);
     } else if (method == "GET") {
          curl_easy_setopt(c, CURLOPT_HTTPGET, 1L);
     } else if (method == "POST") {
          curl_easy_setopt(c, CURLOPT_POST, 1L);
     } else {
          curl_easy_setopt(c, CURLOPT_CUSTOMREQUEST, method.c_str());
     }
     curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);  // Follow redirects
     curl_easy_setopt(c, CURLOPT_FAILONERROR, 0L);

     // Response body
     curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_cb);
     curl_easy_setopt(c, CURLOPT_WRITEDATA, &rx_buf);

     // Request body
     if (!body.empty() || method == "POST" || method == "PUT") {
          // The POST and PUT checks in the IF are there because cURL will
          // wait for input data on stdin if not supplied.
          curl_easy_setopt(c, CURLOPT_POSTFIELDS, body.data());
          curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE_LARGE,
                           static_cast<curl_off_t>(body.size()));
     }

     // Headers
     curl_slist* hl = nullptr;
     for (const auto& [k, v] : headers)
          hl = curl_slist_append(hl, (k + ": " + v).c_str());
     curl_easy_setopt(c, CURLOPT_HTTPHEADER, hl);

     // Stop token
     curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, stopper_check);
     curl_easy_setopt(c, CURLOPT_XFERINFODATA, &stopper);
     curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);

     // Timeout
     curl_easy_setopt(c, CURLOPT_TIMEOUT_MS, FETCH_TIMEO);
     curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT_MS, FETCH_TIMEO);

     // EXECUTE! (finally...)
     CURLcode rc = curl_easy_perform(c);  // cURL status code
     long status = 0;                     // HTTP status code
     curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
     if (status < 0 || status >= 600) status = 0;

     // Cleanup
     curl_slist_free_all(hl);
     curl_easy_cleanup(c);

     return {.status_http = static_cast<unsigned int>(status),
             .status_exe = static_cast<unsigned int>(rc),
             .data = rx_buf};
}