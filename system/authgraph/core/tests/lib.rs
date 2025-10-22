// Copyright 2025 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
////////////////////////////////////////////////////////////////////////////////

//! The unit tests module
#![no_std]
extern crate alloc;

#[cfg(test)]
mod tests {
    use super::*;
    use alloc::string::ToString;
    use alloc::vec;
    use authgraph_boringssl::BoringEcDsa;
    use authgraph_core::key::{
        CertChain, DiceChainEntry, DiceChainEntryPayload, DiceChainEntryPayloadPartiallyDecoded,
        Identity,
    };
    use authgraph_core_test::{
        create_dice_cert_chain_for_guest_os, create_dice_leaf_cert, SAMPLE_INSTANCE_HASH,
    };
    use coset::CborSerializable;

    #[test]
    fn test_legacy_open_dice_payload() {
        // Some legacy devices have an open-DICE format config descriptor (b/261647022) rather than
        // the format used in the Android RKP HAL.  Ensure that they still parse.
        let data = hex::decode(concat!(
            "a8",   // 8-map
            "01",   // Issuer:
            "7828", // 40-tstr
            "32336462613837333030633932323934663836333566323738316464346633366362313934383835",
            "02",   // Subject:
            "7828", // 40-tstr
            "33376165616366396230333465643064376166383665306634653431656163356335383134343966",
            "3a00474450", // Code Hash(-4670545):
            "5840",       // 64-bstr
            "3c9aa93a6766f16f5fbd3dfc7e5059b39cdc8aa0cf546cc878d588a69cfcd654",
            "2fa509bd6cc14b7160a6bf34545ffdd840f0e91e35b274a7a952b5b0efcff1b0",
            "3a00474453", // Configuration Descriptor (-4670548):
            "5840",       // 64-bstr
            // The RKP HAL expects the following data to match schema:
            //
            //     { ? -70002 : tstr, ? -70003 : int / tstr, ? -70004 : null,
            //       ? -70005 : uint, ? -70006 : null, }
            //
            // However, the open-DICE spec had:
            //     If the configuration input is a hash this field contains the original
            //     configuration data that was hashed. If it is not a hash, this field contains the
            //     exact 64-byte configuration input value used to compute CDI values."
            "e2000000000001508609939b5a4f0f0800000000000000000101000000000000",
            "0000000000000000000000000000000000000000000000000000000000000000",
            "3a00474454", // Authority Hash (-4670549):
            "5840",       // 64-bstr
            "4d00da66eabbb2b684641a57e96c8e64d76df1e31ea203bbbb9f439372c1a8ec",
            "aa550000aa550000aa550000aa550000aa550000aa550000aa550000aa550000",
            "3a00474456", // Mode (-4670551):
            "4101",       // 1-bstr value 0x01
            "3a00474457", // Subject Public Key (-4670552):
            "5871",       // 113-bstr
            "a601020338220481022002215830694a8fa269c3375b770ef61d06dec5a78595",
            "2ee96db3602b57c50d8fa67f97e874fbd3f5b42e66ac8ead3f3eb3b130f42258",
            "301b5574256be9f4770c3325422e53981b1a969387068a51aea68fe98f779be5",
            "75ecb077a60106852af654377e56d446a6",
            "3a00474458", // Key Usage (-4670553):
            "4120"        // 1-bstr value 0x20
        ))
        .unwrap();

        assert!(DiceChainEntryPayloadPartiallyDecoded::from_slice(&data).is_ok());
        assert!(DiceChainEntryPayload::from_slice(&data).is_ok());
    }

    /// Test instance hash extraction API method with test data (from a device) that has
    /// an instance hash
    #[test]
    fn test_instance_hash_extraction() {
        // Read the DICE chain bytes from the file
        let dice_chain_bytes: &[u8] = include_bytes!("../testdata/bcc");
        // Create an explicit key DICE chain out of it
        let explicit_key_dice_chain = CertChain::from_non_explicit_key_cert_chain(dice_chain_bytes)
            .expect("error converting DICE chain to an explicit key DICE chain");
        // Extract the instance hash
        let instance_hash = explicit_key_dice_chain
            .extract_instance_identifier_in_guest_os_entry()
            .expect("error in extracting the instance id")
            .expect("no instance id found");
        let expected_instance_hash = vec![
            68, 32, 41, 225, 228, 67, 229, 107, 207, 212, 74, 74, 191, 25, 211, 133, 57, 166, 35,
            146, 86, 89, 182, 52, 183, 255, 215, 204, 5, 183, 254, 79, 129, 240, 197, 252, 238, 69,
            124, 44, 164, 214, 205, 87, 194, 226, 124, 249, 158, 219, 188, 127, 55, 143, 232, 142,
            119, 174, 202, 160, 234, 179, 205, 30,
        ];
        assert_eq!(instance_hash, expected_instance_hash);
    }

    /// Test instance hash extraction API method with test data (from a device) that does not
    /// have an instance hash
    #[test]
    fn test_instance_hash_extraction_negative() {
        let mut hex_data =
            std::str::from_utf8(include_bytes!("../../tests/testdata/sample_identity.hex"))
                .unwrap()
                .to_string();
        hex_data.retain(|c| !c.is_whitespace());
        let data = hex::decode(hex_data).unwrap();
        let identity = Identity::from_slice(&data).expect("identity data did not decode");
        // Extract the instance hash
        let instance_hash = identity
            .cert_chain
            .extract_instance_identifier_in_guest_os_entry()
            .expect("error in extracting the instance id");
        assert_eq!(instance_hash, None);
    }

    /// Test instance hash extraction with a programmatically created DICE cert chain that has
    /// instance hash
    #[test]
    fn test_instance_hash_extraction_with_code_generated_dice_chain() {
        let (_, _, dice_chain_bytes) =
            create_dice_cert_chain_for_guest_os(Some(SAMPLE_INSTANCE_HASH), 1);
        let dice_cert_chain =
            CertChain::from_slice(&dice_chain_bytes).expect("error decoding the DICE chain");
        // Extract the instance hash
        let instance_hash = dice_cert_chain
            .extract_instance_identifier_in_guest_os_entry()
            .expect("error in extracting the instance id")
            .expect("no instance id found");
        assert_eq!(instance_hash, SAMPLE_INSTANCE_HASH);
    }

    /// Test instance hash extraction with a programmatically created DICE cert chain that does not
    /// has instance hash
    #[test]
    fn test_instance_hash_extraction_with_code_generated_dice_chain_negative() {
        let (_, _, dice_chain_bytes) = create_dice_cert_chain_for_guest_os(None, 0);
        let dice_cert_chain =
            CertChain::from_slice(&dice_chain_bytes).expect("error decoding the DICE chain");
        // Extract the instance id
        let instance_hash = dice_cert_chain
            .extract_instance_identifier_in_guest_os_entry()
            .expect("error in extracting the instance id");
        assert_eq!(instance_hash, None);
    }

    /// Test extending a DICE chain with a giveen DICE certificate
    #[test]
    fn test_dice_chain_extend() {
        let ecdsa = BoringEcDsa;
        // Create a DICE chain for a pvm instance and a leaf cert for keymint TA
        let (_sign_key, cdi_values, cert_chain_bytes) =
            create_dice_cert_chain_for_guest_os(Some(SAMPLE_INSTANCE_HASH), 1);
        let leaf_cert_bytes = create_dice_leaf_cert(cdi_values, "keymint", 1);
        let dice_chain =
            CertChain::from_slice(&cert_chain_bytes).expect("failed to decode dice chain");
        let leaf_cert =
            DiceChainEntry::from_slice(&leaf_cert_bytes).expect("failed to decode the leaf cert");
        // Extend the pvm's DICE chain with the leaf cert
        let extended_dice_chain =
            dice_chain.extend_with(&leaf_cert, &ecdsa).expect("failed to extend the dice chain");
        // Verify that the original DICE chain has extended as expected
        assert_eq!(
            dice_chain.dice_cert_chain.as_ref().unwrap().len() + 1,
            extended_dice_chain.dice_cert_chain.as_ref().unwrap().len()
        );
        assert!(extended_dice_chain.is_current_leaf(&leaf_cert));
        assert!(!dice_chain.is_current_leaf(&leaf_cert));

        assert_eq!(
            &leaf_cert,
            extended_dice_chain.dice_cert_chain.as_ref().unwrap().last().unwrap()
        );

        // Create a secondary DICE chain, try to extend it with the previous leaf cert and expect
        // error
        let (_, _, cert_chain_bytes_2) =
            create_dice_cert_chain_for_guest_os(Some(SAMPLE_INSTANCE_HASH), 2);
        let dice_chain_2 =
            CertChain::from_slice(&cert_chain_bytes_2).expect("failed to decode dice chain 2");
        assert!(dice_chain_2.extend_with(&leaf_cert, &ecdsa).is_err());
    }
}
