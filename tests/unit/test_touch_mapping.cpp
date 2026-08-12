/**
 * @file tests/unit/test_touch_mapping.cpp
 * @brief Tests for pure native touch coordinate mapping.
 */
#include "../tests_common.h"

#include <src/input.h>

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

TEST(TouchMapping, NormalizesRotationAndPreservesUnknown) {
  EXPECT_EQ(input::normalize_touch_rotation(720), 0);
  EXPECT_EQ(input::normalize_touch_rotation(361), 1);
  EXPECT_EQ(input::normalize_touch_rotation(LI_ROT_UNKNOWN), LI_ROT_UNKNOWN);
}
