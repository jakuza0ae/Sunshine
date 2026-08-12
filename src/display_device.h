/**
 * @file src/display_device.h
 * @brief Declarations for display device handling.
 */
#pragma once

// standard includes
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// lib includes
#include <display_device/display_power_interface.h>
#include <display_device/types.h>

// forward declarations
namespace platf {
  class deinit_t;
}

namespace config {
  struct video_t;
}

namespace rtsp_stream {
  struct launch_session_t;
}

namespace display_device {
  /**
   * @brief Lifecycle states reported for an enabled second-monitor profile.
   */
  enum class second_monitor_state_e {
    waiting_for_vdd,
    display_configuring,
    capturing,
    capture_reinitializing,
    display_restore_pending,
    display_restore_failed
  };

  /**
   * @brief Convert a second-monitor lifecycle state to its API spelling.
   */
  [[nodiscard]] std::string_view second_monitor_state_name(second_monitor_state_e state);

  /**
   * @brief Return the libdisplaydevice-provided virtual-device classification.
   * @param device Enumerated display device.
   * @return True for an indirect virtual output, false for a known physical output, or empty when unknown.
   */
  [[nodiscard]] std::optional<bool> classify_virtual_device(const EnumeratedDevice &device);

  /**
   * @brief Secret-free display choice shown by the second-monitor selector.
   */
  struct second_monitor_device_status_t {
    std::string device_id {};  ///< Stable display device ID.
    std::string display_name {};  ///< Current platform output name.
    std::string friendly_name {};  ///< Human-readable display name.
    bool active {};  ///< Whether the display has a current output mapping.
    std::optional<bool> virtual_device {};  ///< Whether the device is identified as virtual, or empty when unknown.
    unsigned int current_width {};  ///< Current width when available.
    unsigned int current_height {};  ///< Current height when available.
  };

  /**
   * @brief Secret-free second-monitor lifecycle information.
   */
  struct second_monitor_status_t {
    std::string state {"waiting_for_vdd"};  ///< Current second-monitor lifecycle state.
    std::string selected_device_id {};  ///< Stable VDD device ID selected by the profile.
    std::string mapped_output {};  ///< Current platform output name for the selected VDD.
    std::string friendly_name {};  ///< Human-readable name of the selected device.
    bool active {};  ///< Whether the selected device is currently enumerated.
    std::optional<bool> virtual_device {};  ///< Whether the selected device is virtual, or empty when unknown.
    unsigned int current_width {};  ///< Current enumerated device width in pixels.
    unsigned int current_height {};  ///< Current enumerated device height in pixels.
    std::string capture_backend {};  ///< Capture backend selected for the active profile.
    std::string encoder {};  ///< Encoder selected by the most recent probe.
    unsigned int active_width {};  ///< Active profile width in pixels.
    unsigned int active_height {};  ///< Active profile height in pixels.
    unsigned int active_fps {};  ///< Active profile frame rate.
    std::string last_reinitialization_reason {};  ///< Last bounded reason for capture reinitialization.
    std::vector<second_monitor_device_status_t> devices {};  ///< Enumerated display choices for the selector.
  };

  /**
   * @brief Testable ownership and lifecycle state for a second-monitor stream.
   */
  class second_monitor_lifecycle_t {
  public:
    void launch_requested(bool vdd_available);
    void retry(bool vdd_available);
    void capture_started(std::string selected_device_id, std::string mapped_output, std::string capture_backend, std::string encoder, unsigned int width, unsigned int height, unsigned int fps);
    void capture_reinitializing(std::string reason);
    void capture_reinitialized();
    void session_started();
    [[nodiscard]] bool session_stopped(bool restore_enabled);
    [[nodiscard]] bool launch_failed(bool restore_enabled);
    [[nodiscard]] bool consume_restore_request();
    void restore_finished(bool success);
    [[nodiscard]] second_monitor_status_t status() const;

  private:
    second_monitor_state_e m_state {second_monitor_state_e::waiting_for_vdd};
    second_monitor_status_t m_status {};
    std::size_t m_session_owners {};
    bool m_restore_scheduled {};
    bool m_restore_request_pending {};
  };

  /**
   * @brief Return the current secret-free second-monitor status snapshot.
   */
  [[nodiscard]] second_monitor_status_t second_monitor_status();

  /**
   * @brief Record the beginning of display configuration for an enabled profile.
   */
  void mark_display_configuration(bool vdd_available);

  /**
   * @brief Record the beginning of capture for an enabled profile.
   */
  void mark_capture_started(const std::string &selected_device_id, const std::string &mapped_output, unsigned int width, unsigned int height, unsigned int fps);

  /**
   * @brief Record a capture retry reason.
   */
  void mark_capture_reinitializing(std::string reason);

  /**
   * @brief Record a successfully owned stream session.
   */
  void mark_session_started();

  /**
   * @brief Record a stopped stream and return whether final restore should run.
   */
  [[nodiscard]] bool mark_session_stopped(bool restore_enabled);

  /**
   * @brief Record a failed launch and return whether final restore should run.
   */
  [[nodiscard]] bool mark_launch_failed(bool restore_enabled);

  /**
   * @brief Record successful capture reinitialization.
   */
  void mark_capture_reinitialized();

  /**
   * @brief Resolved display selection used by display configuration and capture.
   */
  struct DisplaySelection {
    std::string selected_device_id {};  ///< Stable device ID selected by configuration.
    std::string mapped_display_name {};  ///< Current platform display name for the selected device.
    bool unavailable {};  ///< True when an explicitly selected device is not currently available.
  };

  /**
   * @brief Resolve a configured display device against an enumeration snapshot.
   * @param video_config User's video related configuration.
   * @param devices Enumerated display devices.
   * @returns A stable selection result. Empty selections retain default platform behavior.
   */
  [[nodiscard]] DisplaySelection resolve_display_selection(const config::video_t &video_config, const EnumeratedDeviceList &devices);

  /**
   * @brief Resolve the configured display device against the current platform state.
   * @param video_config User's video related configuration.
   * @returns A stable selection result. Empty selections retain default platform behavior.
   */
  [[nodiscard]] DisplaySelection get_display_selection(const config::video_t &video_config);

  /**
   * @brief Initialize the implementation and perform the initial state recovery (if needed).
   * @param persistence_filepath File location for reading/saving persistent state.
   * @param video_config User's video related configuration.
   * @returns A deinit_t instance that performs cleanup when destroyed.
   *
   * @examples
   * const config::video_t &video_config { config::video };
   * const auto init_guard { init("/my/persitence/file.state", video_config) };
   * @examples_end
   */
  [[nodiscard]] std::unique_ptr<platf::deinit_t> init(const std::filesystem::path &persistence_filepath, const config::video_t &video_config);

  /**
   * @brief Map the output name to a specific display.
   * @param output_name The user-configurable output name.
   * @returns Mapped display name or empty string if the output name could not be mapped.
   *
   * @examples
   * const auto mapped_name_config { map_output_name(config::video.output_name) };
   * const auto mapped_name_custom { map_output_name("{some-device-id}") };
   * @examples_end
   */
  [[nodiscard]] std::string map_output_name(const std::string &output_name);

  /**
   * @brief Ask the platform to wake displays before detection or capture.
   * @param display_name Platform capture selector.
   * @param timeout Maximum time to wait for platform-specific wake detection.
   * @returns True if the display was already available or the wake request succeeded.
   */
  [[nodiscard]] bool wake_display(const std::string &display_name, std::chrono::milliseconds timeout);

  /**
   * @brief Keep displays awake until the returned guard is destroyed.
   * @param reason Short human-readable reason for the power assertion.
   * @returns A guard owning the platform assertion, or nullptr if unsupported or unavailable.
   */
  [[nodiscard]] std::unique_ptr<DisplayPowerGuardInterface> keep_display_awake(const std::string &reason);

  /**
   * @brief Configure the display device based on the user configuration and the session information.
   * @note This is a convenience method for calling similar method of a different signature.
   *
   * @param video_config User's video related configuration.
   * @param session Session information.
   *
   * @examples
   * const std::shared_ptr<rtsp_stream::launch_session_t> launch_session;
   * const config::video_t &video_config { config::video };
   *
   * configure_display(video_config, *launch_session);
   * @examples_end
   */
  void configure_display(const config::video_t &video_config, const rtsp_stream::launch_session_t &session);

  /**
   * @brief Configure the display device using the provided configuration.
   *
   * In some cases configuring display can fail due to transient issues and
   * we will keep trying every 5 seconds, even if the stream has already started as there was
   * no possibility to apply settings before the stream start.
   *
   * Therefore, there is no return value as we still want to continue with the stream, so that
   * the users can do something about it once they are connected. Otherwise, we might
   * prevent users from logging in at all if we keep failing to apply configuration.
   *
   * @param config Configuration for the display.
   *
   * @examples
   * const SingleDisplayConfiguration valid_config { };
   * configure_display(valid_config);
   * @examples_end
   */
  void configure_display(const SingleDisplayConfiguration &config);

  /**
   * @brief Revert the display configuration and restore the previous state.
   *
   * In case the state could not be restored, by default it will be retried again in 5 seconds
   * (repeating indefinitely until success or until persistence is reset).
   *
   * @examples
   * revert_configuration();
   * @examples_end
   */
  void revert_configuration();

  /**
   * @brief Reset persisted display state and the captured initial state.
   *
   * This is normally used to get out of the "broken" state where the algorithm wants
   * to restore the initial display state, but it is no longer possible.
   *
   * This could happen if the display is no longer available or the hardware was changed
   * and the device ids no longer match.
   *
   * The user then accepts that Sunshine is not able to restore the state and "agrees" to
   * do it manually.
   *
   * @return True if persistence was reset, false otherwise.
   * @note Whether the function succeeds or fails, any of the scheduled "retries" from
   *       other methods will be stopped to not interfere with the user actions.
   *
   * @examples
   * const auto result = reset_persistence();
   * @examples_end
   */
  [[nodiscard]] bool reset_persistence();

  /**
   * @brief Enumerate the available devices.
   * @return A list of devices.
   *
   * @examples
   * const auto devices = enumerate_devices();
   * @examples_end
   */
  [[nodiscard]] EnumeratedDeviceList enumerate_devices();

  /**
   * @brief A tag structure indicating that configuration parsing has failed.
   */
  struct failed_to_parse_tag_t {};

  /**
   * @brief A tag structure indicating that configuration is disabled.
   */
  struct configuration_disabled_tag_t {};

  /**
   * @brief Parse the user configuration and the session information.
   * @param video_config User's video related configuration.
   * @param session Session information.
   * @return Parsed single display configuration or
   *         a tag indicating that the parsing has failed or
   *         a tag indicating that the user does not want to perform any configuration.
   *
   * @examples
   * const std::shared_ptr<rtsp_stream::launch_session_t> launch_session;
   * const config::video_t &video_config { config::video };
   *
   * const auto config { parse_configuration(video_config, *launch_session) };
   * if (const auto *parsed_config { std::get_if<SingleDisplayConfiguration>(&result) }; parsed_config) {
   *    configure_display(*config);
   * }
   * @examples_end
   */
  [[nodiscard]] std::variant<failed_to_parse_tag_t, configuration_disabled_tag_t, SingleDisplayConfiguration> parse_configuration(const config::video_t &video_config, const rtsp_stream::launch_session_t &session);
}  // namespace display_device
