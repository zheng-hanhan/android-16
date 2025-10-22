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

#[cfg(test)]
mod tests {
    use alloctop::*;
    use std::io::Write;
    use tempfile::NamedTempFile;

    /// Tests the handling of missing input files.
    ///
    /// This test verifies that the `run` function correctly handles cases where
    /// the input file specified does not exist. It ensures that an error is returned
    /// and that the error message indicates an issue with file reading.
    #[test]
    fn test_missing_file_handling() {
        // Define a non-existent file path
        let non_existent_file = "/this/file/should/not/exist.txt";

        // Attempt to run the alloctop logic with the non-existent file
        let result = run(
            usize::MAX,         // max_lines (not relevant for this test)
            Some(SortBy::Size), // sort_by (not relevant for this test)
            0,                  // min_size (not relevant for this test)
            false,              // use_tree (not relevant for this test)
            non_existent_file,
        );

        // Assert that the result is an error
        assert!(result.is_err());

        // Optionally, check the error message to ensure it's related to file access
        if let Err(msg) = result {
            assert!(
                msg.contains("Error reading or parsing allocinfo"),
                "Error message should indicate an issue with file reading"
            );
        }
    }

    /// Tests parsing an empty allocinfo file.
    ///
    /// This test verifies that the `parse_allocinfo` function correctly handles
    /// an empty input file. It ensures that an empty vector of `AllocInfo` is returned.
    #[test]
    fn test_parse_allocinfo_empty_file() {
        let mut temp_file = NamedTempFile::new().unwrap();
        writeln!(temp_file).unwrap();

        let result = parse_allocinfo(temp_file.path().to_str().unwrap()).unwrap();

        assert_eq!(result.len(), 0);
    }

    /// Tests parsing a valid allocinfo file.
    ///
    /// This test verifies that the `parse_allocinfo` function correctly parses
    /// a valid allocinfo file with multiple valid lines. It checks that the correct
    /// number of `AllocInfo` entries are parsed and that the values within each entry
    /// are correct.
    #[test]
    fn test_parse_allocinfo_valid_file() {
        let mut temp_file = NamedTempFile::new().unwrap();
        writeln!(
            temp_file,
            "allocinfo - version: 1.0
             #     <size>  <calls> <tag info>
             1024 5 /some/tag/path
             2048 2 /another/tag
             512 10 /third/tag/here"
        )
        .unwrap();

        let result = parse_allocinfo(temp_file.path().to_str().unwrap()).unwrap();

        assert_eq!(result.len(), 3);
        assert_eq!(
            result[0],
            AllocInfo { size: 1024, calls: 5, tag: "/some/tag/path".to_string() }
        );
        assert_eq!(result[1], AllocInfo { size: 2048, calls: 2, tag: "/another/tag".to_string() });
        assert_eq!(
            result[2],
            AllocInfo { size: 512, calls: 10, tag: "/third/tag/here".to_string() }
        );
    }

    /// Tests parsing an allocinfo file with invalid lines.
    ///
    /// This test verifies that the `parse_allocinfo` function correctly handles
    /// invalid lines within the input file. It checks that valid lines are parsed
    /// correctly and invalid lines are skipped, with potentially incomplete data if possible.
    #[test]
    fn test_parse_allocinfo_invalid_lines() {
        let mut temp_file = NamedTempFile::new().unwrap();
        writeln!(
            temp_file,
            "allocinfo - version: 1.0
             #     <size>  <calls> <tag info>
             1024 5 /some/tag/path
             invalid line
             512 abc /third/tag/here"
        )
        .unwrap();

        let result = parse_allocinfo(temp_file.path().to_str().unwrap()).unwrap();

        assert_eq!(result.len(), 2);
        assert_eq!(
            result[0],
            AllocInfo { size: 1024, calls: 5, tag: "/some/tag/path".to_string() }
        );
        assert_eq!(
            result[1],
            AllocInfo { size: 512, calls: 0, tag: "/third/tag/here".to_string() }
        );
    }

    /// Tests parsing a missing allocinfo file.
    ///
    /// This test verifies that the `parse_allocinfo` function correctly handles
    /// cases where the input file does not exist. It ensures that an error is returned.
    #[test]
    fn test_parse_allocinfo_missing_file() {
        let result = parse_allocinfo("nonexistent_file.txt");
        assert!(result.is_err());
    }

    /// Tests parsing an allocinfo file with comments.
    ///
    /// This test verifies that the `parse_allocinfo` function correctly handles
    /// comment lines within the input file. It ensures that comment lines are ignored
    /// and valid lines are parsed correctly.
    #[test]
    fn test_parse_allocinfo_comments() {
        let mut temp_file = NamedTempFile::new().unwrap();
        writeln!(
            temp_file,
            "allocinfo - version: 1.0
             #     <size>  <calls> <tag info># This is a comment
             1024 5 /some/tag/path
             # Another comment
             512 10 /third/tag/here"
        )
        .unwrap();

        let result = parse_allocinfo(temp_file.path().to_str().unwrap()).unwrap();

        assert_eq!(result.len(), 2);
        assert_eq!(
            result[0],
            AllocInfo { size: 1024, calls: 5, tag: "/some/tag/path".to_string() }
        );
        assert_eq!(
            result[1],
            AllocInfo { size: 512, calls: 10, tag: "/third/tag/here".to_string() }
        );
    }

    /// Tests sorting allocinfo data by size.
    ///
    /// This test verifies that the `sort_allocinfo` function correctly sorts
    /// a vector of `AllocInfo` entries by size in descending order.
    #[test]
    fn test_sort_allocinfo_by_size() {
        let mut data = vec![
            AllocInfo { size: 1024, calls: 5, tag: "/tag1".to_string() },
            AllocInfo { size: 512, calls: 10, tag: "/tag2".to_string() },
            AllocInfo { size: 2048, calls: 2, tag: "/tag3".to_string() },
        ];

        sort_allocinfo(&mut data, SortBy::Size);

        assert_eq!(data[0].size, 2048);
        assert_eq!(data[1].size, 1024);
        assert_eq!(data[2].size, 512);
    }

    /// Tests sorting allocinfo data by number of calls.
    ///
    /// This test verifies that the `sort_allocinfo` function correctly sorts
    /// a vector of `AllocInfo` entries by the number of calls in descending order.
    #[test]
    fn test_sort_allocinfo_by_calls() {
        let mut data = vec![
            AllocInfo { size: 1024, calls: 5, tag: "/tag1".to_string() },
            AllocInfo { size: 512, calls: 10, tag: "/tag2".to_string() },
            AllocInfo { size: 2048, calls: 2, tag: "/tag3".to_string() },
        ];

        sort_allocinfo(&mut data, SortBy::Calls);

        assert_eq!(data[0].calls, 10);
        assert_eq!(data[1].calls, 5);
        assert_eq!(data[2].calls, 2);
    }

    /// Tests sorting allocinfo data by tag.
    ///
    /// This test verifies that the `sort_allocinfo` function correctly sorts
    /// a vector of `AllocInfo` entries by tag in ascending order.
    #[test]
    fn test_sort_allocinfo_by_tag() {
        let mut data = vec![
            AllocInfo { size: 1024, calls: 5, tag: "/tag2".to_string() },
            AllocInfo { size: 512, calls: 10, tag: "/tag3".to_string() },
            AllocInfo { size: 2048, calls: 2, tag: "/tag1".to_string() },
        ];

        sort_allocinfo(&mut data, SortBy::Tag);

        assert_eq!(data[0].tag, "/tag1");
        assert_eq!(data[1].tag, "/tag2");
        assert_eq!(data[2].tag, "/tag3");
    }

    /// Tests aggregating allocinfo data into a tree structure.
    ///
    /// This test verifies that the `aggregate_tree` function correctly aggregates
    /// allocation data into a tree structure based on the tags. It checks that the
    /// total size and calls are correctly summed for each node in the tree.
    #[test]
    fn test_aggregate_tree() {
        let data = vec![
            AllocInfo { size: 100, calls: 10, tag: "/A/B/C".to_string() },
            AllocInfo { size: 200, calls: 20, tag: "/A/B".to_string() },
            AllocInfo { size: 300, calls: 30, tag: "/A/X".to_string() },
            AllocInfo { size: 50, calls: 5, tag: "/A/B/C".to_string() },
        ];

        let result = aggregate_tree(&data);

        assert_eq!(result.get("/A"), Some(&(650, 65)));
        assert_eq!(result.get("/A/B"), Some(&(350, 35)));
        assert_eq!(result.get("/A/X"), Some(&(300, 30)));
        assert_eq!(result.get("/A/B/C"), Some(&(150, 15)));
        assert_eq!(result.get("/A/Y"), None);
    }

    /// Tests aggregating allocinfo data globally.
    ///
    /// This test verifies that the `aggregate_global` function correctly aggregates
    /// allocation data into a single `AllocInfo` entry representing the total size
    /// and number of calls across all entries.
    #[test]
    fn test_aggregate_global() {
        let data = vec![
            AllocInfo { size: 100, calls: 10, tag: "/A/B/C".to_string() },
            AllocInfo { size: 200, calls: 20, tag: "/A/B".to_string() },
            AllocInfo { size: 300, calls: 30, tag: "/A/X".to_string() },
        ];

        let result = aggregate_global(&data);

        assert_eq!(result.size, 600);
        assert_eq!(result.calls, 60);
    }
}
