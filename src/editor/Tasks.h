#pragma once

#include <functional>
#include <future>
#include <memory>
#include <string>
#include <vector>

namespace throne {
/// Long work the editor keeps drawing frames through, so the window never freezes or
/// goes blank while it runs (TDDja/09 10). A step runs a little on the main thread every
/// frame; background work runs on a worker thread and then finishes on the main thread.
/// While any task runs, draw() shows a window listing them, each with a progress bar
/// when its progress is known and a spinner otherwise.
class Tasks {
public:
    /// The share done, from 0 to 1, or a negative value when it is not known.
    using Progress = std::function<float()>;

    /// Adds work that update() advances once per frame, from the second frame its entry
    /// has been on screen (a step may stall its frame), until @p step answers true.
    /// @p detail, when given, adds a line under the label.
    void add(std::string label, std::function<bool()> step, Progress progress = nullptr,
             std::function<std::string()> detail = nullptr);

    /// Runs @p work on a worker thread; once it returns, update() passes its result to
    /// @p finish on the main thread. @p work must not touch OGRE or the editor's state.
    template <class Result>
    void background(std::string label, std::function<Result()> work, std::function<void(Result&)> finish) {
        auto future = std::make_shared<std::future<Result>>(std::async(std::launch::async, std::move(work)));
        add(std::move(label), [future, finish = std::move(finish)] {
            if (future->wait_for(std::chrono::seconds(0)) != std::future_status::ready) return false;
            auto result = future->get();
            finish(result);
            return true;
        });
    }

    /// Advances every task once and drops the finished ones; call once per frame.
    void update();
    /// Draws the progress window while a task runs.
    void draw();
    /// Whether any task is still running.
    bool busy() const { return !tasks_.empty(); }

private:
    /// One running task and the frames its entry has been drawn in.
    struct Task {
        std::string label;
        std::function<bool()> step;
        Progress progress;
        std::function<std::string()> detail;
        unsigned shown = 0;
    };
    std::vector<Task> tasks_;
    /// Tasks added while update() runs, joined once it is done.
    std::vector<Task> added_;
    bool updating_ = false;
};
}
