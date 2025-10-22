/*
 * This file is auto-generated.  DO NOT MODIFY.
 * Using: out/host/linux-x86/bin/aidl --lang=rust -Weverything -Wno-missing-permission-annotation -Werror --min_sdk_version current --ninja -d out/soong/.intermediates/system/tools/aidl/aidl-test-interface-rust-source/gen/android/aidl/tests/IDeprecated.rs.d -o out/soong/.intermediates/system/tools/aidl/aidl-test-interface-rust-source/gen -Nsystem/tools/aidl/tests system/tools/aidl/tests/android/aidl/tests/IDeprecated.aidl
 *
 * DO NOT CHECK THIS FILE INTO A CODE TREE (e.g. git, etc..).
 * ALWAYS GENERATE THIS FILE FROM UPDATED AIDL COMPILER
 * AS A BUILD INTERMEDIATE ONLY. THIS IS NOT SOURCE CODE.
 */
#![forbid(unsafe_code)]
#![cfg_attr(rustfmt, rustfmt_skip)]
#![allow(non_upper_case_globals)]
#![allow(non_snake_case)]
#[allow(unused_imports)] use binder::binder_impl::IBinderInternal;
#[cfg(any(android_vndk, not(android_ndk)))]
const FLAG_PRIVATE_LOCAL: binder::binder_impl::TransactionFlags = binder::binder_impl::FLAG_PRIVATE_LOCAL;
#[cfg(not(any(android_vndk, not(android_ndk))))]
const FLAG_PRIVATE_LOCAL: binder::binder_impl::TransactionFlags = 0;
use binder::declare_binder_interface;
declare_binder_interface! {
  IDeprecated["android.aidl.tests.IDeprecated"] {
    native: BnDeprecated(on_transact),
    proxy: BpDeprecated {
    },
    async: IDeprecatedAsync(try_into_local_async),
  }
}
#[deprecated = "test"]
pub trait IDeprecated: binder::Interface + Send {
  fn get_descriptor() -> &'static str where Self: Sized { "android.aidl.tests.IDeprecated" }
  fn getDefaultImpl() -> IDeprecatedDefaultRef where Self: Sized {
    DEFAULT_IMPL.lock().unwrap().clone()
  }
  fn setDefaultImpl(d: IDeprecatedDefaultRef) -> IDeprecatedDefaultRef where Self: Sized {
    std::mem::replace(&mut *DEFAULT_IMPL.lock().unwrap(), d)
  }
  fn try_as_async_server<'a>(&'a self) -> Option<&'a (dyn IDeprecatedAsyncServer + Send + Sync)> {
    None
  }
}
#[deprecated = "test"]
pub trait IDeprecatedAsync<P>: binder::Interface + Send {
  fn get_descriptor() -> &'static str where Self: Sized { "android.aidl.tests.IDeprecated" }
}
#[deprecated = "test"]
#[::async_trait::async_trait]
pub trait IDeprecatedAsyncServer: binder::Interface + Send {
  fn get_descriptor() -> &'static str where Self: Sized { "android.aidl.tests.IDeprecated" }
}
impl BnDeprecated {
  /// Create a new async binder service.
  pub fn new_async_binder<T, R>(inner: T, rt: R, features: binder::BinderFeatures) -> binder::Strong<dyn IDeprecated>
  where
    T: IDeprecatedAsyncServer + binder::Interface + Send + Sync + 'static,
    R: binder::binder_impl::BinderAsyncRuntime + Send + Sync + 'static,
  {
    struct Wrapper<T, R> {
      _inner: T,
      _rt: R,
    }
    impl<T, R> binder::Interface for Wrapper<T, R> where T: binder::Interface, R: Send + Sync + 'static {
      fn as_binder(&self) -> binder::SpIBinder { self._inner.as_binder() }
      fn dump(&self, _writer: &mut dyn std::io::Write, _args: &[&std::ffi::CStr]) -> std::result::Result<(), binder::StatusCode> { self._inner.dump(_writer, _args) }
    }
    impl<T, R> IDeprecated for Wrapper<T, R>
    where
      T: IDeprecatedAsyncServer + Send + Sync + 'static,
      R: binder::binder_impl::BinderAsyncRuntime + Send + Sync + 'static,
    {
      fn try_as_async_server(&self) -> Option<&(dyn IDeprecatedAsyncServer + Send + Sync)> {
        Some(&self._inner)
      }
    }
    let wrapped = Wrapper { _inner: inner, _rt: rt };
    Self::new_binder(wrapped, features)
  }
  pub fn try_into_local_async<P: binder::BinderAsyncPool + 'static>(_native: binder::binder_impl::Binder<Self>) -> Option<binder::Strong<dyn IDeprecatedAsync<P>>> {
    struct Wrapper {
      _native: binder::binder_impl::Binder<BnDeprecated>
    }
    impl binder::Interface for Wrapper {}
    impl<P: binder::BinderAsyncPool> IDeprecatedAsync<P> for Wrapper {
    }
    if _native.try_as_async_server().is_some() {
      Some(binder::Strong::new(Box::new(Wrapper { _native }) as Box<dyn IDeprecatedAsync<P>>))
    } else {
      None
    }
  }
}
pub trait IDeprecatedDefault: Send + Sync {
}
pub mod transactions {
}
pub type IDeprecatedDefaultRef = Option<std::sync::Arc<dyn IDeprecatedDefault>>;
static DEFAULT_IMPL: std::sync::Mutex<IDeprecatedDefaultRef> = std::sync::Mutex::new(None);
impl BpDeprecated {
}
impl IDeprecated for BpDeprecated {
}
impl<P: binder::BinderAsyncPool> IDeprecatedAsync<P> for BpDeprecated {
}
impl IDeprecated for binder::binder_impl::Binder<BnDeprecated> {
}
fn on_transact(_aidl_service: &dyn IDeprecated, _aidl_code: binder::binder_impl::TransactionCode, _aidl_data: &binder::binder_impl::BorrowedParcel<'_>, _aidl_reply: &mut binder::binder_impl::BorrowedParcel<'_>) -> std::result::Result<(), binder::StatusCode> {
  match _aidl_code {
    _ => Err(binder::StatusCode::UNKNOWN_TRANSACTION)
  }
}
pub(crate) mod mangled {
 pub use super::r#IDeprecated as _7_android_4_aidl_5_tests_11_IDeprecated;
}
