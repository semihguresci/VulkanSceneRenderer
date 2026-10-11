#pragma once

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <limits>
#include <mutex>
#include <optional>
#include <stdexcept>
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
  using Task = std::move_only_function<Result()>;
  using Outcome = std::variant<Result, std::exception_ptr>;

  struct Completion {
    RequestId requestId;
    Outcome outcome;
  };

  AsyncModelPreparation() : worker_([this] { run(); }) {}
  ~AsyncModelPreparation() { shutdown(); }

  AsyncModelPreparation(const AsyncModelPreparation &) = delete;
  AsyncModelPreparation &operator=(const AsyncModelPreparation &) = delete;
  AsyncModelPreparation(AsyncModelPreparation &&) = delete;
  AsyncModelPreparation &operator=(AsyncModelPreparation &&) = delete;

  // The newest request supersedes both queued work and unpublished results.
  // An already executing CPU task is allowed to finish, then discarded.
  [[nodiscard]] RequestId start(Task task) {
    if (!task)
      throw std::invalid_argument("Model preparation requires a callable");
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
      pending_.emplace(Job{requestId, std::move(task)});
    }
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
           (executing_ && executingRequestId_ == latestRequestId_);
  }

  // Useful for unload: forget outstanding loads immediately. No wait for an
  // importer, and it cannot subsequently publish its canceled scene.
  void cancel() {
    std::optional<Job> discardedJob;
    {
      std::lock_guard lock(mutex_);
      latestRequestId_ = 0;
      takeInto(pending_, discardedJob);
      retireCompletion();
    }
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
  // Shutdown discards queued work and joins the one running CPU operation.
  // It is idempotent; concurrent shutdown calls are not supported.
  void shutdown() {
    std::optional<Job> discardedJob;
    {
      std::lock_guard lock(mutex_);
      stopping_ = true;
      latestRequestId_ = 0;
      takeInto(pending_, discardedJob);
      retireCompletion();
    }
    changed_.notify_one();
    if (worker_.joinable())
      worker_.join();
  }

private:
  struct Job {
    RequestId requestId;
    Task task;
  };

  template <class T>
  static void takeInto(std::optional<T> &source, std::optional<T> &target) {
    if (source) {
      target.emplace(std::move(*source));
      source.reset();
    }
  }

  static Outcome prepare(Task &task) noexcept {
    try {
      return Outcome(std::in_place_index<0>, task());
    } catch (...) {
      return Outcome(std::in_place_index<1>, std::current_exception());
    }
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
          executing_ = true;
          executingRequestId_ = job->requestId;
        }
      }
      if (!retired.empty())
        continue; // Reclaim before starting another import or finishing shutdown.
      Completion prepared{job->requestId, prepare(job->task)};
      {
        std::lock_guard lock(mutex_);
        executing_ = false;
        executingRequestId_ = 0;
        if (!stopping_ && prepared.requestId == latestRequestId_)
          completion_.emplace(std::move(prepared));
      }
      // Superseded results and callable captures are destroyed outside the
      // mutex, so their cleanup does not obstruct main-thread polling.
    }
  }

  mutable std::mutex mutex_;
  std::condition_variable changed_;
  std::optional<Job> pending_;
  std::optional<Completion> completion_;
  std::deque<Outcome> retired_;
  RequestId nextRequestId_{0};
  RequestId latestRequestId_{0};
  RequestId executingRequestId_{0};
  bool executing_{false};
  bool stopping_{false};
  std::thread worker_;
};

} // namespace container::util
