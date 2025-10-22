/*
 * This file is auto-generated.  DO NOT MODIFY.
 * Using: out/host/linux-x86/bin/aidl --lang=ndk -Weverything -Wno-missing-permission-annotation -Werror --structured --version 2 --hash notfrozen -t --min_sdk_version current --log --ninja -d out/soong/.intermediates/system/tools/aidl/tests/trunk_stable_test/android.aidl.test.trunk-V2-ndk-source/gen/staging/android/aidl/test/trunk/ITrunkStableTest.cpp.d -h out/soong/.intermediates/system/tools/aidl/tests/trunk_stable_test/android.aidl.test.trunk-V2-ndk-source/gen/include/staging -o out/soong/.intermediates/system/tools/aidl/tests/trunk_stable_test/android.aidl.test.trunk-V2-ndk-source/gen/staging -Nsystem/tools/aidl/tests/trunk_stable_test system/tools/aidl/tests/trunk_stable_test/android/aidl/test/trunk/ITrunkStableTest.aidl
 *
 * DO NOT CHECK THIS FILE INTO A CODE TREE (e.g. git, etc..).
 * ALWAYS GENERATE THIS FILE FROM UPDATED AIDL COMPILER
 * AS A BUILD INTERMEDIATE ONLY. THIS IS NOT SOURCE CODE.
 */
#pragma once

#include "aidl/android/aidl/test/trunk/ITrunkStableTest.h"

#include <android/binder_ibinder.h>
#include <functional>
#include <chrono>
#include <sstream>

namespace aidl {
namespace android {
namespace aidl {
namespace test {
namespace trunk {
class BpTrunkStableTest : public ::ndk::BpCInterface<ITrunkStableTest> {
public:
  explicit BpTrunkStableTest(const ::ndk::SpAIBinder& binder);
  virtual ~BpTrunkStableTest();

  ::ndk::ScopedAStatus repeatParcelable(const ::aidl::android::aidl::test::trunk::ITrunkStableTest::MyParcelable& in_input, ::aidl::android::aidl::test::trunk::ITrunkStableTest::MyParcelable* _aidl_return) override;
  ::ndk::ScopedAStatus repeatEnum(::aidl::android::aidl::test::trunk::ITrunkStableTest::MyEnum in_input, ::aidl::android::aidl::test::trunk::ITrunkStableTest::MyEnum* _aidl_return) override;
  ::ndk::ScopedAStatus repeatUnion(const ::aidl::android::aidl::test::trunk::ITrunkStableTest::MyUnion& in_input, ::aidl::android::aidl::test::trunk::ITrunkStableTest::MyUnion* _aidl_return) override;
  ::ndk::ScopedAStatus callMyCallback(const std::shared_ptr<::aidl::android::aidl::test::trunk::ITrunkStableTest::IMyCallback>& in_cb) override;
  ::ndk::ScopedAStatus repeatOtherParcelable(const ::aidl::android::aidl::test::trunk::ITrunkStableTest::MyOtherParcelable& in_input, ::aidl::android::aidl::test::trunk::ITrunkStableTest::MyOtherParcelable* _aidl_return) override;
  ::ndk::ScopedAStatus getInterfaceVersion(int32_t* _aidl_return) override;
  ::ndk::ScopedAStatus getInterfaceHash(std::string* _aidl_return) override;
  int32_t _aidl_cached_version = -1;
  std::string _aidl_cached_hash = "-1";
  std::mutex _aidl_cached_hash_mutex;
  struct TransactionLog {
    double duration_ms;
    std::string interface_name;
    std::string method_name;
    const void* proxy_address;
    const void* stub_address;
    std::vector<std::pair<std::string, std::string>> input_args;
    std::vector<std::pair<std::string, std::string>> output_args;
    std::string result;
    std::string exception_message;
    int32_t exception_code;
    int32_t transaction_error;
    int32_t service_specific_error_code;
  };
  static std::function<void(const TransactionLog&)> logFunc;
};
}  // namespace trunk
}  // namespace test
}  // namespace aidl
}  // namespace android
}  // namespace aidl
