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

//! mmd is a native daemon managing memory task:
//!
//! * zram

mod atom;
mod properties;
mod service;

use std::fmt;
use std::path::Path;
use std::sync::Arc;
use std::sync::Mutex;
use std::time::Duration;
use std::time::Instant;

use anyhow::anyhow;
use anyhow::Context;
use binder::BinderFeatures;
use log::error;
use log::info;
use log::warn;
use log::LevelFilter;
use mmd::block_dev::clear_block_device_scheduler;
use mmd::block_dev::configure_block_device_queue_depth;
use mmd::os::get_page_count;
use mmd::os::get_page_size;
use mmd::size_spec::parse_size_spec;
use mmd::suspend_history::SuspendHistory;
use mmd::suspend_history::SuspendMonitor;
use mmd::time::TimeApiImpl;
use mmd::zram::recompression::get_zram_recompression_status;
use mmd::zram::recompression::ZramRecompression;
use mmd::zram::recompression::ZramRecompressionStatus;
use mmd::zram::setup::activate_zram;
use mmd::zram::setup::create_zram_writeback_device;
use mmd::zram::setup::enable_zram_writeback_limit;
use mmd::zram::setup::is_zram_swap_activated;
use mmd::zram::setup::SetupApiImpl;
use mmd::zram::setup::WritebackDeviceSetupError;
use mmd::zram::stats::load_total_zram_size;
use mmd::zram::writeback::get_zram_writeback_status;
use mmd::zram::writeback::ZramWriteback;
use mmd::zram::writeback::ZramWritebackStatus;
use mmd::zram::SysfsZramApi;
use mmd::zram::SysfsZramApiImpl;
use mmd_aidl_interface::aidl::android::os::IMmd::BnMmd;
use nix::sys::statvfs::statvfs;
use properties::SecondsProp;
use properties::U64Prop;
use rustutils::system_properties;
use statslog_rust::zram_setup_executed::CompAlgorithmSetupResult;
use statslog_rust::zram_setup_executed::RecompressionSetupResult;
use statslog_rust::zram_setup_executed::WritebackSetupResult;
use statslog_rust::zram_setup_executed::ZramSetupExecuted;
use statslog_rust::zram_setup_executed::ZramSetupResult;
use statspull_rust::set_pull_atom_callback;

use crate::atom::create_default_setup_atom;
use crate::atom::report_zram_bd_stat;
use crate::atom::report_zram_mm_stat;
use crate::atom::update_zram_setup_metrics;
use crate::properties::is_zram_enabled;
use crate::properties::BoolProp;
use crate::properties::StringProp;

struct ZramContext {
    zram_writeback: Option<ZramWriteback>,
    zram_recompression: Option<ZramRecompression>,
    suspend_history: SuspendHistory,
    last_maintenance_at: Instant,
}

const DEFAULT_ZRAM_WRITEBACK_ENABLED: bool = false;

// In Android zram writeback file is always "/data/per_boot/zram_swap".
const ZRAM_WRITEBACK_FILE_PATH: &str = "/data/per_boot/zram_swap";

// Default writeback device size of 1G.
const DEFAULT_WRITEBACK_DEVICE_SIZE: u64 = 1 << 30;

// Default minimum freespace for writeback file setup is 1.5G.
const DEFAULT_WRITEBACK_MIN_FREE_SPACE_MIB: u64 = 1536;

// The default minimum size a zram writeback device may be.
// This prevents a writeback device of 1MiB from being created, for example.
const DEFAULT_WRITEBACK_MIN_VOLUME_SIZE: u64 = 128 << 20; // 128 MiB.

const DEFAULT_ZRAM_RECOMPRESSION_ENABLED: bool = false;
const DEFAULT_ZRAM_RECOMPRESSION_ALGORITHM: &str = "zstd";

const MAX_ZRAM_PERCENTAGE_ALLOWED: u64 = 500;
const MAX_WRITEBACK_SIZE_PERCENTAGE_ALLOWED: u64 = 100;

// MiB in bytes.
const MIB: u64 = 1 << 20;

fn adjust_writeback_device_size(
    requested_device_size: u64,
    min_free_space: u64,
    free_space: u64,
    block_size: u64,
) -> u64 {
    if free_space <= min_free_space {
        info!("there is not enough free space to meet the minimum space requirement of {min_free_space} bytes to set up zram writeback device");
        return 0;
    }

    let mut adjusted_device_size = if requested_device_size + min_free_space > free_space {
        let adjusted_device_size = free_space - min_free_space;
        info!("adjusting zram writeback device size from {requested_device_size} to {adjusted_device_size} bytes to meet the minimum space requirement of {min_free_space} bytes");
        adjusted_device_size
    } else {
        requested_device_size
    };

    if adjusted_device_size < DEFAULT_WRITEBACK_MIN_VOLUME_SIZE {
        info!("adjusting zram writeback device size to 0, since the device size of {adjusted_device_size} is less than the minimum device size of {DEFAULT_WRITEBACK_MIN_VOLUME_SIZE} bytes");
        return 0;
    }

    if adjusted_device_size % block_size != 0 {
        adjusted_device_size = adjusted_device_size - adjusted_device_size % block_size;
        info!("adjusting zram writeback device size to {adjusted_device_size} for block size alignment");
    }
    adjusted_device_size
}

#[derive(thiserror::Error)]
#[error("{source}")]
struct WritebackSetupError {
    source: anyhow::Error,
    reason: WritebackSetupResult,
}

impl fmt::Debug for WritebackSetupError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{0:?}", self.source)
    }
}

fn setup_zram_writeback() -> Result<u64, WritebackSetupError> {
    let zram_writeback_status =
        get_zram_writeback_status::<SysfsZramApiImpl>().map_err(|e| WritebackSetupError {
            source: e.into(),
            reason: WritebackSetupResult::WritebackSetupCheckStatus,
        })?;
    match zram_writeback_status {
        ZramWritebackStatus::Unsupported => {
            return Err(WritebackSetupError {
                source: anyhow!(
                    "zram writeback is not supported by the kernel, skipping zram \
                    writeback device setup"
                ),
                reason: WritebackSetupResult::WritebackSetupNotSupported,
            });
        }
        ZramWritebackStatus::Activated => {
            return Err(WritebackSetupError {
                source: anyhow!(
                    "zram writeback is already activated, skipping zram writeback device setup"
                ),
                reason: WritebackSetupResult::WritebackSetupActivated,
            });
        }
        ZramWritebackStatus::NotConfigured => {
            // Do nothing, we should proceed to set up the writeback device.
        }
    };

    enable_zram_writeback_limit::<SysfsZramApiImpl>()
        .context("failed to enable zram writeback limit")
        .map_err(|e| WritebackSetupError {
            source: e,
            reason: WritebackSetupResult::WritebackSetupWritebackLimitEnableFail,
        })?;

    let backing_device_size_spec =
        StringProp::ZramWritebackDeviceSize.get(&DEFAULT_WRITEBACK_DEVICE_SIZE);
    let partition_stat =
        statvfs("/data").context("failed to get /data partition stats").map_err(|e| {
            WritebackSetupError { source: e, reason: WritebackSetupResult::WritebackSetupParseSpec }
        })?;
    // Fragment size isn't a commonly used term in file systems on linux, in most other cases
    // than NFS they are equal to block size. However POSIX defines the unit of f_blocks as
    // f_frsize.
    // https://man7.org/linux/man-pages/man3/statvfs.3.html
    // We prioritize following POSIX compatibility than readability on codebase here.
    // libc::c_ulong and libc::fsblkcnt_t can be non-u64 on some platforms for statvfs fields.
    #[allow(clippy::useless_conversion)]
    let partition_block_size = u64::from(partition_stat.fragment_size());
    let requested_device_size = parse_size_spec(
        &backing_device_size_spec,
        partition_block_size,
        // libc::c_ulong and libc::fsblkcnt_t can be non-u64 on some platforms for statvfs fields.
        #[allow(clippy::useless_conversion)]
        u64::from(partition_stat.blocks()),
        MAX_WRITEBACK_SIZE_PERCENTAGE_ALLOWED,
    )
    .context("failed to parse device size spec")
    .map_err(|e| WritebackSetupError {
        source: e,
        reason: WritebackSetupResult::WritebackSetupParseSpec,
    })?;
    // libc::c_ulong and libc::fsblkcnt_t can be non-u64 on some platforms for statvfs fields.
    #[allow(clippy::useless_conversion)]
    let backing_device_size = adjust_writeback_device_size(
        requested_device_size,
        U64Prop::ZramWritebackMinFreeSpaceMib.get(DEFAULT_WRITEBACK_MIN_FREE_SPACE_MIB) * MIB,
        partition_block_size * u64::from(partition_stat.blocks_free()),
        partition_block_size,
    );
    if backing_device_size == 0 {
        warn!("zram writeback is enabled but backing device size is 0, skipping zram writeback device setup");
        return Ok(backing_device_size);
    }

    mmdproperties::mmdproperties::set_actual_zram_backing_device_size(backing_device_size)
        .context("failed to update actual zram writeback device size")
        .map_err(|e| WritebackSetupError {
            source: e,
            reason: WritebackSetupResult::WritebackSetupSetActualDeviceSizeFail,
        })?;

    info!("setting up zram writeback device with size {}", backing_device_size);
    let writeback_device = create_zram_writeback_device::<SetupApiImpl>(
        Path::new(ZRAM_WRITEBACK_FILE_PATH),
        backing_device_size,
    )
    .map_err(|e| match e {
        WritebackDeviceSetupError::CreateBackingFile(_) => WritebackSetupError {
            source: e.into(),
            reason: WritebackSetupResult::WritebackSetupCreateBackingFileFail,
        },
        WritebackDeviceSetupError::CreateBackingDevice(_) => WritebackSetupError {
            source: e.into(),
            reason: WritebackSetupResult::WritebackSetupCreateBackingDeviceFail,
        },
    })?;

    let device_path = writeback_device.path;
    let device_name = device_path
        .file_name()
        .and_then(std::ffi::OsStr::to_str)
        .context("failed to get backing device name")
        .map_err(|e| WritebackSetupError {
            source: e,
            reason: WritebackSetupResult::WritebackSetupCreateBackingDeviceFail,
        })?;
    if let Err(e) = clear_block_device_scheduler(device_name) {
        warn!("failed to clear writeback device scheduler: {e:?}");
    }
    if let Err(e) = configure_block_device_queue_depth(device_name, "/") {
        warn!("failed to configure device queue depth: {e:?}");
    }

    let device_path_str = device_path
        .to_str()
        .context("failed to get backing device path")
        .map_err(|e| WritebackSetupError {
            source: e,
            reason: WritebackSetupResult::WritebackSetupCreateBackingDeviceFail,
        })?;
    SysfsZramApiImpl::write_backing_dev(device_path_str)
        .context("failed to set zram backing device")
        .map_err(|e| WritebackSetupError {
            source: e,
            reason: WritebackSetupResult::WritebackSetupSetWritebackDeviceFail,
        })?;
    Ok(backing_device_size)
}

#[derive(thiserror::Error)]
#[error("{source}")]
struct RecompressionError {
    source: anyhow::Error,
    reason: RecompressionSetupResult,
}

impl fmt::Debug for RecompressionError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{0:?}", self.source)
    }
}

fn setup_zram_recompression() -> Result<(), RecompressionError> {
    match get_zram_recompression_status::<SysfsZramApiImpl>().map_err(|e| RecompressionError {
        source: e.into(),
        reason: RecompressionSetupResult::RecompressionSetupCheckStatus,
    })? {
        ZramRecompressionStatus::Unsupported => {
            return Err(RecompressionError {
                source: anyhow!(
                    "zram recompression is not supported by the kernel, skipping zram \
                    recompression setup"
                ),
                reason: RecompressionSetupResult::RecompressionSetupNotSupported,
            });
        }
        ZramRecompressionStatus::Activated => {
            return Err(RecompressionError {
                source: anyhow!(
                    "zram recompression is already activated, skipping zram recompression setup"
                ),
                reason: RecompressionSetupResult::RecompressionSetupActivated,
            });
        }
        ZramRecompressionStatus::NotConfigured => {
            // Do nothing, we should proceed to set up recompression algorithm.
        }
    };

    let recompression_algorithm =
        StringProp::ZramRecompressionAlgorithm.get(DEFAULT_ZRAM_RECOMPRESSION_ALGORITHM);
    let recomp_algo_config = format!("algo={recompression_algorithm}");
    SysfsZramApiImpl::write_recomp_algorithm(&recomp_algo_config)
        .context(format!(
            "Failed to set up recompression algorithm with config {recomp_algo_config}"
        ))
        .map_err(|e| RecompressionError {
            source: e,
            reason: RecompressionSetupResult::RecompressionSetupSetRecompAlgorithmFail,
        })?;

    Ok(())
}

fn setup_zram(zram_setup_atom: &mut ZramSetupExecuted) -> anyhow::Result<()> {
    zram_setup_atom.zram_setup_result = ZramSetupResult::ZramSetupCheckStatus;
    let zram_activated = is_zram_swap_activated::<SetupApiImpl>()?;
    if zram_activated {
        info!("zram is already on, skipping zram setup");
        zram_setup_atom.zram_setup_result = ZramSetupResult::ZramSetupActivated;
        return Ok(());
    }

    if BoolProp::ZramWritebackEnabled.get(DEFAULT_ZRAM_WRITEBACK_ENABLED) {
        match setup_zram_writeback() {
            Ok(device_size) => {
                if device_size > 0 {
                    // u64 bytes in MiB should fit int64.
                    zram_setup_atom.writeback_size_mb = (device_size / MIB) as i64;
                    zram_setup_atom.writeback_setup_result =
                        WritebackSetupResult::WritebackSetupSuccess;
                } else {
                    zram_setup_atom.writeback_setup_result =
                        WritebackSetupResult::WritebackSetupDeviceSizeZero;
                }
            }
            Err(e) => {
                error!(
                    "failed to set up zram writeback: {e:?}, zram device will be set up with no \
                    backing device"
                );
                zram_setup_atom.writeback_setup_result = e.reason;
            }
        }
    }

    if BoolProp::ZramRecompressionEnabled.get(DEFAULT_ZRAM_RECOMPRESSION_ENABLED) {
        match setup_zram_recompression() {
            Ok(()) => {
                zram_setup_atom.recompression_setup_result =
                    RecompressionSetupResult::RecompressionSetupSuccess;
            }
            Err(e) => {
                error!(
                    "failed to set up zram recompression: {e:?}, zram device will be set up \
                    without recompression feature"
                );
                zram_setup_atom.recompression_setup_result = e.reason;
            }
        }
    }

    zram_setup_atom.zram_setup_result = ZramSetupResult::ZramSetupParseSpec;
    let zram_size_spec = StringProp::ZramSize.get("50%");
    let zram_size = parse_size_spec(
        &zram_size_spec,
        get_page_size(),
        get_page_count(),
        MAX_ZRAM_PERCENTAGE_ALLOWED,
    )?;
    let comp_algorithm = StringProp::ZramCompAlgorithm.get("");
    if !comp_algorithm.is_empty() {
        match SysfsZramApiImpl::write_comp_algorithm(&comp_algorithm) {
            Ok(_) => {
                zram_setup_atom.comp_algorithm_setup_result =
                    CompAlgorithmSetupResult::CompAlgorithmSetupSuccess;
            }
            Err(e) => {
                // Continue to utilize zram with default algorithm if specifying algorithm fails
                // (e.g. the algorithm is not supported by the kernel).
                error!("failed to update zram comp algorithm: {e:?}");
                zram_setup_atom.comp_algorithm_setup_result =
                    CompAlgorithmSetupResult::CompAlgorithmSetupFail;
            }
        }
    }
    let activate_result = activate_zram::<SysfsZramApiImpl, SetupApiImpl>(zram_size);
    update_zram_setup_metrics(zram_setup_atom, &activate_result);
    activate_result?;
    // u64 bytes in MiB should fit int64.
    zram_setup_atom.zram_size_mb = (zram_size / MIB) as i64;
    Ok(())
}

fn main() {
    let cmd = std::env::args().nth(1).unwrap_or_default();
    // "mmd --set-property" command copies the AConfig flag to "mmd.enabled_aconfig" system
    // property as either "true" or "false".
    // This is the workaround for init language which does not support AConfig integration.
    // TODO: b/380365026 - Remove "--set-property" command when init language supports AConfig
    // integration.
    if cmd == "--set-property" {
        let value = if mmd_flags::mmd_enabled() { "true" } else { "false" };
        system_properties::write("mmd.enabled_aconfig", value).expect("set system property");
        return;
    }

    let _init_success = logger::init(
        logger::Config::default().with_tag_on_device("mmd").with_max_level(LevelFilter::Trace),
    );

    if !mmd_flags::mmd_enabled() {
        // It is mmd.rc responsibility to start mmd process only if AConfig flag is enabled.
        // This is a safe guard to ensure that mmd runs only when AConfig flag is enabled.
        warn!("mmd is disabled");
        return;
    }

    if cmd == "--setup-zram" {
        if !is_zram_enabled() {
            warn!("mmd zram setup is disabled");
            return;
        }
        let mut zram_setup_atom = create_default_setup_atom();
        let setup_zram_result = setup_zram(&mut zram_setup_atom);
        if let Err(e) = zram_setup_atom.stats_write() {
            error!("failed to submit ZramSetupExecuted atom: {e:?}");
        }
        setup_zram_result.expect("zram setup");
        return;
    } else if !cmd.is_empty() {
        error!(
            "unexpected command {cmd}. mmd only supports either --set-property or --setup-zram."
        );
        return;
    }

    let mut zram_writeback = match load_zram_writeback_disk_size() {
        Ok(Some(zram_writeback_disk_size)) => {
            info!("zram writeback is activated");
            match load_total_zram_size::<SysfsZramApiImpl>() {
                Ok(total_zram_size) => {
                    Some(ZramWriteback::new(total_zram_size, zram_writeback_disk_size))
                }
                Err(e) => {
                    error!("failed to load total zram size: {e:?}");
                    None
                }
            }
        }
        Ok(None) => {
            info!("zram writeback is not activated");
            None
        }
        Err(e) => {
            error!("failed to load zram writeback file size: {e:?}");
            None
        }
    };

    let mut zram_recompression = match get_zram_recompression_status::<SysfsZramApiImpl>() {
        Ok(status) => {
            if status == ZramRecompressionStatus::Activated {
                info!("zram recompression is activated");
                Some(ZramRecompression::new())
            } else {
                info!("zram recompression is not activated");
                None
            }
        }
        Err(e) => {
            error!("failed to check zram recompression is activated: {e:?}");
            None
        }
    };

    if zram_writeback.is_some() || zram_recompression.is_some() {
        match is_idle_aging_supported() {
            Ok(idle_aging_supported) => {
                if !idle_aging_supported {
                    warn!(
                        "mmd zram maintenance is disabled due to missing kernel config. mmd zram \
                        maintenance requires either CONFIG_ZRAM_TRACK_ENTRY_ACTIME or \
                        CONFIG_ZRAM_MEMORY_TRACKING kernel config enabled for tracking idle pages \
                        based on last accessed time."
                    );
                    // TODO: b/396439110 - Implement some zram maintenance fallback logic to
                    // support the case when idle aging is not supported by the kernel. Eg: only
                    // handle huge pages.
                    zram_writeback = None;
                    zram_recompression = None;
                }
            }
            Err(e) => {
                error!(
                    "failed to check whether idle aging is supported, mmd zram maintenance is \
                    enabled but might not work properly: {e:?}"
                );
            }
        }
    };

    let ctx = Arc::new(Mutex::new(ZramContext {
        zram_writeback,
        zram_recompression,
        suspend_history: SuspendHistory::new(),
        last_maintenance_at: Instant::now(),
    }));

    let mmd_service = service::MmdService::new(ctx.clone());
    let mmd_service_binder = BnMmd::new_binder(mmd_service, BinderFeatures::default());
    binder::add_service("mmd", mmd_service_binder.as_binder()).expect("register service");

    let suspend_monitor_thread_handle = std::thread::spawn(move || {
        let mut suspend_monitor = SuspendMonitor::<TimeApiImpl>::new();
        loop {
            // Storing suspend duration log in 1 hour interval has a good enough resolution to
            // adjust 2 (for zram recompression) ~ 25 (for zram writeback) hours idle duration at
            // SuspendHistory.
            std::thread::sleep(Duration::from_secs(3600));

            let max_idle_duration = std::cmp::max(
                SecondsProp::ZramWritebackMaxIdle
                    .get(mmd::zram::writeback::Params::default().max_idle),
                SecondsProp::ZramRecompressionMaxIdle
                    .get(mmd::zram::recompression::Params::default().max_idle),
            );
            let (suspend_duration, now_boot) = suspend_monitor.generate_suspend_duration();
            let mut ctx = ctx.lock().expect("mmd aborts on panics");
            ctx.suspend_history.record_suspend_duration(
                suspend_duration,
                now_boot,
                max_idle_duration,
            );
        }
    });

    // mmd sends reports of zram stats if zram is activated on the device regardless of who (mmd or
    // others) manages zram.
    if is_zram_swap_activated::<SetupApiImpl>().unwrap_or(false) {
        set_pull_atom_callback(
            statslog_rust_header::Atoms::ZramMmStatMmd,
            None,
            report_zram_mm_stat,
        );
        set_pull_atom_callback(
            statslog_rust_header::Atoms::ZramBdStatMmd,
            None,
            report_zram_bd_stat,
        );
    }

    info!("mmd started");

    binder::ProcessState::join_thread_pool();

    suspend_monitor_thread_handle.join().expect("thread join");
}

/// Whether zram idle aging is supported.
///
/// Idle aging requires either CONFIG_ZRAM_MEMORY_TRACKING or CONFIG_ZRAM_TRACK_ENTRY_ACTIME
/// kernel config enabled to work. If it's not supported by the kernel, -EINVAL will be
/// returned from writing a number to the idle file after the zram device is initialized.
fn is_idle_aging_supported() -> std::io::Result<bool> {
    if let Err(e) = SysfsZramApiImpl::set_idle(&u32::MAX.to_string()) {
        if e.kind() == std::io::ErrorKind::InvalidInput {
            Ok(false)
        } else {
            Err(e)
        }
    } else {
        Ok(true)
    }
}

/// Loads the zram writeback disk size.
///
/// If zram writeback is not enabled, this returns `Ok(None)`.
pub fn load_zram_writeback_disk_size() -> std::io::Result<Option<u64>> {
    if get_zram_writeback_status::<SysfsZramApiImpl>()? == ZramWritebackStatus::Activated {
        Ok(mmdproperties::mmdproperties::actual_zram_backing_device_size().unwrap_or(None))
    } else {
        Ok(None)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    // GiB in bytes.
    const GIB: u64 = 1 << 30;
    const DEFAULT_BLOCK_SIZE: u64 = 4096;

    #[test]
    fn adjust_writeback_device_size_enough_disk_space() {
        let size = adjust_writeback_device_size(
            /* requested_device_size */ GIB,
            /* min_free_space */ GIB,
            /* free_space */ 3 * GIB,
            DEFAULT_BLOCK_SIZE,
        );
        assert_eq!(size, GIB);
    }

    #[test]
    fn adjust_writeback_device_size_enough_disk_space_but_size_too_small() {
        let size = adjust_writeback_device_size(
            /* requested_device_size */ 127 * MIB,
            /* min_free_space */ GIB,
            /* free_space */ 3 * GIB,
            DEFAULT_BLOCK_SIZE,
        );
        assert_eq!(size, 0);
    }

    #[test]
    fn adjust_writeback_device_size_enough_disk_space_meeting_min_size_requirement() {
        let size = adjust_writeback_device_size(
            /* requested_device_size */ 128 * MIB,
            /* min_free_space */ GIB,
            /* free_space */ 3 * GIB,
            DEFAULT_BLOCK_SIZE,
        );
        assert_eq!(size, 128 * MIB);
    }

    #[test]
    fn adjust_writeback_device_size_disk_space_too_low() {
        let size = adjust_writeback_device_size(
            /* requested_device_size */ GIB,
            /* min_free_space */ 2 * GIB,
            /* free_space */ GIB,
            DEFAULT_BLOCK_SIZE,
        );
        assert_eq!(size, 0);
    }

    #[test]
    fn adjust_writeback_device_size_needs_adjusted() {
        let size = adjust_writeback_device_size(
            /* requested_device_size */ 2 * GIB,
            /* min_free_space */ GIB,
            /* free_space */ 2 * GIB,
            DEFAULT_BLOCK_SIZE,
        );
        assert_eq!(size, GIB);
    }

    #[test]
    fn adjust_writeback_device_size_too_small_after_adjusted() {
        let size = adjust_writeback_device_size(
            /* requested_device_size */ 2 * GIB,
            /* min_free_space */ GIB,
            /* free_space */ GIB + MIB,
            DEFAULT_BLOCK_SIZE,
        );
        assert_eq!(size, 0);
    }

    #[test]
    fn adjust_writeback_device_size_block_size_alignment() {
        let size = adjust_writeback_device_size(
            /* requested_device_size */ GIB + 1,
            /* min_free_space */ GIB,
            /* free_space */ 3 * GIB,
            DEFAULT_BLOCK_SIZE,
        );
        assert_eq!(size, GIB);
    }
}
