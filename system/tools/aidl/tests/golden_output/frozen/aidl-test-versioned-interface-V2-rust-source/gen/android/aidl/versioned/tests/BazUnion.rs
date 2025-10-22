/*
 * This file is auto-generated.  DO NOT MODIFY.
 * Using: out/host/linux-x86/bin/aidl --lang=rust --structured --version 2 --hash da8c4bc94ca7feff0e0a65563a466787698b5891 --min_sdk_version current --ninja -d out/soong/.intermediates/system/tools/aidl/aidl-test-versioned-interface-V2-rust-source/gen/android/aidl/versioned/tests/BazUnion.rs.d -o out/soong/.intermediates/system/tools/aidl/aidl-test-versioned-interface-V2-rust-source/gen -Nsystem/tools/aidl/aidl_api/aidl-test-versioned-interface/2 system/tools/aidl/aidl_api/aidl-test-versioned-interface/2/android/aidl/versioned/tests/BazUnion.aidl
 *
 * DO NOT CHECK THIS FILE INTO A CODE TREE (e.g. git, etc..).
 * ALWAYS GENERATE THIS FILE FROM UPDATED AIDL COMPILER
 * AS A BUILD INTERMEDIATE ONLY. THIS IS NOT SOURCE CODE.
 */
#![forbid(unsafe_code)]
#![cfg_attr(rustfmt, rustfmt_skip)]
#[derive(Debug)]
pub enum r#BazUnion {
  IntNum(i32),
  LongNum(i64),
}
impl Default for r#BazUnion {
  fn default() -> Self {
    Self::IntNum(0)
  }
}
impl binder::Parcelable for r#BazUnion {
  fn write_to_parcel(&self, parcel: &mut binder::binder_impl::BorrowedParcel) -> std::result::Result<(), binder::StatusCode> {
    match self {
      Self::IntNum(v) => {
        parcel.write(&0i32)?;
        parcel.write(v)
      }
      Self::LongNum(v) => {
        parcel.write(&1i32)?;
        parcel.write(v)
      }
    }
  }
  fn read_from_parcel(&mut self, parcel: &binder::binder_impl::BorrowedParcel) -> std::result::Result<(), binder::StatusCode> {
    let tag: i32 = parcel.read()?;
    match tag {
      0 => {
        let value: i32 = parcel.read()?;
        *self = Self::IntNum(value);
        Ok(())
      }
      1 => {
        let value: i64 = parcel.read()?;
        *self = Self::LongNum(value);
        Ok(())
      }
      _ => {
        Err(binder::StatusCode::BAD_VALUE)
      }
    }
  }
}
binder::impl_serialize_for_parcelable!(r#BazUnion);
binder::impl_deserialize_for_parcelable!(r#BazUnion);
impl binder::binder_impl::ParcelableMetadata for r#BazUnion {
  fn get_descriptor() -> &'static str { "android.aidl.versioned.tests.BazUnion" }
}
pub mod r#Tag {
  #![allow(non_upper_case_globals)]
  use binder::declare_binder_enum;
  declare_binder_enum! {
    #[repr(C, align(4))]
    r#Tag : [i32; 2] {
      r#intNum = 0,
      r#longNum = 1,
    }
  }
}
pub(crate) mod mangled {
 pub use super::r#BazUnion as _7_android_4_aidl_9_versioned_5_tests_8_BazUnion;
 pub use super::r#Tag::r#Tag as _7_android_4_aidl_9_versioned_5_tests_8_BazUnion_3_Tag;
}
