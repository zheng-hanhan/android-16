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

//! This module defines block device utilities used for mmd.

use std::fs;
use std::os::unix::fs::MetadataExt;
use std::path::Path;
use std::path::PathBuf;

/// Error from block device operations.
#[derive(Debug, thiserror::Error)]
pub enum BlockDeviceError {
    /// Failed to perform an IO operation on some block device file
    #[error("failed to perform IOs on a block device file: {0}")]
    DeviceFileIo(#[from] std::io::Error),
    /// Failed to get input file metadata
    #[error("failed to get input file metadata: {0}")]
    InputFileMetadata(std::io::Error),
    /// Failed to parse device queue depth
    #[error("failed to parse device queue depth: {0}")]
    ParseDeviceQueueDepth(#[from] std::num::ParseIntError),
    /// Block dev path is invalid
    #[error("block device path {0} is invalid: {1}")]
    InvalidBlockDevicePath(PathBuf, String),
}

type Result<T> = std::result::Result<T, BlockDeviceError>;

/// Clear device IO scheduler by setting the scheduler to none.
///
/// Only works for Kernels version v4.1 and after. Kernels before v4.1 only support 'noop'.
/// However, Android does not need to support kernels lower than v4.1 because v6.1 is the minimum
/// version on Android 15.
pub fn clear_block_device_scheduler(device_name: &str) -> std::io::Result<()> {
    fs::write(format!("/sys/block/{device_name}/queue/scheduler"), "none")
}

/// Configure block device `nr_requests` to be the same as the queue depth of the block device backing `file_path`.
pub fn configure_block_device_queue_depth<P: AsRef<Path>>(
    device_name: &str,
    file_path: P,
) -> Result<()> {
    configure_block_device_queue_depth_with_sysfs(device_name, file_path.as_ref(), "/sys")
}

// Using `&str` type instead of `&Path` for `sysfs_path` to make it easier for formatting block
// device paths. It should be fince since this is a private method introduced for testability only.
fn configure_block_device_queue_depth_with_sysfs(
    device_name: &str,
    file_path: &Path,
    sysfs_path: &str,
) -> Result<()> {
    let file_backing_device = find_backing_block_device(file_path, sysfs_path)?;

    let backing_device_queue_depth =
        fs::read_to_string(format!("{sysfs_path}/class/block/{file_backing_device}/mq/0/nr_tags"))?;
    let backing_device_queue_depth = backing_device_queue_depth.trim().parse::<u32>()?;

    fs::write(
        format!("{sysfs_path}/class/block/{device_name}/queue/nr_requests"),
        backing_device_queue_depth.to_string(),
    )?;
    Ok(())
}

/// For file `file_path`, retrieve the block device backing the filesystem on
/// which the file exists.
fn find_backing_block_device(file_path: &Path, sysfs_path: &str) -> Result<String> {
    let mut device_name = get_block_device_name(file_path, sysfs_path)?;

    while let Some(parent_device) = get_parent_block_device(&device_name, sysfs_path)? {
        device_name = parent_device;
    }

    device_name = partition_parent(&device_name, sysfs_path)?;

    Ok(device_name)
}

/// Get immediate block device name backing `file_path`.
///
/// By following the symlink `/sys/dev/block/{major}:{minor}` to the actual device path.
fn get_block_device_name(file_path: &Path, sysfs_path: &str) -> Result<String> {
    let devnum =
        fs::metadata(file_path).map_err(BlockDeviceError::InputFileMetadata)?.dev() as libc::dev_t;
    // TODO: b/388993276 - Use nix::sys::stat::major|minor once they are configured to be built for Android.
    // SAFETY: devnum should be valid because it's from file metadata.
    let (major, minor) = unsafe { (libc::major(devnum), libc::minor(devnum)) };
    let device_path = std::fs::canonicalize(format!("{sysfs_path}/dev/block/{major}:{minor}"))?;
    Ok(device_path
        .file_name()
        .ok_or_else(|| {
            BlockDeviceError::InvalidBlockDevicePath(
                device_path.clone(),
                "block device real path doesn't have a file name".to_string(),
            )
        })?
        .to_str()
        .ok_or_else(|| {
            BlockDeviceError::InvalidBlockDevicePath(
                device_path.clone(),
                "block device name is not valid Unicode".to_string(),
            )
        })?
        .to_string())
}

/// Returns a parent block device of a dm device with the given name.
///
/// None will be returned if:
///  * Given path doesn't correspond to a dm device.
///  * A dm device is based on top of more than one block devices.
fn get_parent_block_device(device_name: &str, sysfs_path: &str) -> std::io::Result<Option<String>> {
    if !device_name.starts_with("dm-") {
        // Reached bottom of the device mapper stack.
        return Ok(None);
    }
    let mut sub_device_name = None;
    for entry in fs::read_dir(format!("{sysfs_path}/block/{device_name}/slaves"))? {
        let entry = entry?;
        if entry.file_type()?.is_symlink() {
            if sub_device_name.is_some() {
                // Too many slaves. Returning None to be consistent with fs_mgr's libdm implementation:
                // https://cs.android.com/android/platform/superproject/main/+/main:system/core/fs_mgr/libdm/dm.cpp;l=677-678;drc=2bd1c1b20871bcf4ef4660beaa218f2c2bce4630
                return Ok(None);
            }
            sub_device_name = Some(entry.file_name().to_string_lossy().to_string());
        }
    }
    Ok(sub_device_name)
}

/// Returns the parent device of a partition.
///
/// Converts e.g. "sda26" into "sda".
fn partition_parent(device_name: &str, sysfs_path: &str) -> std::io::Result<String> {
    for entry in fs::read_dir(format!("{sysfs_path}/class/block"))? {
        let name = entry?.file_name();
        let name = name.to_string_lossy();

        if name.starts_with('.') {
            continue;
        }

        if fs::exists(format!("{sysfs_path}/class/block/{name}/{device_name}"))? {
            return Ok(name.to_string());
        }
    }
    Ok(device_name.to_string())
}

#[cfg(test)]
mod tests {
    use std::os::unix::fs::symlink;

    use tempfile::tempdir;
    use tempfile::TempDir;

    use super::*;

    enum FakeFs<'a> {
        Symlink(&'a str, &'a str),
        File(&'a str, &'a str),
        Dir(&'a str),
        BackingDevice(&'a Path, &'a str),
    }

    impl FakeFs<'_> {
        fn build(entries: &[Self]) -> TempDir {
            let tempdir = tempdir().unwrap();
            let root = tempdir.path();
            for entry in entries {
                match entry {
                    Self::Symlink(link, original) => {
                        let link = root.join(link.trim_start_matches("/"));
                        let original = root.join(original.trim_start_matches("/"));
                        fs::create_dir_all(link.parent().unwrap()).unwrap();
                        symlink(original, link).unwrap();
                    }
                    Self::File(path, content) => {
                        let path = root.join(path.trim_start_matches("/"));
                        fs::create_dir_all(path.parent().unwrap()).unwrap();
                        fs::write(path, content).unwrap();
                    }
                    Self::Dir(path) => {
                        fs::create_dir_all(root.join(path.trim_start_matches("/"))).unwrap();
                    }
                    Self::BackingDevice(file_path, device_path) => {
                        let device_path = root.join(device_path.trim_start_matches("/"));
                        let devnum = fs::metadata(file_path).unwrap().dev() as libc::dev_t;
                        // TODO: b/388993276 - Use nix::sys::stat::major|minor once they are configured to be built for Android.
                        // SAFETY: devnum should be valid because it's from file metadata.
                        let (major, minor) = unsafe { (libc::major(devnum), libc::minor(devnum)) };
                        let link = root.join(format!("sys/dev/block/{major}:{minor}"));
                        fs::create_dir_all(link.parent().unwrap()).unwrap();
                        symlink(device_path, link).unwrap();
                    }
                }
            }
            tempdir
        }
    }

    #[test]
    fn find_backing_block_device_simple() {
        let file = tempfile::NamedTempFile::new().unwrap();
        let fake_fs = FakeFs::build(&[
            FakeFs::Dir("/sys/devices/platform/block/vda/"),
            FakeFs::Symlink("/sys/class/block/vda", "/sys/devices/platform/block/vda/"),
            FakeFs::BackingDevice(file.path(), "/sys/devices/platform/block/vda"),
        ]);

        assert_eq!(
            find_backing_block_device(file.path(), fake_fs.path().join("sys").to_str().unwrap())
                .unwrap(),
            "vda"
        );
    }

    #[test]
    fn find_backing_block_device_device_mapper() {
        let file = tempfile::NamedTempFile::new().unwrap();
        let fake_fs = FakeFs::build(&[
            FakeFs::Dir("/sys/devices/platform/block/vda/"),
            FakeFs::Dir("/sys/devices/virtual/block/dm-0/"),
            FakeFs::Dir("/sys/devices/virtual/block/dm-7/"),
            FakeFs::Symlink("/sys/block/dm-0/slaves/vda", "/sys/devices/platform/block/vda"),
            FakeFs::Symlink("/sys/block/dm-7/slaves/dm-0", "/sys/devices/virtual/block/dm-0"),
            FakeFs::Symlink("/sys/class/block/vda", "/sys/devices/platform/block/vda"),
            FakeFs::BackingDevice(file.path(), "/sys/devices/virtual/block/dm-7"),
        ]);

        assert_eq!(
            find_backing_block_device(file.path(), fake_fs.path().join("sys").to_str().unwrap())
                .unwrap(),
            "vda"
        );
    }

    #[test]
    fn find_backing_block_device_parent_partition() {
        let file = tempfile::NamedTempFile::new().unwrap();
        let fake_fs = FakeFs::build(&[
            FakeFs::Dir("/sys/devices/platform/block/vda/vda2"),
            FakeFs::Symlink("/sys/class/block/vda", "/sys/devices/platform/block/vda"),
            FakeFs::Symlink("/sys/class/block/vda2", "/sys/devices/platform/block/vda/vda2"),
            FakeFs::BackingDevice(file.path(), "/sys/devices/platform/block/vda/vda2"),
        ]);

        assert_eq!(
            find_backing_block_device(file.path(), fake_fs.path().join("sys").to_str().unwrap())
                .unwrap(),
            "vda"
        );
    }

    #[test]
    fn configure_block_device_queue_depth() {
        let file = tempfile::NamedTempFile::new().unwrap();
        let fake_fs = FakeFs::build(&[
            FakeFs::File("/sys/devices/platform/block/vda/mq/0/nr_tags", "31"),
            FakeFs::File("/sys/devices/virtual/block/loop97/queue/nr_requests", "128"),
            FakeFs::Symlink("/sys/class/block/vda", "/sys/devices/platform/block/vda"),
            FakeFs::Symlink("/sys/class/block/loop97", "/sys/devices/virtual/block/loop97"),
            FakeFs::BackingDevice(file.path(), "/sys/devices/platform/block/vda/"),
        ]);

        configure_block_device_queue_depth_with_sysfs(
            "loop97",
            file.path(),
            fake_fs.path().join("sys").to_str().unwrap(),
        )
        .unwrap();

        assert_eq!(
            fs::read_to_string(fake_fs.path().join("sys/class/block/loop97/queue/nr_requests"))
                .unwrap(),
            "31"
        );
    }

    #[test]
    fn configure_block_device_queue_depth_error() {
        let file = tempfile::NamedTempFile::new().unwrap();
        let fake_fs = FakeFs::build(&[
            FakeFs::Dir("/sys/devices/platform/block/vda/"),
            FakeFs::File("/sys/devices/virtual/block/loop97/queue/nr_requests", "128"),
            FakeFs::Symlink("/sys/class/block/vda", "/sys/devices/platform/block/vda"),
            FakeFs::Symlink("/sys/class/block/loop97", "/sys/devices/virtual/block/loop97"),
            FakeFs::BackingDevice(file.path(), "/sys/devices/platform/block/vda/"),
        ]);

        assert!(matches!(
            configure_block_device_queue_depth_with_sysfs(
                "loop97",
                file.path(),
                fake_fs.path().join("sys").to_str().unwrap()
            ),
            Err(BlockDeviceError::DeviceFileIo(_))
        ));
    }
}
