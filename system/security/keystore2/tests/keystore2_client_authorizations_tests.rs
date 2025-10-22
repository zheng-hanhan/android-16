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

use crate::keystore2_client_test_utils::{
    app_attest_key_feature_exists, delete_app_key, get_vsr_api_level,
    perform_sample_asym_sign_verify_op, perform_sample_hmac_sign_verify_op,
    perform_sample_sym_key_decrypt_op, perform_sample_sym_key_encrypt_op,
    verify_certificate_serial_num, verify_certificate_subject_name, SAMPLE_PLAIN_TEXT,
};
use crate::{require_keymint, skip_test_if_no_app_attest_key_feature};
use android_hardware_security_keymint::aidl::android::hardware::security::keymint::{
    Algorithm::Algorithm, BlockMode::BlockMode, Digest::Digest, EcCurve::EcCurve,
    ErrorCode::ErrorCode, KeyPurpose::KeyPurpose, PaddingMode::PaddingMode,
    SecurityLevel::SecurityLevel, Tag::Tag,
};
use android_hardware_security_keymint::aidl::android::hardware::security::keymint::{
    HardwareAuthToken::HardwareAuthToken, HardwareAuthenticatorType::HardwareAuthenticatorType,
    KeyParameter::KeyParameter, KeyParameterValue::KeyParameterValue,
};
use android_hardware_security_secureclock::aidl::android::hardware::security::secureclock::{
    Timestamp::Timestamp
};
use android_system_keystore2::aidl::android::system::keystore2::{
    Domain::Domain, KeyDescriptor::KeyDescriptor, KeyMetadata::KeyMetadata,
    ResponseCode::ResponseCode,
};
use bssl_crypto::digest;
use keystore_attestation::{AttestationExtension, ATTESTATION_EXTENSION_OID};
use keystore2_test_utils::ffi_test_utils::get_value_from_attest_record;
use keystore2_test_utils::{
    authorizations, get_keystore_auth_service, key_generations,
    key_generations::Error, SecLevel,
};
use openssl::bn::{BigNum, MsbOption};
use openssl::x509::X509NameBuilder;
use std::time::SystemTime;
use x509_cert::{certificate::Certificate, der::Decode};

fn gen_key_including_unique_id(sl: &SecLevel, alias: &str) -> Option<Vec<u8>> {
    let gen_params = authorizations::AuthSetBuilder::new()
        .no_auth_required()
        .algorithm(Algorithm::EC)
        .purpose(KeyPurpose::SIGN)
        .purpose(KeyPurpose::VERIFY)
        .digest(Digest::SHA_2_256)
        .ec_curve(EcCurve::P_256)
        .attestation_challenge(b"foo".to_vec())
        .include_unique_id();

    let key_metadata =
        key_generations::map_ks_error(key_generations::generate_key(sl, &gen_params, alias))
            .unwrap()?;

    let unique_id = get_value_from_attest_record(
        key_metadata.certificate.as_ref().unwrap(),
        Tag::UNIQUE_ID,
        key_metadata.keySecurityLevel,
    )
    .expect("Unique id not found.");
    assert!(!unique_id.is_empty());
    Some(unique_id)
}

fn generate_key_and_perform_sign_verify_op_max_times(
    sl: &SecLevel,
    gen_params: &authorizations::AuthSetBuilder,
    alias: &str,
    max_usage_count: i32,
) -> binder::Result<Option<KeyMetadata>> {
    let Some(key_metadata) = key_generations::generate_key(sl, gen_params, alias)? else {
        return Ok(None);
    };

    // Use above generated key `max_usage_count` times.
    for _ in 0..max_usage_count {
        perform_sample_asym_sign_verify_op(
            &sl.binder,
            &key_metadata,
            None,
            Some(Digest::SHA_2_256),
        );
    }

    Ok(Some(key_metadata))
}

/// Generate a key with `USAGE_COUNT_LIMIT` and verify the key characteristics. Test should be able
/// to use the key successfully `max_usage_count` times. After exceeding key usage `max_usage_count`
/// times subsequent attempts to use the key in test should fail with response code `KEY_NOT_FOUND`.
/// Test should also verify that the attest record includes `USAGE_COUNT_LIMIT` for attested keys.
fn generate_key_and_perform_op_with_max_usage_limit(
    sl: &SecLevel,
    gen_params: &authorizations::AuthSetBuilder,
    alias: &str,
    max_usage_count: i32,
    check_attestation: bool,
) {
    // Generate a key and use the key for `max_usage_count` times.
    let Some(key_metadata) =
        generate_key_and_perform_sign_verify_op_max_times(sl, gen_params, alias, max_usage_count)
            .unwrap()
    else {
        return;
    };

    let auth = key_generations::get_key_auth(&key_metadata.authorizations, Tag::USAGE_COUNT_LIMIT)
        .unwrap();
    if check_attestation && sl.is_keymint() {
        // Check usage-count-limit is included in attest-record.
        // `USAGE_COUNT_LIMIT` is supported from KeyMint1.0
        assert_ne!(
            gen_params.iter().filter(|kp| kp.tag == Tag::ATTESTATION_CHALLENGE).count(),
            0,
            "Attestation challenge is missing in generated key parameters."
        );
        let result = get_value_from_attest_record(
            key_metadata.certificate.as_ref().unwrap(),
            Tag::USAGE_COUNT_LIMIT,
            auth.securityLevel,
        )
        .expect("Attest id verification failed.");
        let usage_count: i32 = std::str::from_utf8(&result).unwrap().parse().unwrap();
        assert_eq!(usage_count, max_usage_count);
    }
    if max_usage_count == 1 {
        assert!(matches!(
            auth.securityLevel,
            SecurityLevel::KEYSTORE | SecurityLevel::TRUSTED_ENVIRONMENT
        ));
    } else {
        assert_eq!(auth.securityLevel, SecurityLevel::KEYSTORE);
    }

    // Try to use the key one more time.
    let result = key_generations::map_ks_error(sl.binder.createOperation(
        &key_metadata.key,
        &authorizations::AuthSetBuilder::new().purpose(KeyPurpose::SIGN).digest(Digest::SHA_2_256),
        false,
    ));
    assert!(result.is_err());
    assert_eq!(Error::Rc(ResponseCode::KEY_NOT_FOUND), result.unwrap_err());
}

/// Generate a key with `ACTIVE_DATETIME` set to current time. Test should successfully generate
/// a key and verify the key characteristics. Test should be able to create a sign operation using
/// the generated key successfully.
#[test]
fn keystore2_gen_key_auth_active_datetime_test_success() {
    let sl = SecLevel::tee();

    let duration_since_epoch = SystemTime::now().duration_since(SystemTime::UNIX_EPOCH).unwrap();
    let active_datetime = duration_since_epoch.as_millis();
    let gen_params = authorizations::AuthSetBuilder::new()
        .no_auth_required()
        .algorithm(Algorithm::EC)
        .purpose(KeyPurpose::SIGN)
        .purpose(KeyPurpose::VERIFY)
        .digest(Digest::SHA_2_256)
        .ec_curve(EcCurve::P_256)
        .active_date_time(active_datetime.try_into().unwrap());

    let alias = "ks_test_auth_tags_test";
    let result = key_generations::create_key_and_operation(
        &sl,
        &gen_params,
        &authorizations::AuthSetBuilder::new().purpose(KeyPurpose::SIGN).digest(Digest::SHA_2_256),
        alias,
    );
    assert!(result.is_ok());
    delete_app_key(&sl.keystore2, alias).unwrap();
}

/// Generate a key with `ACTIVE_DATETIME` set to future date and time. Test should successfully
/// generate a key and verify the key characteristics. Try to create a sign operation
/// using the generated key, test should fail to create an operation with error code
/// `KEY_NOT_YET_VALID`.
#[test]
fn keystore2_gen_key_auth_future_active_datetime_test_op_fail() {
    let sl = SecLevel::tee();

    let duration_since_epoch = SystemTime::now().duration_since(SystemTime::UNIX_EPOCH).unwrap();
    let future_active_datetime = duration_since_epoch.as_millis() + (24 * 60 * 60 * 1000);
    let gen_params = authorizations::AuthSetBuilder::new()
        .no_auth_required()
        .algorithm(Algorithm::EC)
        .purpose(KeyPurpose::SIGN)
        .purpose(KeyPurpose::VERIFY)
        .digest(Digest::SHA_2_256)
        .ec_curve(EcCurve::P_256)
        .active_date_time(future_active_datetime.try_into().unwrap());

    let alias = "ks_test_auth_tags_test";
    let result = key_generations::map_ks_error(key_generations::create_key_and_operation(
        &sl,
        &gen_params,
        &authorizations::AuthSetBuilder::new().purpose(KeyPurpose::SIGN).digest(Digest::SHA_2_256),
        alias,
    ));
    assert!(result.is_err());
    assert_eq!(Error::Km(ErrorCode::KEY_NOT_YET_VALID), result.unwrap_err());
    delete_app_key(&sl.keystore2, alias).unwrap();
}

/// Generate a key with `ORIGINATION_EXPIRE_DATETIME` set to future date and time. Test should
/// successfully generate a key and verify the key characteristics. Test should be able to create
/// sign operation using the generated key successfully.
#[test]
fn keystore2_gen_key_auth_future_origination_expire_datetime_test_success() {
    let sl = SecLevel::tee();

    let duration_since_epoch = SystemTime::now().duration_since(SystemTime::UNIX_EPOCH).unwrap();
    let origination_expire_datetime = duration_since_epoch.as_millis() + (24 * 60 * 60 * 1000);
    let gen_params = authorizations::AuthSetBuilder::new()
        .no_auth_required()
        .algorithm(Algorithm::EC)
        .purpose(KeyPurpose::SIGN)
        .purpose(KeyPurpose::VERIFY)
        .digest(Digest::SHA_2_256)
        .ec_curve(EcCurve::P_256)
        .origination_expire_date_time(origination_expire_datetime.try_into().unwrap());

    let alias = "ks_test_auth_tags_test";
    let result = key_generations::create_key_and_operation(
        &sl,
        &gen_params,
        &authorizations::AuthSetBuilder::new().purpose(KeyPurpose::SIGN).digest(Digest::SHA_2_256),
        alias,
    );
    assert!(result.is_ok());
    delete_app_key(&sl.keystore2, alias).unwrap();
}

/// Generate a key with `ORIGINATION_EXPIRE_DATETIME` set to current date and time. Test should
/// successfully generate a key and verify the key characteristics. Try to create a sign operation
/// using the generated key, test should fail to create an operation with error code
/// `KEY_EXPIRED`.
#[test]
fn keystore2_gen_key_auth_origination_expire_datetime_test_op_fail() {
    let sl = SecLevel::tee();

    let duration_since_epoch = SystemTime::now().duration_since(SystemTime::UNIX_EPOCH).unwrap();
    let origination_expire_datetime = duration_since_epoch.as_millis();
    let gen_params = authorizations::AuthSetBuilder::new()
        .no_auth_required()
        .algorithm(Algorithm::EC)
        .purpose(KeyPurpose::SIGN)
        .purpose(KeyPurpose::VERIFY)
        .digest(Digest::SHA_2_256)
        .ec_curve(EcCurve::P_256)
        .origination_expire_date_time(origination_expire_datetime.try_into().unwrap());

    let alias = "ks_test_auth_tags_test";
    let result = key_generations::map_ks_error(key_generations::create_key_and_operation(
        &sl,
        &gen_params,
        &authorizations::AuthSetBuilder::new().purpose(KeyPurpose::SIGN).digest(Digest::SHA_2_256),
        alias,
    ));
    assert!(result.is_err());
    assert_eq!(Error::Km(ErrorCode::KEY_EXPIRED), result.unwrap_err());
    delete_app_key(&sl.keystore2, alias).unwrap();
}

/// Generate a HMAC key with `USAGE_EXPIRE_DATETIME` set to future date and time. Test should
/// successfully generate a key and verify the key characteristics. Test should be able to create
/// sign and verify operations using the generated key successfully.
#[test]
fn keystore2_gen_key_auth_future_usage_expire_datetime_hmac_verify_op_success() {
    let sl = SecLevel::tee();

    let duration_since_epoch = SystemTime::now().duration_since(SystemTime::UNIX_EPOCH).unwrap();
    let usage_expire_datetime = duration_since_epoch.as_millis() + (24 * 60 * 60 * 1000);
    let gen_params = authorizations::AuthSetBuilder::new()
        .no_auth_required()
        .algorithm(Algorithm::HMAC)
        .purpose(KeyPurpose::SIGN)
        .purpose(KeyPurpose::VERIFY)
        .key_size(128)
        .min_mac_length(256)
        .digest(Digest::SHA_2_256)
        .usage_expire_date_time(usage_expire_datetime.try_into().unwrap());

    let alias = "ks_test_auth_tags_hmac_verify_success";
    let Some(key_metadata) = key_generations::generate_key(&sl, &gen_params, alias).unwrap() else {
        return;
    };

    perform_sample_hmac_sign_verify_op(&sl.binder, &key_metadata.key);
    delete_app_key(&sl.keystore2, alias).unwrap();
}

/// Generate a key with `USAGE_EXPIRE_DATETIME` set to current date and time. Test should
/// successfully generate a key and verify the key characteristics. Test should be able to create
/// sign operation successfully and fail while performing verify operation with error code
/// `KEY_EXPIRED`.
#[test]
fn keystore2_gen_key_auth_usage_expire_datetime_hmac_verify_op_fail() {
    let sl = SecLevel::tee();

    let duration_since_epoch = SystemTime::now().duration_since(SystemTime::UNIX_EPOCH).unwrap();
    let usage_expire_datetime = duration_since_epoch.as_millis();
    let gen_params = authorizations::AuthSetBuilder::new()
        .no_auth_required()
        .algorithm(Algorithm::HMAC)
        .purpose(KeyPurpose::SIGN)
        .purpose(KeyPurpose::VERIFY)
        .key_size(128)
        .min_mac_length(256)
        .digest(Digest::SHA_2_256)
        .usage_expire_date_time(usage_expire_datetime.try_into().unwrap());

    let alias = "ks_test_auth_tags_hamc_verify_fail";
    let Some(key_metadata) = key_generations::generate_key(&sl, &gen_params, alias).unwrap() else {
        return;
    };

    let result = key_generations::map_ks_error(
        sl.binder.createOperation(
            &key_metadata.key,
            &authorizations::AuthSetBuilder::new()
                .purpose(KeyPurpose::VERIFY)
                .digest(Digest::SHA_2_256),
            false,
        ),
    );
    assert!(result.is_err());
    assert_eq!(Error::Km(ErrorCode::KEY_EXPIRED), result.unwrap_err());
    delete_app_key(&sl.keystore2, alias).unwrap();
}

/// Generate AES key with `USAGE_EXPIRE_DATETIME` set to future date and time. Test should
/// successfully generate a key and verify the key characteristics. Test should be able to create
/// Encrypt and Decrypt operations successfully.
#[test]
fn keystore2_gen_key_auth_usage_future_expire_datetime_decrypt_op_success() {
    let sl = SecLevel::tee();

    let duration_since_epoch = SystemTime::now().duration_since(SystemTime::UNIX_EPOCH).unwrap();
    let usage_expire_datetime = duration_since_epoch.as_millis() + (24 * 60 * 60 * 1000);
    let gen_params = authorizations::AuthSetBuilder::new()
        .no_auth_required()
        .algorithm(Algorithm::AES)
        .purpose(KeyPurpose::ENCRYPT)
        .purpose(KeyPurpose::DECRYPT)
        .key_size(128)
        .padding_mode(PaddingMode::PKCS7)
        .block_mode(BlockMode::ECB)
        .usage_expire_date_time(usage_expire_datetime.try_into().unwrap());

    let alias = "ks_test_auth_tags_test";
    let Some(key_metadata) = key_generations::generate_key(&sl, &gen_params, alias).unwrap() else {
        return;
    };
    let cipher_text = perform_sample_sym_key_encrypt_op(
        &sl.binder,
        PaddingMode::PKCS7,
        BlockMode::ECB,
        &mut None,
        None,
        &key_metadata.key,
    )
    .unwrap();

    assert!(cipher_text.is_some());

    let plain_text = perform_sample_sym_key_decrypt_op(
        &sl.binder,
        &cipher_text.unwrap(),
        PaddingMode::PKCS7,
        BlockMode::ECB,
        &mut None,
        None,
        &key_metadata.key,
    )
    .unwrap();
    assert!(plain_text.is_some());
    assert_eq!(plain_text.unwrap(), SAMPLE_PLAIN_TEXT.to_vec());
    delete_app_key(&sl.keystore2, alias).unwrap();
}

/// Generate AES key with `USAGE_EXPIRE_DATETIME` set to current date and time. Test should
/// successfully generate a key and verify the key characteristics. Test should be able to create
/// Encrypt operation successfully and fail while performing decrypt operation with error code
/// `KEY_EXPIRED`.
#[test]
fn keystore2_gen_key_auth_usage_expire_datetime_decrypt_op_fail() {
    let sl = SecLevel::tee();

    let duration_since_epoch = SystemTime::now().duration_since(SystemTime::UNIX_EPOCH).unwrap();
    let usage_expire_datetime = duration_since_epoch.as_millis();
    let gen_params = authorizations::AuthSetBuilder::new()
        .no_auth_required()
        .algorithm(Algorithm::AES)
        .purpose(KeyPurpose::ENCRYPT)
        .purpose(KeyPurpose::DECRYPT)
        .key_size(128)
        .padding_mode(PaddingMode::PKCS7)
        .block_mode(BlockMode::ECB)
        .usage_expire_date_time(usage_expire_datetime.try_into().unwrap());

    let alias = "ks_test_auth_tags_test";
    let Some(key_metadata) = key_generations::generate_key(&sl, &gen_params, alias).unwrap() else {
        return;
    };
    let cipher_text = perform_sample_sym_key_encrypt_op(
        &sl.binder,
        PaddingMode::PKCS7,
        BlockMode::ECB,
        &mut None,
        None,
        &key_metadata.key,
    )
    .unwrap();

    assert!(cipher_text.is_some());

    let result = key_generations::map_ks_error(perform_sample_sym_key_decrypt_op(
        &sl.binder,
        &cipher_text.unwrap(),
        PaddingMode::PKCS7,
        BlockMode::ECB,
        &mut None,
        None,
        &key_metadata.key,
    ));
    assert!(result.is_err());
    assert_eq!(Error::Km(ErrorCode::KEY_EXPIRED), result.unwrap_err());
    delete_app_key(&sl.keystore2, alias).unwrap();
}

/// Generate a key with `EARLY_BOOT_ONLY`. Test should successfully generate
/// a key and verify the key characteristics. Test should fail with error code `EARLY_BOOT_ENDED`
/// during creation of an operation using this key.
#[test]
fn keystore2_gen_key_auth_early_boot_only_op_fail() {
    let sl = SecLevel::tee();
    require_keymint!(sl);

    let gen_params = authorizations::AuthSetBuilder::new()
        .no_auth_required()
        .algorithm(Algorithm::EC)
        .purpose(KeyPurpose::SIGN)
        .purpose(KeyPurpose::VERIFY)
        .digest(Digest::SHA_2_256)
        .ec_curve(EcCurve::P_256)
        .early_boot_only();

    let alias = "ks_test_auth_tags_test";
    let result = key_generations::map_ks_error(key_generations::create_key_and_operation(
        &sl,
        &gen_params,
        &authorizations::AuthSetBuilder::new().purpose(KeyPurpose::SIGN).digest(Digest::SHA_2_256),
        alias,
    ));
    assert!(result.is_err());
    assert_eq!(Error::Km(ErrorCode::EARLY_BOOT_ENDED), result.unwrap_err());
    delete_app_key(&sl.keystore2, alias).unwrap();
}

/// Generate a key with `MAX_USES_PER_BOOT`. Test should successfully generate
/// a key and verify the key characteristics. Test should be able to use the key successfully
/// `MAX_USES_COUNT` times. After exceeding key usage `MAX_USES_COUNT` times
/// subsequent attempts to use the key in test should fail with error code MAX_OPS_EXCEEDED.
#[test]
fn keystore2_gen_key_auth_max_uses_per_boot() {
    let sl = SecLevel::tee();
    if sl.is_keymaster() {
        // Older devices with Keymaster implementation may use the key during generateKey to export
        // the generated public key (EC Key), leading to an unnecessary increment of the
        // key-associated counter. This can cause the test to fail, so skipping this test on older
        // devices to avoid test failure.
        return;
    }
    const MAX_USES_COUNT: i32 = 3;

    let gen_params = authorizations::AuthSetBuilder::new()
        .no_auth_required()
        .algorithm(Algorithm::EC)
        .purpose(KeyPurpose::SIGN)
        .purpose(KeyPurpose::VERIFY)
        .digest(Digest::SHA_2_256)
        .ec_curve(EcCurve::P_256)
        .max_uses_per_boot(MAX_USES_COUNT);

    let alias = "ks_test_auth_tags_test";
    // Generate a key and use the key for `MAX_USES_COUNT` times.
    let Some(key_metadata) =
        generate_key_and_perform_sign_verify_op_max_times(&sl, &gen_params, alias, MAX_USES_COUNT)
            .unwrap()
    else {
        return;
    };

    // Try to use the key one more time.
    let result = key_generations::map_ks_error(sl.binder.createOperation(
        &key_metadata.key,
        &authorizations::AuthSetBuilder::new().purpose(KeyPurpose::SIGN).digest(Digest::SHA_2_256),
        false,
    ));
    assert!(result.is_err());
    assert_eq!(Error::Km(ErrorCode::KEY_MAX_OPS_EXCEEDED), result.unwrap_err());
    delete_app_key(&sl.keystore2, alias).unwrap();
}

/// Generate a key with `USAGE_COUNT_LIMIT`. Test should successfully generate
/// a key and verify the key characteristics. Test should be able to use the key successfully
/// `MAX_USES_COUNT` times. After exceeding key usage `MAX_USES_COUNT` times
/// subsequent attempts to use the key in test should fail with response code `KEY_NOT_FOUND`.
/// Test should also verify that the attest record includes `USAGE_COUNT_LIMIT`.
#[test]
fn keystore2_gen_key_auth_usage_count_limit() {
    let sl = SecLevel::tee();
    if sl.is_keymaster() {
        // `USAGE_COUNT_LIMIT` is supported from KeyMint1.0
        return;
    }
    const MAX_USES_COUNT: i32 = 3;

    let gen_params = authorizations::AuthSetBuilder::new()
        .no_auth_required()
        .algorithm(Algorithm::EC)
        .purpose(KeyPurpose::SIGN)
        .purpose(KeyPurpose::VERIFY)
        .digest(Digest::SHA_2_256)
        .ec_curve(EcCurve::P_256)
        .attestation_challenge(b"foo".to_vec())
        .usage_count_limit(MAX_USES_COUNT);

    let alias = "ks_test_auth_tags_test";
    generate_key_and_perform_op_with_max_usage_limit(&sl, &gen_params, alias, MAX_USES_COUNT, true);
}

/// Generate a key with `USAGE_COUNT_LIMIT`. Test should successfully generate
/// a key and verify the key characteristics. Test should be able to use the key successfully
/// `MAX_USES_COUNT` times. After exceeding key usage `MAX_USES_COUNT` times
/// subsequent attempts to use the key in test should fail with response code `KEY_NOT_FOUND`.
/// Test should also verify that the attest record includes `USAGE_COUNT_LIMIT`.
#[test]
fn keystore2_gen_key_auth_usage_count_limit_one() {
    let sl = SecLevel::tee();
    if sl.is_keymaster() {
        // `USAGE_COUNT_LIMIT` is supported from KeyMint1.0
        return;
    }
    const MAX_USES_COUNT: i32 = 1;

    let gen_params = authorizations::AuthSetBuilder::new()
        .no_auth_required()
        .algorithm(Algorithm::EC)
        .purpose(KeyPurpose::SIGN)
        .purpose(KeyPurpose::VERIFY)
        .digest(Digest::SHA_2_256)
        .ec_curve(EcCurve::P_256)
        .attestation_challenge(b"foo".to_vec())
        .usage_count_limit(MAX_USES_COUNT);

    let alias = "ks_test_auth_tags_test";
    generate_key_and_perform_op_with_max_usage_limit(&sl, &gen_params, alias, MAX_USES_COUNT, true);
}

/// Generate a non-attested key with `USAGE_COUNT_LIMIT`. Test should successfully generate
/// a key and verify the key characteristics. Test should be able to use the key successfully
/// `MAX_USES_COUNT` times. After exceeding key usage `MAX_USES_COUNT` times
/// subsequent attempts to use the key in test should fail with response code `KEY_NOT_FOUND`.
#[test]
fn keystore2_gen_non_attested_key_auth_usage_count_limit() {
    let sl = SecLevel::tee();
    if sl.is_keymaster() {
        // `USAGE_COUNT_LIMIT` is supported from KeyMint1.0
        return;
    }
    const MAX_USES_COUNT: i32 = 2;

    let gen_params = authorizations::AuthSetBuilder::new()
        .no_auth_required()
        .algorithm(Algorithm::EC)
        .purpose(KeyPurpose::SIGN)
        .purpose(KeyPurpose::VERIFY)
        .digest(Digest::SHA_2_256)
        .ec_curve(EcCurve::P_256)
        .usage_count_limit(MAX_USES_COUNT);

    let alias = "ks_test_auth_tags_test";
    generate_key_and_perform_op_with_max_usage_limit(
        &sl,
        &gen_params,
        alias,
        MAX_USES_COUNT,
        false,
    );
}

/// Try to generate a key with `Tag::CREATION_DATETIME` set to valid value. Test should fail
/// to generate a key with `INVALID_ARGUMENT` error as Keystore2 backend doesn't allow user to
/// specify `CREATION_DATETIME`.
#[test]
fn keystore2_gen_key_auth_creation_date_time_test_fail_with_invalid_arg_error() {
    let sl = SecLevel::tee();

    let duration_since_epoch = SystemTime::now().duration_since(SystemTime::UNIX_EPOCH).unwrap();
    let creation_datetime = duration_since_epoch.as_millis();
    let gen_params = authorizations::AuthSetBuilder::new()
        .no_auth_required()
        .algorithm(Algorithm::EC)
        .purpose(KeyPurpose::SIGN)
        .purpose(KeyPurpose::VERIFY)
        .digest(Digest::SHA_2_256)
        .ec_curve(EcCurve::P_256)
        .creation_date_time(creation_datetime.try_into().unwrap());

    let alias = "ks_test_auth_tags_test";
    let result = key_generations::map_ks_error(sl.binder.generateKey(
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
    ));

    assert!(result.is_err());
    assert_eq!(Error::Rc(ResponseCode::INVALID_ARGUMENT), result.unwrap_err());
}

/// Generate a key with `Tag::INCLUDE_UNIQUE_ID` set. Test should verify that `Tag::UNIQUE_ID` is
/// included in attest record and it remains the same for new keys generated.
#[test]
fn keystore2_gen_key_auth_include_unique_id_success() {
    let sl = SecLevel::tee();
    if sl.is_keymaster() {
        // b/387208956 - Some older devices with Keymaster implementations fail to generate an
        // attestation key with `INCLUDE_UNIQUE_ID`, but this was not previously tested. Skip this
        // test on devices with Keymaster implementation.
        return;
    }

    let alias_first = "ks_test_auth_tags_test_1";
    if let Some(unique_id_first) = gen_key_including_unique_id(&sl, alias_first) {
        let alias_second = "ks_test_auth_tags_test_2";
        let unique_id_second = gen_key_including_unique_id(&sl, alias_second).unwrap();

        assert_eq!(unique_id_first, unique_id_second);

        delete_app_key(&sl.keystore2, alias_first).unwrap();
        delete_app_key(&sl.keystore2, alias_second).unwrap();
    }
}

/// Generate a key with `APPLICATION_DATA` and `APPLICATION_ID`. Test should create an operation
/// successfully using the same `APPLICATION_DATA` and `APPLICATION_ID`.
#[test]
fn keystore2_gen_key_auth_app_data_app_id_test_success() {
    let sl = SecLevel::tee();
    if sl.is_keymaster() && get_vsr_api_level() < 35 {
        // `APPLICATION_DATA` key-parameter is causing the error on older devices, so skipping this
        // test to run on older devices.
        return;
    }

    let gen_params = authorizations::AuthSetBuilder::new()
        .no_auth_required()
        .algorithm(Algorithm::EC)
        .purpose(KeyPurpose::SIGN)
        .purpose(KeyPurpose::VERIFY)
        .digest(Digest::NONE)
        .ec_curve(EcCurve::P_256)
        .app_data(b"app-data".to_vec())
        .app_id(b"app-id".to_vec());

    let alias = "ks_test_auth_tags_test";
    let result = key_generations::create_key_and_operation(
        &sl,
        &gen_params,
        &authorizations::AuthSetBuilder::new()
            .purpose(KeyPurpose::SIGN)
            .digest(Digest::NONE)
            .app_data(b"app-data".to_vec())
            .app_id(b"app-id".to_vec()),
        alias,
    );
    assert!(result.is_ok());
    delete_app_key(&sl.keystore2, alias).unwrap();
}

/// Generate a key with `APPLICATION_DATA` and `APPLICATION_ID`. Try to create an operation using
/// the different `APPLICATION_DATA` and `APPLICATION_ID`, test should fail to create an operation.
#[test]
fn keystore2_op_auth_invalid_app_data_app_id_test_fail() {
    let sl = SecLevel::tee();
    if sl.is_keymaster() && get_vsr_api_level() < 35 {
        // `APPLICATION_DATA` key-parameter is causing the error on older devices, so skipping this
        // test to run on older devices.
        return;
    }

    let gen_params = authorizations::AuthSetBuilder::new()
        .no_auth_required()
        .algorithm(Algorithm::EC)
        .purpose(KeyPurpose::SIGN)
        .purpose(KeyPurpose::VERIFY)
        .digest(Digest::NONE)
        .ec_curve(EcCurve::P_256)
        .app_data(b"app-data".to_vec())
        .app_id(b"app-id".to_vec());

    let alias = "ks_test_auth_tags_test";
    let result = key_generations::map_ks_error(key_generations::create_key_and_operation(
        &sl,
        &gen_params,
        &authorizations::AuthSetBuilder::new()
            .purpose(KeyPurpose::SIGN)
            .digest(Digest::NONE)
            .app_data(b"invalid-app-data".to_vec())
            .app_id(b"invalid-app-id".to_vec()),
        alias,
    ));
    assert!(result.is_err());
    assert_eq!(Error::Km(ErrorCode::INVALID_KEY_BLOB), result.unwrap_err());
    delete_app_key(&sl.keystore2, alias).unwrap();
}

/// Generate a key with `APPLICATION_DATA` and `APPLICATION_ID`. Try to create an operation using
/// only `APPLICATION_ID`, test should fail to create an operation.
#[test]
fn keystore2_op_auth_missing_app_data_test_fail() {
    let sl = SecLevel::tee();
    if sl.is_keymaster() && get_vsr_api_level() < 35 {
        // `APPLICATION_DATA` key-parameter is causing the error on older devices, so skipping this
        // test to run on older devices.
        return;
    }

    let gen_params = authorizations::AuthSetBuilder::new()
        .no_auth_required()
        .algorithm(Algorithm::EC)
        .purpose(KeyPurpose::SIGN)
        .purpose(KeyPurpose::VERIFY)
        .digest(Digest::NONE)
        .ec_curve(EcCurve::P_256)
        .app_id(b"app-id".to_vec())
        .app_data(b"app-data".to_vec());

    let alias = "ks_test_auth_tags_test";
    let result = key_generations::map_ks_error(key_generations::create_key_and_operation(
        &sl,
        &gen_params,
        &authorizations::AuthSetBuilder::new()
            .purpose(KeyPurpose::SIGN)
            .digest(Digest::NONE)
            .app_id(b"app-id".to_vec()),
        alias,
    ));

    assert!(result.is_err());
    assert_eq!(Error::Km(ErrorCode::INVALID_KEY_BLOB), result.unwrap_err());
    delete_app_key(&sl.keystore2, alias).unwrap();
}

/// Generate a key with `APPLICATION_DATA` and `APPLICATION_ID`. Try to create an operation using
/// only `APPLICATION_DATA`, test should fail to create an operation.
#[test]
fn keystore2_op_auth_missing_app_id_test_fail() {
    let sl = SecLevel::tee();
    if sl.is_keymaster() && get_vsr_api_level() < 35 {
        // `APPLICATION_DATA` key-parameter is causing the error on older devices, so skipping this
        // test to run on older devices.
        return;
    }

    let gen_params = authorizations::AuthSetBuilder::new()
        .no_auth_required()
        .algorithm(Algorithm::EC)
        .purpose(KeyPurpose::SIGN)
        .purpose(KeyPurpose::VERIFY)
        .digest(Digest::NONE)
        .ec_curve(EcCurve::P_256)
        .app_data(b"app-data".to_vec())
        .app_id(b"app-id".to_vec());

    let alias = "ks_test_auth_tags_test";
    let result = key_generations::map_ks_error(key_generations::create_key_and_operation(
        &sl,
        &gen_params,
        &authorizations::AuthSetBuilder::new()
            .purpose(KeyPurpose::SIGN)
            .digest(Digest::NONE)
            .app_data(b"app-data".to_vec()),
        alias,
    ));
    assert!(result.is_err());
    assert_eq!(Error::Km(ErrorCode::INVALID_KEY_BLOB), result.unwrap_err());
    delete_app_key(&sl.keystore2, alias).unwrap();
}

/// Generate an attestation-key without specifying `APPLICATION_ID` and `APPLICATION_DATA`.
/// Test should be able to generate a new key with specifying app-id and app-data using previously
/// generated attestation-key.
#[test]
fn keystore2_gen_attested_key_auth_app_id_app_data_test_success() {
    skip_test_if_no_app_attest_key_feature!();
    let sl = SecLevel::tee();
    if sl.is_keymaster() && get_vsr_api_level() < 35 {
        // `APPLICATION_DATA` key-parameter is causing the error on older devices, so skipping this
        // test to run on older devices.
        return;
    }

    // Generate attestation key.
    let attest_gen_params = authorizations::AuthSetBuilder::new()
        .no_auth_required()
        .algorithm(Algorithm::EC)
        .purpose(KeyPurpose::ATTEST_KEY)
        .digest(Digest::SHA_2_256)
        .ec_curve(EcCurve::P_256)
        .attestation_challenge(b"foo".to_vec());
    let attest_alias = "ks_test_auth_tags_attest_key";
    let Some(attest_key_metadata) =
        key_generations::generate_key(&sl, &attest_gen_params, attest_alias).unwrap()
    else {
        return;
    };

    // Generate attested key.
    let alias = "ks_test_auth_tags_attested_key";
    let gen_params = authorizations::AuthSetBuilder::new()
        .no_auth_required()
        .algorithm(Algorithm::EC)
        .purpose(KeyPurpose::SIGN)
        .purpose(KeyPurpose::VERIFY)
        .digest(Digest::SHA_2_256)
        .ec_curve(EcCurve::P_256)
        .attestation_challenge(b"bar".to_vec())
        .app_id(b"app-id".to_vec())
        .app_data(b"app-data".to_vec());

    let result = sl.binder.generateKey(
        &KeyDescriptor {
            domain: Domain::APP,
            nspace: -1,
            alias: Some(alias.to_string()),
            blob: None,
        },
        Some(&attest_key_metadata.key),
        &gen_params,
        0,
        b"entropy",
    );

    assert!(result.is_ok());
    delete_app_key(&sl.keystore2, alias).unwrap();
    delete_app_key(&sl.keystore2, attest_alias).unwrap();
}

/// Generate an attestation-key with specifying `APPLICATION_ID` and `APPLICATION_DATA`.
/// Test should try to generate an attested key using previously generated attestation-key without
/// specifying app-id and app-data. Test should fail to generate a new key.
/// It is an oversight of the Keystore API that `APPLICATION_ID` and `APPLICATION_DATA` tags cannot
/// be provided to generateKey for an attestation key that was generated with them.
#[test]
fn keystore2_gen_attestation_key_with_auth_app_id_app_data_test_fail() {
    skip_test_if_no_app_attest_key_feature!();
    let sl = SecLevel::tee();
    if sl.is_keymaster() && get_vsr_api_level() < 35 {
        // `APPLICATION_DATA` key-parameter is causing the error on older devices, so skipping this
        // test to run on older devices.
        return;
    }

    // Generate attestation key.
    let attest_gen_params = authorizations::AuthSetBuilder::new()
        .no_auth_required()
        .algorithm(Algorithm::EC)
        .purpose(KeyPurpose::ATTEST_KEY)
        .digest(Digest::SHA_2_256)
        .ec_curve(EcCurve::P_256)
        .attestation_challenge(b"foo".to_vec())
        .app_id(b"app-id".to_vec())
        .app_data(b"app-data".to_vec());
    let attest_alias = "ks_test_auth_tags_attest_key";
    let Some(attest_key_metadata) =
        key_generations::generate_key(&sl, &attest_gen_params, attest_alias).unwrap()
    else {
        return;
    };

    // Generate new key using above generated attestation key without providing app-id and app-data.
    let alias = "ks_test_auth_tags_attested_key";
    let gen_params = authorizations::AuthSetBuilder::new()
        .no_auth_required()
        .algorithm(Algorithm::EC)
        .purpose(KeyPurpose::SIGN)
        .purpose(KeyPurpose::VERIFY)
        .digest(Digest::SHA_2_256)
        .ec_curve(EcCurve::P_256)
        .attestation_challenge(b"foo".to_vec());

    let result = key_generations::map_ks_error(sl.binder.generateKey(
        &KeyDescriptor {
            domain: Domain::APP,
            nspace: -1,
            alias: Some(alias.to_string()),
            blob: None,
        },
        Some(&attest_key_metadata.key),
        &gen_params,
        0,
        b"entropy",
    ));

    assert!(result.is_err());
    assert_eq!(Error::Km(ErrorCode::INVALID_KEY_BLOB), result.unwrap_err());
    delete_app_key(&sl.keystore2, attest_alias).unwrap();
}

fn add_hardware_token(auth_type: HardwareAuthenticatorType) {
    let keystore_auth = get_keystore_auth_service();

    let token = HardwareAuthToken {
        challenge: 0,
        userId: 0,
        authenticatorId: 0,
        authenticatorType: auth_type,
        timestamp: Timestamp { milliSeconds: 500 },
        mac: vec![],
    };
    keystore_auth.addAuthToken(&token).unwrap();
}

#[test]
fn keystore2_get_last_auth_password_success() {
    let keystore_auth = get_keystore_auth_service();

    add_hardware_token(HardwareAuthenticatorType::PASSWORD);
    assert!(keystore_auth.getLastAuthTime(0, &[HardwareAuthenticatorType::PASSWORD]).unwrap() > 0);
}

#[test]
fn keystore2_get_last_auth_fingerprint_success() {
    let keystore_auth = get_keystore_auth_service();

    add_hardware_token(HardwareAuthenticatorType::FINGERPRINT);
    assert!(
        keystore_auth.getLastAuthTime(0, &[HardwareAuthenticatorType::FINGERPRINT]).unwrap() > 0
    );
}

/// Generate a key with specifying `CERTIFICATE_SUBJECT and CERTIFICATE_SERIAL`. Test should
/// generate a key successfully and verify the specified key parameters.
#[test]
fn keystore2_gen_key_auth_serial_number_subject_test_success() {
    let sl = SecLevel::tee();
    require_keymint!(sl);

    let cert_subject = "test cert subject";
    let mut x509_name = X509NameBuilder::new().unwrap();
    x509_name.append_entry_by_text("CN", cert_subject).unwrap();
    let x509_name = x509_name.build().to_der().unwrap();

    let mut serial = BigNum::new().unwrap();
    serial.rand(159, MsbOption::MAYBE_ZERO, false).unwrap();

    let gen_params = authorizations::AuthSetBuilder::new()
        .no_auth_required()
        .algorithm(Algorithm::EC)
        .purpose(KeyPurpose::SIGN)
        .purpose(KeyPurpose::VERIFY)
        .digest(Digest::SHA_2_256)
        .ec_curve(EcCurve::P_256)
        .cert_subject_name(x509_name)
        .cert_serial(serial.to_vec());

    let alias = "ks_test_auth_tags_test";
    let Some(key_metadata) = key_generations::generate_key(&sl, &gen_params, alias).unwrap() else {
        return;
    };
    verify_certificate_subject_name(
        key_metadata.certificate.as_ref().unwrap(),
        cert_subject.as_bytes(),
    );
    verify_certificate_serial_num(key_metadata.certificate.as_ref().unwrap(), &serial);
    delete_app_key(&sl.keystore2, alias).unwrap();
}

#[test]
fn test_supplementary_attestation_info() {
    if !keystore2_flags::attest_modules() {
        // Module info is only populated if the flag is set.
        return;
    }

    // Test should not run before MODULE_HASH supplementary info is populated.
    assert!(rustutils::system_properties::read_bool("keystore.module_hash.sent", false)
        .unwrap_or(false));

    let sl = SecLevel::tee();

    // Retrieve the input value that gets hashed into the attestation.
    let module_info = sl
        .keystore2
        .getSupplementaryAttestationInfo(Tag::MODULE_HASH)
        .expect("supplementary info for MODULE_HASH should be populated during startup");
    let again = sl.keystore2.getSupplementaryAttestationInfo(Tag::MODULE_HASH).unwrap();
    assert_eq!(again, module_info);
    let want_hash = digest::Sha256::hash(&module_info).to_vec();

    // Requesting other types of information should fail.
    let result = key_generations::map_ks_error(
        sl.keystore2.getSupplementaryAttestationInfo(Tag::BLOCK_MODE),
    );
    assert!(result.is_err());
    assert_eq!(result.unwrap_err(), Error::Rc(ResponseCode::INVALID_ARGUMENT));

    if sl.get_keymint_version() < 400 {
        // Module hash will only be populated in KeyMint if the underlying device is KeyMint V4+.
        return;
    }

    // Generate an attestation.
    let alias = "ks_module_info_test";
    let params = authorizations::AuthSetBuilder::new()
        .no_auth_required()
        .algorithm(Algorithm::EC)
        .purpose(KeyPurpose::SIGN)
        .purpose(KeyPurpose::VERIFY)
        .digest(Digest::SHA_2_256)
        .ec_curve(EcCurve::P_256)
        .attestation_challenge(b"froop".to_vec());
    let metadata = key_generations::generate_key(&sl, &params, alias)
        .expect("failed key generation")
        .expect("no metadata");
    let cert_data = metadata.certificate.as_ref().unwrap();
    let cert = Certificate::from_der(cert_data).expect("failed to parse X509 cert");
    let exts = cert.tbs_certificate.extensions.expect("no X.509 extensions");
    let ext = exts
        .iter()
        .find(|ext| ext.extn_id == ATTESTATION_EXTENSION_OID)
        .expect("no attestation extension");
    let ext = AttestationExtension::from_der(ext.extn_value.as_bytes())
        .expect("failed to parse attestation extension");

    // Find the attested module hash value.
    let mut got_hash = None;
    for auth in ext.sw_enforced.auths.into_owned().iter() {
        if let KeyParameter { tag: Tag::MODULE_HASH, value: KeyParameterValue::Blob(hash) } = auth {
            got_hash = Some(hash.clone());
        }
    }
    let got_hash = got_hash.expect("no MODULE_HASH in sw_enforced");
    assert_eq!(hex::encode(got_hash), hex::encode(want_hash));
}
