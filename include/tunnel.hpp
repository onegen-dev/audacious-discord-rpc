/**
 * @file tunnel.hpp
 * @brief Spawns a `cloudflared tunnel --url ...` quick tunnel and captures
 * the public url it prints out, so the user doesn't have to run it
 * themselves in a separate terminal.
 * @author asadirectly (github), for onegen's audacious-discord-rpc
 * @date 2026-09-13
 *
 * @license MIT
 * @copyright Copyright (c) 2025-2026 onegen
 *
 * This needs the `cloudflared` binary to already be installed and on the
 * user's PATH, we just launch it as a child process. Quick tunnels are
 * free and need no Cloudflare account, but the url is random and only
 * lives as long as this process does.
 */

#pragma once

#include <libaudcore/mainloop.h>
#include <libaudcore/runtime.h>

#include <atomic>
#include <cstdio>
#include <functional>
#include <mutex>
#include <regex>
#include <string>
#include <thread>

#ifdef _WIN32
#     include <windows.h>
#else
#     include <signal.h>
#     include <sys/wait.h>
#     include <unistd.h>
#endif

class CloudflareTunnel {
   public:
     ~CloudflareTunnel() { stop(); }

     CloudflareTunnel(const CloudflareTunnel&) = delete;
     CloudflareTunnel& operator=(const CloudflareTunnel&) = delete;
     CloudflareTunnel() = default;

     bool start(unsigned short local_port);
     void stop();

     bool ready() const { return m_ready.load(); }
     std::string url() const {
          std::lock_guard<std::mutex> lock(m_lock);
          return m_url;
     }

     /**
      * Called on the main thread when the tunnel URL becomes available.
      * Set before start() to avoid races with fast-starting tunnels.
      */
     void set_on_ready(std::function<void()> cb) { m_on_ready = std::move(cb); }

   private:
     void watch_output(FILE* pipe);

     std::atomic<bool> m_ready{false};
     std::atomic<bool> m_running{false};
     mutable std::mutex m_lock;
     std::string m_url;
     std::thread m_reader;
     std::function<void()> m_on_ready;
     QueuedFunc m_ready_dispatch;

#ifdef _WIN32
     PROCESS_INFORMATION m_proc_info{};
     HANDLE m_stdout_read = nullptr;
#else
     pid_t m_pid = -1;
     int m_pipe_fd = -1;
#endif
};

#ifndef _WIN32

inline bool CloudflareTunnel::start(unsigned short local_port) {
     if (m_running.load()) return true;  // already going

     int pipefd[2];
     if (pipe(pipefd) != 0) {
          AUDERR("Discord RPC: couldn't create pipe for cloudflared\r\n");
          return false;
     }

     pid_t pid = fork();
     if (pid < 0) {
          AUDERR("Discord RPC: fork() failed for cloudflared\r\n");
          close(pipefd[0]);
          close(pipefd[1]);
          return false;
     }

     if (pid == 0) {
          dup2(pipefd[1], STDOUT_FILENO);
          dup2(pipefd[1], STDERR_FILENO);
          close(pipefd[0]);
          close(pipefd[1]);

          std::string url_arg
              = "http://127.0.0.1:" + std::to_string(local_port);
          execlp("cloudflared", "cloudflared", "tunnel", "--url",
                 url_arg.c_str(), (char*)nullptr);
          _exit(127);
     }

     // parent
     close(pipefd[1]);
     m_pid = pid;
     m_pipe_fd = pipefd[0];
     m_running.store(true);

     m_reader = std::thread([this] {
          FILE* f = fdopen(m_pipe_fd, "r");
          if (!f) return;
          watch_output(f);
          fclose(f);
     });

     return true;
}

inline void CloudflareTunnel::stop() {
     if (!m_running.load()) return;

     if (m_pid > 0) {
          kill(m_pid, SIGTERM);
          int status;
          waitpid(m_pid, &status, 0);
          m_pid = -1;
     }
     if (m_reader.joinable()) m_reader.join();

     m_running.store(false);
     m_ready.store(false);
}

#else  // _WIN32

inline bool CloudflareTunnel::start(unsigned short local_port) {
     if (m_running.load()) return true;

     SECURITY_ATTRIBUTES sa{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
     HANDLE write_handle = nullptr;
     if (!CreatePipe(&m_stdout_read, &write_handle, &sa, 0)) {
          AUDERR("Discord RPC: couldn't create pipe for cloudflared\r\n");
          return false;
     }
     SetHandleInformation(m_stdout_read, HANDLE_FLAG_INHERIT, 0);

     STARTUPINFOA si{};
     si.cb = sizeof(si);
     si.dwFlags |= STARTF_USESTDHANDLES;
     si.hStdOutput = write_handle;
     si.hStdError = write_handle;

     std::string cmdline = "cloudflared.exe tunnel --url http://127.0.0.1:"
                           + std::to_string(local_port);

     BOOL ok = CreateProcessA(nullptr, cmdline.data(), nullptr, nullptr, TRUE,
                              CREATE_NO_WINDOW, nullptr, nullptr, &si,
                              &m_proc_info);
     CloseHandle(write_handle);

     if (!ok) {
          AUDERR(
              "Discord RPC: couldn't start cloudflared, is it installed and "
              "on PATH?\r\n");
          CloseHandle(m_stdout_read);
          m_stdout_read = nullptr;
          return false;
     }

     m_running.store(true);
     m_reader = std::thread([this] {
          FILE* f = _fdopen(_open_osfhandle((intptr_t)m_stdout_read, 0), "r");
          if (!f) return;
          watch_output(f);
          fclose(f);  // also closes the underlying handle
     });

     return true;
}

inline void CloudflareTunnel::stop() {
     if (!m_running.load()) return;

     if (m_proc_info.hProcess) {
          TerminateProcess(m_proc_info.hProcess, 0);
          WaitForSingleObject(m_proc_info.hProcess, 2000);
          CloseHandle(m_proc_info.hProcess);
          CloseHandle(m_proc_info.hThread);
          m_proc_info = PROCESS_INFORMATION{};
     }
     if (m_reader.joinable()) m_reader.join();

     m_running.store(false);
     m_ready.store(false);
}

#endif

inline void CloudflareTunnel::watch_output(FILE* pipe) {
     static const std::regex url_re(
         R"(https://[a-z0-9-]+\.trycloudflare\.com)");

     char buf[512];
     bool saw_any_line = false;

     while (fgets(buf, sizeof(buf), pipe)) {
          saw_any_line = true;
          if (m_ready.load()) continue;

          std::cmatch match;
          if (std::regex_search(buf, match, url_re)) {
               std::lock_guard<std::mutex> lock(m_lock);
               m_url = match.str();
               m_ready.store(true);
               AUDINFO("Discord RPC: cloudflare tunnel ready at %s\r\n",
                       m_url.c_str());
               if (m_on_ready) m_ready_dispatch.queue(m_on_ready);
          }
     }

     if (!m_ready.load()) {
          AUDERR(
              "Discord RPC: cloudflared exited without producing a tunnel "
              "url%s\r\n",
              saw_any_line ? "" : ", is cloudflared installed and on PATH?");
     }
}
