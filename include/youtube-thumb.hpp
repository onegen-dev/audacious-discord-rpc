/**
 * @file youtube-thumb.hpp
 * @brief Pulls a YouTube thumbnail URL out of a track's comment tag, for
 * files ripped from YouTube that have no embedded cover art of their own.
 * @author asadirectly (Github), for onegen's audacious-discord-rpc
 * @date 2026-09-13
 *
 * @license MIT
 * @copyright Copyright (c) 2025-2026 onegen
 *
 * Many yt2mp3 tools write the source video's url into the comment tag when ripping to mp3.
 * This just regex-searches that field for a youtube.com or youtu.be link and pulls the 11
 * character video id out of it. If the comment doesn't have one (a lot of rips won't), this
 * comes back empty and the caller should fall back to default
 *
 * img.youtube.com is YouTube's own public thumbnail cdn, so there is no api key or
 * network request needed, this just hands Discord the url and it
 * should fetch the image itself.
 */

#pragma once

#include <regex>
#include <string>

inline std::string youtube_thumbnail_from_comment(const std::string& comment) {
     if (comment.empty()) return "";

     // covers both youtube.com/watch?v=ID and youtu.be/ID
     static const std::regex yt_re(
         R"((?:youtube\.com/watch\?v=|youtu\.be/)([A-Za-z0-9_-]{11}))");

     std::smatch match;
     if (!std::regex_search(comment, match, yt_re)) return "";

     return "https://img.youtube.com/vi/" + match[1].str() + "/hqdefault.jpg";
}
