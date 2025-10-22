/*
 * This file is auto-generated.  DO NOT MODIFY.
 * Using: out/host/linux-x86/bin/aidl --lang=cpp -Weverything -Wno-missing-permission-annotation -Werror -t --min_sdk_version current --ninja -d out/soong/.intermediates/system/tools/aidl/aidl-test-interface-cpp-source/gen/staging/android/aidl/tests/IDeprecated.cpp.d -h out/soong/.intermediates/system/tools/aidl/aidl-test-interface-cpp-source/gen/include/staging -o out/soong/.intermediates/system/tools/aidl/aidl-test-interface-cpp-source/gen/staging -Nsystem/tools/aidl/tests system/tools/aidl/tests/android/aidl/tests/IDeprecated.aidl
 *
 * DO NOT CHECK THIS FILE INTO A CODE TREE (e.g. git, etc..).
 * ALWAYS GENERATE THIS FILE FROM UPDATED AIDL COMPILER
 * AS A BUILD INTERMEDIATE ONLY. THIS IS NOT SOURCE CODE.
 */
#pragma once

#include <binder/IBinder.h>
#include <binder/IInterface.h>
#include <binder/Status.h>
#include <binder/Trace.h>
#include <utils/StrongPointer.h>

namespace android {
namespace aidl {
namespace tests {
class LIBBINDER_EXPORTED IDeprecatedDelegator;

class LIBBINDER_EXPORTED __attribute__((deprecated("test"))) IDeprecated : public ::android::IInterface {
public:
  typedef IDeprecatedDelegator DefaultDelegator;
  DECLARE_META_INTERFACE(Deprecated)
};  // class IDeprecated

class LIBBINDER_EXPORTED __attribute__((deprecated("test"))) IDeprecatedDefault : public IDeprecated {
public:
  ::android::IBinder* onAsBinder() override {
    return nullptr;
  }
};  // class IDeprecatedDefault
}  // namespace tests
}  // namespace aidl
}  // namespace android
