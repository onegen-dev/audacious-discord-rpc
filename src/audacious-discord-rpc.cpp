/**
 * @file audacious-discord-rpc.cpp
 * @brief Discord Rich Presence plugin for Audacious
 * @author onegen <onegen@onegen.dev>
 * @author Derzsi Dániel <daniel@tohka.us>
 * @date 2026-06-29 (last modified)
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
      " © onegen <onegen@onegen.dev> (2024–2025)\n"
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
    WidgetButton(N_("Reconnect to Discord"), {reconnect_discord, nullptr}),
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
static discord::Presence rpc;

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

void clear_discord() {
     std::lock_guard<std::mutex> lock(rpc_lock);
     if (!is_connected.load()) return;
     ++req_id_now;               // Invalidate cover fetch tasks
     rpc = discord::Presence{};  // Full reset
     conn.clearPresence();
     rpc.setLargeImageKey("logo").setLargeImageText("Audacious");
}

void reconnect_discord() {
     AUDINFO("Discord RPC: reconnecting...\r\n");
     ++req_id_now;  // Invalidate cover fetch tasks
     is_connected.store(false);
     conn.shutdown();
     conn.initialize();
}

void cleanup_discord() {
     ++req_id_now;  // Invalidate cover fetch tasks
     {
          std::lock_guard<std::mutex> lock(rpc_lock);
          rpc = discord::Presence{};  // Full reset
          if (is_connected.load()) conn.clearPresence();
     }
     conn.shutdown();
     is_connected.store(false);
     AUDINFO("Discord RPC shut down.\r\n");
}

void update_presence() {
     if (!is_connected.load()) return;
     conn.setPresence(rpc).refresh();
}

void init_presence() {
     std::lock_guard<std::mutex> lock(rpc_lock);
     rpc = discord::Presence{};
     rpc.setLargeImageKey("logo").setLargeImageText("Audacious");
     update_presence();
     req_id_now = 0;
}

/* === Audacious playback -> Discord RPC (main function) === */

void playback_to_presence() {
     if (!is_connected.load()) return;
     if (!aud_drct_get_playing() || !aud_drct_get_ready()) {
          clear_discord();
          return;
     }

     const bool playing = !aud_drct_get_paused();
     if (aud_get_bool(PLUGIN_ID, "hide_when_paused") && !playing) {
          clear_discord();
          return;
     }

     AUDDBG("Discord RPC: playback_to_presence called\r\n");
     const Tuple tuple = aud_drct_get_tuple();
     String title = tuple.get_str(Tuple::Title);
     String artist = tuple.get_str(Tuple::Artist);
     String album = tuple.get_str(Tuple::Album);
     if (audstr_empty(title)) {
          // Fallback to filename
          title = tuple.get_str(Tuple::Basename);
          if (audstr_empty(title)) {
               // Give up
               AUDINFO("Discord RPC: no title or filename, giving up.\r\n");
               clear_discord();
               return;
          }
     }

     title = field_sanitise(title);
     artist = field_sanitise(artist);
     bool has_album = !audstr_empty(album);
     album = has_album ? field_sanitise(album) : String("[unknown]");
     int status_display_type = aud_get_int(PLUGIN_ID, "status_display_type");

     std::lock_guard<std::mutex> lock(rpc_lock);
     rpc.setLargeImageKey("logo")
         .setActivityType(discord::ActivityType::Listening)
         .setStatusDisplayType(
             static_cast<discord::StatusDisplayType>(status_display_type))
         .setDetails((const char*)title)
         .setState((const char*)artist)
         .setLargeImageText((const char*)album)
         .setSmallImageKey(playing ? "play" : "pause")
         .setSmallImageText("Audacious");

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

     update_presence();
     AUDINFO("Discord RPC: playback_to_presence updated RPC\r\n");

     if (has_album && aud_get_bool(PLUGIN_ID, "fetch_covers")) {
          String album_artist = tuple.get_str(Tuple::AlbumArtist);
          bool has_album_artist = !audstr_empty(album_artist);

          AUDINFO("Discord RPC: starting cover art fetching task\r\n");
          cover_to_presence(has_album_artist ? album_artist : artist, album);
     }
}

/* == Attempt to fetch cover art, if enabled */

void cover_to_presence(const String& artist, const String& album) {
#if (defined(DISABLE_RPC_CAF) && DISABLE_RPC_CAF)
     return;
#else
     std::string artist_str = (const char*)artist;
     std::string album_str = (const char*)album;
     unsigned long long req_id = ++req_id_now;
     std::thread([req_id, artist_str = std::move(artist_str),
                  album_str = std::move(album_str)] {
          if (req_id != req_id_now.load(std::memory_order_relaxed)) return;
          auto url = cover_lookup(artist_str, album_str, &req_id_now, req_id);
          if (url && !url->empty()
              && req_id == req_id_now.load(std::memory_order_relaxed)) {
               std::lock_guard<std::mutex> lock(rpc_lock);
               rpc.setLargeImageKey(*url);
               update_presence();
               AUDINFO("Discord RPC: cover fetch task %llu applied\r\n",
                       req_id);
          } else {
               AUDINFO("Discord RPC: dismissed stale fetch task %llu\r\n",
                       req_id);
          }
     }).detach();
#endif
}

/* === Hook RPC to Audacious === */

bool RPCPlugin::init() {
     aud_config_set_defaults(PLUGIN_ID, defaults);
     init_discord();
     init_presence();
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
     cleanup_discord();
}
