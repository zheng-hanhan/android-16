/*
 * This file is auto-generated.  DO NOT MODIFY.
 * Using: out/host/linux-x86/bin/aidl --lang=ndk -Weverything -Wno-missing-permission-annotation -Werror -t --min_sdk_version current --ninja -d out/soong/.intermediates/system/tools/aidl/aidl-test-interface-ndk-source/gen/staging/android/aidl/tests/INewName.cpp.d -h out/soong/.intermediates/system/tools/aidl/aidl-test-interface-ndk-source/gen/include/staging -o out/soong/.intermediates/system/tools/aidl/aidl-test-interface-ndk-source/gen/staging -Nsystem/tools/aidl/tests system/tools/aidl/tests/android/aidl/tests/INewName.aidl
 *
 * DO NOT CHECK THIS FILE INTO A CODE TREE (e.g. git, etc..).
 * ALWAYS GENERATE THIS FILE FROM UPDATED AIDL COMPILER
 * AS A BUILD INTERMEDIATE ONLY. THIS IS NOT SOURCE CODE.
 */
#pragma once

#include "aidl/android/aidl/tests/INewName.h"

#include <android/binder_ibinder.h>

namespace aidl {
namespace android {
namespace aidl {
namespace tests {
class BpNewName : public ::ndk::BpCInterface<INewName> {
public:
  explicit BpNewName(const ::ndk::SpAIBinder& binder);
  virtual ~BpNewName();

  ::ndk::ScopedAStatus RealName(std::string* _aidl_return) override;
};
}  // namespace tests
}  // namespace aidl
}  // namespace android
}  // namespace aidl
