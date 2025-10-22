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
use crate::zram::MockSysfsZramApi;
use crate::zram::ZRAM_API_MTX;

#[test]
fn test_load_total_zram_size() {
    let _m = ZRAM_API_MTX.lock();
    let mock = MockSysfsZramApi::read_disksize_context();
    let contents = "12345\n";
    mock.expect().returning(|| Ok(contents.to_string()));

    assert_eq!(load_total_zram_size::<MockSysfsZramApi>().unwrap(), 12345);
}

#[test]
fn test_load_total_zram_size_invalid_value() {
    let _m = ZRAM_API_MTX.lock();
    let mock = MockSysfsZramApi::read_disksize_context();
    let contents = "a";
    mock.expect().returning(|| Ok(contents.to_string()));

    assert!(load_total_zram_size::<MockSysfsZramApi>().is_err());
}

#[test]
fn test_load_total_zram_size_fail_read() {
    let _m = ZRAM_API_MTX.lock();
    let mock = MockSysfsZramApi::read_disksize_context();
    mock.expect().returning(|| Err(std::io::Error::other("error")));

    assert!(load_total_zram_size::<MockSysfsZramApi>().is_err());
}

#[test]
fn test_zram_mm_stat() {
    let _m = ZRAM_API_MTX.lock();
    let mock = MockSysfsZramApi::read_mm_stat_context();
    let contents = " 1 2 3 4 5 6 7 8 9";
    mock.expect().returning(|| Ok(contents.to_string()));

    assert_eq!(
        ZramMmStat::load::<MockSysfsZramApi>().unwrap(),
        ZramMmStat {
            orig_data_size: 1,
            compr_data_size: 2,
            mem_used_total: 3,
            mem_limit: 4,
            mem_used_max: 5,
            same_pages: 6,
            pages_compacted: 7,
            huge_pages: Some(8),
            huge_pages_since: Some(9),
        }
    );
}

#[test]
fn test_zram_mm_stat_skip_huge_pages_since() {
    let _m = ZRAM_API_MTX.lock();
    let mock = MockSysfsZramApi::read_mm_stat_context();
    let contents = " 1 2 3 4 5 6 7 8";
    mock.expect().returning(|| Ok(contents.to_string()));

    assert_eq!(
        ZramMmStat::load::<MockSysfsZramApi>().unwrap(),
        ZramMmStat {
            orig_data_size: 1,
            compr_data_size: 2,
            mem_used_total: 3,
            mem_limit: 4,
            mem_used_max: 5,
            same_pages: 6,
            pages_compacted: 7,
            huge_pages: Some(8),
            huge_pages_since: None,
        }
    );
}

#[test]
fn test_zram_mm_stat_skip_huge_pages() {
    let _m = ZRAM_API_MTX.lock();
    let mock = MockSysfsZramApi::read_mm_stat_context();
    let contents = " 1 2 3 4 5 6 7";
    mock.expect().returning(|| Ok(contents.to_string()));

    assert_eq!(
        ZramMmStat::load::<MockSysfsZramApi>().unwrap(),
        ZramMmStat {
            orig_data_size: 1,
            compr_data_size: 2,
            mem_used_total: 3,
            mem_limit: 4,
            mem_used_max: 5,
            same_pages: 6,
            pages_compacted: 7,
            huge_pages: None,
            huge_pages_since: None,
        }
    );
}

#[test]
fn test_zram_mm_stat_negative_mem_used_max() {
    let _m = ZRAM_API_MTX.lock();
    let mock = MockSysfsZramApi::read_mm_stat_context();
    let contents = " 1 2 3 4 -5 6 7 8 9";
    mock.expect().returning(|| Ok(contents.to_string()));

    assert_eq!(
        ZramMmStat::load::<MockSysfsZramApi>().unwrap(),
        ZramMmStat {
            orig_data_size: 1,
            compr_data_size: 2,
            mem_used_total: 3,
            mem_limit: 4,
            mem_used_max: -5,
            same_pages: 6,
            pages_compacted: 7,
            huge_pages: Some(8),
            huge_pages_since: Some(9),
        }
    );
}

#[test]
fn test_zram_mm_stat_fail_read() {
    let _m = ZRAM_API_MTX.lock();
    let mock = MockSysfsZramApi::read_mm_stat_context();
    mock.expect().returning(|| Err(std::io::Error::other("error")));

    assert!(ZramMmStat::load::<MockSysfsZramApi>().is_err());
}

#[test]
fn test_zram_mm_stat_less_values() {
    let _m = ZRAM_API_MTX.lock();
    let mock = MockSysfsZramApi::read_mm_stat_context();
    let contents = " 1 2 3 4 5 6";
    mock.expect().returning(|| Ok(contents.to_string()));

    assert!(ZramMmStat::load::<MockSysfsZramApi>().is_err());
}

#[test]
fn test_zram_mm_stat_invalid_value() {
    let _m = ZRAM_API_MTX.lock();
    let mock = MockSysfsZramApi::read_mm_stat_context();
    let contents = " 1 2 3 4 5 6 a";
    mock.expect().returning(|| Ok(contents.to_string()));

    assert!(ZramMmStat::load::<MockSysfsZramApi>().is_err());
}

#[test]
fn test_zram_bd_stat() {
    let _m = ZRAM_API_MTX.lock();
    let mock = MockSysfsZramApi::read_bd_stat_context();
    let contents = "1 2 3";
    mock.expect().returning(|| Ok(contents.to_string()));

    assert_eq!(
        ZramBdStat::load::<MockSysfsZramApi>().unwrap(),
        ZramBdStat { bd_count_pages: 1, bd_reads_pages: 2, bd_writes_pages: 3 }
    );
}

#[test]
fn test_zram_bd_stat_fail_read() {
    let _m = ZRAM_API_MTX.lock();
    let mock = MockSysfsZramApi::read_bd_stat_context();
    mock.expect().returning(|| Err(std::io::Error::other("error")));

    assert!(ZramBdStat::load::<MockSysfsZramApi>().is_err());
}

#[test]
fn test_zram_bd_stat_less_values() {
    let _m = ZRAM_API_MTX.lock();
    let mock = MockSysfsZramApi::read_bd_stat_context();
    let contents = "1 2";
    mock.expect().returning(|| Ok(contents.to_string()));

    assert!(ZramBdStat::load::<MockSysfsZramApi>().is_err());
}

#[test]
fn test_zram_bd_stat_invalid_value() {
    let _m = ZRAM_API_MTX.lock();
    let mock = MockSysfsZramApi::read_bd_stat_context();
    let contents = "1 2 a";
    mock.expect().returning(|| Ok(contents.to_string()));

    assert!(ZramBdStat::load::<MockSysfsZramApi>().is_err());
}
