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

//! This module implement size spec parsing for mmd.

/// Error from [parse_size_spec].
#[derive(Debug, thiserror::Error)]
pub enum ParsingError {
    /// Size spec was not specified
    #[error("Size spec is empty")]
    EmptySpec,
    /// Specified percentage is out of range
    #[error("Percentage out of range: {0} (expected to be less than {1})")]
    PercentageOutOfRange(u64, u64),
    /// Parsing int error
    #[error("Size spec is not a valid integer: {0}")]
    ParsingInt(#[from] std::num::ParseIntError),
}

/// Parse zram size that can be specified by a percentage or an absolute value.
pub fn parse_size_spec(
    spec: &str,
    block_size: u64,
    block_count: u64,
    max_percentage_allowed: u64,
) -> Result<u64, ParsingError> {
    if spec.is_empty() {
        return Err(ParsingError::EmptySpec);
    }

    if let Some(percentage_str) = spec.strip_suffix('%') {
        let percentage = percentage_str.parse::<u64>()?;

        if percentage > max_percentage_allowed {
            return Err(ParsingError::PercentageOutOfRange(percentage, max_percentage_allowed));
        }
        return Ok(block_count * percentage / 100 * block_size);
    }

    let size = spec.parse::<u64>()?;
    Ok(size)
}

#[cfg(test)]
mod tests {
    use super::*;
    const DEFAULT_BLOCK_SIZE: u64 = 4096;
    const DEFAULT_BLOCK_COUNT: u64 = 998875;
    const MAX_PERCENTAGE_ALLOWED: u64 = 500;

    #[test]
    fn parse_size_spec_invalid() {
        assert!(parse_size_spec(
            "",
            DEFAULT_BLOCK_SIZE,
            DEFAULT_BLOCK_COUNT,
            MAX_PERCENTAGE_ALLOWED
        )
        .is_err());
        assert!(parse_size_spec(
            "not_int%",
            DEFAULT_BLOCK_SIZE,
            DEFAULT_BLOCK_COUNT,
            MAX_PERCENTAGE_ALLOWED
        )
        .is_err());
        assert!(parse_size_spec(
            "not_int",
            DEFAULT_BLOCK_SIZE,
            DEFAULT_BLOCK_COUNT,
            MAX_PERCENTAGE_ALLOWED
        )
        .is_err());
    }

    #[test]
    fn parse_size_spec_percentage_out_of_range() {
        assert!(parse_size_spec("201%", DEFAULT_BLOCK_SIZE, DEFAULT_BLOCK_COUNT, 200).is_err());
    }

    #[test]
    fn parse_size_spec_percentage() {
        assert_eq!(parse_size_spec("0%", 4096, 5, MAX_PERCENTAGE_ALLOWED).unwrap(), 0);
        assert_eq!(parse_size_spec("33%", 4096, 5, MAX_PERCENTAGE_ALLOWED).unwrap(), 4096);
        assert_eq!(parse_size_spec("50%", 4096, 5, MAX_PERCENTAGE_ALLOWED).unwrap(), 8192);
        assert_eq!(parse_size_spec("90%", 4096, 5, MAX_PERCENTAGE_ALLOWED).unwrap(), 16384);
        assert_eq!(parse_size_spec("100%", 4096, 5, MAX_PERCENTAGE_ALLOWED).unwrap(), 20480);
        assert_eq!(parse_size_spec("200%", 4096, 5, MAX_PERCENTAGE_ALLOWED).unwrap(), 40960);
        assert_eq!(
            parse_size_spec("100%", 4096, 3995500, MAX_PERCENTAGE_ALLOWED).unwrap(),
            16365568000
        );
    }

    #[test]
    fn parse_size_spec_bytes() {
        assert_eq!(
            parse_size_spec(
                "1234567",
                DEFAULT_BLOCK_SIZE,
                DEFAULT_BLOCK_COUNT,
                MAX_PERCENTAGE_ALLOWED
            )
            .unwrap(),
            1234567
        );
    }
}
