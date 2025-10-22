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

use std::collections::VecDeque;
use std::time::Duration;

use crate::time::BootTime;

// 24 hours.
const HISTORY_EXPIRY: Duration = Duration::from_secs(60 * 60 * 24);

/// Stores the log of zram writeback size to calculate daily limit.
pub struct ZramWritebackHistory {
    history: VecDeque<(u64, BootTime)>,
}

impl ZramWritebackHistory {
    /// Creates a new [ZramWritebackHistory].
    pub fn new() -> Self {
        Self { history: VecDeque::new() }
    }

    /// Records a new log of zram writeback.
    pub fn record(&mut self, pages: u64, now: BootTime) {
        self.history.push_back((pages, now));
    }

    /// Evicts expired records.
    pub fn cleanup(&mut self, now: BootTime) {
        while !self.history.is_empty()
            && now.saturating_duration_since(self.history.front().unwrap().1) > HISTORY_EXPIRY
        {
            self.history.pop_front();
        }
    }

    /// Calculates the daily limit of zram writeback left.
    pub fn calculate_daily_limit(&self, max_pages_per_day: u64, now: BootTime) -> u64 {
        let pages_written = self
            .history
            .iter()
            .filter(|(_, t)| now.saturating_duration_since(*t) < HISTORY_EXPIRY)
            .map(|(p, _)| p)
            .sum::<u64>();
        if pages_written >= max_pages_per_day {
            return 0;
        }
        max_pages_per_day - pages_written
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::time::TimeApi;
    use crate::time::TimeApiImpl;

    #[test]
    fn test_calculate_daily_limit() {
        let mut history = ZramWritebackHistory::new();
        let base_time = TimeApiImpl::get_boot_time();

        // records 1 day before is ignored.
        history.record(1, base_time);
        history.record(1, base_time);
        history.record(2, base_time.checked_add(Duration::from_secs(1)).unwrap());
        history.record(3, base_time.checked_add(HISTORY_EXPIRY).unwrap());
        assert_eq!(
            history.calculate_daily_limit(100, base_time.checked_add(HISTORY_EXPIRY).unwrap()),
            95
        );
    }

    #[test]
    fn test_calculate_daily_limit_empty() {
        let history = ZramWritebackHistory::new();
        assert_eq!(history.calculate_daily_limit(100, TimeApiImpl::get_boot_time()), 100);
    }

    #[test]
    fn test_calculate_daily_limit_exceeds_max() {
        let mut history = ZramWritebackHistory::new();
        let base_time = TimeApiImpl::get_boot_time();
        // records 1 day before is ignored.
        history.record(1, base_time);
        history.record(2, base_time.checked_add(Duration::from_secs(1)).unwrap());
        history.record(3, base_time.checked_add(HISTORY_EXPIRY).unwrap());

        assert_eq!(
            history.calculate_daily_limit(1, base_time.checked_add(HISTORY_EXPIRY).unwrap()),
            0
        );
        assert_eq!(
            history.calculate_daily_limit(2, base_time.checked_add(HISTORY_EXPIRY).unwrap()),
            0
        );
        assert_eq!(
            history.calculate_daily_limit(3, base_time.checked_add(HISTORY_EXPIRY).unwrap()),
            0
        );
        assert_eq!(
            history.calculate_daily_limit(4, base_time.checked_add(HISTORY_EXPIRY).unwrap()),
            0
        );
        assert_eq!(
            history.calculate_daily_limit(5, base_time.checked_add(HISTORY_EXPIRY).unwrap()),
            0
        );
        assert_eq!(
            history.calculate_daily_limit(6, base_time.checked_add(HISTORY_EXPIRY).unwrap()),
            1
        );
    }

    #[test]
    fn test_calculate_daily_limit_after_cleanup() {
        let mut history = ZramWritebackHistory::new();
        let base_time = TimeApiImpl::get_boot_time();
        // records 1 day before will be cleaned up.
        history.record(1, base_time);
        history.record(1, base_time);
        history.record(2, base_time.checked_add(Duration::from_secs(1)).unwrap());
        history.record(3, base_time.checked_add(HISTORY_EXPIRY).unwrap());

        history.cleanup(base_time.checked_add(HISTORY_EXPIRY).unwrap());

        // The same result as test_calculate_daily_limit
        assert_eq!(
            history.calculate_daily_limit(100, base_time.checked_add(HISTORY_EXPIRY).unwrap()),
            95
        );
    }
}
