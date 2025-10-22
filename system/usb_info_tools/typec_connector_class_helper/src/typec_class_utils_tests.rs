// Copyright 2024 The Android Open Source Project
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

//! typec_class_utils_tests

use std::path::PathBuf;
use std::{fs, io::Write};

use std::io::Cursor;

use tempfile::tempdir;

use crate::typec_class_utils::*;
use crate::usb_pd_utils::*;

/// Creates a temporary hierarchy of directories and files with content.
/// The dir_paths_files_content array param expects the following format:
/// * `[(dir_path, [array of (file_name, file_content)])]`
///
/// ### Example
/// To create the following directory hierarchy:
/// ```
/// /port1
///     /port1/file1: content1
///     /port1/file2: content2
/// /port1/subport1
///     /port2/file1: content1
/// ```
/// The dir_paths_files_content parameter should receive the value:
/// ```
/// [
/// (
///    "port1",
///    &[("file1", "content1"), ("file2", "content2")],
/// ),
/// (
///    "port1/subport1",
///    &[("file1", "content1")],
/// )
/// ]
/// ```
fn create_temp_dir_with_dirs_files_content(
    dir_paths_files_content: &[(&str, &[(&str, &str)])],
) -> PathBuf {
    let root_dir = tempdir().expect("Cannot create temporary directory");
    for (dir_path, files_content) in dir_paths_files_content {
        let full_dir_path = root_dir.path().join(dir_path);
        println!("FULL: {:?}", &full_dir_path);
        fs::create_dir(&full_dir_path).expect("Cannot create directory");

        for (file_name, file_content) in files_content.iter() {
            let mut file = fs::File::create(full_dir_path.join(file_name))
                .expect("Cannot create temporary file");
            writeln!(file, "{file_content}").expect("Could not write to temporary file");
        }
    }
    root_dir.into_path()
}

/// Creates a hierarchy of directories and files similar to the one created by the Type-C Connector Class.
fn create_full_typec_connector_class_temp_file_hierarchy() -> PathBuf {
    create_temp_dir_with_dirs_files_content(&[
        (
            "port0",
            &[
                ("uevent", "DEVTYPE=typec_port\nTYPEC_PORT=port0"),
                ("vconn_source", "yes"),
                ("data_role", "[host] device"),
            ],
        ),
        (
            "port0/port0-partner",
            &[("usb_power_delivery_revision", "3.0"), ("number_of_alternate_modes", "1")],
        ),
        (
            "port0/port0-partner/identity",
            &[
                ("id_header", "0x4c4018d1"),
                ("product", "0x50490001"),
                ("cert_stat", "0x0"),
                ("product_type_vdo1", "0x45800012"),
                ("product_type_vdo2", "0x0"),
                ("product_type_vdo3", "0x0"),
            ],
        ),
        (
            "port0/port0-partner/port0-partner.0",
            &[("description", "DisplayPort"), ("vdo", "0x001c0045")],
        ),
        (
            "port0/port0-partner/port0-partner.0/mode1",
            &[("description", "DisplayPort"), ("vdo", "0x001c0045")],
        ),
        ("port0/port0-partner/port0-partner.0/ignored", &[("ignored", "ignored content")]),
        ("port0/port0-partner/pd0", &[("revision", "3.0")]),
        ("port0/port0-partner/pd0/sink-capabilities", &[("uevent", "")]),
        (
            "port0/port0-partner/pd0/sink-capabilities/1:fixed_supply",
            &[("operational_current", "0mA"), ("voltage", "5000mV")],
        ),
        ("port0/port0-partner/pd0/source-capabilities", &[("uevent", "")]),
        (
            "port0/port0-partner/pd0/source-capabilities/4:fixed_supply",
            &[("maximum_current", "3000mA"), ("voltage", "20000mV")],
        ),
        (
            "port0/port0-partner/pd0/source-capabilities/2:fixed_supply",
            &[("maximum_current", "3000mA"), ("voltage", "9000mV")],
        ),
        ("port0/port0-cable", &[("usb_power_delivery_revision", "3.0")]),
        (
            "port0/port0-cable/identity",
            &[
                ("cert_stat", "0x290"),
                ("id_header", "0x1800228a"),
                ("product", "0x94880001"),
                ("product_type_vdo1", "0x11082043"),
                ("product_type_vdo2", "0x00000000"),
                ("product_type_vdo3", "0x00000000"),
            ],
        ),
        ("port0/port0-cable/port0-plug0", &[("number_of_alternate_modes", "2")]),
        ("port0/port0-cable/port0-plug0/port0-plug0.0", &[("mode", "1"), ("svid", "1e4e")]),
        (
            "port0/port0-cable/port0-plug0/port0-plug0.0/mode1",
            &[("active", "no"), ("vdo", "0x9031011c")],
        ),
        ("port0/port0-cable/port0-plug0/port0-plug0.1", &[("mode", "1"), ("svid", "8087")]),
        (
            "port0/port0-cable/port0-plug0/port0-plug0.1/mode1",
            &[("active", "no"), ("vdo", "0x00030001")],
        ),
        ("port0/port0-cable/port0-plug0/port0-plug0.ignore", &[("ignored", "ignored content")]),
        (
            "port0/physical_location",
            &[
                ("panel", "right"),
                ("horizontal_position", "right"),
                ("vertical_position", "ignored"),
            ],
        ),
        ("port0/usb3-port1", &[]),
        ("port0/usb3-port1/device", &[("busnum", "3"), ("devnum", "9"), ("devpath", "1")]),
        ("port0/usb3-port1/device/3-1.1", &[("busnum", "3"), ("devnum", "10"), ("devpath", "1.1")]),
        ("port0/usb3-port1/device/3-1.2", &[("busnum", "3"), ("devnum", "11"), ("devpath", "1.2")]),
        (
            "port0/usb3-port1/device/3-1.2/3-1.2.4",
            &[("busnum", "3"), ("devnum", "12"), ("devpath", "1.2.4")],
        ),
        ("port0/drm_connector", &[("connector_id", "258"), ("status", "disconnected")]),
        ("port1", &[]),
        ("portignored", &[("ignored", "ignored content")]),
    ])
}

#[test]
fn test_print_dir_files() {
    let temp_dir_path = create_full_typec_connector_class_temp_file_hierarchy();
    let mut outp_buffer: Cursor<Vec<u8>> = Cursor::new(Vec::new());
    let mut outp_writer = OutputWriter::new(&mut outp_buffer, 0);
    let expected_outp = "port0
  data_role: [host] device
  uevent: DEVTYPE=typec_port
  TYPEC_PORT=port0
  vconn_source: yes
";

    outp_writer
        .print_dir_files(&temp_dir_path.join("port0"))
        .expect("Could not execute print_dir_files");

    assert_eq!(
        &String::from_utf8(outp_buffer.into_inner()).expect("Cannot decode output buffer"),
        expected_outp
    );
}

#[test]
fn test_print_vdo() {
    let temp_dir_path = create_full_typec_connector_class_temp_file_hierarchy();
    let mut outp_buffer: Cursor<Vec<u8>> = Cursor::new(Vec::new());
    let mut outp_writer = OutputWriter::new(&mut outp_buffer, 0);
    let expected_outp = "id_header: 0x4c4018d1
  USB Vendor ID: 0x18d1
  Reserved: 0x40
  Product Type (DFP): 0x0
  Modal Operation Supported: 0x1
  Product Type (UFP/Cable Plug): 0x1
  USB Capable as a USB Device: 0x1
  USB Capable as a USB Host: 0x0
";

    let _ = print_vdo(
        &temp_dir_path.join("port0/port0-partner/identity/id_header"),
        pd30_data::ID_HEADER_VDO,
        &mut outp_writer,
    );

    assert_eq!(
        &String::from_utf8(outp_buffer.into_inner()).expect("Cannot decode output buffer"),
        expected_outp
    );
}

#[test]
fn test_read_vdo() {
    let temp_dir_path =
        create_temp_dir_with_dirs_files_content(&[("identity", &[("id_header", "0x4c4018d1")])]);

    let vdo =
        read_vdo(&temp_dir_path.join("identity/id_header")).expect("read_vdo returned an error");
    assert_eq!(vdo, 0x4c4018d1);
}

#[test]
fn test_get_pd_revision() {
    let temp_dir_path = create_temp_dir_with_dirs_files_content(&[(
        "port0-partner",
        &[("usb_power_delivery_revision", "3.0")],
    )]);
    let pd_rev = get_pd_revision(&temp_dir_path.join("port0-partner"));
    assert_eq!(pd_rev, PdRev::Pd30);
}

#[test]
fn test_parse_dirs_and_execute() {
    let temp_dir_path = create_full_typec_connector_class_temp_file_hierarchy();
    let mut outp_buffer: Cursor<Vec<u8>> = Cursor::new(Vec::new());
    let mut outp_writer = OutputWriter::new(&mut outp_buffer, 0);
    let expected_outp = "port0
  data_role: [host] device
  uevent: DEVTYPE=typec_port
  TYPEC_PORT=port0
  vconn_source: yes
port1
";

    let _ = parse_dirs_and_execute(&temp_dir_path, &mut outp_writer, PORT_REGEX, |path, out_wr| {
        let _ = out_wr.print_dir_files(path);
    });

    assert_eq!(
        &String::from_utf8(outp_buffer.into_inner()).expect("Cannot decode output buffer"),
        expected_outp
    )
}

#[test]
fn test_get_partner_product_type_pd30_ufp() {
    let temp_dir_path =
        create_temp_dir_with_dirs_files_content(&[("identity", &[("id_header", "0x4c4018d1")])]);
    let prod_type = get_partner_product_type(&temp_dir_path, PdRev::Pd30);

    assert_eq!(prod_type, ProductType::Port((PortType::Ufp, PdRev::Pd30)));
}

#[test]
fn test_get_partner_product_type_pd31_drd() {
    let temp_dir_path =
        create_temp_dir_with_dirs_files_content(&[("identity", &[("id_header", "0x08800000")])]);
    let prod_type = get_partner_product_type(&temp_dir_path, PdRev::Pd31);

    assert_eq!(prod_type, ProductType::Port((PortType::Drd, PdRev::Pd31)));
}

#[test]
fn test_get_partner_product_type_other() {
    let temp_dir_path = create_temp_dir_with_dirs_files_content(&[]);
    let prod_type = get_partner_product_type(&temp_dir_path, PdRev::Pd20);

    assert_eq!(prod_type, ProductType::Other);
}

#[test]
fn test_print_alt_mode() {
    let temp_dir_path = create_full_typec_connector_class_temp_file_hierarchy();
    let mut outp_buffer: Cursor<Vec<u8>> = Cursor::new(Vec::new());
    let mut outp_writer = OutputWriter::new(&mut outp_buffer, 0);
    let expected_outp = "port0-partner.0
  description: DisplayPort
  vdo: 0x001c0045
  mode1
    description: DisplayPort
    vdo: 0x001c0045
";

    print_alt_mode(
        &temp_dir_path.join("port0").join("port0-partner").join("port0-partner.0"),
        &mut outp_writer,
    );
    assert_eq!(
        &String::from_utf8(outp_buffer.into_inner()).expect("Cannot decode output buffer"),
        expected_outp
    )
}

#[test]
fn test_print_pdos() {
    let temp_dir_path = create_full_typec_connector_class_temp_file_hierarchy();
    let mut outp_buffer: Cursor<Vec<u8>> = Cursor::new(Vec::new());
    let mut outp_writer = OutputWriter::new(&mut outp_buffer, 0);
    let expected_outp = "pd0
  revision: 3.0
  sink-capabilities
    uevent:\x20
    1:fixed_supply
      operational_current: 0mA
      voltage: 5000mV
  source-capabilities
    uevent:\x20
    2:fixed_supply
      maximum_current: 3000mA
      voltage: 9000mV
    4:fixed_supply
      maximum_current: 3000mA
      voltage: 20000mV
";

    print_pdos(&temp_dir_path.join("port0").join("port0-partner").join("pd0"), &mut outp_writer);

    assert_eq!(
        &String::from_utf8(outp_buffer.into_inner()).expect("Cannot decode output buffer"),
        expected_outp
    )
}

#[test]
fn test_get_cable_product_type() {
    let temp_dir_path = create_temp_dir_with_dirs_files_content(&[
        ("port0-cable", &[("usb_power_delivery_revision", "3.0")]),
        ("port0-cable/identity", &[("id_header", "0x1c6003f0")]),
    ]);
    let expected_outp: ProductType = ProductType::Cable((CableType::Passive, PdRev::Pd30));

    let prod_type = get_cable_product_type(&temp_dir_path.join("port0-cable"));

    assert_eq!(prod_type, expected_outp)
}

#[test]
fn test_full_command_output() {
    let temp_dir_path = create_full_typec_connector_class_temp_file_hierarchy();
    let mut outp_buffer: Cursor<Vec<u8>> = Cursor::new(Vec::new());
    let mut outp_writer = OutputWriter::new(&mut outp_buffer, 0);
    let expected_outp = "port0
  data_role: [host] device
  uevent: DEVTYPE=typec_port
  TYPEC_PORT=port0
  vconn_source: yes
  port0-partner
    number_of_alternate_modes: 1
    usb_power_delivery_revision: 3.0
    identity
      id_header: 0x4c4018d1
        USB Vendor ID: 0x18d1
        Reserved: 0x40
        Product Type (DFP): 0x0
        Modal Operation Supported: 0x1
        Product Type (UFP/Cable Plug): 0x1
        USB Capable as a USB Device: 0x1
        USB Capable as a USB Host: 0x0
      cert_stat: 0x0
        XID: 0x0
      product: 0x50490001
        bcdDevice: 0x1
        USB Product ID: 0x5049
      product_type_vdo1: 0x45800012
        USB Highest Speed: 0x2
        Alternate Modes: 0x2
        Reserved: 0x20000
        Device Capability: 0x5
        Reserved: 0x0
        UFP VDO Version: 0x2
      product_type_vdo2: 0x0
        USB3 Max Power: 0x0
        USB3 Min Power: 0x0
        Reserved: 0x0
        USB4 Max Power: 0x0
        USB4 Min Power: 0x0
        Reserved: 0x0
      product_type_vdo3: 0x0
    port0-partner.0
      description: DisplayPort
      vdo: 0x001c0045
      mode1
        description: DisplayPort
        vdo: 0x001c0045
    pd0
      revision: 3.0
      sink-capabilities
        uevent:\x20
        1:fixed_supply
          operational_current: 0mA
          voltage: 5000mV
      source-capabilities
        uevent:\x20
        2:fixed_supply
          maximum_current: 3000mA
          voltage: 9000mV
        4:fixed_supply
          maximum_current: 3000mA
          voltage: 20000mV
  port0-cable
    usb_power_delivery_revision: 3.0
    identity
      id_header: 0x1800228a
        USB Vendor ID: 0x228a
        Reserved: 0x0
        Product Type (DFP): 0x0
        Modal Operation Supported: 0x0
        Product Type (UFP/Cable Plug): 0x3
        USB Capable as a USB Device: 0x0
        USB Capable as a USB Host: 0x0
      cert_stat: 0x290
        XID: 0x290
      product: 0x94880001
        bcdDevice: 0x1
        USB Product ID: 0x9488
      product_type_vdo1: 0x11082043
        USB Speed: 0x3
        Reserved: 0x0
        Vbus Current Handling: 0x2
        Reserved: 0x0
        Maximum Vbus Voltage: 0x0
        Cable Termination Type: 0x0
        Cable Latency: 0x1
        Reserved: 0x0
        USB Type-C Plug to USB Type: 0x2
        Reserved: 0x0
        VDO Version: 0x0
        Firmware Version: 0x1
        HW Version: 0x1
      product_type_vdo2: 0x0
      product_type_vdo3: 0x0
    port0-plug0
      number_of_alternate_modes: 2
      port0-plug0.0
        mode: 1
        svid: 1e4e
        mode1
          active: no
          vdo: 0x9031011c
      port0-plug0.1
        mode: 1
        svid: 8087
        mode1
          active: no
          vdo: 0x00030001
  physical_location
    panel: right
    horizontal_position: right
  usb_device
    busnum: 3
    devnum: 9
    devpath: 1
    usb_device
      busnum: 3
      devnum: 10
      devpath: 1.1
    usb_device
      busnum: 3
      devnum: 11
      devpath: 1.2
      usb_device
        busnum: 3
        devnum: 12
        devpath: 1.2.4
  dp_connector
    connector_id: 258
    status: disconnected
port1
";
    let _ = parse_dirs_and_execute(&temp_dir_path, &mut outp_writer, PORT_REGEX, print_port_info);
    assert_eq!(
        &String::from_utf8(outp_buffer.into_inner()).expect("Cannot decode output buffer"),
        expected_outp
    )
}
