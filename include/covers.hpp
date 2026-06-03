/**
 * @file covers.hpp
 * @brief Cover art fetching functionality for Audacious Discord RPC.
 * @author onegen <onegen@onegen.dev>
 * @date 2025-11-27 (last modified)
 *
 * @license MIT
 * @copyright Copyright (c) 2025 onegen
 *
 */

#pragma once
#define JSON_NOEXCEPTION

#include <curl/curl.h>
#include <glaze/glaze.hpp>
#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <thread>

#include "covers-cache.hpp"

#ifdef _WIN32
#     include "fetch-win.hpp"  // Uses WinHTTP
#else
#     include "fetch-lin.hpp"  // Uses cURL (libcurl)
#endif

using json = nlohmann::json;

constexpr unsigned int FETCH_DEBOUNCE = 2000;  // [ms]
constexpr unsigned int FETCH_MAX_RETRIES = 5;

/* === Cache === */

static CoverArtCache cache(
    /* max_items */ 256,
    /* max_bytes (1 MiB) */ (1 << 20),
    /* TTL (1 hr) */ std::chrono::seconds(3600));

/* === Helpers === */

inline std::string esc_quotes(const std::string& s) {
     std::string r;
     r.reserve(s.size());
     for (char c : s)
          if (c == '"' || c == '\\')
               r += '\\', r += c;
          else
               r += c;
     return r;
}

inline bool is_cancelled(const std::atomic<unsigned long long>* active_req_id,
                         unsigned long long this_req_id) {
     return active_req_id && (this_req_id != active_req_id->load());
}

/* === Structs for fetched objects === */

/**
 * MusicBrainz release object
 * @see https://musicbrainz.org/doc/MusicBrainz_API/Search#Release
 * @note Many fields in the schema irrelevant to this project are omitted.
 *       Please refer to the full schema in the above link for more details
 *       and potential capabilities and future additions.
 * @date 2026-06-03
 */
struct MBRelease {
     std::string id;            // MBID (UUID-like)
     unsigned short int score;  // Relevance score (0-100)
     std::string title;
     std::string status;
     std::string packaging;
     std::string date;
     std::string country;  // ISO 3166-1 alpha-2 code
     std::string barcode;
     unsigned int track_count = 1;
     unsigned int disc_count = 1;
};

/** MusicBrainz release search result */
struct MBReleaseSearchResult {
     unsigned int count;
     unsigned int offset;
     std::vector<MBRelease> releases;
};

template <> struct glz::meta<MBRelease> {
     static constexpr std::string_view rename_key(const std::string_view key) {
          if (key == "text_representation") return "text-representation";
          if (key == "artist_credit") return "artist-credit";
          if (key == "release_group") return "release-group";
          if (key == "track_count") return "track-count";
          if (key == "disc_count") return "disc-count";
          if (key == "label_info") return "label-info";
     }
};

/**
 * Cover Art Archive image object
 * @see https://musicbrainz.org/doc/Cover_Art_Archive/API
 */
struct CAAImage {
     std::string image;  // Full-size image URL
     std::map<std::string, std::string> thumbnails;
     std::vector<std::string> types;
     bool front = false;
     bool back = false;
     bool approved = false;
};

struct CAAImageSearchResult {
     std::string release;
     std::vector<CAAImage> images;
};

/* === Exported Function === */

std::optional<std::string> cover_lookup(
    const std::string& artist, const std::string& album,
    const std::atomic<unsigned long long>* active_req_id = nullptr,
    unsigned long long this_req_id = 0) {
     // Cache
     auto cache_res = cache.get(artist, album);
     if (cache_res.has_value()) {
          AUDINFO("Discord RPC: Cover art cache hit!\r\n");
          return cache_res;
     } else {
          AUDDBG("Discord RPC: Cover art cache miss, continuing...\r\n");
     }

     unsigned int tries = 0;
     do {
          // 2 second debounce (in case user is mashing NEXT)
          for (int i = 0; i < 20; ++i) {
               // 20 sleeps × 100 ms = 2000 ms = 2 s
               if (is_cancelled(active_req_id, this_req_id))
                    return std::nullopt;
               std::this_thread::sleep_for(std::chrono::milliseconds(100));
          }

          /* The query disregards the track artist, focusing on the album artist
           * a la LastFM. Album title is prioritised over album alias. Artist
           * name is also checked for label (for compilations, like Monstercat,
           * which MB often tags as "Various Artists") but most users use
           * "Monstercat". Artist match is slightly prioritised over label. All
           * formats are accepted (for e.g. CD rips) but digital media has
           * slight priority. Only matches with score >= 90 are considered
           * + only front cover is used. If no match or error occurs, function
           * returns nullopt (Audacious logo shown instead).
           */

          auto esc_album = esc_quotes(album);
          auto esc_artist = esc_quotes(artist);
          std::string q = "(\"" + esc_album + "\"^2 OR alias:\"" + esc_album
                          + "\")^3 AND (artistname:\"" + esc_artist
                          + "\"^2 OR artist:\"" + esc_artist
                          + "\"^2 OR label:\"" + esc_artist
                          + "\") AND (format:\"Digital Media\"^2 OR format:*)"
                          + " AND NOT status:\"Pseudo-Release\"";
          auto enc_q = uri_encode(q);
          if (!enc_q)
               return std::nullopt;  // Skip retries, it’s unlikely to work
          std::string req = "https://musicbrainz.org/ws/2/release?query="
                            + enc_q.value() + "&fmt=json";

          // MB (get release MBID)
          if (is_cancelled(active_req_id, this_req_id)) return std::nullopt;
          auto mb_json = fetch(req);
          if (!mb_json) {
               AUDINFO(
                   "Discord RPC: MusicBrainz sent a bad reply (task %llu)\r\n",
                   this_req_id);
               continue;
          }

          auto mb = json::parse(*mb_json, nullptr, false);
          if (mb.is_discarded() || !mb.is_object() || mb["count"] == 0
              || mb["releases"].empty()) {
               AUDINFO(
                   "Discord RPC: MusicBrainz found no releases (task %llu)\r\n",
                   this_req_id);
               return std::nullopt;
          }
          auto release = mb["releases"][0];
          if (release["score"] < 90)
               return std::nullopt;  // No good-enough match
          std::string mbid = release["id"];
          AUDINFO("Discord RPC: MusicBrainz found release %s (task %llu)\r\n",
                  mbid.c_str(), this_req_id);

          // CAA (fetch all artwork)
          if (is_cancelled(active_req_id, this_req_id)) return std::nullopt;
          auto caa_json = fetch("https://coverartarchive.org/release/" + mbid);
          if (!caa_json) {
               AUDINFO("Discord RPC: CAA sent a bad reply (task %llu)\r\n",
                       this_req_id);
               continue;
          }

          // CAA (parse and find front cover)
          auto caa = json::parse(*caa_json, nullptr, false);
          if (caa.is_discarded() || !caa.is_object() || caa["images"].empty()) {
               AUDINFO("Discord RPC: CAA found no images (task %llu)\r\n",
                       this_req_id);
               return std::nullopt;
          }

          for (auto& image : caa["images"]) {
               if (image.contains("front") && image["front"] == true
                   && image.contains("thumbnails")
                   && image["thumbnails"].contains("large")) {
                    std::string image_url = image["thumbnails"]["large"];
                    cache.put(artist, album, image_url);
                    AUDINFO(
                        "Discord RPC: CAA found a front image (task %llu)\r\n",
                        this_req_id);
                    return image_url;
               }
          }

          AUDINFO("Discord RPC: CAA found no front images (task %llu)\r\n",
                  this_req_id);
          return std::nullopt;

     } while (++tries < FETCH_MAX_RETRIES);

     AUDINFO(
         "Discord RPC: Cover art fetch failed after %u retries (task %llu)\r\n",
         FETCH_MAX_RETRIES, this_req_id);
     return std::nullopt;
}