/**
 * @file art-server.hpp
 * @brief Serves the currently playing track's own embedded cover art over
 * local HTTP, so a tunnel (cloudflared, ngrok, etc) can hand Discord a
 * public URL for it.
 * @author asadirectly (github), for onegen's audacious-discord-rpc
 * @date 2026-09-13
 *
 * @license MIT
 * @copyright Copyright (c) 2025-2026 onegen
 *
 * Unlike covers.hpp (which tries to find the album online via MusicBrainz), this
 * reads the art Audacious already extracted from the file itself via
 * aud_art_request(..., AUD_ART_FILE). there is no network lookup
 *
 * Discord fetches image URLs from its own servers, so serving on
 * 127.0.0.1 alone does not work, the user still needs to point an actual
 * tunnel at this server's port
 */

#pragma once

#include <httplib.h>

#include <libaudcore/audstrings.h>
#include <libaudcore/runtime.h>

#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>

namespace fs = std::filesystem;

/**
 * runs a small local http server that serves whatever's in its cover
 * directory. track gets copied in under a hashed filename (based on
 * the source file's path), so the url changes per track and Discord
 * doesn't hang on to a stale cached image after a skip.
 */
class ArtServer {
   public:
     ArtServer() = default;
     ~ArtServer() { stop(); }

     ArtServer(const ArtServer&) = delete;
     ArtServer& operator=(const ArtServer&) = delete;

     bool start(unsigned short port) {
          std::lock_guard<std::mutex> lock(m_lock);
          if (m_running.load()) return true;

          m_dir = fs::temp_directory_path() / "audacious-discord-rpc-covers";
          std::error_code ec;
          fs::create_directories(m_dir, ec);
          if (ec) {
               AUDERR("Discord RPC: couldn't create cover dir: %s\r\n",
                      ec.message().c_str());
               return false;
          }

          m_server = std::make_unique<httplib::Server>();
          m_server->set_mount_point("/", m_dir.string());
          // a bare directory listing would leak every cover we've ever
          // cached to anyone who has the tunnel url, so refuse that even though it's mostly harmless info
          m_server->Get(R"(/)",
                        [](const httplib::Request&, httplib::Response& res) {
                             res.status = 404;
                        });

          m_thread = std::thread([this, port] {
               AUDINFO("Discord RPC: serving cover art on port %u\r\n", port);
               if (!m_server->listen("127.0.0.1", port)) {
                    AUDERR(
                        "Discord RPC: cover art server failed to bind port "
                        "%u, is something else already using it?\r\n",
                        port);
               }
          });
          m_running.store(true);
          return true;
     }

     void stop() {
          std::lock_guard<std::mutex> lock(m_lock);
          if (!m_running.load()) return;
          if (m_server) m_server->stop();
          if (m_thread.joinable()) m_thread.join();
          m_server.reset();
          m_running.store(false);
     }

     /**
      * Copies the source art file into the served directory under a hashed
      * name and returns the filename. Converts Audacious's file:// URI to
      * a filesystem path first. Returns empty on failure; callers shuld
      * fall back to the static logo.
      */
     std::string publish(const std::string& source_uri) {
          if (source_uri.empty()) {
               AUDINFO("Discord RPC: publish() called with empty uri\r\n");
               return "";
          }

          StringBuf local_path = uri_to_filename(source_uri.c_str());
          if (!local_path) {
               AUDINFO(
                   "Discord RPC: publish() couldn't convert uri to a "
                   "local path: %s\r\n",
                   source_uri.c_str());
               return "";
          }

          std::error_code ec;
          fs::path src((const char*)local_path);
          if (!fs::exists(src, ec) || ec) {
               AUDINFO("Discord RPC: publish() source doesn't exist: %s\r\n",
                       (const char*)local_path);
               return "";
          }

          std::string ext = src.extension().string();
          if (ext.empty()) ext = ".jpg";  // audacious's cache usually omits it

          std::string name
              = std::to_string(std::hash<std::string>{}(source_uri)) + ext;
          fs::path dest = m_dir / name;

          if (!fs::exists(dest, ec)) {
               fs::copy_file(src, dest, fs::copy_options::overwrite_existing,
                             ec);
               if (ec) {
                    AUDERR("Discord RPC: couldn't copy cover art: %s\r\n",
                           ec.message().c_str());
                    return "";
               }
          }

          return name;
     }

   private:
     std::unique_ptr<httplib::Server> m_server;
     std::thread m_thread;
     std::atomic<bool> m_running{false};
     std::mutex m_lock;
     fs::path m_dir;
};
