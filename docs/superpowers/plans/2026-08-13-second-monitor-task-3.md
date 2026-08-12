# Sunshine Second Monitor Task 3 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans. Steps use checkbox (`- [ ]`) syntax.

**Goal:** Add an opt-in VDD-backed second-monitor profile, lifecycle ownership tracking, authenticated diagnostics, and a selector that never falls back to a physical display.

**Architecture:** Keep the existing flat Sunshine configuration format and add `second_monitor_*` keys whose non-empty device ID enables the profile. The profile is converted into the existing display/audio/capture settings only when enabled, so an empty profile leaves ordinary Sunshine behavior unchanged. `display_device` owns a small synchronized lifecycle/status record; launch, capture retry, session teardown, and restore update that record without invoking process commands or touching the kernel driver.

**Tech Stack:** C++23, Boost property tree, nlohmann/json, libdisplaydevice, GoogleTest, Vue 3.

## Global Constraints

- The selected VDD is the only capture target for second-monitor mode.
- The profile defaults to 1280x720 at 30 FPS, HDR disabled, Desktop Duplication capture, no streamed audio, `vdd_only`, and restore enabled.
- Regular Sunshine behavior remains unchanged when `second_monitor_device_id` is empty.
- Existing VDD devices may be enumerated and configured; Sunshine must not create or destroy the kernel driver.
- Diagnostics contain no credentials, pairing material, command text, or arbitrary command execution path.

---

### Task 1: Profile Contract And Red Tests

**Files:**
- Modify: `src/config.h`
- Modify: `src/config.cpp`
- Test: `tests/unit/test_display_device.cpp`
- Test: `tests/integration/test_config_consistency.cpp`

**Interfaces:**
- `config::second_monitor_t` contains `device_id`, `resolution`, `fps`, `hdr`, `capture_backend`, `stream_audio`, `display_policy`, and `restore_on_disconnect`.
- `config::parse_second_monitor_profile(const std::unordered_map<std::string, std::string> &)` returns `std::optional<second_monitor_t>` and rejects a partially specified profile, unsupported policy, malformed resolution, FPS outside 1..240, and unknown `second_monitor_*` keys.
- `second_monitor_t::enabled()` is true only for a non-empty `device_id` paired with the safe `vdd_only` policy.

- [ ] **Step 1: Add profile parsing tests before implementation.** Cover empty/default opt-out, missing device ID, valid 1280x720/30 VDD profile, `vdd_only` and `disabled` policy handling, invalid policy, invalid resolution/FPS, and an unknown profile key.
- [ ] **Step 2: Add config consistency expectations.** Assert every persisted `second_monitor_*` key is represented in the strict parser and that an arbitrary profile field is not accepted.
- [ ] **Step 3: Run the focused tests.** Run `ctest --test-dir build -R 'display_device|config_consistency' --output-on-failure`; when no build exists, record the exact missing executable/toolchain failure instead of claiming a pass.
- [ ] **Step 4: Implement the contract minimally.** Parse only the eight documented keys, use the stated defaults, require a device ID whenever any profile key is supplied, and leave unrelated config keys on the existing warning path.
- [ ] **Step 5: Apply an enabled profile at config load.** Copy the profile device ID, policy, manual resolution/FPS, HDR mode, capture backend, audio setting, and restore policy into the existing runtime settings; do nothing when disabled.
- [ ] **Step 6: Re-run the focused tests.** Verify profile parsing, defaults, strict profile-key rejection, and existing config consistency behavior.

### Task 2: Lifecycle Ownership And Red Tests

**Files:**
- Modify: `src/display_device.cpp`
- Modify: `src/display_device.h`
- Modify: `src/nvhttp.cpp`
- Modify: `src/stream.cpp`
- Test: `tests/unit/test_display_device.cpp`

**Interfaces:**
- `display_device::second_monitor_state_e` has `waiting_for_vdd`, `display_configuring`, `capturing`, `capture_reinitializing`, `display_restore_pending`, and `display_restore_failed`.
- `display_device::second_monitor_status_t` contains the state string, selected device ID, mapped output, capture backend, encoder, active width/height/FPS, and last reinitialization reason.
- `display_device::second_monitor_status()` returns a secret-free snapshot.
- `display_device::mark_capture_reinitializing(std::string reason)`, `mark_capture_started(...)`, `mark_session_started()`, and `mark_session_stopped(bool restore)` update ownership under the display-device mutex.

- [ ] **Step 1: Add lifecycle tests before implementation.** Cover missing VDD before launch, VDD appearing on a retry, failed launch cleanup, one restore after the final session, and no restore while a second session still owns the display.
- [ ] **Step 2: Run the lifecycle tests.** Run `ctest --test-dir build -R display_device --output-on-failure` and record the expected failure.
- [ ] **Step 3: Implement the synchronized state record.** Keep selected ID and mapped output from the existing VDD resolver, update reinitialization reason only from bounded display/capture events, count owners, and make restore transition to pending exactly once for the final owner.
- [ ] **Step 4: Make launch and reconnect retryable.** Before `configure_display`, mark `waiting_for_vdd` for an unavailable selected VDD and return the existing GameStream 503 response; on a later available enumeration, transition through `display_configuring` without changing the selected ID.
- [ ] **Step 5: Connect capture and cleanup hooks.** Mark capture after successful encoder setup, mark capture reinitialization on retry paths, and call final-session cleanup from `stream::session::join` while preserving normal app/session behavior.
- [ ] **Step 6: Handle restore outcomes.** Keep the existing libdisplaydevice retry scheduler; report `display_restore_failed` when restoration cannot be scheduled/applied and never execute a configured process command for restoration.
- [ ] **Step 7: Re-run focused lifecycle tests.** Verify state transitions and ownership semantics without requiring a real Windows display.

### Task 3: Diagnostics Endpoint And Selector UI

**Files:**
- Modify: `src/confighttp.cpp`
- Modify: `src_assets/common/assets/web/configs/tabs/audiovideo/DisplayOutputSelector.vue`
- Modify: `tests/integration/test_config_consistency.cpp`

**Interfaces:**
- `GET /api/second-monitor/status` uses the existing `authenticate()` path and returns JSON from `display_device::second_monitor_status()`.
- The response contains `state`, `selected_device_id`, `mapped_output`, `capture_backend`, `encoder`, `resolution`, `fps`, and `last_reinitialization_reason`, plus non-sensitive device-list metadata used by the selector.
- The selector reads the authenticated status/device data, displays friendly name, stable ID, active state, current resolution, and virtual-device status, and keeps manual GUID entry only as a fallback.

- [ ] **Step 1: Add endpoint/response assertions.** Verify the route is registered, authentication is required, and serialized output has no password, certificate, key, prep command, or arbitrary command fields.
- [ ] **Step 2: Implement the GET handler.** Authenticate, log the sanitized request with existing redaction, serialize the status snapshot with `send_response`, and register only the exact GET route.
- [ ] **Step 3: Implement the selector.** Preserve existing non-Windows fields, show a VDD selection list/status when virtual-device enumeration data is available, bind the selected stable ID to `second_monitor_device_id`, and use the existing output text input for regular Sunshine configuration.
- [ ] **Step 4: Re-run UI/config consistency checks.** Confirm the new keys and selector bindings remain discoverable by the existing integration test.

### Task 4: Verification And Commit

**Files:**
- Verify: all modified files above

- [ ] **Step 1: Run formatting/static checks.** Run `git diff --check` and the repository's available C++/Vue lint commands without modifying unrelated files.
- [ ] **Step 2: Run focused and full tests.** Run `ctest --test-dir build -R 'display_device|config_consistency' --output-on-failure` and `ctest --test-dir build --output-on-failure`; record unavailable MSYS2/CMake/CTest evidence explicitly.
- [ ] **Step 3: Review the diff.** Confirm no secrets, process commands, driver lifecycle calls, or regular-mode changes are present.
- [ ] **Step 4: Commit the implementation.** Use `git add` only for the intended Task 3 files and commit with `feat: add second monitor profiles and display diagnostics`.
- [ ] **Step 5: Write the external report.** Include status, commit hash, one-line test summary, and concerns at `C:\Users\0ae\Desktop\moonlight\.worktrees\second-monitor\.superpowers\sdd\2026-08-12-second-monitor\task-3-report.md`.
