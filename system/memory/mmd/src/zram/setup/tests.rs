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

use std::os::unix::process::ExitStatusExt;
use std::path::PathBuf;
use std::sync::LockResult;
use std::sync::Mutex;
use std::sync::MutexGuard;

use mockall::predicate::*;
use mockall::Sequence;

use super::*;
use crate::zram::MockSysfsZramApi;
use crate::zram::ZRAM_API_MTX;

const PROC_SWAP_HEADER: &str = "Filename                                Type            Size            Used            Priority\n";
const DEFAULT_ZRAM_SIZE: u64 = 1000000;
const DEFAULT_WRITEBACK_DEVICE_SIZE: u64 = 1 << 20;

fn success_command_output() -> std::process::Output {
    std::process::Output {
        status: std::process::ExitStatus::from_raw(0),
        stderr: "".to_owned().into_bytes(),
        stdout: "".to_owned().into_bytes(),
    }
}

fn failure_command_output() -> std::process::Output {
    std::process::Output {
        status: std::process::ExitStatus::from_raw(1),
        stderr: "".to_owned().into_bytes(),
        stdout: "".to_owned().into_bytes(),
    }
}

/// Mutex to synchronize tests using [MockSetupApi].
///
/// mockall for static functions requires synchronization.
///
/// https://docs.rs/mockall/latest/mockall/#static-methods
pub static SETUP_API_MTX: Mutex<()> = Mutex::new(());

struct MockContext<'a> {
    write_disksize: crate::zram::__mock_MockSysfsZramApi_SysfsZramApi::__write_disksize::Context,
    write_backing_dev:
        crate::zram::__mock_MockSysfsZramApi_SysfsZramApi::__write_backing_dev::Context,
    read_swap_areas: crate::zram::setup::__mock_MockSetupApi_SetupApi::__read_swap_areas::Context,
    mkswap: crate::zram::setup::__mock_MockSetupApi_SetupApi::__mkswap::Context,
    swapon: crate::zram::setup::__mock_MockSetupApi_SetupApi::__swapon::Context,
    attach_loop_device:
        crate::zram::setup::__mock_MockSetupApi_SetupApi::__attach_loop_device::Context,
    // Lock will be released after mock contexts are dropped.
    _setup_lock: LockResult<MutexGuard<'a, ()>>,
    _zram_lock: LockResult<MutexGuard<'a, ()>>,
}

impl MockContext<'_> {
    fn new() -> Self {
        let _zram_lock = ZRAM_API_MTX.lock();
        let _setup_lock = SETUP_API_MTX.lock();
        Self {
            write_disksize: MockSysfsZramApi::write_disksize_context(),
            write_backing_dev: MockSysfsZramApi::write_backing_dev_context(),
            read_swap_areas: MockSetupApi::read_swap_areas_context(),
            mkswap: MockSetupApi::mkswap_context(),
            swapon: MockSetupApi::swapon_context(),
            attach_loop_device: MockSetupApi::attach_loop_device_context(),
            _setup_lock,
            _zram_lock,
        }
    }
}

#[test]
fn is_zram_swap_activated_zram_off() {
    let mock = MockContext::new();
    mock.read_swap_areas.expect().returning(|| Ok(PROC_SWAP_HEADER.to_string()));

    assert!(!is_zram_swap_activated::<MockSetupApi>().unwrap());
}

#[test]
fn is_zram_swap_activated_zram_on() {
    let mock = MockContext::new();
    let zram_area = "/dev/block/zram0                        partition       7990996         87040           -2\n";
    mock.read_swap_areas.expect().returning(|| Ok(PROC_SWAP_HEADER.to_owned() + zram_area));

    assert!(is_zram_swap_activated::<MockSetupApi>().unwrap());
}

#[test]
fn activate_success() {
    let mock = MockContext::new();
    let zram_size = 123456;
    let mut seq = Sequence::new();
    mock.write_disksize
        .expect()
        .with(eq("123456"))
        .times(1)
        .returning(|_| Ok(()))
        .in_sequence(&mut seq);
    mock.mkswap
        .expect()
        .with(eq(ZRAM_DEVICE_PATH))
        .times(1)
        .returning(|_| Ok(success_command_output()))
        .in_sequence(&mut seq);
    mock.swapon
        .expect()
        .with(eq(std::ffi::CString::new(ZRAM_DEVICE_PATH).unwrap()))
        .times(1)
        .returning(|_| Ok(()))
        .in_sequence(&mut seq);

    assert!(activate_zram::<MockSysfsZramApi, MockSetupApi>(zram_size).is_ok());
}

#[test]
fn activate_failed_update_size() {
    let mock = MockContext::new();
    mock.write_disksize.expect().returning(|_| Err(std::io::Error::other("error")));

    assert!(matches!(
        activate_zram::<MockSysfsZramApi, MockSetupApi>(DEFAULT_ZRAM_SIZE),
        Err(ZramActivationError::UpdateZramDiskSize(_))
    ));
}

#[test]
fn activate_failed_mkswap() {
    let mock = MockContext::new();
    mock.write_disksize.expect().returning(|_| Ok(()));
    mock.mkswap.expect().returning(|_| Ok(failure_command_output()));

    assert!(matches!(
        activate_zram::<MockSysfsZramApi, MockSetupApi>(DEFAULT_ZRAM_SIZE),
        Err(ZramActivationError::MkSwap(_))
    ));
}

#[test]
fn activate_failed_swapon() {
    let mock = MockContext::new();
    mock.write_disksize.expect().returning(|_| Ok(()));
    mock.mkswap.expect().returning(|_| Ok(success_command_output()));
    mock.swapon.expect().returning(|_| Err(std::io::Error::other("error")));

    assert!(matches!(
        activate_zram::<MockSysfsZramApi, MockSetupApi>(DEFAULT_ZRAM_SIZE),
        Err(ZramActivationError::SwapOn(_))
    ));
}

#[test]
fn set_up_zram_backing_device_success() {
    let mock = MockContext::new();
    let backing_file_dir = tempfile::tempdir().unwrap();
    let backing_file_path = backing_file_dir.path().join("zram_swap");
    let loop_file_path = "/dev/block/loop97";
    let writeback_device_size = 2 << 20;

    mock.attach_loop_device
        .expect()
        .withf(move |path, size| {
            std::fs::metadata(path).unwrap().len() == writeback_device_size
                && *size == writeback_device_size
        })
        .returning(move |_, _| {
            Ok(loopdevice::LoopDevice {
                file: tempfile::tempfile().unwrap(),
                path: PathBuf::from(loop_file_path),
            })
        });
    assert!(create_zram_writeback_device::<MockSetupApi>(
        &backing_file_path,
        writeback_device_size
    )
    .is_ok());
    assert!(!std::fs::exists(&backing_file_path).unwrap());
}

#[test]
fn set_up_zram_backing_device_failed_to_create_backing_file() {
    let mock = MockContext::new();
    let backing_file_path = Path::new("/dev/null");

    mock.attach_loop_device.expect().times(0);
    mock.write_backing_dev.expect().times(0);

    assert!(matches!(
        create_zram_writeback_device::<MockSetupApi>(
            backing_file_path,
            DEFAULT_WRITEBACK_DEVICE_SIZE
        ),
        Err(WritebackDeviceSetupError::CreateBackingFile(_))
    ));
}

#[test]
fn set_up_zram_backing_device_failed_to_create_backing_device() {
    let mock = MockContext::new();
    let backing_file = tempfile::NamedTempFile::new().unwrap();

    mock.attach_loop_device
        .expect()
        .returning(|_, _| Err(anyhow::anyhow!("failed to create loop device")));
    mock.write_backing_dev.expect().times(0);

    assert!(matches!(
        create_zram_writeback_device::<MockSetupApi>(
            backing_file.path(),
            DEFAULT_WRITEBACK_DEVICE_SIZE
        ),
        Err(WritebackDeviceSetupError::CreateBackingDevice(_))
    ));
    assert!(!std::fs::exists(backing_file).unwrap());
}

#[test]
fn enable_zram_writeback_limit_success() {
    let _m = ZRAM_API_MTX.lock();
    let mock = MockSysfsZramApi::write_writeback_limit_enable_context();

    mock.expect().with(eq("1")).times(1).returning(|_| Ok(()));

    assert!(enable_zram_writeback_limit::<MockSysfsZramApi>().is_ok());
}
