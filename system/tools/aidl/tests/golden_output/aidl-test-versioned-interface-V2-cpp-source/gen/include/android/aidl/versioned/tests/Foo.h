/*
 * This file is auto-generated.  DO NOT MODIFY.
 * Using: out/host/linux-x86/bin/aidl --lang=cpp --structured --version 2 --hash da8c4bc94ca7feff0e0a65563a466787698b5891 -t --min_sdk_version current --ninja -d out/soong/.intermediates/system/tools/aidl/aidl-test-versioned-interface-V2-cpp-source/gen/staging/android/aidl/versioned/tests/Foo.cpp.d -h out/soong/.intermediates/system/tools/aidl/aidl-test-versioned-interface-V2-cpp-source/gen/include/staging -o out/soong/.intermediates/system/tools/aidl/aidl-test-versioned-interface-V2-cpp-source/gen/staging -Nsystem/tools/aidl/aidl_api/aidl-test-versioned-interface/2 system/tools/aidl/aidl_api/aidl-test-versioned-interface/2/android/aidl/versioned/tests/Foo.aidl
 *
 * DO NOT CHECK THIS FILE INTO A CODE TREE (e.g. git, etc..).
 * ALWAYS GENERATE THIS FILE FROM UPDATED AIDL COMPILER
 * AS A BUILD INTERMEDIATE ONLY. THIS IS NOT SOURCE CODE.
 */
#pragma once

#include <android/binder_to_string.h>
#include <binder/Parcel.h>
#include <binder/Status.h>
#include <cstdint>
#include <tuple>
#include <utils/String16.h>

namespace android {
namespace aidl {
namespace versioned {
namespace tests {
class LIBBINDER_EXPORTED Foo : public ::android::Parcelable {
public:
  int32_t intDefault42 = 42;
  inline bool operator==(const Foo& _rhs) const {
    return std::tie(intDefault42) == std::tie(_rhs.intDefault42);
  }
  inline bool operator<(const Foo& _rhs) const {
    return std::tie(intDefault42) < std::tie(_rhs.intDefault42);
  }
  inline bool operator!=(const Foo& _rhs) const {
    return !(*this == _rhs);
  }
  inline bool operator>(const Foo& _rhs) const {
    return _rhs < *this;
  }
  inline bool operator>=(const Foo& _rhs) const {
    return !(*this < _rhs);
  }
  inline bool operator<=(const Foo& _rhs) const {
    return !(_rhs < *this);
  }

  ::android::status_t readFromParcel(const ::android::Parcel* _aidl_parcel) final;
  ::android::status_t writeToParcel(::android::Parcel* _aidl_parcel) const final;
  static const ::android::String16& getParcelableDescriptor() {
    static const ::android::StaticString16 DESCRIPTOR (u"android.aidl.versioned.tests.Foo");
    return DESCRIPTOR;
  }
  inline std::string toString() const {
    std::ostringstream _aidl_os;
    _aidl_os << "Foo{";
    _aidl_os << "intDefault42: " << ::android::internal::ToString(intDefault42);
    _aidl_os << "}";
    return _aidl_os.str();
  }
};  // class Foo
}  // namespace tests
}  // namespace versioned
}  // namespace aidl
}  // namespace android
