// POSIX OpenSSH transport. Each profile owns one process group and one cancellable worker.
#include "remote.h"
#include "collector_script.h"
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#include <signal.h>
#include <poll.h>
#include <fcntl.h>
#include <cstring>
#include <vector>
#include <chrono>
extern char** environ;

RemoteCollector::RemoteCollector() = default;
RemoteCollector::~RemoteCollector() {
    stop();
}
void RemoteCollector::stop() {
    stopping_ = true;
    wake_.notify_all();
    int pid = child_.load();
    if (pid > 0)
        kill(-pid, SIGTERM);
    if (thread_.joinable())
        thread_.join();
}
void RemoteCollector::start(const Config& config) {
    stop();
    stopping_ = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        state_ = RemoteState{};
        samples_.clear();
        dropped_ = 0;
    }
    thread_ = std::thread(&RemoteCollector::run, this, config);
}
RemoteState RemoteCollector::snapshot() {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_;
}
/** posix_spawn avoids running C++ allocation/locking after fork in a multithreaded desktop. */
void RemoteCollector::run(Config c) {
    while (!stopping_) {
        int output[2]{-1, -1}, input[2]{-1, -1};
        if (pipe2(output, O_CLOEXEC) || pipe2(input, O_CLOEXEC)) {
            for (int fd : output)
                if (fd >= 0)
                    close(fd);
            for (int fd : input)
                if (fd >= 0)
                    close(fd);
            std::lock_guard<std::mutex> lock(mutex_);
            state_.message = L"Could not create SSH pipes.";
            return;
        }
        std::vector<std::string> args = {"ssh", "-T",
                                         "-o",  "BatchMode=yes",
                                         "-o",  "StrictHostKeyChecking=accept-new",
                                         "-o",  "ConnectTimeout=6",
                                         "-o",  "ServerAliveInterval=5",
                                         "-o",  "ServerAliveCountMax=2",
                                         "-o",  "LogLevel=ERROR"};
        if (!c.key.empty()) {
            args.insert(args.end(), {"-o", "IdentitiesOnly=yes", "-i", wideToUtf8(c.key)});
        }
        args.insert(args.end(), {"-p", std::to_string(c.port), "-l", wideToUtf8(c.user), wideToUtf8(c.host),
                                 "sh", "-s", "--", intervalArgument(c.intervalMs)});
        std::vector<char*> argv;
        for (auto& arg : args)
            argv.push_back(arg.data());
        argv.push_back(nullptr);
        posix_spawn_file_actions_t actions;
        posix_spawn_file_actions_init(&actions);
        posix_spawn_file_actions_adddup2(&actions, input[0], STDIN_FILENO);
        posix_spawn_file_actions_adddup2(&actions, output[1], STDOUT_FILENO);
        posix_spawn_file_actions_adddup2(&actions, output[1], STDERR_FILENO);
        for (int fd : input)
            posix_spawn_file_actions_addclose(&actions, fd);
        for (int fd : output)
            posix_spawn_file_actions_addclose(&actions, fd);
        posix_spawnattr_t attr;
        posix_spawnattr_init(&attr);
        posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP);
        posix_spawnattr_setpgroup(&attr, 0);
        pid_t pid = 0;
        const int result = posix_spawnp(&pid, "ssh", &actions, &attr, argv.data(), environ);
        posix_spawnattr_destroy(&attr);
        posix_spawn_file_actions_destroy(&actions);
        close(input[0]);
        close(output[1]);
        if (result == 0) {
            child_ = pid;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                state_.connected = false;
                state_.metrics.reset();
                state_.message = L"Connecting via SSH...";
            }
            // The entire small script is streamed before closing stdin; SIGPIPE is ignored by main.
            const char* script = linuxCollectorScript;
            size_t remaining = strlen(script);
            while (remaining && !stopping_) {
                auto n = write(input[1], script, remaining);
                if (n <= 0)
                    break;
                script += n;
                remaining -= static_cast<size_t>(n);
            }
            close(input[1]);
            std::string pending;
            while (!stopping_) {
                pollfd fd{output[0], POLLIN, 0};
                int ready = poll(&fd, 1, 200);
                if (ready < 0)
                    break;
                if (!ready)
                    continue;
                char buffer[2048];
                auto n = read(output[0], buffer, sizeof(buffer));
                if (n <= 0)
                    break;
                pending.append(buffer, static_cast<size_t>(n));
                size_t end;
                while ((end = pending.find('\n')) != std::string::npos) {
                    auto line = pending.substr(0, end);
                    pending.erase(0, end + 1);
                    if (!line.empty() && line.back() == '\r')
                        line.pop_back();
                    RawSample sample;
                    std::lock_guard<std::mutex> lock(mutex_);
                    if (parseSample(line, monotonicSeconds(), sample)) {
                        sample.session = state_.reconnects;
                        state_.metrics.update(sample);
                        if (samples_.size() >= 256) {
                            samples_.pop_front();
                            ++dropped_;
                        }
                        samples_.push_back(sample);
                        state_.lastSample = sample.time;
                        state_.connected = true;
                        state_.message = L"Encrypted SSH connection";
                    } else if (!line.empty())
                        state_.message = utf8ToWide(line.substr(0, 300));
                }
                if (pending.size() > 8192)
                    pending.clear();
            }
            // A bounded shutdown also handles an SSH process which ignores normal termination.
            kill(-pid, SIGTERM);
            bool reaped = false;
            for (int i = 0; i < 20; ++i) {
                if (waitpid(pid, nullptr, WNOHANG) == pid) {
                    reaped = true;
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            if (!reaped) {
                kill(-pid, SIGKILL);
                waitpid(pid, nullptr, 0);
            }
            child_ = 0;
        } else
            close(input[1]);
        close(output[0]);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            state_.connected = false;
            state_.message =
                result ? L"Could not start system OpenSSH." : L"Disconnected. Retrying in 5 seconds.";
            ++state_.reconnects;
        }
        std::unique_lock<std::mutex> waitLock(waitMutex_);
        wake_.wait_for(waitLock, std::chrono::seconds(5), [&] { return stopping_.load(); });
    }
}

/** Move complete samples to the UI-owned recorder without sharing mutable collector state. */
SampleBatch RemoteCollector::takeSamples() {
    std::lock_guard<std::mutex> lock(mutex_);
    SampleBatch batch;
    batch.samples.assign(samples_.begin(), samples_.end());
    samples_.clear();
    batch.dropped = dropped_;
    dropped_ = 0;
    return batch;
}
