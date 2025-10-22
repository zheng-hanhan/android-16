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

//! typec_class_utils

use crate::usb_pd_utils::*;
use anyhow::{bail, Context, Result};
use log::warn;
use regex::Regex;
use std::{
    fs,
    io::Write,
    path::{Path, PathBuf},
};

pub const TYPEC_SYSFS_PATH: &str = "/sys/class/typec";

pub const MODE_REGEX: &str = "^mode[0-9]+$";
pub const PLUG_ALT_MODE_REGEX: &str = "^port[0-9]+\\-plug[0-9]+\\.[0-9]+$";
pub const PLUG_REGEX: &str = "^port[0-9]+\\-plug[0-9]+$";
pub const PORT_REGEX: &str = "^port[0-9]+$";
pub const PARTNER_ALT_MODE_REGEX: &str = "^port[0-9]+-partner\\.[0-9]+$";
pub const PARTNER_PDO_REGEX: &str = "^pd[0-9]+$";
pub const PDO_CAPABILITIES_REGEX: &str = "^(sink|source)-capabilities$";
pub const PDO_TYPE_REGEX: &str =
    "^[0-9]+:(battery|fixed_supply|programmable_supply|variable_supply)$";
pub const USB_PORT_REGEX: &str = "^usb[0-9]+\\-port[0-9]+$";
pub const USB_DEVICE_REGEX: &str = "^[0-9]+\\-[0-9]+(\\.[0-9]+)*$";

pub const INDENT_STEP: usize = 2;

const ERR_READ_DIR: &str = "Failed to iterate over files in directory";
const ERR_READ_FILE: &str = "Cannot read file from path";
const ERR_FILE_NAME_FROM_PATH: &str = "Cannot get file name at path";
const ERR_FILE_NAME_UTF_INVALID: &str = "The file name is UTF-8 invalid at path";
const ERR_DIR_ENTRY_ACCESS: &str = "Cannot access entry in directory";
const ERR_WRITE_TO_BUFFER: &str = "Cannot write to output buffer. Attempted to print";
const ERR_PATH_NOT_DIR_OR_SYMLINK: &str = "The path is not a directory or a symbolic link";

trait WarnErr {
    fn warn_err(self);
}

impl<T> WarnErr for Result<T> {
    /// In case the result is an error, logs the error message as a warning.
    fn warn_err(self) {
        if let Err(err) = self {
            warn!("{err:#}");
        }
    }
}

pub struct OutputWriter<W: Write> {
    buffer: W,
    indent: usize,
}

impl<W: Write> OutputWriter<W> {
    pub fn new(buffer: W, indent: usize) -> Self {
        Self { buffer, indent }
    }

    /// Prints a string with indentation.
    fn print_str_indent(&mut self, str_to_print: &str) -> Result<()> {
        writeln!(self.buffer, "{:indent$}{str_to_print}", "", indent = self.indent)
            .with_context(|| format!("{ERR_WRITE_TO_BUFFER} {:?}", str_to_print))?;
        Ok(())
    }

    /// Prints a file's contents in a "name: content" format and also adds indentation to multiline strings.
    fn print_file_formatted(&mut self, file_path: &Path) -> Result<()> {
        let file_name = get_path_basename(file_path)?;
        let file_contents = fs::read_to_string(file_path)
            .with_context(|| format!("{ERR_READ_FILE} {:?}", file_path))?;
        let formatted_contents = file_contents
            .trim_ascii_end()
            .replace("\n", format!("\n{:indent$}", "", indent = self.indent).as_str());
        writeln!(
            self.buffer,
            "{:indent$}{file_name}: {formatted_contents}",
            "",
            indent = self.indent
        )
        .with_context(|| format!("{ERR_WRITE_TO_BUFFER} {:?}", formatted_contents))?;
        Ok(())
    }

    /// Prints the name of the directory followed by all the files in it in a "name: content" format.
    /// The files are sorted alphabetically by path to ensure consistency.
    pub fn print_dir_files(&mut self, dir_path: &Path) -> Result<()> {
        let dir_name = get_path_basename(dir_path)?;
        self.print_str_indent(dir_name)?;

        for path in get_sorted_paths_from_dir(dir_path, |path| path.is_file())? {
            self.indent += INDENT_STEP;
            self.print_file_formatted(&path).warn_err();
            self.indent -= INDENT_STEP;
        }

        Ok(())
    }
}

/// Retrieves a sorted vector of paths in the given directory. Filters the paths according to `path_filter`.
/// Logs the errors and ignores faulty entries/paths.
fn get_sorted_paths_from_dir(
    dir_path: &Path,
    path_filter: fn(&PathBuf) -> bool,
) -> Result<Vec<PathBuf>> {
    let mut sorted_paths: Vec<PathBuf> = fs::read_dir(dir_path)
        .with_context(|| format!("{ERR_READ_DIR} {:?}", dir_path))?
        .filter_map(|entry| {
            entry
                .inspect_err(|err| warn!("{ERR_DIR_ENTRY_ACCESS} {:?}: {err}", dir_path))
                .map(|entry| entry.path())
                .ok()
        })
        .filter(path_filter)
        .collect();
    sorted_paths.sort();
    Ok(sorted_paths)
}

/// Returns the rightmost element of a path.
fn get_path_basename(path: &Path) -> Result<&str> {
    let basename = path
        .file_name()
        .with_context(|| format!("{ERR_FILE_NAME_FROM_PATH} {:?}", path))?
        .to_str()
        .with_context(|| format!("{ERR_FILE_NAME_UTF_INVALID} {:?}", path))?;
    Ok(basename)
}

/// Decodes and prints VDOs from the files specified relative to `identity_path_buf` path.
/// * `filename_vdo_fields_arr` - Is an array of tuples containing the name of the file where the VDO can be found and the VDO description array (`&[VdoField]`).
pub fn print_decoded_vdos_from_files<W: Write>(
    identity_path_buf: &mut PathBuf,
    filename_vdo_fields_arr: &[(&str, &[VdoField])],
    out_writer: &mut OutputWriter<W>,
) {
    for (filename, vdo_fields) in filename_vdo_fields_arr {
        identity_path_buf.push(filename);
        print_vdo(identity_path_buf, vdo_fields, out_writer).warn_err();
        identity_path_buf.pop();
    }
}

/// Prints the identity information of a USB device.
/// This function reads and decodes the identity descriptors of a USB device,
/// including its ID header, certification status, and product information.
/// The output is formatted and written using the provided `OutputWriter`.
/// # Arguments
/// * `dev_path` - A reference to the Path of the device's directory.
/// * `out_writer` - A mutable reference to an OutputWriter for writing the output.
/// # Returns
/// * `Result<()>` - A Result indicating success or failure of the printing operation.
pub fn print_identity<W: Write>(dev_path: &Path, out_writer: &mut OutputWriter<W>) -> Result<()> {
    out_writer.print_str_indent("identity").warn_err();

    out_writer.indent += INDENT_STEP;

    let filename_vdo_fields_arr: &[(&str, &[VdoField]); 3] = match get_pd_revision(dev_path) {
        PdRev::Pd20 => &[
            ("id_header", pd20_data::ID_HEADER_VDO),
            ("cert_stat", pd20_data::CERT_STAT_VDO),
            ("product", pd20_data::PRODUCT_VDO),
        ],
        PdRev::Pd30 => &[
            ("id_header", pd30_data::ID_HEADER_VDO),
            ("cert_stat", pd30_data::CERT_STAT_VDO),
            ("product", pd30_data::PRODUCT_VDO),
        ],
        PdRev::Pd31 => &[
            ("id_header", pd31_data::ID_HEADER_VDO),
            ("cert_stat", pd31_data::CERT_STAT_VDO),
            ("product", pd31_data::PRODUCT_VDO),
        ],
        PdRev::None => &[("id_header", &[]), ("cert_stat", &[]), ("product", &[])],
    };
    print_decoded_vdos_from_files(
        &mut dev_path.join("identity"),
        filename_vdo_fields_arr,
        out_writer,
    );
    out_writer.indent -= INDENT_STEP;

    Ok(())
}

/// Prints the contents of an identity directory including VDO fields which are determined by product type.
pub fn print_partner_identity<W: Write>(
    partner_path: &Path,
    out_writer: &mut OutputWriter<W>,
) -> Result<()> {
    let mut identity_path_buf: PathBuf = partner_path.join("identity");
    if !(identity_path_buf.is_dir() | identity_path_buf.is_symlink()) {
        bail!("{ERR_PATH_NOT_DIR_OR_SYMLINK}: {:?}", identity_path_buf);
    }

    print_identity(partner_path, out_writer).warn_err();

    out_writer.indent += INDENT_STEP;

    let pd_rev = get_pd_revision(partner_path);

    let vdo_fields_arr: [&[VdoField]; 3] = match get_partner_product_type(partner_path, pd_rev) {
        ProductType::Ama(PdRev::Pd20) => [pd20_data::AMA_VDO, &[], &[]],
        ProductType::Vpd(PdRev::Pd30) => [pd30_data::VPD_VDO, &[], &[]],
        ProductType::Ama(PdRev::Pd30) => [pd30_data::AMA_VDO, &[], &[]],
        ProductType::Port((PortType::Ufp, PdRev::Pd30)) => {
            [pd30_data::UFP_VDO1, pd30_data::UFP_VDO2, &[]]
        }
        ProductType::Port((PortType::Dfp, PdRev::Pd30)) => [pd30_data::DFP_VDO, &[], &[]],
        ProductType::Port((PortType::Drd, PdRev::Pd30)) => {
            [pd30_data::UFP_VDO1, pd30_data::UFP_VDO2, pd30_data::DFP_VDO]
        }
        ProductType::Port((PortType::Ufp, PdRev::Pd31)) => [pd31_data::UFP_VDO, &[], &[]],
        ProductType::Port((PortType::Dfp, PdRev::Pd31)) => [pd31_data::DFP_VDO, &[], &[]],
        ProductType::Port((PortType::Drd, PdRev::Pd31)) => {
            [pd31_data::UFP_VDO, &[], pd31_data::DFP_VDO]
        }
        _ => [&[], &[], &[]],
    };
    let filenames = ["product_type_vdo1", "product_type_vdo2", "product_type_vdo3"];
    let filename_vdo_fields_arr: Vec<(&str, &[VdoField])> =
        filenames.into_iter().zip(vdo_fields_arr).collect();

    print_decoded_vdos_from_files(&mut identity_path_buf, &filename_vdo_fields_arr, out_writer);

    out_writer.indent -= INDENT_STEP;

    Ok(())
}

/// Reads the usb_power_delivery_revision file in a given directory
/// and returns a PdRev enum noting the supported PD Revision.
pub fn get_pd_revision(dir_path: &Path) -> PdRev {
    let pd_path: PathBuf = dir_path.join("usb_power_delivery_revision");
    let pd_revision_str = fs::read_to_string(&pd_path)
        .inspect_err(|err| warn!("{ERR_READ_FILE} {:?} {:#}", &pd_path, err))
        .unwrap_or_default();
    match pd_revision_str.trim_ascii() {
        "2.0" => PdRev::Pd20,
        "3.0" => PdRev::Pd30,
        "3.1" => PdRev::Pd31,
        _ => PdRev::None,
    }
}

/// Uses the id_header VDO and USB PD revision to decode what type of device is being parsed.
/// In case of failure returns ProductType::Other.
pub fn get_partner_product_type(partner_dir: &Path, pd_rev: PdRev) -> ProductType {
    let id_header_vdo_path: PathBuf = partner_dir.join("identity").join("id_header");

    let id_header_vdo = match read_vdo(&id_header_vdo_path) {
        Ok(vdo) => vdo,
        Err(err) => {
            warn!("{:#}", err);
            return ProductType::Other;
        }
    };

    match pd_rev {
        PdRev::Pd20 => {
            // Alternate Mode Adapter (AMA) is the only partner product type in the
            // USB PD 2.0 specification.
            match id_header_vdo & UFP_PRODUCT_TYPE_MASK {
                pd20_data::AMA_COMP => ProductType::Ama(PdRev::Pd20),
                _ => ProductType::Other,
            }
        }
        PdRev::Pd30 => {
            // In USB PD 3.0 a partner can be an upstream facing port (UFP),
            // downstream facing port (DFP), or a dual-role data port (DRD).
            // Information about UFP/DFP are in different fields, so they are checked
            // separately then compared to determine a partner's product type.
            // Separate from UFP/DFP, they can support AMA/VPD as a UFP type.

            let ufp_supported = match id_header_vdo & UFP_PRODUCT_TYPE_MASK {
                pd30_data::HUB_COMP => true,
                pd30_data::PERIPHERAL_COMP => true,
                pd30_data::AMA_COMP => return ProductType::Ama(PdRev::Pd30),
                pd30_data::VPD_COMP => return ProductType::Vpd(PdRev::Pd30),
                _ => false,
            };

            let dfp_supported = matches!(
                id_header_vdo & DFP_PRODUCT_TYPE_MASK,
                pd30_data::DFP_HUB_COMP | pd30_data::DFP_HOST_COMP | pd30_data::POWER_BRICK_COMP
            );

            match (ufp_supported, dfp_supported) {
                (true, true) => ProductType::Port((PortType::Drd, PdRev::Pd30)),
                (true, false) => ProductType::Port((PortType::Ufp, PdRev::Pd30)),
                (false, true) => ProductType::Port((PortType::Dfp, PdRev::Pd30)),
                _ => ProductType::Other,
            }
        }
        PdRev::Pd31 => {
            // Similar to USB PD 3.0, USB PD 3.1 can have a partner which is both UFP
            // and DFP (DRD). AMA product type is no longer present in PD 3.1.
            let ufp_supported = matches!(
                id_header_vdo & UFP_PRODUCT_TYPE_MASK,
                pd31_data::HUB_COMP | pd31_data::PERIPHERAL_COMP
            );

            let dfp_supported = matches!(
                id_header_vdo & DFP_PRODUCT_TYPE_MASK,
                pd31_data::DFP_HOST_COMP | pd31_data::DFP_HUB_COMP | pd31_data::POWER_BRICK_COMP
            );

            match (ufp_supported, dfp_supported) {
                (true, true) => ProductType::Port((PortType::Drd, PdRev::Pd31)),
                (true, false) => ProductType::Port((PortType::Ufp, PdRev::Pd31)),
                (false, true) => ProductType::Port((PortType::Dfp, PdRev::Pd31)),
                _ => ProductType::Other,
            }
        }
        _ => ProductType::Other,
    }
}

/// Reads a vdo value from a text file and converts it to a u32 variable. Then, prints out the values of each field according to the vdo_description.
pub fn print_vdo<W: Write>(
    vdo_path: &Path,
    vdo_description: &[VdoField],
    out_writer: &mut OutputWriter<W>,
) -> Result<()> {
    let vdo = read_vdo(vdo_path)?;
    let vdo_str = format!("{}: 0x{:x}", get_path_basename(vdo_path)?, vdo);
    out_writer.print_str_indent(&vdo_str).warn_err();
    out_writer.indent += INDENT_STEP;
    for vdo_field in vdo_description {
        out_writer.print_str_indent(&vdo_field.decode_vdo(vdo)).warn_err(); // log error but continue iterating.
    }
    out_writer.indent -= INDENT_STEP;
    Ok(())
}

/// Reads a file containing a 32 bit VDO value and loads it into a u32.
pub fn read_vdo(vdo_path: &Path) -> Result<u32> {
    let vdo_str =
        fs::read_to_string(vdo_path).with_context(|| format!("{ERR_READ_FILE} {:?}", vdo_path))?;
    let vdo_str = vdo_str.trim_ascii();
    let vdo: u32 =
        u32::from_str_radix(vdo_str.trim_start_matches("0x"), 16).with_context(|| {
            format!("Cannot convert string read from path: {:?} to u32: {vdo_str}", vdo_path)
        })?;
    Ok(vdo)
}

// Prints the immediate files in an alternate mode directory, then prints the files in each mode subdirectory.
pub fn print_alt_mode<W: Write>(alt_mode_dir_path: &Path, out_writer: &mut OutputWriter<W>) {
    if !(alt_mode_dir_path.is_dir() | alt_mode_dir_path.is_symlink()) {
        warn!("{ERR_PATH_NOT_DIR_OR_SYMLINK}: {:?}", alt_mode_dir_path);
        return;
    }

    out_writer.print_dir_files(alt_mode_dir_path).warn_err();
    out_writer.indent += INDENT_STEP;
    parse_dirs_and_execute(
        alt_mode_dir_path,
        out_writer,
        MODE_REGEX,
        |path: &Path, out_writer: &mut OutputWriter<W>| {
            out_writer.print_dir_files(path).warn_err();
        },
    )
    .warn_err();
    out_writer.indent -= INDENT_STEP;
}

// Prints detailed information about the PDOs given at capabilities_dir_path, including available voltages and currents.
pub fn print_pdo_capabilities<W: Write>(capabilities: &Path, out_writer: &mut OutputWriter<W>) {
    out_writer.print_dir_files(capabilities).warn_err();
    out_writer.indent += INDENT_STEP;
    parse_dirs_and_execute(
        capabilities,
        out_writer,
        PDO_TYPE_REGEX,
        |path: &Path, out_writer: &mut OutputWriter<W>| {
            out_writer.print_dir_files(path).warn_err();
        },
    )
    .warn_err();
    out_writer.indent -= INDENT_STEP;
}

// Prints the immediate files in a PDO data directory, then call
// print_pdo_capabilities to print more detailed PDO information.
pub fn print_pdos<W: Write>(pdo_dir_path: &Path, out_writer: &mut OutputWriter<W>) {
    if !(pdo_dir_path.is_dir() | pdo_dir_path.is_symlink()) {
        warn!("{ERR_PATH_NOT_DIR_OR_SYMLINK}: {:?}", pdo_dir_path);
        return;
    }

    out_writer.print_dir_files(pdo_dir_path).warn_err();
    out_writer.indent += INDENT_STEP;
    parse_dirs_and_execute(
        pdo_dir_path,
        out_writer,
        PDO_CAPABILITIES_REGEX,
        print_pdo_capabilities,
    )
    .warn_err();
    out_writer.indent -= INDENT_STEP;
}

/// Prints the immediate information in the partner directory, then prints identity, alternate mode, and power data objects (PDO) information.
pub fn print_partner<W: Write>(port_path: &Path, out_writer: &mut OutputWriter<W>) -> Result<()> {
    let partner_dir_name = [get_path_basename(port_path)?, "-partner"].concat();
    let partner_path_buf: PathBuf = port_path.join(partner_dir_name);

    if !(partner_path_buf.is_dir() | partner_path_buf.is_symlink()) {
        bail!("{ERR_PATH_NOT_DIR_OR_SYMLINK}: {:?}", partner_path_buf);
    }

    out_writer.print_dir_files(&partner_path_buf).warn_err();

    out_writer.indent += INDENT_STEP;

    print_partner_identity(&partner_path_buf, out_writer).warn_err();

    parse_dirs_and_execute(&partner_path_buf, out_writer, PARTNER_ALT_MODE_REGEX, print_alt_mode)
        .warn_err();

    parse_dirs_and_execute(&partner_path_buf, out_writer, PARTNER_PDO_REGEX, print_pdos).warn_err();

    out_writer.indent -= INDENT_STEP;
    Ok(())
}

/// Determines the product type of a USB cable by reading its identity descriptors.
/// This function reads the "id_header" file within the "identity" directory of the cable's path
/// to determine its product type based on the USB PD revision and product mask.
/// # Arguments
/// * `dir` - A reference to the Path of the cable's directory.
/// # Returns
/// * `ProductType` - The determined product type of the cable.
pub fn get_cable_product_type(dir: &Path) -> ProductType {
    let id_header = match read_vdo(&dir.join("identity").join("id_header")) {
        Err(err) => {
            warn!("get cable type failed to read vdo: {:#}", err);
            return ProductType::Other;
        }
        Ok(id_header) => id_header,
    };

    let pd = get_pd_revision(dir);
    let prod_mask = id_header & UFP_PRODUCT_TYPE_MASK;

    // All USB PD 3.1/3.0/2.0 support passive cables.
    if pd == PdRev::Pd20 && prod_mask == pd20_data::PASSIVE_CABLE_COMP
        || pd == PdRev::Pd30 && prod_mask == pd30_data::PASSIVE_CABLE_COMP
        || pd == PdRev::Pd31 && prod_mask == pd31_data::PASSIVE_CABLE_COMP
    {
        return ProductType::Cable((CableType::Passive, pd));
    }

    // All USB PD 3.1/3.0/2.0 support active cables.
    if pd == PdRev::Pd20 && prod_mask == pd20_data::ACTIVE_CABLE_COMP
        || pd == PdRev::Pd30 && prod_mask == pd30_data::ACTIVE_CABLE_COMP
        || pd == PdRev::Pd31 && prod_mask == pd31_data::ACTIVE_CABLE_COMP
    {
        return ProductType::Cable((CableType::Active, pd));
    }

    // USB PD 3.1 also supports Vconn Powered Devices (VPD).
    if pd == PdRev::Pd31 && prod_mask == pd31_data::VPD_COMP {
        return ProductType::Cable((CableType::Vpd, pd));
    }

    ProductType::Other
}

/// Prints the identity information of a USB cable.
/// This function prints the identity information of a USB cable, including its product type and
/// decoded VDOs (Vendor-Defined Objects) based on its PD revision.
/// # Arguments
/// * `cable` - A reference to the Path of the cable's directory.
/// * `out_writer` - A mutable reference to an OutputWriter for writing the output.
pub fn print_cable_identity<W: Write>(cable: &Path, out_writer: &mut OutputWriter<W>) {
    let mut identity = cable.join("identity");
    if !(identity.is_dir() | identity.is_symlink()) {
        warn!("{ERR_PATH_NOT_DIR_OR_SYMLINK}: {:?}", identity);
        return;
    }

    print_identity(cable, out_writer).warn_err();

    out_writer.indent += INDENT_STEP;

    let vdos: [&[VdoField]; 3] = match get_cable_product_type(cable) {
        ProductType::Cable(cable_type) => match cable_type {
            (CableType::Passive, PdRev::Pd20) => [pd20_data::PASSIVE_VDO, &[], &[]],
            (CableType::Active, PdRev::Pd20) => [pd20_data::ACTIVE_VDO, &[], &[]],
            (CableType::Passive, PdRev::Pd30) => [pd30_data::PASSIVE_VDO, &[], &[]],
            (CableType::Active, PdRev::Pd30) => {
                [pd30_data::ACTIVE_VDO1, pd30_data::ACTIVE_VDO2, &[]]
            }
            (CableType::Passive, PdRev::Pd31) => [pd31_data::PASSIVE_VDO, &[], &[]],
            (CableType::Active, PdRev::Pd31) => {
                [pd31_data::ACTIVE_VDO1, pd31_data::ACTIVE_VDO2, &[]]
            }
            (CableType::Vpd, PdRev::Pd31) => [pd31_data::VPD_VDO, &[], &[]],
            _ => [&[], &[], &[]],
        },
        _ => [&[], &[], &[]],
    };

    let descriptions = ["product_type_vdo1", "product_type_vdo2", "product_type_vdo3"];
    let to_print: Vec<(&str, &[VdoField])> = descriptions.into_iter().zip(vdos).collect();

    print_decoded_vdos_from_files(&mut identity, &to_print, out_writer);

    out_writer.indent -= INDENT_STEP;
}

/// Prints information about a USB cable plug.
/// This function prints information about a USB cable plug, including its alternate modes.
/// # Arguments
/// * `plug` - A reference to the Path of the plug's directory.
/// * `out_writer` - A mutable reference to an OutputWriter for writing the output.
pub fn print_plug_info<W: Write>(plug: &Path, out_writer: &mut OutputWriter<W>) {
    if !(plug.is_dir() | plug.is_symlink()) {
        warn!("{ERR_PATH_NOT_DIR_OR_SYMLINK}: {:?}", plug);
        return;
    }

    out_writer.print_dir_files(plug).warn_err();
    out_writer.indent += INDENT_STEP;
    parse_dirs_and_execute(plug, out_writer, PLUG_ALT_MODE_REGEX, print_alt_mode).warn_err();
    out_writer.indent -= INDENT_STEP;
}

/// Prints information about a USB cable connected to a port.
/// This function prints information about a USB cable connected to a port, including its identity,
/// plug information, and alternate modes.
/// # Arguments
/// * `port` - A reference to the Path of the port's directory.
/// * `out_writer` - A mutable reference to an OutputWriter for writing the output.
/// # Returns
/// * `Result<()>` - A Result indicating success or failure.
pub fn print_cable<W: Write>(port: &Path, out_writer: &mut OutputWriter<W>) -> Result<()> {
    let cable_path = format!("{}-cable", get_path_basename(port)?);
    let cable_dir = port.join(cable_path);

    if !(cable_dir.is_dir() | cable_dir.is_symlink()) {
        bail!("{ERR_PATH_NOT_DIR_OR_SYMLINK}: {:?}", cable_dir);
    }

    out_writer.print_dir_files(&cable_dir).warn_err();

    out_writer.indent += INDENT_STEP;
    print_cable_identity(&cable_dir, out_writer);
    parse_dirs_and_execute(&cable_dir, out_writer, PLUG_REGEX, print_plug_info).warn_err();
    out_writer.indent -= INDENT_STEP;

    Ok(())
}

/// Prints the panel and horizontal_position in the physical_location directory of a port.
pub fn print_physical_location<W: Write>(
    port_path: &Path,
    out_writer: &mut OutputWriter<W>,
) -> Result<()> {
    let physical_location_dir = port_path.join("physical_location");
    if !(physical_location_dir.is_dir() | physical_location_dir.is_symlink()) {
        bail!("{ERR_PATH_NOT_DIR_OR_SYMLINK}: {:?}", physical_location_dir);
    }
    out_writer.print_str_indent("physical_location")?;

    out_writer.indent += INDENT_STEP;
    out_writer.print_file_formatted(&physical_location_dir.join("panel"))?;
    out_writer.print_file_formatted(&physical_location_dir.join("horizontal_position"))?;
    out_writer.indent -= INDENT_STEP;

    Ok(())
}

// Prints the `busnum`, `devnum`, `devpath` in the usb device directory, which are minimal
// info needed to map to corresponding peripheral.
pub fn print_usb_device_info<W: Write>(usb_device: &Path, out_writer: &mut OutputWriter<W>) {
    out_writer.print_str_indent("usb_device").warn_err();

    out_writer.indent += INDENT_STEP;
    out_writer.print_file_formatted(&usb_device.join("busnum")).warn_err();
    out_writer.print_file_formatted(&usb_device.join("devnum")).warn_err();
    out_writer.print_file_formatted(&usb_device.join("devpath")).warn_err();
    parse_dirs_and_execute(usb_device, out_writer, USB_DEVICE_REGEX, print_usb_device_info)
        .warn_err();
    out_writer.indent -= INDENT_STEP;
}

// Finds and prints information about the usb device in the usb port directory.
pub fn print_usb_device<W: Write>(usb_port: &Path, out_writer: &mut OutputWriter<W>) {
    let usb_device_dir = usb_port.join("device");
    if !(usb_device_dir.is_dir() | usb_device_dir.is_symlink()) {
        warn!("{ERR_PATH_NOT_DIR_OR_SYMLINK}: {:?}", usb_device_dir);
        return;
    }

    print_usb_device_info(&usb_device_dir, out_writer);
}

/// Finds the type-c port's linked usb ports and print relevant usb subsystem information.
pub fn print_usb_subsystem<W: Write>(
    port_path: &Path,
    out_writer: &mut OutputWriter<W>,
) -> Result<()> {
    parse_dirs_and_execute(port_path, out_writer, USB_PORT_REGEX, print_usb_device).warn_err();
    Ok(())
}

/// Prints `connector_id` and `status` in the drm_connector directory which is a symlink
/// to the corresponding DP connector.
pub fn print_drm_subsystem<W: Write>(
    port_path: &Path,
    out_writer: &mut OutputWriter<W>,
) -> Result<()> {
    let drm_path = port_path.join("drm_connector");
    if !(drm_path.is_dir() | drm_path.is_symlink()) {
        bail!("{ERR_PATH_NOT_DIR_OR_SYMLINK}: {:?}", drm_path);
    }
    out_writer.print_str_indent("dp_connector")?;

    out_writer.indent += INDENT_STEP;
    out_writer.print_file_formatted(&drm_path.join("connector_id"))?;
    out_writer.print_file_formatted(&drm_path.join("status"))?;
    out_writer.indent -= INDENT_STEP;

    Ok(())
}

/// Prints relevant type-c connector class information for the port located at the sysfs path "port_path".
pub fn print_port_info<W: Write>(port_path: &Path, out_writer: &mut OutputWriter<W>) {
    out_writer.print_dir_files(port_path).warn_err();
    out_writer.indent += INDENT_STEP;
    print_partner(port_path, out_writer).warn_err();
    print_cable(port_path, out_writer).warn_err();
    print_physical_location(port_path, out_writer).warn_err();
    print_usb_subsystem(port_path, out_writer).warn_err();
    print_drm_subsystem(port_path, out_writer).warn_err();
    out_writer.indent -= INDENT_STEP;
}

/// Looks at subdirectories of a given directory and executes a passed function on directories matching a given regular expression.
/// Directories are sorted alphabetically by path to ensure consistency in the order of processing.
pub fn parse_dirs_and_execute<W: Write>(
    dir_path: &Path,
    out_writer: &mut OutputWriter<W>,
    regex: &str,
    func: fn(&Path, &mut OutputWriter<W>),
) -> Result<()> {
    let re = Regex::new(regex).unwrap();
    for path in get_sorted_paths_from_dir(dir_path, |path| path.is_dir() | path.is_symlink())? {
        // Assumption: all the symlinks created by the Type-C connector class point to directories.
        get_path_basename(&path)
            .inspect_err(|err| warn!("{:#}", err))
            .is_ok_and(|file_name| re.is_match(file_name))
            .then(|| func(&path, out_writer));
    }
    Ok(())
}
