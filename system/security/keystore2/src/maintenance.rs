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

//! This module implements IKeystoreMaintenance AIDL interface.

use crate::database::{KeyEntryLoadBits, KeyType};
use crate::error::into_logged_binder;
use crate::error::map_km_error;
use crate::error::Error;
use crate::globals::get_keymint_device;
use crate::globals::{DB, ENCODED_MODULE_INFO, LEGACY_IMPORTER, SUPER_KEY};
use crate::ks_err;
use crate::permission::{KeyPerm, KeystorePerm};
use crate::super_key::SuperKeyManager;
use crate::utils::{
    check_dump_permission, check_get_app_uids_affected_by_sid_permissions, check_key_permission,
    check_keystore_permission, uid_to_android_user, watchdog as wd,
};
use android_hardware_security_keymint::aidl::android::hardware::security::keymint::{
    ErrorCode::ErrorCode, IKeyMintDevice::IKeyMintDevice, KeyParameter::KeyParameter, KeyParameterValue::KeyParameterValue, SecurityLevel::SecurityLevel, Tag::Tag,
};
use apex_aidl_interface::aidl::android::apex::{
    IApexService::IApexService,
};
use android_security_maintenance::aidl::android::security::maintenance::IKeystoreMaintenance::{
    BnKeystoreMaintenance, IKeystoreMaintenance,
};
use android_security_maintenance::binder::{
    BinderFeatures, Interface, Result as BinderResult, Strong, ThreadState,
};
use android_security_metrics::aidl::android::security::metrics::{
    KeystoreAtomPayload::KeystoreAtomPayload::StorageStats
};
use android_system_keystore2::aidl::android::system::keystore2::KeyDescriptor::KeyDescriptor;
use android_system_keystore2::aidl::android::system::keystore2::ResponseCode::ResponseCode;
use anyhow::{anyhow, Context, Result};
use binder::wait_for_interface;
use bssl_crypto::digest;
use der::{DerOrd, Encode, asn1::OctetString, asn1::SetOfVec, Sequence};
use keystore2_crypto::Password;
use rustutils::system_properties::PropertyWatcher;
use std::cmp::Ordering;

/// Reexport Domain for the benefit of DeleteListener
pub use android_system_keystore2::aidl::android::system::keystore2::Domain::Domain;

#[cfg(test)]
mod tests;

/// Version number of KeyMint V4.
pub const KEYMINT_V4: i32 = 400;

/// Module information structure for DER-encoding.
#[derive(Sequence, Debug, PartialEq, Eq)]
struct ModuleInfo {
    name: OctetString,
    version: i64,
}

impl DerOrd for ModuleInfo {
    // DER mandates "encodings of the component values of a set-of value shall appear in ascending
    // order". `der_cmp` serves as a proxy for determining that ordering (though why the `der` crate
    // requires this is unclear). Essentially, we just need to compare the `name` lengths, and then
    // if those are equal, the `name`s themselves. (No need to consider `version`s since there can't
    // be more than one `ModuleInfo` with the same `name` in the set-of `ModuleInfo`s.) We rely on
    // `OctetString`'s `der_cmp` to do the aforementioned comparison.
    fn der_cmp(&self, other: &Self) -> std::result::Result<Ordering, der::Error> {
        self.name.der_cmp(&other.name)
    }
}

/// The Maintenance module takes a delete listener argument which observes user and namespace
/// deletion events.
pub trait DeleteListener {
    /// Called by the maintenance module when an app/namespace is deleted.
    fn delete_namespace(&self, domain: Domain, namespace: i64) -> Result<()>;
    /// Called by the maintenance module when a user is deleted.
    fn delete_user(&self, user_id: u32) -> Result<()>;
}

/// This struct is defined to implement the aforementioned AIDL interface.
pub struct Maintenance {
    delete_listener: Box<dyn DeleteListener + Send + Sync + 'static>,
}

impl Maintenance {
    /// Create a new instance of Keystore Maintenance service.
    pub fn new_native_binder(
        delete_listener: Box<dyn DeleteListener + Send + Sync + 'static>,
    ) -> Result<Strong<dyn IKeystoreMaintenance>> {
        Ok(BnKeystoreMaintenance::new_binder(
            Self { delete_listener },
            BinderFeatures { set_requesting_sid: true, ..BinderFeatures::default() },
        ))
    }

    fn add_or_remove_user(&self, user_id: i32) -> Result<()> {
        // Check permission. Function should return if this failed. Therefore having '?' at the end
        // is very important.
        check_keystore_permission(KeystorePerm::ChangeUser).context(ks_err!())?;

        DB.with(|db| {
            SUPER_KEY.write().unwrap().remove_user(
                &mut db.borrow_mut(),
                &LEGACY_IMPORTER,
                user_id as u32,
            )
        })
        .context(ks_err!("Trying to delete keys from db."))?;
        self.delete_listener
            .delete_user(user_id as u32)
            .context(ks_err!("While invoking the delete listener."))
    }

    fn init_user_super_keys(
        &self,
        user_id: i32,
        password: Password,
        allow_existing: bool,
    ) -> Result<()> {
        // Permission check. Must return on error. Do not touch the '?'.
        check_keystore_permission(KeystorePerm::ChangeUser).context(ks_err!())?;

        let mut skm = SUPER_KEY.write().unwrap();
        DB.with(|db| {
            skm.initialize_user(
                &mut db.borrow_mut(),
                &LEGACY_IMPORTER,
                user_id as u32,
                &password,
                allow_existing,
            )
        })
        .context(ks_err!("Failed to initialize user super keys"))
    }

    // Deletes all auth-bound keys when the user's LSKF is removed.
    fn on_user_lskf_removed(user_id: i32) -> Result<()> {
        // Permission check. Must return on error. Do not touch the '?'.
        check_keystore_permission(KeystorePerm::ChangePassword).context(ks_err!())?;

        LEGACY_IMPORTER
            .bulk_delete_user(user_id as u32, true)
            .context(ks_err!("Failed to delete legacy keys."))?;

        DB.with(|db| db.borrow_mut().unbind_auth_bound_keys_for_user(user_id as u32))
            .context(ks_err!("Failed to delete auth-bound keys."))
    }

    fn clear_namespace(&self, domain: Domain, nspace: i64) -> Result<()> {
        // Permission check. Must return on error. Do not touch the '?'.
        check_keystore_permission(KeystorePerm::ClearUID).context("In clear_namespace.")?;

        LEGACY_IMPORTER
            .bulk_delete_uid(domain, nspace)
            .context(ks_err!("Trying to delete legacy keys."))?;
        DB.with(|db| db.borrow_mut().unbind_keys_for_namespace(domain, nspace))
            .context(ks_err!("Trying to delete keys from db."))?;
        self.delete_listener
            .delete_namespace(domain, nspace)
            .context(ks_err!("While invoking the delete listener."))
    }

    fn call_with_watchdog<F>(
        sec_level: SecurityLevel,
        name: &'static str,
        op: &F,
        min_version: Option<i32>,
    ) -> Result<()>
    where
        F: Fn(Strong<dyn IKeyMintDevice>) -> binder::Result<()>,
    {
        let (km_dev, hw_info, _) =
            get_keymint_device(&sec_level).context(ks_err!("getting keymint device"))?;

        if let Some(min_version) = min_version {
            if hw_info.versionNumber < min_version {
                log::info!("skipping {name} for {sec_level:?} since its keymint version {} is less than the minimum required version {min_version}", hw_info.versionNumber);
                return Ok(());
            }
        }

        let _wp = wd::watch_millis_with("Maintenance::call_with_watchdog", 500, (sec_level, name));
        map_km_error(op(km_dev)).with_context(|| ks_err!("calling {}", name))?;
        Ok(())
    }

    fn call_on_all_security_levels<F>(
        name: &'static str,
        op: F,
        min_version: Option<i32>,
    ) -> Result<()>
    where
        F: Fn(Strong<dyn IKeyMintDevice>) -> binder::Result<()>,
    {
        let sec_levels = [
            (SecurityLevel::TRUSTED_ENVIRONMENT, "TRUSTED_ENVIRONMENT"),
            (SecurityLevel::STRONGBOX, "STRONGBOX"),
        ];
        sec_levels.iter().try_fold((), |_result, (sec_level, sec_level_string)| {
            let curr_result = Maintenance::call_with_watchdog(*sec_level, name, &op, min_version);
            match curr_result {
                Ok(()) => log::info!(
                    "Call to {} succeeded for security level {}.",
                    name,
                    &sec_level_string
                ),
                Err(ref e) => {
                    if *sec_level == SecurityLevel::STRONGBOX
                        && e.downcast_ref::<Error>()
                            == Some(&Error::Km(ErrorCode::HARDWARE_TYPE_UNAVAILABLE))
                    {
                        log::info!("Call to {} failed for StrongBox as it is not available", name);
                        return Ok(());
                    } else {
                        log::error!(
                            "Call to {} failed for security level {}: {}.",
                            name,
                            &sec_level_string,
                            e
                        )
                    }
                }
            }
            curr_result
        })
    }

    fn early_boot_ended() -> Result<()> {
        check_keystore_permission(KeystorePerm::EarlyBootEnded)
            .context(ks_err!("Checking permission"))?;
        log::info!("In early_boot_ended.");

        if let Err(e) =
            DB.with(|db| SuperKeyManager::set_up_boot_level_cache(&SUPER_KEY, &mut db.borrow_mut()))
        {
            log::error!("SUPER_KEY.set_up_boot_level_cache failed:\n{:?}\n:(", e);
        }
        Maintenance::call_on_all_security_levels("earlyBootEnded", |dev| dev.earlyBootEnded(), None)
    }

    /// Spawns a thread to send module info if it hasn't already been sent. The thread first waits
    /// for the apex info to be available.
    /// (Module info would have already been sent in the case of a Keystore restart.)
    ///
    /// # Panics
    ///
    /// This method, and methods it calls, panic on failure, because a failure to populate module
    /// information will block the boot process from completing. In this method, this happens if:
    /// - the `apexd.status` property is unable to be monitored
    /// - the `keystore.module_hash.sent` property cannot be updated
    pub fn check_send_module_info() {
        if rustutils::system_properties::read_bool("keystore.module_hash.sent", false)
            .unwrap_or(false)
        {
            log::info!("Module info has already been sent.");
            return;
        }
        if keystore2_flags::attest_modules() {
            std::thread::spawn(move || {
                // Wait for apex info to be available before populating.
                Self::watch_apex_info().unwrap_or_else(|e| {
                    log::error!("failed to monitor apexd.status property: {e:?}");
                    panic!("Terminating due to inaccessibility of apexd.status property, blocking boot: {e:?}");
                });
            });
        } else {
            rustutils::system_properties::write("keystore.module_hash.sent", "true")
                .unwrap_or_else(|e| {
                        log::error!("Failed to set keystore.module_hash.sent to true; this will therefore block boot: {e:?}");
                        panic!("Crashing Keystore because it failed to set keystore.module_hash.sent to true (which blocks boot).");
                    }
                );
        }
    }

    /// Watch the `apexd.status` system property, and read apex module information once
    /// it is `activated`.
    ///
    /// Blocks waiting for system property changes, so must be run in its own thread.
    fn watch_apex_info() -> Result<()> {
        let apex_prop = "apexd.status";
        log::info!("start monitoring '{apex_prop}' property");
        let mut w =
            PropertyWatcher::new(apex_prop).context(ks_err!("PropertyWatcher::new failed"))?;
        loop {
            let value = w.read(|_name, value| Ok(value.to_string()));
            log::info!("property '{apex_prop}' is now '{value:?}'");
            if matches!(value.as_deref(), Ok("activated")) {
                Self::read_and_set_module_info();
                return Ok(());
            }
            log::info!("await a change to '{apex_prop}'...");
            w.wait(None).context(ks_err!("property wait failed"))?;
            log::info!("await a change to '{apex_prop}'...notified");
        }
    }

    /// Read apex information (which is assumed to be present) and propagate module
    /// information to KeyMint instances.
    ///
    /// # Panics
    ///
    /// This method panics on failure, because a failure to populate module information
    /// will block the boot process from completing.  This happens if:
    /// - apex information is not available (precondition)
    /// - KeyMint instances fail to accept module information
    /// - the `keystore.module_hash.sent` property cannot be updated
    fn read_and_set_module_info() {
        let modules = Self::read_apex_info().unwrap_or_else(|e| {
            log::error!("failed to read apex info: {e:?}");
            panic!("Terminating due to unavailability of apex info, blocking boot: {e:?}");
        });
        Self::set_module_info(modules).unwrap_or_else(|e| {
            log::error!("failed to set module info: {e:?}");
            panic!("Terminating due to KeyMint not accepting module info, blocking boot: {e:?}");
        });
        rustutils::system_properties::write("keystore.module_hash.sent", "true").unwrap_or_else(|e| {
            log::error!("failed to set keystore.module_hash.sent property: {e:?}");
            panic!("Terminating due to failure to set keystore.module_hash.sent property, blocking boot: {e:?}");
        });
    }

    fn read_apex_info() -> Result<Vec<ModuleInfo>> {
        let _wp = wd::watch("read_apex_info via IApexService.getActivePackages()");
        let apexd: Strong<dyn IApexService> =
            wait_for_interface("apexservice").context("failed to AIDL connect to apexd")?;
        let packages = apexd.getActivePackages().context("failed to retrieve active packages")?;
        packages
            .into_iter()
            .map(|pkg| {
                log::info!("apex modules += {} version {}", pkg.moduleName, pkg.versionCode);
                let name = OctetString::new(pkg.moduleName.as_bytes()).map_err(|e| {
                    anyhow!("failed to convert '{}' to OCTET_STRING: {e:?}", pkg.moduleName)
                })?;
                Ok(ModuleInfo { name, version: pkg.versionCode })
            })
            .collect()
    }

    fn migrate_key_namespace(source: &KeyDescriptor, destination: &KeyDescriptor) -> Result<()> {
        let calling_uid = ThreadState::get_calling_uid();

        match source.domain {
            Domain::SELINUX | Domain::KEY_ID | Domain::APP => (),
            _ => {
                return Err(Error::Rc(ResponseCode::INVALID_ARGUMENT))
                    .context(ks_err!("Source domain must be one of APP, SELINUX, or KEY_ID."));
            }
        };

        match destination.domain {
            Domain::SELINUX | Domain::APP => (),
            _ => {
                return Err(Error::Rc(ResponseCode::INVALID_ARGUMENT))
                    .context(ks_err!("Destination domain must be one of APP or SELINUX."));
            }
        };

        let user_id = uid_to_android_user(calling_uid);

        let super_key = SUPER_KEY.read().unwrap().get_after_first_unlock_key_by_user_id(user_id);

        DB.with(|db| {
            let (key_id_guard, _) = LEGACY_IMPORTER
                .with_try_import(source, calling_uid, super_key, || {
                    db.borrow_mut().load_key_entry(
                        source,
                        KeyType::Client,
                        KeyEntryLoadBits::NONE,
                        calling_uid,
                        |k, av| {
                            check_key_permission(KeyPerm::Use, k, &av)?;
                            check_key_permission(KeyPerm::Delete, k, &av)?;
                            check_key_permission(KeyPerm::Grant, k, &av)
                        },
                    )
                })
                .context(ks_err!("Failed to load key blob."))?;
            {
                db.borrow_mut().migrate_key_namespace(key_id_guard, destination, calling_uid, |k| {
                    check_key_permission(KeyPerm::Rebind, k, &None)
                })
            }
        })
    }

    fn delete_all_keys() -> Result<()> {
        // Security critical permission check. This statement must return on fail.
        check_keystore_permission(KeystorePerm::DeleteAllKeys)
            .context(ks_err!("Checking permission"))?;
        log::info!("In delete_all_keys.");

        Maintenance::call_on_all_security_levels("deleteAllKeys", |dev| dev.deleteAllKeys(), None)
    }

    fn get_app_uids_affected_by_sid(
        user_id: i32,
        secure_user_id: i64,
    ) -> Result<std::vec::Vec<i64>> {
        // This method is intended to be called by Settings and discloses a list of apps
        // associated with a user, so it requires the "android.permission.MANAGE_USERS"
        // permission (to avoid leaking list of apps to unauthorized callers).
        check_get_app_uids_affected_by_sid_permissions().context(ks_err!())?;
        DB.with(|db| db.borrow_mut().get_app_uids_affected_by_sid(user_id, secure_user_id))
            .context(ks_err!("Failed to get app UIDs affected by SID"))
    }

    fn dump_state(&self, f: &mut dyn std::io::Write) -> std::io::Result<()> {
        writeln!(f, "keystore2 running")?;
        writeln!(f)?;

        // Display underlying device information.
        //
        // Note that this chunk of output is parsed in a GTS test, so do not change the format
        // without checking that the test still works.
        for sec_level in &[SecurityLevel::TRUSTED_ENVIRONMENT, SecurityLevel::STRONGBOX] {
            let Ok((_dev, hw_info, uuid)) = get_keymint_device(sec_level) else { continue };

            writeln!(f, "Device info for {sec_level:?} with {uuid:?}")?;
            writeln!(f, "  HAL version:              {}", hw_info.versionNumber)?;
            writeln!(f, "  Implementation name:      {}", hw_info.keyMintName)?;
            writeln!(f, "  Implementation author:    {}", hw_info.keyMintAuthorName)?;
            writeln!(f, "  Timestamp token required: {}", hw_info.timestampTokenRequired)?;
        }
        writeln!(f)?;

        // Display module attestation information
        {
            let info = ENCODED_MODULE_INFO.read().unwrap();
            if let Some(info) = info.as_ref() {
                writeln!(f, "Attested module information (DER-encoded):")?;
                writeln!(f, "  {}", hex::encode(info))?;
                writeln!(f)?;
            } else {
                writeln!(f, "Attested module information not set")?;
                writeln!(f)?;
            }
        }

        // Display database size information.
        match crate::metrics_store::pull_storage_stats() {
            Ok(atoms) => {
                writeln!(f, "Database size information (in bytes):")?;
                for atom in atoms {
                    if let StorageStats(stats) = &atom.payload {
                        let stype = format!("{:?}", stats.storage_type);
                        if stats.unused_size == 0 {
                            writeln!(f, "  {:<40}: {:>12}", stype, stats.size)?;
                        } else {
                            writeln!(
                                f,
                                "  {:<40}: {:>12} (unused {})",
                                stype, stats.size, stats.unused_size
                            )?;
                        }
                    }
                }
            }
            Err(e) => {
                writeln!(f, "Failed to retrieve storage stats: {e:?}")?;
            }
        }
        writeln!(f)?;

        // Display database config information.
        writeln!(f, "Database configuration:")?;
        DB.with(|db| -> std::io::Result<()> {
            let pragma_str = |f: &mut dyn std::io::Write, name| -> std::io::Result<()> {
                let mut db = db.borrow_mut();
                let value: String = db
                    .pragma(name)
                    .unwrap_or_else(|e| format!("unknown value for '{name}', failed: {e:?}"));
                writeln!(f, "  {name} = {value}")
            };
            let pragma_i32 = |f: &mut dyn std::io::Write, name| -> std::io::Result<()> {
                let mut db = db.borrow_mut();
                let value: i32 = db.pragma(name).unwrap_or_else(|e| {
                    log::error!("unknown value for '{name}', failed: {e:?}");
                    -1
                });
                writeln!(f, "  {name} = {value}")
            };
            pragma_i32(f, "auto_vacuum")?;
            pragma_str(f, "journal_mode")?;
            pragma_i32(f, "journal_size_limit")?;
            pragma_i32(f, "synchronous")?;
            pragma_i32(f, "schema_version")?;
            pragma_i32(f, "user_version")?;
            Ok(())
        })?;
        writeln!(f)?;

        // Display accumulated metrics.
        writeln!(f, "Metrics information:")?;
        writeln!(f)?;
        write!(f, "{:?}", *crate::metrics_store::METRICS_STORE)?;
        writeln!(f)?;

        // Reminder: any additional information added to the `dump_state()` output needs to be
        // careful not to include confidential information (e.g. key material).

        Ok(())
    }

    fn set_module_info(module_info: Vec<ModuleInfo>) -> Result<()> {
        log::info!("set_module_info with {} modules", module_info.len());
        let encoding = Self::encode_module_info(module_info)
            .map_err(|e| anyhow!({ e }))
            .context(ks_err!("Failed to encode module_info"))?;
        let hash = digest::Sha256::hash(&encoding).to_vec();

        {
            let mut saved = ENCODED_MODULE_INFO.write().unwrap();
            if let Some(saved_encoding) = &*saved {
                if *saved_encoding == encoding {
                    log::warn!(
                        "Module info already set, ignoring repeated attempt to set same info."
                    );
                    return Ok(());
                }
                return Err(Error::Rc(ResponseCode::INVALID_ARGUMENT)).context(ks_err!(
                    "Failed to set module info as it is already set to a different value."
                ));
            }
            *saved = Some(encoding);
        }

        let kps =
            vec![KeyParameter { tag: Tag::MODULE_HASH, value: KeyParameterValue::Blob(hash) }];

        Maintenance::call_on_all_security_levels(
            "setAdditionalAttestationInfo",
            |dev| dev.setAdditionalAttestationInfo(&kps),
            Some(KEYMINT_V4),
        )
    }

    fn encode_module_info(module_info: Vec<ModuleInfo>) -> Result<Vec<u8>, der::Error> {
        SetOfVec::<ModuleInfo>::from_iter(module_info.into_iter())?.to_der()
    }
}

impl Interface for Maintenance {
    fn dump(
        &self,
        f: &mut dyn std::io::Write,
        _args: &[&std::ffi::CStr],
    ) -> Result<(), binder::StatusCode> {
        log::info!("dump()");
        let _wp = wd::watch("IKeystoreMaintenance::dump");
        check_dump_permission().map_err(|_e| {
            log::error!("dump permission denied");
            binder::StatusCode::PERMISSION_DENIED
        })?;

        self.dump_state(f).map_err(|e| {
            log::error!("dump_state failed: {e:?}");
            binder::StatusCode::UNKNOWN_ERROR
        })
    }
}

impl IKeystoreMaintenance for Maintenance {
    fn onUserAdded(&self, user_id: i32) -> BinderResult<()> {
        log::info!("onUserAdded(user={user_id})");
        let _wp = wd::watch("IKeystoreMaintenance::onUserAdded");
        self.add_or_remove_user(user_id).map_err(into_logged_binder)
    }

    fn initUserSuperKeys(
        &self,
        user_id: i32,
        password: &[u8],
        allow_existing: bool,
    ) -> BinderResult<()> {
        log::info!("initUserSuperKeys(user={user_id}, allow_existing={allow_existing})");
        let _wp = wd::watch("IKeystoreMaintenance::initUserSuperKeys");
        self.init_user_super_keys(user_id, password.into(), allow_existing)
            .map_err(into_logged_binder)
    }

    fn onUserRemoved(&self, user_id: i32) -> BinderResult<()> {
        log::info!("onUserRemoved(user={user_id})");
        let _wp = wd::watch("IKeystoreMaintenance::onUserRemoved");
        self.add_or_remove_user(user_id).map_err(into_logged_binder)
    }

    fn onUserLskfRemoved(&self, user_id: i32) -> BinderResult<()> {
        log::info!("onUserLskfRemoved(user={user_id})");
        let _wp = wd::watch("IKeystoreMaintenance::onUserLskfRemoved");
        Self::on_user_lskf_removed(user_id).map_err(into_logged_binder)
    }

    fn clearNamespace(&self, domain: Domain, nspace: i64) -> BinderResult<()> {
        log::info!("clearNamespace({domain:?}, nspace={nspace})");
        let _wp = wd::watch("IKeystoreMaintenance::clearNamespace");
        self.clear_namespace(domain, nspace).map_err(into_logged_binder)
    }

    fn earlyBootEnded(&self) -> BinderResult<()> {
        log::info!("earlyBootEnded()");
        let _wp = wd::watch("IKeystoreMaintenance::earlyBootEnded");
        Self::early_boot_ended().map_err(into_logged_binder)
    }

    fn migrateKeyNamespace(
        &self,
        source: &KeyDescriptor,
        destination: &KeyDescriptor,
    ) -> BinderResult<()> {
        log::info!("migrateKeyNamespace(src={source:?}, dest={destination:?})");
        let _wp = wd::watch("IKeystoreMaintenance::migrateKeyNamespace");
        Self::migrate_key_namespace(source, destination).map_err(into_logged_binder)
    }

    fn deleteAllKeys(&self) -> BinderResult<()> {
        log::warn!("deleteAllKeys() invoked, indicating initial setup or post-factory reset");
        let _wp = wd::watch("IKeystoreMaintenance::deleteAllKeys");
        Self::delete_all_keys().map_err(into_logged_binder)
    }

    fn getAppUidsAffectedBySid(
        &self,
        user_id: i32,
        secure_user_id: i64,
    ) -> BinderResult<std::vec::Vec<i64>> {
        log::info!("getAppUidsAffectedBySid(secure_user_id={secure_user_id:?})");
        let _wp = wd::watch("IKeystoreMaintenance::getAppUidsAffectedBySid");
        Self::get_app_uids_affected_by_sid(user_id, secure_user_id).map_err(into_logged_binder)
    }
}
