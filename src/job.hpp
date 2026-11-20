#pragma once

#include "files.hpp"

#include <future>
#include <thread>
#include <utility>

namespace nf {

template<class Result>
struct Job {
    std::shared_ptr<Progress> progress;
    std::future<Result> future;

    bool ready() const {
        return future.valid() && future.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
    }

    explicit operator bool() const { return future.valid(); }

    template<class Work>
    void start(Work work, Phase phase = Phase::Read) {
        auto state = std::make_shared<Progress>();
        state->phase = phase;
        std::packaged_task<Result()> task([state, work = std::move(work)]() mutable {
            return work(*state);
        });
        auto value = task.get_future();
        std::thread worker(std::move(task));
        worker.detach();
        if (progress) progress->cancel = true;
        progress = std::move(state);
        future = std::move(value);
    }
};

}
