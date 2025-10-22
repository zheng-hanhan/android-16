/*
 * Copyright (C) 2024 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

use aconfigd_protos::ProtoStorageReturnMessage;
use aconfigd_rust::aconfigd::Aconfigd;
use anyhow::{anyhow, bail, Result};
use log::{debug, error, info};
use std::io::{Read, Write};
use std::os::fd::AsRawFd;
use std::os::unix::net::UnixListener;
use std::path::Path;

const ACONFIGD_SOCKET: &str = "aconfigd_system";
const ACONFIGD_ROOT_DIR: &str = "/metadata/aconfig";
const STORAGE_RECORDS: &str = "/metadata/aconfig/storage_records.pb";
const PLATFORM_STORAGE_RECORDS: &str = "/metadata/aconfig/platform_storage_records.pb";
const ACONFIGD_SOCKET_BACKLOG: i32 = 8;

/// start aconfigd socket service
pub fn start_socket() -> Result<()> {
    let fd = rustutils::sockets::android_get_control_socket(ACONFIGD_SOCKET)?;

    // SAFETY: Safe because this doesn't modify any memory and we check the return value.
    let ret = unsafe { libc::listen(fd.as_raw_fd(), ACONFIGD_SOCKET_BACKLOG) };
    if ret < 0 {
        bail!(std::io::Error::last_os_error());
    }

    let listener = UnixListener::from(fd);

    let storage_records = if aconfig_new_storage_flags::enable_aconfigd_from_mainline() {
        PLATFORM_STORAGE_RECORDS
    } else {
        STORAGE_RECORDS
    };

    let mut aconfigd = Aconfigd::new(Path::new(ACONFIGD_ROOT_DIR), Path::new(storage_records));
    aconfigd.initialize_from_storage_record()?;

    debug!("start waiting for a new client connection through socket.");
    for stream in listener.incoming() {
        match stream {
            Ok(mut stream) => {
                if let Err(errmsg) = aconfigd.handle_socket_request_from_stream(&mut stream) {
                    error!("failed to handle socket request: {:?}", errmsg);
                }
            }
            Err(errmsg) => {
                error!("failed to listen for an incoming message: {:?}", errmsg);
            }
        }
    }

    Ok(())
}

/// initialize mainline module storage files
pub fn mainline_init() -> Result<()> {
    let mut aconfigd = Aconfigd::new(Path::new(ACONFIGD_ROOT_DIR), Path::new(STORAGE_RECORDS));
    aconfigd.initialize_from_storage_record()?;
    Ok(aconfigd.initialize_mainline_storage()?)
}

/// initialize platform storage files
pub fn platform_init() -> Result<()> {
    let storage_records = if aconfig_new_storage_flags::enable_aconfigd_from_mainline() {
        PLATFORM_STORAGE_RECORDS
    } else {
        STORAGE_RECORDS
    };

    let mut aconfigd = Aconfigd::new(Path::new(ACONFIGD_ROOT_DIR), Path::new(storage_records));
    aconfigd.remove_boot_files()?;
    aconfigd.initialize_from_storage_record()?;
    Ok(aconfigd.initialize_platform_storage()?)
}
