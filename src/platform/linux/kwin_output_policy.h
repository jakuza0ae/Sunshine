/**
 * @file src/platform/linux/kwin_output_policy.h
 * @brief Selection policy for KWin outputs.
 */
#pragma once

#include <algorithm>
#include <ranges>
#include <string_view>

namespace kwin {
  /**
   * @brief Find an exact output, optionally refusing to fall back to the primary display.
   * @param outputs Enumerated outputs.
   * @param name Requested output name.
   * @param require_output Reject a missing or unspecified name when true.
   * @param output_name Projection that returns an enumerated output's name.
   * @return Selected iterator, or the end iterator if capture must be refused.
   */
  template<class Range, class Projection>
  auto select_output(Range &outputs, std::string_view name, bool require_output, Projection output_name) {
    if (!name.empty()) {
      auto match = std::ranges::find(outputs, name, output_name);
      if (match != std::ranges::end(outputs)) {
        return match;
      }
    }
    return require_output ? std::ranges::end(outputs) : std::ranges::begin(outputs);
  }
}
