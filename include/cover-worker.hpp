/**
 * @file cover-worker.hpp
 * @brief The one and only cover art fetching thread
 * @author onegen <onegen@onegen.dev>
 * @date 2026-09-18 (last modified)
 *
 * @note Cover worked deliberately makes all cover arts
 *       fetched on a single worker thread rather than on
 *       many detached threads, as before (see issue #17).
 *       It follows a simple set of design rules:
 *
 *       1. There is exactly one worker thread,
 *              owned by one object.
 *       2. That thread is always joined before
 *              the plugin can go away.
 *       3. Every request carries a stop token;
 *              flipping it means the fetch is lo longer
 *              desired and every “slow step” checks it.
 *
 * @licence MIT
 * @copyright Copyright (c) 2026 onegen
 *
 */

#pragma once

#include <condition_variable>
#include <exception>
#include <functional>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>

#include "covers.hpp"

class CoverWorker {
   public:
     /**
      * Called when a lookup produces a cover art URL.
      *
      * @warning Runs on the worker thread. This function must not
      *          touch any Discord object/struct (use QueuedFunc for that).
      *
      * @param req_id Task number
      * @param url    The cover art URL that was found.
      * @param stop   Cancel switch of this request (stop token).
      */
     using ResultFn
         = std::function<void(unsigned long long req_id, const std::string &url,
                              std::stop_token stop)>;

     explicit CoverWorker(ResultFn on_result)
         : on_result(std::move(on_result)) {}
     ~CoverWorker() { this->stop(); }
     CoverWorker(const CoverWorker &) = delete;
     CoverWorker &operator=(const CoverWorker &) = delete;

     void start() {
          if (this->thread.joinable()) return;  // Thread already running
          this->thread
              = std::jthread([this](std::stop_token quit) { run(quit); });
     }

     void stop() {
          this->cancel();               // Tell the fetch req to give up
          this->thread.request_stop();  // Tell the loop itself to give up
          this->wake.notify_all();      // …and poke ’em
          if (this->thread.joinable()) this->thread.join();
     }

     void submit(std::string artist, std::string album) {
          {
               std::lock_guard<std::mutex> lk(this->lock);
               this->job_stop.request_stop();  // Supersede the request
               pending
                   = FetchJob{++last_id, std::move(artist), std::move(album)};
          }
          wake.notify_one();
     }

     void cancel() {
          std::lock_guard<std::mutex> lk(this->lock);
          this->pending.reset();
          this->job_stop.request_stop();
     }

   private:
     struct FetchJob {
          unsigned long long id = 0;
          std::string artist;
          std::string album;
     };

     /** @brief Body of the worker thread (take a request, run it, repeat). */
     void run(std::stop_token quit) {
          std::unique_lock<std::mutex> lk(this->lock);
          while (!quit.stop_requested()) {
               wake.wait(lk, quit,
                         [this] { return this->pending.has_value(); });
               if (quit.stop_requested()) break;

               FetchJob job = std::move(*this->pending);
               this->pending.reset();
               this->job_stop = std::stop_source{};
               std::stop_token stop = job_stop.get_token();
               lk.unlock();  // Do the slow fetch without locking a potential
                             // change

               try {
                    auto url
                        = cover_lookup(job.artist, job.album, stop, job.id);
                    if (url && !url->empty() && !stop.stop_requested())
                         on_result(job.id, *url, stop);
                    else
                         AUDINFO(
                             "Discord RPC: cover fetch task %llu found "
                             "nothing\r\n",
                             job.id);
               } catch (const std::exception &e) {
                    AUDERR("Discord RPC: cover fetch task %llu failed: %s\r\n",
                           job.id, e.what());
               } catch (...) {
                    AUDERR(
                        "Discord RPC: cover fetch task %llu failed "
                        "(unknown)\r\n",
                        job.id);
               }

               lk.lock();
          }
     }

     ResultFn on_result;  //< Callback to run after a fetch job is complete

     std::mutex lock;  //< Mutual execution lock for all operations
     std::condition_variable_any
         wake;  //< Condition variable conveying whether we are operating or not
     std::optional<FetchJob> pending;  //< Potential pending FetchJob
     std::stop_source job_stop;       //< Cancel switch for the current FetchJob
     unsigned long long last_id = 0;  //< Task counter

     std::jthread thread;
};