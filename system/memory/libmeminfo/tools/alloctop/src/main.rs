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

//! Tool to help the parsing and filtering of /proc/allocinfo

use alloctop::{run, SortBy};
use std::env;
use std::process;

fn print_help() {
    println!("alloctop - A tool for analyzing memory allocations from /proc/allocinfo\n");
    println!("Usage: alloctop [OPTIONS]\n");
    println!("Options:");
    println!("  -m, --min <size>    Only display allocations with size greater than <size>");
    println!("  -n, --lines <num>   Only output the first <num> lines");
    println!("  -o, --once          Display the output once and then exit.");
    println!("  -s, --sort <s|c|t>  Sort the output by size (s), number of calls (c), or tag (t)");
    println!("  -t, --tree          Aggregate output data by tag components. Only the \"min\"");
    println!("                      option is implemented for this visualization\n");
    println!("  -h, --help          Display this help message and exit");
}

#[cfg(unix)]
fn reset_sigpipe() {
    // SAFETY:
    // This is safe because we are simply resetting the SIGPIPE signal handler to its default behavior.
    // The `signal` function itself is marked as unsafe because it can globally modify process state.
    // However, in this specific case, we are restoring the default behavior of ignoring the signal
    // which is a well-defined and safe operation.
    unsafe {
        libc::signal(libc::SIGPIPE, libc::SIG_DFL);
    }
}

#[cfg(not(unix))]
fn reset_sigpipe() {
    // no-op
}

fn main() {
    reset_sigpipe();

    let args: Vec<String> = env::args().collect();
    let mut max_lines: usize = usize::MAX;
    let mut sort_by = None;
    let mut min_size = 0;
    let mut use_tree = false;
    let mut display_once = false;

    let mut i = 1;
    while i < args.len() {
        match args[i].as_str() {
            "-h" | "--help" => {
                print_help();
                process::exit(0);
            }
            "-s" | "--sort" => {
                i += 1;
                if i < args.len() {
                    sort_by = match args[i].as_str() {
                        "s" => Some(SortBy::Size),
                        "c" => Some(SortBy::Calls),
                        "t" => Some(SortBy::Tag),
                        _ => {
                            eprintln!("Invalid sort option. Use 's', 'c', or 't'.");
                            process::exit(1);
                        }
                    };
                } else {
                    eprintln!("Missing argument for --sort.");
                    process::exit(1);
                }
            }
            "-m" | "--min" => {
                i += 1;
                if i < args.len() {
                    min_size = match args[i].parse::<u64>() {
                        Ok(val) => val,
                        Err(_) => {
                            eprintln!("Invalid minimum size. Please provide a valid number.");
                            process::exit(1);
                        }
                    };
                } else {
                    eprintln!("Missing argument for --min.");
                    process::exit(1);
                }
            }
            "-n" | "--lines" => {
                i += 1;
                if i < args.len() {
                    max_lines = match args[i].parse::<usize>() {
                        Ok(val) => val,
                        Err(_) => {
                            eprintln!("Invalid lines. Please provide a valid number.");
                            process::exit(1);
                        }
                    };
                } else {
                    eprintln!("Missing argument for --lines.");
                    process::exit(1);
                }
            }
            "-o" | "--once" => {
                display_once = true;
            }
            "-t" | "--tree" => {
                use_tree = true;
            }
            _ => {
                eprintln!("Invalid argument: {}", args[i]);
                print_help();
                process::exit(1);
            }
        }
        i += 1;
    }

    if !display_once {
        eprintln!("Only \"display once\" mode currently available, run with \"-o\".");
        process::exit(1);
    }

    if let Err(e) = run(max_lines, sort_by, min_size, use_tree, alloctop::PROC_ALLOCINFO) {
        eprintln!("{}", e);
        process::exit(1);
    }
}
