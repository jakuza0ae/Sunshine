/**
 * @file tests/unit/test_display_device.cpp
 * @brief Test src/display_device.*.
 */
#include "../tests_common.h"

#include <format>
#include <initializer_list>
#include <src/config.h>
#include <src/confighttp.h>
#include <src/display_device.h>
#include <src/rtsp.h>

#include <unordered_map>

namespace {
  using namespace std::literals;

  using config_option_e = config::video_t::dd_t::config_option_e;
  using device_prep_t = display_device::SingleDisplayConfiguration::DevicePreparation;

  using hdr_option_e = config::video_t::dd_t::hdr_option_e;
  using hdr_state_e = display_device::HdrState;

  using resolution_option_e = config::video_t::dd_t::resolution_option_e;
  using resolution_t = display_device::Resolution;

  using refresh_rate_option_e = config::video_t::dd_t::refresh_rate_option_e;
  using rational_t = display_device::Rational;

  struct failed_to_parse_resolution_tag_t {};

  struct failed_to_parse_refresh_rate_tag_t {};

  struct no_refresh_rate_tag_t {};

  struct no_resolution_tag_t {};

  struct client_resolution_t {
    int width;
    int height;
  };

  using client_fps_t = int;
  using sops_enabled_t = bool;
  using client_wants_hdr_t = bool;

  constexpr unsigned int max_uint {std::numeric_limits<unsigned int>::max()};
  const std::string max_uint_string {std::to_string(std::numeric_limits<unsigned int>::max())};

  template<class T>
  struct DisplayDeviceConfigTest: BaseTest, testing::WithParamInterface<T> {};
}  // namespace

using ParseDeviceId = DisplayDeviceConfigTest<std::pair<std::string, std::string>>;
INSTANTIATE_TEST_SUITE_P(
  DisplayDeviceConfigTest,
  ParseDeviceId,
  testing::Values(
    std::make_pair(""s, ""s),
    std::make_pair("SomeId"s, "SomeId"s),
    std::make_pair("{daeac860-f4db-5208-b1f5-cf59444fb768}"s, "{daeac860-f4db-5208-b1f5-cf59444fb768}"s)
  )
);

TEST_P(ParseDeviceId, IntegrationTest) {
  const auto &[input_value, expected_value] = GetParam();

  config::video_t video_config {};
  video_config.dd.configuration_option = config_option_e::verify_only;
  video_config.output_name = input_value;

  const auto result {display_device::parse_configuration(video_config, {})};
  EXPECT_EQ(std::get<display_device::SingleDisplayConfiguration>(result).m_device_id, expected_value);
}

using ParseConfigOption = DisplayDeviceConfigTest<std::pair<config_option_e, std::optional<device_prep_t>>>;
INSTANTIATE_TEST_SUITE_P(
  DisplayDeviceConfigTest,
  ParseConfigOption,
  testing::Values(
    std::make_pair(config_option_e::disabled, std::nullopt),
    std::make_pair(config_option_e::verify_only, device_prep_t::VerifyOnly),
    std::make_pair(config_option_e::ensure_active, device_prep_t::EnsureActive),
    std::make_pair(config_option_e::ensure_primary, device_prep_t::EnsurePrimary),
    std::make_pair(config_option_e::ensure_only_display, device_prep_t::EnsureOnlyDisplay),
    std::make_pair(config_option_e::vdd_only, device_prep_t::VerifyOnly)
  )
);

TEST_P(ParseConfigOption, IntegrationTest) {
  const auto &[input_value, expected_value] = GetParam();

  config::video_t video_config {};
  video_config.dd.configuration_option = input_value;

  const auto result {display_device::parse_configuration(video_config, {})};
  if (const auto *parsed_config {std::get_if<display_device::SingleDisplayConfiguration>(&result)}; parsed_config) {
    ASSERT_EQ(parsed_config->m_device_prep, expected_value);
  } else {
    ASSERT_EQ(std::get_if<display_device::configuration_disabled_tag_t>(&result) != nullptr, !expected_value);
  }
}

TEST(DisplayDeviceSelection, SelectedVddUsesCurrentDisplayName) {
  config::video_t video_config {};
  video_config.dd.configuration_option = config_option_e::vdd_only;
  video_config.output_name = "vdd-guid";

  const display_device::EnumeratedDeviceList devices {
    {.m_device_id = "vdd-guid", .m_display_name = "\\\\.\\DISPLAY2", .m_friendly_name = "Virtual Display", .m_virtual_device = true}
  };

  const auto result {display_device::resolve_display_selection(video_config, devices)};

  EXPECT_EQ(result.selected_device_id, "vdd-guid");
  EXPECT_EQ(result.mapped_display_name, "\\\\.\\DISPLAY2");
  EXPECT_FALSE(result.unavailable);
}

TEST(DisplayDeviceSelection, MissingVddIsUnavailable) {
  config::video_t video_config {};
  video_config.dd.configuration_option = config_option_e::vdd_only;
  video_config.output_name = "missing-vdd-guid";

  const auto result {display_device::resolve_display_selection(video_config, {})};

  EXPECT_EQ(result.selected_device_id, "missing-vdd-guid");
  EXPECT_TRUE(result.mapped_display_name.empty());
  EXPECT_TRUE(result.unavailable);
}

TEST(DisplayDeviceSelection, DeviceOrderDoesNotChangeSelectedVdd) {
  config::video_t video_config {};
  video_config.dd.configuration_option = config_option_e::vdd_only;
  video_config.output_name = "vdd-guid";

  const display_device::EnumeratedDeviceList first_order {
    {.m_device_id = "vdd-guid", .m_display_name = "\\\\.\\DISPLAY2", .m_friendly_name = "Virtual Display", .m_virtual_device = true},
    {.m_device_id = "physical-guid", .m_display_name = "\\\\.\\DISPLAY1"}
  };
  const display_device::EnumeratedDeviceList second_order {
    {.m_device_id = "physical-guid", .m_display_name = "\\\\.\\DISPLAY1"},
    {.m_device_id = "vdd-guid", .m_display_name = "\\\\.\\DISPLAY2", .m_friendly_name = "Virtual Display", .m_virtual_device = true}
  };

  const auto first_result {display_device::resolve_display_selection(video_config, first_order)};
  const auto second_result {display_device::resolve_display_selection(video_config, second_order)};

  EXPECT_EQ(first_result.selected_device_id, "vdd-guid");
  EXPECT_EQ(second_result.selected_device_id, "vdd-guid");
  EXPECT_EQ(first_result.mapped_display_name, second_result.mapped_display_name);
  EXPECT_FALSE(first_result.unavailable);
  EXPECT_FALSE(second_result.unavailable);
}

TEST(DisplayDeviceSelection, EmptySelectionRetainsDefaultBehavior) {
  config::video_t video_config {};
  video_config.dd.configuration_option = config_option_e::vdd_only;

  const auto result {display_device::resolve_display_selection(video_config, {})};

  EXPECT_TRUE(result.selected_device_id.empty());
  EXPECT_TRUE(result.mapped_display_name.empty());
  EXPECT_FALSE(result.unavailable);
}

TEST(DisplayDeviceSelection, PhysicalAndUnknownVddAreUnavailable) {
  config::video_t video_config {};
  video_config.dd.configuration_option = config_option_e::vdd_only;

  for (const auto &device_id : {"physical-guid"sv, "unknown-guid"sv}) {
    video_config.output_name = device_id;
    video_config.second_monitor.device_id = std::string {device_id};
    video_config.second_monitor.display_policy = config_option_e::vdd_only;
    const display_device::EnumeratedDevice device {
      .m_device_id = std::string {device_id},
      .m_display_name = "\\\\.\\DISPLAY2",
      .m_friendly_name = device_id == "physical-guid"sv ? "ROG PG279Q" : "Display",
      .m_edid = device_id == "physical-guid"sv ? std::optional<display_device::EdidData> {display_device::EdidData {}} : std::nullopt,
      .m_virtual_device = device_id == "physical-guid"sv ? std::optional<bool> {false} : std::nullopt
    };

    const auto result {display_device::resolve_display_selection(video_config, {device})};

    EXPECT_TRUE(result.unavailable) << device_id;
    EXPECT_TRUE(result.mapped_display_name.empty()) << device_id;
  }
}

TEST(DisplayDeviceSelection, GenericVddOnlyRetainsLegacyDeviceMapping) {
  config::video_t video_config {};
  video_config.dd.configuration_option = config_option_e::vdd_only;
  video_config.output_name = "physical-guid";

  const display_device::EnumeratedDeviceList devices {
    {.m_device_id = "physical-guid", .m_display_name = "\\\\.\\DISPLAY1", .m_friendly_name = "Physical Display", .m_virtual_device = false}
  };

  const auto result {display_device::resolve_display_selection(video_config, devices)};

  EXPECT_FALSE(result.unavailable);
  EXPECT_EQ(result.mapped_display_name, "\\\\.\\DISPLAY1");
}

TEST(SecondMonitorProfile, EmptyConfigIsOptedOutWithSafeDefaults) {
  const auto profile {config::parse_second_monitor_profile({})};

  ASSERT_TRUE(profile);
  EXPECT_FALSE(profile->enabled());
  EXPECT_TRUE(profile->device_id.empty());
  EXPECT_EQ(profile->resolution, "1280x720");
  EXPECT_EQ(profile->fps, 30);
  EXPECT_FALSE(profile->hdr);
  EXPECT_EQ(profile->capture_backend, "ddx");
  EXPECT_FALSE(profile->stream_audio);
  EXPECT_EQ(profile->display_policy, config_option_e::disabled);
  EXPECT_TRUE(profile->restore_on_disconnect);
}

TEST(SecondMonitorProfile, RequiresDeviceIdWhenEnabled) {
  const std::unordered_map<std::string, std::string> values {
    {"second_monitor_resolution", "1280x720"},
    {"second_monitor_fps", "30"},
    {"second_monitor_display_policy", "vdd_only"}
  };

  EXPECT_FALSE(config::parse_second_monitor_profile(values));
}

TEST(SecondMonitorProfile, DisabledPolicyWithoutDeviceIdIsValidOptOut) {
  const std::unordered_map<std::string, std::string> values {
    {"second_monitor_display_policy", "disabled"}
  };

  const auto profile {config::parse_second_monitor_profile(values)};

  ASSERT_TRUE(profile);
  EXPECT_FALSE(profile->enabled());
  EXPECT_TRUE(profile->device_id.empty());
  EXPECT_EQ(profile->display_policy, config_option_e::disabled);
}

TEST(SecondMonitorProfile, ParsesValidVddProfile) {
  const std::unordered_map<std::string, std::string> values {
    {"second_monitor_device_id", "{vdd-guid}"},
    {"second_monitor_resolution", "1920x1080"},
    {"second_monitor_fps", "60"},
    {"second_monitor_hdr", "false"},
    {"second_monitor_capture_backend", "wgc"},
    {"second_monitor_stream_audio", "true"},
    {"second_monitor_display_policy", "vdd_only"},
    {"second_monitor_restore_on_disconnect", "true"}
  };

  const auto profile {config::parse_second_monitor_profile(values)};

  ASSERT_TRUE(profile);
  EXPECT_TRUE(profile->enabled());
  EXPECT_EQ(profile->device_id, "{vdd-guid}");
  EXPECT_EQ(profile->resolution, "1920x1080");
  EXPECT_EQ(profile->fps, 60);
  EXPECT_FALSE(profile->hdr);
  EXPECT_EQ(profile->capture_backend, "wgc");
  EXPECT_TRUE(profile->stream_audio);
  EXPECT_EQ(profile->display_policy, config_option_e::vdd_only);
  EXPECT_TRUE(profile->restore_on_disconnect);
}

TEST(SecondMonitorProfile, RejectsUnsafePolicyAndInvalidRanges) {
  for (const auto &[key, value] : std::initializer_list<std::pair<std::string, std::string>> {
         {"second_monitor_display_policy", "ensure_primary"},
         {"second_monitor_resolution", "0x720"},
         {"second_monitor_resolution", "7681x4320"},
         {"second_monitor_fps", "0"},
         {"second_monitor_fps", "241"}
       }) {
    const std::unordered_map<std::string, std::string> values {
      {"second_monitor_device_id", "{vdd-guid}"},
      {key, value}
    };

    EXPECT_FALSE(config::parse_second_monitor_profile(values)) << key << '=' << value;
  }
}

TEST(SecondMonitorProfile, RejectsUnknownProfileFields) {
  const std::unordered_map<std::string, std::string> values {
    {"second_monitor_device_id", "{vdd-guid}"},
    {"second_monitor_arbitrary_command", "whoami"}
  };

  EXPECT_FALSE(config::parse_second_monitor_profile(values));
}

TEST(SecondMonitorProfile, UsesFixedDisplayModeWithoutClientSops) {
  config::video_t video_config {};
  video_config.second_monitor.device_id = "{vdd-guid}";
  video_config.second_monitor.resolution = "1280x720";
  video_config.second_monitor.fps = 30;
  video_config.second_monitor.hdr = true;
  video_config.second_monitor.display_policy = config_option_e::vdd_only;
  video_config.dd.configuration_option = config_option_e::vdd_only;
  video_config.dd.resolution_option = resolution_option_e::manual;
  video_config.dd.manual_resolution = video_config.second_monitor.resolution;
  video_config.dd.refresh_rate_option = refresh_rate_option_e::manual;
  video_config.dd.manual_refresh_rate = "30";
  video_config.dd.hdr_option = hdr_option_e::automatic;

  rtsp_stream::launch_session_t session {};
  session.enable_sops = false;
  session.enable_hdr = false;

  const auto result {display_device::parse_configuration(video_config, session)};
  const auto &parsed {std::get<display_device::SingleDisplayConfiguration>(result)};

  EXPECT_EQ(parsed.m_resolution, std::optional<resolution_t> {{1280, 720}});
  EXPECT_EQ(parsed.m_refresh_rate, std::optional<display_device::FloatingPoint> {rational_t {30, 1}});
  EXPECT_EQ(parsed.m_hdr_state, std::optional<hdr_state_e> {hdr_state_e::Enabled});
}

TEST(SecondMonitorProfile, AppliesVddSettingsOnlyToMarkedSessions) {
  config::video_t video_config {};
  video_config.capture = "regular-capture";
  video_config.output_name = "regular-output";
  video_config.dd.configuration_option = config_option_e::disabled;
  video_config.second_monitor.device_id = "{vdd-guid}";
  video_config.second_monitor.capture_backend = "ddx";
  video_config.second_monitor.resolution = "1280x720";
  video_config.second_monitor.fps = 30;
  video_config.second_monitor.display_policy = config_option_e::vdd_only;

  const auto regular_session {config::video_for_session(video_config, false)};
  const auto second_monitor_session {config::video_for_session(video_config, true)};

  ASSERT_TRUE(regular_session);
  ASSERT_TRUE(second_monitor_session);
  EXPECT_EQ(regular_session->output_name, "regular-output");
  EXPECT_EQ(regular_session->dd.configuration_option, config_option_e::disabled);
  EXPECT_EQ(second_monitor_session->output_name, "{vdd-guid}");
  EXPECT_EQ(second_monitor_session->dd.configuration_option, config_option_e::vdd_only);
  EXPECT_EQ(second_monitor_session->capture, "ddx");
}

TEST(SecondMonitorProfile, InvalidProfileFailsClosedOnlyForMarkedSessions) {
  config::video_t video_config {};
  video_config.second_monitor_profile_invalid = true;
  video_config.second_monitor.device_id = "{bad-vdd-guid}";
  video_config.second_monitor.display_policy = config_option_e::vdd_only;

  ASSERT_TRUE(config::video_for_session(video_config, false));
  EXPECT_FALSE(config::video_for_session(video_config, true));
}

TEST(SecondMonitorLifecycle, VddAppearingDuringRetryMovesToConfiguration) {
  display_device::second_monitor_lifecycle_t lifecycle;

  lifecycle.launch_requested(false);
  EXPECT_EQ(lifecycle.status().state, "waiting_for_vdd");

  lifecycle.retry(false);
  EXPECT_EQ(lifecycle.status().state, "waiting_for_vdd");

  lifecycle.retry(true);
  EXPECT_EQ(lifecycle.status().state, "display_configuring");
}

TEST(SecondMonitorLifecycle, InitialVddLossStaysRetryableInsteadOfReportingCapture) {
  display_device::second_monitor_lifecycle_t lifecycle;

  lifecycle.launch_requested(false);

  EXPECT_FALSE(lifecycle.launch_failed(true));
  EXPECT_EQ(lifecycle.status().state, "waiting_for_vdd");
}

TEST(SecondMonitorLifecycle, FailedLaunchSchedulesOneRestore) {
  display_device::second_monitor_lifecycle_t lifecycle;

  lifecycle.launch_requested(true);
  EXPECT_TRUE(lifecycle.launch_failed(true));
  EXPECT_EQ(lifecycle.status().state, "display_restore_pending");
  EXPECT_FALSE(lifecycle.launch_failed(true));

  lifecycle.restore_finished(false);
  EXPECT_EQ(lifecycle.status().state, "display_restore_failed");
}

TEST(SecondMonitorLifecycle, FailedPendingLaunchDoesNotRestoreAnotherOwner) {
  display_device::second_monitor_lifecycle_t lifecycle;

  lifecycle.capture_started("{vdd-guid}", "\\\\.\\DISPLAY2", "ddx", "h264_amf", 1280, 720, 30);
  lifecycle.session_started("h264", 5000);

  EXPECT_FALSE(lifecycle.launch_failed(true));
  EXPECT_EQ(lifecycle.status().state, "capturing");
  EXPECT_TRUE(lifecycle.session_stopped(true));
}

TEST(SecondMonitorLifecycle, FinalOwnerRestoresAfterSharedSessionsEnd) {
  display_device::second_monitor_lifecycle_t lifecycle;

  lifecycle.capture_started("{vdd-guid}", "\\\\.\\DISPLAY2", "ddx", "h264_amf", 1280, 720, 30);
  lifecycle.session_started("h264", 5000);
  lifecycle.session_started("h264", 5000);

  EXPECT_FALSE(lifecycle.session_stopped(true));
  EXPECT_EQ(lifecycle.status().state, "capturing");
  EXPECT_TRUE(lifecycle.session_stopped(true));
  EXPECT_EQ(lifecycle.status().state, "display_restore_pending");
  EXPECT_TRUE(lifecycle.consume_restore_request());
  EXPECT_FALSE(lifecycle.consume_restore_request());
  EXPECT_FALSE(lifecycle.session_stopped(true));
}

TEST(SecondMonitorLifecycle, CaptureReinitializationKeepsReasonInDiagnostics) {
  display_device::second_monitor_lifecycle_t lifecycle;

  lifecycle.capture_reinitializing("VDD hotplug");

  EXPECT_EQ(lifecycle.status().state, "capture_reinitializing");
  EXPECT_EQ(lifecycle.status().last_reinitialization_reason, "VDD hotplug");

  lifecycle.capture_reinitialized();
  EXPECT_EQ(lifecycle.status().state, "capturing");
}

TEST(SecondMonitorDiagnostics, SerializesTheDiagnosticRecordWithoutSecretsOrCommands) {
  display_device::second_monitor_status_t status {
    .state = "capturing",
    .selected_device_id = "{vdd-guid}",
    .host_display_id = "{vdd-guid}",
    .mapped_output = "\\\\.\\DISPLAY2",
    .friendly_name = "Virtual Display",
    .active = true,
    .virtual_device = true,
    .current_width = 1280,
    .current_height = 720,
    .capture_backend = "ddx",
    .encoder = "h264_nvenc",
    .codec = "h264",
    .active_width = 1280,
    .active_height = 720,
    .active_fps = 30,
    .bitrate_kbps = 5000,
    .rtt_ms = 22,
    .dropped_frames = 3,
    .reconnect_count = 1,
    .last_error = "password=not-a-diagnostic-value",
    .last_reinitialization_reason = "whoami --pairing-key=not-a-diagnostic-value"
  };

  const auto json {confighttp::serialize_second_monitor_status(status)};
  const auto serialized {json.dump()};

  EXPECT_EQ(json.at("state").get<std::string>(), "capturing");
  EXPECT_EQ(json.at("host_display_id").get<std::string>(), "{vdd-guid}");
  EXPECT_EQ(json.at("mapped_output").get<std::string>(), "\\\\.\\DISPLAY2");
  EXPECT_EQ(json.at("capture_backend").get<std::string>(), "ddx");
  EXPECT_EQ(json.at("encoder").get<std::string>(), "h264_nvenc");
  EXPECT_EQ(json.at("codec").get<std::string>(), "h264");
  EXPECT_EQ(json.at("resolution").get<std::string>(), "1280x720");
  EXPECT_EQ(json.at("fps").get<unsigned int>(), 30);
  EXPECT_EQ(json.at("bitrate_kbps").get<unsigned int>(), 5000);
  EXPECT_EQ(json.at("rtt_ms").get<unsigned int>(), 22);
  EXPECT_EQ(json.at("dropped_frames").get<unsigned int>(), 3);
  EXPECT_EQ(json.at("reconnect_count").get<unsigned int>(), 1);
  EXPECT_EQ(json.at("last_error").get<std::string>(), "redacted");
  EXPECT_EQ(json.at("last_reinitialization_reason").get<std::string>(), "redacted");
  EXPECT_EQ(serialized.find("not-a-diagnostic-value"), std::string::npos);
  EXPECT_EQ(serialized.find("whoami"), std::string::npos);
  EXPECT_EQ(serialized.find("pairing-key"), std::string::npos);
  EXPECT_EQ(serialized.find("password"), std::string::npos);
}

TEST(SecondMonitorDiagnostics, UsesNullForUnavailableMetricsAndSanitizesUntrustedMetadata) {
  display_device::second_monitor_status_t status {
    .state = "waiting_for_vdd",
    .selected_device_id = "{stable-vdd-guid}",
    .host_display_id = "{stable-vdd-guid}",
    .mapped_output = "\\\\.\\DISPLAY2\r\nwhoami",
    .friendly_name = "whoami && type secret.txt",
    .capture_backend = "",
    .encoder = "",
    .codec = "",
    .active_width = 0,
    .active_height = 0,
    .active_fps = 0,
    .last_error = "powershell -Command whoami",
    .last_reinitialization_reason = "pairing-key=password"
  };
  status.devices.push_back({
    .device_id = "{device-guid}\nwhoami",
    .display_name = "Monitor\r\nwhoami",
    .friendly_name = std::string(300, 'a')
  });
  status.devices.push_back({
    .device_id = "{safe-device-guid}",
    .display_name = "Display",
    .friendly_name = "physical && type secret.txt"
  });

  const auto json {confighttp::serialize_second_monitor_status(status)};
  const auto serialized {json.dump()};
  const auto &device_json {json.at("devices").at(0)};
  const auto &dangerous_device_json {json.at("devices").at(1)};

  EXPECT_EQ(json.at("state").get<std::string>(), "waiting_for_vdd");
  EXPECT_EQ(json.at("host_display_id").get<std::string>(), "{stable-vdd-guid}");
  EXPECT_TRUE(json.at("capture_backend").is_null());
  EXPECT_TRUE(json.at("encoder").is_null());
  EXPECT_TRUE(json.at("codec").is_null());
  EXPECT_TRUE(json.at("resolution").is_null());
  EXPECT_TRUE(json.at("fps").is_null());
  EXPECT_TRUE(json.at("bitrate_kbps").is_null());
  EXPECT_TRUE(json.at("rtt_ms").is_null());
  EXPECT_TRUE(json.at("dropped_frames").is_null());
  EXPECT_TRUE(json.at("reconnect_count").is_null());
  EXPECT_EQ(json.at("last_error").get<std::string>(), "redacted");
  EXPECT_EQ(json.at("last_reinitialization_reason").get<std::string>(), "redacted");
  EXPECT_EQ(json.at("mapped_output").get<std::string>().find_first_of("\r\n"), std::string::npos);
  EXPECT_EQ(device_json.at("device_id").get<std::string>().find("whoami"), std::string::npos);
  EXPECT_EQ(device_json.at("display_name").get<std::string>().find("whoami"), std::string::npos);
  EXPECT_LE(device_json.at("friendly_name").get<std::string>().size(), 128U);
  EXPECT_EQ(dangerous_device_json.at("friendly_name").get<std::string>(), "redacted");
  EXPECT_EQ(serialized.find("powershell"), std::string::npos);
  EXPECT_EQ(serialized.find("pairing-key"), std::string::npos);
  EXPECT_EQ(serialized.find("password"), std::string::npos);
}

TEST(SecondMonitorDiagnostics, UsesNullForUnavailableDeviceDimensions) {
  display_device::second_monitor_status_t status;
  status.devices.push_back({
    .device_id = "{vdd-guid}",
    .display_name = "",
    .friendly_name = "Virtual Display"
  });

  const auto json {confighttp::serialize_second_monitor_status(status)};

  EXPECT_TRUE(json.at("current_resolution").is_null());
  EXPECT_TRUE(json.at("devices").at(0).at("current_resolution").is_null());
}

TEST(DisplayDeviceClassification, UsesVirtualIdentitySignalsAndUnknownWhenMetadataIsInsufficient) {
  const display_device::EnumeratedDevice virtual_device {
    .m_device_id = "{vdd-guid}",
    .m_friendly_name = "Virtual Display",
    .m_virtual_device = true
  };
  const display_device::EnumeratedDevice physical_device {
    .m_device_id = "{physical-guid}",
    .m_friendly_name = "ROG PG279Q",
    .m_edid = display_device::EdidData {},
    .m_virtual_device = false
  };
  const display_device::EnumeratedDevice virtual_device_with_edid {
    .m_device_id = "{vdd-edid-guid}",
    .m_friendly_name = "Virtual Display",
    .m_edid = display_device::EdidData {},
    .m_virtual_device = std::nullopt
  };
  const display_device::EnumeratedDevice unknown_device {
    .m_device_id = "{unknown-guid}",
    .m_friendly_name = "Display",
    .m_virtual_device = std::nullopt
  };

  EXPECT_EQ(display_device::classify_virtual_device(virtual_device), std::optional<bool> {true});
  EXPECT_EQ(display_device::classify_virtual_device(physical_device), std::optional<bool> {false});
  EXPECT_EQ(display_device::classify_virtual_device(virtual_device_with_edid), std::nullopt);
  EXPECT_EQ(display_device::classify_virtual_device(unknown_device), std::nullopt);
}

using ParseHdrOption = DisplayDeviceConfigTest<std::pair<std::pair<hdr_option_e, client_wants_hdr_t>, std::optional<hdr_state_e>>>;
INSTANTIATE_TEST_SUITE_P(
  DisplayDeviceConfigTest,
  ParseHdrOption,
  testing::Values(
    std::make_pair(std::make_pair(hdr_option_e::disabled, client_wants_hdr_t {true}), std::nullopt),
    std::make_pair(std::make_pair(hdr_option_e::disabled, client_wants_hdr_t {false}), std::nullopt),
#ifdef __APPLE__
    std::make_pair(std::make_pair(hdr_option_e::automatic, client_wants_hdr_t {true}), std::nullopt),
    std::make_pair(std::make_pair(hdr_option_e::automatic, client_wants_hdr_t {false}), std::nullopt)
#else
    std::make_pair(std::make_pair(hdr_option_e::automatic, client_wants_hdr_t {true}), hdr_state_e::Enabled),
    std::make_pair(std::make_pair(hdr_option_e::automatic, client_wants_hdr_t {false}), hdr_state_e::Disabled)
#endif
  )
);

TEST_P(ParseHdrOption, IntegrationTest) {
  const auto &[input_value, expected_value] = GetParam();
  const auto &[input_hdr_option, input_enable_hdr] = input_value;

  config::video_t video_config {};
  video_config.dd.configuration_option = config_option_e::verify_only;
  video_config.dd.hdr_option = input_hdr_option;

  rtsp_stream::launch_session_t session {};
  session.enable_hdr = input_enable_hdr;

  const auto result {display_device::parse_configuration(video_config, session)};
  EXPECT_EQ(std::get<display_device::SingleDisplayConfiguration>(result).m_hdr_state, expected_value);
}

using ParseResolutionOption = DisplayDeviceConfigTest<std::pair<std::tuple<resolution_option_e, sops_enabled_t, std::variant<client_resolution_t, std::string>>, std::variant<failed_to_parse_resolution_tag_t, no_resolution_tag_t, resolution_t>>>;
INSTANTIATE_TEST_SUITE_P(
  DisplayDeviceConfigTest,
  ParseResolutionOption,
  testing::Values(
    //---- Disabled cases ----
    std::make_pair(std::make_tuple(resolution_option_e::disabled, sops_enabled_t {true}, client_resolution_t {1920, 1080}), no_resolution_tag_t {}),
    std::make_pair(std::make_tuple(resolution_option_e::disabled, sops_enabled_t {true}, "1920x1080"s), no_resolution_tag_t {}),
    std::make_pair(std::make_tuple(resolution_option_e::disabled, sops_enabled_t {true}, client_resolution_t {-1, -1}), no_resolution_tag_t {}),
    std::make_pair(std::make_tuple(resolution_option_e::disabled, sops_enabled_t {true}, "invalid_res"s), no_resolution_tag_t {}),
    std::make_pair(std::make_tuple(resolution_option_e::disabled, sops_enabled_t {false}, client_resolution_t {1920, 1080}), no_resolution_tag_t {}),
    std::make_pair(std::make_tuple(resolution_option_e::disabled, sops_enabled_t {false}, "1920x1080"s), no_resolution_tag_t {}),
    std::make_pair(std::make_tuple(resolution_option_e::disabled, sops_enabled_t {false}, client_resolution_t {-1, -1}), no_resolution_tag_t {}),
    std::make_pair(std::make_tuple(resolution_option_e::disabled, sops_enabled_t {false}, "invalid_res"s), no_resolution_tag_t {}),
    //---- Automatic cases ----
    std::make_pair(std::make_tuple(resolution_option_e::automatic, sops_enabled_t {true}, client_resolution_t {1920, 1080}), resolution_t {1920, 1080}),
    std::make_pair(std::make_tuple(resolution_option_e::automatic, sops_enabled_t {true}, "1920x1080"s), resolution_t {}),
    std::make_pair(std::make_tuple(resolution_option_e::automatic, sops_enabled_t {true}, client_resolution_t {-1, -1}), failed_to_parse_resolution_tag_t {}),
    std::make_pair(std::make_tuple(resolution_option_e::automatic, sops_enabled_t {true}, "invalid_res"s), resolution_t {}),
    std::make_pair(std::make_tuple(resolution_option_e::automatic, sops_enabled_t {false}, client_resolution_t {1920, 1080}), no_resolution_tag_t {}),
    std::make_pair(std::make_tuple(resolution_option_e::automatic, sops_enabled_t {false}, "1920x1080"s), no_resolution_tag_t {}),
    std::make_pair(std::make_tuple(resolution_option_e::automatic, sops_enabled_t {false}, client_resolution_t {-1, -1}), no_resolution_tag_t {}),
    std::make_pair(std::make_tuple(resolution_option_e::automatic, sops_enabled_t {false}, "invalid_res"s), no_resolution_tag_t {}),
    //---- Manual cases ----
    std::make_pair(std::make_tuple(resolution_option_e::manual, sops_enabled_t {true}, client_resolution_t {1920, 1080}), failed_to_parse_resolution_tag_t {}),
    std::make_pair(std::make_tuple(resolution_option_e::manual, sops_enabled_t {true}, "1920x1080"s), resolution_t {1920, 1080}),
    std::make_pair(std::make_tuple(resolution_option_e::manual, sops_enabled_t {true}, client_resolution_t {-1, -1}), failed_to_parse_resolution_tag_t {}),
    std::make_pair(std::make_tuple(resolution_option_e::manual, sops_enabled_t {true}, "invalid_res"s), failed_to_parse_resolution_tag_t {}),
    std::make_pair(std::make_tuple(resolution_option_e::manual, sops_enabled_t {false}, client_resolution_t {1920, 1080}), no_resolution_tag_t {}),
    std::make_pair(std::make_tuple(resolution_option_e::manual, sops_enabled_t {false}, "1920x1080"s), no_resolution_tag_t {}),
    std::make_pair(std::make_tuple(resolution_option_e::manual, sops_enabled_t {false}, client_resolution_t {-1, -1}), no_resolution_tag_t {}),
    std::make_pair(std::make_tuple(resolution_option_e::manual, sops_enabled_t {false}, "invalid_res"s), no_resolution_tag_t {}),
    //---- Both negative values from client are checked ----
    std::make_pair(std::make_tuple(resolution_option_e::automatic, sops_enabled_t {true}, client_resolution_t {0, 0}), resolution_t {0, 0}),
    std::make_pair(std::make_tuple(resolution_option_e::automatic, sops_enabled_t {true}, client_resolution_t {-1, 0}), failed_to_parse_resolution_tag_t {}),
    std::make_pair(std::make_tuple(resolution_option_e::automatic, sops_enabled_t {true}, client_resolution_t {0, -1}), failed_to_parse_resolution_tag_t {}),
    //---- Resolution string format validation ----
    std::make_pair(std::make_tuple(resolution_option_e::manual, sops_enabled_t {true}, "0x0"s), resolution_t {0, 0}),
    std::make_pair(std::make_tuple(resolution_option_e::manual, sops_enabled_t {true}, "0x"s), failed_to_parse_resolution_tag_t {}),
    std::make_pair(std::make_tuple(resolution_option_e::manual, sops_enabled_t {true}, "x0"s), failed_to_parse_resolution_tag_t {}),
    std::make_pair(std::make_tuple(resolution_option_e::manual, sops_enabled_t {true}, "-1x1"s), failed_to_parse_resolution_tag_t {}),
    std::make_pair(std::make_tuple(resolution_option_e::manual, sops_enabled_t {true}, "1x-1"s), failed_to_parse_resolution_tag_t {}),
    std::make_pair(std::make_tuple(resolution_option_e::manual, sops_enabled_t {true}, "x0x0"s), failed_to_parse_resolution_tag_t {}),
    std::make_pair(std::make_tuple(resolution_option_e::manual, sops_enabled_t {true}, "0x0x"s), failed_to_parse_resolution_tag_t {}),
    //---- String number is out of bounds ----
    std::make_pair(std::make_tuple(resolution_option_e::manual, sops_enabled_t {true}, max_uint_string + "x"s + max_uint_string), resolution_t {max_uint, max_uint}),
    std::make_pair(std::make_tuple(resolution_option_e::manual, sops_enabled_t {true}, max_uint_string + "0"s + "x"s + max_uint_string), failed_to_parse_resolution_tag_t {}),
    std::make_pair(std::make_tuple(resolution_option_e::manual, sops_enabled_t {true}, max_uint_string + "x"s + max_uint_string + "0"s), failed_to_parse_resolution_tag_t {})
  )
);

TEST_P(ParseResolutionOption, IntegrationTest) {
  const auto &[input_value, expected_value] = GetParam();
  const auto &[input_resolution_option, input_enable_sops, input_resolution] = input_value;

  config::video_t video_config {};
  video_config.dd.configuration_option = config_option_e::verify_only;
  video_config.dd.resolution_option = input_resolution_option;

  rtsp_stream::launch_session_t session {};
  session.enable_sops = input_enable_sops;

  if (const auto *client_res {std::get_if<client_resolution_t>(&input_resolution)}; client_res) {
    video_config.dd.manual_resolution = {};
    session.width = client_res->width;
    session.height = client_res->height;
  } else {
    video_config.dd.manual_resolution = std::get<std::string>(input_resolution);
    session.width = {};
    session.height = {};
  }

  const auto result {display_device::parse_configuration(video_config, session)};
  if (const auto *failed_option {std::get_if<failed_to_parse_resolution_tag_t>(&expected_value)}; failed_option) {
    EXPECT_NO_THROW((void) std::get<display_device::failed_to_parse_tag_t>(result));
  } else {
    std::optional<resolution_t> expected_resolution;
    if (const auto *valid_resolution_option {std::get_if<resolution_t>(&expected_value)}; valid_resolution_option) {
      expected_resolution = *valid_resolution_option;
    }

    EXPECT_EQ(std::get<display_device::SingleDisplayConfiguration>(result).m_resolution, expected_resolution);
  }
}

using ParseRefreshRateOption = DisplayDeviceConfigTest<std::pair<std::tuple<refresh_rate_option_e, std::variant<client_fps_t, std::string>>, std::variant<failed_to_parse_refresh_rate_tag_t, no_refresh_rate_tag_t, rational_t>>>;
INSTANTIATE_TEST_SUITE_P(
  DisplayDeviceConfigTest,
  ParseRefreshRateOption,
  testing::Values(
    //---- Disabled cases ----
    std::make_pair(std::make_tuple(refresh_rate_option_e::disabled, client_fps_t {60}), no_refresh_rate_tag_t {}),
    std::make_pair(std::make_tuple(refresh_rate_option_e::disabled, "60"s), no_refresh_rate_tag_t {}),
    std::make_pair(std::make_tuple(refresh_rate_option_e::disabled, "59.9885"s), no_refresh_rate_tag_t {}),
    std::make_pair(std::make_tuple(refresh_rate_option_e::disabled, client_fps_t {-1}), no_refresh_rate_tag_t {}),
    std::make_pair(std::make_tuple(refresh_rate_option_e::disabled, "invalid_refresh_rate"s), no_refresh_rate_tag_t {}),
    //---- Automatic cases ----
    std::make_pair(std::make_tuple(refresh_rate_option_e::automatic, client_fps_t {60}), rational_t {60, 1}),
    std::make_pair(std::make_tuple(refresh_rate_option_e::automatic, "60"s), rational_t {0, 1}),
    std::make_pair(std::make_tuple(refresh_rate_option_e::automatic, "59.9885"s), rational_t {0, 1}),
    std::make_pair(std::make_tuple(refresh_rate_option_e::automatic, client_fps_t {-1}), failed_to_parse_refresh_rate_tag_t {}),
    std::make_pair(std::make_tuple(refresh_rate_option_e::automatic, "invalid_refresh_rate"s), rational_t {0, 1}),
    //---- Manual cases ----
    std::make_pair(std::make_tuple(refresh_rate_option_e::manual, client_fps_t {60}), failed_to_parse_refresh_rate_tag_t {}),
    std::make_pair(std::make_tuple(refresh_rate_option_e::manual, "60"s), rational_t {60, 1}),
    std::make_pair(std::make_tuple(refresh_rate_option_e::manual, "59.9885"s), rational_t {599885, 10000}),
    std::make_pair(std::make_tuple(refresh_rate_option_e::manual, client_fps_t {-1}), failed_to_parse_refresh_rate_tag_t {}),
    std::make_pair(std::make_tuple(refresh_rate_option_e::manual, "invalid_refresh_rate"s), failed_to_parse_refresh_rate_tag_t {}),
    //---- Refresh rate string format validation ----
    std::make_pair(std::make_tuple(refresh_rate_option_e::manual, "0000000000000"s), rational_t {0, 1}),
    std::make_pair(std::make_tuple(refresh_rate_option_e::manual, "0"s), rational_t {0, 1}),
    std::make_pair(std::make_tuple(refresh_rate_option_e::manual, "00000000.0000000"s), rational_t {0, 1}),
    std::make_pair(std::make_tuple(refresh_rate_option_e::manual, "0.0"s), rational_t {0, 1}),
    std::make_pair(std::make_tuple(refresh_rate_option_e::manual, "000000000000010"s), rational_t {10, 1}),
    std::make_pair(std::make_tuple(refresh_rate_option_e::manual, "00000010.0000000"s), rational_t {10, 1}),
    std::make_pair(std::make_tuple(refresh_rate_option_e::manual, "00000010.1000000"s), rational_t {101, 10}),
    std::make_pair(std::make_tuple(refresh_rate_option_e::manual, "00000010.0100000"s), rational_t {1001, 100}),
    std::make_pair(std::make_tuple(refresh_rate_option_e::manual, "00000000.1000000"s), rational_t {1, 10}),
    std::make_pair(std::make_tuple(refresh_rate_option_e::manual, "60,0"s), failed_to_parse_refresh_rate_tag_t {}),
    std::make_pair(std::make_tuple(refresh_rate_option_e::manual, "-60.0"s), failed_to_parse_refresh_rate_tag_t {}),
    std::make_pair(std::make_tuple(refresh_rate_option_e::manual, "60.-0"s), failed_to_parse_refresh_rate_tag_t {}),
    std::make_pair(std::make_tuple(refresh_rate_option_e::manual, "a60.0"s), failed_to_parse_refresh_rate_tag_t {}),
    std::make_pair(std::make_tuple(refresh_rate_option_e::manual, "60.0b"s), failed_to_parse_refresh_rate_tag_t {}),
    std::make_pair(std::make_tuple(refresh_rate_option_e::manual, "a60"s), failed_to_parse_refresh_rate_tag_t {}),
    std::make_pair(std::make_tuple(refresh_rate_option_e::manual, "60b"s), failed_to_parse_refresh_rate_tag_t {}),
    std::make_pair(std::make_tuple(refresh_rate_option_e::manual, "-60"s), failed_to_parse_refresh_rate_tag_t {}),
    //---- String number is out of bounds ----
    std::make_pair(std::make_tuple(refresh_rate_option_e::manual, max_uint_string), rational_t {max_uint, 1}),
    std::make_pair(std::make_tuple(refresh_rate_option_e::manual, max_uint_string + "0"s), failed_to_parse_refresh_rate_tag_t {}),
    std::make_pair(std::make_tuple(refresh_rate_option_e::manual, max_uint_string.substr(0, 1) + "."s + max_uint_string.substr(1)), rational_t {max_uint, static_cast<unsigned int>(std::pow(10, max_uint_string.size() - 1))}),
    std::make_pair(std::make_tuple(refresh_rate_option_e::manual, max_uint_string.substr(0, 1) + "0"s + "."s + max_uint_string.substr(1)), failed_to_parse_refresh_rate_tag_t {}),
    std::make_pair(std::make_tuple(refresh_rate_option_e::manual, max_uint_string.substr(0, 1) + "."s + "0"s + max_uint_string.substr(1)), failed_to_parse_refresh_rate_tag_t {})
  )
);

TEST_P(ParseRefreshRateOption, IntegrationTest) {
  const auto &[input_value, expected_value] = GetParam();
  const auto &[input_refresh_rate_option, input_refresh_rate] = input_value;

  config::video_t video_config {};
  video_config.dd.configuration_option = config_option_e::verify_only;
  video_config.dd.refresh_rate_option = input_refresh_rate_option;

  rtsp_stream::launch_session_t session {};
  if (const auto *client_refresh_rate {std::get_if<client_fps_t>(&input_refresh_rate)}; client_refresh_rate) {
    video_config.dd.manual_refresh_rate = {};
    session.fps = *client_refresh_rate;
  } else {
    video_config.dd.manual_refresh_rate = std::get<std::string>(input_refresh_rate);
    session.fps = {};
  }

  const auto result {display_device::parse_configuration(video_config, session)};
  if (const auto *failed_option {std::get_if<failed_to_parse_refresh_rate_tag_t>(&expected_value)}; failed_option) {
    EXPECT_NO_THROW((void) std::get<display_device::failed_to_parse_tag_t>(result));
  } else {
    std::optional<display_device::FloatingPoint> expected_refresh_rate;
    if (const auto *valid_refresh_rate_option {std::get_if<rational_t>(&expected_value)}; valid_refresh_rate_option) {
      expected_refresh_rate = *valid_refresh_rate_option;
    }

    EXPECT_EQ(std::get<display_device::SingleDisplayConfiguration>(result).m_refresh_rate, expected_refresh_rate);
  }
}

namespace {
  using res_t = resolution_t;
  using fps_t = client_fps_t;
  using remap_entries_t = config::video_t::dd_t::mode_remapping_t;

  struct no_value_t {};

  template<class T>
  struct auto_value_t {
    T value;
  };

  template<class T>
  struct manual_value_t {
    T value;
  };

  using resolution_variant_t = std::variant<no_value_t, auto_value_t<res_t>, manual_value_t<res_t>>;
  using rational_variant_t = std::variant<no_value_t, auto_value_t<fps_t>, manual_value_t<fps_t>>;

  struct failed_to_remap_t {};

  struct final_values_t {
    std::optional<resolution_t> resolution;
    std::optional<rational_t> refresh_rate;
  };

  const std::string INVALID_RES {"INVALID"};
  const std::string INVALID_FPS {"1.23"};
  const std::string INVALID_REFRESH_RATE {"INVALID"};
  const remap_entries_t VALID_ENTRIES {
    .mixed = {
      {"1920x1080", "11", "1024x720", "1.11"},
      {"1920x1080", "", "1024x720", "2"},
      {"", "33", "1024x720", "3"},
      {"1920x720", "44", "1024x720", ""},
      {"1920x720", "55", "", "5"},
      {"1920x720", "", "1024x720", ""},
      {"", "11", "", "7.77"}
    },
    .resolution_only = {{"1920x1080", "", "720x720", ""}, {"1024x720", "", "1920x1920", ""}},
    .refresh_rate_only = {{"", "11", "", "1.23"}, {"", "22", "", "2.34"}}
  };
  const remap_entries_t INVALID_REQ_RES {
    .mixed = {{INVALID_RES, "11", "1024x720", "1.11"}},
    .resolution_only = {{INVALID_RES, "", "720x720", ""}},
    .refresh_rate_only = {{INVALID_RES, "11", "", "1.23"}}
  };
  const remap_entries_t INVALID_REQ_FPS {
    .mixed = {{"1920x1080", INVALID_FPS, "1024x720", "1.11"}},
    .resolution_only = {{"1920x1080", INVALID_FPS, "720x720", ""}},
    .refresh_rate_only = {{"", INVALID_FPS, "", "1.23"}}
  };
  const remap_entries_t INVALID_FINAL_RES {
    .mixed = {{"1920x1080", "11", INVALID_RES, "1.11"}},
    .resolution_only = {{"1920x1080", "", INVALID_RES, ""}},
    .refresh_rate_only = {{"", "11", INVALID_RES, "1.23"}}
  };
  const remap_entries_t INVALID_FINAL_REFRESH_RATE {
    .mixed = {{"1920x1080", "11", "1024x720", INVALID_REFRESH_RATE}},
    .resolution_only = {{"1920x1080", "", "720x720", INVALID_REFRESH_RATE}},
    .refresh_rate_only = {{"", "11", "", INVALID_REFRESH_RATE}}
  };
  const remap_entries_t EMPTY_REQ_ENTRIES {
    .mixed = {{"", "", "1024x720", "1.11"}},
    .resolution_only = {{"", "", "720x720", ""}},
    .refresh_rate_only = {{"", "", "", "1.23"}}
  };
  const remap_entries_t EMPTY_FINAL_ENTRIES {
    .mixed = {{"1920x1080", "11", "", ""}},
    .resolution_only = {{"1920x1080", "", "", ""}},
    .refresh_rate_only = {{"", "11", "", ""}}
  };

  using DisplayModeRemapping = DisplayDeviceConfigTest<std::pair<std::tuple<resolution_variant_t, rational_variant_t, sops_enabled_t, remap_entries_t>, std::variant<failed_to_remap_t, final_values_t>>>;
  INSTANTIATE_TEST_SUITE_P(
    DisplayDeviceConfigTest,
    DisplayModeRemapping,
    testing::Values(
      //---- Mixed (valid), SOPS enabled ----
      std::make_pair(std::make_tuple(auto_value_t<res_t> {1920, 1080}, auto_value_t<fps_t> {11}, sops_enabled_t {true}, VALID_ENTRIES), final_values_t {{{1024, 720}}, {{111, 100}}}),
      std::make_pair(std::make_tuple(auto_value_t<res_t> {1920, 1080}, auto_value_t<fps_t> {120}, sops_enabled_t {true}, VALID_ENTRIES), final_values_t {{{1024, 720}}, {{2, 1}}}),
      std::make_pair(std::make_tuple(auto_value_t<res_t> {1, 1}, auto_value_t<fps_t> {33}, sops_enabled_t {true}, VALID_ENTRIES), final_values_t {{{1024, 720}}, {{3, 1}}}),
      std::make_pair(std::make_tuple(auto_value_t<res_t> {1920, 720}, auto_value_t<fps_t> {44}, sops_enabled_t {true}, VALID_ENTRIES), final_values_t {{{1024, 720}}, {{44, 1}}}),
      std::make_pair(std::make_tuple(auto_value_t<res_t> {1920, 720}, auto_value_t<fps_t> {55}, sops_enabled_t {true}, VALID_ENTRIES), final_values_t {{{1920, 720}}, {{5, 1}}}),
      std::make_pair(std::make_tuple(auto_value_t<res_t> {1920, 720}, auto_value_t<fps_t> {60}, sops_enabled_t {true}, VALID_ENTRIES), final_values_t {{{1024, 720}}, {{60, 1}}}),
      std::make_pair(std::make_tuple(auto_value_t<res_t> {1, 1}, auto_value_t<fps_t> {123}, sops_enabled_t {true}, VALID_ENTRIES), final_values_t {{{1, 1}}, {{123, 1}}}),
      //---- Mixed (valid), SOPS disabled ----
      std::make_pair(std::make_tuple(auto_value_t<res_t> {1920, 1080}, auto_value_t<fps_t> {11}, sops_enabled_t {false}, VALID_ENTRIES), final_values_t {std::nullopt, {{777, 100}}}),
      std::make_pair(std::make_tuple(auto_value_t<res_t> {1920, 1080}, auto_value_t<fps_t> {120}, sops_enabled_t {false}, VALID_ENTRIES), final_values_t {std::nullopt, {{120, 1}}}),
      std::make_pair(std::make_tuple(auto_value_t<res_t> {1, 1}, auto_value_t<fps_t> {33}, sops_enabled_t {false}, VALID_ENTRIES), final_values_t {std::nullopt, {{33, 1}}}),
      std::make_pair(std::make_tuple(auto_value_t<res_t> {1920, 720}, auto_value_t<fps_t> {44}, sops_enabled_t {false}, VALID_ENTRIES), final_values_t {std::nullopt, {{44, 1}}}),
      std::make_pair(std::make_tuple(auto_value_t<res_t> {1920, 720}, auto_value_t<fps_t> {55}, sops_enabled_t {false}, VALID_ENTRIES), final_values_t {std::nullopt, {{55, 1}}}),
      std::make_pair(std::make_tuple(auto_value_t<res_t> {1920, 720}, auto_value_t<fps_t> {60}, sops_enabled_t {false}, VALID_ENTRIES), final_values_t {std::nullopt, {{60, 1}}}),
      std::make_pair(std::make_tuple(auto_value_t<res_t> {1, 1}, auto_value_t<fps_t> {123}, sops_enabled_t {false}, VALID_ENTRIES), final_values_t {std::nullopt, {{123, 1}}}),
      //---- Resolution only (valid), SOPS enabled ----
      std::make_pair(std::make_tuple(auto_value_t<res_t> {1920, 1080}, manual_value_t<fps_t> {11}, sops_enabled_t {true}, VALID_ENTRIES), final_values_t {{{720, 720}}, {{11, 1}}}),
      std::make_pair(std::make_tuple(auto_value_t<res_t> {1024, 720}, no_value_t {}, sops_enabled_t {true}, VALID_ENTRIES), final_values_t {{{1920, 1920}}, std::nullopt}),
      std::make_pair(std::make_tuple(auto_value_t<res_t> {11, 11}, manual_value_t<fps_t> {33}, sops_enabled_t {true}, VALID_ENTRIES), final_values_t {{{11, 11}}, {{33, 1}}}),
      //---- Resolution only (valid), SOPS disabled ----
      std::make_pair(std::make_tuple(auto_value_t<res_t> {1920, 1080}, manual_value_t<fps_t> {11}, sops_enabled_t {false}, VALID_ENTRIES), final_values_t {std::nullopt, {{11, 1}}}),
      std::make_pair(std::make_tuple(auto_value_t<res_t> {1024, 720}, no_value_t {}, sops_enabled_t {false}, VALID_ENTRIES), final_values_t {std::nullopt, std::nullopt}),
      std::make_pair(std::make_tuple(auto_value_t<res_t> {11, 11}, manual_value_t<fps_t> {33}, sops_enabled_t {false}, VALID_ENTRIES), final_values_t {std::nullopt, {{33, 1}}}),
      //---- Refresh rate only (valid), SOPS enabled ----
      std::make_pair(std::make_tuple(manual_value_t<res_t> {1920, 1080}, auto_value_t<fps_t> {11}, sops_enabled_t {true}, VALID_ENTRIES), final_values_t {{{1920, 1080}}, {{123, 100}}}),
      std::make_pair(std::make_tuple(no_value_t {}, auto_value_t<fps_t> {22}, sops_enabled_t {true}, VALID_ENTRIES), final_values_t {std::nullopt, {{234, 100}}}),
      std::make_pair(std::make_tuple(manual_value_t<res_t> {11, 11}, auto_value_t<fps_t> {33}, sops_enabled_t {true}, VALID_ENTRIES), final_values_t {{{11, 11}}, {{33, 1}}}),
      //---- Refresh rate only (valid), SOPS disabled ----
      std::make_pair(std::make_tuple(manual_value_t<res_t> {1920, 1080}, auto_value_t<fps_t> {11}, sops_enabled_t {false}, VALID_ENTRIES), final_values_t {std::nullopt, {{123, 100}}}),
      std::make_pair(std::make_tuple(no_value_t {}, auto_value_t<fps_t> {22}, sops_enabled_t {false}, VALID_ENTRIES), final_values_t {std::nullopt, {{234, 100}}}),
      std::make_pair(std::make_tuple(manual_value_t<res_t> {11, 11}, auto_value_t<fps_t> {33}, sops_enabled_t {false}, VALID_ENTRIES), final_values_t {std::nullopt, {{33, 1}}}),
      //---- No mapping (valid), SOPS enabled ----
      std::make_pair(std::make_tuple(manual_value_t<res_t> {1920, 1080}, manual_value_t<fps_t> {11}, sops_enabled_t {true}, VALID_ENTRIES), final_values_t {{{1920, 1080}}, {{11, 1}}}),
      std::make_pair(std::make_tuple(no_value_t {}, no_value_t {}, sops_enabled_t {true}, VALID_ENTRIES), final_values_t {std::nullopt, std::nullopt}),
      //---- No mapping (valid), SOPS disabled ----
      std::make_pair(std::make_tuple(manual_value_t<res_t> {1920, 1080}, manual_value_t<fps_t> {11}, sops_enabled_t {false}, VALID_ENTRIES), final_values_t {std::nullopt, {{11, 1}}}),
      std::make_pair(std::make_tuple(no_value_t {}, no_value_t {}, sops_enabled_t {false}, VALID_ENTRIES), final_values_t {std::nullopt, std::nullopt}),
      // ---- Invalid requested resolution, SOPS enabled ----
      std::make_pair(std::make_tuple(auto_value_t<res_t> {1920, 1080}, auto_value_t<fps_t> {11}, sops_enabled_t {true}, INVALID_REQ_RES), failed_to_remap_t {}),
      std::make_pair(std::make_tuple(auto_value_t<res_t> {1920, 1080}, manual_value_t<fps_t> {11}, sops_enabled_t {true}, INVALID_REQ_RES), failed_to_remap_t {}),
      std::make_pair(std::make_tuple(manual_value_t<res_t> {1920, 1080}, auto_value_t<fps_t> {11}, sops_enabled_t {true}, INVALID_REQ_RES), final_values_t {{{1920, 1080}}, {{123, 100}}}),
      // ---- Invalid requested resolution, SOPS disabled ----
      std::make_pair(std::make_tuple(auto_value_t<res_t> {1920, 1080}, auto_value_t<fps_t> {11}, sops_enabled_t {false}, INVALID_REQ_RES), failed_to_remap_t {}),
      std::make_pair(std::make_tuple(auto_value_t<res_t> {1920, 1080}, manual_value_t<fps_t> {11}, sops_enabled_t {false}, INVALID_REQ_RES), failed_to_remap_t {}),
      std::make_pair(std::make_tuple(manual_value_t<res_t> {1920, 1080}, auto_value_t<fps_t> {11}, sops_enabled_t {false}, INVALID_REQ_RES), final_values_t {std::nullopt, {{123, 100}}}),
      // ---- Invalid requested FPS, SOPS enabled ----
      std::make_pair(std::make_tuple(auto_value_t<res_t> {1920, 1080}, auto_value_t<fps_t> {11}, sops_enabled_t {true}, INVALID_REQ_FPS), failed_to_remap_t {}),
      std::make_pair(std::make_tuple(auto_value_t<res_t> {1920, 1080}, manual_value_t<fps_t> {11}, sops_enabled_t {true}, INVALID_REQ_FPS), final_values_t {{{720, 720}}, {{11, 1}}}),
      std::make_pair(std::make_tuple(manual_value_t<res_t> {1920, 1080}, auto_value_t<fps_t> {11}, sops_enabled_t {true}, INVALID_REQ_FPS), failed_to_remap_t {}),
      // ---- Invalid requested FPS, SOPS disabled ----
      std::make_pair(std::make_tuple(auto_value_t<res_t> {1920, 1080}, auto_value_t<fps_t> {11}, sops_enabled_t {false}, INVALID_REQ_FPS), failed_to_remap_t {}),
      std::make_pair(std::make_tuple(auto_value_t<res_t> {1920, 1080}, manual_value_t<fps_t> {11}, sops_enabled_t {false}, INVALID_REQ_FPS), final_values_t {std::nullopt, {{11, 1}}}),
      std::make_pair(std::make_tuple(manual_value_t<res_t> {1920, 1080}, auto_value_t<fps_t> {11}, sops_enabled_t {false}, INVALID_REQ_FPS), failed_to_remap_t {}),
      // ---- Invalid final resolution, SOPS enabled ----
      std::make_pair(std::make_tuple(auto_value_t<res_t> {1920, 1080}, auto_value_t<fps_t> {11}, sops_enabled_t {true}, INVALID_FINAL_RES), failed_to_remap_t {}),
      std::make_pair(std::make_tuple(auto_value_t<res_t> {1920, 1080}, manual_value_t<fps_t> {11}, sops_enabled_t {true}, INVALID_FINAL_RES), failed_to_remap_t {}),
      std::make_pair(std::make_tuple(manual_value_t<res_t> {1920, 1080}, auto_value_t<fps_t> {11}, sops_enabled_t {true}, INVALID_FINAL_RES), final_values_t {{{1920, 1080}}, {{123, 100}}}),
      // ---- Invalid final resolution, SOPS disabled ----
      std::make_pair(std::make_tuple(auto_value_t<res_t> {1920, 1080}, auto_value_t<fps_t> {11}, sops_enabled_t {false}, INVALID_FINAL_RES), failed_to_remap_t {}),
      std::make_pair(std::make_tuple(auto_value_t<res_t> {1920, 1080}, manual_value_t<fps_t> {11}, sops_enabled_t {false}, INVALID_FINAL_RES), failed_to_remap_t {}),
      std::make_pair(std::make_tuple(manual_value_t<res_t> {1920, 1080}, auto_value_t<fps_t> {11}, sops_enabled_t {false}, INVALID_FINAL_RES), final_values_t {std::nullopt, {{123, 100}}}),
      // ---- Invalid final refresh rate, SOPS enabled ----
      std::make_pair(std::make_tuple(auto_value_t<res_t> {1920, 1080}, auto_value_t<fps_t> {11}, sops_enabled_t {true}, INVALID_FINAL_REFRESH_RATE), failed_to_remap_t {}),
      std::make_pair(std::make_tuple(auto_value_t<res_t> {1920, 1080}, manual_value_t<fps_t> {11}, sops_enabled_t {true}, INVALID_FINAL_REFRESH_RATE), final_values_t {{{720, 720}}, {{11, 1}}}),
      std::make_pair(std::make_tuple(manual_value_t<res_t> {1920, 1080}, auto_value_t<fps_t> {11}, sops_enabled_t {true}, INVALID_FINAL_REFRESH_RATE), failed_to_remap_t {}),
      // ---- Invalid final refresh rate, SOPS disabled ----
      std::make_pair(std::make_tuple(auto_value_t<res_t> {1920, 1080}, auto_value_t<fps_t> {11}, sops_enabled_t {false}, INVALID_FINAL_REFRESH_RATE), failed_to_remap_t {}),
      std::make_pair(std::make_tuple(auto_value_t<res_t> {1920, 1080}, manual_value_t<fps_t> {11}, sops_enabled_t {false}, INVALID_FINAL_REFRESH_RATE), final_values_t {std::nullopt, {{11, 1}}}),
      std::make_pair(std::make_tuple(manual_value_t<res_t> {1920, 1080}, auto_value_t<fps_t> {11}, sops_enabled_t {false}, INVALID_FINAL_REFRESH_RATE), failed_to_remap_t {}),
      // ---- Empty req entries, SOPS enabled ----
      std::make_pair(std::make_tuple(auto_value_t<res_t> {1920, 1080}, auto_value_t<fps_t> {11}, sops_enabled_t {true}, EMPTY_REQ_ENTRIES), final_values_t {{{1024, 720}}, {{111, 100}}}),
      std::make_pair(std::make_tuple(auto_value_t<res_t> {1920, 1080}, manual_value_t<fps_t> {11}, sops_enabled_t {true}, EMPTY_REQ_ENTRIES), final_values_t {{{720, 720}}, {{11, 1}}}),
      std::make_pair(std::make_tuple(manual_value_t<res_t> {1920, 1080}, auto_value_t<fps_t> {11}, sops_enabled_t {true}, EMPTY_REQ_ENTRIES), final_values_t {{{1920, 1080}}, {{123, 100}}}),
      // ---- Empty req entries, SOPS disabled ----
      std::make_pair(std::make_tuple(auto_value_t<res_t> {1920, 1080}, auto_value_t<fps_t> {11}, sops_enabled_t {false}, EMPTY_REQ_ENTRIES), final_values_t {std::nullopt, {{11, 1}}}),
      std::make_pair(std::make_tuple(auto_value_t<res_t> {1920, 1080}, manual_value_t<fps_t> {11}, sops_enabled_t {false}, EMPTY_REQ_ENTRIES), final_values_t {std::nullopt, {{11, 1}}}),
      std::make_pair(std::make_tuple(manual_value_t<res_t> {1920, 1080}, auto_value_t<fps_t> {11}, sops_enabled_t {false}, EMPTY_REQ_ENTRIES), final_values_t {std::nullopt, {{123, 100}}}),
      // ---- Empty final entries, SOPS enabled ----
      std::make_pair(std::make_tuple(auto_value_t<res_t> {1920, 1080}, auto_value_t<fps_t> {11}, sops_enabled_t {true}, EMPTY_FINAL_ENTRIES), failed_to_remap_t {}),
      std::make_pair(std::make_tuple(auto_value_t<res_t> {1920, 1080}, manual_value_t<fps_t> {11}, sops_enabled_t {true}, EMPTY_FINAL_ENTRIES), failed_to_remap_t {}),
      std::make_pair(std::make_tuple(manual_value_t<res_t> {1920, 1080}, auto_value_t<fps_t> {11}, sops_enabled_t {true}, EMPTY_FINAL_ENTRIES), failed_to_remap_t {}),
      // ---- Empty final entries, SOPS disabled ----
      std::make_pair(std::make_tuple(auto_value_t<res_t> {1920, 1080}, auto_value_t<fps_t> {11}, sops_enabled_t {false}, EMPTY_FINAL_ENTRIES), failed_to_remap_t {}),
      std::make_pair(std::make_tuple(auto_value_t<res_t> {1920, 1080}, manual_value_t<fps_t> {11}, sops_enabled_t {false}, EMPTY_FINAL_ENTRIES), failed_to_remap_t {}),
      std::make_pair(std::make_tuple(manual_value_t<res_t> {1920, 1080}, auto_value_t<fps_t> {11}, sops_enabled_t {false}, EMPTY_FINAL_ENTRIES), failed_to_remap_t {})
    )
  );

  TEST_P(DisplayModeRemapping, IntegrationTest) {
    const auto &[input_value, expected_value] = GetParam();
    const auto &[input_res, input_fps, input_enable_sops, input_entries] = input_value;

    config::video_t video_config {};
    rtsp_stream::launch_session_t session {};

    {  // resolution
      using enum resolution_option_e;

      if (const auto *no_res {std::get_if<no_value_t>(&input_res)}; no_res) {
        video_config.dd.resolution_option = disabled;
      } else if (const auto *auto_res {std::get_if<auto_value_t<res_t>>(&input_res)}; auto_res) {
        video_config.dd.resolution_option = automatic;
        session.width = static_cast<int>(auto_res->value.m_width);
        session.height = static_cast<int>(auto_res->value.m_height);
      } else {
        const auto [manual_res] = std::get<manual_value_t<res_t>>(input_res);
        video_config.dd.resolution_option = manual;
        video_config.dd.manual_resolution = std::format("{}x{}", static_cast<int>(manual_res.m_width), static_cast<int>(manual_res.m_height));
      }
    }

    {  // fps
      using enum refresh_rate_option_e;

      if (const auto *no_fps {std::get_if<no_value_t>(&input_fps)}; no_fps) {
        video_config.dd.refresh_rate_option = disabled;
      } else if (const auto *auto_fps {std::get_if<auto_value_t<fps_t>>(&input_fps)}; auto_fps) {
        video_config.dd.refresh_rate_option = automatic;
        session.fps = auto_fps->value;
      } else {
        const auto [manual_fps] = std::get<manual_value_t<fps_t>>(input_fps);
        video_config.dd.refresh_rate_option = manual;
        video_config.dd.manual_refresh_rate = std::to_string(manual_fps);
      }
    }

    video_config.dd.configuration_option = config_option_e::verify_only;
    video_config.dd.mode_remapping = input_entries;
    session.enable_sops = input_enable_sops;

    const auto result {display_device::parse_configuration(video_config, session)};
    if (const auto *failed_option {std::get_if<failed_to_remap_t>(&expected_value)}; failed_option) {
      EXPECT_NO_THROW((void) std::get<display_device::failed_to_parse_tag_t>(result));
    } else {
      const auto &[expected_resolution, expected_refresh_rate] = std::get<final_values_t>(expected_value);
      const auto &parsed_config = std::get<display_device::SingleDisplayConfiguration>(result);

      EXPECT_EQ(parsed_config.m_resolution, expected_resolution);
      EXPECT_EQ(parsed_config.m_refresh_rate, expected_refresh_rate ? std::make_optional(display_device::FloatingPoint {*expected_refresh_rate}) : std::nullopt);
    }
  }
}  // namespace
