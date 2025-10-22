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

//! The time library of libmmd.
//!
//! libmmd cares about the type of clock is boot time or monotonic time to calculate suspend
//! duration of the system at `crate::suspend_history` module.
//!
//! In Android [std::time::Instant] is based on boot time clock unlike the official rust standard
//! library is based on monotonic clock. libmmd defines its own [BootTime] and [MonotonicTime]
//! explicitly and refrain from depending on [std::time::Instant].
//!
//! https://android.googlesource.com/toolchain/android_rust/+/refs/heads/main/patches/longterm/rustc-0018-Switch-Instant-to-use-CLOCK_BOOTTIME.patch

use std::time::Duration;

use nix::time::clock_gettime;

/// [TimeApi] is the mockable interface of clock_gettime(3).
#[cfg_attr(test, mockall::automock)]
pub trait TimeApi {
    /// Get the current monotonic time.
    fn get_monotonic_time() -> MonotonicTime;
    /// Get the current boot time.
    fn get_boot_time() -> BootTime;
}

/// The implementation of [TimeApi].
pub struct TimeApiImpl;

impl TimeApi for TimeApiImpl {
    fn get_monotonic_time() -> MonotonicTime {
        clock_gettime(nix::time::ClockId::CLOCK_MONOTONIC)
            .map(|t| MonotonicTime(t.into()))
            .expect("clock_gettime(CLOCK_MONOTONIC) never fails")
    }

    fn get_boot_time() -> BootTime {
        clock_gettime(nix::time::ClockId::CLOCK_BOOTTIME)
            .map(|t| BootTime(t.into()))
            .expect("clock_gettime(CLOCK_BOOTTIME) never fails")
    }
}

/// Mutex to synchronize tests using [MockTimeApi].
///
/// mockall for static functions requires synchronization.
///
/// https://docs.rs/mockall/latest/mockall/#static-methods
#[cfg(test)]
pub static TIME_API_MTX: std::sync::Mutex<()> = std::sync::Mutex::new(());

/// The representation of monotonic time.
#[derive(Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash, Default, Debug)]
pub struct MonotonicTime(Duration);

impl MonotonicTime {
    /// This returns the durtion elapsed from `earlier` time.
    ///
    /// This returns zero duration if `earlier` is later than this.
    pub fn saturating_duration_since(&self, earlier: Self) -> Duration {
        self.0.saturating_sub(earlier.0)
    }

    /// Creates [MonotonicTime] from [Duration].
    ///
    /// This will be mainly used for testing purpose. Otherwise, [TimeApiImpl::get_monotonic_time]
    /// is recommended.
    pub const fn from_duration(value: Duration) -> Self {
        Self(value)
    }
}

/// The representation of boot time.
#[derive(Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash, Default, Debug)]
pub struct BootTime(Duration);

impl BootTime {
    /// The zero boot time.
    pub const ZERO: BootTime = BootTime(Duration::ZERO);

    /// This returns the durtion elapsed from `earlier` time.
    ///
    /// This returns zero duration if `earlier` is later than this.
    pub fn saturating_duration_since(&self, earlier: Self) -> Duration {
        self.0.saturating_sub(earlier.0)
    }

    /// Returns the `BootTime` added by the duration.
    ///
    /// Returns `None` if overflow occurs.
    pub fn checked_add(&self, duration: Duration) -> Option<Self> {
        self.0.checked_add(duration).map(Self)
    }

    /// Returns the `BootTime` subtracted by the duration.
    ///
    /// Returns `None` if the duration is bigger than the boot time.
    pub fn checked_sub(&self, duration: Duration) -> Option<Self> {
        self.0.checked_sub(duration).map(Self)
    }

    /// Creates [BootTime] from [Duration].
    ///
    /// This will be mainly used for testing purpose. Otherwise, [TimeApiImpl::get_boot_time] is
    /// recommended.
    pub const fn from_duration(value: Duration) -> Self {
        Self(value)
    }
}
