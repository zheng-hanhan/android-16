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

use std::sync::LockResult;
use std::sync::MutexGuard;

use mockall::predicate::*;
use mockall::Sequence;

use super::*;
use crate::os::MockMeminfoApi;
use crate::os::MEMINFO_API_MTX;
use crate::time::TimeApi;
use crate::time::TimeApiImpl;
use crate::zram::MockSysfsZramApi;
use crate::zram::ZRAM_API_MTX;

const DEFAULT_TOTAL_ZRAM_SIZE: u64 = 4 << 30;
const DEFAULT_ZRAM_WRITEBACK_SIZE: u64 = 1 << 30;
const DEFAULT_PAGE_SIZE: u64 = 4096;

struct MockContext<'a> {
    read_backing_dev:
        crate::zram::__mock_MockSysfsZramApi_SysfsZramApi::__read_backing_dev::Context,
    writeback: crate::zram::__mock_MockSysfsZramApi_SysfsZramApi::__writeback::Context,
    write_writeback_limit:
        crate::zram::__mock_MockSysfsZramApi_SysfsZramApi::__write_writeback_limit::Context,
    read_writeback_limit:
        crate::zram::__mock_MockSysfsZramApi_SysfsZramApi::__read_writeback_limit::Context,
    set_idle: crate::zram::__mock_MockSysfsZramApi_SysfsZramApi::__set_idle::Context,
    read_meminfo: crate::os::__mock_MockMeminfoApi_MeminfoApi::__read_meminfo::Context,
    // Lock will be released after mock contexts are dropped.
    _meminfo_lock: LockResult<MutexGuard<'a, ()>>,
    _zram_lock: LockResult<MutexGuard<'a, ()>>,
}

impl MockContext<'_> {
    fn new() -> Self {
        let _zram_lock = ZRAM_API_MTX.lock();
        let _meminfo_lock = MEMINFO_API_MTX.lock();
        Self {
            read_backing_dev: MockSysfsZramApi::read_backing_dev_context(),
            writeback: MockSysfsZramApi::writeback_context(),
            write_writeback_limit: MockSysfsZramApi::write_writeback_limit_context(),
            read_writeback_limit: MockSysfsZramApi::read_writeback_limit_context(),
            set_idle: MockSysfsZramApi::set_idle_context(),
            read_meminfo: MockMeminfoApi::read_meminfo_context(),
            _meminfo_lock,
            _zram_lock,
        }
    }

    fn setup_default_meminfo(&self) {
        let meminfo = "MemTotal: 8144296 kB
            MemAvailable: 346452 kB";
        self.read_meminfo.expect().returning(|| Ok(meminfo.to_string()));
    }

    fn setup_default_writeback_limit_read(&self) {
        self.read_writeback_limit.expect().returning(|| Ok("100\n".to_string()));
    }
}

fn default_stats(params: &Params) -> Stats {
    Stats { orig_data_size: params.max_bytes, ..Default::default() }
}

#[test]
fn test_get_zram_writeback_status() {
    let mock = MockContext::new();
    mock.read_backing_dev.expect().returning(|| Ok("/dev/dm-1".to_string()));

    assert_eq!(
        get_zram_writeback_status::<MockSysfsZramApi>().unwrap(),
        ZramWritebackStatus::Activated
    );
}

#[test]
fn test_load_zram_writeback_disk_size_writeback_is_not_enabled() {
    let mock = MockContext::new();
    mock.read_backing_dev.expect().returning(|| Ok("none".to_string()));

    assert_eq!(
        get_zram_writeback_status::<MockSysfsZramApi>().unwrap(),
        ZramWritebackStatus::NotConfigured
    );
}

#[test]
fn test_get_zram_writeback_status_writeback_is_not_supported() {
    let mock = MockContext::new();
    mock.read_backing_dev
        .expect()
        .returning(|| Err(std::io::Error::new(std::io::ErrorKind::NotFound, "not found")));

    assert_eq!(
        get_zram_writeback_status::<MockSysfsZramApi>().unwrap(),
        ZramWritebackStatus::Unsupported
    );
}

#[test]
fn test_get_zram_writeback_status_failure() {
    let mock = MockContext::new();
    mock.read_backing_dev.expect().returning(|| Err(std::io::Error::other("error")));

    assert!(get_zram_writeback_status::<MockSysfsZramApi>().is_err());
}

#[test]
fn mark_and_flush_pages() {
    let mock = MockContext::new();
    let mut seq = Sequence::new();
    mock.setup_default_meminfo();
    mock.setup_default_writeback_limit_read();
    let params = Params::default();
    let stats = default_stats(&params);
    let suspend_history = SuspendHistory::new();
    let mut zram_writeback =
        ZramWriteback::new(DEFAULT_TOTAL_ZRAM_SIZE, DEFAULT_ZRAM_WRITEBACK_SIZE);

    mock.write_writeback_limit.expect().times(1).in_sequence(&mut seq).returning(|_| Ok(()));
    mock.set_idle.expect().times(1).in_sequence(&mut seq).returning(|_| Ok(()));
    mock.writeback.expect().times(1).in_sequence(&mut seq).returning(|_| Ok(()));
    mock.set_idle.expect().times(1).in_sequence(&mut seq).returning(|_| Ok(()));
    mock.writeback.expect().times(1).in_sequence(&mut seq).returning(|_| Ok(()));
    mock.writeback.expect().times(1).in_sequence(&mut seq).returning(|_| Ok(()));

    assert!(zram_writeback
        .mark_and_flush_pages::<MockSysfsZramApi, MockMeminfoApi>(
            &params,
            &stats,
            &suspend_history,
            TimeApiImpl::get_boot_time()
        )
        .is_ok());
}

#[test]
fn mark_and_flush_pages_before_backoff() {
    let mock = MockContext::new();
    mock.writeback.expect().returning(|_| Ok(()));
    mock.write_writeback_limit.expect().returning(|_| Ok(()));
    mock.set_idle.expect().returning(|_| Ok(()));
    mock.setup_default_meminfo();
    mock.setup_default_writeback_limit_read();
    let params = Params { backoff_duration: Duration::from_secs(100), ..Default::default() };
    let stats = default_stats(&params);
    let suspend_history = SuspendHistory::new();
    let base_time = TimeApiImpl::get_boot_time();
    let mut zram_writeback =
        ZramWriteback::new(DEFAULT_TOTAL_ZRAM_SIZE, DEFAULT_ZRAM_WRITEBACK_SIZE);
    assert!(zram_writeback
        .mark_and_flush_pages::<MockSysfsZramApi, MockMeminfoApi>(
            &params,
            &stats,
            &suspend_history,
            base_time
        )
        .is_ok());
    mock.writeback.checkpoint();

    mock.writeback.expect().times(0);

    assert!(matches!(
        zram_writeback.mark_and_flush_pages::<MockSysfsZramApi, MockMeminfoApi>(
            &params,
            &stats,
            &suspend_history,
            base_time.checked_add(Duration::from_secs(99)).unwrap()
        ),
        Err(Error::BackoffTime)
    ));
}

#[test]
fn mark_and_flush_pages_after_backoff() {
    let mock = MockContext::new();
    mock.writeback.expect().returning(|_| Ok(()));
    mock.write_writeback_limit.expect().returning(|_| Ok(()));
    mock.set_idle.expect().returning(|_| Ok(()));
    mock.setup_default_meminfo();
    mock.setup_default_writeback_limit_read();
    let params = Params { backoff_duration: Duration::from_secs(100), ..Default::default() };
    let stats = default_stats(&params);
    let suspend_history = SuspendHistory::new();
    let base_time = TimeApiImpl::get_boot_time();
    let mut zram_writeback =
        ZramWriteback::new(DEFAULT_TOTAL_ZRAM_SIZE, DEFAULT_ZRAM_WRITEBACK_SIZE);
    assert!(zram_writeback
        .mark_and_flush_pages::<MockSysfsZramApi, MockMeminfoApi>(
            &params,
            &stats,
            &suspend_history,
            base_time
        )
        .is_ok());
    mock.writeback.checkpoint();
    mock.write_writeback_limit.expect().returning(|_| Ok(()));
    mock.set_idle.expect().returning(|_| Ok(()));
    mock.setup_default_meminfo();
    mock.setup_default_writeback_limit_read();

    mock.writeback.expect().times(3).returning(|_| Ok(()));

    assert!(zram_writeback
        .mark_and_flush_pages::<MockSysfsZramApi, MockMeminfoApi>(
            &params,
            &stats,
            &suspend_history,
            base_time.checked_add(Duration::from_secs(100)).unwrap()
        )
        .is_ok());
}

#[test]
fn mark_and_flush_pages_idle_time() {
    let mock = MockContext::new();
    mock.write_writeback_limit.expect().returning(|_| Ok(()));
    mock.writeback.expect().returning(|_| Ok(()));
    let meminfo = "MemTotal: 10000 kB
        MemAvailable: 8000 kB";
    mock.read_meminfo.expect().returning(|| Ok(meminfo.to_string()));
    mock.setup_default_writeback_limit_read();
    let params = Params {
        min_idle: Duration::from_secs(3600),
        max_idle: Duration::from_secs(4000),
        ..Default::default()
    };
    let stats = default_stats(&params);
    let suspend_history = SuspendHistory::new();
    let mut zram_writeback =
        ZramWriteback::new(DEFAULT_TOTAL_ZRAM_SIZE, DEFAULT_ZRAM_WRITEBACK_SIZE);

    mock.set_idle.expect().with(eq("3747")).times(2).returning(|_| Ok(()));

    assert!(zram_writeback
        .mark_and_flush_pages::<MockSysfsZramApi, MockMeminfoApi>(
            &params,
            &stats,
            &suspend_history,
            TimeApiImpl::get_boot_time()
        )
        .is_ok());
}

#[test]
fn mark_and_flush_pages_idle_time_adjusted_by_suspend_duration() {
    let mock = MockContext::new();
    mock.write_writeback_limit.expect().returning(|_| Ok(()));
    mock.writeback.expect().returning(|_| Ok(()));
    let meminfo = "MemTotal: 10000 kB
        MemAvailable: 8000 kB";
    mock.read_meminfo.expect().returning(|| Ok(meminfo.to_string()));
    mock.setup_default_writeback_limit_read();
    let params = Params {
        min_idle: Duration::from_secs(3600),
        max_idle: Duration::from_secs(4000),
        ..Default::default()
    };
    let stats = default_stats(&params);
    let mut suspend_history = SuspendHistory::new();
    let boot_now = BootTime::from_duration(Duration::from_secs(12345));
    suspend_history.record_suspend_duration(Duration::from_secs(1000), boot_now, params.max_idle);
    let mut zram_writeback =
        ZramWriteback::new(DEFAULT_TOTAL_ZRAM_SIZE, DEFAULT_ZRAM_WRITEBACK_SIZE);

    mock.set_idle.expect().with(eq("4747")).times(2).returning(|_| Ok(()));

    assert!(zram_writeback
        .mark_and_flush_pages::<MockSysfsZramApi, MockMeminfoApi>(
            &params,
            &stats,
            &suspend_history,
            boot_now
        )
        .is_ok());
}

#[test]
fn mark_and_flush_pages_calculate_idle_failure() {
    let mock = MockContext::new();
    mock.write_writeback_limit.expect().returning(|_| Ok(()));
    mock.writeback.expect().returning(|_| Ok(()));
    mock.setup_default_writeback_limit_read();
    let params = Params {
        min_idle: Duration::from_secs(4000),
        max_idle: Duration::from_secs(3600),
        ..Default::default()
    };
    let stats = default_stats(&params);
    let suspend_history = SuspendHistory::new();
    let mut zram_writeback =
        ZramWriteback::new(DEFAULT_TOTAL_ZRAM_SIZE, DEFAULT_ZRAM_WRITEBACK_SIZE);

    assert!(matches!(
        zram_writeback.mark_and_flush_pages::<MockSysfsZramApi, MockMeminfoApi>(
            &params,
            &stats,
            &suspend_history,
            TimeApiImpl::get_boot_time()
        ),
        Err(Error::CalculateIdle(_))
    ));
}

#[test]
fn mark_and_flush_pages_mark_idle_failure() {
    let mock = MockContext::new();
    mock.write_writeback_limit.expect().returning(|_| Ok(()));
    mock.writeback.expect().returning(|_| Ok(()));
    mock.setup_default_meminfo();
    mock.setup_default_writeback_limit_read();
    let params = Params::default();
    let stats = default_stats(&params);
    let suspend_history = SuspendHistory::new();
    let mut zram_writeback =
        ZramWriteback::new(DEFAULT_TOTAL_ZRAM_SIZE, DEFAULT_ZRAM_WRITEBACK_SIZE);

    mock.set_idle.expect().returning(|_| Err(std::io::Error::other("error")));

    assert!(matches!(
        zram_writeback.mark_and_flush_pages::<MockSysfsZramApi, MockMeminfoApi>(
            &params,
            &stats,
            &suspend_history,
            TimeApiImpl::get_boot_time()
        ),
        Err(Error::MarkIdle(_))
    ));
}

#[test]
fn mark_and_flush_pages_skip_huge_idle() {
    let mock = MockContext::new();
    mock.write_writeback_limit.expect().returning(|_| Ok(()));
    mock.set_idle.expect().returning(|_| Ok(()));
    mock.setup_default_meminfo();
    mock.setup_default_writeback_limit_read();
    let params = Params { huge_idle: false, ..Default::default() };
    let stats = default_stats(&params);
    let suspend_history = SuspendHistory::new();
    let mut zram_writeback =
        ZramWriteback::new(DEFAULT_TOTAL_ZRAM_SIZE, DEFAULT_ZRAM_WRITEBACK_SIZE);

    mock.writeback.expect().with(eq("huge_idle")).times(0).returning(|_| Ok(()));
    mock.writeback.expect().with(eq("idle")).times(1).returning(|_| Ok(()));
    mock.writeback.expect().with(eq("huge")).times(1).returning(|_| Ok(()));

    assert!(zram_writeback
        .mark_and_flush_pages::<MockSysfsZramApi, MockMeminfoApi>(
            &params,
            &stats,
            &suspend_history,
            TimeApiImpl::get_boot_time()
        )
        .is_ok());
}

#[test]
fn mark_and_flush_pages_skip_idle() {
    let mock = MockContext::new();
    mock.write_writeback_limit.expect().returning(|_| Ok(()));
    mock.set_idle.expect().returning(|_| Ok(()));
    mock.setup_default_meminfo();
    mock.setup_default_writeback_limit_read();
    let params = Params { idle: false, ..Default::default() };
    let stats = default_stats(&params);
    let suspend_history = SuspendHistory::new();
    let mut zram_writeback =
        ZramWriteback::new(DEFAULT_TOTAL_ZRAM_SIZE, DEFAULT_ZRAM_WRITEBACK_SIZE);

    mock.writeback.expect().with(eq("huge_idle")).times(1).returning(|_| Ok(()));
    mock.writeback.expect().with(eq("idle")).times(0).returning(|_| Ok(()));
    mock.writeback.expect().with(eq("huge")).times(1).returning(|_| Ok(()));

    assert!(zram_writeback
        .mark_and_flush_pages::<MockSysfsZramApi, MockMeminfoApi>(
            &params,
            &stats,
            &suspend_history,
            TimeApiImpl::get_boot_time()
        )
        .is_ok());
}

#[test]
fn mark_and_flush_pages_skip_huge() {
    let mock = MockContext::new();
    mock.write_writeback_limit.expect().returning(|_| Ok(()));
    mock.set_idle.expect().returning(|_| Ok(()));
    mock.setup_default_meminfo();
    mock.setup_default_writeback_limit_read();
    let params = Params { huge: false, ..Default::default() };
    let stats = default_stats(&params);
    let suspend_history = SuspendHistory::new();
    let mut zram_writeback =
        ZramWriteback::new(DEFAULT_TOTAL_ZRAM_SIZE, DEFAULT_ZRAM_WRITEBACK_SIZE);

    mock.writeback.expect().with(eq("huge_idle")).times(1).returning(|_| Ok(()));
    mock.writeback.expect().with(eq("idle")).times(1).returning(|_| Ok(()));
    mock.writeback.expect().with(eq("huge")).times(0).returning(|_| Ok(()));

    assert!(zram_writeback
        .mark_and_flush_pages::<MockSysfsZramApi, MockMeminfoApi>(
            &params,
            &stats,
            &suspend_history,
            TimeApiImpl::get_boot_time()
        )
        .is_ok());
}

#[test]
fn mark_and_flush_pages_write_limit_from_orig_data_size() {
    let mock = MockContext::new();
    mock.writeback.expect().returning(|_| Ok(()));
    mock.set_idle.expect().returning(|_| Ok(()));
    mock.setup_default_meminfo();
    mock.setup_default_writeback_limit_read();
    let params = Params {
        max_bytes: 600 * DEFAULT_PAGE_SIZE,
        min_bytes: 10 * DEFAULT_PAGE_SIZE,
        ..Default::default()
    };
    // zram utilization is 25%
    let stats = Stats { orig_data_size: 500 * DEFAULT_PAGE_SIZE, ..Default::default() };
    let suspend_history = SuspendHistory::new();
    let mut zram_writeback = ZramWriteback::new_with_page_size(
        2000 * DEFAULT_PAGE_SIZE,
        1000 * DEFAULT_PAGE_SIZE,
        DEFAULT_PAGE_SIZE,
    );

    // Writeback limit is 25% of max_bytes.
    mock.write_writeback_limit.expect().with(eq("150")).times(1).returning(|_| Ok(()));

    assert!(zram_writeback
        .mark_and_flush_pages::<MockSysfsZramApi, MockMeminfoApi>(
            &params,
            &stats,
            &suspend_history,
            TimeApiImpl::get_boot_time()
        )
        .is_ok());
}

#[test]
fn mark_and_flush_pages_write_limit_from_orig_data_size_with_big_page_size() {
    let mock = MockContext::new();
    mock.writeback.expect().returning(|_| Ok(()));
    mock.set_idle.expect().returning(|_| Ok(()));
    mock.setup_default_meminfo();
    mock.setup_default_writeback_limit_read();
    let page_size = 2 * DEFAULT_PAGE_SIZE;
    let params =
        Params { max_bytes: 600 * page_size, min_bytes: 10 * page_size, ..Default::default() };
    // zram utilization is 25%
    let stats = Stats { orig_data_size: 500 * page_size, ..Default::default() };
    let suspend_history = SuspendHistory::new();
    let mut zram_writeback =
        ZramWriteback::new_with_page_size(2000 * page_size, 1000 * page_size, page_size);

    // Writeback limit is 25% of maxPages.
    mock.write_writeback_limit.expect().with(eq("150")).times(1).returning(|_| Ok(()));

    assert!(zram_writeback
        .mark_and_flush_pages::<MockSysfsZramApi, MockMeminfoApi>(
            &params,
            &stats,
            &suspend_history,
            TimeApiImpl::get_boot_time()
        )
        .is_ok());
}

#[test]
fn mark_and_flush_pages_write_limit_capped_by_current_writeback_size() {
    let mock = MockContext::new();
    mock.writeback.expect().returning(|_| Ok(()));
    mock.set_idle.expect().returning(|_| Ok(()));
    mock.setup_default_meminfo();
    mock.setup_default_writeback_limit_read();
    let params = Params {
        max_bytes: 600 * DEFAULT_PAGE_SIZE,
        min_bytes: 10 * DEFAULT_PAGE_SIZE,
        ..Default::default()
    };
    // zram utilization is 25%
    let stats =
        Stats { orig_data_size: 500 * DEFAULT_PAGE_SIZE, current_writeback_pages: 1000 - 50 };
    let suspend_history = SuspendHistory::new();
    let mut zram_writeback = ZramWriteback::new_with_page_size(
        2000 * DEFAULT_PAGE_SIZE,
        1000 * DEFAULT_PAGE_SIZE,
        DEFAULT_PAGE_SIZE,
    );

    // Writeback disk only has 50 pages left.
    mock.write_writeback_limit.expect().with(eq("50")).times(1).returning(|_| Ok(()));

    assert!(zram_writeback
        .mark_and_flush_pages::<MockSysfsZramApi, MockMeminfoApi>(
            &params,
            &stats,
            &suspend_history,
            TimeApiImpl::get_boot_time()
        )
        .is_ok());
}

#[test]
fn mark_and_flush_pages_write_limit_capped_by_min_pages() {
    let mock = MockContext::new();
    mock.writeback.expect().returning(|_| Ok(()));
    mock.set_idle.expect().returning(|_| Ok(()));
    mock.setup_default_meminfo();
    mock.setup_default_writeback_limit_read();
    let params = Params {
        max_bytes: 500 * DEFAULT_PAGE_SIZE,
        min_bytes: 6 * DEFAULT_PAGE_SIZE,
        ..Default::default()
    };
    // zram utilization is 1%
    let stats = Stats { orig_data_size: 20 * DEFAULT_PAGE_SIZE, ..Default::default() };
    let suspend_history = SuspendHistory::new();
    let mut zram_writeback = ZramWriteback::new_with_page_size(
        2000 * DEFAULT_PAGE_SIZE,
        1000 * DEFAULT_PAGE_SIZE,
        DEFAULT_PAGE_SIZE,
    );

    // Writeback limit is 5 pages (= 1% of 500 pages). But it is lower than min pages.
    mock.write_writeback_limit.expect().times(0);

    assert!(matches!(
        zram_writeback.mark_and_flush_pages::<MockSysfsZramApi, MockMeminfoApi>(
            &params,
            &stats,
            &suspend_history,
            TimeApiImpl::get_boot_time()
        ),
        Err(Error::Limit)
    ));
}

#[test]
fn mark_and_flush_pages_write_limit_capped_by_daily_limit_with_no_log() {
    let mock = MockContext::new();
    mock.writeback.expect().returning(|_| Ok(()));
    mock.set_idle.expect().returning(|_| Ok(()));
    mock.setup_default_meminfo();
    mock.setup_default_writeback_limit_read();
    let params = Params {
        max_bytes: 600 * DEFAULT_PAGE_SIZE,
        min_bytes: 10 * DEFAULT_PAGE_SIZE,
        max_bytes_per_day: 100 * DEFAULT_PAGE_SIZE,
        ..Default::default()
    };
    // zram utilization is 25%
    let stats = Stats { orig_data_size: 500 * DEFAULT_PAGE_SIZE, current_writeback_pages: 0 };
    let suspend_history = SuspendHistory::new();
    let mut zram_writeback = ZramWriteback::new_with_page_size(
        2000 * DEFAULT_PAGE_SIZE,
        1000 * DEFAULT_PAGE_SIZE,
        DEFAULT_PAGE_SIZE,
    );

    // Writeback limit is 25% of max_bytes. But use smaller maxPagesPerDay as the limit.
    mock.write_writeback_limit.expect().with(eq("100")).times(1).returning(|_| Ok(()));

    assert!(zram_writeback
        .mark_and_flush_pages::<MockSysfsZramApi, MockMeminfoApi>(
            &params,
            &stats,
            &suspend_history,
            TimeApiImpl::get_boot_time()
        )
        .is_ok());
}

#[test]
fn mark_and_flush_pages_write_limit_capped_by_daily_limit() {
    let mock = MockContext::new();
    mock.writeback.expect().returning(|_| Ok(()));
    mock.set_idle.expect().returning(|_| Ok(()));
    mock.setup_default_meminfo();
    let params = Params {
        max_bytes: 600 * DEFAULT_PAGE_SIZE,
        min_bytes: 10 * DEFAULT_PAGE_SIZE,
        max_bytes_per_day: 100 * DEFAULT_PAGE_SIZE,
        ..Default::default()
    };
    let stats = Stats { orig_data_size: 500 * DEFAULT_PAGE_SIZE, current_writeback_pages: 0 };
    let suspend_history = SuspendHistory::new();
    let mut zram_writeback = ZramWriteback::new_with_page_size(
        2000 * DEFAULT_PAGE_SIZE,
        1000 * DEFAULT_PAGE_SIZE,
        DEFAULT_PAGE_SIZE,
    );
    let base_point = TimeApiImpl::get_boot_time();

    // Sets 100 as the daily limit for the first time.
    mock.write_writeback_limit.expect().with(eq("100")).times(1).returning(|_| Ok(()));
    // Load updated writeback_limit as the initial value.
    mock.read_writeback_limit.expect().times(1).returning(|| Ok("100".to_string()));
    // On first writeback, 40 pages are written back.
    mock.read_writeback_limit.expect().returning(|| Ok("60".to_string()));

    assert!(zram_writeback
        .mark_and_flush_pages::<MockSysfsZramApi, MockMeminfoApi>(
            &params,
            &stats,
            &suspend_history,
            base_point
        )
        .is_ok());

    // Daily limit with the history is applied on second markAndFlushPages.
    mock.write_writeback_limit.expect().with(eq("60")).times(1).returning(|_| Ok(()));

    assert!(zram_writeback
        .mark_and_flush_pages::<MockSysfsZramApi, MockMeminfoApi>(
            &params,
            &stats,
            &suspend_history,
            base_point.checked_add(Duration::from_secs(3600)).unwrap()
        )
        .is_ok());
}

#[test]
fn mark_and_flush_pages_write_limit_capped_by_daily_limit_expired() {
    let mock = MockContext::new();
    mock.writeback.expect().returning(|_| Ok(()));
    mock.set_idle.expect().returning(|_| Ok(()));
    mock.setup_default_meminfo();
    let params = Params {
        max_bytes: 600 * DEFAULT_PAGE_SIZE,
        min_bytes: 10 * DEFAULT_PAGE_SIZE,
        max_bytes_per_day: 100 * DEFAULT_PAGE_SIZE,
        ..Default::default()
    };
    let stats = Stats { orig_data_size: 500 * DEFAULT_PAGE_SIZE, current_writeback_pages: 0 };
    let suspend_history = SuspendHistory::new();
    let mut zram_writeback = ZramWriteback::new_with_page_size(
        2000 * DEFAULT_PAGE_SIZE,
        1000 * DEFAULT_PAGE_SIZE,
        DEFAULT_PAGE_SIZE,
    );
    let base_point = TimeApiImpl::get_boot_time();

    // Sets 100 as the daily limit for the first time.
    mock.write_writeback_limit.expect().with(eq("100")).times(1).returning(|_| Ok(()));
    // Load updated writeback_limit as the initial value.
    mock.read_writeback_limit.expect().times(1).returning(|| Ok("100\n".to_string()));
    // On first writeback, 40 pages are written back.
    mock.read_writeback_limit.expect().returning(|| Ok("60\n".to_string()));

    assert!(zram_writeback
        .mark_and_flush_pages::<MockSysfsZramApi, MockMeminfoApi>(
            &params,
            &stats,
            &suspend_history,
            base_point
        )
        .is_ok());

    // On second time, the history is expired after 24 hours.
    mock.write_writeback_limit.expect().with(eq("100")).times(1).returning(|_| Ok(()));
    assert!(zram_writeback
        .mark_and_flush_pages::<MockSysfsZramApi, MockMeminfoApi>(
            &params,
            &stats,
            &suspend_history,
            base_point.checked_add(Duration::from_secs(24 * 3600)).unwrap()
        )
        .is_ok());
}

#[test]
fn mark_and_flush_pages_skip_on_write_limit() {
    let mock = MockContext::new();
    mock.write_writeback_limit.expect().returning(|_| Ok(()));
    mock.set_idle.expect().returning(|_| Ok(()));
    mock.setup_default_meminfo();
    let params = Params::default();
    let stats = default_stats(&params);
    let suspend_history = SuspendHistory::new();
    let mut zram_writeback = ZramWriteback::new_with_page_size(
        DEFAULT_TOTAL_ZRAM_SIZE,
        DEFAULT_ZRAM_WRITEBACK_SIZE,
        DEFAULT_PAGE_SIZE,
    );

    // Load updated writeback_limit as the initial value.
    mock.read_writeback_limit.expect().times(1).returning(|| Ok("100\n".to_string()));
    // On first writeback, writeback limit becomes zero and skip following writeback.
    mock.read_writeback_limit.expect().returning(|| Ok("0\n".to_string()));
    mock.writeback.expect().with(eq("huge_idle")).times(1).returning(|_| Ok(()));
    mock.writeback.expect().with(eq("idle")).times(0);
    mock.writeback.expect().with(eq("huge")).times(0);

    assert!(zram_writeback
        .mark_and_flush_pages::<MockSysfsZramApi, MockMeminfoApi>(
            &params,
            &stats,
            &suspend_history,
            TimeApiImpl::get_boot_time()
        )
        .is_ok());
}

#[test]
fn mark_and_flush_pages_skip_next_by_daily_limit() {
    let mock = MockContext::new();
    mock.writeback.expect().returning(|_| Ok(()));
    mock.write_writeback_limit.expect().returning(|_| Ok(()));
    mock.set_idle.expect().returning(|_| Ok(()));
    mock.setup_default_meminfo();
    let params = Params { max_bytes_per_day: 100 * DEFAULT_PAGE_SIZE, ..Default::default() };
    let stats = default_stats(&params);
    let suspend_history = SuspendHistory::new();
    let mut zram_writeback = ZramWriteback::new_with_page_size(
        DEFAULT_TOTAL_ZRAM_SIZE,
        DEFAULT_ZRAM_WRITEBACK_SIZE,
        DEFAULT_PAGE_SIZE,
    );
    let base_point = TimeApiImpl::get_boot_time();

    // Load updated writeback_limit as the initial value.
    mock.read_writeback_limit.expect().times(1).returning(|| Ok("100\n".to_string()));
    // On first writeback, writeback limit becomes zero and skip following writeback.
    mock.read_writeback_limit.expect().returning(|| Ok("0\n".to_string()));
    assert!(zram_writeback
        .mark_and_flush_pages::<MockSysfsZramApi, MockMeminfoApi>(
            &params,
            &stats,
            &suspend_history,
            base_point
        )
        .is_ok());

    mock.write_writeback_limit.checkpoint();
    mock.write_writeback_limit.expect().times(0);
    assert!(matches!(
        zram_writeback.mark_and_flush_pages::<MockSysfsZramApi, MockMeminfoApi>(
            &params,
            &stats,
            &suspend_history,
            base_point.checked_add(params.backoff_duration).unwrap()
        ),
        Err(Error::Limit)
    ));

    mock.write_writeback_limit.checkpoint();
    mock.writeback.expect().returning(|_| Ok(()));
    mock.set_idle.expect().returning(|_| Ok(()));
    mock.setup_default_meminfo();
    mock.setup_default_writeback_limit_read();
    mock.write_writeback_limit.expect().returning(|_| Ok(()));
    // writeback succeeds 24 hours after.
    assert!(zram_writeback
        .mark_and_flush_pages::<MockSysfsZramApi, MockMeminfoApi>(
            &params,
            &stats,
            &suspend_history,
            base_point.checked_add(Duration::from_secs(24 * 3600)).unwrap()
        )
        .is_ok());
}

#[test]
fn mark_and_flush_pages_fails_to_record_history_by_writeback_error() {
    let mock = MockContext::new();
    mock.set_idle.expect().returning(|_| Ok(()));
    mock.setup_default_meminfo();
    let params = Params { max_bytes_per_day: 100 * DEFAULT_PAGE_SIZE, ..Default::default() };
    let stats = default_stats(&params);
    let suspend_history = SuspendHistory::new();
    let mut zram_writeback = ZramWriteback::new_with_page_size(
        DEFAULT_TOTAL_ZRAM_SIZE,
        DEFAULT_ZRAM_WRITEBACK_SIZE,
        DEFAULT_PAGE_SIZE,
    );
    let base_point = TimeApiImpl::get_boot_time();

    mock.write_writeback_limit.expect().with(eq("100")).times(1).returning(|_| Ok(()));
    mock.read_writeback_limit.expect().times(1).returning(|| Ok("100\n".to_string()));
    mock.read_writeback_limit.expect().times(1).returning(|| Ok("30\n".to_string()));
    mock.writeback.expect().times(1).returning(|_| Err(std::io::Error::other("error")));
    assert!(zram_writeback
        .mark_and_flush_pages::<MockSysfsZramApi, MockMeminfoApi>(
            &params,
            &stats,
            &suspend_history,
            base_point
        )
        .is_err());

    mock.write_writeback_limit.checkpoint();
    // 70 pages of previous writeback is discounted.
    mock.write_writeback_limit.expect().with(eq("30")).times(1).returning(|_| Ok(()));
    mock.read_writeback_limit.expect().returning(|| Ok("30\n".to_string()));
    mock.writeback.expect().returning(|_| Ok(()));
    zram_writeback
        .mark_and_flush_pages::<MockSysfsZramApi, MockMeminfoApi>(
            &params,
            &stats,
            &suspend_history,
            base_point.checked_add(params.backoff_duration).unwrap(),
        )
        .unwrap();
}

#[test]
fn mark_and_flush_pages_fails_to_record_history_by_limit_load_error() {
    let mock = MockContext::new();
    mock.writeback.expect().returning(|_| Ok(()));
    mock.write_writeback_limit.expect().returning(|_| Ok(()));
    mock.set_idle.expect().returning(|_| Ok(()));
    mock.setup_default_meminfo();
    let params = Params { max_bytes_per_day: 100 * DEFAULT_PAGE_SIZE, ..Default::default() };
    let stats = default_stats(&params);
    let suspend_history = SuspendHistory::new();
    let mut zram_writeback = ZramWriteback::new_with_page_size(
        DEFAULT_TOTAL_ZRAM_SIZE,
        DEFAULT_ZRAM_WRITEBACK_SIZE,
        DEFAULT_PAGE_SIZE,
    );
    let base_point = TimeApiImpl::get_boot_time();

    mock.read_writeback_limit.expect().times(1).returning(|| Ok("100\n".to_string()));
    // read writeback_limit fails just after writeback.
    mock.read_writeback_limit.expect().returning(|| Err(std::io::Error::other("error")));
    assert!(zram_writeback
        .mark_and_flush_pages::<MockSysfsZramApi, MockMeminfoApi>(
            &params,
            &stats,
            &suspend_history,
            base_point
        )
        .is_ok());

    mock.write_writeback_limit.checkpoint();
    mock.write_writeback_limit.expect().times(0);
    assert!(matches!(
        zram_writeback.mark_and_flush_pages::<MockSysfsZramApi, MockMeminfoApi>(
            &params,
            &stats,
            &suspend_history,
            base_point.checked_add(params.backoff_duration).unwrap()
        ),
        Err(Error::Limit)
    ));
}

#[test]
fn mark_and_flush_pages_eio_due_consuming_writeback_limit() {
    let mock = MockContext::new();
    mock.write_writeback_limit.expect().returning(|_| Ok(()));
    mock.set_idle.expect().returning(|_| Ok(()));
    mock.setup_default_meminfo();
    let params = Params { max_bytes_per_day: 100 * DEFAULT_PAGE_SIZE, ..Default::default() };
    let stats = default_stats(&params);
    let suspend_history = SuspendHistory::new();
    let mut zram_writeback = ZramWriteback::new_with_page_size(
        DEFAULT_TOTAL_ZRAM_SIZE,
        DEFAULT_ZRAM_WRITEBACK_SIZE,
        DEFAULT_PAGE_SIZE,
    );
    let base_point = TimeApiImpl::get_boot_time();

    mock.read_writeback_limit.expect().times(1).returning(|| Ok("100\n".to_string()));
    mock.read_writeback_limit.expect().times(1).returning(|| Ok("0\n".to_string()));
    mock.writeback.expect().returning(|_| Err(std::io::Error::from_raw_os_error(libc::EIO)));
    // EIO with zero writeback limit is not considered as an error.
    assert!(zram_writeback
        .mark_and_flush_pages::<MockSysfsZramApi, MockMeminfoApi>(
            &params,
            &stats,
            &suspend_history,
            base_point
        )
        .is_ok());

    mock.write_writeback_limit.checkpoint();
    mock.write_writeback_limit.expect().times(0);
    assert!(matches!(
        zram_writeback.mark_and_flush_pages::<MockSysfsZramApi, MockMeminfoApi>(
            &params,
            &stats,
            &suspend_history,
            base_point.checked_add(params.backoff_duration).unwrap()
        ),
        Err(Error::Limit)
    ));
}

#[test]
fn mark_and_flush_pages_output_details() {
    let mock = MockContext::new();
    mock.writeback.expect().returning(|_| Ok(()));
    mock.set_idle.expect().returning(|_| Ok(()));
    mock.setup_default_meminfo();
    let params = Params {
        max_bytes: 600 * DEFAULT_PAGE_SIZE,
        min_bytes: 10 * DEFAULT_PAGE_SIZE,
        max_bytes_per_day: 100 * DEFAULT_PAGE_SIZE,
        ..Default::default()
    };
    // zram utilization is 25%
    let stats = Stats { orig_data_size: 500 * DEFAULT_PAGE_SIZE, ..Default::default() };
    let suspend_history = SuspendHistory::new();
    let mut zram_writeback = ZramWriteback::new_with_page_size(
        2000 * DEFAULT_PAGE_SIZE,
        1000 * DEFAULT_PAGE_SIZE,
        DEFAULT_PAGE_SIZE,
    );

    mock.write_writeback_limit.expect().with(eq("100")).times(1).returning(|_| Ok(()));
    mock.read_writeback_limit.expect().times(1).returning(|| Ok("90\n".to_string()));
    // 10 huge_idle pages were written back.
    mock.read_writeback_limit.expect().times(1).returning(|| Ok("80\n".to_string()));
    // 20 idle pages were written back.
    mock.read_writeback_limit.expect().times(1).returning(|| Ok("60\n".to_string()));
    // 25 huge pages were written back.
    mock.read_writeback_limit.expect().times(1).returning(|| Ok("35\n".to_string()));

    let result = zram_writeback.mark_and_flush_pages::<MockSysfsZramApi, MockMeminfoApi>(
        &params,
        &stats,
        &suspend_history,
        TimeApiImpl::get_boot_time(),
    );
    assert!(result.is_ok());
    let details = result.unwrap();
    assert_eq!(details.limit_pages, 150);
    assert_eq!(details.daily_limit_pages, 100);
    assert_eq!(details.actual_limit_pages, 90);
    assert_eq!(details.huge_idle.written_pages, 10);
    assert_eq!(details.idle.written_pages, 20);
    assert_eq!(details.huge.written_pages, 25);
}
