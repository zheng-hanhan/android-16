/*
 * This file is auto-generated.  DO NOT MODIFY.
 * Using: out/host/linux-x86/bin/aidl --lang=cpp -Weverything -Wno-missing-permission-annotation -Werror -t --min_sdk_version current --ninja -d out/soong/.intermediates/system/tools/aidl/aidl-test-interface-cpp-source/gen/staging/android/aidl/tests/StructuredParcelable.cpp.d -h out/soong/.intermediates/system/tools/aidl/aidl-test-interface-cpp-source/gen/include/staging -o out/soong/.intermediates/system/tools/aidl/aidl-test-interface-cpp-source/gen/staging -Nsystem/tools/aidl/tests system/tools/aidl/tests/android/aidl/tests/StructuredParcelable.aidl
 *
 * DO NOT CHECK THIS FILE INTO A CODE TREE (e.g. git, etc..).
 * ALWAYS GENERATE THIS FILE FROM UPDATED AIDL COMPILER
 * AS A BUILD INTERMEDIATE ONLY. THIS IS NOT SOURCE CODE.
 */
#pragma once

#include <android/aidl/tests/ByteEnum.h>
#include <android/aidl/tests/ConstantExpressionEnum.h>
#include <android/aidl/tests/IntEnum.h>
#include <android/aidl/tests/LongEnum.h>
#include <android/aidl/tests/StructuredParcelable.h>
#include <android/aidl/tests/Union.h>
#include <android/binder_to_string.h>
#include <binder/IBinder.h>
#include <binder/Parcel.h>
#include <binder/Status.h>
#include <cstdint>
#include <optional>
#include <string>
#include <tuple>
#include <utils/String16.h>
#include <vector>

namespace android {
namespace aidl {
namespace tests {
class LIBBINDER_EXPORTED StructuredParcelable : public ::android::Parcelable {
public:
  class LIBBINDER_EXPORTED Empty : public ::android::Parcelable {
  public:
    inline bool operator==(const Empty&) const {
      return std::tie() == std::tie();
    }
    inline bool operator<(const Empty&) const {
      return std::tie() < std::tie();
    }
    inline bool operator!=(const Empty& _rhs) const {
      return !(*this == _rhs);
    }
    inline bool operator>(const Empty& _rhs) const {
      return _rhs < *this;
    }
    inline bool operator>=(const Empty& _rhs) const {
      return !(*this < _rhs);
    }
    inline bool operator<=(const Empty& _rhs) const {
      return !(_rhs < *this);
    }

    ::android::status_t readFromParcel(const ::android::Parcel* _aidl_parcel) final;
    ::android::status_t writeToParcel(::android::Parcel* _aidl_parcel) const final;
    static const ::android::String16& getParcelableDescriptor() {
      static const ::android::StaticString16 DESCRIPTOR (u"android.aidl.tests.StructuredParcelable.Empty");
      return DESCRIPTOR;
    }
    inline std::string toString() const {
      std::ostringstream _aidl_os;
      _aidl_os << "Empty{";
      _aidl_os << "}";
      return _aidl_os.str();
    }
  };  // class Empty
  ::std::vector<int32_t> shouldContainThreeFs;
  int32_t f = 0;
  ::std::string shouldBeJerry;
  ::android::aidl::tests::ByteEnum shouldBeByteBar = ::android::aidl::tests::ByteEnum(0);
  ::android::aidl::tests::IntEnum shouldBeIntBar = ::android::aidl::tests::IntEnum(0);
  ::android::aidl::tests::LongEnum shouldBeLongBar = ::android::aidl::tests::LongEnum(0);
  ::std::vector<::android::aidl::tests::ByteEnum> shouldContainTwoByteFoos;
  ::std::vector<::android::aidl::tests::IntEnum> shouldContainTwoIntFoos;
  ::std::vector<::android::aidl::tests::LongEnum> shouldContainTwoLongFoos;
  ::android::String16 stringDefaultsToFoo = ::android::String16("foo");
  int8_t byteDefaultsToFour = 4;
  int32_t intDefaultsToFive = 5;
  int64_t longDefaultsToNegativeSeven = -7L;
  bool booleanDefaultsToTrue = true;
  char16_t charDefaultsToC = 'C';
  float floatDefaultsToPi = 3.140000f;
  double doubleWithDefault = -314000000000000000.000000;
  ::std::vector<int32_t> arrayDefaultsTo123 = {1, 2, 3};
  ::std::vector<int32_t> arrayDefaultsToEmpty = {};
  bool boolDefault = false;
  int8_t byteDefault = 0;
  int32_t intDefault = 0;
  int64_t longDefault = 0L;
  float floatDefault = 0.000000f;
  double doubleDefault = 0.000000;
  double checkDoubleFromFloat = 3.140000;
  ::std::vector<::android::String16> checkStringArray1 = {::android::String16("a"), ::android::String16("b")};
  ::std::vector<::std::string> checkStringArray2 = {"a", "b"};
  int32_t int32_min = -2147483648;
  int32_t int32_max = 2147483647;
  int64_t int64_max = 9223372036854775807L;
  int32_t hexInt32_neg_1 = -1;
  ::android::sp<::android::IBinder> ibinder;
  ::android::aidl::tests::StructuredParcelable::Empty empty;
  ::std::vector<uint8_t> int8_t_large = {uint8_t(-1), uint8_t(-64)};
  ::std::vector<int32_t> int32_t_large = {-1, -1073741824};
  ::std::vector<int64_t> int64_t_large = {-1L, -4611686018427387904L};
  ::std::vector<uint8_t> int8_1 = {1, 1, 1, 1, 1};
  ::std::vector<int32_t> int32_1 = {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1};
  ::std::vector<int64_t> int64_1 = {1L, 1L, 1L, 1L, 1L, 1L, 1L, 1L, 1L, 1L, 1L, 1L, 1L, 1L, 1L, 1L, 1L, 1L};
  int32_t hexInt32_pos_1 = 1;
  int32_t hexInt64_pos_1 = 1;
  ::android::aidl::tests::ConstantExpressionEnum const_exprs_1 = ::android::aidl::tests::ConstantExpressionEnum(0);
  ::android::aidl::tests::ConstantExpressionEnum const_exprs_2 = ::android::aidl::tests::ConstantExpressionEnum(0);
  ::android::aidl::tests::ConstantExpressionEnum const_exprs_3 = ::android::aidl::tests::ConstantExpressionEnum(0);
  ::android::aidl::tests::ConstantExpressionEnum const_exprs_4 = ::android::aidl::tests::ConstantExpressionEnum(0);
  ::android::aidl::tests::ConstantExpressionEnum const_exprs_5 = ::android::aidl::tests::ConstantExpressionEnum(0);
  ::android::aidl::tests::ConstantExpressionEnum const_exprs_6 = ::android::aidl::tests::ConstantExpressionEnum(0);
  ::android::aidl::tests::ConstantExpressionEnum const_exprs_7 = ::android::aidl::tests::ConstantExpressionEnum(0);
  ::android::aidl::tests::ConstantExpressionEnum const_exprs_8 = ::android::aidl::tests::ConstantExpressionEnum(0);
  ::android::aidl::tests::ConstantExpressionEnum const_exprs_9 = ::android::aidl::tests::ConstantExpressionEnum(0);
  ::android::aidl::tests::ConstantExpressionEnum const_exprs_10 = ::android::aidl::tests::ConstantExpressionEnum(0);
  ::std::string addString1 = "hello world!";
  ::std::string addString2 = "The quick brown fox jumps over the lazy dog.";
  int32_t shouldSetBit0AndBit2 = 0;
  ::std::optional<::android::aidl::tests::Union> u;
  ::std::optional<::android::aidl::tests::Union> shouldBeConstS1;
  ::android::aidl::tests::IntEnum defaultWithFoo = ::android::aidl::tests::IntEnum::FOO;
  inline bool operator==(const StructuredParcelable& _rhs) const {
    return std::tie(shouldContainThreeFs, f, shouldBeJerry, shouldBeByteBar, shouldBeIntBar, shouldBeLongBar, shouldContainTwoByteFoos, shouldContainTwoIntFoos, shouldContainTwoLongFoos, stringDefaultsToFoo, byteDefaultsToFour, intDefaultsToFive, longDefaultsToNegativeSeven, booleanDefaultsToTrue, charDefaultsToC, floatDefaultsToPi, doubleWithDefault, arrayDefaultsTo123, arrayDefaultsToEmpty, boolDefault, byteDefault, intDefault, longDefault, floatDefault, doubleDefault, checkDoubleFromFloat, checkStringArray1, checkStringArray2, int32_min, int32_max, int64_max, hexInt32_neg_1, ibinder, empty, int8_t_large, int32_t_large, int64_t_large, int8_1, int32_1, int64_1, hexInt32_pos_1, hexInt64_pos_1, const_exprs_1, const_exprs_2, const_exprs_3, const_exprs_4, const_exprs_5, const_exprs_6, const_exprs_7, const_exprs_8, const_exprs_9, const_exprs_10, addString1, addString2, shouldSetBit0AndBit2, u, shouldBeConstS1, defaultWithFoo) == std::tie(_rhs.shouldContainThreeFs, _rhs.f, _rhs.shouldBeJerry, _rhs.shouldBeByteBar, _rhs.shouldBeIntBar, _rhs.shouldBeLongBar, _rhs.shouldContainTwoByteFoos, _rhs.shouldContainTwoIntFoos, _rhs.shouldContainTwoLongFoos, _rhs.stringDefaultsToFoo, _rhs.byteDefaultsToFour, _rhs.intDefaultsToFive, _rhs.longDefaultsToNegativeSeven, _rhs.booleanDefaultsToTrue, _rhs.charDefaultsToC, _rhs.floatDefaultsToPi, _rhs.doubleWithDefault, _rhs.arrayDefaultsTo123, _rhs.arrayDefaultsToEmpty, _rhs.boolDefault, _rhs.byteDefault, _rhs.intDefault, _rhs.longDefault, _rhs.floatDefault, _rhs.doubleDefault, _rhs.checkDoubleFromFloat, _rhs.checkStringArray1, _rhs.checkStringArray2, _rhs.int32_min, _rhs.int32_max, _rhs.int64_max, _rhs.hexInt32_neg_1, _rhs.ibinder, _rhs.empty, _rhs.int8_t_large, _rhs.int32_t_large, _rhs.int64_t_large, _rhs.int8_1, _rhs.int32_1, _rhs.int64_1, _rhs.hexInt32_pos_1, _rhs.hexInt64_pos_1, _rhs.const_exprs_1, _rhs.const_exprs_2, _rhs.const_exprs_3, _rhs.const_exprs_4, _rhs.const_exprs_5, _rhs.const_exprs_6, _rhs.const_exprs_7, _rhs.const_exprs_8, _rhs.const_exprs_9, _rhs.const_exprs_10, _rhs.addString1, _rhs.addString2, _rhs.shouldSetBit0AndBit2, _rhs.u, _rhs.shouldBeConstS1, _rhs.defaultWithFoo);
  }
  inline bool operator<(const StructuredParcelable& _rhs) const {
    return std::tie(shouldContainThreeFs, f, shouldBeJerry, shouldBeByteBar, shouldBeIntBar, shouldBeLongBar, shouldContainTwoByteFoos, shouldContainTwoIntFoos, shouldContainTwoLongFoos, stringDefaultsToFoo, byteDefaultsToFour, intDefaultsToFive, longDefaultsToNegativeSeven, booleanDefaultsToTrue, charDefaultsToC, floatDefaultsToPi, doubleWithDefault, arrayDefaultsTo123, arrayDefaultsToEmpty, boolDefault, byteDefault, intDefault, longDefault, floatDefault, doubleDefault, checkDoubleFromFloat, checkStringArray1, checkStringArray2, int32_min, int32_max, int64_max, hexInt32_neg_1, ibinder, empty, int8_t_large, int32_t_large, int64_t_large, int8_1, int32_1, int64_1, hexInt32_pos_1, hexInt64_pos_1, const_exprs_1, const_exprs_2, const_exprs_3, const_exprs_4, const_exprs_5, const_exprs_6, const_exprs_7, const_exprs_8, const_exprs_9, const_exprs_10, addString1, addString2, shouldSetBit0AndBit2, u, shouldBeConstS1, defaultWithFoo) < std::tie(_rhs.shouldContainThreeFs, _rhs.f, _rhs.shouldBeJerry, _rhs.shouldBeByteBar, _rhs.shouldBeIntBar, _rhs.shouldBeLongBar, _rhs.shouldContainTwoByteFoos, _rhs.shouldContainTwoIntFoos, _rhs.shouldContainTwoLongFoos, _rhs.stringDefaultsToFoo, _rhs.byteDefaultsToFour, _rhs.intDefaultsToFive, _rhs.longDefaultsToNegativeSeven, _rhs.booleanDefaultsToTrue, _rhs.charDefaultsToC, _rhs.floatDefaultsToPi, _rhs.doubleWithDefault, _rhs.arrayDefaultsTo123, _rhs.arrayDefaultsToEmpty, _rhs.boolDefault, _rhs.byteDefault, _rhs.intDefault, _rhs.longDefault, _rhs.floatDefault, _rhs.doubleDefault, _rhs.checkDoubleFromFloat, _rhs.checkStringArray1, _rhs.checkStringArray2, _rhs.int32_min, _rhs.int32_max, _rhs.int64_max, _rhs.hexInt32_neg_1, _rhs.ibinder, _rhs.empty, _rhs.int8_t_large, _rhs.int32_t_large, _rhs.int64_t_large, _rhs.int8_1, _rhs.int32_1, _rhs.int64_1, _rhs.hexInt32_pos_1, _rhs.hexInt64_pos_1, _rhs.const_exprs_1, _rhs.const_exprs_2, _rhs.const_exprs_3, _rhs.const_exprs_4, _rhs.const_exprs_5, _rhs.const_exprs_6, _rhs.const_exprs_7, _rhs.const_exprs_8, _rhs.const_exprs_9, _rhs.const_exprs_10, _rhs.addString1, _rhs.addString2, _rhs.shouldSetBit0AndBit2, _rhs.u, _rhs.shouldBeConstS1, _rhs.defaultWithFoo);
  }
  inline bool operator!=(const StructuredParcelable& _rhs) const {
    return !(*this == _rhs);
  }
  inline bool operator>(const StructuredParcelable& _rhs) const {
    return _rhs < *this;
  }
  inline bool operator>=(const StructuredParcelable& _rhs) const {
    return !(*this < _rhs);
  }
  inline bool operator<=(const StructuredParcelable& _rhs) const {
    return !(_rhs < *this);
  }

  enum : int32_t { BIT0 = 1 };
  enum : int32_t { BIT1 = 2 };
  enum : int32_t { BIT2 = 4 };
  ::android::status_t readFromParcel(const ::android::Parcel* _aidl_parcel) final;
  ::android::status_t writeToParcel(::android::Parcel* _aidl_parcel) const final;
  static const ::android::String16& getParcelableDescriptor() {
    static const ::android::StaticString16 DESCRIPTOR (u"android.aidl.tests.StructuredParcelable");
    return DESCRIPTOR;
  }
  inline std::string toString() const {
    std::ostringstream _aidl_os;
    _aidl_os << "StructuredParcelable{";
    _aidl_os << "shouldContainThreeFs: " << ::android::internal::ToString(shouldContainThreeFs);
    _aidl_os << ", f: " << ::android::internal::ToString(f);
    _aidl_os << ", shouldBeJerry: " << ::android::internal::ToString(shouldBeJerry);
    _aidl_os << ", shouldBeByteBar: " << ::android::internal::ToString(shouldBeByteBar);
    _aidl_os << ", shouldBeIntBar: " << ::android::internal::ToString(shouldBeIntBar);
    _aidl_os << ", shouldBeLongBar: " << ::android::internal::ToString(shouldBeLongBar);
    _aidl_os << ", shouldContainTwoByteFoos: " << ::android::internal::ToString(shouldContainTwoByteFoos);
    _aidl_os << ", shouldContainTwoIntFoos: " << ::android::internal::ToString(shouldContainTwoIntFoos);
    _aidl_os << ", shouldContainTwoLongFoos: " << ::android::internal::ToString(shouldContainTwoLongFoos);
    _aidl_os << ", stringDefaultsToFoo: " << ::android::internal::ToString(stringDefaultsToFoo);
    _aidl_os << ", byteDefaultsToFour: " << ::android::internal::ToString(byteDefaultsToFour);
    _aidl_os << ", intDefaultsToFive: " << ::android::internal::ToString(intDefaultsToFive);
    _aidl_os << ", longDefaultsToNegativeSeven: " << ::android::internal::ToString(longDefaultsToNegativeSeven);
    _aidl_os << ", booleanDefaultsToTrue: " << ::android::internal::ToString(booleanDefaultsToTrue);
    _aidl_os << ", charDefaultsToC: " << ::android::internal::ToString(charDefaultsToC);
    _aidl_os << ", floatDefaultsToPi: " << ::android::internal::ToString(floatDefaultsToPi);
    _aidl_os << ", doubleWithDefault: " << ::android::internal::ToString(doubleWithDefault);
    _aidl_os << ", arrayDefaultsTo123: " << ::android::internal::ToString(arrayDefaultsTo123);
    _aidl_os << ", arrayDefaultsToEmpty: " << ::android::internal::ToString(arrayDefaultsToEmpty);
    _aidl_os << ", boolDefault: " << ::android::internal::ToString(boolDefault);
    _aidl_os << ", byteDefault: " << ::android::internal::ToString(byteDefault);
    _aidl_os << ", intDefault: " << ::android::internal::ToString(intDefault);
    _aidl_os << ", longDefault: " << ::android::internal::ToString(longDefault);
    _aidl_os << ", floatDefault: " << ::android::internal::ToString(floatDefault);
    _aidl_os << ", doubleDefault: " << ::android::internal::ToString(doubleDefault);
    _aidl_os << ", checkDoubleFromFloat: " << ::android::internal::ToString(checkDoubleFromFloat);
    _aidl_os << ", checkStringArray1: " << ::android::internal::ToString(checkStringArray1);
    _aidl_os << ", checkStringArray2: " << ::android::internal::ToString(checkStringArray2);
    _aidl_os << ", int32_min: " << ::android::internal::ToString(int32_min);
    _aidl_os << ", int32_max: " << ::android::internal::ToString(int32_max);
    _aidl_os << ", int64_max: " << ::android::internal::ToString(int64_max);
    _aidl_os << ", hexInt32_neg_1: " << ::android::internal::ToString(hexInt32_neg_1);
    _aidl_os << ", ibinder: " << ::android::internal::ToString(ibinder);
    _aidl_os << ", empty: " << ::android::internal::ToString(empty);
    _aidl_os << ", int8_t_large: " << ::android::internal::ToString(int8_t_large);
    _aidl_os << ", int32_t_large: " << ::android::internal::ToString(int32_t_large);
    _aidl_os << ", int64_t_large: " << ::android::internal::ToString(int64_t_large);
    _aidl_os << ", int8_1: " << ::android::internal::ToString(int8_1);
    _aidl_os << ", int32_1: " << ::android::internal::ToString(int32_1);
    _aidl_os << ", int64_1: " << ::android::internal::ToString(int64_1);
    _aidl_os << ", hexInt32_pos_1: " << ::android::internal::ToString(hexInt32_pos_1);
    _aidl_os << ", hexInt64_pos_1: " << ::android::internal::ToString(hexInt64_pos_1);
    _aidl_os << ", const_exprs_1: " << ::android::internal::ToString(const_exprs_1);
    _aidl_os << ", const_exprs_2: " << ::android::internal::ToString(const_exprs_2);
    _aidl_os << ", const_exprs_3: " << ::android::internal::ToString(const_exprs_3);
    _aidl_os << ", const_exprs_4: " << ::android::internal::ToString(const_exprs_4);
    _aidl_os << ", const_exprs_5: " << ::android::internal::ToString(const_exprs_5);
    _aidl_os << ", const_exprs_6: " << ::android::internal::ToString(const_exprs_6);
    _aidl_os << ", const_exprs_7: " << ::android::internal::ToString(const_exprs_7);
    _aidl_os << ", const_exprs_8: " << ::android::internal::ToString(const_exprs_8);
    _aidl_os << ", const_exprs_9: " << ::android::internal::ToString(const_exprs_9);
    _aidl_os << ", const_exprs_10: " << ::android::internal::ToString(const_exprs_10);
    _aidl_os << ", addString1: " << ::android::internal::ToString(addString1);
    _aidl_os << ", addString2: " << ::android::internal::ToString(addString2);
    _aidl_os << ", shouldSetBit0AndBit2: " << ::android::internal::ToString(shouldSetBit0AndBit2);
    _aidl_os << ", u: " << ::android::internal::ToString(u);
    _aidl_os << ", shouldBeConstS1: " << ::android::internal::ToString(shouldBeConstS1);
    _aidl_os << ", defaultWithFoo: " << ::android::internal::ToString(defaultWithFoo);
    _aidl_os << "}";
    return _aidl_os.str();
  }
};  // class StructuredParcelable
}  // namespace tests
}  // namespace aidl
}  // namespace android
