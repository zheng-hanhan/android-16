/*
 * This file is auto-generated.  DO NOT MODIFY.
 * Using: out/host/linux-x86/bin/aidl --lang=cpp -Weverything -Wno-missing-permission-annotation -Werror -t --min_sdk_version current --ninja -d out/soong/.intermediates/system/tools/aidl/aidl-test-interface-cpp-source/gen/staging/android/aidl/tests/ICircular.cpp.d -h out/soong/.intermediates/system/tools/aidl/aidl-test-interface-cpp-source/gen/include/staging -o out/soong/.intermediates/system/tools/aidl/aidl-test-interface-cpp-source/gen/staging -Nsystem/tools/aidl/tests system/tools/aidl/tests/android/aidl/tests/ICircular.aidl
 *
 * DO NOT CHECK THIS FILE INTO A CODE TREE (e.g. git, etc..).
 * ALWAYS GENERATE THIS FILE FROM UPDATED AIDL COMPILER
 * AS A BUILD INTERMEDIATE ONLY. THIS IS NOT SOURCE CODE.
 */
#pragma once

#include <binder/IBinder.h>
#include <binder/IInterface.h>
#include <utils/Errors.h>
#include <android/aidl/tests/ICircular.h>

namespace android {
namespace aidl {
namespace tests {
class LIBBINDER_EXPORTED BpCircular : public ::android::BpInterface<ICircular> {
public:
  explicit BpCircular(const ::android::sp<::android::IBinder>& _aidl_impl);
  virtual ~BpCircular() = default;
  ::android::binder::Status GetTestService(::android::sp<::android::aidl::tests::ITestService>* _aidl_return) override;
};  // class BpCircular
}  // namespace tests
}  // namespace aidl
}  // namespace android
