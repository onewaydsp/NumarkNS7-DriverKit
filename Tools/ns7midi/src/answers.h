// answers.h
// Where step answers come from: the terminal, or a --script file. A script
// may be a regular file (answers are read as soon as they are needed) or a
// FIFO that a coordinator writes to while the session runs.

#pragma once

#include <memory>
#include <string>

class LineSource {
public:
    enum class Result { Line, Timeout, Eof };

    ~LineSource();
    static std::unique_ptr<LineSource> Stdin();
    static std::unique_ptr<LineSource> Open(const std::string & path, std::string * error);

    // Waits up to timeoutSec (< 0 means forever) for one line. Script lines
    // that are blank after stripping a "#" comment count as "accept" only if
    // they are exactly empty; comment-only lines are skipped.
    Result WaitLine(double timeoutSec, std::string * line);
    bool IsScript() const { return script_; }

private:
    LineSource(int fd, bool script, bool ownsFd) : fd_(fd), script_(script), ownsFd_(ownsFd) {}
    bool TakeBufferedLine(std::string * line);

    int fd_;
    bool script_;
    bool ownsFd_;
    bool eof_ = false;
    std::string buffer_;
};
