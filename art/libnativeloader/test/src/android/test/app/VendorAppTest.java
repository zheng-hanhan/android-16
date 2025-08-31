/*
 * Copyright (C) 2022 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

package android.test.app;

import android.test.lib.AppTestCommon;
import android.test.lib.TestUtils;
import android.test.productsharedlib.ProductSharedLib;
import android.test.systemextsharedlib.SystemExtSharedLib;
import android.test.systemsharedlib.SystemSharedLib;
import android.test.vendorsharedlib.VendorSharedLib;

import androidx.test.filters.MediumTest;

import org.junit.Test;

@MediumTest
public class VendorAppTest extends AppTestCommon {
    @Override
    public AppLocation getAppLocation() {
        return AppLocation.VENDOR;
    }

    @Test
    public void testPrivateLibsExist() {
        TestUtils.testPrivateLibsExist("/vendor", "vendor_private");
    }

    @Test
    public void testLoadExtendedPublicLibraries() {
        TestUtils.assertLibraryInaccessible(() -> System.loadLibrary("system_extpub.oem1"));
        TestUtils.assertLibraryInaccessible(() -> System.loadLibrary("system_extpub.oem2"));
        TestUtils.assertLibraryInaccessible(() -> System.loadLibrary("system_extpub1.oem1"));
        TestUtils.assertLibraryInaccessible(() -> System.loadLibrary("system_extpub_nouses.oem2"));
        if (!TestUtils.skipPublicProductLibTests()) {
            System.loadLibrary("product_extpub.product1");
            System.loadLibrary("product_extpub1.product1");
        }
    }

    @Test
    public void testLoadPrivateLibraries() {
        TestUtils.assertLibraryInaccessible(() -> System.loadLibrary("system_private1"));
        TestUtils.assertLibraryInaccessible(() -> System.loadLibrary("systemext_private1"));
        if (!TestUtils.skipPublicProductLibTests()) {
            TestUtils.assertLibraryInaccessible(() -> System.loadLibrary("product_private1"));
        }
        System.loadLibrary("vendor_private1");
    }

    @Test
    public void testLoadExtendedPublicLibrariesViaSystemSharedLib() {
        SystemSharedLib.loadLibrary("system_extpub2.oem1");
        if (!TestUtils.skipPublicProductLibTests()) {
            SystemSharedLib.loadLibrary("product_extpub2.product1");
        }
    }

    @Test
    public void testLoadPrivateLibrariesViaSystemSharedLib() {
        if (TestUtils.canLoadPrivateLibsFromSamePartition()) {
            // TODO(b/186729817): These loads work because the findLibrary call in
            // loadLibrary0 in Runtime.java searches the system libs and converts
            // them to absolute paths.
            SystemSharedLib.loadLibrary("system_private2");
            SystemSharedLib.loadLibrary("systemext_private2");
        } else {
            TestUtils.assertLibraryInaccessible(
                    () -> SystemSharedLib.loadLibrary("system_private2"));
            TestUtils.assertLibraryInaccessible(
                    () -> SystemSharedLib.loadLibrary("systemext_private2"));
        }

        if (!TestUtils.skipPublicProductLibTests()) {
            TestUtils.assertLibraryInaccessible(
                    () -> SystemSharedLib.loadLibrary("product_private2"));
        }

        TestUtils.assertLibraryInaccessible(() -> SystemSharedLib.loadLibrary("vendor_private2"));
    }

    @Test
    public void testLoadPrivateLibrariesViaSystemExtSharedLib() {
        if (TestUtils.canLoadPrivateLibsFromSamePartition()) {
            // TODO(b/186729817): These loads work because the findLibrary call in
            // loadLibrary0 in Runtime.java searches the system libs and converts
            // them to absolute paths.
            SystemExtSharedLib.loadLibrary("system_private3");
            SystemExtSharedLib.loadLibrary("systemext_private3");
        } else {
            TestUtils.assertLibraryInaccessible(
                    () -> SystemExtSharedLib.loadLibrary("system_private3"));
            TestUtils.assertLibraryInaccessible(
                    () -> SystemExtSharedLib.loadLibrary("systemext_private3"));
        }

        if (!TestUtils.skipPublicProductLibTests()) {
            TestUtils.assertLibraryInaccessible(
                    () -> SystemExtSharedLib.loadLibrary("product_private3"));
        }

        TestUtils.assertLibraryInaccessible(
                () -> SystemExtSharedLib.loadLibrary("vendor_private3"));
    }

    @Test
    public void testLoadPrivateLibrariesViaProductSharedLib() {
        TestUtils.assertLibraryInaccessible(() -> ProductSharedLib.loadLibrary("system_private4"));
        TestUtils.assertLibraryInaccessible(
                () -> ProductSharedLib.loadLibrary("systemext_private4"));

        if (!TestUtils.skipPublicProductLibTests()) {
            ProductSharedLib.loadLibrary("product_private4");
        }

        TestUtils.assertLibraryInaccessible(() -> ProductSharedLib.loadLibrary("vendor_private4"));
    }

    @Test
    public void testLoadPrivateLibrariesViaVendorSharedLib() {
        TestUtils.assertLibraryInaccessible(() -> VendorSharedLib.loadLibrary("system_private5"));
        TestUtils.assertLibraryInaccessible(
                () -> VendorSharedLib.loadLibrary("systemext_private5"));

        if (!TestUtils.skipPublicProductLibTests()) {
            TestUtils.assertLibraryInaccessible(
                    () -> VendorSharedLib.loadLibrary("product_private5"));
        }

        // Can load vendor_private5 by name only through the app classloader namespace.
        VendorSharedLib.loadLibrary("vendor_private5");
    }

    @Test
    public void testLoadExtendedPublicLibrariesWithAbsolutePaths() {
        TestUtils.assertLibraryInaccessible(
                () -> System.load(TestUtils.libPath("/system", "system_extpub3.oem1")));
        if (!TestUtils.skipPublicProductLibTests()) {
            System.load(TestUtils.libPath("/product", "product_extpub3.product1"));
        }
    }

    @Test
    public void testLoadPrivateLibrariesWithAbsolutePaths() {
        TestUtils.assertLibraryInaccessible(
                () -> System.load(TestUtils.libPath("/system", "system_private6")));
        TestUtils.assertLibraryInaccessible(
                () -> System.load(TestUtils.libPath("/system_ext", "systemext_private6")));
        if (!TestUtils.skipPublicProductLibTests()) {
            TestUtils.assertLibraryInaccessible(
                    () -> System.load(TestUtils.libPath("/product", "product_private6")));
        }
        System.load(TestUtils.libPath("/vendor", "vendor_private6"));
    }
}
