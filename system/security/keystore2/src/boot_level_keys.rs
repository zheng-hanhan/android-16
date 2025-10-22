// Copyright 2021, The Android Open Source Project
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

//! Offer keys based on the "boot level" for superencryption.

use crate::ks_err;
use crate::{
    database::{KeyType, KeystoreDB},
    key_parameter::KeyParameterValue,
    raw_device::KeyMintDevice,
};
use android_hardware_security_keymint::aidl::android::hardware::security::keymint::{
    Algorithm::Algorithm, Digest::Digest, KeyParameter::KeyParameter as KmKeyParameter,
    KeyPurpose::KeyPurpose, SecurityLevel::SecurityLevel,
};
use anyhow::{Context, Result};
use keystore2_crypto::{hkdf_expand, ZVec, AES_256_KEY_LENGTH};
use std::{collections::VecDeque, convert::TryFrom};

/// Strategies used to prevent later boot stages from using the KM key that protects the level 0
/// key
#[derive(Debug, PartialEq, Eq, Clone, Copy)]
enum DenyLaterStrategy {
    /// set MaxUsesPerBoot to 1. This is much less secure, since the attacker can replace the key
    /// itself, and therefore create artifacts which appear to come from early boot.
    MaxUsesPerBoot,
    /// set the EarlyBootOnly property. This property is only supported in KM from 4.1 on, but
    /// it ensures that the level 0 key was genuinely created in early boot
    EarlyBootOnly,
}

/// Generally the L0 KM and strategy are chosen by probing KM versions in TEE and Strongbox.
/// However, once a device is launched the KM and strategy must never change, even if the
/// KM version in TEE or Strongbox is updated. Setting this property at build time using
/// `PRODUCT_VENDOR_PROPERTIES` means that the strategy can be fixed no matter what versions
/// of KM are present.
const PROPERTY_NAME: &str = "ro.keystore.boot_level_key.strategy";

fn lookup_level_zero_km_and_strategy() -> Result<Option<(SecurityLevel, DenyLaterStrategy)>> {
    let property_val = rustutils::system_properties::read(PROPERTY_NAME)
        .with_context(|| ks_err!("property read failed: {}", PROPERTY_NAME))?;
    // TODO: use feature(let_else) when that's stabilized.
    let property_val = if let Some(p) = property_val {
        p
    } else {
        log::info!("{} not set, inferring from installed KM instances", PROPERTY_NAME);
        return Ok(None);
    };
    let (level, strategy) = if let Some(c) = property_val.split_once(':') {
        c
    } else {
        log::error!("Missing colon in {}: {:?}", PROPERTY_NAME, property_val);
        return Ok(None);
    };
    let level = match level {
        "TRUSTED_ENVIRONMENT" => SecurityLevel::TRUSTED_ENVIRONMENT,
        "STRONGBOX" => SecurityLevel::STRONGBOX,
        _ => {
            log::error!("Unknown security level in {}: {:?}", PROPERTY_NAME, level);
            return Ok(None);
        }
    };
    let strategy = match strategy {
        "EARLY_BOOT_ONLY" => DenyLaterStrategy::EarlyBootOnly,
        "MAX_USES_PER_BOOT" => DenyLaterStrategy::MaxUsesPerBoot,
        _ => {
            log::error!("Unknown DenyLaterStrategy in {}: {:?}", PROPERTY_NAME, strategy);
            return Ok(None);
        }
    };
    log::info!("Set from {}: {}", PROPERTY_NAME, property_val);
    Ok(Some((level, strategy)))
}

fn get_level_zero_key_km_and_strategy() -> Result<(KeyMintDevice, DenyLaterStrategy)> {
    if let Some((level, strategy)) = lookup_level_zero_km_and_strategy()? {
        return Ok((
            KeyMintDevice::get(level).context(ks_err!("Get KM instance failed."))?,
            strategy,
        ));
    }
    let tee = KeyMintDevice::get(SecurityLevel::TRUSTED_ENVIRONMENT)
        .context(ks_err!("Get TEE instance failed."))?;
    if tee.version() >= KeyMintDevice::KEY_MASTER_V4_1 {
        Ok((tee, DenyLaterStrategy::EarlyBootOnly))
    } else {
        match KeyMintDevice::get_or_none(SecurityLevel::STRONGBOX)
            .context(ks_err!("Get Strongbox instance failed."))?
        {
            Some(strongbox) if strongbox.version() >= KeyMintDevice::KEY_MASTER_V4_1 => {
                Ok((strongbox, DenyLaterStrategy::EarlyBootOnly))
            }
            _ => Ok((tee, DenyLaterStrategy::MaxUsesPerBoot)),
        }
    }
}

/// This is not thread safe; caller must hold a lock before calling.
/// In practice the caller is SuperKeyManager and the lock is the
/// Mutex on its internal state.
pub fn get_level_zero_key(db: &mut KeystoreDB) -> Result<ZVec> {
    let (km_dev, deny_later_strategy) = get_level_zero_key_km_and_strategy()
        .context(ks_err!("get preferred KM instance failed"))?;
    log::info!(
        "In get_level_zero_key: security_level={:?}, deny_later_strategy={:?}",
        km_dev.security_level(),
        deny_later_strategy
    );
    let required_security_level = km_dev.security_level();
    let required_param: KmKeyParameter = match deny_later_strategy {
        DenyLaterStrategy::EarlyBootOnly => KeyParameterValue::EarlyBootOnly,
        DenyLaterStrategy::MaxUsesPerBoot => KeyParameterValue::MaxUsesPerBoot(1),
    }
    .into();
    let params = vec![
        KeyParameterValue::Algorithm(Algorithm::HMAC).into(),
        KeyParameterValue::Digest(Digest::SHA_2_256).into(),
        KeyParameterValue::KeySize(256).into(),
        KeyParameterValue::MinMacLength(256).into(),
        KeyParameterValue::KeyPurpose(KeyPurpose::SIGN).into(),
        KeyParameterValue::NoAuthRequired.into(),
        required_param.clone(),
    ];

    let key_desc = KeyMintDevice::internal_descriptor("boot_level_key".to_string());
    let (key_id_guard, key_entry) = km_dev
        .lookup_or_generate_key(db, &key_desc, KeyType::Client, &params, |key_characteristics| {
            key_characteristics.iter().any(|kc| {
                if kc.securityLevel != required_security_level {
                    log::error!(
                        "In get_level_zero_key: security level expected={:?} got={:?}",
                        required_security_level,
                        kc.securityLevel
                    );
                    return false;
                }
                if !kc.authorizations.iter().any(|a| a == &required_param) {
                    log::error!(
                        "In get_level_zero_key: required param absent {:?}",
                        required_param
                    );
                    return false;
                }
                true
            })
        })
        .context(ks_err!("lookup_or_generate_key failed"))?;

    let params = [
        KeyParameterValue::MacLength(256).into(),
        KeyParameterValue::Digest(Digest::SHA_2_256).into(),
    ];
    let level_zero_key = km_dev
        .use_key_in_one_step(
            db,
            &key_id_guard,
            &key_entry,
            KeyPurpose::SIGN,
            &params,
            None,
            b"Create boot level key",
        )
        .context(ks_err!("use_key_in_one_step failed"))?;
    // TODO: this is rather unsatisfactory, we need a better way to handle
    // sensitive binder returns.
    let level_zero_key =
        ZVec::try_from(level_zero_key).context(ks_err!("conversion to ZVec failed"))?;
    Ok(level_zero_key)
}

/// Holds the key for the current boot level, and a cache of future keys generated as required.
/// When the boot level advances, keys prior to the current boot level are securely dropped.
pub struct BootLevelKeyCache {
    /// Least boot level currently accessible, if any is.
    current: usize,
    /// Invariant: cache entry *i*, if it exists, holds the HKDF key for boot level
    /// *i* + `current`. If the cache is non-empty it can be grown forwards, but it cannot be
    /// grown backwards, so keys below `current` are inaccessible.
    /// `cache.clear()` makes all keys inaccessible.
    cache: VecDeque<ZVec>,
}

impl BootLevelKeyCache {
    const HKDF_ADVANCE: &'static [u8] = b"Advance KDF one step";
    const HKDF_AES: &'static [u8] = b"Generate AES-256-GCM key";
    const HKDF_KEY_SIZE: usize = 32;

    /// Initialize the cache with the level zero key.
    pub fn new(level_zero_key: ZVec) -> Self {
        let mut cache: VecDeque<ZVec> = VecDeque::new();
        cache.push_back(level_zero_key);
        Self { current: 0, cache }
    }

    /// Report whether the key for the given level can be inferred.
    pub fn level_accessible(&self, boot_level: usize) -> bool {
        // If the requested boot level is lower than the current boot level
        // or if we have reached the end (`cache.empty()`) we can't retrieve
        // the boot key.
        boot_level >= self.current && !self.cache.is_empty()
    }

    /// Get the HKDF key for boot level `boot_level`. The key for level *i*+1
    /// is calculated from the level *i* key using `hkdf_expand`.
    fn get_hkdf_key(&mut self, boot_level: usize) -> Result<Option<&ZVec>> {
        if !self.level_accessible(boot_level) {
            return Ok(None);
        }
        // `self.cache.len()` represents the first entry not in the cache,
        // so `self.current + self.cache.len()` is the first boot level not in the cache.
        let first_not_cached = self.current + self.cache.len();

        // Grow the cache forwards until it contains the desired boot level.
        for _level in first_not_cached..=boot_level {
            // We check at the start that cache is non-empty and future iterations only push,
            // so this must unwrap.
            let highest_key = self.cache.back().unwrap();
            let next_key = hkdf_expand(Self::HKDF_KEY_SIZE, highest_key, Self::HKDF_ADVANCE)
                .context(ks_err!("Advancing key one step"))?;
            self.cache.push_back(next_key);
        }

        // If we reach this point, we should have a key at index boot_level - current.
        Ok(Some(self.cache.get(boot_level - self.current).unwrap()))
    }

    /// Drop keys prior to the given boot level, while retaining the ability to generate keys for
    /// that level and later.
    pub fn advance_boot_level(&mut self, new_boot_level: usize) -> Result<()> {
        if !self.level_accessible(new_boot_level) {
            log::error!(
                "Failed to advance boot level to {}, current is {}, cache size {}",
                new_boot_level,
                self.current,
                self.cache.len()
            );
            return Ok(());
        }

        // We `get` the new boot level for the side effect of advancing the cache to a point
        // where the new boot level is present.
        self.get_hkdf_key(new_boot_level).context(ks_err!("Advancing cache"))?;

        // Then we split the queue at the index of the new boot level and discard the front,
        // keeping only the keys with the current boot level or higher.
        self.cache = self.cache.split_off(new_boot_level - self.current);

        // The new cache has the new boot level at index 0, so we set `current` to
        // `new_boot_level`.
        self.current = new_boot_level;

        Ok(())
    }

    /// Drop all keys, effectively raising the current boot level to infinity; no keys can
    /// be inferred from this point on.
    pub fn finish(&mut self) {
        self.cache.clear();
    }

    fn expand_key(
        &mut self,
        boot_level: usize,
        out_len: usize,
        info: &[u8],
    ) -> Result<Option<ZVec>> {
        self.get_hkdf_key(boot_level)
            .context(ks_err!("Looking up HKDF key"))?
            .map(|k| hkdf_expand(out_len, k, info))
            .transpose()
            .context(ks_err!("Calling hkdf_expand"))
    }

    /// Return the AES-256-GCM key for the current boot level.
    pub fn aes_key(&mut self, boot_level: usize) -> Result<Option<ZVec>> {
        self.expand_key(boot_level, AES_256_KEY_LENGTH, BootLevelKeyCache::HKDF_AES)
            .context(ks_err!("expand_key failed"))
    }
}

#[cfg(test)]
mod test {
    use super::*;

    #[test]
    fn test_output_is_consistent() -> Result<()> {
        let initial_key = b"initial key";
        let mut blkc = BootLevelKeyCache::new(ZVec::try_from(initial_key as &[u8])?);
        assert!(blkc.level_accessible(0));
        assert!(blkc.level_accessible(9));
        assert!(blkc.level_accessible(10));
        assert!(blkc.level_accessible(100));
        let v0 = blkc.aes_key(0).unwrap().unwrap();
        let v10 = blkc.aes_key(10).unwrap().unwrap();
        assert_eq!(Some(&v0), blkc.aes_key(0)?.as_ref());
        assert_eq!(Some(&v10), blkc.aes_key(10)?.as_ref());
        blkc.advance_boot_level(5)?;
        assert!(!blkc.level_accessible(0));
        assert!(blkc.level_accessible(9));
        assert!(blkc.level_accessible(10));
        assert!(blkc.level_accessible(100));
        assert_eq!(None, blkc.aes_key(0)?);
        assert_eq!(Some(&v10), blkc.aes_key(10)?.as_ref());
        blkc.advance_boot_level(10)?;
        assert!(!blkc.level_accessible(0));
        assert!(!blkc.level_accessible(9));
        assert!(blkc.level_accessible(10));
        assert!(blkc.level_accessible(100));
        assert_eq!(None, blkc.aes_key(0)?);
        assert_eq!(Some(&v10), blkc.aes_key(10)?.as_ref());
        blkc.advance_boot_level(0)?;
        assert!(!blkc.level_accessible(0));
        assert!(!blkc.level_accessible(9));
        assert!(blkc.level_accessible(10));
        assert!(blkc.level_accessible(100));
        assert_eq!(None, blkc.aes_key(0)?);
        assert_eq!(Some(v10), blkc.aes_key(10)?);
        blkc.finish();
        assert!(!blkc.level_accessible(0));
        assert!(!blkc.level_accessible(9));
        assert!(!blkc.level_accessible(10));
        assert!(!blkc.level_accessible(100));
        assert_eq!(None, blkc.aes_key(0)?);
        assert_eq!(None, blkc.aes_key(10)?);
        Ok(())
    }
}
