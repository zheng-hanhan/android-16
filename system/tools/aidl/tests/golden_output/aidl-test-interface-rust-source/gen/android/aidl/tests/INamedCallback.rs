/*
 * This file is auto-generated.  DO NOT MODIFY.
 * Using: out/host/linux-x86/bin/aidl --lang=rust -Weverything -Wno-missing-permission-annotation -Werror --min_sdk_version current --ninja -d out/soong/.intermediates/system/tools/aidl/aidl-test-interface-rust-source/gen/android/aidl/tests/INamedCallback.rs.d -o out/soong/.intermediates/system/tools/aidl/aidl-test-interface-rust-source/gen -Nsystem/tools/aidl/tests system/tools/aidl/tests/android/aidl/tests/INamedCallback.aidl
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
  INamedCallback["android.aidl.tests.INamedCallback"] {
    native: BnNamedCallback(on_transact),
    proxy: BpNamedCallback {
    },
    async: INamedCallbackAsync(try_into_local_async),
  }
}
pub trait INamedCallback: binder::Interface + Send {
  fn get_descriptor() -> &'static str where Self: Sized { "android.aidl.tests.INamedCallback" }
  fn r#GetName<'a, >(&'a self) -> binder::Result<String>;
  fn getDefaultImpl() -> INamedCallbackDefaultRef where Self: Sized {
    DEFAULT_IMPL.lock().unwrap().clone()
  }
  fn setDefaultImpl(d: INamedCallbackDefaultRef) -> INamedCallbackDefaultRef where Self: Sized {
    std::mem::replace(&mut *DEFAULT_IMPL.lock().unwrap(), d)
  }
  fn try_as_async_server<'a>(&'a self) -> Option<&'a (dyn INamedCallbackAsyncServer + Send + Sync)> {
    None
  }
}
pub trait INamedCallbackAsync<P>: binder::Interface + Send {
  fn get_descriptor() -> &'static str where Self: Sized { "android.aidl.tests.INamedCallback" }
  fn r#GetName<'a, >(&'a self) -> binder::BoxFuture<'a, binder::Result<String>>;
}
#[::async_trait::async_trait]
pub trait INamedCallbackAsyncServer: binder::Interface + Send {
  fn get_descriptor() -> &'static str where Self: Sized { "android.aidl.tests.INamedCallback" }
  async fn r#GetName<'a, >(&'a self) -> binder::Result<String>;
}
impl BnNamedCallback {
  /// Create a new async binder service.
  pub fn new_async_binder<T, R>(inner: T, rt: R, features: binder::BinderFeatures) -> binder::Strong<dyn INamedCallback>
  where
    T: INamedCallbackAsyncServer + binder::Interface + Send + Sync + 'static,
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
    impl<T, R> INamedCallback for Wrapper<T, R>
    where
      T: INamedCallbackAsyncServer + Send + Sync + 'static,
      R: binder::binder_impl::BinderAsyncRuntime + Send + Sync + 'static,
    {
      fn r#GetName<'a, >(&'a self) -> binder::Result<String> {
        self._rt.block_on(self._inner.r#GetName())
      }
      fn try_as_async_server(&self) -> Option<&(dyn INamedCallbackAsyncServer + Send + Sync)> {
        Some(&self._inner)
      }
    }
    let wrapped = Wrapper { _inner: inner, _rt: rt };
    Self::new_binder(wrapped, features)
  }
  pub fn try_into_local_async<P: binder::BinderAsyncPool + 'static>(_native: binder::binder_impl::Binder<Self>) -> Option<binder::Strong<dyn INamedCallbackAsync<P>>> {
    struct Wrapper {
      _native: binder::binder_impl::Binder<BnNamedCallback>
    }
    impl binder::Interface for Wrapper {}
    impl<P: binder::BinderAsyncPool> INamedCallbackAsync<P> for Wrapper {
      fn r#GetName<'a, >(&'a self) -> binder::BoxFuture<'a, binder::Result<String>> {
        Box::pin(self._native.try_as_async_server().unwrap().r#GetName())
      }
    }
    if _native.try_as_async_server().is_some() {
      Some(binder::Strong::new(Box::new(Wrapper { _native }) as Box<dyn INamedCallbackAsync<P>>))
    } else {
      None
    }
  }
}
pub trait INamedCallbackDefault: Send + Sync {
  fn r#GetName<'a, >(&'a self) -> binder::Result<String> {
    Err(binder::StatusCode::UNKNOWN_TRANSACTION.into())
  }
}
pub mod transactions {
  pub const r#GetName: binder::binder_impl::TransactionCode = binder::binder_impl::FIRST_CALL_TRANSACTION + 0;
}
pub type INamedCallbackDefaultRef = Option<std::sync::Arc<dyn INamedCallbackDefault>>;
static DEFAULT_IMPL: std::sync::Mutex<INamedCallbackDefaultRef> = std::sync::Mutex::new(None);
impl BpNamedCallback {
  fn build_parcel_GetName(&self) -> binder::Result<binder::binder_impl::Parcel> {
    let mut aidl_data = self.binder.prepare_transact()?;
    Ok(aidl_data)
  }
  fn read_response_GetName(&self, _aidl_reply: std::result::Result<binder::binder_impl::Parcel, binder::StatusCode>) -> binder::Result<String> {
    if let Err(binder::StatusCode::UNKNOWN_TRANSACTION) = _aidl_reply {
      if let Some(_aidl_default_impl) = <Self as INamedCallback>::getDefaultImpl() {
        return _aidl_default_impl.r#GetName();
      }
    }
    let _aidl_reply = _aidl_reply?;
    let _aidl_status: binder::Status = _aidl_reply.read()?;
    if !_aidl_status.is_ok() { return Err(_aidl_status); }
    let _aidl_return: String = _aidl_reply.read()?;
    Ok(_aidl_return)
  }
}
impl INamedCallback for BpNamedCallback {
  fn r#GetName<'a, >(&'a self) -> binder::Result<String> {
    let _aidl_data = self.build_parcel_GetName()?;
    let _aidl_reply = self.binder.submit_transact(transactions::r#GetName, _aidl_data, FLAG_PRIVATE_LOCAL);
    self.read_response_GetName(_aidl_reply)
  }
}
impl<P: binder::BinderAsyncPool> INamedCallbackAsync<P> for BpNamedCallback {
  fn r#GetName<'a, >(&'a self) -> binder::BoxFuture<'a, binder::Result<String>> {
    let _aidl_data = match self.build_parcel_GetName() {
      Ok(_aidl_data) => _aidl_data,
      Err(err) => return Box::pin(std::future::ready(Err(err))),
    };
    let binder = self.binder.clone();
    P::spawn(
      move || binder.submit_transact(transactions::r#GetName, _aidl_data, FLAG_PRIVATE_LOCAL),
      move |_aidl_reply| async move {
        self.read_response_GetName(_aidl_reply)
      }
    )
  }
}
impl INamedCallback for binder::binder_impl::Binder<BnNamedCallback> {
  fn r#GetName<'a, >(&'a self) -> binder::Result<String> { self.0.r#GetName() }
}
fn on_transact(_aidl_service: &dyn INamedCallback, _aidl_code: binder::binder_impl::TransactionCode, _aidl_data: &binder::binder_impl::BorrowedParcel<'_>, _aidl_reply: &mut binder::binder_impl::BorrowedParcel<'_>) -> std::result::Result<(), binder::StatusCode> {
  match _aidl_code {
    transactions::r#GetName => {
      let _aidl_return = _aidl_service.r#GetName();
      match &_aidl_return {
        Ok(_aidl_return) => {
          _aidl_reply.write(&binder::Status::from(binder::StatusCode::OK))?;
          _aidl_reply.write(_aidl_return)?;
        }
        Err(_aidl_status) => _aidl_reply.write(_aidl_status)?
      }
      Ok(())
    }
    _ => Err(binder::StatusCode::UNKNOWN_TRANSACTION)
  }
}
pub(crate) mod mangled {
 pub use super::r#INamedCallback as _7_android_4_aidl_5_tests_14_INamedCallback;
}
