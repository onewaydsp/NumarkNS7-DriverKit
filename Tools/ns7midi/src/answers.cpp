// answers.cpp

#include "answers.h"

#include <chrono>
#include <cstdio>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

LineSource::~LineSource()
{
    if (ownsFd_) close(fd_);
}

std::unique_ptr<LineSource> LineSource::Stdin()
{
    return std::unique_ptr<LineSource>(new LineSource(STDIN_FILENO, false, false));
}

std::unique_ptr<LineSource> LineSource::Open(const std::string & path, std::string * error)
{
    // O_RDWR on a FIFO keeps it open when the writer closes between answers.
    int fd = open(path.c_str(), O_RDWR);
    if (fd < 0) fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) { *error = path + ": cannot open"; return nullptr; }
    return std::unique_ptr<LineSource>(new LineSource(fd, true, true));
}

bool LineSource::TakeBufferedLine(std::string * line)
{
    while (true) {
        const size_t newline = buffer_.find('\n');
        if (newline == std::string::npos) return false;
        std::string text = buffer_.substr(0, newline);
        buffer_.erase(0, newline + 1);
        if (!text.empty() && text.back() == '\r') text.pop_back();
        if (script_) {
            // Skip comment-only lines; strip trailing comments and spaces.
            const size_t hash = text.find('#');
            const bool commentOnly = hash != std::string::npos &&
                                     text.find_first_not_of(" \t") == hash;
            if (commentOnly) continue;
            if (hash != std::string::npos) text.resize(hash);
            while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) text.pop_back();
        }
        *line = text;
        return true;
    }
}

LineSource::Result LineSource::WaitLine(double timeoutSec, std::string * line)
{
    using Clock = std::chrono::steady_clock;
    const auto deadline = Clock::now() + std::chrono::duration<double>(timeoutSec < 0 ? 0 : timeoutSec);
    while (true) {
        if (TakeBufferedLine(line)) return Result::Line;
        if (eof_) {
            if (!buffer_.empty()) { *line = buffer_; buffer_.clear(); return Result::Line; }
            return Result::Eof;
        }
        int waitMs = -1;
        if (timeoutSec >= 0) {
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
            if (left <= 0) return Result::Timeout;
            waitMs = int(left);
        }
        pollfd pfd = { fd_, POLLIN, 0 };
        const int ready = poll(&pfd, 1, waitMs);
        if (ready == 0) return Result::Timeout;
        if (ready < 0) continue;   // EINTR
        char chunk[512];
        const ssize_t n = read(fd_, chunk, sizeof chunk);
        if (n <= 0) eof_ = true;
        else buffer_.append(chunk, size_t(n));
    }
}
