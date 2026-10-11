#include "Container/utility/AsyncModelPreparation.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

namespace {
using namespace std::chrono_literals;
using Worker = container::util::AsyncModelPreparation<int>;

// A preparation task is held until the test explicitly releases it. The guard
// must be declared after its worker so assertion exits cannot strand a join.
struct Gate {
  std::promise<void> entered;
  std::future<void> enteredSignal{entered.get_future()};
  std::promise<void> release;
  std::shared_future<void> releaseSignal{release.get_future().share()};
  std::atomic<bool> released{false};

  void hold() {
    entered.set_value();
    releaseSignal.wait();
  }
  void unblock() {
    if (!released.exchange(true))
      release.set_value();
  }
};

struct ReleaseOnExit {
  Gate &gate;
  ~ReleaseOnExit() { gate.unblock(); }
};

struct PayloadDestruction {
  std::promise<std::thread::id> destroyed;
  std::future<std::thread::id> signal{destroyed.get_future()};
};

struct CpuPayload {
  std::shared_ptr<PayloadDestruction> destruction;
  explicit CpuPayload(std::shared_ptr<PayloadDestruction> value)
      : destruction(std::move(value)) {}
  CpuPayload(const CpuPayload &) = delete;
  CpuPayload &operator=(const CpuPayload &) = delete;
  ~CpuPayload() {
    destruction->destroyed.set_value(std::this_thread::get_id());
  }
};

using Payload = std::unique_ptr<CpuPayload>;
using PayloadWorker = container::util::AsyncModelPreparation<Payload>;

template <class Predicate> bool await(Predicate predicate) {
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate())
      return true;
    std::this_thread::yield();
  }
  return predicate();
}

template <class Result>
std::optional<typename container::util::AsyncModelPreparation<Result>::Completion>
takeReady(container::util::AsyncModelPreparation<Result> &worker) {
  std::optional<typename container::util::AsyncModelPreparation<Result>::Completion>
      result;
  await([&] {
    auto ready = worker.tryTake();
    if (ready) {
      result.emplace(std::move(*ready));
      return true;
    }
    return false;
  });
  return result;
}

TEST(AsyncModelPreparation, StartsIdleAndRejectsEmptyCallable) {
  Worker worker;
  EXPECT_FALSE(worker.busy());
  EXPECT_FALSE(worker.tryTake());
  EXPECT_THROW(worker.start({}), std::invalid_argument);
  EXPECT_FALSE(worker.busy());
}

TEST(AsyncModelPreparation, PreparationRunsOffThreadAndPollingDoesNotWaitForIt) {
  Gate gate;
  Worker worker;
  ReleaseOnExit guard{gate};
  const auto callerThread = std::this_thread::get_id();
  std::thread::id preparationThread;
  const auto request = worker.start([&] {
    preparationThread = std::this_thread::get_id();
    gate.hold();
    return 42;
  });
  ASSERT_EQ(gate.enteredSignal.wait_for(2s), std::future_status::ready);
  EXPECT_NE(preparationThread, callerThread);
  EXPECT_TRUE(worker.busy());

  // A regression that waits for task completion fails this bounded probe,
  // while still releasing the task before any future/worker destructor joins.
  auto poll = std::async(std::launch::async, [&] { return worker.tryTake(); });
  const auto pollState = poll.wait_for(500ms);
  gate.unblock();
  EXPECT_EQ(pollState, std::future_status::ready);
  auto early = poll.get();
  EXPECT_FALSE(early);
  auto completed = takeReady(worker);
  ASSERT_TRUE(completed);
  EXPECT_EQ(completed->requestId, request);
  ASSERT_EQ(completed->outcome.index(), 0u);
  EXPECT_EQ(std::get<0>(completed->outcome), 42);
  EXPECT_FALSE(worker.busy());
  EXPECT_FALSE(worker.tryTake());
}

TEST(AsyncModelPreparation, SupportsOwnedMoveOnlyInputsAndResults) {
  container::util::AsyncModelPreparation<std::unique_ptr<int>> worker;
  const auto request = worker.start([value = std::make_unique<int>(73)]() mutable {
    return std::move(value);
  });
  auto completed = takeReady(worker);
  ASSERT_TRUE(completed);
  EXPECT_EQ(completed->requestId, request);
  ASSERT_EQ(completed->outcome.index(), 0u);
  ASSERT_TRUE(std::get<0>(completed->outcome));
  EXPECT_EQ(*std::get<0>(completed->outcome), 73);
}

TEST(AsyncModelPreparation, ReturnsImporterErrorForHandlingOnCallerThread) {
  Worker worker;
  const auto request = worker.start([]() -> int {
    throw std::runtime_error("malformed model: missing geometry");
  });
  auto completed = takeReady(worker);
  ASSERT_TRUE(completed);
  EXPECT_EQ(completed->requestId, request);
  ASSERT_EQ(completed->outcome.index(), 1u);
  try {
    std::rethrow_exception(std::get<1>(completed->outcome));
    FAIL() << "Missing importer exception";
  } catch (const std::runtime_error &error) {
    EXPECT_EQ(std::string(error.what()), "malformed model: missing geometry");
  }
  EXPECT_FALSE(worker.busy());
  const auto recovery = worker.start([] { return 9; });
  auto recovered = takeReady(worker);
  ASSERT_TRUE(recovered);
  EXPECT_EQ(recovered->requestId, recovery);
  ASSERT_EQ(recovered->outcome.index(), 0u);
  EXPECT_EQ(std::get<0>(recovered->outcome), 9);
}

TEST(AsyncModelPreparation, PreservesNonStandardImporterExceptions) {
  Worker worker;
  const auto request = worker.start([]() -> int { throw 19; });
  auto completed = takeReady(worker);
  ASSERT_TRUE(completed);
  EXPECT_EQ(completed->requestId, request);
  ASSERT_EQ(completed->outcome.index(), 1u);
  try {
    std::rethrow_exception(std::get<1>(completed->outcome));
    FAIL() << "Missing non-standard importer exception";
  } catch (int value) {
    EXPECT_EQ(value, 19);
  }
}

TEST(AsyncModelPreparation, OnlyNewestPendingRequestExecutesAndActiveResultIsDiscarded) {
  Gate gate;
  Worker worker;
  ReleaseOnExit guard{gate};
  std::atomic<int> executing{0}, peakExecuting{0};
  std::atomic<bool> middleExecuted{false};
  auto enter = [&] {
    const int active = ++executing;
    int peak = peakExecuting.load();
    while (peak < active && !peakExecuting.compare_exchange_weak(peak, active)) {}
  };
  const auto first = worker.start([&] {
    enter();
    gate.hold();
    --executing;
    return 1;
  });
  ASSERT_EQ(gate.enteredSignal.wait_for(2s), std::future_status::ready);
  const auto middle = worker.start([&] {
    middleExecuted = true;
    return 2;
  });
  const auto latest = worker.start([&] {
    enter();
    --executing;
    return 3;
  });
  EXPECT_LT(first, middle);
  EXPECT_LT(middle, latest);
  EXPECT_TRUE(worker.busy());
  EXPECT_FALSE(worker.tryTake());
  gate.unblock();
  auto completed = takeReady(worker);
  ASSERT_TRUE(completed);
  EXPECT_EQ(completed->requestId, latest);
  ASSERT_EQ(completed->outcome.index(), 0u);
  EXPECT_EQ(std::get<0>(completed->outcome), 3);
  EXPECT_FALSE(middleExecuted);
  EXPECT_EQ(peakExecuting, 1);
  EXPECT_FALSE(worker.tryTake());
}

TEST(AsyncModelPreparation, NewRequestAlsoDiscardsAlreadyReadyResult) {
  Worker worker;
  const auto first = worker.start([] { return 1; });
  ASSERT_TRUE(await([&] { return !worker.busy(); }));
  const auto latest = worker.start([] { return 2; });
  EXPECT_GT(latest, first);
  auto completed = takeReady(worker);
  ASSERT_TRUE(completed);
  EXPECT_EQ(completed->requestId, latest);
  ASSERT_EQ(completed->outcome.index(), 0u);
  EXPECT_EQ(std::get<0>(completed->outcome), 2);
  EXPECT_FALSE(worker.tryTake());
}

TEST(AsyncModelPreparation, UnloadCancellationAlsoDiscardsAlreadyPreparedScene) {
  Worker worker;
  const auto first = worker.start([] { return 1; });
  ASSERT_TRUE(await([&] { return !worker.busy(); }));
  worker.cancel();
  EXPECT_FALSE(worker.tryTake());
  EXPECT_FALSE(worker.busy());
  const auto replacement = worker.start([] { return 2; });
  EXPECT_GT(replacement, first);
  auto completed = takeReady(worker);
  ASSERT_TRUE(completed);
  EXPECT_EQ(completed->requestId, replacement);
  ASSERT_EQ(completed->outcome.index(), 0u);
  EXPECT_EQ(std::get<0>(completed->outcome), 2);
}

TEST(AsyncModelPreparation, UnloadCancellationReturnsImmediatelyAndPreventsLateLoad) {
  Gate gate;
  Worker worker;
  ReleaseOnExit guard{gate};
  std::atomic<bool> queuedExecuted{false}, activeFinished{false};
  const auto first = worker.start([&] {
    gate.hold();
    activeFinished = true;
    return 1;
  });
  ASSERT_EQ(gate.enteredSignal.wait_for(2s), std::future_status::ready);
  const auto queued = worker.start([&] {
    queuedExecuted = true;
    return 2;
  });
  EXPECT_GT(queued, first);
  auto cancel = std::async(std::launch::async, [&] { worker.cancel(); });
  const auto cancelState = cancel.wait_for(500ms);
  // Release even when cancellation regresses to waiting on active work.
  gate.unblock();
  EXPECT_EQ(cancelState, std::future_status::ready);
  cancel.get();
  EXPECT_FALSE(worker.busy());
  worker.shutdown();
  EXPECT_TRUE(activeFinished);
  EXPECT_FALSE(queuedExecuted);
  EXPECT_FALSE(worker.tryTake());
}

TEST(AsyncModelPreparation, CanQueueNewLoadAfterCancelingActiveLoad) {
  Gate gate;
  Worker worker;
  ReleaseOnExit guard{gate};
  const auto first = worker.start([&] {
    gate.hold();
    return 1;
  });
  ASSERT_EQ(gate.enteredSignal.wait_for(2s), std::future_status::ready);
  worker.cancel();
  EXPECT_FALSE(worker.busy());
  const auto replacement = worker.start([] { return 5; });
  EXPECT_GT(replacement, first);
  EXPECT_TRUE(worker.busy());
  gate.unblock();
  auto completed = takeReady(worker);
  ASSERT_TRUE(completed);
  EXPECT_EQ(completed->requestId, replacement);
  ASSERT_EQ(completed->outcome.index(), 0u);
  EXPECT_EQ(std::get<0>(completed->outcome), 5);
}

TEST(AsyncModelPreparation, ShutdownJoinsActiveWorkDropsQueueAndRejectsNewRequests) {
  Gate gate;
  Worker worker;
  ReleaseOnExit guard{gate};
  std::atomic<bool> activeFinished{false}, queuedExecuted{false};
  const auto first = worker.start([&] {
    gate.hold();
    activeFinished = true;
    return 1;
  });
  ASSERT_EQ(gate.enteredSignal.wait_for(2s), std::future_status::ready);
  const auto queued = worker.start([&] {
    queuedExecuted = true;
    return 2;
  });
  EXPECT_GT(queued, first);
  auto shutdown = std::async(std::launch::async, [&] { worker.shutdown(); });
  const bool stopping = await([&] { return !worker.busy(); });
  const auto beforeRelease = shutdown.wait_for(0s);
  // Evaluate start only after shutdown has invalidated the latest request.
  if (stopping)
    EXPECT_THROW(worker.start([] { return 3; }), std::logic_error);
  gate.unblock();
  EXPECT_TRUE(stopping);
  EXPECT_EQ(beforeRelease, std::future_status::timeout);
  ASSERT_EQ(shutdown.wait_for(2s), std::future_status::ready);
  shutdown.get();
  EXPECT_TRUE(activeFinished);
  EXPECT_FALSE(queuedExecuted);
  EXPECT_FALSE(worker.tryTake());
  EXPECT_FALSE(worker.busy());
  worker.shutdown();
}

TEST(AsyncModelPreparation, DestructionJoinsOwnedTaskWithoutPublishingStaleScene) {
  Gate gate;
  auto worker = std::make_unique<Worker>();
  ReleaseOnExit guard{gate};
  std::atomic<bool> finished{false};
  const auto request = worker->start([&] {
    gate.hold();
    finished = true;
    return 1;
  });
  EXPECT_NE(request, 0u);
  ASSERT_EQ(gate.enteredSignal.wait_for(2s), std::future_status::ready);
  auto destruction = std::async(std::launch::async, [&] { worker.reset(); });
  const auto beforeRelease = destruction.wait_for(100ms);
  gate.unblock();
  EXPECT_EQ(beforeRelease, std::future_status::timeout);
  ASSERT_EQ(destruction.wait_for(2s), std::future_status::ready);
  destruction.get();
  EXPECT_TRUE(finished);
  EXPECT_FALSE(worker);
}

TEST(AsyncModelPreparation, DiscardedInstalledPayloadIsDestroyedOnPreparationThread) {
  PayloadWorker worker;
  auto destruction = std::make_shared<PayloadDestruction>();
  std::thread::id preparationThread;
  const auto request = worker.start([&] {
    preparationThread = std::this_thread::get_id();
    return std::make_unique<CpuPayload>(destruction);
  });
  auto completed = takeReady(worker);
  ASSERT_TRUE(completed);
  EXPECT_EQ(completed->requestId, request);
  ASSERT_EQ(completed->outcome.index(), 0u);
  worker.discard(std::move(std::get<0>(completed->outcome)));
  completed.reset();
  ASSERT_EQ(destruction->signal.wait_for(2s), std::future_status::ready);
  const auto destructorThread = destruction->signal.get();
  EXPECT_EQ(destructorThread, preparationThread);
  EXPECT_NE(destructorThread, std::this_thread::get_id());
}

TEST(AsyncModelPreparation, CanceledReadyPayloadIsDestroyedOnPreparationThread) {
  PayloadWorker worker;
  auto destruction = std::make_shared<PayloadDestruction>();
  std::thread::id preparationThread;
  const auto request = worker.start([&] {
    preparationThread = std::this_thread::get_id();
    return std::make_unique<CpuPayload>(destruction);
  });
  EXPECT_NE(request, 0u);
  ASSERT_TRUE(await([&] { return !worker.busy(); }));
  worker.cancel();
  ASSERT_EQ(destruction->signal.wait_for(2s), std::future_status::ready);
  const auto destructorThread = destruction->signal.get();
  EXPECT_EQ(destructorThread, preparationThread);
  EXPECT_NE(destructorThread, std::this_thread::get_id());
  EXPECT_FALSE(worker.tryTake());
}

TEST(AsyncModelPreparation, SupersededReadyPayloadIsReclaimedBeforeNextPreparation) {
  PayloadWorker worker;
  auto destruction = std::make_shared<PayloadDestruction>();
  const auto first = worker.start([destruction] {
    return std::make_unique<CpuPayload>(destruction);
  });
  ASSERT_TRUE(await([&] { return !worker.busy(); }));
  std::atomic<bool> reclaimedBeforePreparation{false};
  const auto replacement = worker.start([&] {
    reclaimedBeforePreparation =
        destruction->signal.wait_for(0s) == std::future_status::ready;
    return Payload{};
  });
  auto completed = takeReady(worker);
  ASSERT_TRUE(completed);
  EXPECT_GT(replacement, first);
  EXPECT_EQ(completed->requestId, replacement);
  EXPECT_TRUE(reclaimedBeforePreparation);
  ASSERT_EQ(destruction->signal.wait_for(2s), std::future_status::ready);
  EXPECT_NE(destruction->signal.get(), std::this_thread::get_id());
}

TEST(AsyncModelPreparation, DiscardIsNonblockingDuringImportAndShutdownDrainsRetiredPayloads) {
  Gate gate;
  PayloadWorker worker;
  ReleaseOnExit guard{gate};
  auto destruction = std::make_shared<PayloadDestruction>();
  std::thread::id preparationThread;
  const auto request = worker.start([&] {
    preparationThread = std::this_thread::get_id();
    gate.hold();
    return Payload{};
  });
  EXPECT_NE(request, 0u);
  ASSERT_EQ(gate.enteredSignal.wait_for(2s), std::future_status::ready);
  auto discard = std::async(std::launch::async, [&] {
    worker.discard(std::make_unique<CpuPayload>(destruction));
  });
  const auto discardState = discard.wait_for(500ms);
  // The importer is gated, so reclamation cannot run until it is released.
  const auto beforeRelease = destruction->signal.wait_for(0s);
  gate.unblock();
  EXPECT_EQ(discardState, std::future_status::ready);
  EXPECT_EQ(beforeRelease, std::future_status::timeout);
  discard.get();
  worker.shutdown();
  ASSERT_EQ(destruction->signal.wait_for(0s), std::future_status::ready);
  EXPECT_EQ(destruction->signal.get(), preparationThread);
}

} // namespace
