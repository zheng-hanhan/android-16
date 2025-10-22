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

//! This module provides functions to load zram stats.

#[cfg(test)]
mod tests;

use std::ops::Deref;

use crate::zram::SysfsZramApi;

/// Error from loading zram stats.
#[derive(Debug, thiserror::Error)]
pub enum Error {
    /// Stat file format is invalid.
    #[error("failed to parse")]
    Parse,
    /// Failed to read stat file.
    #[error("failed to read: {0}")]
    Io(#[from] std::io::Error),
}

type Result<T> = std::result::Result<T, Error>;

fn parse_next<T: std::str::FromStr>(
    iter: &mut impl Iterator<Item = impl Deref<Target = str>>,
) -> Result<T> {
    iter.next().ok_or(Error::Parse)?.parse().map_err(|_| Error::Parse)
}

fn parse_next_optional<T: std::str::FromStr>(
    iter: &mut impl Iterator<Item = impl Deref<Target = str>>,
) -> Result<Option<T>> {
    iter.next().map(|v| v.parse()).transpose().map_err(|_| Error::Parse)
}

/// Loads /sys/block/zram0/disksize
pub fn load_total_zram_size<Z: SysfsZramApi>() -> Result<u64> {
    let contents = Z::read_disksize()?;
    contents.trim().parse().map_err(|_| Error::Parse)
}

/// Stats from /sys/block/zram0/mm_stat
#[derive(Debug, Default, PartialEq, Eq)]
pub struct ZramMmStat {
    /// Uncompressed size of data stored in this disk. This excludes same-element-filled pages
    /// (same_pages) since no memory is allocated for them. Unit: bytes
    pub orig_data_size: u64,
    /// Compressed size of data stored in this disk.
    pub compr_data_size: u64,
    /// The amount of memory allocated for this disk. This includes allocator fragmentation and
    /// metadata overhead, allocated for this disk. So, allocator space efficiency can be calculated
    /// using compr_data_size and this statistic. Unit: bytes
    pub mem_used_total: u64,
    /// The maximum amount of memory ZRAM can use to store The compressed data.
    pub mem_limit: u32,
    /// The maximum amount of memory zram have consumed to store the data.
    ///
    /// In zram_drv.h we define max_used_pages as atomic_long_t which could be negative, but
    /// negative value does not make sense for the variable.
    pub mem_used_max: i64,
    /// The number of same element filled pages written to this disk. No memory is allocated for
    /// such pages.
    pub same_pages: u64,
    /// The number of pages freed during compaction.
    pub pages_compacted: u32,
    /// The number of incompressible pages.
    /// Start supporting from v4.19.
    pub huge_pages: Option<u64>,
    /// The number of huge pages since zram set up.
    /// Start supporting from v5.15.
    pub huge_pages_since: Option<u64>,
}

impl ZramMmStat {
    /// Parse /sys/block/zram0/mm_stat.
    pub fn load<Z: SysfsZramApi>() -> Result<Self> {
        let contents = Z::read_mm_stat()?;
        let mut values = contents.split_whitespace();
        Ok(ZramMmStat {
            orig_data_size: parse_next(&mut values)?,
            compr_data_size: parse_next(&mut values)?,
            mem_used_total: parse_next(&mut values)?,
            mem_limit: parse_next(&mut values)?,
            mem_used_max: parse_next(&mut values)?,
            same_pages: parse_next(&mut values)?,
            pages_compacted: parse_next(&mut values)?,
            huge_pages: parse_next_optional(&mut values)?,
            huge_pages_since: parse_next_optional(&mut values)?,
        })
    }
}

/// Stats from /sys/block/zram0/bd_stat
#[derive(Debug, Default, PartialEq, Eq)]
pub struct ZramBdStat {
    /// Size of data written in backing device. Unit: page
    pub bd_count_pages: u64,
    /// The number of reads from backing device. Unit: page
    pub bd_reads_pages: u64,
    /// The number of writes to backing device. Unit: page
    pub bd_writes_pages: u64,
}

impl ZramBdStat {
    /// Parse /sys/block/zram0/bd_stat.
    pub fn load<Z: SysfsZramApi>() -> Result<Self> {
        let contents = Z::read_bd_stat()?;
        let mut values = contents.split_whitespace();
        Ok(ZramBdStat {
            bd_count_pages: parse_next(&mut values)?,
            bd_reads_pages: parse_next(&mut values)?,
            bd_writes_pages: parse_next(&mut values)?,
        })
    }
}
