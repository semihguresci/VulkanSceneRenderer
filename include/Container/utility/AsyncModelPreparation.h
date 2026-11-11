#pragma once

#include "Container/utility/ModelImportCancellation.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <limits>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <thread>
#include <type_traits>
#include <utility>
#include <variant>

namespace container::util {

// CPU preparation only: callables must own their inputs, rather than borrow
// renderer/managers whose scene state may change before preparation finishes.
// One task executes at a time, with at most one newer task waiting behind it.
template <class Result> class AsyncModelPreparation {
  static_assert(std::is_nothrow_move_constructible_v<Result>,
                "Prepared model results must support noexcept move construction");

public:
  using RequestId = uint64_t;
  using Task = std::move_only_function<Result(std::stop_token)>;
  using Outcome = std::variant<Result, std::exception_ptr>;

  struct Completion {
    RequestId requestId;
    Outcome outcome;
  };

  struct CancellationStats {
    uint64_t retiredActiveRequests = 0;
    uint64_t cooperativeExits = 0;
    double lastRetirementMs = 0.0;
  };

  AsyncModelPreparation() : worker_([this] { run(); }) {}
  ~AsyncModelPreparation() { shutdown(); }

  AsyncModelPreparation(const AsyncModelPreparation &) = delete;
  AsyncModelPreparation &operator=(const AsyncModelPreparation &) = delete;
  AsyncModelPreparation(AsyncModelPreparation &&) = delete;
  AsyncModelPreparation &operator=(AsyncModelPreparation &&) = delete;

  // The newest request supersedes both queued work and unpublished results.
  // An active task receives a stop request and releases its payload on the
  // preparation thread.
  [[nodiscard]] RequestId start(Task task) {
    if (!task)
      throw std::invalid_argument("Model preparation requires a callable");
    std::stop_source nextStop;
    std::stop_source previousStop{std::nostopstate};
    std::optional<Job> discardedJob;
    RequestId requestId;
    {
      std::lock_guard lock(mutex_);
      if (stopping_)
        throw std::logic_error("Model preparation has shut down");
      if (nextRequestId_ == std::numeric_limits<RequestId>::max())
        throw std::overflow_error("Model preparation request IDs exhausted");
      requestId = ++nextRequestId_;
      latestRequestId_ = requestId;
      takeInto(pending_, discardedJob);
      retireCompletion();
      pending_.emplace(Job{requestId, std::move(task), std::move(nextStop)});
      previousStop = requestActiveStop();
    }
    // Stop callbacks run synchronously. Keep them outside the worker mutex so
    // they can wake an importer or query preparation state.
    previousStop.request_stop();
    changed_.notify_one();
    return requestId;
  }

  // Polling never waits for preparation. Success is outcome index 0, failure
  // index 1; rethrow the exception_ptr on the caller thread for its detail.
  [[nodiscard]] std::optional<Completion> tryTake() {
    std::optional<Completion> result;
    {
      std::lock_guard lock(mutex_);
      takeInto(completion_, result);
    }
    return result;
  }

  // Busy describes the latest request, excluding an already ready completion
  // and old CPU work whose result was canceled or superseded.
  [[nodiscard]] bool busy() const {
    std::lock_guard lock(mutex_);
    return pending_.has_value() ||
           (active_ && active_->requestId == latestRequestId_);
  }

  [[nodiscard]] CancellationStats cancellationStats() const {
    std::lock_guard lock(mutex_);
    return cancellationStats_;
  }

  // Useful for unload: forget outstanding loads immediately. No wait for an
  // importer, and it cannot subsequently publish its canceled scene.
  void cancel() {
    std::optional<Job> discardedJob;
    std::stop_source activeStop{std::nostopstate};
    {
      std::lock_guard lock(mutex_);
      latestRequestId_ = 0;
      takeInto(pending_, discardedJob);
      retireCompletion();
      activeStop = requestActiveStop();
    }
    activeStop.request_stop();
    changed_.notify_one();
  }

  // Return a consumed CPU payload after installation without freeing its large
  // arrays on the UI thread. The result's move must transfer its ownership.
  // The frontend retains at most one consumed result; discard it before taking
  // another, keeping reclamation bounded alongside the one completion slot.
  void discard(Result result) {
    {
      std::lock_guard lock(mutex_);
      if (stopping_)
        throw std::logic_error("Model preparation has shut down");
      retired_.emplace_back(std::in_place_index<0>, std::move(result));
    }
    changed_.notify_one();
  }

  // Call on the owner thread before destroying any resources used by tasks.
  // Shutdown discards queued work, requests cancellation, and joins the worker.
  // It is idempotent; concurrent shutdown calls are not supported.
  void shutdown() {
    std::optional<Job> discardedJob;
    std::stop_source activeStop{std::nostopstate};
    {
      std::lock_guard lock(mutex_);
      stopping_ = true;
      latestRequestId_ = 0;
      takeInto(pending_, discardedJob);
      retireCompletion();
      activeStop = requestActiveStop();
    }
    activeStop.request_stop();
    changed_.notify_one();
    if (worker_.joinable())
      worker_.join();
  }

private:
  using Clock = std::chrono::steady_clock;

  struct Job {
    RequestId requestId;
    Task task;
    std::stop_source stop;
  };

  struct ActiveRequest {
    RequestId requestId;
    std::stop_source stop;
    std::optional<Clock::time_point> cancelledAt;
  };

  template <class T>
  static void takeInto(std::optional<T> &source, std::optional<T> &target) {
    if (source) {
      target.emplace(std::move(*source));
      source.reset();
    }
  }

  static Outcome prepare(Job &job, bool &cooperativeExit) noexcept {
    try {
      const auto stop = job.stop.get_token();
      checkModelImportCancellation(stop);
      return Outcome(std::in_place_index<0>, job.task(stop));
    } catch (const ModelImportCancelled &) {
      cooperativeExit = true;
      return Outcome(std::in_place_index<1>, std::current_exception());
    } catch (...) {
      return Outcome(std::in_place_index<1>, std::current_exception());
    }
  }

  // Called under mutex_. The caller signals the captured source after unlock.
  std::stop_source requestActiveStop() {
    if (!active_)
      return std::stop_source{std::nostopstate};
    if (!active_->cancelledAt)
      active_->cancelledAt = Clock::now();
    return active_->stop;
  }

  // Called under mutex_; moved-from results own no arrays. Full payloads and
  // exception objects are reclaimed by the worker outside its critical section.
  void retireCompletion() {
    if (completion_) {
      retired_.emplace_back(std::move(completion_->outcome));
      completion_.reset();
    }
  }

  void run() {
    for (;;) {
      std::optional<Job> job;
      std::deque<Outcome> retired;
      {
        std::unique_lock lock(mutex_);
        changed_.wait(lock, [&] {
          return stopping_ || pending_.has_value() || !retired_.empty();
        });
        if (!retired_.empty()) {
          retired.swap(retired_);
        } else {
          if (stopping_)
            return;
          takeInto(pending_, job);
          active_.emplace(ActiveRequest{job->requestId, job->stop, std::nullopt});
        }
      }
      if (!retired.empty())
        continue; // Reclaim before starting another import or finishing shutdown.
      std::optional<Clock::time_point> cancelledAt;
      bool cooperativeExit = false;
      {
        Completion prepared{job->requestId, prepare(*job, cooperativeExit)};
        std::lock_guard lock(mutex_);
        cancelledAt = active_->cancelledAt;
        active_.reset();
        if (!stopping_ && prepared.requestId == latestRequestId_)
          completion_.emplace(std::move(prepared));
      }
      // Superseded results and callable captures are destroyed outside the
      // mutex, so their cleanup does not obstruct main-thread polling.
      if (cancelledAt) {
        job.reset();
        const auto elapsed = std::chrono::duration<double, std::milli>(
            Clock::now() - *cancelledAt);
        std::lock_guard lock(mutex_);
        ++cancellationStats_.retiredActiveRequests;
        cancellationStats_.cooperativeExits += cooperativeExit;
        cancellationStats_.lastRetirementMs = elapsed.count();
      }
    }
  }

  mutable std::mutex mutex_;
  std::condition_variable changed_;
  std::optional<Job> pending_;
  std::optional<ActiveRequest> active_;
  std::optional<Completion> completion_;
  std::deque<Outcome> retired_;
  CancellationStats cancellationStats_;
  RequestId nextRequestId_{0};
  RequestId latestRequestId_{0};
  bool stopping_{false};
  std::thread worker_;
};

} // namespace container::util
