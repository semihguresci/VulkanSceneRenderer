#include "Container/renderer/lighting/SubmittedUploadWait.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <exception>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using container::renderer::detail::waitForSubmittedUpload;

struct ObservedOwner {
  std::vector<int> &events;
  ~ObservedOwner() { events.push_back(3); }
};

TEST(SubmittedUploadWait, SuccessfulFenceDoesNotDrainQueue) {
  bool drained = false;
  EXPECT_NO_THROW(waitForSubmittedUpload([] {}, [&] { drained = true; }));
  EXPECT_FALSE(drained);
}

TEST(SubmittedUploadWait, HostOomDrainsBeforeOwnersUnwind) {
  std::vector<int> events;
  try {
    ObservedOwner owner{events};
    waitForSubmittedUpload(
        [&] {
          events.push_back(1);
          throw vk::OutOfHostMemoryError("fence wait");
        },
        [&] { events.push_back(2); });
    FAIL() << "Original wait failure must propagate";
  } catch (const vk::OutOfHostMemoryError &error) {
    EXPECT_NE(std::string(error.what()).find("fence wait"), std::string::npos);
  }
  EXPECT_EQ(events, (std::vector<int>{1, 2, 3}));
}

TEST(SubmittedUploadWait, DeviceOomDrainsBeforeOwnersUnwind) {
  std::vector<int> events;
  try {
    ObservedOwner owner{events};
    waitForSubmittedUpload(
        [&] {
          events.push_back(1);
          throw vk::OutOfDeviceMemoryError("fence wait");
        },
        [&] { events.push_back(2); });
    FAIL() << "Original wait failure must propagate";
  } catch (const vk::OutOfDeviceMemoryError &) {
  }
  EXPECT_EQ(events, (std::vector<int>{1, 2, 3}));
}

TEST(SubmittedUploadWait, DeviceLossFromWaitNeedsNoDrain) {
  bool drained = false;
  EXPECT_THROW(waitForSubmittedUpload(
                   [] { throw vk::DeviceLostError("fence wait"); },
                   [&] { drained = true; }),
               vk::DeviceLostError);
  EXPECT_FALSE(drained);
}

TEST(SubmittedUploadWait, DeviceLossFromDrainPreservesOriginalError) {
  std::vector<int> events;
  try {
    ObservedOwner owner{events};
    waitForSubmittedUpload(
        [&] {
          events.push_back(1);
          throw vk::OutOfHostMemoryError("fence wait");
        },
        [&] {
          events.push_back(2);
          throw vk::DeviceLostError("queue drain");
        });
    FAIL() << "Original wait failure must propagate";
  } catch (const vk::OutOfHostMemoryError &) {
  }
  EXPECT_EQ(events, (std::vector<int>{1, 2, 3}));
}

TEST(SubmittedUploadWait, UnresolvedDrainFailureStopsBeforeOwnersUnwind) {
  EXPECT_EXIT(
      {
        std::set_terminate([] { std::_Exit(63); });
        struct PendingOwner {
          ~PendingOwner() { std::_Exit(64); }
        } owner;
        waitForSubmittedUpload(
            [] { throw vk::OutOfHostMemoryError("fence wait"); },
            [] { throw vk::OutOfDeviceMemoryError("queue drain"); });
        std::_Exit(65);
      },
      ::testing::ExitedWithCode(63), "");
}

} // namespace
