// Copyright 2025, The Android Open Source Project
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

//! This module provides utilities to report Atoms for mmd managed resources.

use log::error;
use mmd::os::get_page_size;
use mmd::zram::recompression::Error as ZramRecompressionError;
use mmd::zram::setup::ZramActivationError;
use mmd::zram::stats::ZramBdStat;
use mmd::zram::stats::ZramMmStat;
use mmd::zram::writeback::Error as ZramWritebackError;
use mmd::zram::writeback::WritebackDetails;
use mmd::zram::SysfsZramApi;
use mmd::zram::SysfsZramApiImpl;
use statslog_rust::zram_bd_stat_mmd::ZramBdStatMmd;
use statslog_rust::zram_maintenance_executed::RecompressionResult;
use statslog_rust::zram_maintenance_executed::WritebackResult;
use statslog_rust::zram_maintenance_executed::ZramMaintenanceExecuted;
use statslog_rust::zram_mm_stat_mmd::ZramMmStatMmd;
use statslog_rust::zram_setup_executed::CompAlgorithmSetupResult;
use statslog_rust::zram_setup_executed::RecompressionSetupResult;
use statslog_rust::zram_setup_executed::WritebackSetupResult;
use statslog_rust::zram_setup_executed::ZramSetupExecuted;
use statslog_rust::zram_setup_executed::ZramSetupResult;
use statspull_rust::StatsPullResult;

const KB: u64 = 1024;

/// Converts u64 number to i64
///
/// If the value is more than i64::MAX, i64::MAX is returned.
fn u64_to_i64(v: u64) -> i64 {
    // The try_into() conversion fails only if the value is more than i64::MAX.
    v.try_into().unwrap_or(i64::MAX)
}

/// Create the default ZramMaintenanceExecuted
pub fn create_default_maintenance_atom() -> ZramMaintenanceExecuted {
    ZramMaintenanceExecuted {
        writeback_result: WritebackResult::WritebackNotSupported,
        writeback_huge_idle_pages: 0,
        writeback_huge_pages: 0,
        writeback_idle_pages: 0,
        writeback_latency_millis: 0,
        writeback_limit_kb: 0,
        writeback_daily_limit_kb: 0,
        writeback_actual_limit_kb: 0,
        writeback_total_kb: 0,
        recompression_result: RecompressionResult::RecompressionNotSupported,
        recompress_latency_millis: 0,
        interval_from_previous_seconds: 0,
    }
}

/// Update [ZramMaintenanceExecuted] based on the result of zram writeback.
pub fn update_writeback_metrics(
    atom: &mut ZramMaintenanceExecuted,
    result: &Result<WritebackDetails, ZramWritebackError>,
) {
    atom.writeback_result = match result {
        Ok(_) => WritebackResult::WritebackSuccess,
        Err(ZramWritebackError::BackoffTime) => WritebackResult::WritebackBackoffTime,
        Err(ZramWritebackError::Limit) => WritebackResult::WritebackLimit,
        Err(ZramWritebackError::InvalidWritebackLimit) => WritebackResult::WritebackInvalidLimit,
        Err(ZramWritebackError::CalculateIdle(_)) => WritebackResult::WritebackCalculateIdleFail,
        Err(ZramWritebackError::MarkIdle(_)) => WritebackResult::WritebackMarkIdleFail,
        Err(ZramWritebackError::Writeback(_)) => WritebackResult::WritebackTriggerFail,
        Err(ZramWritebackError::WritebackLimit(_)) => {
            WritebackResult::WritebackAccessWritebackLimitFail
        }
    };

    if let Ok(details) = result {
        let kb_per_page = get_page_size() / KB;
        atom.writeback_huge_idle_pages = u64_to_i64(details.huge_idle.written_pages);
        atom.writeback_idle_pages = u64_to_i64(details.idle.written_pages);
        atom.writeback_huge_pages = u64_to_i64(details.huge.written_pages);
        atom.writeback_limit_kb = u64_to_i64(details.limit_pages.saturating_mul(kb_per_page));
        atom.writeback_daily_limit_kb =
            u64_to_i64(details.daily_limit_pages.saturating_mul(kb_per_page));
        atom.writeback_actual_limit_kb =
            u64_to_i64(details.actual_limit_pages.saturating_mul(kb_per_page));
        atom.writeback_total_kb = u64_to_i64(
            (details
                .huge_idle
                .written_pages
                .saturating_add(details.idle.written_pages)
                .saturating_add(details.huge.written_pages))
            .saturating_mul(kb_per_page),
        );
    }
}

/// Update [ZramMaintenanceExecuted] based on the result of zram recompression.
pub fn update_recompress_metrics(
    atom: &mut ZramMaintenanceExecuted,
    result: &Result<(), ZramRecompressionError>,
) {
    atom.recompression_result = match result {
        Ok(_) => RecompressionResult::RecompressionSuccess,
        Err(ZramRecompressionError::BackoffTime) => RecompressionResult::RecompressionBackoffTime,
        Err(ZramRecompressionError::CalculateIdle(_)) => {
            RecompressionResult::RecompressionCalculateIdleFail
        }
        Err(ZramRecompressionError::MarkIdle(_)) => RecompressionResult::RecompressionMarkIdleFail,
        Err(ZramRecompressionError::Recompress(_)) => RecompressionResult::RecompressionTriggerFail,
    };
}

/// Reports ZramMmStatMmd atom.
pub fn report_zram_mm_stat() -> StatsPullResult {
    match generate_zram_mm_stat_atom::<SysfsZramApiImpl>() {
        Ok(atom) => vec![Box::new(atom)],
        Err(e) => {
            error!("failed to load mm stat atom: {:?}", e);
            vec![]
        }
    }
}

fn generate_zram_mm_stat_atom<Z: SysfsZramApi>() -> Result<ZramMmStatMmd, mmd::zram::stats::Error> {
    let stat = ZramMmStat::load::<Z>()?;
    let kb_per_page = get_page_size() / KB;
    Ok(ZramMmStatMmd {
        orig_data_kb: u64_to_i64(stat.orig_data_size / KB),
        compr_data_kb: u64_to_i64(stat.compr_data_size / KB),
        mem_used_total_kb: u64_to_i64(stat.mem_used_total / KB),
        mem_limit_kb: (stat.mem_limit / (KB as u32)).into(),
        mem_used_max_kb: stat.mem_used_max / (KB as i64),
        same_pages_kb: u64_to_i64(stat.same_pages.saturating_mul(kb_per_page)),
        pages_compacted_kb: (stat.pages_compacted as i64).saturating_mul(kb_per_page as i64),
        huge_pages_kb: u64_to_i64(stat.huge_pages.unwrap_or(0).saturating_mul(kb_per_page)),
        huge_pages_since_kb: u64_to_i64(
            stat.huge_pages_since.unwrap_or(0).saturating_mul(kb_per_page),
        ),
    })
}

/// Reports ZramBdStatMmd atom.
pub fn report_zram_bd_stat() -> StatsPullResult {
    match generate_zram_bd_stat_atom::<SysfsZramApiImpl>() {
        Ok(atom) => vec![Box::new(atom)],
        Err(e) => {
            error!("failed to load bd stat atom: {:?}", e);
            vec![]
        }
    }
}

fn generate_zram_bd_stat_atom<Z: SysfsZramApi>() -> Result<ZramBdStatMmd, mmd::zram::stats::Error> {
    let stat = ZramBdStat::load::<Z>()?;
    let kb_per_page = get_page_size() / KB;
    Ok(ZramBdStatMmd {
        bd_count_kb: u64_to_i64(stat.bd_count_pages.saturating_mul(kb_per_page)),
        bd_reads_kb: u64_to_i64(stat.bd_reads_pages.saturating_mul(kb_per_page)),
        bd_writes_kb: u64_to_i64(stat.bd_writes_pages.saturating_mul(kb_per_page)),
    })
}

/// Update [ZramSetupExecuted] based on the result of zram activation.
pub fn update_zram_setup_metrics(
    atom: &mut ZramSetupExecuted,
    result: &Result<(), ZramActivationError>,
) {
    atom.zram_setup_result = match result {
        Ok(_) => ZramSetupResult::ZramSetupSuccess,
        Err(ZramActivationError::UpdateZramDiskSize(_)) => {
            ZramSetupResult::ZramSetupUpdateDiskSizeFail
        }
        Err(ZramActivationError::SwapOn(_)) => ZramSetupResult::ZramSetupSwapOnFail,
        Err(ZramActivationError::ExecuteMkSwap(_)) | Err(ZramActivationError::MkSwap(_)) => {
            ZramSetupResult::ZramSetupMkSwapFail
        }
    };
}

/// Create the default ZramSetupExecuted atom.
pub fn create_default_setup_atom() -> ZramSetupExecuted {
    ZramSetupExecuted {
        zram_setup_result: ZramSetupResult::ZramSetupUnspecified,
        comp_algorithm_setup_result: CompAlgorithmSetupResult::CompAlgorithmSetupUnspecified,
        writeback_setup_result: WritebackSetupResult::WritebackSetupUnspecified,
        recompression_setup_result: RecompressionSetupResult::RecompressionSetupUnspecified,
        zram_size_mb: 0,
        writeback_size_mb: 0,
    }
}

#[cfg(test)]
mod tests {
    use mmd::zram::writeback::WritebackModeDetails;
    use mmd::zram::MockSysfsZramApi;
    use mmd::zram::ZRAM_API_MTX;

    use super::*;

    #[test]
    fn test_update_writeback_metrics_success() {
        let mut atom = create_default_maintenance_atom();

        update_writeback_metrics(
            &mut atom,
            &Ok(WritebackDetails {
                huge_idle: WritebackModeDetails { written_pages: 1 },
                idle: WritebackModeDetails { written_pages: 12345 },
                huge: WritebackModeDetails { written_pages: u64::MAX },
                limit_pages: 1,
                daily_limit_pages: 6789,
                actual_limit_pages: u64::MAX,
            }),
        );

        assert!(matches!(atom.writeback_result, WritebackResult::WritebackSuccess));
        assert_eq!(atom.writeback_huge_idle_pages, 1);
        assert_eq!(atom.writeback_idle_pages, 12345);
        assert_eq!(atom.writeback_huge_pages, i64::MAX);
        let kb_per_page = get_page_size() as i64 / 1024;
        assert_eq!(atom.writeback_limit_kb, kb_per_page);
        assert_eq!(atom.writeback_daily_limit_kb, 6789 * kb_per_page);
        assert_eq!(atom.writeback_actual_limit_kb, i64::MAX);
        assert_eq!(atom.writeback_total_kb, i64::MAX);
    }

    #[test]
    fn test_update_writeback_metrics_writeback_total_kb() {
        let mut atom = create_default_maintenance_atom();

        update_writeback_metrics(
            &mut atom,
            &Ok(WritebackDetails {
                huge_idle: WritebackModeDetails { written_pages: 10 },
                idle: WritebackModeDetails { written_pages: 200 },
                huge: WritebackModeDetails { written_pages: 3000 },
                ..Default::default()
            }),
        );

        let kb_per_page = get_page_size() as i64 / 1024;
        assert_eq!(atom.writeback_total_kb, 3210 * kb_per_page);
    }

    #[test]
    fn test_update_writeback_metrics_on_failure() {
        let mut atom = create_default_maintenance_atom();
        update_writeback_metrics(&mut atom, &Err(ZramWritebackError::BackoffTime));
        assert!(matches!(atom.writeback_result, WritebackResult::WritebackBackoffTime));
    }

    #[test]
    fn test_update_recompress_metrics_success() {
        let mut atom = create_default_maintenance_atom();
        update_recompress_metrics(&mut atom, &Ok(()));
        assert!(matches!(atom.recompression_result, RecompressionResult::RecompressionSuccess));
    }

    #[test]
    fn test_update_recompress_metrics_on_failure() {
        let mut atom = create_default_maintenance_atom();
        update_recompress_metrics(&mut atom, &Err(ZramRecompressionError::BackoffTime));
        assert!(matches!(atom.recompression_result, RecompressionResult::RecompressionBackoffTime));
    }

    #[test]
    fn test_generate_zram_mm_stat_atom() {
        let _m = ZRAM_API_MTX.lock();
        let mock = MockSysfsZramApi::read_mm_stat_context();
        mock.expect().returning(move || {
            Ok(format!("123456 {} 1023 1024 1235 1 {} 12345 {}", u64::MAX, u32::MAX, u64::MAX))
        });

        let result = generate_zram_mm_stat_atom::<MockSysfsZramApi>();

        assert!(result.is_ok());
        let kb_per_page = get_page_size() as i64 / 1024;
        let atom = result.unwrap();
        assert_eq!(atom.orig_data_kb, 120);
        assert_eq!(atom.compr_data_kb, (u64::MAX / KB) as i64);
        assert_eq!(atom.mem_used_total_kb, 0);
        assert_eq!(atom.mem_limit_kb, 1);
        assert_eq!(atom.mem_used_max_kb, 1);
        assert_eq!(atom.same_pages_kb, kb_per_page);
        assert_eq!(atom.pages_compacted_kb, u32::MAX as i64 * kb_per_page);
        assert_eq!(atom.huge_pages_kb, 12345 * kb_per_page);
        assert_eq!(atom.huge_pages_since_kb, i64::MAX);
    }

    #[test]
    fn test_generate_zram_mm_stat_atom_without_huge_pages() {
        let _m = ZRAM_API_MTX.lock();
        let mock = MockSysfsZramApi::read_mm_stat_context();
        mock.expect().returning(|| Ok("12345 2 3 4 5 6 7".to_string()));

        let result = generate_zram_mm_stat_atom::<MockSysfsZramApi>();

        assert!(result.is_ok());
        let kb_per_page = get_page_size() as i64 / 1024;
        let atom = result.unwrap();
        assert_eq!(atom.orig_data_kb, 12);
        assert_eq!(atom.pages_compacted_kb, 7 * kb_per_page);
        assert_eq!(atom.huge_pages_kb, 0);
        assert_eq!(atom.huge_pages_since_kb, 0);
    }

    #[test]
    fn test_generate_zram_bd_stat_atom() {
        let _m = ZRAM_API_MTX.lock();
        let mock = MockSysfsZramApi::read_bd_stat_context();
        mock.expect().returning(move || Ok(format!("1 12345 {}", u64::MAX)));

        let result = generate_zram_bd_stat_atom::<MockSysfsZramApi>();

        assert!(result.is_ok());
        let kb_per_page = get_page_size() as i64 / 1024;
        let atom = result.unwrap();
        assert_eq!(atom.bd_count_kb, kb_per_page);
        assert_eq!(atom.bd_reads_kb, 12345 * kb_per_page);
        assert_eq!(atom.bd_writes_kb, i64::MAX);
    }
}
