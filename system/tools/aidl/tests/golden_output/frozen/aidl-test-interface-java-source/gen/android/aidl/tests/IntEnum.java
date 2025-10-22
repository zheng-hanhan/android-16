/*
 * This file is auto-generated.  DO NOT MODIFY.
 * Using: out/host/linux-x86/bin/aidl --lang=java -Weverything -Wno-missing-permission-annotation -Werror -t --min_sdk_version platform_apis --ninja -d out/soong/.intermediates/system/tools/aidl/aidl-test-interface-java-source/gen/android/aidl/tests/IntEnum.java.d -o out/soong/.intermediates/system/tools/aidl/aidl-test-interface-java-source/gen -Nsystem/tools/aidl/tests system/tools/aidl/tests/android/aidl/tests/IntEnum.aidl
 *
 * DO NOT CHECK THIS FILE INTO A CODE TREE (e.g. git, etc..).
 * ALWAYS GENERATE THIS FILE FROM UPDATED AIDL COMPILER
 * AS A BUILD INTERMEDIATE ONLY. THIS IS NOT SOURCE CODE.
 */
package android.aidl.tests;
public @interface IntEnum {
  public static final int ZERO = 0;
  public static final int ONE = 1;
  public static final int TWO = 2;
  /**
   * Reserved: 12 and 2040
   * We are using 12 and (FOO | BAR) in some tests because
   * they _are not_ defined in this enum.
   * Please do not add them here.
   */
  public static final int FOO = 1000;
  public static final int BAR = 2000;
  public static final int BAZ = 2001;
  /** @deprecated do not use this */
  @Deprecated
  public static final int QUX = 2002;
  interface $ {
    static String toString(int _aidl_v) {
      if (_aidl_v == ZERO) return "ZERO";
      if (_aidl_v == ONE) return "ONE";
      if (_aidl_v == TWO) return "TWO";
      if (_aidl_v == FOO) return "FOO";
      if (_aidl_v == BAR) return "BAR";
      if (_aidl_v == BAZ) return "BAZ";
      if (_aidl_v == QUX) return "QUX";
      return Integer.toString(_aidl_v);
    }
    static String arrayToString(Object _aidl_v) {
      if (_aidl_v == null) return "null";
      Class<?> _aidl_cls = _aidl_v.getClass();
      if (!_aidl_cls.isArray()) throw new IllegalArgumentException("not an array: " + _aidl_v);
      Class<?> comp = _aidl_cls.getComponentType();
      java.util.StringJoiner _aidl_sj = new java.util.StringJoiner(", ", "[", "]");
      if (comp.isArray()) {
        for (int _aidl_i = 0; _aidl_i < java.lang.reflect.Array.getLength(_aidl_v); _aidl_i++) {
          _aidl_sj.add(arrayToString(java.lang.reflect.Array.get(_aidl_v, _aidl_i)));
        }
      } else {
        if (_aidl_cls != int[].class) throw new IllegalArgumentException("wrong type: " + _aidl_cls);
        for (int e : (int[]) _aidl_v) {
          _aidl_sj.add(toString(e));
        }
      }
      return _aidl_sj.toString();
    }
  }
}
