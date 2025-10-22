// Copyright 2024, The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

//! `/sys/block/zram0/idle` marks idle pages based on boottime clock timestamp which keeps ticking
//! even while the device is suspended. This can end up marking relatively new pages as idle. For
//! example, when the threshold for idle page is 25 hours and the user suspends the device whole the
//! weekend (i.e. 2days), all pages in zram are marked as idle which is too aggressive.
//!
//! [SuspendHistory] mitigates the issue by adjusting the idle threshold by the actual duration of
//! the device is suspended because fixing the kernel to use monotonic clock instead of boottime
//! clock can break existing user space behavior.
//!
//! In this module, we don't use [std::time::Instant] because the Rust standard
//! library used in Android uses [libc::CLOCK_BOOTTIME] while the official Rust
//! standard library implementation uses [libc::CLOCK_MONOTONIC].

#[cfg(test)]
mod tests;

use std::collections::VecDeque;
use std::marker::PhantomData;
use std::time::Duration;

use crate::time::BootTime;
use crate::time::MonotonicTime;
use crate::time::TimeApi;

/// Estimates the suspend duration by comparing the elapsed times on monotonic clock and boot time
/// clock.
///
/// In Linux kernel, boot time is calculated as <monotonic time> + <boot time offset>. However the
/// kernel does not provides API to expose the boot time offset (The internal API is
/// `ktime_get_offs_boot_ns()`).
pub struct SuspendMonitor<T: TimeApi> {
    monitonic_time: MonotonicTime,
    boot_time: BootTime,
    negative_adjustment: Duration,
    _phantom_data: PhantomData<T>,
}

impl<T: TimeApi> SuspendMonitor<T> {
    /// Creates [SuspendMonitor].
    pub fn new() -> Self {
        Self {
            monitonic_time: T::get_monotonic_time(),
            boot_time: T::get_boot_time(),
            negative_adjustment: Duration::ZERO,
            _phantom_data: PhantomData,
        }
    }

    /// Estimate suspend duration by comparing the elapsed time between monotonic clock and boottime
    /// clock.
    ///
    /// This returns the estimated suspend duration and the boot time timestamp of now.
    pub fn generate_suspend_duration(&mut self) -> (Duration, BootTime) {
        let monotonic_time = T::get_monotonic_time();
        let boot_time = T::get_boot_time();

        let monotonic_diff = monotonic_time.saturating_duration_since(self.monitonic_time);
        let boot_diff = boot_time.saturating_duration_since(self.boot_time);

        let suspend_duration = if boot_diff < monotonic_diff {
            // Since kernel does not provide API to get both boot time and
            // monotonic time atomically, the elapsed time on monotonic time
            // can be longer than boot time. Store the diff in
            // negative_adjustment and adjust it at the next call.
            self.negative_adjustment =
                self.negative_adjustment.saturating_add(monotonic_diff - boot_diff);
            Duration::ZERO
        } else {
            let suspend_duration = boot_diff - monotonic_diff;
            if suspend_duration >= self.negative_adjustment {
                let negative_adjustment = self.negative_adjustment;
                self.negative_adjustment = Duration::ZERO;
                suspend_duration - negative_adjustment
            } else {
                self.negative_adjustment =
                    self.negative_adjustment.saturating_sub(suspend_duration);
                Duration::ZERO
            }
        };

        self.monitonic_time = monotonic_time;
        self.boot_time = boot_time;

        (suspend_duration, boot_time)
    }
}

impl<T: TimeApi> Default for SuspendMonitor<T> {
    fn default() -> Self {
        Self::new()
    }
}

struct Entry {
    suspend_duration: Duration,
    time: BootTime,
}

/// [SuspendHistory] tracks the duration of suspends.
///
/// The adjustment duration is calculated by [SuspendHistory::calculate_total_suspend_duration].
/// For example, if the idle threshold is 4 hours just after these usage log:
///
/// * User suspends 1 hours (A) and use the device for 2 hours and,
/// * User suspends 5 hours (B) and use the device for 1 hours and,
/// * User suspends 2 hours (C) and use the device for 1 hours and,
/// * User suspends 1 hours (D) and use the device for 1 hours
///
/// In this case, the threshold need to be adjusted by 8 hours (B + C + D).
///
/// ```
///                                                      now
/// log       : |-A-|     |----B----|   |--C--|   |-D-|   |
/// threshold :                            |---original---|
/// adjustment:        |----B----|--C--|-D-|
/// ```
///
/// SuspendHistory uses deque to store the suspend logs. Each entry is 32 bytes. mmd will add a
/// record every hour and evict obsolete records. At worst case, Even if a user uses the device only
/// 10 seconds per hour and device is in suspend for 59 min 50 seconds, the history consumes only
/// 281KiB (= 32 bytes * 25 hours / (10 seconds / 3600 seconds)). Traversing < 300KiB on each zram
/// maintenance is an acceptable cost.
pub struct SuspendHistory {
    history: VecDeque<Entry>,
    total_awake_duration: Duration,
}

impl SuspendHistory {
    /// Creates [SuspendHistory].
    pub fn new() -> Self {
        let mut history = VecDeque::new();
        history.push_front(Entry { suspend_duration: Duration::ZERO, time: BootTime::ZERO });
        Self { history, total_awake_duration: Duration::ZERO }
    }

    /// Add a record of suspended duration.
    ///
    /// This also evicts records which will exceeds `max_idle_duration`.
    pub fn record_suspend_duration(
        &mut self,
        suspend_duration: Duration,
        time: BootTime,
        max_idle_duration: Duration,
    ) {
        // self.history never be empty while expired entries are popped out in the following loop
        // because the loop pop one entry at a time only when self.history has at least 2 entries.
        assert!(!self.history.is_empty());
        let awake_duration = time
            .saturating_duration_since(self.history.front().expect("history is not empty").time)
            .saturating_sub(suspend_duration);
        self.total_awake_duration = self.total_awake_duration.saturating_add(awake_duration);

        while self.total_awake_duration > max_idle_duration && self.history.len() >= 2 {
            // The oldest entry must not None because the history had at least 2 entries.
            let oldest_wake_at = self.history.pop_back().expect("history is not empty").time;

            // oldest_entry must not None because the history had at least 2 entries.
            let oldest_entry = self.history.back().expect("history had at least 2 entries");
            let oldest_awake_duration = oldest_entry
                .time
                .saturating_duration_since(oldest_wake_at)
                .saturating_sub(oldest_entry.suspend_duration);
            self.total_awake_duration =
                self.total_awake_duration.saturating_sub(oldest_awake_duration);
        }

        self.history.push_front(Entry { suspend_duration, time });
    }

    /// Calculates total suspend duration which overlaps the target_idle_duration.
    ///
    /// See the comment of [SuspendHistory] for details.
    pub fn calculate_total_suspend_duration(
        &self,
        target_idle_duration: Duration,
        now: BootTime,
    ) -> Duration {
        let Some(target_time) = now.checked_sub(target_idle_duration) else {
            return Duration::ZERO;
        };

        let mut total_suspend_duration = Duration::ZERO;

        for entry in self.history.iter() {
            let Some(adjusted_target_time) = target_time.checked_sub(total_suspend_duration) else {
                break;
            };
            if entry.time > adjusted_target_time {
                total_suspend_duration =
                    total_suspend_duration.saturating_add(entry.suspend_duration);
            } else {
                break;
            }
        }

        total_suspend_duration
    }
}

impl Default for SuspendHistory {
    fn default() -> Self {
        Self::new()
    }
}
