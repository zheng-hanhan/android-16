// Copyright 2022, The Android Open Source Project
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

use crate::keystore2_client_test_utils::{delete_all_entries, delete_app_key, verify_aliases};
use android_system_keystore2::aidl::android::system::keystore2::{
    Domain::Domain, IKeystoreService::IKeystoreService, KeyDescriptor::KeyDescriptor,
    KeyPermission::KeyPermission, ResponseCode::ResponseCode,
};
use keystore2_test_utils::{
    get_keystore_service, key_generations, key_generations::Error, run_as, SecLevel,
};
use nix::unistd::getuid;
use rustutils::users::AID_USER_OFFSET;
use std::collections::HashSet;
use std::fmt::Write;

/// Try to find a key with given key parameters using `listEntries` API.
fn key_alias_exists(
    keystore2: &binder::Strong<dyn IKeystoreService>,
    domain: Domain,
    nspace: i64,
    alias: String,
) -> bool {
    let key_descriptors = keystore2.listEntries(domain, nspace).unwrap();
    let alias_count = key_descriptors
        .into_iter()
        .map(|key| key.alias.unwrap())
        .filter(|key_alias| *key_alias == alias)
        .count();

    alias_count != 0
}

/// List key entries with domain as SELINUX and APP.
/// 1. Generate a key with domain as SELINUX and find this key entry in list of keys retrieved from
///    `listEntries` with domain SELINUX. Test should be able find this key entry successfully.
/// 2. Grant above generated Key to a user.
/// 3. In a user context, generate a new key with domain as APP. Try to list the key entries with
///    domain APP. Test should find only one key entry that should be the key generated in user
///    context. GRANT keys shouldn't be part of this list.
#[test]
fn keystore2_list_entries_success() {
    const USER_ID: u32 = 91;
    const APPLICATION_ID: u32 = 10006;
    static GRANTEE_UID: u32 = USER_ID * AID_USER_OFFSET + APPLICATION_ID;
    static GRANTEE_GID: u32 = GRANTEE_UID;

    let gen_key_fn = || {
        let sl = SecLevel::tee();

        let alias = format!("list_entries_grant_key1_{}", getuid());

        // Make sure there is no key exist with this `alias` in `SELINUX` domain and
        // `SELINUX_SHELL_NAMESPACE` namespace.
        if key_alias_exists(
            &sl.keystore2,
            Domain::SELINUX,
            key_generations::SELINUX_SHELL_NAMESPACE,
            alias.to_string(),
        ) {
            sl.keystore2
                .deleteKey(&KeyDescriptor {
                    domain: Domain::SELINUX,
                    nspace: key_generations::SELINUX_SHELL_NAMESPACE,
                    alias: Some(alias.to_string()),
                    blob: None,
                })
                .unwrap();
        }

        // Generate a key with above defined `alias`.
        let key_metadata = key_generations::generate_ec_p256_signing_key(
            &sl,
            Domain::SELINUX,
            key_generations::SELINUX_SHELL_NAMESPACE,
            Some(alias.to_string()),
            None,
        )
        .unwrap();

        // Verify that above generated key entry is listed with domain SELINUX and
        // namespace SELINUX_SHELL_NAMESPACE
        assert!(key_alias_exists(
            &sl.keystore2,
            Domain::SELINUX,
            key_generations::SELINUX_SHELL_NAMESPACE,
            alias,
        ));

        // Grant a key with GET_INFO permission.
        let access_vector = KeyPermission::GET_INFO.0;
        sl.keystore2
            .grant(&key_metadata.key, GRANTEE_UID.try_into().unwrap(), access_vector)
            .unwrap();
    };

    // Safety: only one thread at this point (enforced by `AndroidTest.xml` setting
    // `--test-threads=1`), and nothing yet done with binder.
    unsafe { run_as::run_as_root(gen_key_fn) };

    // In user context validate list of key entries associated with it.
    let list_keys_fn = move || {
        let sl = SecLevel::tee();
        let alias = format!("list_entries_success_key{}", getuid());

        let key_metadata = key_generations::generate_ec_p256_signing_key(
            &sl,
            Domain::APP,
            -1,
            Some(alias.to_string()),
            None,
        )
        .unwrap();

        // Make sure there is only one existing key entry and that should be the same key
        // generated in this user context. Granted key shouldn't be included in this list.
        let key_descriptors = sl.keystore2.listEntries(Domain::APP, -1).unwrap();
        assert_eq!(1, key_descriptors.len());

        let key = key_descriptors.first().unwrap();
        assert_eq!(key.alias, Some(alias));
        assert_eq!(key.nspace, GRANTEE_UID.try_into().unwrap());
        assert_eq!(key.domain, Domain::APP);

        sl.keystore2.deleteKey(&key_metadata.key).unwrap();

        let key_descriptors = sl.keystore2.listEntries(Domain::APP, -1).unwrap();
        assert_eq!(0, key_descriptors.len());
    };

    // Safety: only one thread at this point (enforced by `AndroidTest.xml` setting
    // `--test-threads=1`), and nothing yet done with binder.
    unsafe { run_as::run_as_app(GRANTEE_UID, GRANTEE_GID, list_keys_fn) };
}

/// Try to list the key entries with domain SELINUX from user context where user doesn't possesses
/// `GET_INFO` permission for specified namespace. Test should fail to list key entries with error
/// response code `PERMISSION_DENIED`.
#[test]
fn keystore2_list_entries_fails_perm_denied() {
    let auid = 91 * AID_USER_OFFSET + 10001;
    let agid = 91 * AID_USER_OFFSET + 10001;
    let list_keys_fn = move || {
        let keystore2 = get_keystore_service();

        let result = key_generations::map_ks_error(
            keystore2.listEntries(Domain::SELINUX, key_generations::SELINUX_SHELL_NAMESPACE),
        );
        assert!(result.is_err());
        assert_eq!(Error::Rc(ResponseCode::PERMISSION_DENIED), result.unwrap_err());
    };

    // Safety: only one thread at this point (enforced by `AndroidTest.xml` setting
    // `--test-threads=1`), and nothing yet done with binder.
    unsafe { run_as::run_as_app(auid, agid, list_keys_fn) };
}

/// Try to list key entries with domain BLOB. Test should fail with error repose code
/// `INVALID_ARGUMENT`.
#[test]
fn keystore2_list_entries_fails_invalid_arg() {
    let keystore2 = get_keystore_service();

    let result = key_generations::map_ks_error(
        keystore2.listEntries(Domain::BLOB, key_generations::SELINUX_SHELL_NAMESPACE),
    );
    assert!(result.is_err());
    assert_eq!(Error::Rc(ResponseCode::INVALID_ARGUMENT), result.unwrap_err());
}

/// Import large number of Keystore entries with long aliases and try to list aliases
/// of all the entries in the keystore.
#[test]
fn keystore2_list_entries_with_long_aliases_success() {
    const USER_ID: u32 = 92;
    const APPLICATION_ID: u32 = 10002;
    static CLIENT_UID: u32 = USER_ID * AID_USER_OFFSET + APPLICATION_ID;
    static CLIENT_GID: u32 = CLIENT_UID;

    let import_keys_fn = || {
        let sl = SecLevel::tee();

        // Make sure there are no keystore entries exist before adding new entries.
        let key_descriptors = sl.keystore2.listEntries(Domain::APP, -1).unwrap();
        if !key_descriptors.is_empty() {
            key_descriptors.into_iter().map(|key| key.alias.unwrap()).for_each(|alias| {
                delete_app_key(&sl.keystore2, &alias).unwrap();
            });
        }

        let mut imported_key_aliases = HashSet::new();

        // Import 100 keys with aliases of length 6000.
        for count in 1..101 {
            let mut alias = String::new();
            write!(alias, "{}_{}", "X".repeat(6000), count).unwrap();
            imported_key_aliases.insert(alias.clone());

            let result = key_generations::import_aes_key(&sl, Domain::APP, -1, Some(alias));
            assert!(result.is_ok());
        }

        // b/222287335 Limiting Keystore `listEntries` API to return subset of the Keystore
        // entries to avoid running out of binder buffer space.
        // To verify that all the imported key aliases are present in Keystore,
        //  - get the list of entries from Keystore
        //  - check whether the retrieved key entries list is a subset of imported key aliases
        //  - delete this subset of keystore entries from Keystore as well as from imported
        //    list of key aliases
        //  - continue above steps till it cleanup all the imported keystore entries.
        while !imported_key_aliases.is_empty() {
            let key_descriptors = sl.keystore2.listEntries(Domain::APP, -1).unwrap();

            // Check retrieved key entries list is a subset of imported keys list.
            assert!(key_descriptors
                .iter()
                .all(|key| imported_key_aliases.contains(key.alias.as_ref().unwrap())));

            // Delete the listed key entries from Keystore as well as from imported keys list.
            key_descriptors.into_iter().map(|key| key.alias.unwrap()).for_each(|alias| {
                delete_app_key(&sl.keystore2, &alias).unwrap();
                assert!(imported_key_aliases.remove(&alias));
            });
        }

        assert!(imported_key_aliases.is_empty());
    };

    // Safety: only one thread at this point (enforced by `AndroidTest.xml` setting
    // `--test-threads=1`), and nothing yet done with binder.
    unsafe { run_as::run_as_app(CLIENT_UID, CLIENT_GID, import_keys_fn) };
}

/// Import large number of Keystore entries with long aliases such that the
/// aliases list would exceed the binder transaction size limit.
/// Try to list aliases of all the entries in the keystore using `listEntriesBatched` API.
#[test]
fn keystore2_list_entries_batched_with_long_aliases_success() {
    const USER_ID: u32 = 92;
    const APPLICATION_ID: u32 = 10002;
    static CLIENT_UID: u32 = USER_ID * AID_USER_OFFSET + APPLICATION_ID;
    static CLIENT_GID: u32 = CLIENT_UID;

    let import_keys_fn = || {
        let sl = SecLevel::tee();

        // Make sure there are no keystore entries exist before adding new entries.
        delete_all_entries(&sl.keystore2);

        // Import 100 keys with aliases of length 6000.
        let mut imported_key_aliases =
            key_generations::import_aes_keys(&sl, "X".repeat(6000), 1..101).unwrap();
        assert_eq!(
            sl.keystore2.getNumberOfEntries(Domain::APP, -1).unwrap(),
            100,
            "Error while importing keys"
        );

        let mut start_past_alias = None;
        let mut alias;
        while !imported_key_aliases.is_empty() {
            let key_descriptors =
                sl.keystore2.listEntriesBatched(Domain::APP, -1, start_past_alias).unwrap();

            // Check retrieved key entries list is a subset of imported keys list.
            assert!(key_descriptors
                .iter()
                .all(|key| imported_key_aliases.contains(key.alias.as_ref().unwrap())));

            alias = key_descriptors.last().unwrap().alias.clone().unwrap();
            start_past_alias = Some(alias.as_ref());
            // Delete the listed key entries from imported keys list.
            key_descriptors.into_iter().map(|key| key.alias.unwrap()).for_each(|alias| {
                assert!(imported_key_aliases.remove(&alias));
            });
        }

        assert!(imported_key_aliases.is_empty());
        delete_all_entries(&sl.keystore2);
        assert_eq!(
            sl.keystore2.getNumberOfEntries(Domain::APP, -1).unwrap(),
            0,
            "Error while doing cleanup"
        );
    };

    // Safety: only one thread at this point (enforced by `AndroidTest.xml` setting
    // `--test-threads=1`), and nothing yet done with binder.
    unsafe { run_as::run_as_app(CLIENT_UID, CLIENT_GID, import_keys_fn) };
}

/// Import keys from multiple processes with same user context and try to list the keystore entries
/// using `listEntriesBatched` API.
///  - Create two processes sharing user-id.
///  - From process-1, import 3 keys and try to list the keys using `listEntriesBatched`
///    without `startingPastAlias`, it should list all the 3 entries.
///  - From process-2, import another 5 keys and try to list the keys using `listEntriesBatched`
///    with the alias of the last key listed in process-1 as `startingPastAlias`. It should list
///    all the entries whose alias is greater than the provided `startingPastAlias`.
///  - From process-2 try to list all entries accessible to it by using `listEntriesBatched` with
///    `startingPastAlias` as None. It should list all the keys imported in process-1 and process-2.
#[test]
fn keystore2_list_entries_batched_with_multi_procs_success() {
    const USER_ID: u32 = 92;
    const APPLICATION_ID: u32 = 10002;
    static CLIENT_UID: u32 = USER_ID * AID_USER_OFFSET + APPLICATION_ID;
    static CLIENT_GID: u32 = CLIENT_UID;
    static ALIAS_PREFIX: &str = "key_test_batch_list";

    let import_keys_fn = || {
        let sl = SecLevel::tee();

        // Make sure there are no keystore entries exist before adding new entries.
        delete_all_entries(&sl.keystore2);

        // Import 3 keys with below aliases -
        // [key_test_batch_list_1, key_test_batch_list_2, key_test_batch_list_3]
        let imported_key_aliases =
            key_generations::import_aes_keys(&sl, ALIAS_PREFIX.to_string(), 1..4).unwrap();
        assert_eq!(
            sl.keystore2.getNumberOfEntries(Domain::APP, -1).unwrap(),
            3,
            "Error while importing keys"
        );

        // List all entries in keystore for this user-id.
        let key_descriptors = sl.keystore2.listEntriesBatched(Domain::APP, -1, None).unwrap();
        assert_eq!(key_descriptors.len(), 3);

        // Makes sure all listed aliases are matching with imported keys aliases.
        assert!(key_descriptors
            .iter()
            .all(|key| imported_key_aliases.contains(key.alias.as_ref().unwrap())));
    };

    // Safety: only one thread at this point (enforced by `AndroidTest.xml` setting
    // `--test-threads=1`), and nothing yet done with binder.
    unsafe { run_as::run_as_app(CLIENT_UID, CLIENT_GID, import_keys_fn) };

    let import_more_fn = || {
        let sl = SecLevel::tee();

        // Import another 5 keys with below aliases -
        // [ key_test_batch_list_4, key_test_batch_list_5, key_test_batch_list_6,
        //   key_test_batch_list_7, key_test_batch_list_8 ]
        let mut imported_key_aliases =
            key_generations::import_aes_keys(&sl, ALIAS_PREFIX.to_string(), 4..9).unwrap();

        // Above context already 3 keys are imported, in this context 5 keys are imported,
        // total 8 keystore entries are expected to be present in Keystore for this user-id.
        assert_eq!(
            sl.keystore2.getNumberOfEntries(Domain::APP, -1).unwrap(),
            8,
            "Error while importing keys"
        );

        // List keystore entries with `start_past_alias` as "key_test_batch_list_3".
        // `listEntriesBatched` should list all the keystore entries with
        // alias > "key_test_batch_list_3".
        let key_descriptors = sl
            .keystore2
            .listEntriesBatched(Domain::APP, -1, Some("key_test_batch_list_3"))
            .unwrap();
        assert_eq!(key_descriptors.len(), 5);

        // Make sure above listed aliases are matching with imported keys aliases.
        assert!(key_descriptors
            .iter()
            .all(|key| imported_key_aliases.contains(key.alias.as_ref().unwrap())));

        // List all keystore entries with `start_past_alias` as `None`.
        // `listEntriesBatched` should list all the keystore entries.
        let key_descriptors = sl.keystore2.listEntriesBatched(Domain::APP, -1, None).unwrap();
        assert_eq!(key_descriptors.len(), 8);

        // Include previously imported keys aliases as well
        imported_key_aliases.insert(ALIAS_PREFIX.to_owned() + "_1");
        imported_key_aliases.insert(ALIAS_PREFIX.to_owned() + "_2");
        imported_key_aliases.insert(ALIAS_PREFIX.to_owned() + "_3");

        // Make sure all the above listed aliases are matching with imported keys aliases.
        assert!(key_descriptors
            .iter()
            .all(|key| imported_key_aliases.contains(key.alias.as_ref().unwrap())));

        delete_all_entries(&sl.keystore2);
        assert_eq!(
            sl.keystore2.getNumberOfEntries(Domain::APP, -1).unwrap(),
            0,
            "Error while doing cleanup"
        );
    };

    // Safety: only one thread at this point (enforced by `AndroidTest.xml` setting
    // `--test-threads=1`), and nothing yet done with binder.
    unsafe { run_as::run_as_app(CLIENT_UID, CLIENT_GID, import_more_fn) };
}

#[test]
fn keystore2_list_entries_batched_with_empty_keystore_success() {
    const USER_ID: u32 = 92;
    const APPLICATION_ID: u32 = 10002;
    static CLIENT_UID: u32 = USER_ID * AID_USER_OFFSET + APPLICATION_ID;
    static CLIENT_GID: u32 = CLIENT_UID;

    let list_keys_fn = || {
        let keystore2 = get_keystore_service();

        // Make sure there are no keystore entries exist before adding new entries.
        delete_all_entries(&keystore2);

        // List all entries in keystore for this user-id, pass startingPastAlias = None
        let key_descriptors = keystore2.listEntriesBatched(Domain::APP, -1, None).unwrap();
        assert_eq!(key_descriptors.len(), 0);

        // List all entries in keystore for this user-id, pass startingPastAlias = <random value>
        let key_descriptors =
            keystore2.listEntriesBatched(Domain::APP, -1, Some("startingPastAlias")).unwrap();
        assert_eq!(key_descriptors.len(), 0);
    };

    // Safety: only one thread at this point (enforced by `AndroidTest.xml` setting
    // `--test-threads=1`), and nothing yet done with binder.
    unsafe { run_as::run_as_app(CLIENT_UID, CLIENT_GID, list_keys_fn) };
}

/// Import a key with SELINUX as domain, list aliases using `listEntriesBatched`.
/// Test should successfully list the imported key.
#[test]
fn keystore2_list_entries_batched_with_selinux_domain_success() {
    let sl = SecLevel::tee();

    let alias = "test_selinux_key_list_alias_batched";
    let _result = sl.keystore2.deleteKey(&KeyDescriptor {
        domain: Domain::SELINUX,
        nspace: key_generations::SELINUX_SHELL_NAMESPACE,
        alias: Some(alias.to_string()),
        blob: None,
    });

    let initial_count = sl
        .keystore2
        .getNumberOfEntries(Domain::SELINUX, key_generations::SELINUX_SHELL_NAMESPACE)
        .unwrap();

    key_generations::import_aes_key(
        &sl,
        Domain::SELINUX,
        key_generations::SELINUX_SHELL_NAMESPACE,
        Some(alias.to_string()),
    )
    .unwrap();

    assert_eq!(
        sl.keystore2
            .getNumberOfEntries(Domain::SELINUX, key_generations::SELINUX_SHELL_NAMESPACE)
            .unwrap(),
        initial_count + 1,
        "Error while getting number of keystore entries accessible."
    );

    let key_descriptors = sl
        .keystore2
        .listEntriesBatched(Domain::SELINUX, key_generations::SELINUX_SHELL_NAMESPACE, None)
        .unwrap();
    assert_eq!(key_descriptors.len(), (initial_count + 1) as usize);

    let count =
        key_descriptors.into_iter().map(|key| key.alias.unwrap()).filter(|a| a == alias).count();
    assert_eq!(count, 1);

    sl.keystore2
        .deleteKey(&KeyDescriptor {
            domain: Domain::SELINUX,
            nspace: key_generations::SELINUX_SHELL_NAMESPACE,
            alias: Some(alias.to_string()),
            blob: None,
        })
        .unwrap();
}

#[test]
fn keystore2_list_entries_batched_validate_count_and_order_success() {
    const USER_ID: u32 = 92;
    const APPLICATION_ID: u32 = 10002;
    static CLIENT_UID: u32 = USER_ID * AID_USER_OFFSET + APPLICATION_ID;
    static CLIENT_GID: u32 = CLIENT_UID;
    static ALIAS_PREFIX: &str = "key_test_batch_list";

    let list_keys_fn = || {
        let sl = SecLevel::tee();

        // Make sure there are no keystore entries exist before adding new entries.
        delete_all_entries(&sl.keystore2);

        // Import keys with below mentioned aliases -
        // [
        //   key_test_batch_list_1,
        //   key_test_batch_list_2,
        //   key_test_batch_list_3,
        //   key_test_batch_list_4,
        //   key_test_batch_list_5,
        //   key_test_batch_list_10,
        //   key_test_batch_list_11,
        //   key_test_batch_list_12,
        //   key_test_batch_list_21,
        //   key_test_batch_list_22,
        // ]
        let _imported_key_aliases =
            key_generations::import_aes_keys(&sl, ALIAS_PREFIX.to_string(), 1..6).unwrap();
        assert_eq!(
            sl.keystore2.getNumberOfEntries(Domain::APP, -1).unwrap(),
            5,
            "Error while importing keys"
        );
        let _imported_key_aliases =
            key_generations::import_aes_keys(&sl, ALIAS_PREFIX.to_string(), 10..13).unwrap();
        assert_eq!(
            sl.keystore2.getNumberOfEntries(Domain::APP, -1).unwrap(),
            8,
            "Error while importing keys"
        );
        let _imported_key_aliases =
            key_generations::import_aes_keys(&sl, ALIAS_PREFIX.to_string(), 21..23).unwrap();
        assert_eq!(
            sl.keystore2.getNumberOfEntries(Domain::APP, -1).unwrap(),
            10,
            "Error while importing keys"
        );

        // List the aliases using given `startingPastAlias` and verify the listed
        // aliases with the expected list of aliases.
        verify_aliases(&sl.keystore2, Some(format!("{}{}", ALIAS_PREFIX, "_5").as_str()), vec![]);

        verify_aliases(
            &sl.keystore2,
            Some(format!("{}{}", ALIAS_PREFIX, "_4").as_str()),
            vec![ALIAS_PREFIX.to_owned() + "_5"],
        );

        verify_aliases(
            &sl.keystore2,
            Some(format!("{}{}", ALIAS_PREFIX, "_3").as_str()),
            vec![ALIAS_PREFIX.to_owned() + "_4", ALIAS_PREFIX.to_owned() + "_5"],
        );

        verify_aliases(
            &sl.keystore2,
            Some(format!("{}{}", ALIAS_PREFIX, "_2").as_str()),
            vec![
                ALIAS_PREFIX.to_owned() + "_21",
                ALIAS_PREFIX.to_owned() + "_22",
                ALIAS_PREFIX.to_owned() + "_3",
                ALIAS_PREFIX.to_owned() + "_4",
                ALIAS_PREFIX.to_owned() + "_5",
            ],
        );

        verify_aliases(
            &sl.keystore2,
            Some(format!("{}{}", ALIAS_PREFIX, "_1").as_str()),
            vec![
                ALIAS_PREFIX.to_owned() + "_10",
                ALIAS_PREFIX.to_owned() + "_11",
                ALIAS_PREFIX.to_owned() + "_12",
                ALIAS_PREFIX.to_owned() + "_2",
                ALIAS_PREFIX.to_owned() + "_21",
                ALIAS_PREFIX.to_owned() + "_22",
                ALIAS_PREFIX.to_owned() + "_3",
                ALIAS_PREFIX.to_owned() + "_4",
                ALIAS_PREFIX.to_owned() + "_5",
            ],
        );

        verify_aliases(
            &sl.keystore2,
            Some(ALIAS_PREFIX),
            vec![
                ALIAS_PREFIX.to_owned() + "_1",
                ALIAS_PREFIX.to_owned() + "_10",
                ALIAS_PREFIX.to_owned() + "_11",
                ALIAS_PREFIX.to_owned() + "_12",
                ALIAS_PREFIX.to_owned() + "_2",
                ALIAS_PREFIX.to_owned() + "_21",
                ALIAS_PREFIX.to_owned() + "_22",
                ALIAS_PREFIX.to_owned() + "_3",
                ALIAS_PREFIX.to_owned() + "_4",
                ALIAS_PREFIX.to_owned() + "_5",
            ],
        );

        verify_aliases(
            &sl.keystore2,
            None,
            vec![
                ALIAS_PREFIX.to_owned() + "_1",
                ALIAS_PREFIX.to_owned() + "_10",
                ALIAS_PREFIX.to_owned() + "_11",
                ALIAS_PREFIX.to_owned() + "_12",
                ALIAS_PREFIX.to_owned() + "_2",
                ALIAS_PREFIX.to_owned() + "_21",
                ALIAS_PREFIX.to_owned() + "_22",
                ALIAS_PREFIX.to_owned() + "_3",
                ALIAS_PREFIX.to_owned() + "_4",
                ALIAS_PREFIX.to_owned() + "_5",
            ],
        );
    };

    // Safety: only one thread at this point (enforced by `AndroidTest.xml` setting
    // `--test-threads=1`), and nothing yet done with binder.
    unsafe { run_as::run_as_app(CLIENT_UID, CLIENT_GID, list_keys_fn) };
}

/// Try to list the key entries with domain SELINUX from user context where user doesn't possesses
/// `GET_INFO` permission for specified namespace. Test should fail to list key entries with error
/// response code `PERMISSION_DENIED`.
#[test]
fn keystore2_list_entries_batched_fails_perm_denied() {
    let auid = 91 * AID_USER_OFFSET + 10001;
    let agid = 91 * AID_USER_OFFSET + 10001;
    let list_keys_fn = move || {
        let keystore2 = get_keystore_service();

        let result = key_generations::map_ks_error(keystore2.listEntriesBatched(
            Domain::SELINUX,
            key_generations::SELINUX_SHELL_NAMESPACE,
            None,
        ));
        assert!(result.is_err());
        assert_eq!(Error::Rc(ResponseCode::PERMISSION_DENIED), result.unwrap_err());
    };

    // Safety: only one thread at this point (enforced by `AndroidTest.xml` setting
    // `--test-threads=1`), and nothing yet done with binder.
    unsafe { run_as::run_as_app(auid, agid, list_keys_fn) };
}

/// Try to list key entries with domain BLOB. Test should fail with error response code
/// `INVALID_ARGUMENT`.
#[test]
fn keystore2_list_entries_batched_fails_invalid_arg() {
    let keystore2 = get_keystore_service();

    let result = key_generations::map_ks_error(keystore2.listEntriesBatched(
        Domain::BLOB,
        key_generations::SELINUX_SHELL_NAMESPACE,
        None,
    ));
    assert!(result.is_err());
    assert_eq!(Error::Rc(ResponseCode::INVALID_ARGUMENT), result.unwrap_err());
}

/// Try to get the number of key entries with domain SELINUX from user context where user doesn't
/// possesses `GET_INFO` permission for specified namespace. Test should fail to list key entries
/// with error response code `PERMISSION_DENIED`.
#[test]
fn keystore2_get_number_of_entries_fails_perm_denied() {
    let auid = 91 * AID_USER_OFFSET + 10001;
    let agid = 91 * AID_USER_OFFSET + 10001;
    let get_num_fn = move || {
        let keystore2 = get_keystore_service();

        let result = key_generations::map_ks_error(
            keystore2.getNumberOfEntries(Domain::SELINUX, key_generations::SELINUX_SHELL_NAMESPACE),
        );
        assert!(result.is_err());
        assert_eq!(Error::Rc(ResponseCode::PERMISSION_DENIED), result.unwrap_err());
    };

    // Safety: only one thread at this point (enforced by `AndroidTest.xml` setting
    // `--test-threads=1`), and nothing yet done with binder.
    unsafe { run_as::run_as_app(auid, agid, get_num_fn) };
}

/// Try to get number of key entries with domain BLOB. Test should fail with error response code
/// `INVALID_ARGUMENT`.
#[test]
fn keystore2_get_number_of_entries_fails_invalid_arg() {
    let keystore2 = get_keystore_service();

    let result = key_generations::map_ks_error(
        keystore2.getNumberOfEntries(Domain::BLOB, key_generations::SELINUX_SHELL_NAMESPACE),
    );
    assert!(result.is_err());
    assert_eq!(Error::Rc(ResponseCode::INVALID_ARGUMENT), result.unwrap_err());
}
