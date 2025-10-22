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

//! Helper library to handle the contents of /proc/allocinfo

use std::collections::HashMap;
use std::fs::File;
use std::io::{self, BufRead, BufReader};

/// `PROC_ALLOCINFO` is a constant string that represents the default path to the allocinfo file.
///
/// This file is expected to contain allocation information in a specific format, which is parsed by the `parse_allocinfo` function.
pub const PROC_ALLOCINFO: &str = "/proc/allocinfo";

/// `AllocInfo` represents a single allocation record.
#[derive(Debug, PartialEq, Eq)]
pub struct AllocInfo {
    /// The total size of all allocations in bytes (unsigned 64-bit integer).
    pub size: u64,
    /// The total number of all allocation calls (unsigned 64-bit integer).
    pub calls: u64,
    /// A string representing the tag or label associated with this allocation (source code path and line, and function name).
    pub tag: String,
}

/// `AllocGlobal` represents aggregated global allocation statistics.
pub struct AllocGlobal {
    /// The total size of all allocations in bytes (unsigned 64-bit integer).
    pub size: u64,
    /// The total number of all allocation calls (unsigned 64-bit integer).
    pub calls: u64,
}

/// `SortBy` is an enumeration representing the different criteria by which allocation data can be sorted.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum SortBy {
    /// Sort by the size of the allocation.
    Size,
    /// Sort by the number of allocation calls.
    Calls,
    /// Sort by the allocation tag.
    Tag,
}

/// `parse_allocinfo` parses allocation information from a file.
///
/// This function reads and parses an allocinfo file, returning a vector of `AllocInfo` structs.
/// It expects each line of the file (after the first header line) to contain at least three whitespace-separated fields:
/// size, calls, and tag.
///
/// # Arguments
///
/// * `filename`: The path to the allocinfo file.
///
/// # Returns
///
/// A `Result` containing either a vector of `AllocInfo` structs or an `io::Error` if an error occurred during file reading or parsing.
pub fn parse_allocinfo(filename: &str) -> io::Result<Vec<AllocInfo>> {
    let file = File::open(filename)?;
    let reader = BufReader::new(file);
    let mut alloc_info_list = Vec::new();

    for (index, line) in reader.lines().enumerate() {
        // Skip the first line (also the second line can be skipped, but it's handled as a comment)
        if index < 1 {
            continue;
        }

        let line = line?;
        let fields: Vec<&str> = line.split_whitespace().collect();

        if fields.len() >= 3 && fields[0] != "#" {
            let size = fields[0].parse::<u64>().unwrap_or(0);
            let calls = fields[1].parse::<u64>().unwrap_or(0);
            let tag = fields[2..].join(" ");

            // One possible implementation would be to check for the minimum size here, but skipping
            // lines at parsing time won't give correct results when the data is aggregated (e.g., for
            // tree view).
            alloc_info_list.push(AllocInfo { size, calls, tag });
        }
    }

    Ok(alloc_info_list)
}

/// `sort_allocinfo` sorts a slice of `AllocInfo` structs based on the specified criteria.
///
/// # Arguments
///
/// * `data`: A mutable slice of `AllocInfo` structs to be sorted.
/// * `sort_by`: The criteria by which to sort the data, as defined by the `SortBy` enum.
pub fn sort_allocinfo(data: &mut [AllocInfo], sort_by: SortBy) {
    match sort_by {
        SortBy::Size => data.sort_by(|a, b| b.size.cmp(&a.size)),
        SortBy::Calls => data.sort_by(|a, b| b.calls.cmp(&a.calls)),
        SortBy::Tag => data.sort_by(|a, b| a.tag.cmp(&b.tag)),
    }
}

/// `aggregate_tree` aggregates allocation data into a tree structure based on hierarchical tags.
///
/// This function takes a slice of `AllocInfo` and aggregates the size and calls for each unique tag prefix,
/// creating a hierarchical representation of the allocation data.
///
/// # Arguments
///
/// * `data`: A slice of `AllocInfo` structs to be aggregated.
///
/// # Returns
///
/// A `HashMap` where keys are tag prefixes (representing nodes in the tree) and values are tuples containing
/// the aggregated size and calls for that tag prefix.
pub fn aggregate_tree(data: &[AllocInfo]) -> HashMap<String, (u64, u64)> {
    let mut aggregated_data: HashMap<String, (u64, u64)> = HashMap::new();

    for info in data {
        let parts: Vec<&str> = info.tag.split('/').collect();
        for i in 0..parts.len() {
            let tag_prefix = parts[..=i].join("/");
            let entry = aggregated_data.entry(tag_prefix).or_insert((0, 0));
            entry.0 += info.size;
            entry.1 += info.calls;
        }
    }

    aggregated_data
}

/// `print_tree_data` prints the aggregated tree data, filtering by a minimum size.
///
/// This function prints the aggregated allocation data in a tree-like format, sorted by tag.
/// It only prints entries where the aggregated size is greater than or equal to `min_size`.
///
/// # Arguments
///
/// * `data`: A reference to a `HashMap` containing the aggregated tree data, as produced by `aggregate_tree`.
/// * `min_size`: The minimum aggregated size (in bytes) for an entry to be printed.
pub fn print_tree_data(data: &HashMap<String, (u64, u64)>, min_size: u64) {
    let mut sorted_data: Vec<_> = data.iter().collect();
    sorted_data.sort_by(|a, b| a.0.cmp(b.0));

    println!("{:>10} {:>10} Tag", "Size", "Calls");
    for (tag, (size, calls)) in sorted_data {
        if *size < min_size {
            continue;
        }
        println!("{:>10} {:>10} {}", size, calls, tag);
    }
}

/// `aggregate_global` aggregates allocation data to calculate global statistics.
///
/// This function computes the total size and total number of calls across all allocations.
///
/// # Arguments
///
/// * `data`: A slice of `AllocInfo` structs to be aggregated.
///
/// # Returns
///
/// An `AllocGlobal` struct containing the total size and total number of calls.
pub fn aggregate_global(data: &[AllocInfo]) -> AllocGlobal {
    let mut globals = AllocGlobal { size: 0, calls: 0 };

    for info in data {
        globals.size += info.size;
        globals.calls += info.calls;
    }

    globals
}

/// `print_aggregated_global_data` prints the aggregated global allocation statistics.
///
/// This function prints the total size and total number of allocation calls.
///
/// # Arguments
///
/// * `data`: A reference to an `AllocGlobal` struct containing the aggregated data.
pub fn print_aggregated_global_data(data: &AllocGlobal) {
    println!("{:>11} : {}", "Total Size", data.size);
    println!("{:>11} : {}\n", "Total Calls", data.calls);
}

/// `run` is the main entry point for the allocation analysis logic.
///
/// This function orchestrates the process of reading, parsing, aggregating, and displaying allocation information.
/// It handles both flat and tree-based aggregation and display, based on the provided options.
///
/// # Arguments
///
/// * `max_lines`: The maximum number of lines to print in the flat view.
/// * `sort_by`: An optional `SortBy` enum value indicating how to sort the data in the flat view.
/// * `min_size`: The minimum size for an allocation to be included in the output (flat view) or printed (tree view).
/// * `use_tree`: A boolean flag indicating whether to use tree-based aggregation and display.
/// * `filename`: The path to the allocinfo file.
///
/// # Returns
///
/// A `Result` indicating success (`Ok(())`) or an error message (`Err(String)`) if an error occurred.
pub fn run(
    max_lines: usize,
    sort_by: Option<SortBy>,
    min_size: u64,
    use_tree: bool,
    filename: &str,
) -> Result<(), String> {
    match parse_allocinfo(filename) {
        Ok(mut data) => {
            {
                let aggregated_data = aggregate_global(&data);
                print_aggregated_global_data(&aggregated_data);
            }

            if use_tree {
                let tree_data = aggregate_tree(&data);
                print_tree_data(&tree_data, min_size);
            } else {
                data.retain(|alloc_info| alloc_info.size >= min_size);

                if let Some(sort_by) = sort_by {
                    sort_allocinfo(&mut data, sort_by);
                }

                let printable_lines = if max_lines <= data.len() { max_lines } else { data.len() };
                println!("{:>10} {:>10} Tag", "Size", "Calls");
                for info in &data[0..printable_lines] {
                    println!("{:>10} {:>10} {}", info.size, info.calls, info.tag);
                }
            }
            Ok(())
        }
        Err(e) => Err(format!("Error reading or parsing allocinfo: {}", e)),
    }
}
