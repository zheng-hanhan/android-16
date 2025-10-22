/*
 * This file is auto-generated.  DO NOT MODIFY.
 * Using: out/host/linux-x86/bin/aidl --lang=rust --structured --version 3 --hash 70d76c61eb0c82288e924862c10b910d1b7d8cf8 --min_sdk_version current --ninja -d out/soong/.intermediates/system/tools/aidl/aidl-test-versioned-interface-V3-rust-source/gen/android/aidl/versioned/tests/IFooInterface.rs.d -o out/soong/.intermediates/system/tools/aidl/aidl-test-versioned-interface-V3-rust-source/gen -Nsystem/tools/aidl/aidl_api/aidl-test-versioned-interface/3 system/tools/aidl/aidl_api/aidl-test-versioned-interface/3/android/aidl/versioned/tests/IFooInterface.aidl
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
  IFooInterface["android.aidl.versioned.tests.IFooInterface"] {
    native: BnFooInterface(on_transact),
    proxy: BpFooInterface {
      cached_version: std::sync::atomic::AtomicI32 = std::sync::atomic::AtomicI32::new(-1),
      cached_hash: std::sync::Mutex<Option<String>> = std::sync::Mutex::new(None)
    },
    async: IFooInterfaceAsync(try_into_local_async),
  }
}
pub trait IFooInterface: binder::Interface + Send {
  fn get_descriptor() -> &'static str where Self: Sized { "android.aidl.versioned.tests.IFooInterface" }
  fn r#originalApi<'a, >(&'a self) -> binder::Result<()>;
  fn r#acceptUnionAndReturnString<'a, 'l1, >(&'a self, _arg_u: &'l1 crate::mangled::_7_android_4_aidl_9_versioned_5_tests_8_BazUnion) -> binder::Result<String>;
  fn r#ignoreParcelablesAndRepeatInt<'a, 'l1, 'l2, 'l3, >(&'a self, _arg_inFoo: &'l1 crate::mangled::_7_android_4_aidl_9_versioned_5_tests_3_Foo, _arg_inoutFoo: &'l2 mut crate::mangled::_7_android_4_aidl_9_versioned_5_tests_3_Foo, _arg_outFoo: &'l3 mut crate::mangled::_7_android_4_aidl_9_versioned_5_tests_3_Foo, _arg_value: i32) -> binder::Result<i32>;
  fn r#returnsLengthOfFooArray<'a, 'l1, >(&'a self, _arg_foos: &'l1 [crate::mangled::_7_android_4_aidl_9_versioned_5_tests_3_Foo]) -> binder::Result<i32>;
  fn r#newApi<'a, >(&'a self) -> binder::Result<()>;
  fn r#getInterfaceVersion<'a, >(&'a self) -> binder::Result<i32> {
    Ok(VERSION)
  }
  fn r#getInterfaceHash<'a, >(&'a self) -> binder::Result<String> {
    Ok(HASH.into())
  }
  fn getDefaultImpl() -> IFooInterfaceDefaultRef where Self: Sized {
    DEFAULT_IMPL.lock().unwrap().clone()
  }
  fn setDefaultImpl(d: IFooInterfaceDefaultRef) -> IFooInterfaceDefaultRef where Self: Sized {
    std::mem::replace(&mut *DEFAULT_IMPL.lock().unwrap(), d)
  }
  fn try_as_async_server<'a>(&'a self) -> Option<&'a (dyn IFooInterfaceAsyncServer + Send + Sync)> {
    None
  }
}
pub trait IFooInterfaceAsync<P>: binder::Interface + Send {
  fn get_descriptor() -> &'static str where Self: Sized { "android.aidl.versioned.tests.IFooInterface" }
  fn r#originalApi<'a, >(&'a self) -> binder::BoxFuture<'a, binder::Result<()>>;
  fn r#acceptUnionAndReturnString<'a, >(&'a self, _arg_u: &'a crate::mangled::_7_android_4_aidl_9_versioned_5_tests_8_BazUnion) -> binder::BoxFuture<'a, binder::Result<String>>;
  fn r#ignoreParcelablesAndRepeatInt<'a, >(&'a self, _arg_inFoo: &'a crate::mangled::_7_android_4_aidl_9_versioned_5_tests_3_Foo, _arg_inoutFoo: &'a mut crate::mangled::_7_android_4_aidl_9_versioned_5_tests_3_Foo, _arg_outFoo: &'a mut crate::mangled::_7_android_4_aidl_9_versioned_5_tests_3_Foo, _arg_value: i32) -> binder::BoxFuture<'a, binder::Result<i32>>;
  fn r#returnsLengthOfFooArray<'a, >(&'a self, _arg_foos: &'a [crate::mangled::_7_android_4_aidl_9_versioned_5_tests_3_Foo]) -> binder::BoxFuture<'a, binder::Result<i32>>;
  fn r#newApi<'a, >(&'a self) -> binder::BoxFuture<'a, binder::Result<()>>;
  fn r#getInterfaceVersion<'a, >(&'a self) -> binder::BoxFuture<'a, binder::Result<i32>> {
    Box::pin(async move { Ok(VERSION) })
  }
  fn r#getInterfaceHash<'a, >(&'a self) -> binder::BoxFuture<'a, binder::Result<String>> {
    Box::pin(async move { Ok(HASH.into()) })
  }
}
#[::async_trait::async_trait]
pub trait IFooInterfaceAsyncServer: binder::Interface + Send {
  fn get_descriptor() -> &'static str where Self: Sized { "android.aidl.versioned.tests.IFooInterface" }
  async fn r#originalApi<'a, >(&'a self) -> binder::Result<()>;
  async fn r#acceptUnionAndReturnString<'a, 'l1, >(&'a self, _arg_u: &'l1 crate::mangled::_7_android_4_aidl_9_versioned_5_tests_8_BazUnion) -> binder::Result<String>;
  async fn r#ignoreParcelablesAndRepeatInt<'a, 'l1, 'l2, 'l3, >(&'a self, _arg_inFoo: &'l1 crate::mangled::_7_android_4_aidl_9_versioned_5_tests_3_Foo, _arg_inoutFoo: &'l2 mut crate::mangled::_7_android_4_aidl_9_versioned_5_tests_3_Foo, _arg_outFoo: &'l3 mut crate::mangled::_7_android_4_aidl_9_versioned_5_tests_3_Foo, _arg_value: i32) -> binder::Result<i32>;
  async fn r#returnsLengthOfFooArray<'a, 'l1, >(&'a self, _arg_foos: &'l1 [crate::mangled::_7_android_4_aidl_9_versioned_5_tests_3_Foo]) -> binder::Result<i32>;
  async fn r#newApi<'a, >(&'a self) -> binder::Result<()>;
}
impl BnFooInterface {
  /// Create a new async binder service.
  pub fn new_async_binder<T, R>(inner: T, rt: R, features: binder::BinderFeatures) -> binder::Strong<dyn IFooInterface>
  where
    T: IFooInterfaceAsyncServer + binder::Interface + Send + Sync + 'static,
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
    impl<T, R> IFooInterface for Wrapper<T, R>
    where
      T: IFooInterfaceAsyncServer + Send + Sync + 'static,
      R: binder::binder_impl::BinderAsyncRuntime + Send + Sync + 'static,
    {
      fn r#originalApi<'a, >(&'a self) -> binder::Result<()> {
        self._rt.block_on(self._inner.r#originalApi())
      }
      fn r#acceptUnionAndReturnString<'a, 'l1, >(&'a self, _arg_u: &'l1 crate::mangled::_7_android_4_aidl_9_versioned_5_tests_8_BazUnion) -> binder::Result<String> {
        self._rt.block_on(self._inner.r#acceptUnionAndReturnString(_arg_u))
      }
      fn r#ignoreParcelablesAndRepeatInt<'a, 'l1, 'l2, 'l3, >(&'a self, _arg_inFoo: &'l1 crate::mangled::_7_android_4_aidl_9_versioned_5_tests_3_Foo, _arg_inoutFoo: &'l2 mut crate::mangled::_7_android_4_aidl_9_versioned_5_tests_3_Foo, _arg_outFoo: &'l3 mut crate::mangled::_7_android_4_aidl_9_versioned_5_tests_3_Foo, _arg_value: i32) -> binder::Result<i32> {
        self._rt.block_on(self._inner.r#ignoreParcelablesAndRepeatInt(_arg_inFoo, _arg_inoutFoo, _arg_outFoo, _arg_value))
      }
      fn r#returnsLengthOfFooArray<'a, 'l1, >(&'a self, _arg_foos: &'l1 [crate::mangled::_7_android_4_aidl_9_versioned_5_tests_3_Foo]) -> binder::Result<i32> {
        self._rt.block_on(self._inner.r#returnsLengthOfFooArray(_arg_foos))
      }
      fn r#newApi<'a, >(&'a self) -> binder::Result<()> {
        self._rt.block_on(self._inner.r#newApi())
      }
      fn try_as_async_server(&self) -> Option<&(dyn IFooInterfaceAsyncServer + Send + Sync)> {
        Some(&self._inner)
      }
    }
    let wrapped = Wrapper { _inner: inner, _rt: rt };
    Self::new_binder(wrapped, features)
  }
  pub fn try_into_local_async<P: binder::BinderAsyncPool + 'static>(_native: binder::binder_impl::Binder<Self>) -> Option<binder::Strong<dyn IFooInterfaceAsync<P>>> {
    struct Wrapper {
      _native: binder::binder_impl::Binder<BnFooInterface>
    }
    impl binder::Interface for Wrapper {}
    impl<P: binder::BinderAsyncPool> IFooInterfaceAsync<P> for Wrapper {
      fn r#originalApi<'a, >(&'a self) -> binder::BoxFuture<'a, binder::Result<()>> {
        Box::pin(self._native.try_as_async_server().unwrap().r#originalApi())
      }
      fn r#acceptUnionAndReturnString<'a, >(&'a self, _arg_u: &'a crate::mangled::_7_android_4_aidl_9_versioned_5_tests_8_BazUnion) -> binder::BoxFuture<'a, binder::Result<String>> {
        Box::pin(self._native.try_as_async_server().unwrap().r#acceptUnionAndReturnString(_arg_u))
      }
      fn r#ignoreParcelablesAndRepeatInt<'a, >(&'a self, _arg_inFoo: &'a crate::mangled::_7_android_4_aidl_9_versioned_5_tests_3_Foo, _arg_inoutFoo: &'a mut crate::mangled::_7_android_4_aidl_9_versioned_5_tests_3_Foo, _arg_outFoo: &'a mut crate::mangled::_7_android_4_aidl_9_versioned_5_tests_3_Foo, _arg_value: i32) -> binder::BoxFuture<'a, binder::Result<i32>> {
        Box::pin(self._native.try_as_async_server().unwrap().r#ignoreParcelablesAndRepeatInt(_arg_inFoo, _arg_inoutFoo, _arg_outFoo, _arg_value))
      }
      fn r#returnsLengthOfFooArray<'a, >(&'a self, _arg_foos: &'a [crate::mangled::_7_android_4_aidl_9_versioned_5_tests_3_Foo]) -> binder::BoxFuture<'a, binder::Result<i32>> {
        Box::pin(self._native.try_as_async_server().unwrap().r#returnsLengthOfFooArray(_arg_foos))
      }
      fn r#newApi<'a, >(&'a self) -> binder::BoxFuture<'a, binder::Result<()>> {
        Box::pin(self._native.try_as_async_server().unwrap().r#newApi())
      }
    }
    if _native.try_as_async_server().is_some() {
      Some(binder::Strong::new(Box::new(Wrapper { _native }) as Box<dyn IFooInterfaceAsync<P>>))
    } else {
      None
    }
  }
}
pub trait IFooInterfaceDefault: Send + Sync {
  fn r#originalApi<'a, >(&'a self) -> binder::Result<()> {
    Err(binder::StatusCode::UNKNOWN_TRANSACTION.into())
  }
  fn r#acceptUnionAndReturnString<'a, 'l1, >(&'a self, _arg_u: &'l1 crate::mangled::_7_android_4_aidl_9_versioned_5_tests_8_BazUnion) -> binder::Result<String> {
    Err(binder::StatusCode::UNKNOWN_TRANSACTION.into())
  }
  fn r#ignoreParcelablesAndRepeatInt<'a, 'l1, 'l2, 'l3, >(&'a self, _arg_inFoo: &'l1 crate::mangled::_7_android_4_aidl_9_versioned_5_tests_3_Foo, _arg_inoutFoo: &'l2 mut crate::mangled::_7_android_4_aidl_9_versioned_5_tests_3_Foo, _arg_outFoo: &'l3 mut crate::mangled::_7_android_4_aidl_9_versioned_5_tests_3_Foo, _arg_value: i32) -> binder::Result<i32> {
    Err(binder::StatusCode::UNKNOWN_TRANSACTION.into())
  }
  fn r#returnsLengthOfFooArray<'a, 'l1, >(&'a self, _arg_foos: &'l1 [crate::mangled::_7_android_4_aidl_9_versioned_5_tests_3_Foo]) -> binder::Result<i32> {
    Err(binder::StatusCode::UNKNOWN_TRANSACTION.into())
  }
  fn r#newApi<'a, >(&'a self) -> binder::Result<()> {
    Err(binder::StatusCode::UNKNOWN_TRANSACTION.into())
  }
}
pub mod transactions {
  pub const r#originalApi: binder::binder_impl::TransactionCode = binder::binder_impl::FIRST_CALL_TRANSACTION + 0;
  pub const r#acceptUnionAndReturnString: binder::binder_impl::TransactionCode = binder::binder_impl::FIRST_CALL_TRANSACTION + 1;
  pub const r#ignoreParcelablesAndRepeatInt: binder::binder_impl::TransactionCode = binder::binder_impl::FIRST_CALL_TRANSACTION + 2;
  pub const r#returnsLengthOfFooArray: binder::binder_impl::TransactionCode = binder::binder_impl::FIRST_CALL_TRANSACTION + 3;
  pub const r#newApi: binder::binder_impl::TransactionCode = binder::binder_impl::FIRST_CALL_TRANSACTION + 4;
  pub const r#getInterfaceVersion: binder::binder_impl::TransactionCode = binder::binder_impl::FIRST_CALL_TRANSACTION + 16777214;
  pub const r#getInterfaceHash: binder::binder_impl::TransactionCode = binder::binder_impl::FIRST_CALL_TRANSACTION + 16777213;
}
pub type IFooInterfaceDefaultRef = Option<std::sync::Arc<dyn IFooInterfaceDefault>>;
static DEFAULT_IMPL: std::sync::Mutex<IFooInterfaceDefaultRef> = std::sync::Mutex::new(None);
pub const VERSION: i32 = 3;
pub const HASH: &str = "70d76c61eb0c82288e924862c10b910d1b7d8cf8";
impl BpFooInterface {
  fn build_parcel_originalApi(&self) -> binder::Result<binder::binder_impl::Parcel> {
    let mut aidl_data = self.binder.prepare_transact()?;
    Ok(aidl_data)
  }
  fn read_response_originalApi(&self, _aidl_reply: std::result::Result<binder::binder_impl::Parcel, binder::StatusCode>) -> binder::Result<()> {
    if let Err(binder::StatusCode::UNKNOWN_TRANSACTION) = _aidl_reply {
      if let Some(_aidl_default_impl) = <Self as IFooInterface>::getDefaultImpl() {
        return _aidl_default_impl.r#originalApi();
      }
    }
    let _aidl_reply = _aidl_reply?;
    let _aidl_status: binder::Status = _aidl_reply.read()?;
    if !_aidl_status.is_ok() { return Err(_aidl_status); }
    Ok(())
  }
  fn build_parcel_acceptUnionAndReturnString(&self, _arg_u: &crate::mangled::_7_android_4_aidl_9_versioned_5_tests_8_BazUnion) -> binder::Result<binder::binder_impl::Parcel> {
    let mut aidl_data = self.binder.prepare_transact()?;
    aidl_data.write(_arg_u)?;
    Ok(aidl_data)
  }
  fn read_response_acceptUnionAndReturnString(&self, _arg_u: &crate::mangled::_7_android_4_aidl_9_versioned_5_tests_8_BazUnion, _aidl_reply: std::result::Result<binder::binder_impl::Parcel, binder::StatusCode>) -> binder::Result<String> {
    if let Err(binder::StatusCode::UNKNOWN_TRANSACTION) = _aidl_reply {
      if let Some(_aidl_default_impl) = <Self as IFooInterface>::getDefaultImpl() {
        return _aidl_default_impl.r#acceptUnionAndReturnString(_arg_u);
      }
    }
    let _aidl_reply = _aidl_reply?;
    let _aidl_status: binder::Status = _aidl_reply.read()?;
    if !_aidl_status.is_ok() { return Err(_aidl_status); }
    let _aidl_return: String = _aidl_reply.read()?;
    Ok(_aidl_return)
  }
  fn build_parcel_ignoreParcelablesAndRepeatInt(&self, _arg_inFoo: &crate::mangled::_7_android_4_aidl_9_versioned_5_tests_3_Foo, _arg_inoutFoo: &mut crate::mangled::_7_android_4_aidl_9_versioned_5_tests_3_Foo, _arg_outFoo: &mut crate::mangled::_7_android_4_aidl_9_versioned_5_tests_3_Foo, _arg_value: i32) -> binder::Result<binder::binder_impl::Parcel> {
    let mut aidl_data = self.binder.prepare_transact()?;
    aidl_data.write(_arg_inFoo)?;
    aidl_data.write(_arg_inoutFoo)?;
    aidl_data.write(&_arg_value)?;
    Ok(aidl_data)
  }
  fn read_response_ignoreParcelablesAndRepeatInt(&self, _arg_inFoo: &crate::mangled::_7_android_4_aidl_9_versioned_5_tests_3_Foo, _arg_inoutFoo: &mut crate::mangled::_7_android_4_aidl_9_versioned_5_tests_3_Foo, _arg_outFoo: &mut crate::mangled::_7_android_4_aidl_9_versioned_5_tests_3_Foo, _arg_value: i32, _aidl_reply: std::result::Result<binder::binder_impl::Parcel, binder::StatusCode>) -> binder::Result<i32> {
    if let Err(binder::StatusCode::UNKNOWN_TRANSACTION) = _aidl_reply {
      if let Some(_aidl_default_impl) = <Self as IFooInterface>::getDefaultImpl() {
        return _aidl_default_impl.r#ignoreParcelablesAndRepeatInt(_arg_inFoo, _arg_inoutFoo, _arg_outFoo, _arg_value);
      }
    }
    let _aidl_reply = _aidl_reply?;
    let _aidl_status: binder::Status = _aidl_reply.read()?;
    if !_aidl_status.is_ok() { return Err(_aidl_status); }
    let _aidl_return: i32 = _aidl_reply.read()?;
    _aidl_reply.read_onto(_arg_inoutFoo)?;
    _aidl_reply.read_onto(_arg_outFoo)?;
    Ok(_aidl_return)
  }
  fn build_parcel_returnsLengthOfFooArray(&self, _arg_foos: &[crate::mangled::_7_android_4_aidl_9_versioned_5_tests_3_Foo]) -> binder::Result<binder::binder_impl::Parcel> {
    let mut aidl_data = self.binder.prepare_transact()?;
    aidl_data.write(_arg_foos)?;
    Ok(aidl_data)
  }
  fn read_response_returnsLengthOfFooArray(&self, _arg_foos: &[crate::mangled::_7_android_4_aidl_9_versioned_5_tests_3_Foo], _aidl_reply: std::result::Result<binder::binder_impl::Parcel, binder::StatusCode>) -> binder::Result<i32> {
    if let Err(binder::StatusCode::UNKNOWN_TRANSACTION) = _aidl_reply {
      if let Some(_aidl_default_impl) = <Self as IFooInterface>::getDefaultImpl() {
        return _aidl_default_impl.r#returnsLengthOfFooArray(_arg_foos);
      }
    }
    let _aidl_reply = _aidl_reply?;
    let _aidl_status: binder::Status = _aidl_reply.read()?;
    if !_aidl_status.is_ok() { return Err(_aidl_status); }
    let _aidl_return: i32 = _aidl_reply.read()?;
    Ok(_aidl_return)
  }
  fn build_parcel_newApi(&self) -> binder::Result<binder::binder_impl::Parcel> {
    let mut aidl_data = self.binder.prepare_transact()?;
    Ok(aidl_data)
  }
  fn read_response_newApi(&self, _aidl_reply: std::result::Result<binder::binder_impl::Parcel, binder::StatusCode>) -> binder::Result<()> {
    if let Err(binder::StatusCode::UNKNOWN_TRANSACTION) = _aidl_reply {
      if let Some(_aidl_default_impl) = <Self as IFooInterface>::getDefaultImpl() {
        return _aidl_default_impl.r#newApi();
      }
    }
    let _aidl_reply = _aidl_reply?;
    let _aidl_status: binder::Status = _aidl_reply.read()?;
    if !_aidl_status.is_ok() { return Err(_aidl_status); }
    Ok(())
  }
  fn build_parcel_getInterfaceVersion(&self) -> binder::Result<binder::binder_impl::Parcel> {
    let mut aidl_data = self.binder.prepare_transact()?;
    Ok(aidl_data)
  }
  fn read_response_getInterfaceVersion(&self, _aidl_reply: std::result::Result<binder::binder_impl::Parcel, binder::StatusCode>) -> binder::Result<i32> {
    let _aidl_reply = _aidl_reply?;
    let _aidl_status: binder::Status = _aidl_reply.read()?;
    if !_aidl_status.is_ok() { return Err(_aidl_status); }
    let _aidl_return: i32 = _aidl_reply.read()?;
    self.cached_version.store(_aidl_return, std::sync::atomic::Ordering::Relaxed);
    Ok(_aidl_return)
  }
  fn build_parcel_getInterfaceHash(&self) -> binder::Result<binder::binder_impl::Parcel> {
    let mut aidl_data = self.binder.prepare_transact()?;
    Ok(aidl_data)
  }
  fn read_response_getInterfaceHash(&self, _aidl_reply: std::result::Result<binder::binder_impl::Parcel, binder::StatusCode>) -> binder::Result<String> {
    let _aidl_reply = _aidl_reply?;
    let _aidl_status: binder::Status = _aidl_reply.read()?;
    if !_aidl_status.is_ok() { return Err(_aidl_status); }
    let _aidl_return: String = _aidl_reply.read()?;
    *self.cached_hash.lock().unwrap() = Some(_aidl_return.clone());
    Ok(_aidl_return)
  }
}
impl IFooInterface for BpFooInterface {
  fn r#originalApi<'a, >(&'a self) -> binder::Result<()> {
    let _aidl_data = self.build_parcel_originalApi()?;
    let _aidl_reply = self.binder.submit_transact(transactions::r#originalApi, _aidl_data, FLAG_PRIVATE_LOCAL);
    self.read_response_originalApi(_aidl_reply)
  }
  fn r#acceptUnionAndReturnString<'a, 'l1, >(&'a self, _arg_u: &'l1 crate::mangled::_7_android_4_aidl_9_versioned_5_tests_8_BazUnion) -> binder::Result<String> {
    let _aidl_data = self.build_parcel_acceptUnionAndReturnString(_arg_u)?;
    let _aidl_reply = self.binder.submit_transact(transactions::r#acceptUnionAndReturnString, _aidl_data, FLAG_PRIVATE_LOCAL);
    self.read_response_acceptUnionAndReturnString(_arg_u, _aidl_reply)
  }
  fn r#ignoreParcelablesAndRepeatInt<'a, 'l1, 'l2, 'l3, >(&'a self, _arg_inFoo: &'l1 crate::mangled::_7_android_4_aidl_9_versioned_5_tests_3_Foo, _arg_inoutFoo: &'l2 mut crate::mangled::_7_android_4_aidl_9_versioned_5_tests_3_Foo, _arg_outFoo: &'l3 mut crate::mangled::_7_android_4_aidl_9_versioned_5_tests_3_Foo, _arg_value: i32) -> binder::Result<i32> {
    let _aidl_data = self.build_parcel_ignoreParcelablesAndRepeatInt(_arg_inFoo, _arg_inoutFoo, _arg_outFoo, _arg_value)?;
    let _aidl_reply = self.binder.submit_transact(transactions::r#ignoreParcelablesAndRepeatInt, _aidl_data, FLAG_PRIVATE_LOCAL);
    self.read_response_ignoreParcelablesAndRepeatInt(_arg_inFoo, _arg_inoutFoo, _arg_outFoo, _arg_value, _aidl_reply)
  }
  fn r#returnsLengthOfFooArray<'a, 'l1, >(&'a self, _arg_foos: &'l1 [crate::mangled::_7_android_4_aidl_9_versioned_5_tests_3_Foo]) -> binder::Result<i32> {
    let _aidl_data = self.build_parcel_returnsLengthOfFooArray(_arg_foos)?;
    let _aidl_reply = self.binder.submit_transact(transactions::r#returnsLengthOfFooArray, _aidl_data, FLAG_PRIVATE_LOCAL);
    self.read_response_returnsLengthOfFooArray(_arg_foos, _aidl_reply)
  }
  fn r#newApi<'a, >(&'a self) -> binder::Result<()> {
    let _aidl_data = self.build_parcel_newApi()?;
    let _aidl_reply = self.binder.submit_transact(transactions::r#newApi, _aidl_data, FLAG_PRIVATE_LOCAL);
    self.read_response_newApi(_aidl_reply)
  }
  fn r#getInterfaceVersion<'a, >(&'a self) -> binder::Result<i32> {
    let _aidl_version = self.cached_version.load(std::sync::atomic::Ordering::Relaxed);
    if _aidl_version != -1 { return Ok(_aidl_version); }
    let _aidl_data = self.build_parcel_getInterfaceVersion()?;
    let _aidl_reply = self.binder.submit_transact(transactions::r#getInterfaceVersion, _aidl_data, FLAG_PRIVATE_LOCAL);
    self.read_response_getInterfaceVersion(_aidl_reply)
  }
  fn r#getInterfaceHash<'a, >(&'a self) -> binder::Result<String> {
    {
      let _aidl_hash_lock = self.cached_hash.lock().unwrap();
      if let Some(ref _aidl_hash) = *_aidl_hash_lock {
        return Ok(_aidl_hash.clone());
      }
    }
    let _aidl_data = self.build_parcel_getInterfaceHash()?;
    let _aidl_reply = self.binder.submit_transact(transactions::r#getInterfaceHash, _aidl_data, FLAG_PRIVATE_LOCAL);
    self.read_response_getInterfaceHash(_aidl_reply)
  }
}
impl<P: binder::BinderAsyncPool> IFooInterfaceAsync<P> for BpFooInterface {
  fn r#originalApi<'a, >(&'a self) -> binder::BoxFuture<'a, binder::Result<()>> {
    let _aidl_data = match self.build_parcel_originalApi() {
      Ok(_aidl_data) => _aidl_data,
      Err(err) => return Box::pin(std::future::ready(Err(err))),
    };
    let binder = self.binder.clone();
    P::spawn(
      move || binder.submit_transact(transactions::r#originalApi, _aidl_data, FLAG_PRIVATE_LOCAL),
      move |_aidl_reply| async move {
        self.read_response_originalApi(_aidl_reply)
      }
    )
  }
  fn r#acceptUnionAndReturnString<'a, >(&'a self, _arg_u: &'a crate::mangled::_7_android_4_aidl_9_versioned_5_tests_8_BazUnion) -> binder::BoxFuture<'a, binder::Result<String>> {
    let _aidl_data = match self.build_parcel_acceptUnionAndReturnString(_arg_u) {
      Ok(_aidl_data) => _aidl_data,
      Err(err) => return Box::pin(std::future::ready(Err(err))),
    };
    let binder = self.binder.clone();
    P::spawn(
      move || binder.submit_transact(transactions::r#acceptUnionAndReturnString, _aidl_data, FLAG_PRIVATE_LOCAL),
      move |_aidl_reply| async move {
        self.read_response_acceptUnionAndReturnString(_arg_u, _aidl_reply)
      }
    )
  }
  fn r#ignoreParcelablesAndRepeatInt<'a, >(&'a self, _arg_inFoo: &'a crate::mangled::_7_android_4_aidl_9_versioned_5_tests_3_Foo, _arg_inoutFoo: &'a mut crate::mangled::_7_android_4_aidl_9_versioned_5_tests_3_Foo, _arg_outFoo: &'a mut crate::mangled::_7_android_4_aidl_9_versioned_5_tests_3_Foo, _arg_value: i32) -> binder::BoxFuture<'a, binder::Result<i32>> {
    let _aidl_data = match self.build_parcel_ignoreParcelablesAndRepeatInt(_arg_inFoo, _arg_inoutFoo, _arg_outFoo, _arg_value) {
      Ok(_aidl_data) => _aidl_data,
      Err(err) => return Box::pin(std::future::ready(Err(err))),
    };
    let binder = self.binder.clone();
    P::spawn(
      move || binder.submit_transact(transactions::r#ignoreParcelablesAndRepeatInt, _aidl_data, FLAG_PRIVATE_LOCAL),
      move |_aidl_reply| async move {
        self.read_response_ignoreParcelablesAndRepeatInt(_arg_inFoo, _arg_inoutFoo, _arg_outFoo, _arg_value, _aidl_reply)
      }
    )
  }
  fn r#returnsLengthOfFooArray<'a, >(&'a self, _arg_foos: &'a [crate::mangled::_7_android_4_aidl_9_versioned_5_tests_3_Foo]) -> binder::BoxFuture<'a, binder::Result<i32>> {
    let _aidl_data = match self.build_parcel_returnsLengthOfFooArray(_arg_foos) {
      Ok(_aidl_data) => _aidl_data,
      Err(err) => return Box::pin(std::future::ready(Err(err))),
    };
    let binder = self.binder.clone();
    P::spawn(
      move || binder.submit_transact(transactions::r#returnsLengthOfFooArray, _aidl_data, FLAG_PRIVATE_LOCAL),
      move |_aidl_reply| async move {
        self.read_response_returnsLengthOfFooArray(_arg_foos, _aidl_reply)
      }
    )
  }
  fn r#newApi<'a, >(&'a self) -> binder::BoxFuture<'a, binder::Result<()>> {
    let _aidl_data = match self.build_parcel_newApi() {
      Ok(_aidl_data) => _aidl_data,
      Err(err) => return Box::pin(std::future::ready(Err(err))),
    };
    let binder = self.binder.clone();
    P::spawn(
      move || binder.submit_transact(transactions::r#newApi, _aidl_data, FLAG_PRIVATE_LOCAL),
      move |_aidl_reply| async move {
        self.read_response_newApi(_aidl_reply)
      }
    )
  }
  fn r#getInterfaceVersion<'a, >(&'a self) -> binder::BoxFuture<'a, binder::Result<i32>> {
    let _aidl_version = self.cached_version.load(std::sync::atomic::Ordering::Relaxed);
    if _aidl_version != -1 { return Box::pin(std::future::ready(Ok(_aidl_version))); }
    let _aidl_data = match self.build_parcel_getInterfaceVersion() {
      Ok(_aidl_data) => _aidl_data,
      Err(err) => return Box::pin(std::future::ready(Err(err))),
    };
    let binder = self.binder.clone();
    P::spawn(
      move || binder.submit_transact(transactions::r#getInterfaceVersion, _aidl_data, FLAG_PRIVATE_LOCAL),
      move |_aidl_reply| async move {
        self.read_response_getInterfaceVersion(_aidl_reply)
      }
    )
  }
  fn r#getInterfaceHash<'a, >(&'a self) -> binder::BoxFuture<'a, binder::Result<String>> {
    {
      let _aidl_hash_lock = self.cached_hash.lock().unwrap();
      if let Some(ref _aidl_hash) = *_aidl_hash_lock {
        return Box::pin(std::future::ready(Ok(_aidl_hash.clone())));
      }
    }
    let _aidl_data = match self.build_parcel_getInterfaceHash() {
      Ok(_aidl_data) => _aidl_data,
      Err(err) => return Box::pin(std::future::ready(Err(err))),
    };
    let binder = self.binder.clone();
    P::spawn(
      move || binder.submit_transact(transactions::r#getInterfaceHash, _aidl_data, FLAG_PRIVATE_LOCAL),
      move |_aidl_reply| async move {
        self.read_response_getInterfaceHash(_aidl_reply)
      }
    )
  }
}
impl IFooInterface for binder::binder_impl::Binder<BnFooInterface> {
  fn r#originalApi<'a, >(&'a self) -> binder::Result<()> { self.0.r#originalApi() }
  fn r#acceptUnionAndReturnString<'a, 'l1, >(&'a self, _arg_u: &'l1 crate::mangled::_7_android_4_aidl_9_versioned_5_tests_8_BazUnion) -> binder::Result<String> { self.0.r#acceptUnionAndReturnString(_arg_u) }
  fn r#ignoreParcelablesAndRepeatInt<'a, 'l1, 'l2, 'l3, >(&'a self, _arg_inFoo: &'l1 crate::mangled::_7_android_4_aidl_9_versioned_5_tests_3_Foo, _arg_inoutFoo: &'l2 mut crate::mangled::_7_android_4_aidl_9_versioned_5_tests_3_Foo, _arg_outFoo: &'l3 mut crate::mangled::_7_android_4_aidl_9_versioned_5_tests_3_Foo, _arg_value: i32) -> binder::Result<i32> { self.0.r#ignoreParcelablesAndRepeatInt(_arg_inFoo, _arg_inoutFoo, _arg_outFoo, _arg_value) }
  fn r#returnsLengthOfFooArray<'a, 'l1, >(&'a self, _arg_foos: &'l1 [crate::mangled::_7_android_4_aidl_9_versioned_5_tests_3_Foo]) -> binder::Result<i32> { self.0.r#returnsLengthOfFooArray(_arg_foos) }
  fn r#newApi<'a, >(&'a self) -> binder::Result<()> { self.0.r#newApi() }
  fn r#getInterfaceVersion<'a, >(&'a self) -> binder::Result<i32> { self.0.r#getInterfaceVersion() }
  fn r#getInterfaceHash<'a, >(&'a self) -> binder::Result<String> { self.0.r#getInterfaceHash() }
}
fn on_transact(_aidl_service: &dyn IFooInterface, _aidl_code: binder::binder_impl::TransactionCode, _aidl_data: &binder::binder_impl::BorrowedParcel<'_>, _aidl_reply: &mut binder::binder_impl::BorrowedParcel<'_>) -> std::result::Result<(), binder::StatusCode> {
  match _aidl_code {
    transactions::r#originalApi => {
      let _aidl_return = _aidl_service.r#originalApi();
      match &_aidl_return {
        Ok(_aidl_return) => {
          _aidl_reply.write(&binder::Status::from(binder::StatusCode::OK))?;
        }
        Err(_aidl_status) => _aidl_reply.write(_aidl_status)?
      }
      Ok(())
    }
    transactions::r#acceptUnionAndReturnString => {
      let _arg_u: crate::mangled::_7_android_4_aidl_9_versioned_5_tests_8_BazUnion = _aidl_data.read()?;
      let _aidl_return = _aidl_service.r#acceptUnionAndReturnString(&_arg_u);
      match &_aidl_return {
        Ok(_aidl_return) => {
          _aidl_reply.write(&binder::Status::from(binder::StatusCode::OK))?;
          _aidl_reply.write(_aidl_return)?;
        }
        Err(_aidl_status) => _aidl_reply.write(_aidl_status)?
      }
      Ok(())
    }
    transactions::r#ignoreParcelablesAndRepeatInt => {
      let _arg_inFoo: crate::mangled::_7_android_4_aidl_9_versioned_5_tests_3_Foo = _aidl_data.read()?;
      let mut _arg_inoutFoo: crate::mangled::_7_android_4_aidl_9_versioned_5_tests_3_Foo = _aidl_data.read()?;
      let mut _arg_outFoo: crate::mangled::_7_android_4_aidl_9_versioned_5_tests_3_Foo = Default::default();
      let _arg_value: i32 = _aidl_data.read()?;
      let _aidl_return = _aidl_service.r#ignoreParcelablesAndRepeatInt(&_arg_inFoo, &mut _arg_inoutFoo, &mut _arg_outFoo, _arg_value);
      match &_aidl_return {
        Ok(_aidl_return) => {
          _aidl_reply.write(&binder::Status::from(binder::StatusCode::OK))?;
          _aidl_reply.write(_aidl_return)?;
          _aidl_reply.write(&_arg_inoutFoo)?;
          _aidl_reply.write(&_arg_outFoo)?;
        }
        Err(_aidl_status) => _aidl_reply.write(_aidl_status)?
      }
      Ok(())
    }
    transactions::r#returnsLengthOfFooArray => {
      let _arg_foos: Vec<crate::mangled::_7_android_4_aidl_9_versioned_5_tests_3_Foo> = _aidl_data.read()?;
      let _aidl_return = _aidl_service.r#returnsLengthOfFooArray(&_arg_foos);
      match &_aidl_return {
        Ok(_aidl_return) => {
          _aidl_reply.write(&binder::Status::from(binder::StatusCode::OK))?;
          _aidl_reply.write(_aidl_return)?;
        }
        Err(_aidl_status) => _aidl_reply.write(_aidl_status)?
      }
      Ok(())
    }
    transactions::r#newApi => {
      let _aidl_return = _aidl_service.r#newApi();
      match &_aidl_return {
        Ok(_aidl_return) => {
          _aidl_reply.write(&binder::Status::from(binder::StatusCode::OK))?;
        }
        Err(_aidl_status) => _aidl_reply.write(_aidl_status)?
      }
      Ok(())
    }
    transactions::r#getInterfaceVersion => {
      let _aidl_return = _aidl_service.r#getInterfaceVersion();
      match &_aidl_return {
        Ok(_aidl_return) => {
          _aidl_reply.write(&binder::Status::from(binder::StatusCode::OK))?;
          _aidl_reply.write(_aidl_return)?;
        }
        Err(_aidl_status) => _aidl_reply.write(_aidl_status)?
      }
      Ok(())
    }
    transactions::r#getInterfaceHash => {
      let _aidl_return = _aidl_service.r#getInterfaceHash();
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
 pub use super::r#IFooInterface as _7_android_4_aidl_9_versioned_5_tests_13_IFooInterface;
}
