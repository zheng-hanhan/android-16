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

//! This module provides policies to manage zram features.

pub mod idle;
pub mod recompression;
pub mod setup;
pub mod stats;
pub mod writeback;

use std::io;

// Files for zram general information
const ZRAM_DISKSIZE_PATH: &str = "/sys/block/zram0/disksize";
const ZRAM_MM_STAT_PATH: &str = "/sys/block/zram0/mm_stat";
const ZRAM_COMP_ALGORITHM_PATH: &str = "/sys/block/zram0/comp_algorithm";

// Files for memory tracking
const ZRAM_IDLE_PATH: &str = "/sys/block/zram0/idle";

// Files for writeback
const ZRAM_BACKING_DEV_PATH: &str = "/sys/block/zram0/backing_dev";
const ZRAM_WRITEBACK_PATH: &str = "/sys/block/zram0/writeback";
const ZRAM_WRITEBACK_LIMIT_ENABLE_PATH: &str = "/sys/block/zram0/writeback_limit_enable";
const ZRAM_WRITEBACK_LIMIT_PATH: &str = "/sys/block/zram0/writeback_limit";
const ZRAM_BD_STAT_PATH: &str = "/sys/block/zram0/bd_stat";

// Files for recompression
const ZRAM_RECOMP_ALGORITHM_PATH: &str = "/sys/block/zram0/recomp_algorithm";
const ZRAM_RECOMPRESS_PATH: &str = "/sys/block/zram0/recompress";

/// [SysfsZramApi] is a mockable interface for access to files under
/// "/sys/block/zram0" which is system global.
///
/// The naming convention: functions for files which is readable and writable
///
/// * fn read_<file_name>() -> io::Result<String>
/// * fn write_<file_name>(contents: &str) -> io::Result<()>
///
/// We don't have naming conventions for files which is writable only.
#[cfg_attr(any(test, feature = "test_utils"), mockall::automock)]
pub trait SysfsZramApi {
    /// Read "/sys/block/zram0/disksize".
    fn read_disksize() -> io::Result<String>;
    /// Write "/sys/block/zram0/disksize".
    fn write_disksize(contents: &str) -> io::Result<()>;
    /// Read "/sys/block/zram0/mm_stat".
    fn read_mm_stat() -> io::Result<String>;

    /// Set compression algorithm.
    fn write_comp_algorithm(contents: &str) -> io::Result<()>;

    /// Write contents to "/sys/block/zram0/idle".
    fn set_idle(contents: &str) -> io::Result<()>;

    /// Read "/sys/block/zram0/backing_dev".
    fn read_backing_dev() -> io::Result<String>;
    /// Write "/sys/block/zram0/backing_dev".
    fn write_backing_dev(contents: &str) -> io::Result<()>;
    /// Write contents to "/sys/block/zram0/writeback".
    fn writeback(contents: &str) -> io::Result<()>;
    /// Write contents to "/sys/block/zram0/writeback_limit_enable".
    fn write_writeback_limit_enable(contents: &str) -> io::Result<()>;
    /// Write contents to "/sys/block/zram0/writeback_limit".
    fn write_writeback_limit(contents: &str) -> io::Result<()>;
    /// Read "/sys/block/zram0/writeback_limit".
    fn read_writeback_limit() -> io::Result<String>;
    /// Read "/sys/block/zram0/bd_stat".
    fn read_bd_stat() -> io::Result<String>;

    /// Read "/sys/block/zram0/recomp_algorithm".
    fn read_recomp_algorithm() -> io::Result<String>;
    /// Write "/sys/block/zram0/recomp_algorithm".
    fn write_recomp_algorithm(contents: &str) -> io::Result<()>;
    /// Write contents to "/sys/block/zram0/recompress".
    fn recompress(contents: &str) -> io::Result<()>;
}

/// The implementation of [SysfsZramApi].
pub struct SysfsZramApiImpl;

impl SysfsZramApi for SysfsZramApiImpl {
    fn read_disksize() -> io::Result<String> {
        std::fs::read_to_string(ZRAM_DISKSIZE_PATH)
    }

    fn write_disksize(contents: &str) -> io::Result<()> {
        std::fs::write(ZRAM_DISKSIZE_PATH, contents)
    }

    fn read_mm_stat() -> io::Result<String> {
        std::fs::read_to_string(ZRAM_MM_STAT_PATH)
    }

    fn set_idle(contents: &str) -> io::Result<()> {
        std::fs::write(ZRAM_IDLE_PATH, contents)
    }

    fn read_backing_dev() -> io::Result<String> {
        std::fs::read_to_string(ZRAM_BACKING_DEV_PATH)
    }

    fn write_backing_dev(contents: &str) -> io::Result<()> {
        std::fs::write(ZRAM_BACKING_DEV_PATH, contents)
    }

    fn writeback(contents: &str) -> io::Result<()> {
        std::fs::write(ZRAM_WRITEBACK_PATH, contents)
    }

    fn write_writeback_limit(contents: &str) -> io::Result<()> {
        std::fs::write(ZRAM_WRITEBACK_LIMIT_PATH, contents)
    }

    fn write_writeback_limit_enable(contents: &str) -> io::Result<()> {
        std::fs::write(ZRAM_WRITEBACK_LIMIT_ENABLE_PATH, contents)
    }

    fn read_writeback_limit() -> io::Result<String> {
        std::fs::read_to_string(ZRAM_WRITEBACK_LIMIT_PATH)
    }

    fn read_bd_stat() -> io::Result<String> {
        std::fs::read_to_string(ZRAM_BD_STAT_PATH)
    }

    fn read_recomp_algorithm() -> io::Result<String> {
        std::fs::read_to_string(ZRAM_RECOMP_ALGORITHM_PATH)
    }

    fn write_recomp_algorithm(contents: &str) -> io::Result<()> {
        std::fs::write(ZRAM_RECOMP_ALGORITHM_PATH, contents)
    }

    fn recompress(contents: &str) -> io::Result<()> {
        std::fs::write(ZRAM_RECOMPRESS_PATH, contents)
    }

    fn write_comp_algorithm(contents: &str) -> io::Result<()> {
        std::fs::write(ZRAM_COMP_ALGORITHM_PATH, contents)
    }
}

/// Mutex to synchronize tests using [MockSysfsZramApi].
///
/// mockall for static functions requires synchronization.
///
/// https://docs.rs/mockall/latest/mockall/#static-methods
pub static ZRAM_API_MTX: std::sync::Mutex<()> = std::sync::Mutex::new(());
