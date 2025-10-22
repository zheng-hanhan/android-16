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

//! This module provides policy to manage zram writeback feature.
//!
//! See "writeback" section in the kernel document for details.
//!
//! https://www.kernel.org/doc/Documentation/blockdev/zram.txt

mod history;
#[cfg(test)]
mod tests;

use std::time::Duration;

use crate::os::get_page_size;
use crate::os::MeminfoApi;
use crate::suspend_history::SuspendHistory;
use crate::time::BootTime;
use crate::zram::idle::calculate_idle_time;
use crate::zram::idle::set_zram_idle_time;
use crate::zram::writeback::history::ZramWritebackHistory;
use crate::zram::SysfsZramApi;

/// Error from [ZramWriteback].
#[derive(Debug, thiserror::Error)]
pub enum Error {
    /// writeback too frequently
    #[error("writeback too frequently")]
    BackoffTime,
    /// no more space for zram writeback
    #[error("no pages in zram for zram writeback")]
    Limit,
    /// failed to parse writeback_limit
    #[error("failed to parse writeback_limit")]
    InvalidWritebackLimit,
    /// failure on setting zram idle
    #[error("calculate zram idle {0}")]
    CalculateIdle(#[from] crate::zram::idle::CalculateError),
    /// failure on setting zram idle
    #[error("set zram idle {0}")]
    MarkIdle(std::io::Error),
    /// failure on writing to /sys/block/zram0/writeback
    #[error("writeback: {0}")]
    Writeback(std::io::Error),
    /// failure on access to /sys/block/zram0/writeback_limit
    #[error("writeback_limit: {0}")]
    WritebackLimit(std::io::Error),
}

type Result<T> = std::result::Result<T, Error>;

/// Current zram writeback setup status
#[derive(Debug, PartialEq)]
pub enum ZramWritebackStatus {
    /// Zram writeback is not supported by the kernel.
    Unsupported,
    /// Zram writeback is supported but not configured yet.
    NotConfigured,
    /// Zram writeback was already activated.
    Activated,
}

/// Whether the zram writeback is activated on the device or not.
pub fn get_zram_writeback_status<Z: SysfsZramApi>() -> std::io::Result<ZramWritebackStatus> {
    match Z::read_backing_dev() {
        // If /sys/block/zram0/backing_dev is "none", zram writeback is not configured yet.
        Ok(backing_dev) => {
            if backing_dev.trim() == "none" {
                Ok(ZramWritebackStatus::NotConfigured)
            } else {
                Ok(ZramWritebackStatus::Activated)
            }
        }
        // If it can't access /sys/block/zram0/backing_dev, zram writeback feature is disabled on
        // the kernel.
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => Ok(ZramWritebackStatus::Unsupported),
        Err(e) => Err(e),
    }
}

/// The parameters for zram writeback.
pub struct Params {
    /// The backoff time since last writeback.
    pub backoff_duration: Duration,
    /// The minimum idle duration to writeback. This is used for [calculate_idle_time].
    pub min_idle: Duration,
    /// The maximum idle duration to writeback. This is used for [calculate_idle_time].
    pub max_idle: Duration,
    /// Whether writeback huge and idle pages or not.
    pub huge_idle: bool,
    /// Whether writeback idle pages or not.
    pub idle: bool,
    /// Whether writeback huge pages or not.
    pub huge: bool,
    /// Minimum bytes to writeback in 1 round.
    pub min_bytes: u64,
    /// Maximum bytes to writeback in 1 round.
    pub max_bytes: u64,
    /// Maximum bytes to writeback allowed in a day.
    pub max_bytes_per_day: u64,
}

impl Default for Params {
    fn default() -> Self {
        Self {
            // 10 minutes
            backoff_duration: Duration::from_secs(600),
            // 20 hours
            min_idle: Duration::from_secs(20 * 3600),
            // 25 hours
            max_idle: Duration::from_secs(25 * 3600),
            huge_idle: true,
            idle: true,
            huge: true,
            // 5 MiB
            min_bytes: 5 << 20,
            // 300 MiB
            max_bytes: 300 << 20,
            // 1 GiB
            max_bytes_per_day: 1 << 30,
        }
    }
}

/// The stats for zram writeback.
#[derive(Debug, Default)]
pub struct Stats {
    /// orig_data_size of [crate::zram::stats::ZramMmStat].
    pub orig_data_size: u64,
    /// bd_count_pages of [crate::zram::stats::ZramBdStat].
    pub current_writeback_pages: u64,
}

/// The detailed results of a zram writeback attempt.
#[derive(Debug, Default)]
pub struct WritebackDetails {
    /// WritebackModeDetails for huge idle pages.
    pub huge_idle: WritebackModeDetails,
    /// WritebackModeDetails for idle pages.
    pub idle: WritebackModeDetails,
    /// WritebackModeDetails for huge pages.
    pub huge: WritebackModeDetails,
    /// Calculated writeback limit pages.
    pub limit_pages: u64,
    /// Writeback daily limit pages. This is calculated from the total written
    /// back page per day.
    pub daily_limit_pages: u64,
    /// The content of /sys/block/zram0/writeback_limit just before starting
    /// zram writeback. This is usually equals to the smaller of limit_pages and
    /// daily_limit_pages unless kernel tweaks the updated writeback_limit
    /// value.
    pub actual_limit_pages: u64,
}

/// The detailed results of a zram writeback attempt per zram page type (i.e.
/// huge_idle, idle, huge pages).
#[derive(Debug, Default)]
pub struct WritebackModeDetails {
    /// Number of pages written back.
    pub written_pages: u64,
}

enum Mode {
    HugeIdle,
    Idle,
    Huge,
}

fn load_current_writeback_limit<Z: SysfsZramApi>() -> Result<u64> {
    let contents = Z::read_writeback_limit().map_err(Error::WritebackLimit)?;
    contents.trim().parse().map_err(|_| Error::InvalidWritebackLimit)
}

/// ZramWriteback manages zram writeback policies.
pub struct ZramWriteback {
    history: ZramWritebackHistory,
    last_writeback_at: Option<BootTime>,
    total_zram_pages: u64,
    zram_writeback_pages: u64,
    page_size: u64,
}

impl ZramWriteback {
    /// Creates a new [ZramWriteback].
    pub fn new(total_zram_size: u64, zram_writeback_size: u64) -> Self {
        Self::new_with_page_size(total_zram_size, zram_writeback_size, get_page_size())
    }

    /// Creates a new [ZramWriteback] with a specified page size.
    pub fn new_with_page_size(
        total_zram_size: u64,
        zram_writeback_size: u64,
        page_size: u64,
    ) -> Self {
        assert!(page_size != 0);
        let total_zram_pages = total_zram_size / page_size;
        let zram_writeback_pages = zram_writeback_size / page_size;
        assert!(total_zram_pages != 0);
        Self {
            history: ZramWritebackHistory::new(),
            last_writeback_at: None,
            total_zram_pages,
            zram_writeback_pages,
            page_size,
        }
    }

    /// Writes back idle or huge zram pages to disk.
    pub fn mark_and_flush_pages<Z: SysfsZramApi, M: MeminfoApi>(
        &mut self,
        params: &Params,
        stats: &Stats,
        suspend_history: &SuspendHistory,
        now: BootTime,
    ) -> Result<WritebackDetails> {
        if let Some(last_at) = self.last_writeback_at {
            if now.saturating_duration_since(last_at) < params.backoff_duration {
                return Err(Error::BackoffTime);
            }
        }

        self.history.cleanup(now);
        let daily_limit_pages =
            self.history.calculate_daily_limit(params.max_bytes_per_day / self.page_size, now);
        let limit_pages = self.calculate_writeback_limit(params, stats);
        let mut details = WritebackDetails { limit_pages, daily_limit_pages, ..Default::default() };

        let limit_pages = std::cmp::min(limit_pages, daily_limit_pages);
        if limit_pages == 0 {
            return Err(Error::Limit);
        }
        Z::write_writeback_limit(&limit_pages.to_string()).map_err(Error::WritebackLimit)?;
        let mut writeback_limit = load_current_writeback_limit::<Z>()?;
        details.actual_limit_pages = writeback_limit;

        if params.huge_idle && writeback_limit > 0 {
            writeback_limit = self.writeback::<Z, M>(
                writeback_limit,
                params,
                Mode::HugeIdle,
                suspend_history,
                &mut details.huge_idle,
                now,
            )?;
        }
        if params.idle && writeback_limit > 0 {
            writeback_limit = self.writeback::<Z, M>(
                writeback_limit,
                params,
                Mode::Idle,
                suspend_history,
                &mut details.idle,
                now,
            )?;
        }
        if params.huge && writeback_limit > 0 {
            self.writeback::<Z, M>(
                writeback_limit,
                params,
                Mode::Huge,
                suspend_history,
                &mut details.huge,
                now,
            )?;
        }

        Ok(details)
    }

    fn calculate_writeback_limit(&self, params: &Params, stats: &Stats) -> u64 {
        let min_pages = params.min_bytes / self.page_size;
        let max_pages = params.max_bytes / self.page_size;
        // All calculations are performed in basis points, 100 bps = 1.00%. The number of pages
        // allowed to be written back follows a simple linear relationship. The allowable range is
        // [min_pages, max_pages], and the writeback limit will be the (zram utilization) * the
        // range, that is, the more zram we're using the more we're going to allow to be written
        // back.
        const BPS: u64 = 100 * 100;
        let zram_utilization_bps =
            stats.orig_data_size / self.page_size * BPS / self.total_zram_pages;
        let limit_pages = zram_utilization_bps * max_pages / BPS;

        // And try to limit it to the approximate number of free backing device pages (if it's
        // less).
        let free_bd_pages = self.zram_writeback_pages - stats.current_writeback_pages;
        let limit_pages = std::cmp::min(limit_pages, free_bd_pages);

        if limit_pages < min_pages {
            // Configured to not writeback fewer than configured min_pages.
            return 0;
        }

        // Finally enforce the limits, we won't even attempt writeback if we cannot writeback at
        // least the min, and we will cap to the max if it's greater.
        std::cmp::min(limit_pages, max_pages)
    }

    fn writeback<Z: SysfsZramApi, M: MeminfoApi>(
        &mut self,
        writeback_limit: u64,
        params: &Params,
        mode: Mode,
        suspend_history: &SuspendHistory,
        details: &mut WritebackModeDetails,
        now: BootTime,
    ) -> Result<u64> {
        match mode {
            Mode::HugeIdle | Mode::Idle => {
                let idle_age = calculate_idle_time::<M>(params.min_idle, params.max_idle)?;
                // Adjust idle age by suspend duration.
                let idle_age = idle_age.saturating_add(
                    suspend_history.calculate_total_suspend_duration(idle_age, now),
                );
                set_zram_idle_time::<Z>(idle_age).map_err(Error::MarkIdle)?;
            }
            Mode::Huge => {}
        }

        let mode = match mode {
            Mode::HugeIdle => "huge_idle",
            Mode::Idle => "idle",
            Mode::Huge => "huge",
        };

        let result = Z::writeback(mode);

        // If reading writeback_limit fails, we assume that all writeback_limit was consumed
        // conservatively.
        let current_writeback_limit = load_current_writeback_limit::<Z>().unwrap_or(0);
        let pages_written = writeback_limit.saturating_sub(current_writeback_limit);
        self.history.record(pages_written, now);
        details.written_pages = pages_written;

        if let Err(e) = result {
            // When zram writeback reaches the writeback_limit, kernel stop writing back and returns
            // EIO. EIO with zero writeback_limit is not an error.
            let is_writeback_limit_reached =
                e.raw_os_error() == Some(libc::EIO) && current_writeback_limit == 0;
            if !is_writeback_limit_reached {
                return Err(Error::Writeback(e));
            }
        }

        self.last_writeback_at = Some(now);

        Ok(current_writeback_limit)
    }
}
