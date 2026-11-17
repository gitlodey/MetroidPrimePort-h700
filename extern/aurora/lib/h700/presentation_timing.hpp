#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>

namespace aurora::h700 {

struct FrameWallTimings {
  using Clock = std::chrono::steady_clock;
  enum Field : size_t { Prepare, Submit, Queue, CompletionObserved, Present, Swap, Count };
  using Sample = std::array<double, Count>;

  Clock::time_point acquired = Clock::now();
  Clock::time_point submit_begin{}, queue_returned{}, submit_returned{}, completion_observed{};

  Sample sample(Clock::time_point present_begin, Clock::time_point swap_begin,
                Clock::time_point swap_returned, Clock::time_point presented) const {
    const auto ms = [](auto begin, auto end) { return std::chrono::duration<double, std::milli>(end - begin).count(); };
    // Queue/Swap are subsets. Completion includes main-thread polling delay, not just GPU work.
    return {ms(acquired, submit_begin), ms(submit_begin, submit_returned), ms(submit_begin, queue_returned),
            ms(submit_returned, completion_observed), ms(present_begin, presented), ms(swap_begin, swap_returned)};
  }
};

struct FrameWallSummary {
  size_t frames = 0;
  FrameWallTimings::Sample total{}, maximum{};

  void record(const FrameWallTimings::Sample& sample) {
    ++frames;
    for (size_t i = 0; i < sample.size(); ++i) {
      total[i] += sample[i];
      maximum[i] = std::max(maximum[i], sample[i]);
    }
  }
  FrameWallTimings::Sample mean() const {
    auto result = total;
    if (frames != 0) for (auto& value : result) value /= frames;
    return result;
  }
};

} // namespace aurora::h700
