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

struct MockContext<'a> {
    read_recomp_algorithm:
        crate::zram::__mock_MockSysfsZramApi_SysfsZramApi::__read_recomp_algorithm::Context,
    recompress: crate::zram::__mock_MockSysfsZramApi_SysfsZramApi::__recompress::Context,
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
            read_recomp_algorithm: MockSysfsZramApi::read_recomp_algorithm_context(),
            recompress: MockSysfsZramApi::recompress_context(),
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
}

#[test]
fn get_zram_recompression_status_not_configured() {
    let mock = MockContext::new();
    mock.read_recomp_algorithm.expect().returning(|| Ok("".to_string()));
    assert_eq!(
        get_zram_recompression_status::<MockSysfsZramApi>().unwrap(),
        ZramRecompressionStatus::NotConfigured
    );
}

#[test]
fn get_zram_recompression_status_activated() {
    let mock = MockContext::new();
    mock.read_recomp_algorithm.expect().returning(|| Ok("zstd".to_string()));
    assert_eq!(
        get_zram_recompression_status::<MockSysfsZramApi>().unwrap(),
        ZramRecompressionStatus::Activated
    );
}

#[test]
fn get_zram_recompression_status_unsupported() {
    let mock = MockContext::new();
    mock.read_recomp_algorithm
        .expect()
        .returning(|| Err(std::io::Error::from_raw_os_error(libc::ENOENT)));
    assert_eq!(
        get_zram_recompression_status::<MockSysfsZramApi>().unwrap(),
        ZramRecompressionStatus::Unsupported
    );
}

#[test]
fn test_get_zram_recompression_status_failure() {
    let mock = MockContext::new();
    mock.read_recomp_algorithm.expect().returning(|| Err(std::io::Error::other("error")));

    assert!(get_zram_recompression_status::<MockSysfsZramApi>().is_err());
}

#[test]
fn mark_and_recompress() {
    let mock = MockContext::new();
    let mut seq = Sequence::new();
    mock.setup_default_meminfo();
    let params = Params { threshold_bytes: 0, ..Default::default() };
    let suspend_history = SuspendHistory::new();
    let mut zram_recompression = ZramRecompression::new();

    mock.set_idle.expect().times(1).in_sequence(&mut seq).returning(|_| Ok(()));
    mock.recompress
        .expect()
        .with(eq("type=huge_idle"))
        .times(1)
        .in_sequence(&mut seq)
        .returning(|_| Ok(()));
    mock.set_idle.expect().times(1).in_sequence(&mut seq).returning(|_| Ok(()));
    mock.recompress
        .expect()
        .with(eq("type=idle"))
        .times(1)
        .in_sequence(&mut seq)
        .returning(|_| Ok(()));
    mock.recompress
        .expect()
        .with(eq("type=huge"))
        .times(1)
        .in_sequence(&mut seq)
        .returning(|_| Ok(()));

    assert!(zram_recompression
        .mark_and_recompress::<MockSysfsZramApi, MockMeminfoApi>(
            &params,
            &suspend_history,
            TimeApiImpl::get_boot_time()
        )
        .is_ok());
}

#[test]
fn mark_and_recompress_with_threshold() {
    let mock = MockContext::new();
    mock.set_idle.expect().returning(|_| Ok(()));
    mock.setup_default_meminfo();
    let params = Params { threshold_bytes: 12345, ..Default::default() };
    let suspend_history = SuspendHistory::new();
    let mut zram_recompression = ZramRecompression::new();

    mock.recompress
        .expect()
        .with(eq("type=huge_idle threshold=12345"))
        .times(1)
        .returning(|_| Ok(()));
    mock.recompress.expect().with(eq("type=idle threshold=12345")).times(1).returning(|_| Ok(()));
    mock.recompress.expect().with(eq("type=huge threshold=12345")).times(1).returning(|_| Ok(()));

    assert!(zram_recompression
        .mark_and_recompress::<MockSysfsZramApi, MockMeminfoApi>(
            &params,
            &suspend_history,
            TimeApiImpl::get_boot_time(),
        )
        .is_ok());
}

#[test]
fn mark_and_recompress_before_backoff() {
    let mock = MockContext::new();
    mock.recompress.expect().returning(|_| Ok(()));
    mock.set_idle.expect().returning(|_| Ok(()));
    mock.setup_default_meminfo();
    let params = Params {
        backoff_duration: Duration::from_secs(100),
        threshold_bytes: 0,
        ..Default::default()
    };
    let suspend_history = SuspendHistory::new();
    let base_time = TimeApiImpl::get_boot_time();
    let mut zram_recompression = ZramRecompression::new();
    assert!(zram_recompression
        .mark_and_recompress::<MockSysfsZramApi, MockMeminfoApi>(
            &params,
            &suspend_history,
            base_time,
        )
        .is_ok());
    mock.recompress.checkpoint();

    mock.recompress.expect().times(0);

    assert!(matches!(
        zram_recompression.mark_and_recompress::<MockSysfsZramApi, MockMeminfoApi>(
            &params,
            &suspend_history,
            base_time.checked_add(Duration::from_secs(99)).unwrap(),
        ),
        Err(Error::BackoffTime)
    ));
}

#[test]
fn mark_and_recompress_after_backoff() {
    let mock = MockContext::new();
    mock.recompress.expect().returning(|_| Ok(()));
    mock.set_idle.expect().returning(|_| Ok(()));
    mock.setup_default_meminfo();
    let params = Params {
        backoff_duration: Duration::from_secs(100),
        threshold_bytes: 0,
        ..Default::default()
    };
    let suspend_history = SuspendHistory::new();
    let base_time = TimeApiImpl::get_boot_time();
    let mut zram_recompression = ZramRecompression::new();
    assert!(zram_recompression
        .mark_and_recompress::<MockSysfsZramApi, MockMeminfoApi>(
            &params,
            &suspend_history,
            base_time,
        )
        .is_ok());
    mock.recompress.checkpoint();
    mock.set_idle.expect().returning(|_| Ok(()));
    mock.setup_default_meminfo();

    mock.recompress.expect().times(3).returning(|_| Ok(()));

    assert!(zram_recompression
        .mark_and_recompress::<MockSysfsZramApi, MockMeminfoApi>(
            &params,
            &suspend_history,
            base_time.checked_add(Duration::from_secs(100)).unwrap()
        )
        .is_ok());
}

#[test]
fn mark_and_recompress_idle_time() {
    let mock = MockContext::new();
    mock.recompress.expect().returning(|_| Ok(()));
    let meminfo = "MemTotal: 10000 kB
        MemAvailable: 8000 kB";
    mock.read_meminfo.expect().returning(|| Ok(meminfo.to_string()));
    let params = Params {
        min_idle: Duration::from_secs(3600),
        max_idle: Duration::from_secs(4000),
        threshold_bytes: 0,
        ..Default::default()
    };
    let suspend_history = SuspendHistory::new();
    let mut zram_recompression = ZramRecompression::new();

    mock.set_idle.expect().with(eq("3747")).times(2).returning(|_| Ok(()));

    assert!(zram_recompression
        .mark_and_recompress::<MockSysfsZramApi, MockMeminfoApi>(
            &params,
            &suspend_history,
            TimeApiImpl::get_boot_time()
        )
        .is_ok());
}

#[test]
fn mark_and_recompress_idle_time_adjusted_by_suspend_duration() {
    let mock = MockContext::new();
    mock.recompress.expect().returning(|_| Ok(()));
    let meminfo = "MemTotal: 10000 kB
        MemAvailable: 8000 kB";
    mock.read_meminfo.expect().returning(|| Ok(meminfo.to_string()));
    let params = Params {
        min_idle: Duration::from_secs(3600),
        max_idle: Duration::from_secs(4000),
        threshold_bytes: 0,
        ..Default::default()
    };
    let mut suspend_history = SuspendHistory::new();
    let boot_now = BootTime::from_duration(Duration::from_secs(12345));
    suspend_history.record_suspend_duration(Duration::from_secs(1000), boot_now, params.max_idle);
    let mut zram_recompression = ZramRecompression::new();

    mock.set_idle.expect().with(eq("4747")).times(2).returning(|_| Ok(()));

    assert!(zram_recompression
        .mark_and_recompress::<MockSysfsZramApi, MockMeminfoApi>(
            &params,
            &suspend_history,
            boot_now
        )
        .is_ok());
}

#[test]
fn mark_and_recompress_calculate_idle_failure() {
    let mock = MockContext::new();
    mock.recompress.expect().returning(|_| Ok(()));
    let params = Params {
        min_idle: Duration::from_secs(4000),
        max_idle: Duration::from_secs(3600),
        threshold_bytes: 0,
        ..Default::default()
    };
    let suspend_history = SuspendHistory::new();
    let mut zram_recompression = ZramRecompression::new();

    assert!(matches!(
        zram_recompression.mark_and_recompress::<MockSysfsZramApi, MockMeminfoApi>(
            &params,
            &suspend_history,
            TimeApiImpl::get_boot_time()
        ),
        Err(Error::CalculateIdle(_))
    ));
}

#[test]
fn mark_and_recompress_mark_idle_failure() {
    let mock = MockContext::new();
    mock.setup_default_meminfo();
    let params = Params { threshold_bytes: 0, ..Default::default() };
    let suspend_history = SuspendHistory::new();
    let mut zram_recompression = ZramRecompression::new();

    mock.set_idle.expect().returning(|_| Err(std::io::Error::other("error")));

    assert!(matches!(
        zram_recompression.mark_and_recompress::<MockSysfsZramApi, MockMeminfoApi>(
            &params,
            &suspend_history,
            TimeApiImpl::get_boot_time()
        ),
        Err(Error::MarkIdle(_))
    ));
}

#[test]
fn mark_and_recompress_skip_huge_idle() {
    let mock = MockContext::new();
    mock.set_idle.expect().returning(|_| Ok(()));
    mock.setup_default_meminfo();
    let params = Params { huge_idle: false, threshold_bytes: 0, ..Default::default() };
    let suspend_history = SuspendHistory::new();
    let mut zram_recompression = ZramRecompression::new();

    mock.recompress.expect().with(eq("type=huge_idle")).times(0).returning(|_| Ok(()));
    mock.recompress.expect().with(eq("type=idle")).times(1).returning(|_| Ok(()));
    mock.recompress.expect().with(eq("type=huge")).times(1).returning(|_| Ok(()));

    assert!(zram_recompression
        .mark_and_recompress::<MockSysfsZramApi, MockMeminfoApi>(
            &params,
            &suspend_history,
            TimeApiImpl::get_boot_time()
        )
        .is_ok());
}

#[test]
fn mark_and_recompress_skip_idle() {
    let mock = MockContext::new();
    mock.set_idle.expect().returning(|_| Ok(()));
    mock.setup_default_meminfo();
    let params = Params { idle: false, threshold_bytes: 0, ..Default::default() };
    let suspend_history = SuspendHistory::new();
    let mut zram_recompression = ZramRecompression::new();

    mock.recompress.expect().with(eq("type=huge_idle")).times(1).returning(|_| Ok(()));
    mock.recompress.expect().with(eq("type=idle")).times(0).returning(|_| Ok(()));
    mock.recompress.expect().with(eq("type=huge")).times(1).returning(|_| Ok(()));

    assert!(zram_recompression
        .mark_and_recompress::<MockSysfsZramApi, MockMeminfoApi>(
            &params,
            &suspend_history,
            TimeApiImpl::get_boot_time()
        )
        .is_ok());
}

#[test]
fn mark_and_recompress_skip_huge() {
    let mock = MockContext::new();
    mock.set_idle.expect().returning(|_| Ok(()));
    mock.setup_default_meminfo();
    let params = Params { huge: false, threshold_bytes: 0, ..Default::default() };
    let suspend_history = SuspendHistory::new();
    let mut zram_recompression = ZramRecompression::new();

    mock.recompress.expect().with(eq("type=huge_idle")).times(1).returning(|_| Ok(()));
    mock.recompress.expect().with(eq("type=idle")).times(1).returning(|_| Ok(()));
    mock.recompress.expect().with(eq("type=huge")).times(0).returning(|_| Ok(()));

    assert!(zram_recompression
        .mark_and_recompress::<MockSysfsZramApi, MockMeminfoApi>(
            &params,
            &suspend_history,
            TimeApiImpl::get_boot_time()
        )
        .is_ok());
}
