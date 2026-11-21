/**
 * @file src/platform/linux/pairing_notifications.h
 * @brief D-Bus notifications for local pairing approval.
 */
#pragma once

#include <stop_token>

namespace platf {
  /**
   * @brief Display pending PIN requests on the desktop notification server.
   * @param stop Signals that the GameStream server is shutting down.
   */
  void run_pairing_notifications(std::stop_token stop);
}  // namespace platf
