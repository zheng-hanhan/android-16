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

//! This module defines system properties used for mmd.
//!
//! System properties "mmd.<flag_name>" are defined per OEM.
//!
//! Server configrations "persist.device_config.mmd_native.<flag_name>" overrides the corresponding
//! system properties for in-field experiment on a small population.

use std::str::FromStr;
use std::time::Duration;

use flags_rust::GetServerConfigurableFlag;
use log::error;
use rustutils::system_properties;

const SERVER_CONFIG_NAMESPACE: &str = "mmd_native";

fn generate_property_name(flag_name: &str) -> String {
    format!("mmd.{flag_name}")
}

/// Returns whether mmd manages zram or not.
///
/// If this is false, zram is managed by other system (e.g. swapon_all) or zram
/// is disabled on the device.
///
/// Mmd checks mmd.zram.enabled without the overlay of DeviceConfig because we
/// don't plan any experiments toggling zram enabled/disabled. Taking
/// DeviceConfig into account rather makes the logic to switch zram management
/// system complex.
pub fn is_zram_enabled() -> bool {
    match mmdproperties::mmdproperties::mmd_zram_enabled() {
        Ok(v) => v.unwrap_or(false),
        Err(e) => {
            error!("failed to load mmd.zram.enabled: {e:?}");
            false
        }
    }
}

/// bool system properties for mmd.
///
/// clippy::enum_variant_names is allowed because we may add more properties.
#[allow(clippy::enum_variant_names)]
pub enum BoolProp {
    ZramWritebackEnabled,
    ZramWritebackHugeIdleEnabled,
    ZramWritebackIdleEnabled,
    ZramWritebackHugeEnabled,
    ZramRecompressionEnabled,
    ZramRecompressionHugeIdleEnabled,
    ZramRecompressionIdleEnabled,
    ZramRecompressionHugeEnabled,
}

impl BoolProp {
    fn flag_name(&self) -> &'static str {
        match self {
            Self::ZramWritebackEnabled => "zram.writeback.enabled",
            Self::ZramWritebackHugeIdleEnabled => "zram.writeback.huge_idle.enabled",
            Self::ZramWritebackIdleEnabled => "zram.writeback.idle.enabled",
            Self::ZramWritebackHugeEnabled => "zram.writeback.huge.enabled",
            Self::ZramRecompressionEnabled => "zram.recompression.enabled",
            Self::ZramRecompressionHugeIdleEnabled => "zram.recompression.huge_idle.enabled",
            Self::ZramRecompressionIdleEnabled => "zram.recompression.idle.enabled",
            Self::ZramRecompressionHugeEnabled => "zram.recompression.huge.enabled",
        }
    }

    pub fn get(&self, default: bool) -> bool {
        if let Some(v) = read(self.flag_name()) {
            v
        } else {
            default
        }
    }
}

/// u64 system properties for mmd.
///
/// clippy::enum_variant_names is allowed because we may add more properties.
#[allow(clippy::enum_variant_names)]
pub enum U64Prop {
    ZramWritebackMinBytes,
    ZramWritebackMaxBytes,
    ZramWritebackMaxBytesPerDay,
    ZramRecompressionThresholdBytes,
    ZramWritebackMinFreeSpaceMib,
}

impl U64Prop {
    fn flag_name(&self) -> &'static str {
        match self {
            Self::ZramWritebackMinBytes => "zram.writeback.min_bytes",
            Self::ZramWritebackMaxBytes => "zram.writeback.max_bytes",
            Self::ZramWritebackMaxBytesPerDay => "zram.writeback.max_bytes_per_day",
            Self::ZramRecompressionThresholdBytes => "zram.recompression.threshold_bytes",
            Self::ZramWritebackMinFreeSpaceMib => "zram.writeback.min_free_space_mib",
        }
    }

    pub fn get(&self, default: u64) -> u64 {
        if let Some(v) = read(self.flag_name()) {
            v
        } else {
            default
        }
    }
}

/// Duration system properties for mmd in seconds.
///
/// clippy::enum_variant_names is allowed because we may add more properties.
#[allow(clippy::enum_variant_names)]
pub enum SecondsProp {
    ZramWritebackBackoff,
    ZramWritebackMinIdle,
    ZramWritebackMaxIdle,
    ZramRecompressionBackoff,
    ZramRecompressionMinIdle,
    ZramRecompressionMaxIdle,
}

impl SecondsProp {
    fn flag_name(&self) -> &'static str {
        match self {
            Self::ZramWritebackBackoff => "zram.writeback.backoff_seconds",
            Self::ZramWritebackMinIdle => "zram.writeback.min_idle_seconds",
            Self::ZramWritebackMaxIdle => "zram.writeback.max_idle_seconds",
            Self::ZramRecompressionBackoff => "zram.recompression.backoff_seconds",
            Self::ZramRecompressionMinIdle => "zram.recompression.min_idle_seconds",
            Self::ZramRecompressionMaxIdle => "zram.recompression.max_idle_seconds",
        }
    }

    pub fn get(&self, default: Duration) -> Duration {
        if let Some(v) = read::<u64>(self.flag_name()) {
            Duration::from_secs(v)
        } else {
            default
        }
    }
}

/// String system properties for mmd.
///
/// clippy::enum_variant_names is allowed because we may add more properties.
#[allow(clippy::enum_variant_names)]
pub enum StringProp {
    ZramSize,
    ZramCompAlgorithm,
    ZramWritebackDeviceSize,
    ZramRecompressionAlgorithm,
}

impl StringProp {
    fn flag_name(&self) -> &'static str {
        match self {
            Self::ZramSize => "zram.size",
            Self::ZramCompAlgorithm => "zram.comp_algorithm",
            Self::ZramWritebackDeviceSize => "zram.writeback.device_size",
            Self::ZramRecompressionAlgorithm => "zram.recompression.algorithm",
        }
    }

    pub fn get<T: ToString + ?Sized>(&self, default: &T) -> String {
        read(self.flag_name()).unwrap_or_else(|| default.to_string())
    }
}

fn read<T: FromStr>(flag_name: &str) -> Option<T> {
    let value = GetServerConfigurableFlag(SERVER_CONFIG_NAMESPACE, flag_name, "");
    if !value.is_empty() {
        if let Ok(v) = value.parse() {
            return Some(v);
        }
        error!("failed to parse server config flag: {flag_name}={value}");
    }

    // fallback if server flag is not set or broken.
    let property_name = generate_property_name(flag_name);
    match system_properties::read(&property_name) {
        Ok(Some(v)) => {
            if let Ok(v) = v.parse() {
                return Some(v);
            } else {
                error!("failed to parse system property: {property_name}={v}");
            }
        }
        Ok(None) => {}
        Err(e) => {
            error!("failed to read system property: {property_name} {e:?}");
        }
    }

    None
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn bool_prop_from_default() {
        // We can't test system properties directly. Just a unit test for
        // default value.
        assert!(BoolProp::ZramWritebackEnabled.get(true));
        assert!(!BoolProp::ZramWritebackEnabled.get(false));
    }

    #[test]
    fn u64_prop_from_default() {
        // We can't test system properties directly. Just a unit test for
        // default value.
        assert_eq!(U64Prop::ZramWritebackMinBytes.get(12345), 12345);
    }

    #[test]
    fn seconds_prop_from_default() {
        // We can't test system properties directly. Just a unit test for
        // default value.
        assert_eq!(
            SecondsProp::ZramWritebackBackoff.get(Duration::from_secs(12345)),
            Duration::from_secs(12345)
        );
    }

    #[test]
    fn string_prop_from_default() {
        // We can't test system properties directly. Just a unit test for
        // default value.
        assert_eq!(StringProp::ZramSize.get("1%"), "1%");
        assert_eq!(StringProp::ZramSize.get(&1024), "1024");
    }
}
