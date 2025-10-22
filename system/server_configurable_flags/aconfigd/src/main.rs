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

//! `aconfigd-mainline` is a daemon binary that responsible for:
//! (1) initialize mainline storage files
//! (2) initialize and maintain a persistent socket based service

use clap::Parser;
use log::{error, info};
use std::panic;

mod aconfigd_commands;

#[derive(Parser, Debug)]
struct Cli {
    #[clap(subcommand)]
    command: Command,
}

#[derive(Parser, Debug)]
enum Command {
    /// start aconfigd socket.
    StartSocket,

    /// initialize platform storage files.
    PlatformInit,

    /// initialize mainline module storage files.
    MainlineInit,
}

fn main() {
    if !aconfig_new_storage_flags::enable_aconfig_storage_daemon() {
        info!("aconfigd_system is disabled, exiting");
        std::process::exit(0);
    }

    // SAFETY: nobody has taken ownership of the inherited FDs yet.
    // This needs to be called before logger initialization as logger setup will
    // create a file descriptor.
    unsafe {
        if let Err(errmsg) = rustutils::inherited_fd::init_once() {
            error!("failed to run init_once for inherited fds: {:?}.", errmsg);
            std::process::exit(1);
        }
    };

    // setup android logger, direct to logcat
    android_logger::init_once(
        android_logger::Config::default()
            .with_tag("aconfigd_system")
            .with_max_level(log::LevelFilter::Trace),
    );
    info!("starting aconfigd_system commands.");

    let cli = Cli::parse();
    let command_return = match cli.command {
        Command::StartSocket => {
            if cfg!(disable_system_aconfigd_socket) {
                info!("aconfigd_system is build-disabled, exiting");
                Ok(())
            } else {
                info!("aconfigd_system is build-enabled, starting socket");
                aconfigd_commands::start_socket()
            }
        }
        Command::PlatformInit => aconfigd_commands::platform_init(),
        Command::MainlineInit => {
            if aconfig_new_storage_flags::enable_aconfigd_from_mainline() {
                info!("aconfigd_mainline is enabled, skipping mainline init");
                std::process::exit(1);
            }
            aconfigd_commands::mainline_init()
        }
    };

    if let Err(errmsg) = command_return {
        error!("failed to run aconfigd command: {:?}.", errmsg);
        std::process::exit(1);
    }
}
