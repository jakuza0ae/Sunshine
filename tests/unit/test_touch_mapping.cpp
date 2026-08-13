/**
 * @file tests/unit/test_touch_mapping.cpp
 * @brief Tests for pure native touch coordinate mapping.
 */
#include "../tests_common.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <limits>
#include <thread>

#include <src/input.h>
#include <src/platform/windows/touch_scheduler.h>

namespace {
  input::touch_port_t make_port(int offset_x = 0, int offset_y = 0, int width = 1920, int height = 1080) {
    input::touch_port_t port {};
    port.offset_x = offset_x;
    port.offset_y = offset_y;
    port.width = width;
    port.height = height;
    port.env_width = width;
    port.env_height = height;
    port.scalar_inv = 1.0f;
    port.scalar_tpcoords = 1.0f;
    return port;
  }
}

TEST(TouchMapping, MapsNormalVddOriginToMonitorLocalCoordinates) {
  auto result = input::map_client_touch_to_monitor(make_port(), 0.25f, 0.75f);

  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->port.offset_x, 0);
  EXPECT_EQ(result->port.offset_y, 0);
  EXPECT_EQ(result->port.width, 1920);
  EXPECT_EQ(result->port.height, 1080);
  EXPECT_FLOAT_EQ(result->x, 0.25f);
  EXPECT_FLOAT_EQ(result->y, 0.75f);
}

TEST(TouchMapping, PreservesNegativeDesktopOriginWithoutChangingLocalCoordinates) {
  auto result = input::map_client_touch_to_monitor(make_port(-1920, -120), 0.5f, 0.5f);

  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->port.offset_x, -1920);
  EXPECT_EQ(result->port.offset_y, -120);
  EXPECT_FLOAT_EQ(result->x, 0.5f);
  EXPECT_FLOAT_EQ(result->y, 0.5f);
}

TEST(TouchMapping, RemovesLetterboxBeforeNormalizingCoordinates) {
  auto port = make_port(100, 200, 1920, 1200);
  port.client_offsetY = 60.0f;

  auto result = input::map_client_touch_to_monitor(port, 0.5f, 0.05f);

  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->port.width, 1920);
  EXPECT_EQ(result->port.height, 1080);
  EXPECT_FLOAT_EQ(result->x, 0.5f);
  EXPECT_FLOAT_EQ(result->y, 0.0f);
}

TEST(TouchMapping, AppliesLogicalDimensionsForDpiScaledDisplays) {
  auto port = make_port(0, 0, 3840, 2160);
  port.scalar_tpcoords = 2.0f;

  auto result = input::map_client_touch_to_monitor(port, 0.5f, 0.5f);

  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->port.width, 1920);
  EXPECT_EQ(result->port.height, 1080);
  EXPECT_FLOAT_EQ(result->x, 0.5f);
  EXPECT_FLOAT_EQ(result->y, 0.5f);
}

TEST(TouchMapping, RejectsInvalidDimensions) {
  auto port = make_port();
  port.width = 0;

  EXPECT_FALSE(input::map_client_touch_to_monitor(port, 0.5f, 0.5f).has_value());
}

TEST(TouchMapping, RejectsLogicalDimensionsBeyondFloatIntBoundary) {
  auto port = make_port();
  port.scalar_inv = std::nextafter(
    static_cast<float>(std::numeric_limits<int>::max()) / port.width,
    std::numeric_limits<float>::infinity()
  );

  EXPECT_FALSE(input::map_client_touch_to_monitor(port, 0.5f, 0.5f).has_value());
}

TEST(TouchMapping, NormalizesRotationAndPreservesUnknown) {
  EXPECT_EQ(input::normalize_touch_rotation(720), 0);
  EXPECT_EQ(input::normalize_touch_rotation(361), 1);
  EXPECT_EQ(input::normalize_touch_rotation(LI_ROT_UNKNOWN), LI_ROT_UNKNOWN);
}

TEST(TouchMapping, RejectsNonFiniteTouchValues) {
  const auto finite = 0.5f;
  const auto not_finite = std::numeric_limits<float>::quiet_NaN();
  const auto infinity = std::numeric_limits<float>::infinity();

  EXPECT_TRUE(input::touch_input_values_are_finite(finite, finite, finite, finite, finite));
  EXPECT_FALSE(input::touch_input_values_are_finite(not_finite, finite, finite, finite, finite));
  EXPECT_FALSE(input::touch_input_values_are_finite(finite, finite, infinity, finite, finite));
  EXPECT_FALSE(input::touch_input_values_are_finite(finite, finite, finite, not_finite, finite));
  EXPECT_FALSE(input::touch_input_values_are_finite(finite, finite, finite, finite, infinity));
  EXPECT_FALSE(input::map_client_touch_to_monitor(make_port(), not_finite, finite).has_value());
}

TEST(NativeTouchPolicy, DoesNotDependOnMousePolicy) {
  const auto old_mouse = config::input.mouse;
  const auto old_native_touch = config::input.native_pen_touch;

  config::input.mouse = true;
  config::input.native_pen_touch = false;
  EXPECT_FALSE(input::native_touch_enabled());

  config::input.mouse = false;
  config::input.native_pen_touch = true;
  EXPECT_TRUE(input::native_touch_enabled());

  config::input.mouse = old_mouse;
  config::input.native_pen_touch = old_native_touch;
}

TEST(TouchSlots, ReusesReleasedPointerIdSlot) {
  platf::touch_slot_tracker_t slots;

  const auto first = slots.acquire(42);
  ASSERT_TRUE(first.has_value());
  slots.release(*first);
  EXPECT_FALSE(slots.is_active(*first));
  EXPECT_EQ(slots.pointer_id(*first), 0U);

  const auto reused = slots.acquire(42);
  ASSERT_TRUE(reused.has_value());
  EXPECT_EQ(*reused, *first);
  EXPECT_EQ(slots.pointer_id(*reused), 42U);
}

TEST(TouchSlots, ClearRemovesEveryActiveSlot) {
  platf::touch_slot_tracker_t slots;
  ASSERT_TRUE(slots.acquire(1).has_value());
  ASSERT_TRUE(slots.acquire(2).has_value());

  slots.clear();

  EXPECT_FALSE(slots.is_active(0));
  EXPECT_FALSE(slots.is_active(1));
  EXPECT_EQ(slots.pointer_id(0), 0U);
  EXPECT_EQ(slots.pointer_id(1), 0U);
  EXPECT_EQ(slots.acquire(3), std::optional<std::size_t> {0});
}

TEST(TouchSlots, CompactionSwapPreservesPointerIdsBeforeReuse) {
  platf::touch_slot_tracker_t slots;
  ASSERT_EQ(slots.acquire(11), std::optional<std::size_t> {0});
  ASSERT_EQ(slots.acquire(22), std::optional<std::size_t> {1});

  slots.swap_slots(0, 1);
  EXPECT_EQ(slots.pointer_id(0), 22U);
  EXPECT_EQ(slots.pointer_id(1), 11U);

  slots.release(0);
  EXPECT_EQ(slots.acquire(33), std::optional<std::size_t> {0});
}

TEST(TouchRepeatScheduler, CancellationInvalidatesPoppedCallbackBeforeItCanReschedule) {
  platf::touch_repeat_scheduler_t scheduler;
  const auto generation = scheduler.queue([](auto) {
    return nullptr;
  });
  ASSERT_TRUE(generation.has_value());
  ASSERT_TRUE(scheduler.begin_callback(*generation));
  int injections = 1;
  int reschedules = scheduler.callback_may_reschedule(*generation) ? 1 : 0;

  std::atomic_bool cancel_finished = false;
  std::thread cancellation([&]() {
    scheduler.begin_cancel(false);
    scheduler.finish_cancel();
    cancel_finished = true;
  });

  while (!scheduler.is_cancelling()) {
    std::this_thread::yield();
  }
  EXPECT_FALSE(scheduler.callback_may_reschedule(*generation));
  EXPECT_FALSE(cancel_finished.load());

  scheduler.finish_callback(*generation);
  cancellation.join();

  EXPECT_TRUE(cancel_finished.load());
  if (scheduler.begin_callback(*generation)) {
    ++injections;
    if (scheduler.callback_may_reschedule(*generation)) {
      ++reschedules;
    }
    scheduler.finish_callback(*generation);
  }
  EXPECT_EQ(injections, 1);
  EXPECT_EQ(reschedules, 1);
  EXPECT_FALSE(scheduler.queue(*generation, [](auto) {
    return nullptr;
  }).has_value());
  EXPECT_TRUE(scheduler.queue([](auto) {
    return nullptr;
  }).has_value());
}

TEST(TouchRepeatScheduler, PreservesFiftyMillisecondKeepaliveTransitions) {
  EXPECT_EQ(platf::touch_keepalive_interval, std::chrono::milliseconds {50});

  platf::touch_repeat_scheduler_t scheduler;
  auto generation = scheduler.queue([](auto) {
    return nullptr;
  });
  ASSERT_TRUE(generation.has_value());

  for (int i = 0; i < 3; ++i) {
    ASSERT_TRUE(scheduler.begin_callback(*generation));
    EXPECT_TRUE(scheduler.callback_may_reschedule(*generation));
    scheduler.finish_callback(*generation);
    generation = scheduler.queue([](auto) {
      return nullptr;
    });
    ASSERT_TRUE(generation.has_value());
  }
}
