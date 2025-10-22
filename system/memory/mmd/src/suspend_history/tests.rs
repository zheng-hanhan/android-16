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

use super::*;
use crate::time::MockTimeApi;
use crate::time::TIME_API_MTX;

const BASE_RAW_MONOTONIC_TIME: Duration = Duration::from_secs(12345);
const BASE_RAW_BOOT_TIME: Duration = Duration::from_secs(67890);
const HOUR_IN_SECONDS: u64 = 3600;
const DEFAULT_MAX_IDLE_DURATION: Duration = Duration::from_secs(25 * HOUR_IN_SECONDS);

#[test]
fn test_suspend_monitor() {
    let _m = TIME_API_MTX.lock();
    let mock_monitonic = MockTimeApi::get_monotonic_time_context();
    let mock_boot = MockTimeApi::get_boot_time_context();
    mock_monitonic
        .expect()
        .times(1)
        .return_const(MonotonicTime::from_duration(BASE_RAW_MONOTONIC_TIME));
    mock_boot.expect().times(1).return_const(BootTime::from_duration(BASE_RAW_BOOT_TIME));
    let mut suspend_monitor = SuspendMonitor::<MockTimeApi>::new();

    // + 100s on monotonic clock
    mock_monitonic.expect().times(1).return_const(MonotonicTime::from_duration(
        BASE_RAW_MONOTONIC_TIME + Duration::from_secs(100),
    ));
    // + 300s on boottime clock
    let boot_now = BootTime::from_duration(BASE_RAW_BOOT_TIME + Duration::from_secs(300));
    mock_boot.expect().times(1).return_const(boot_now);
    assert_eq!(suspend_monitor.generate_suspend_duration(), (Duration::from_secs(200), boot_now));

    // + 900s on monotonic clock
    mock_monitonic.expect().times(1).return_const(MonotonicTime::from_duration(
        BASE_RAW_MONOTONIC_TIME + Duration::from_secs(1000),
    ));
    // + 1000s on boottime clock
    let boot_now = BootTime::from_duration(BASE_RAW_BOOT_TIME + Duration::from_secs(1300));
    mock_boot.expect().times(1).return_const(boot_now);
    assert_eq!(suspend_monitor.generate_suspend_duration(), (Duration::from_secs(100), boot_now));
}

#[test]
fn test_suspend_monitor_negative_adjustment() {
    let _m = TIME_API_MTX.lock();
    let mock_monitonic = MockTimeApi::get_monotonic_time_context();
    let mock_boot = MockTimeApi::get_boot_time_context();
    mock_monitonic
        .expect()
        .times(1)
        .return_const(MonotonicTime::from_duration(BASE_RAW_MONOTONIC_TIME));
    mock_boot.expect().times(1).return_const(BootTime::from_duration(BASE_RAW_BOOT_TIME));
    let mut suspend_monitor = SuspendMonitor::<MockTimeApi>::new();

    // + 400s on monotonic clock
    mock_monitonic.expect().times(1).return_const(MonotonicTime::from_duration(
        BASE_RAW_MONOTONIC_TIME + Duration::from_secs(400),
    ));
    // + 100s on boottime clock
    let boot_now = BootTime::from_duration(BASE_RAW_BOOT_TIME + Duration::from_secs(100));
    mock_boot.expect().times(1).return_const(boot_now);
    // 300s of negative adjustment is stored.
    assert_eq!(suspend_monitor.generate_suspend_duration(), (Duration::ZERO, boot_now));

    // + 100s on monotonic clock
    mock_monitonic.expect().times(1).return_const(MonotonicTime::from_duration(
        BASE_RAW_MONOTONIC_TIME + Duration::from_secs(500),
    ));
    // + 1000s on boottime clock
    let boot_now = BootTime::from_duration(BASE_RAW_BOOT_TIME + Duration::from_secs(1100));
    mock_boot.expect().times(1).return_const(boot_now);
    // suspend duration is 900s - 300s (negative adjustment from the last call)
    assert_eq!(suspend_monitor.generate_suspend_duration(), (Duration::from_secs(600), boot_now));

    // + 100s on monotonic clock
    mock_monitonic.expect().times(1).return_const(MonotonicTime::from_duration(
        BASE_RAW_MONOTONIC_TIME + Duration::from_secs(600),
    ));
    // + 400s on boottime clock
    let boot_now = BootTime::from_duration(BASE_RAW_BOOT_TIME + Duration::from_secs(1500));
    mock_boot.expect().times(1).return_const(boot_now);
    // suspend duration is 300s without negative adjustment
    assert_eq!(suspend_monitor.generate_suspend_duration(), (Duration::from_secs(300), boot_now));
}

#[test]
fn test_suspend_monitor_big_negative_adjustment() {
    let _m = TIME_API_MTX.lock();
    let mock_monitonic = MockTimeApi::get_monotonic_time_context();
    let mock_boot = MockTimeApi::get_boot_time_context();
    mock_monitonic
        .expect()
        .times(1)
        .return_const(MonotonicTime::from_duration(BASE_RAW_MONOTONIC_TIME));
    mock_boot.expect().times(1).return_const(BootTime::from_duration(BASE_RAW_BOOT_TIME));
    let mut suspend_monitor = SuspendMonitor::<MockTimeApi>::new();

    // + 400s on monotonic clock
    mock_monitonic.expect().times(1).return_const(MonotonicTime::from_duration(
        BASE_RAW_MONOTONIC_TIME + Duration::from_secs(400),
    ));
    // + 100s on boottime clock
    let boot_now = BootTime::from_duration(BASE_RAW_BOOT_TIME + Duration::from_secs(100));
    mock_boot.expect().times(1).return_const(boot_now);
    // 300s of negative adjustment is stored.
    assert_eq!(suspend_monitor.generate_suspend_duration(), (Duration::ZERO, boot_now));

    // + 100s on monotonic clock
    mock_monitonic.expect().times(1).return_const(MonotonicTime::from_duration(
        BASE_RAW_MONOTONIC_TIME + Duration::from_secs(500),
    ));
    // + 300s on boottime clock
    let boot_now = BootTime::from_duration(BASE_RAW_BOOT_TIME + Duration::from_secs(400));
    mock_boot.expect().times(1).return_const(boot_now);
    // suspend duration is 200s is shorter than negative adjustment. 100s of negative adjustment is
    // left.
    assert_eq!(suspend_monitor.generate_suspend_duration(), (Duration::ZERO, boot_now));

    // + 100s on monotonic clock
    mock_monitonic.expect().times(1).return_const(MonotonicTime::from_duration(
        BASE_RAW_MONOTONIC_TIME + Duration::from_secs(600),
    ));
    // + 400s on boottime clock
    let boot_now = BootTime::from_duration(BASE_RAW_BOOT_TIME + Duration::from_secs(800));
    mock_boot.expect().times(1).return_const(boot_now);
    // suspend duration is 300s - 100s (negative adjustment).
    assert_eq!(suspend_monitor.generate_suspend_duration(), (Duration::from_secs(200), boot_now));
}

#[test]
fn test_calculate_total_suspend_duration() {
    let mut history = SuspendHistory::new();

    history.record_suspend_duration(
        Duration::from_secs(2 * HOUR_IN_SECONDS),
        BootTime::from_duration(BASE_RAW_BOOT_TIME),
        DEFAULT_MAX_IDLE_DURATION,
    );
    history.record_suspend_duration(
        Duration::from_secs(5 * HOUR_IN_SECONDS),
        BootTime::from_duration(BASE_RAW_BOOT_TIME + Duration::from_secs(7 * HOUR_IN_SECONDS)),
        DEFAULT_MAX_IDLE_DURATION,
    );
    history.record_suspend_duration(
        Duration::from_secs(2 * HOUR_IN_SECONDS),
        BootTime::from_duration(BASE_RAW_BOOT_TIME + Duration::from_secs(10 * HOUR_IN_SECONDS)),
        DEFAULT_MAX_IDLE_DURATION,
    );
    history.record_suspend_duration(
        Duration::from_secs(HOUR_IN_SECONDS),
        BootTime::from_duration(BASE_RAW_BOOT_TIME + Duration::from_secs(12 * HOUR_IN_SECONDS)),
        DEFAULT_MAX_IDLE_DURATION,
    );

    assert_eq!(
        history.calculate_total_suspend_duration(
            Duration::from_secs(4 * HOUR_IN_SECONDS),
            BootTime::from_duration(BASE_RAW_BOOT_TIME + Duration::from_secs(13 * HOUR_IN_SECONDS))
        ),
        Duration::from_secs(8 * HOUR_IN_SECONDS)
    );
}

#[test]
fn test_calculate_total_suspend_duration_empty() {
    let history = SuspendHistory::new();

    assert_eq!(
        history.calculate_total_suspend_duration(
            Duration::from_secs(4 * HOUR_IN_SECONDS),
            BootTime::from_duration(BASE_RAW_BOOT_TIME + Duration::from_secs(13 * HOUR_IN_SECONDS))
        ),
        Duration::ZERO
    );
}

#[test]
fn test_calculate_total_suspend_duration_negative_target() {
    let history = SuspendHistory::new();

    // now - target_duration is negative.
    assert_eq!(
        history.calculate_total_suspend_duration(
            Duration::from_secs(4 * HOUR_IN_SECONDS),
            BootTime::from_duration(Duration::from_secs(HOUR_IN_SECONDS))
        ),
        Duration::ZERO
    );
}

#[test]
fn test_suspend_history_gc_entries() {
    let max_idle_duration = Duration::from_secs(25 * HOUR_IN_SECONDS);
    let base_raw_boot_time: Duration = Duration::ZERO;
    let mut history = SuspendHistory::new();

    assert_eq!(history.history.len(), 1);

    // awake for 26 hours.
    history.record_suspend_duration(
        Duration::from_secs(HOUR_IN_SECONDS),
        BootTime::from_duration(base_raw_boot_time + Duration::from_secs(27 * HOUR_IN_SECONDS)),
        max_idle_duration,
    );
    // Does not pop entry if there was only 1 entry.
    assert_eq!(history.history.len(), 2);

    // awake for 1 hour.
    history.record_suspend_duration(
        Duration::from_secs(HOUR_IN_SECONDS),
        BootTime::from_duration(base_raw_boot_time + Duration::from_secs(29 * HOUR_IN_SECONDS)),
        max_idle_duration,
    );
    // The first entry is GC-ed.
    assert_eq!(history.history.len(), 2);

    // awake for 2 hour.
    history.record_suspend_duration(
        Duration::from_secs(HOUR_IN_SECONDS),
        BootTime::from_duration(base_raw_boot_time + Duration::from_secs(32 * HOUR_IN_SECONDS)),
        max_idle_duration,
    );
    assert_eq!(history.history.len(), 3);

    // awake for 10 hour.
    history.record_suspend_duration(
        Duration::from_secs(11 * HOUR_IN_SECONDS),
        BootTime::from_duration(base_raw_boot_time + Duration::from_secs(53 * HOUR_IN_SECONDS)),
        max_idle_duration,
    );
    assert_eq!(history.history.len(), 4);

    // awake for 20 hours.
    history.record_suspend_duration(
        Duration::from_secs(12 * HOUR_IN_SECONDS),
        BootTime::from_duration(base_raw_boot_time + Duration::from_secs(85 * HOUR_IN_SECONDS)),
        max_idle_duration,
    );
    // The entries except last 2 entries are GC-ed.
    assert_eq!(history.history.len(), 2);

    assert_eq!(
        history.calculate_total_suspend_duration(
            Duration::from_secs(25 * HOUR_IN_SECONDS),
            BootTime::from_duration(base_raw_boot_time + Duration::from_secs(85 * HOUR_IN_SECONDS))
        ),
        Duration::from_secs(23 * HOUR_IN_SECONDS)
    );
}
