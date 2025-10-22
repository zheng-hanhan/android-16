// Copyright 2023, The Android Open Source Project
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

use android_hardware_security_keymint::aidl::android::hardware::security::keymint::{
    Algorithm::Algorithm, Digest::Digest, EcCurve::EcCurve, KeyPurpose::KeyPurpose,
    PaddingMode::PaddingMode,
};
use android_system_keystore2::aidl::android::system::keystore2::{
    Domain::Domain, IKeystoreService::IKeystoreService, KeyDescriptor::KeyDescriptor,
    KeyPermission::KeyPermission,
};
use keystore2_test_utils::ffi_test_utils::perform_crypto_op_using_keystore_engine;
use keystore2_test_utils::{
    authorizations::AuthSetBuilder, get_keystore_service, run_as, SecLevel,
};
use openssl::x509::X509;
use rustutils::users::AID_USER_OFFSET;

fn generate_rsa_key_and_grant_to_user(
    sl: &SecLevel,
    alias: &str,
    grantee_uid: i32,
    access_vector: i32,
) -> binder::Result<KeyDescriptor> {
    let gen_params = AuthSetBuilder::new()
        .no_auth_required()
        .algorithm(Algorithm::RSA)
        .rsa_public_exponent(65537)
        .key_size(2048)
        .purpose(KeyPurpose::SIGN)
        .purpose(KeyPurpose::VERIFY)
        .padding_mode(PaddingMode::NONE)
        .digest(Digest::NONE);

    let key_metadata = sl
        .binder
        .generateKey(
            &KeyDescriptor {
                domain: Domain::APP,
                nspace: -1,
                alias: Some(alias.to_string()),
                blob: None,
            },
            None,
            &gen_params,
            0,
            b"entropy",
        )
        .expect("Failed to generate RSA Key.");

    assert!(key_metadata.certificate.is_some());

    sl.keystore2.grant(&key_metadata.key, grantee_uid, access_vector)
}

fn generate_ec_key_and_grant_to_user(
    sl: &SecLevel,
    alias: &str,
    grantee_uid: i32,
    access_vector: i32,
) -> binder::Result<KeyDescriptor> {
    let gen_params = AuthSetBuilder::new()
        .no_auth_required()
        .algorithm(Algorithm::EC)
        .purpose(KeyPurpose::SIGN)
        .purpose(KeyPurpose::VERIFY)
        .digest(Digest::NONE)
        .ec_curve(EcCurve::P_256);

    let key_metadata = sl
        .binder
        .generateKey(
            &KeyDescriptor {
                domain: Domain::APP,
                nspace: -1,
                alias: Some(alias.to_string()),
                blob: None,
            },
            None,
            &gen_params,
            0,
            b"entropy",
        )
        .expect("Failed to generate EC Key.");

    assert!(key_metadata.certificate.is_some());

    sl.keystore2.grant(&key_metadata.key, grantee_uid, access_vector)
}

fn generate_key_and_grant_to_user(
    sl: &SecLevel,
    alias: &str,
    grantee_uid: u32,
    algo: Algorithm,
) -> Result<i64, Box<dyn std::error::Error>> {
    let access_vector = KeyPermission::GET_INFO.0 | KeyPermission::USE.0 | KeyPermission::DELETE.0;

    assert!(matches!(algo, Algorithm::RSA | Algorithm::EC));

    let grant_key = match algo {
        Algorithm::RSA => generate_rsa_key_and_grant_to_user(
            sl,
            alias,
            grantee_uid.try_into().unwrap(),
            access_vector,
        )
        .unwrap(),
        Algorithm::EC => generate_ec_key_and_grant_to_user(
            sl,
            alias,
            grantee_uid.try_into().unwrap(),
            access_vector,
        )
        .unwrap(),
        _ => panic!("Unsupported algorithms"),
    };

    assert_eq!(grant_key.domain, Domain::GRANT);

    Ok(grant_key.nspace)
}

fn perform_crypto_op_using_granted_key(
    keystore2: &binder::Strong<dyn IKeystoreService>,
    grant_key_nspace: i64,
) {
    // Load the granted key from Keystore2-Engine API and perform crypto operations.
    assert!(perform_crypto_op_using_keystore_engine(grant_key_nspace).unwrap());

    // Delete the granted key.
    keystore2
        .deleteKey(&KeyDescriptor {
            domain: Domain::GRANT,
            nspace: grant_key_nspace,
            alias: None,
            blob: None,
        })
        .unwrap();
}

#[test]
fn keystore2_perform_crypto_op_using_keystore2_engine_rsa_key_success() {
    const USER_ID: u32 = 99;
    const APPLICATION_ID: u32 = 10001;
    static GRANTEE_UID: u32 = USER_ID * AID_USER_OFFSET + APPLICATION_ID;
    static GRANTEE_GID: u32 = GRANTEE_UID;

    // Generate a key and grant it to a user with GET_INFO|USE|DELETE key permissions.
    let grantor_fn = || {
        let sl = SecLevel::tee();
        let alias = "keystore2_engine_rsa_key";
        generate_key_and_grant_to_user(&sl, alias, GRANTEE_UID, Algorithm::RSA).unwrap()
    };

    // Safety: only one thread at this point (enforced by `AndroidTest.xml` setting
    // `--test-threads=1`), and nothing yet done with binder.
    let grant_key_nspace = unsafe { run_as::run_as_root(grantor_fn) };

    // In grantee context load the key and try to perform crypto operation.
    let grantee_fn = move || {
        let keystore2 = get_keystore_service();
        perform_crypto_op_using_granted_key(&keystore2, grant_key_nspace);
    };

    // Safety: only one thread at this point (enforced by `AndroidTest.xml` setting
    // `--test-threads=1`), and nothing yet done with binder.
    unsafe { run_as::run_as_app(GRANTEE_UID, GRANTEE_GID, grantee_fn) };
}

#[test]
fn keystore2_perform_crypto_op_using_keystore2_engine_ec_key_success() {
    const USER_ID: u32 = 99;
    const APPLICATION_ID: u32 = 10001;
    static GRANTEE_UID: u32 = USER_ID * AID_USER_OFFSET + APPLICATION_ID;
    static GRANTEE_GID: u32 = GRANTEE_UID;

    // Generate a key and grant it to a user with GET_INFO|USE|DELETE key permissions.
    let grantor_fn = || {
        let sl = SecLevel::tee();
        let alias = "keystore2_engine_ec_test_key";
        generate_key_and_grant_to_user(&sl, alias, GRANTEE_UID, Algorithm::EC).unwrap()
    };

    // Safety: only one thread at this point (enforced by `AndroidTest.xml` setting
    // `--test-threads=1`), and nothing yet done with binder.
    let grant_key_nspace = unsafe { run_as::run_as_root(grantor_fn) };

    // In grantee context load the key and try to perform crypto operation.
    let grantee_fn = move || {
        let keystore2 = get_keystore_service();
        perform_crypto_op_using_granted_key(&keystore2, grant_key_nspace);
    };

    // Safety: only one thread at this point (enforced by `AndroidTest.xml` setting
    // `--test-threads=1`), and nothing yet done with binder.
    unsafe { run_as::run_as_app(GRANTEE_UID, GRANTEE_GID, grantee_fn) };
}

#[test]
fn keystore2_perform_crypto_op_using_keystore2_engine_pem_pub_key_success() {
    const USER_ID: u32 = 99;
    const APPLICATION_ID: u32 = 10001;
    static GRANTEE_UID: u32 = USER_ID * AID_USER_OFFSET + APPLICATION_ID;
    static GRANTEE_GID: u32 = GRANTEE_UID;

    // Generate a key and re-encode it's certificate as PEM and update it and
    // grant it to a user with GET_INFO|USE|DELETE key permissions.
    let grantor_fn = || {
        let sl = SecLevel::tee();
        let alias = "keystore2_engine_rsa_pem_pub_key";
        let grant_key_nspace =
            generate_key_and_grant_to_user(&sl, alias, GRANTEE_UID, Algorithm::RSA).unwrap();

        // Update certificate with encodeed PEM data.
        let key_entry_response = sl
            .keystore2
            .getKeyEntry(&KeyDescriptor {
                domain: Domain::APP,
                nspace: -1,
                alias: Some(alias.to_string()),
                blob: None,
            })
            .unwrap();
        let cert_bytes = key_entry_response.metadata.certificate.as_ref().unwrap();
        let cert = X509::from_der(cert_bytes.as_ref()).unwrap();
        let cert_pem = cert.to_pem().unwrap();
        sl.keystore2
            .updateSubcomponent(&key_entry_response.metadata.key, Some(&cert_pem), None)
            .expect("updateSubcomponent failed.");

        grant_key_nspace
    };

    // Safety: only one thread at this point (enforced by `AndroidTest.xml` setting
    // `--test-threads=1`), and nothing yet done with binder.
    let grant_key_nspace = unsafe { run_as::run_as_root(grantor_fn) };

    // In grantee context load the key and try to perform crypto operation.
    let grantee_fn = move || {
        let keystore2 = get_keystore_service();
        perform_crypto_op_using_granted_key(&keystore2, grant_key_nspace);
    };

    // Safety: only one thread at this point (enforced by `AndroidTest.xml` setting
    // `--test-threads=1`), and nothing yet done with binder.
    unsafe { run_as::run_as_app(GRANTEE_UID, GRANTEE_GID, grantee_fn) };
}
