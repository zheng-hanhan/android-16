/*
 * This file is auto-generated.  DO NOT MODIFY.
 * Using: out/host/linux-x86/bin/aidl --lang=ndk -Weverything -Wno-missing-permission-annotation -Werror -t --min_sdk_version current --ninja -d out/soong/.intermediates/system/tools/aidl/aidl-test-interface-ndk-source/gen/staging/android/aidl/tests/IntEnum.cpp.d -h out/soong/.intermediates/system/tools/aidl/aidl-test-interface-ndk-source/gen/include/staging -o out/soong/.intermediates/system/tools/aidl/aidl-test-interface-ndk-source/gen/staging -Nsystem/tools/aidl/tests system/tools/aidl/tests/android/aidl/tests/IntEnum.aidl
 *
 * DO NOT CHECK THIS FILE INTO A CODE TREE (e.g. git, etc..).
 * ALWAYS GENERATE THIS FILE FROM UPDATED AIDL COMPILER
 * AS A BUILD INTERMEDIATE ONLY. THIS IS NOT SOURCE CODE.
 */
#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <android/binder_enums.h>
#ifdef BINDER_STABILITY_SUPPORT
#include <android/binder_stability.h>
#endif  // BINDER_STABILITY_SUPPORT

namespace aidl {
namespace android {
namespace aidl {
namespace tests {
enum class IntEnum : int32_t {
  ZERO = 0,
  ONE = 1,
  TWO = 2,
  FOO = 1000,
  BAR = 2000,
  BAZ = 2001,
  QUX __attribute__((deprecated("do not use this"))) = 2002,
};

}  // namespace tests
}  // namespace aidl
}  // namespace android
}  // namespace aidl
namespace aidl {
namespace android {
namespace aidl {
namespace tests {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
[[nodiscard]] static inline std::string toString(IntEnum val) {
  switch(val) {
  case IntEnum::ZERO:
    return "ZERO";
  case IntEnum::ONE:
    return "ONE";
  case IntEnum::TWO:
    return "TWO";
  case IntEnum::FOO:
    return "FOO";
  case IntEnum::BAR:
    return "BAR";
  case IntEnum::BAZ:
    return "BAZ";
  case IntEnum::QUX:
    return "QUX";
  default:
    return std::to_string(static_cast<int32_t>(val));
  }
}
#pragma clang diagnostic pop
}  // namespace tests
}  // namespace aidl
}  // namespace android
}  // namespace aidl
namespace ndk {
namespace internal {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wc++17-extensions"
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
template <>
constexpr inline std::array<aidl::android::aidl::tests::IntEnum, 7> enum_values<aidl::android::aidl::tests::IntEnum> = {
  aidl::android::aidl::tests::IntEnum::ZERO,
  aidl::android::aidl::tests::IntEnum::ONE,
  aidl::android::aidl::tests::IntEnum::TWO,
  aidl::android::aidl::tests::IntEnum::FOO,
  aidl::android::aidl::tests::IntEnum::BAR,
  aidl::android::aidl::tests::IntEnum::BAZ,
  aidl::android::aidl::tests::IntEnum::QUX,
};
#pragma clang diagnostic pop
}  // namespace internal
}  // namespace ndk
