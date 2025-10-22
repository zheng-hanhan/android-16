/*
 * This file is auto-generated.  DO NOT MODIFY.
 * Using: out/host/linux-x86/bin/aidl --lang=rust -Weverything -Wno-missing-permission-annotation -Werror --min_sdk_version current --ninja -d out/soong/.intermediates/system/tools/aidl/aidl-test-interface-rust-source/gen/android/aidl/tests/IOldName.rs.d -o out/soong/.intermediates/system/tools/aidl/aidl-test-interface-rust-source/gen -Nsystem/tools/aidl/tests system/tools/aidl/tests/android/aidl/tests/IOldName.aidl
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
  IOldName["android.aidl.tests.IOldName"] {
    native: BnOldName(on_transact),
    proxy: BpOldName {
    },
    async: IOldNameAsync(try_into_local_async),
  }
}
pub trait IOldName: binder::Interface + Send {
  fn get_descriptor() -> &'static str where Self: Sized { "android.aidl.tests.IOldName" }
  fn r#RealName<'a, >(&'a self) -> binder::Result<String>;
  fn getDefaultImpl() -> IOldNameDefaultRef where Self: Sized {
    DEFAULT_IMPL.lock().unwrap().clone()
  }
  fn setDefaultImpl(d: IOldNameDefaultRef) -> IOldNameDefaultRef where Self: Sized {
    std::mem::replace(&mut *DEFAULT_IMPL.lock().unwrap(), d)
  }
  fn try_as_async_server<'a>(&'a self) -> Option<&'a (dyn IOldNameAsyncServer + Send + Sync)> {
    None
  }
}
pub trait IOldNameAsync<P>: binder::Interface + Send {
  fn get_descriptor() -> &'static str where Self: Sized { "android.aidl.tests.IOldName" }
  fn r#RealName<'a, >(&'a self) -> binder::BoxFuture<'a, binder::Result<String>>;
}
#[::async_trait::async_trait]
pub trait IOldNameAsyncServer: binder::Interface + Send {
  fn get_descriptor() -> &'static str where Self: Sized { "android.aidl.tests.IOldName" }
  async fn r#RealName<'a, >(&'a self) -> binder::Result<String>;
}
impl BnOldName {
  /// Create a new async binder service.
  pub fn new_async_binder<T, R>(inner: T, rt: R, features: binder::BinderFeatures) -> binder::Strong<dyn IOldName>
  where
    T: IOldNameAsyncServer + binder::Interface + Send + Sync + 'static,
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
    impl<T, R> IOldName for Wrapper<T, R>
    where
      T: IOldNameAsyncServer + Send + Sync + 'static,
      R: binder::binder_impl::BinderAsyncRuntime + Send + Sync + 'static,
    {
      fn r#RealName<'a, >(&'a self) -> binder::Result<String> {
        self._rt.block_on(self._inner.r#RealName())
      }
      fn try_as_async_server(&self) -> Option<&(dyn IOldNameAsyncServer + Send + Sync)> {
        Some(&self._inner)
      }
    }
    let wrapped = Wrapper { _inner: inner, _rt: rt };
    Self::new_binder(wrapped, features)
  }
  pub fn try_into_local_async<P: binder::BinderAsyncPool + 'static>(_native: binder::binder_impl::Binder<Self>) -> Option<binder::Strong<dyn IOldNameAsync<P>>> {
    struct Wrapper {
      _native: binder::binder_impl::Binder<BnOldName>
    }
    impl binder::Interface for Wrapper {}
    impl<P: binder::BinderAsyncPool> IOldNameAsync<P> for Wrapper {
      fn r#RealName<'a, >(&'a self) -> binder::BoxFuture<'a, binder::Result<String>> {
        Box::pin(self._native.try_as_async_server().unwrap().r#RealName())
      }
    }
    if _native.try_as_async_server().is_some() {
      Some(binder::Strong::new(Box::new(Wrapper { _native }) as Box<dyn IOldNameAsync<P>>))
    } else {
      None
    }
  }
}
pub trait IOldNameDefault: Send + Sync {
  fn r#RealName<'a, >(&'a self) -> binder::Result<String> {
    Err(binder::StatusCode::UNKNOWN_TRANSACTION.into())
  }
}
pub mod transactions {
  pub const r#RealName: binder::binder_impl::TransactionCode = binder::binder_impl::FIRST_CALL_TRANSACTION + 0;
}
pub type IOldNameDefaultRef = Option<std::sync::Arc<dyn IOldNameDefault>>;
static DEFAULT_IMPL: std::sync::Mutex<IOldNameDefaultRef> = std::sync::Mutex::new(None);
impl BpOldName {
  fn build_parcel_RealName(&self) -> binder::Result<binder::binder_impl::Parcel> {
    let mut aidl_data = self.binder.prepare_transact()?;
    Ok(aidl_data)
  }
  fn read_response_RealName(&self, _aidl_reply: std::result::Result<binder::binder_impl::Parcel, binder::StatusCode>) -> binder::Result<String> {
    if let Err(binder::StatusCode::UNKNOWN_TRANSACTION) = _aidl_reply {
      if let Some(_aidl_default_impl) = <Self as IOldName>::getDefaultImpl() {
        return _aidl_default_impl.r#RealName();
      }
    }
    let _aidl_reply = _aidl_reply?;
    let _aidl_status: binder::Status = _aidl_reply.read()?;
    if !_aidl_status.is_ok() { return Err(_aidl_status); }
    let _aidl_return: String = _aidl_reply.read()?;
    Ok(_aidl_return)
  }
}
impl IOldName for BpOldName {
  fn r#RealName<'a, >(&'a self) -> binder::Result<String> {
    let _aidl_data = self.build_parcel_RealName()?;
    let _aidl_reply = self.binder.submit_transact(transactions::r#RealName, _aidl_data, FLAG_PRIVATE_LOCAL);
    self.read_response_RealName(_aidl_reply)
  }
}
impl<P: binder::BinderAsyncPool> IOldNameAsync<P> for BpOldName {
  fn r#RealName<'a, >(&'a self) -> binder::BoxFuture<'a, binder::Result<String>> {
    let _aidl_data = match self.build_parcel_RealName() {
      Ok(_aidl_data) => _aidl_data,
      Err(err) => return Box::pin(std::future::ready(Err(err))),
    };
    let binder = self.binder.clone();
    P::spawn(
      move || binder.submit_transact(transactions::r#RealName, _aidl_data, FLAG_PRIVATE_LOCAL),
      move |_aidl_reply| async move {
        self.read_response_RealName(_aidl_reply)
      }
    )
  }
}
impl IOldName for binder::binder_impl::Binder<BnOldName> {
  fn r#RealName<'a, >(&'a self) -> binder::Result<String> { self.0.r#RealName() }
}
fn on_transact(_aidl_service: &dyn IOldName, _aidl_code: binder::binder_impl::TransactionCode, _aidl_data: &binder::binder_impl::BorrowedParcel<'_>, _aidl_reply: &mut binder::binder_impl::BorrowedParcel<'_>) -> std::result::Result<(), binder::StatusCode> {
  match _aidl_code {
    transactions::r#RealName => {
      let _aidl_return = _aidl_service.r#RealName();
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
 pub use super::r#IOldName as _7_android_4_aidl_5_tests_8_IOldName;
}
