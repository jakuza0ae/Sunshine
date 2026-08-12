/**
 * @file src/input.h
 * @brief Declarations for gamepad, keyboard, and mouse input handling.
 */
#pragma once

// standard includes
#include <functional>
#include <optional>

// local includes
#include "platform/common.h"
#include "thread_safe.h"

namespace input {
  struct input_t;

  /**
   * @brief Write a debug log representation of the input packet.
   *
   * @param input Raw input packet to format for logging.
   */
  void print(void *input);
  /**
   * @brief Reset stream input state after a client disconnect or shutdown.
   *
   * @param input Shared stream input state to reset.
   */
  void reset(std::shared_ptr<input_t> &input);

  /**
   * @brief Queue a raw input message for platform passthrough.
   */
  void passthrough(std::shared_ptr<input_t> &input, std::vector<std::uint8_t> &&input_data);

  /**
   * @brief Initialize global input resources and platform backends.
   *
   * @return Cleanup handle for initialized input resources, or null if none are required.
   */
  [[nodiscard]] std::unique_ptr<platf::deinit_t> init();

  /**
   * @brief Probe whether the platform can create virtual gamepads.
   *
   * @return True when at least one configured gamepad backend is available.
   */
  bool probe_gamepads();

  /**
   * @brief Allocate and initialize platform input state for a stream.
   *
   * @param mail Mailbox used to exchange messages with worker threads.
   * @return Shared input state bound to the stream mailbox.
   */
  std::shared_ptr<input_t> alloc(safe::mail_t mail);

  /**
   * @brief Touchscreen coordinate bounds used to scale absolute input.
   */
  struct touch_port_t: public platf::touch_port_t {
    int env_width;  ///< Width of the full capture environment in physical pixels.
    int env_height;  ///< Height of the full capture environment in physical pixels.

    // Offset x and y coordinates of the client
    float client_offsetX;  ///< Horizontal client viewport offset used when scaling touch input.
    float client_offsetY;  ///< Vertical client viewport offset used when scaling touch input.

    float scalar_inv;  ///< Inverse scale factor from client coordinates to display coordinates.
    float scalar_tpcoords;  ///< Scale factor from client coordinates to touch-port coordinates.

    int env_logical_width;  ///< Width of the full capture environment after display scaling.
    int env_logical_height;  ///< Height of the full capture environment after display scaling.

    /**
     * @brief Check whether the touch-port bounds are initialized.
     */
    explicit operator bool() const {
      return width != 0 && height != 0 && env_width != 0 && env_height != 0;
    }
  };

  /**
   * @brief Result of mapping a client touch point into a monitor-local viewport.
   */
  struct touch_mapping_result {
    platf::touch_port_t port;  ///< Monitor viewport in logical desktop coordinates.
    float x;  ///< Monitor-local normalized horizontal coordinate.
    float y;  ///< Monitor-local normalized vertical coordinate.
  };

  /**
   * @brief Map a client-normalized touch point to a monitor-local logical viewport.
   *
   * @param port Touch port metadata for the current stream.
   * @param normalized_x Client-normalized horizontal coordinate.
   * @param normalized_y Client-normalized vertical coordinate.
   * @return Monitor-local mapping, or std::nullopt when the port dimensions are invalid.
   */
  std::optional<touch_mapping_result> map_client_touch_to_monitor(
    const touch_port_t &port,
    float normalized_x,
    float normalized_y
  );

  /**
   * @brief Normalize a touch rotation to the protocol's valid degree range.
   *
   * @param rotation Rotation in degrees or LI_ROT_UNKNOWN.
   * @return Rotation in the range [0, 359], or LI_ROT_UNKNOWN.
   */
  uint16_t normalize_touch_rotation(uint16_t rotation);

  /**
   * @brief Check whether decoded native touch values are safe to convert and inject.
   *
   * @param x Normalized horizontal coordinate.
   * @param y Normalized vertical coordinate.
   * @param pressure_or_distance Pressure or hover distance.
   * @param contact_area_major Major contact axis.
   * @param contact_area_minor Minor contact axis.
   * @return True when every value is finite.
   */
  bool touch_input_values_are_finite(
    float x,
    float y,
    float pressure_or_distance,
    float contact_area_major,
    float contact_area_minor
  );

  /**
   * @brief Return whether native touch injection is enabled by policy.
   * @return True when native pen/touch input is enabled.
   */
  bool native_touch_enabled();

  /**
   * @brief Scale the ellipse axes according to the provided size.
   * @param val The major and minor axis pair.
   * @param rotation The rotation value from the touch/pen event.
   * @param scalar The scalar cartesian coordinate pair.
   * @return The major and minor axis pair.
   */
  std::pair<float, float> scale_client_contact_area(const std::pair<float, float> &val, uint16_t rotation, const std::pair<float, float> &scalar);
}  // namespace input
