/**
 * @file src/platform/windows/touch_scheduler.h
 * @brief Testable state helpers for Windows synthetic touch input.
 */
#pragma once

// standard includes
#include <array>
#include <bitset>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <utility>

// local includes
#include "src/task_pool.h"

namespace platf {
  /**
   * @brief Keepalive interval required by Windows synthetic pointer input.
   */
  inline constexpr auto touch_keepalive_interval = std::chrono::milliseconds {50};

  /**
   * @brief Tracks client pointer IDs independently of Windows pointer storage.
   */
  class touch_slot_tracker_t {
  public:
    static constexpr std::size_t max_slots = 10;

    /**
     * @brief Find an active pointer or claim the first free slot.
     * @param pointer_id Client pointer identifier.
     * @return Slot index, or std::nullopt when all slots are active.
     */
    std::optional<std::size_t> acquire(std::uint32_t pointer_id) {
      for (std::size_t i = 0; i < max_slots; ++i) {
        if (m_active[i] && m_pointer_ids[i] == pointer_id) {
          return i;
        }
      }

      for (std::size_t i = 0; i < max_slots; ++i) {
        if (!m_active[i]) {
          m_pointer_ids[i] = pointer_id;
          m_active[i] = true;
          return i;
        }
      }

      return std::nullopt;
    }

    /**
     * @brief Release a slot for pointer-ID reuse.
     * @param slot Slot index to release.
     */
    void release(std::size_t slot) {
      m_active.reset(slot);
      m_pointer_ids[slot] = 0;
    }

    /**
     * @brief Release every tracked slot.
     */
    void clear() {
      m_active.reset();
      m_pointer_ids.fill(0);
    }

    /**
     * @brief Check whether a slot is active.
     * @param slot Slot index to inspect.
     * @return True when the slot is active.
     */
    bool is_active(std::size_t slot) const {
      return m_active[slot];
    }

    /**
     * @brief Return the pointer ID stored in a slot.
     * @param slot Slot index to inspect.
     * @return Pointer ID, or zero for a released slot.
     */
    std::uint32_t pointer_id(std::size_t slot) const {
      return m_pointer_ids[slot];
    }

    /**
     * @brief Keep tracker slots aligned with compacted Windows pointer entries.
     * @param first First slot to swap.
     * @param second Second slot to swap.
     */
    void swap_slots(std::size_t first, std::size_t second) {
      std::swap(m_pointer_ids[first], m_pointer_ids[second]);
      const bool first_active = m_active[first];
      m_active[first] = m_active[second];
      m_active[second] = first_active;
    }

  private:
    std::array<std::uint32_t, max_slots> m_pointer_ids {};
    std::bitset<max_slots> m_active {};
  };

  /**
   * @brief Linearizable state machine for delayed synthetic pointer callbacks.
   */
  class touch_repeat_scheduler_t {
  public:
    using generation_t = std::uint64_t;
    using task_id_t = task_pool_util::TaskPool::task_id_t;

    /**
     * @brief Queue a callback under the current generation.
     * @param enqueue Non-blocking task enqueue operation receiving the generation.
     * @return Generation assigned to the callback, or std::nullopt when cancelled.
     * @details The production enqueue operation only inserts into TaskPool and cannot invoke
     * a callback synchronously, so holding this state lock across it closes the queue/cancel race.
     */
    template<typename Enqueue>
    std::optional<generation_t> queue(Enqueue &&enqueue) {
      return queue_impl(std::nullopt, std::forward<Enqueue>(enqueue));
    }

    /**
     * @brief Queue a callback only when its generation remains current.
     * @param generation Expected callback generation.
     * @param enqueue Non-blocking task enqueue operation receiving the generation.
     * @return Generation assigned to the callback, or std::nullopt when cancelled or stale.
     */
    template<typename Enqueue>
    std::optional<generation_t> queue(generation_t generation, Enqueue &&enqueue) {
      return queue_impl(generation, std::forward<Enqueue>(enqueue));
    }

    /**
     * @brief Enter a callback if its generation is still current.
     * @param generation Callback generation.
     * @return True when the callback may access its owner.
     */
    bool begin_callback(generation_t generation) {
      std::lock_guard lock(m_mutex);
      if (m_shutting_down || m_cancelling || generation != m_generation) {
        return false;
      }

      ++m_active_callbacks;
      return true;
    }

    /**
     * @brief Check whether a callback may enqueue its next keepalive callback.
     * @param generation Callback generation.
     * @return True when the generation remains active.
     */
    bool callback_may_reschedule(generation_t generation) const {
      std::lock_guard lock(m_mutex);
      return !m_shutting_down && !m_cancelling && generation == m_generation;
    }

    /**
     * @brief Leave a callback and wake a cancellation waiter.
     * @param generation Callback generation.
     */
    void finish_callback(generation_t /* generation */) {
      std::lock_guard lock(m_mutex);
      if (m_active_callbacks > 0) {
        --m_active_callbacks;
      }
      m_condition.notify_all();
    }

    /**
     * @brief Invalidate callbacks and wait for active callbacks to finish.
     * @param shutting_down Whether future callbacks must remain disabled permanently.
     * @return Queued task ID that the caller must remove from TaskPool.
     */
    task_id_t begin_cancel(bool shutting_down) {
      std::unique_lock lock(m_mutex);
      m_cancelling = true;
      ++m_generation;
      m_shutting_down = m_shutting_down || shutting_down;
      const auto task = m_task;
      m_task = nullptr;
      m_condition.wait(lock, [this]() {
        return m_active_callbacks == 0;
      });
      return task;
    }

    /**
     * @brief Re-enable non-shutdown scheduling after the queued task is cancelled.
     */
    void finish_cancel() {
      std::lock_guard lock(m_mutex);
      m_cancelling = false;
    }

    /**
     * @brief Check whether cancellation is in progress.
     * @return True while cancellation owns the scheduler transition.
     */
    bool is_cancelling() const {
      std::lock_guard lock(m_mutex);
      return m_cancelling;
    }

  private:
    template<typename Enqueue>
    std::optional<generation_t> queue_impl(std::optional<generation_t> expected_generation, Enqueue &&enqueue) {
      std::lock_guard lock(m_mutex);
      if (
        m_shutting_down || m_cancelling ||
        (expected_generation && *expected_generation != m_generation)
      ) {
        return std::nullopt;
      }

      const auto generation = m_generation;
      m_task = enqueue(generation);
      return generation;
    }

    mutable std::mutex m_mutex;
    std::condition_variable m_condition;
    bool m_shutting_down {};
    bool m_cancelling {};
    std::size_t m_active_callbacks {};
    generation_t m_generation {1};
    task_id_t m_task {};
  };
}  // namespace platf
