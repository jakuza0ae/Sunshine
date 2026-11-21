/**
 * @file tests/unit/test_kwin_output_policy.cpp
 * @brief Tests for exact KWin capture selection.
 */
#include <array>
#include <functional>
#include <string_view>

#include <gtest/gtest.h>

#include "src/platform/linux/kwin_output_policy.h"

using namespace std::literals;

/**
 * @test A configured virtual output takes precedence over the built-in panel.
 */
TEST(KWinOutputPolicy, selects_named_output) {
  const std::array outputs {"eDP-1"sv, "Virtual-StreamDesk"sv};
  EXPECT_EQ(kwin::select_output(outputs, "Virtual-StreamDesk", true, std::identity {}), outputs.begin() + 1);
}

/**
 * @test Strict mode refuses missing and unspecified outputs even with another display available.
 */
TEST(KWinOutputPolicy, refuses_missing_output_without_fallback) {
  const std::array outputs {"eDP-1"sv};
  EXPECT_EQ(kwin::select_output(outputs, "Virtual-StreamDesk", true, std::identity {}), outputs.end());
  EXPECT_EQ(kwin::select_output(outputs, "", true, std::identity {}), outputs.end());
}

/**
 * @test Ordinary capture still selects the first output by default and handles an empty registry.
 */
TEST(KWinOutputPolicy, preserves_default_capture) {
  const std::array outputs {"eDP-1"sv};
  const std::array<std::string_view, 0> empty {};
  EXPECT_EQ(kwin::select_output(outputs, "", false, std::identity {}), outputs.begin());
  EXPECT_EQ(kwin::select_output(outputs, "missing", false, std::identity {}), outputs.begin());
  EXPECT_EQ(kwin::select_output(empty, "", false, std::identity {}), empty.end());
}
