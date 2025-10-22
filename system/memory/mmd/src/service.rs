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

use std::ops::DerefMut;
use std::sync::Arc;
use std::sync::Mutex;
use std::time::Instant;

use anyhow::Context;
use binder::Interface;
use binder::Result as BinderResult;
use log::error;
use log::info;
use mmd::os::MeminfoApiImpl;
use mmd::suspend_history::SuspendHistory;
use mmd::time::TimeApi;
use mmd::time::TimeApiImpl;
use mmd::zram::recompression::Error as ZramRecompressionError;
use mmd::zram::recompression::ZramRecompression;
use mmd::zram::writeback::Error as ZramWritebackError;
use mmd::zram::writeback::ZramWriteback;
use mmd::zram::SysfsZramApiImpl;
use mmd_aidl_interface::aidl::android::os::IMmd::IMmd;
use statslog_rust::zram_maintenance_executed::ZramMaintenanceExecuted;

use crate::atom::create_default_maintenance_atom;
use crate::atom::update_recompress_metrics;
use crate::atom::update_writeback_metrics;
use crate::properties::BoolProp;
use crate::properties::SecondsProp;
use crate::properties::U64Prop;
use crate::ZramContext;
use crate::DEFAULT_ZRAM_RECOMPRESSION_ENABLED;
use crate::DEFAULT_ZRAM_WRITEBACK_ENABLED;

pub struct MmdService {
    ctx: Arc<Mutex<ZramContext>>,
}

impl MmdService {
    pub fn new(ctx: Arc<Mutex<ZramContext>>) -> Self {
        Self { ctx }
    }
}

impl Interface for MmdService {}

impl IMmd for MmdService {
    fn doZramMaintenanceAsync(&self) -> BinderResult<()> {
        let mut atom = create_default_maintenance_atom();
        let mut ctx = self.ctx.lock().expect("mmd aborts on panics");

        let now = Instant::now();
        atom.interval_from_previous_seconds =
            now.duration_since(ctx.last_maintenance_at).as_secs().try_into().unwrap_or(i64::MAX);
        ctx.last_maintenance_at = now;

        let ZramContext { zram_writeback, zram_recompression, suspend_history, .. } =
            ctx.deref_mut();

        // Execute writeback before recompression. Current kernel decompresses
        // pages in zram before writing it back to disk.
        if BoolProp::ZramWritebackEnabled.get(DEFAULT_ZRAM_WRITEBACK_ENABLED) {
            if let Some(zram_writeback) = zram_writeback.as_mut() {
                handle_zram_writeback(zram_writeback, suspend_history, &mut atom);
            }
        }

        if BoolProp::ZramRecompressionEnabled.get(DEFAULT_ZRAM_RECOMPRESSION_ENABLED) {
            if let Some(zram_recompression) = zram_recompression.as_mut() {
                handle_zram_recompression(zram_recompression, suspend_history, &mut atom);
            }
        }

        if let Err(e) = atom.stats_write() {
            error!("failed to submit ZramMaintenanceExecuted atom: {e:?}");
        }

        Ok(())
    }

    fn isZramMaintenanceSupported(&self) -> BinderResult<bool> {
        let ctx = self.ctx.lock().expect("mmd aborts on panics");
        Ok(ctx.zram_writeback.is_some() || ctx.zram_recompression.is_some())
    }
}

fn handle_zram_recompression(
    zram_recompression: &mut ZramRecompression,
    suspend_history: &SuspendHistory,
    atom: &mut ZramMaintenanceExecuted,
) {
    let params = load_zram_recompression_params();

    let start = Instant::now();
    let result = zram_recompression.mark_and_recompress::<SysfsZramApiImpl, MeminfoApiImpl>(
        &params,
        suspend_history,
        TimeApiImpl::get_boot_time(),
    );
    atom.recompress_latency_millis = start.elapsed().as_millis().try_into().unwrap_or(i64::MAX);

    update_recompress_metrics(atom, &result);

    match result {
        Ok(_) | Err(ZramRecompressionError::BackoffTime) => {}
        Err(e) => error!("failed to zram recompress: {e:?}"),
    }
}

fn handle_zram_writeback(
    zram_writeback: &mut ZramWriteback,
    suspend_history: &SuspendHistory,
    atom: &mut ZramMaintenanceExecuted,
) {
    let params = load_zram_writeback_params();
    let stats = match load_zram_writeback_stats() {
        Ok(v) => v,
        Err(e) => {
            error!("failed to load zram writeback stats: {e:?}");
            atom.writeback_result =
                statslog_rust::zram_maintenance_executed::WritebackResult::WritebackLoadStatsFail;
            return;
        }
    };

    let start = Instant::now();
    let result = zram_writeback.mark_and_flush_pages::<SysfsZramApiImpl, MeminfoApiImpl>(
        &params,
        &stats,
        suspend_history,
        TimeApiImpl::get_boot_time(),
    );
    atom.writeback_latency_millis = start.elapsed().as_millis().try_into().unwrap_or(i64::MAX);

    update_writeback_metrics(atom, &result);

    match result {
        Ok(details) => {
            let total_written_pages = details
                .huge_idle
                .written_pages
                .saturating_add(details.idle.written_pages)
                .saturating_add(details.huge.written_pages);
            if total_written_pages > 0 {
                info!(
                    "zram writeback: huge_idle: {} pages, idle: {} pages, huge: {} pages",
                    details.huge_idle.written_pages,
                    details.idle.written_pages,
                    details.huge.written_pages
                );
            }
        }
        Err(ZramWritebackError::BackoffTime) | Err(ZramWritebackError::Limit) => {}
        Err(e) => error!("failed to zram writeback: {e:?}"),
    }
}

fn load_zram_writeback_params() -> mmd::zram::writeback::Params {
    let mut params = mmd::zram::writeback::Params::default();
    params.backoff_duration = SecondsProp::ZramWritebackBackoff.get(params.backoff_duration);
    params.min_idle = SecondsProp::ZramWritebackMinIdle.get(params.min_idle);
    params.max_idle = SecondsProp::ZramWritebackMaxIdle.get(params.max_idle);
    params.huge_idle = BoolProp::ZramWritebackHugeIdleEnabled.get(params.huge_idle);
    params.idle = BoolProp::ZramWritebackIdleEnabled.get(params.idle);
    params.huge = BoolProp::ZramWritebackHugeEnabled.get(params.huge);
    params.min_bytes = U64Prop::ZramWritebackMinBytes.get(params.min_bytes);
    params.max_bytes = U64Prop::ZramWritebackMaxBytes.get(params.max_bytes);
    params.max_bytes_per_day = U64Prop::ZramWritebackMaxBytesPerDay.get(params.max_bytes_per_day);
    params
}

fn load_zram_writeback_stats() -> anyhow::Result<mmd::zram::writeback::Stats> {
    let mm_stat =
        mmd::zram::stats::ZramMmStat::load::<SysfsZramApiImpl>().context("load mm_stat")?;
    let bd_stat =
        mmd::zram::stats::ZramBdStat::load::<SysfsZramApiImpl>().context("load bd_stat")?;
    Ok(mmd::zram::writeback::Stats {
        orig_data_size: mm_stat.orig_data_size,
        current_writeback_pages: bd_stat.bd_count_pages,
    })
}

fn load_zram_recompression_params() -> mmd::zram::recompression::Params {
    let mut params = mmd::zram::recompression::Params::default();
    params.backoff_duration = SecondsProp::ZramRecompressionBackoff.get(params.backoff_duration);
    params.min_idle = SecondsProp::ZramRecompressionMinIdle.get(params.min_idle);
    params.max_idle = SecondsProp::ZramRecompressionMaxIdle.get(params.max_idle);
    params.huge_idle = BoolProp::ZramRecompressionHugeIdleEnabled.get(params.huge_idle);
    params.idle = BoolProp::ZramRecompressionIdleEnabled.get(params.idle);
    params.huge = BoolProp::ZramRecompressionHugeEnabled.get(params.huge);
    params.threshold_bytes = U64Prop::ZramRecompressionThresholdBytes.get(params.threshold_bytes);
    params
}

#[cfg(test)]
mod tests {
    use mmd::suspend_history::SuspendHistory;
    use mmd::zram::recompression::ZramRecompression;
    use mmd::zram::writeback::ZramWriteback;

    use super::*;

    #[test]
    fn test_is_zram_maintenance_supported() {
        assert!(!MmdService::new(Arc::new(Mutex::new(ZramContext {
            zram_writeback: None,
            zram_recompression: None,
            suspend_history: SuspendHistory::new(),
            last_maintenance_at: Instant::now(),
        })))
        .isZramMaintenanceSupported()
        .unwrap());
        assert!(MmdService::new(Arc::new(Mutex::new(ZramContext {
            zram_writeback: Some(ZramWriteback::new(1024 * 1024, 1024 * 1024)),
            zram_recompression: None,
            suspend_history: SuspendHistory::new(),
            last_maintenance_at: Instant::now(),
        })))
        .isZramMaintenanceSupported()
        .unwrap());
        assert!(MmdService::new(Arc::new(Mutex::new(ZramContext {
            zram_writeback: None,
            zram_recompression: Some(ZramRecompression::new()),
            suspend_history: SuspendHistory::new(),
            last_maintenance_at: Instant::now(),
        })))
        .isZramMaintenanceSupported()
        .unwrap());
        assert!(MmdService::new(Arc::new(Mutex::new(ZramContext {
            zram_writeback: Some(ZramWriteback::new(1024 * 1024, 1024 * 1024)),
            zram_recompression: Some(ZramRecompression::new()),
            suspend_history: SuspendHistory::new(),
            last_maintenance_at: Instant::now(),
        })))
        .isZramMaintenanceSupported()
        .unwrap());
    }
}
