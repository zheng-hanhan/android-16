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

//! usb_pd_utils

/// Contains information for decoding a specific piece of data from a
/// Vendor Data Object (VDO).
pub struct VdoField {
    pub index: i32,
    pub mask: u32,
    pub description: &'static str,
}

impl VdoField {
    /// Returns the "description: field_value" string for a VDO field.
    pub fn decode_vdo(&self, vdo: u32) -> String {
        let field_val: u32 = (vdo & self.mask) >> self.index;
        [self.description, &format!(": 0x{:x}", field_val)].concat()
    }
}

// Masks for id_header fields.
pub const UFP_PRODUCT_TYPE_MASK: u32 = 0x38000000;
pub const DFP_PRODUCT_TYPE_MASK: u32 = 0x03800000;

#[derive(Debug, PartialEq)]
pub enum PdRev {
    None,
    Pd20,
    Pd30,
    Pd31,
}

#[derive(Debug, PartialEq)]
pub enum CableType {
    Passive,
    Active,
    Vpd,
}

#[derive(Debug, PartialEq)]
pub enum PortType {
    Ufp,
    Dfp,
    Drd,
}

#[derive(Debug, PartialEq)]
pub enum ProductType {
    Cable((CableType, PdRev)),
    Ama(PdRev),
    Vpd(PdRev),
    Port((PortType, PdRev)),
    Other,
}

/// Constants specific to USB PD revision 2.0.
pub mod pd20_data {
    use super::VdoField;

    // Expected product identifiers extracted from id_header VDO.
    pub const AMA_COMP: u32 = 0x28000000;
    // Expected id_header field results.
    pub const PASSIVE_CABLE_COMP: u32 = 0x20000000;
    pub const ACTIVE_CABLE_COMP: u32 = 0x18000000;

    // Vendor Data Objects (VDO) decoding information (source: USB PD spec).
    pub const ID_HEADER_VDO: &[VdoField] = &[
        VdoField { index: 0, mask: 0x0000ffff, description: "USB Vendor ID" },
        VdoField { index: 16, mask: 0x03ff0000, description: "Reserved" },
        VdoField { index: 26, mask: 0x04000000, description: "Modal Operation Supported" },
        VdoField { index: 27, mask: 0x38000000, description: "Product Type" },
        VdoField { index: 30, mask: 0x40000000, description: "USB Capable as a USB Device" },
        VdoField { index: 31, mask: 0x80000000, description: "USB Capable as a USB Host" },
    ];

    pub const CERT_STAT_VDO: &[VdoField] =
        &[VdoField { index: 0, mask: 0xffffffff, description: "XID" }];

    pub const PRODUCT_VDO: &[VdoField] = &[
        VdoField { index: 0, mask: 0x0000ffff, description: "bcdDevice" },
        VdoField { index: 16, mask: 0xffff0000, description: "USB Product ID" },
    ];

    pub const AMA_VDO: &[VdoField] = &[
        VdoField { index: 0, mask: 0x00000007, description: "USB SS Signaling Support" },
        VdoField { index: 3, mask: 0x00000008, description: "Vbus Required" },
        VdoField { index: 4, mask: 0x00000010, description: "Vconn Required" },
        VdoField { index: 5, mask: 0x000000e0, description: "Vconn Power" },
        VdoField { index: 8, mask: 0x00000100, description: "SSRX2 Directionality Support" },
        VdoField { index: 9, mask: 0x00000200, description: "SSRX1 Directionality Support" },
        VdoField { index: 10, mask: 0x00000400, description: "SSTX2 Directionality Support" },
        VdoField { index: 11, mask: 0x00000800, description: "SSTX1 Directionality Support" },
        VdoField { index: 12, mask: 0x00fff000, description: "Reserved" },
        VdoField { index: 24, mask: 0x0f000000, description: "Firmware Version" },
        VdoField { index: 28, mask: 0xf0000000, description: "Hardware Version" },
    ];

    pub const PASSIVE_VDO: &[VdoField] = &[
        VdoField { index: 0, mask: 0x00000007, description: "USB Speed" },
        VdoField { index: 3, mask: 0x00000008, description: "Reserved" },
        VdoField { index: 4, mask: 0x00000010, description: "Vbus Through Cable" },
        VdoField { index: 5, mask: 0x00000060, description: "Vbus Current Handling" },
        VdoField { index: 7, mask: 0x00000080, description: "SSRX2 Directionality Support" },
        VdoField { index: 8, mask: 0x00000100, description: "SSRX1 Directionality Support" },
        VdoField { index: 9, mask: 0x00000200, description: "SSTX2 Directionality Support" },
        VdoField { index: 10, mask: 0x00000400, description: "SSTX1 Directionality Support" },
        VdoField { index: 11, mask: 0x00001800, description: "Cable Termination Type" },
        VdoField { index: 13, mask: 0x0001e000, description: "Cable Latency" },
        VdoField { index: 17, mask: 0x00020000, description: "Reserved" },
        VdoField { index: 18, mask: 0x000c0000, description: "USB Type-C Plug to USB Type" },
        VdoField { index: 20, mask: 0x00f00000, description: "Reserved" },
        VdoField { index: 24, mask: 0x0f000000, description: "Firmware Version" },
        VdoField { index: 28, mask: 0xf0000000, description: "HW Version" },
    ];

    pub const ACTIVE_VDO: &[VdoField] = &[
        VdoField { index: 0, mask: 0x00000007, description: "USB Speed" },
        VdoField { index: 3, mask: 0x00000008, description: "SOP'' Controller Present" },
        VdoField { index: 4, mask: 0x00000010, description: "Vbus Through Cable" },
        VdoField { index: 5, mask: 0x00000060, description: "Vbus Current Handling" },
        VdoField { index: 7, mask: 0x00000080, description: "SSRX2 Directionality Support" },
        VdoField { index: 8, mask: 0x00000100, description: "SSRX1 Directionality Support" },
        VdoField { index: 9, mask: 0x00000200, description: "SSTX2 Directionality Support" },
        VdoField { index: 10, mask: 0x00000400, description: "SSTX1 Directionality Support" },
        VdoField { index: 11, mask: 0x00001800, description: "Cable Termination Type" },
        VdoField { index: 13, mask: 0x0001e000, description: "Cable Latency" },
        VdoField { index: 17, mask: 0x00020000, description: "Reserved" },
        VdoField { index: 18, mask: 0x000c0000, description: "USB Type-C Plug to USB Type" },
        VdoField { index: 20, mask: 0x00f00000, description: "Reserved" },
        VdoField { index: 24, mask: 0x0f000000, description: "Firmware Version" },
        VdoField { index: 28, mask: 0xf0000000, description: "HW Version" },
    ];
}

/// Constants specific to USB PD revision 3.0.
pub mod pd30_data {
    use super::VdoField;

    pub const AMA_COMP: u32 = 0x28000000;
    pub const VPD_COMP: u32 = 0x30000000;
    pub const HUB_COMP: u32 = 0x08000000;
    pub const PERIPHERAL_COMP: u32 = 0x10000000;
    pub const DFP_HUB_COMP: u32 = 0x00800000;
    pub const DFP_HOST_COMP: u32 = 0x01000000;
    pub const POWER_BRICK_COMP: u32 = 0x01800000;
    // Expected id_header field results.
    pub const PASSIVE_CABLE_COMP: u32 = 0x18000000;
    pub const ACTIVE_CABLE_COMP: u32 = 0x20000000;

    pub const ID_HEADER_VDO: &[VdoField] = &[
        VdoField { index: 0, mask: 0x0000ffff, description: "USB Vendor ID" },
        VdoField { index: 16, mask: 0x007f0000, description: "Reserved" },
        VdoField { index: 23, mask: 0x03800000, description: "Product Type (DFP)" },
        VdoField { index: 26, mask: 0x04000000, description: "Modal Operation Supported" },
        VdoField { index: 27, mask: 0x38000000, description: "Product Type (UFP/Cable Plug)" },
        VdoField { index: 30, mask: 0x40000000, description: "USB Capable as a USB Device" },
        VdoField { index: 31, mask: 0x80000000, description: "USB Capable as a USB Host" },
    ];

    pub const CERT_STAT_VDO: &[VdoField] =
        &[VdoField { index: 0, mask: 0xffffffff, description: "XID" }];

    pub const PRODUCT_VDO: &[VdoField] = &[
        VdoField { index: 0, mask: 0x0000ffff, description: "bcdDevice" },
        VdoField { index: 16, mask: 0xffff0000, description: "USB Product ID" },
    ];

    pub const AMA_VDO: &[VdoField] = &[
        VdoField { index: 0, mask: 0x00000007, description: "USB Highest Speed" },
        VdoField { index: 3, mask: 0x00000008, description: "Vbus Required" },
        VdoField { index: 4, mask: 0x00000010, description: "Vconn Required" },
        VdoField { index: 5, mask: 0x000000e0, description: "Vconn Power" },
        VdoField { index: 8, mask: 0x001fff00, description: "Reserved" },
        VdoField { index: 21, mask: 0x00e00000, description: "VDO Version" },
        VdoField { index: 24, mask: 0x0f000000, description: "Firmware Version" },
        VdoField { index: 28, mask: 0xf0000000, description: "Hardware Version" },
    ];

    pub const VPD_VDO: &[VdoField] = &[
        VdoField { index: 0, mask: 0x00000001, description: "Charge Through Support" },
        VdoField { index: 1, mask: 0x0000007e, description: "Ground Impedance" },
        VdoField { index: 7, mask: 0x00001f80, description: "Vbus Impedance" },
        VdoField { index: 13, mask: 0x00002000, description: "Reserved" },
        VdoField { index: 14, mask: 0x00004000, description: "Charge Through Current Support" },
        VdoField { index: 15, mask: 0x00018000, description: "Maximum Vbus Voltage" },
        VdoField { index: 17, mask: 0x001e0000, description: "Reserved" },
    ];

    pub const PASSIVE_VDO: &[VdoField] = &[
        VdoField { index: 0, mask: 0x00000007, description: "USB Speed" },
        VdoField { index: 3, mask: 0x00000018, description: "Reserved" },
        VdoField { index: 5, mask: 0x00000060, description: "Vbus Current Handling" },
        VdoField { index: 7, mask: 0x00000180, description: "Reserved" },
        VdoField { index: 9, mask: 0x00000600, description: "Maximum Vbus Voltage" },
        VdoField { index: 11, mask: 0x00001800, description: "Cable Termination Type" },
        VdoField { index: 13, mask: 0x0001e000, description: "Cable Latency" },
        VdoField { index: 17, mask: 0x00020000, description: "Reserved" },
        VdoField { index: 18, mask: 0x000c0000, description: "USB Type-C Plug to USB Type" },
        VdoField { index: 20, mask: 0x00100000, description: "Reserved" },
        VdoField { index: 21, mask: 0x00e00000, description: "VDO Version" },
        VdoField { index: 24, mask: 0x0f000000, description: "Firmware Version" },
        VdoField { index: 28, mask: 0xf0000000, description: "HW Version" },
    ];

    pub const UFP_VDO1: &[VdoField] = &[
        VdoField { index: 0, mask: 0x00000007, description: "USB Highest Speed" },
        VdoField { index: 3, mask: 0x00000038, description: "Alternate Modes" },
        VdoField { index: 6, mask: 0x00ffffc0, description: "Reserved" },
        VdoField { index: 24, mask: 0x0f000000, description: "Device Capability" },
        VdoField { index: 28, mask: 0x10000000, description: "Reserved" },
        VdoField { index: 29, mask: 0xe0000000, description: "UFP VDO Version" },
    ];

    pub const UFP_VDO2: &[VdoField] = &[
        VdoField { index: 0, mask: 0x0000007f, description: "USB3 Max Power" },
        VdoField { index: 7, mask: 0x00003f80, description: "USB3 Min Power" },
        VdoField { index: 14, mask: 0x0000c000, description: "Reserved" },
        VdoField { index: 16, mask: 0x007f0000, description: "USB4 Max Power" },
        VdoField { index: 23, mask: 0x3f800000, description: "USB4 Min Power" },
        VdoField { index: 30, mask: 0xc0000000, description: "Reserved" },
    ];

    pub const DFP_VDO: &[VdoField] = &[
        VdoField { index: 0, mask: 0x0000001f, description: "Port Number" },
        VdoField { index: 5, mask: 0x00ffffe0, description: "Reserved" },
        VdoField { index: 24, mask: 0x07000000, description: "Host Capability" },
        VdoField { index: 27, mask: 0x18000000, description: "Reserved" },
        VdoField { index: 29, mask: 0xe0000000, description: "DFP VDO Version" },
        VdoField { index: 21, mask: 0x00e00000, description: "VDO Version" },
        VdoField { index: 24, mask: 0x0f000000, description: "Firmware Version" },
        VdoField { index: 28, mask: 0xf0000000, description: "HW Version" },
    ];

    pub const ACTIVE_VDO1: &[VdoField] = &[
        VdoField { index: 0, mask: 0x00000007, description: "USB Speed" },
        VdoField { index: 3, mask: 0x00000008, description: "SOP'' Controller Present" },
        VdoField { index: 4, mask: 0x00000010, description: "Vbus Through Cable" },
        VdoField { index: 5, mask: 0x00000060, description: "Vbus Current Handling" },
        VdoField { index: 7, mask: 0x00000080, description: "SBU Type" },
        VdoField { index: 8, mask: 0x00000100, description: "SBU Supported" },
        VdoField { index: 9, mask: 0x00000600, description: "Maximum Vbus Voltage" },
        VdoField { index: 11, mask: 0x00001800, description: "Cable Termination Type" },
        VdoField { index: 13, mask: 0x0001e000, description: "Cable Latency" },
        VdoField { index: 17, mask: 0x00020000, description: "Reserved" },
        VdoField { index: 18, mask: 0x000c0000, description: "Connector Type" },
        VdoField { index: 20, mask: 0x00100000, description: "Reserved" },
    ];

    pub const ACTIVE_VDO2: &[VdoField] = &[
        VdoField { index: 0, mask: 0x00000001, description: "USB Gen" },
        VdoField { index: 1, mask: 0x00000002, description: "Reserved" },
        VdoField { index: 2, mask: 0x00000004, description: "Optically Insulated Active Cable" },
        VdoField { index: 3, mask: 0x00000008, description: "USB Lanes Supported" },
        VdoField { index: 4, mask: 0x00000010, description: "USB 3.2 Supported" },
        VdoField { index: 5, mask: 0x00000020, description: "USB 2.0 Supported" },
        VdoField { index: 6, mask: 0x000000c0, description: "USB 2.0 Hub Hops Command" },
        VdoField { index: 8, mask: 0x00000100, description: "USB4 Supported" },
        VdoField { index: 9, mask: 0x00000200, description: "Active Element" },
        VdoField { index: 10, mask: 0x00000400, description: "Physical Connection" },
        VdoField { index: 11, mask: 0x00000800, description: "U3 to U0 Transition Mode" },
        VdoField { index: 12, mask: 0x00007000, description: "U3/CLd Power" },
        VdoField { index: 15, mask: 0x00008000, description: "Reserved" },
        VdoField { index: 16, mask: 0x00ff0000, description: "Shutdown Tempurature" },
        VdoField { index: 24, mask: 0xff000000, description: "Max Operating Tempurature" },
    ];
}

/// Constants specific to USB PD revision 3.1.
pub mod pd31_data {
    use super::VdoField;

    pub const HUB_COMP: u32 = 0x08000000;
    pub const PERIPHERAL_COMP: u32 = 0x10000000;
    pub const DFP_HUB_COMP: u32 = 0x00800000;
    pub const DFP_HOST_COMP: u32 = 0x01000000;
    pub const POWER_BRICK_COMP: u32 = 0x01800000;
    // Expected id_header field results.
    pub const PASSIVE_CABLE_COMP: u32 = 0x18000000;
    pub const ACTIVE_CABLE_COMP: u32 = 0x20000000;
    pub const VPD_COMP: u32 = 0x30000000;

    pub const ID_HEADER_VDO: &[VdoField] = &[
        VdoField { index: 0, mask: 0x0000ffff, description: "USB Vendor ID" },
        VdoField { index: 16, mask: 0x001f0000, description: "Reserved" },
        VdoField { index: 21, mask: 0x00600000, description: "Connector Type" },
        VdoField { index: 23, mask: 0x03800000, description: "Product Type (DFP)" },
        VdoField { index: 26, mask: 0x04000000, description: "Modal Operation Supported" },
        VdoField { index: 27, mask: 0x38000000, description: "Product Type (UFP/Cable Plug)" },
        VdoField { index: 30, mask: 0x40000000, description: "USB Capable as a USB Device" },
        VdoField { index: 31, mask: 0x80000000, description: "USB Capable as a USB Host" },
    ];

    pub const CERT_STAT_VDO: &[VdoField] =
        &[VdoField { index: 0, mask: 0xffffffff, description: "XID" }];

    pub const PRODUCT_VDO: &[VdoField] = &[
        VdoField { index: 0, mask: 0x0000ffff, description: "bcdDevice" },
        VdoField { index: 16, mask: 0xffff0000, description: "USB Product ID" },
    ];

    pub const UFP_VDO: &[VdoField] = &[
        VdoField { index: 0, mask: 0x00000007, description: "USB Highest Speed" },
        VdoField { index: 3, mask: 0x00000038, description: "Alternate Modes" },
        VdoField { index: 6, mask: 0x00000040, description: "Vbus Required" },
        VdoField { index: 7, mask: 0x00000080, description: "Vconn Required" },
        VdoField { index: 8, mask: 0x00000700, description: "Vconn Power" },
        VdoField { index: 11, mask: 0x003ff800, description: "Reserved" },
        VdoField { index: 22, mask: 0x00c00000, description: "Connector Type (Legacy)" },
        VdoField { index: 24, mask: 0x0f000000, description: "Device Capability" },
        VdoField { index: 28, mask: 0x10000000, description: "Reserved" },
        VdoField { index: 29, mask: 0xe0000000, description: "UFP VDO Version" },
    ];

    pub const DFP_VDO: &[VdoField] = &[
        VdoField { index: 0, mask: 0x0000001f, description: "Port Number" },
        VdoField { index: 5, mask: 0x003fffe0, description: "Reserved" },
        VdoField { index: 22, mask: 0x00c00000, description: "Connector Type (Legacy)" },
        VdoField { index: 24, mask: 0x07000000, description: "Host Capability" },
        VdoField { index: 27, mask: 0x18000000, description: "Reserved" },
        VdoField { index: 29, mask: 0xe0000000, description: "DFP VDO Version" },
    ];

    pub const PASSIVE_VDO: &[VdoField] = &[
        VdoField { index: 0, mask: 0x00000007, description: "USB Speed" },
        VdoField { index: 3, mask: 0x00000018, description: "Reserved" },
        VdoField { index: 5, mask: 0x00000060, description: "Vbus Current Handling" },
        VdoField { index: 7, mask: 0x00000180, description: "Reserved" },
        VdoField { index: 9, mask: 0x00000600, description: "Maximum Vbus Voltage" },
        VdoField { index: 11, mask: 0x00001800, description: "Cable Termination Type" },
        VdoField { index: 13, mask: 0x0001e000, description: "Cable Latency" },
        VdoField { index: 17, mask: 0x00020000, description: "EPR Mode Cable" },
        VdoField { index: 18, mask: 0x000c0000, description: "USB Type-C Plug to USB Type" },
        VdoField { index: 20, mask: 0x00100000, description: "Reserved" },
        VdoField { index: 21, mask: 0x00e00000, description: "VDO Version" },
        VdoField { index: 24, mask: 0x0f000000, description: "Firmware Version" },
        VdoField { index: 28, mask: 0xf0000000, description: "HW Version" },
    ];

    pub const ACTIVE_VDO1: &[VdoField] = &[
        VdoField { index: 0, mask: 0x00000007, description: "USB Speed" },
        VdoField { index: 3, mask: 0x00000008, description: "SOP'' Controller Present" },
        VdoField { index: 4, mask: 0x00000010, description: "Vbus Through Cable" },
        VdoField { index: 5, mask: 0x00000060, description: "Vbus Current Handling" },
        VdoField { index: 7, mask: 0x00000080, description: "SBU Type" },
        VdoField { index: 8, mask: 0x00000100, description: "SBU Supported" },
        VdoField { index: 9, mask: 0x00000600, description: "Maximum Vbus Voltage" },
        VdoField { index: 11, mask: 0x00001800, description: "Cable Termination Type" },
        VdoField { index: 13, mask: 0x0001e000, description: "Cable Latency" },
        VdoField { index: 17, mask: 0x00020000, description: "EPR Mode Cable" },
        VdoField { index: 18, mask: 0x000c0000, description: "USB Type-C Plug to USB Type" },
        VdoField { index: 20, mask: 0x00100000, description: "Reserved" },
        VdoField { index: 21, mask: 0x00e00000, description: "VDO Version" },
        VdoField { index: 24, mask: 0x0f000000, description: "Firmware Version" },
        VdoField { index: 28, mask: 0xf0000000, description: "HW Version" },
    ];

    pub const ACTIVE_VDO2: &[VdoField] = &[
        VdoField { index: 0, mask: 0x00000001, description: "USB Gen" },
        VdoField { index: 1, mask: 0x00000002, description: "Reserved" },
        VdoField { index: 2, mask: 0x00000004, description: "Optically Insulated Active Cable" },
        VdoField { index: 3, mask: 0x00000008, description: "USB Lanes Supported" },
        VdoField { index: 4, mask: 0x00000010, description: "USB 3.2 Supported" },
        VdoField { index: 5, mask: 0x00000020, description: "USB 2.0 Supported" },
        VdoField { index: 6, mask: 0x000000c0, description: "USB 2.0 Hub Hops Command" }, // Corrected mask
        VdoField { index: 8, mask: 0x00000100, description: "USB4 Supported" },
        VdoField { index: 9, mask: 0x00000200, description: "Active Element" },
        VdoField { index: 10, mask: 0x00000400, description: "Physical Connection" },
        VdoField { index: 11, mask: 0x00000800, description: "U3 to U0 Transition Mode" },
        VdoField { index: 12, mask: 0x00007000, description: "U3/CLd Power" },
        VdoField { index: 15, mask: 0x00008000, description: "Reserved" },
        VdoField { index: 16, mask: 0x00ff0000, description: "Shutdown Tempurature" },
        VdoField { index: 24, mask: 0xff000000, description: "Max Operating Tempurature" },
    ];

    pub const VPD_VDO: &[VdoField] = &[
        VdoField { index: 0, mask: 0x00000001, description: "Charge Through Support" },
        VdoField { index: 1, mask: 0x0000007e, description: "Ground Impedance" },
        VdoField { index: 7, mask: 0x00001f80, description: "Vbus Impedance" },
        VdoField { index: 13, mask: 0x00002000, description: "Reserved" },
        VdoField { index: 14, mask: 0x00004000, description: "Charge Through Current Support" },
        VdoField { index: 15, mask: 0x00018000, description: "Maximum Vbus Voltage" },
        VdoField { index: 17, mask: 0x001e0000, description: "Reserved" },
        VdoField { index: 21, mask: 0x00e00000, description: "VDO Version" },
        VdoField { index: 24, mask: 0x0f000000, description: "Firmware Version" },
        VdoField { index: 28, mask: 0xf0000000, description: "HW Version" },
    ];
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_decode_vdo() {
        let vdo: u32 = 0x12341234;
        let field = VdoField {
            index: 4,
            mask: 0x000000f0,
            description: "Value of the 2nd least significant byte",
        };
        assert_eq!(field.decode_vdo(vdo), "Value of the 2nd least significant byte: 0x3")
    }
}
