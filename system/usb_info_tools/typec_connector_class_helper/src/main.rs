// Copyright 2024 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

//! main

mod typec_class_utils;
mod usb_pd_utils;

#[cfg(test)]
mod typec_class_utils_tests;

use anyhow::{anyhow, Result};
use std::{env, io::stdout, path::Path};
use typec_class_utils::{
    parse_dirs_and_execute, print_port_info, OutputWriter, PORT_REGEX, TYPEC_SYSFS_PATH,
};

fn main() -> Result<()> {
    logger::init(
        logger::Config::default()
            .with_tag_on_device("usb_info")
            .with_max_level(log::LevelFilter::Info),
    );

    let args: Vec<String> = env::args().collect();
    if args.len() > 1 {
        return Err(anyhow!("typec_connector_class_helper does not accept any arguments."));
    }

    let mut out_writer = OutputWriter::new(stdout(), 0);
    parse_dirs_and_execute(
        Path::new(TYPEC_SYSFS_PATH),
        &mut out_writer,
        PORT_REGEX,
        print_port_info,
    )
}
