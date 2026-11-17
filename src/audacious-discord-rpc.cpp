/**
 * @file audacious-discord-rpc.cpp
 * @brief Discord Rich Presence plugin for Audacious
 * @author onegen <onegen@onegen.dev>
 * @author Derzsi Dániel <daniel@tohka.us>
 * @date 2026-09-18 (last modified)
 *
 * @license MIT
 * @copyright Copyright (c) 2024–2026 onegen
 *                          2018–2022 Derzsi Dániel
 *
 */

#include "audacious-discord-rpc.hpp"

/* === Audacious Plugin Setup === */

class RPCPlugin : public GeneralPlugin {
   public:
     static const char about[];
     static const PreferencesWidget widgets[];
     static const PluginPreferences prefs;
     static const char* const defaults[];

     static constexpr PluginInfo info
         = {N_(PLUGIN_NAME), PLUGIN_ID, about, &prefs, 0};

     constexpr RPCPlugin() : GeneralPlugin(info, false) {}

     bool init();
     void cleanup();
};

const char RPCPlugin::about[]
    = "Discord Rich Presence (RPC) playing status plugin\n"
      "https://github.com/onegen-dev/audacious-discord-rpc\n"
      " © onegen <onegen@onegen.dev> (2024–2026)\n"
      " © Derzsi Dániel <daniel@tohka.us> et al. (2018–2022)\n\n"
      "Displays the current playing track as your Discord status.\n"
      "(Discord should be running before this plugin is loaded.)";

static const ComboItem status_display_items[]
    = {ComboItem(N_("Music player"),
                 static_cast<int>(discord::StatusDisplayType::Name)),
       ComboItem(N_("Song title"),
                 static_cast<int>(discord::StatusDisplayType::Details)),
       ComboItem(N_("Artist name"),
                 static_cast<int>(discord::StatusDisplayType::State))};

const PreferencesWidget RPCPlugin::widgets[] = {
#if (!(defined(DISABLE_RPC_CAF)) && !(DISABLE_RPC_CAF))
    WidgetCheck(N_("(UNSTABLE) Fetch album covers from MusicBrainz/CAA"),
                WidgetBool(PLUGIN_ID, "fetch_covers")),
#endif
    WidgetCheck(N_("Hide presence when paused"),
                WidgetBool(PLUGIN_ID, "hide_when_paused")),
    WidgetCombo(N_("Status display"),
                WidgetInt(PLUGIN_ID, "status_display_type"),
                {{status_display_items}, nullptr}),
    WidgetButton(N_("Show on GitHub"), {open_github, nullptr})};

static_assert(DISCORD_DEFAULT_DISPLAY == 0,
              "defaults[] status_display_type is out of sync!");

const char* const RPCPlugin::defaults[] = {
#if (!(defined(DISABLE_RPC_CAF)) && !(DISABLE_RPC_CAF))
    "fetch_covers",     "FALSE",
#endif
    "hide_when_paused", "FALSE", "status_display_type", "0", nullptr};

const PluginPreferences RPCPlugin::prefs
    = {{widgets}, nullptr, nullptr, nullptr};

EXPORT RPCPlugin aud_plugin_instance;

/**
 * @brief Dispatch callback to call functions (specifically
 * playback_to_presence) on the main thread even when not on main thread.
 * @note Added as a resolution to issue #18.
 */
static QueuedFunc ready_dispatch;

/* === Discord RPC Setup === */

static discord::RPCManager& conn = discord::RPCManager::get();
static discord::Presence rpc{};

void init_discord() {
     conn.setClientID(DISCORD_APP_ID);
     conn.onReady([](const discord::User&) {
              is_connected.store(true);
              AUDINFO("Discord RPC connected.\r\n");
              ready_dispatch.queue([] { playback_to_presence(); });
         })
         .onDisconnected([](int, std::string_view) {
              is_connected.store(false);
              AUDINFO("Discord RPC disconnected.\r\n");
         })
         .onErrored([](int, std::string_view msg) {
              is_connected.store(false);
              AUDERR("Discord RPC error: %.*s\r\n",
                     static_cast<int>(msg.size()), msg.data());
         });
     conn.initialize();
}

void init_presence() {
     std::lock_guard<std::mutex> lock(rpc_lk);
     rpc.clear();
     rpc.setLargeImageKey("logo").setLargeImageText("Audacious");
}

void cleanup_discord() {
     std::lock_guard<std::mutex> lock(rpc_lk);
     rpc_throttle.stop();
     rpc_dirty = false;
     rpc = discord::Presence{};  // Full reset
     if (is_connected.load()) conn.clearPresence();

     conn.shutdown();
     is_connected.store(false);
}

void update_presence() {
     if (!is_connected.load()) return;
     /* Discord requires a throttle of 1 update per 15 seconds,
      * or at least it did for original RPC and I can’t verify it
      * that well now. This block does just that, executing updates
      * only once in 15 seconds.
      */
     if (rpc_throttle.running()) {
          /* A throttle (waiting for the 15 second limit) is running.
           * The rpc_dirty flag tells it that it needs to recheck the
           * presence before sending it. That’s it for this update.
           */
          AUDDBG("Discord RPC: Presence update throttled.\r\n");
          rpc_dirty = true;
          return;
     }

     // No throttle is active => send the update.
     conn.setPresence(rpc).refresh();
     AUDDBG("Discord RPC: Presence update sent.\r\n");

     /* Start a throttle/timer. This uses Audacious’ QueuedFunc
      * which runs periodically and asynchroniously (non-blocking).
      */
     rpc_throttle.start(DISCORD_UPDATE_THROTTLE, [] {
          /* If rpc_dirty (presence update is queued up), update is sent
           * and function remains running (so it waits the throttle time
           * and runs this closure again).
           * If dirty flag is unset, throttle function ends, allowing
           * the next update to be executed without delay.
           */
          if (rpc_dirty) {
               conn.setPresence(rpc).refresh();
               rpc_dirty = false;
               AUDDBG("Discord RPC: Presence update (throttled) sent.\r\n");
          } else {
               rpc_throttle.stop();
               AUDDBG("Discord RPC: Throttle timer ended.\r\n");
          }
     });
}

void clear_presence() {
     if (!is_connected.load()) return;
     cover_cancel();  // Invalidate cover fetch tasks
     rpc_throttle.stop();
     rpc.clear();
     conn.clearPresence();
}

/* === Audacious playback -> Discord RPC (main function) === */

void playback_to_presence() {
     if (!is_connected.load()) return;
     if (!aud_drct_get_playing() || !aud_drct_get_ready()) {
          clear_presence();
          return;
     }

     const bool playing = !aud_drct_get_paused();
     if (aud_get_bool(PLUGIN_ID, "hide_when_paused") && !playing) {
          clear_presence();
          return;
     }

     // Get standard metadata (artist, track and album)
     const Tuple tuple = aud_drct_get_tuple();
     String title = tuple.get_str(Tuple::Title);
     String artist = tuple.get_str(Tuple::Artist);
     String album = tuple.get_str(Tuple::Album);
     if (audstr_empty(title)) {
          // Fallback to filename
          title = tuple.get_str(Tuple::Basename);
          if (audstr_empty(title)) {
               // Give up
               AUDINFO(
                   "Discord RPC: Can't apply presence without a title.\r\n");
               clear_presence();
               return;
          }
     }

     // Sanitise standard meta (make sure they are encodable)
     title = field_sanitise(title);
     artist = field_sanitise(artist);
     bool has_album = !audstr_empty(album);
     album = has_album ? field_sanitise(album) : String("");
     int status_display_type = aud_get_int(PLUGIN_ID, "status_display_type");

     // Setting standard metadata to presence object
     std::lock_guard<std::mutex> lock(rpc_lk);
     rpc.setLargeImageKey("logo")
         .setActivityType(discord::ActivityType::Listening)
         .setStatusDisplayType(
             static_cast<discord::StatusDisplayType>(status_display_type))
         .setDetails((const char*)title)
         .setState((const char*)artist)
         .setLargeImageText((const char*)album)
         .setSmallImageKey(playing ? "play" : "pause")
         .setSmallImageText("Audacious");

     // Add timestamp, if possible
     if (playing && tuple.get_value_type(Tuple::Length) == Tuple::Int) {
          const auto now = std::chrono::system_clock::now();
          const auto start_time
              = now - std::chrono::seconds(aud_drct_get_time() / 1000);
          rpc.setStartTimestamp(
              std::chrono::duration_cast<std::chrono::seconds>(
                  start_time.time_since_epoch())
                  .count());

          int length_s = tuple.get_int(Tuple::Length) / 1000;
          if (length_s > 0) {
               const auto end_time
                   = start_time + std::chrono::seconds(length_s);
               rpc.setEndTimestamp(
                   std::chrono::duration_cast<std::chrono::seconds>(
                       end_time.time_since_epoch())
                       .count());
          } else {
               rpc.setEndTimestamp(0);
          }
     } else {
          rpc.setStartTimestamp(0).setEndTimestamp(0);
     }

     // Send updated presence (throttled, see its implementation for more info)
     update_presence();
     AUDINFO("Discord RPC: Presence changed by playback_to_presence\r\n");

     // Cover art fetching call, if appropriate
     if (has_album && aud_get_bool(PLUGIN_ID, "fetch_covers")) {
          String album_artist = tuple.get_str(Tuple::AlbumArtist);
          bool has_album_artist = !audstr_empty(album_artist);

          AUDINFO("Discord RPC: New cover fetch task starting...\r\n");
          cover_to_presence(has_album_artist ? album_artist : artist, album);
     }
}

/* == Attempt to fetch cover art, if enabled == */

#if (!(defined(DISABLE_RPC_CAF)) && !(DISABLE_RPC_CAF))

static QueuedFunc cover_dispatch;  //< Carries cover art lookups between threads

static CoverWorker cover_worker([](unsigned long long req_id,
                                   const std::string& url,
                                   std::stop_token stop) {
     cover_dispatch.queue([req_id, url, stop = std::move(stop)] {
          if (stop.stop_requested()) {
               AUDINFO(
                   "Discord RPC: Cover fetch task %llu deemed stale, result "
                   "dropped\r\n",
                   req_id);
               return;
          }
          std::lock_guard<std::mutex> lock(rpc_lk);
          rpc.setLargeImageKey(url);
          update_presence();
          AUDINFO(
              "Discord RPC: Cover fetch task %llu successful, result "
              "applied\r\n",
              req_id);
     });
});

void cover_worker_start() { cover_worker.start(); }

void cover_worker_stop() {
     cover_worker.stop();
     cover_dispatch.stop();
}

void cover_cancel() { cover_worker.cancel(); }

void cover_to_presence(const String& artist, const String& album) {
     cover_worker.submit((const char*)artist, (const char*)album);
}

#else

// Cover functions are NOOP if compiled w/o them
void cover_worker_start() {}
void cover_worker_stop() {}
void cover_cancel() {}
void cover_to_presence(const String&, const String&) {}

#endif

/* === Hook RPC to Audacious === */

bool RPCPlugin::init() {
     aud_config_set_defaults(PLUGIN_ID, defaults);
     init_discord();
     init_presence();
     cover_worker_start();
     hook_associate("playback ready", on_playback_update_rpc, nullptr);
     hook_associate("playback end", on_playback_update_rpc, nullptr);
     hook_associate("playback stop", on_playback_update_rpc, nullptr);
     hook_associate("playback seek", on_playback_update_rpc, nullptr);
     hook_associate("playback pause", on_playback_update_rpc, nullptr);
     hook_associate("playback unpause", on_playback_update_rpc, nullptr);
     hook_associate("title change", on_playback_update_rpc, nullptr);
     return true;
}

void RPCPlugin::cleanup() {
     ++req_id_now;
     hook_dissociate("playback ready", on_playback_update_rpc);
     hook_dissociate("playback end", on_playback_update_rpc);
     hook_dissociate("playback stop", on_playback_update_rpc);
     hook_dissociate("playback seek", on_playback_update_rpc);
     hook_dissociate("playback pause", on_playback_update_rpc);
     hook_dissociate("playback unpause", on_playback_update_rpc);
     hook_dissociate("title change", on_playback_update_rpc);
     cover_worker_stop();
     ready_dispatch.stop();
     cleanup_discord();
}
