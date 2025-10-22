/*
 * Copyright (C) 2020 The Android Open Source Project
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
package android.tzdata.mts;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertTrue;
import static org.junit.Assert.fail;
import static org.junit.Assume.assumeTrue;

import static java.util.stream.Collectors.toMap;

import android.icu.util.VersionInfo;
import android.os.Build;
import android.util.TimeUtils;

import com.android.i18n.timezone.TzDataSetVersion;

import org.junit.Test;

import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;
import java.io.UncheckedIOException;
import java.lang.reflect.Method;
import java.nio.charset.StandardCharsets;
import java.util.HashSet;
import java.util.Map;
import java.util.Set;

/**
 * Tests concerning version information associated with, or affected by, the time zone data module.
 *
 * <p>Generally we don't want to assert anything too specific here (like exact version), since that
 * would mean more to update every tzdb release. Also, if the module being tested contains an old
 * version then why wouldn't the tests be just as old too?
 */
public class TimeZoneVersionTest {

    private static final File TIME_ZONE_MODULE_VERSION_FILE =
            new File("/apex/com.android.tzdata/etc/tz/tz_version");

    private static final String VERSIONED_DATA_LOCATION =
            "/apex/com.android.tzdata/etc/tz/versioned/";

    // Android V.
    private static final int MINIMAL_SUPPORTED_MAJOR_VERSION = 8;
    // Android B.
    // LINT.IfChange
    private static final int THE_LATEST_MAJOR_VERSION = 9;
    // LINT.ThenChange(/android_icu4j/libcore_bridge/src/java/com/android/i18n/timezone/TzDataSetVersion.java)

    @Test
    public void timeZoneModuleIsCompatibleWithThisRelease() {
        String majorVersion = readMajorFormatVersionForVersion(getCurrentFormatMajorVersion());

        // Each time a release version of Android is declared, this list needs to be updated to
        // map the Android release to the time zone format version it uses.
        if (Build.VERSION.SDK_INT == Build.VERSION_CODES.Q) {
            assertEquals("003", majorVersion);
        } else if (Build.VERSION.SDK_INT == Build.VERSION_CODES.R) {
            assertEquals("004", majorVersion);
        } else if (Build.VERSION.SDK_INT == Build.VERSION_CODES.S) {
            assertEquals("005", majorVersion);
        } else if (Build.VERSION.SDK_INT == Build.VERSION_CODES.S_V2) {
            // S_V2 is 5.x, as the format version did not change from S.
            assertEquals("005", majorVersion);
        } else if (Build.VERSION.SDK_INT == Build.VERSION_CODES.TIRAMISU) {
            assertEquals("006", majorVersion);
        } else if (Build.VERSION.SDK_INT == Build.VERSION_CODES.UPSIDE_DOWN_CAKE) {
            assertEquals("007", majorVersion);
        } else if (Build.VERSION.SDK_INT == Build.VERSION_CODES.VANILLA_ICE_CREAM) {
            assertEquals("008", majorVersion);
        } else if (Build.VERSION.SDK_INT == Build.VERSION_CODES.BAKLAVA) {
            // The "main" branch is also the staging area for the next Android release that won't
            // have an Android release constant yet. Instead, we have to infer what the expected tz
            // data set version should be when the SDK_INT identifies it as the latest Android release
            // in case it is actually the "main" branch. Below we assume that an increment to ICU is
            // involved with each release of Android and requires an tz data set version increment.


            // ICU version in B is 76. When we update it in a next release major version
            // should be updated too.
            if (VersionInfo.ICU_VERSION.getMajor() > 76) {
                assertEquals("010", majorVersion);
            } else {
                assertEquals("009", majorVersion);
            }
        } else {
            // If this fails, a new API level has likely been finalized and can be made
            // an explicit case. Keep this clause and add an explicit "else if" above.
            // Consider removing any checks for pre-release devices too if they're not
            // needed for now.
            fail("Unhandled SDK_INT version:" + Build.VERSION.SDK_INT);
        }
    }

    /**
     * Confirms that tzdb version information available via published APIs is consistent.
     */
    @Test
    public void tzdbVersionIsConsistentAcrossApis() {
        String tzModuleTzdbVersion = readTzDbVersionFromModuleVersionFile();

        String icu4jTzVersion = android.icu.util.TimeZone.getTZDataVersion();
        assertEquals(tzModuleTzdbVersion, icu4jTzVersion);

        assertEquals(tzModuleTzdbVersion, TimeUtils.getTimeZoneDatabaseVersion());
    }

    @Test
    public void majorVersion_isValid() {
        String msg = "THE_LATEST_MAJOR_VERSION is "
                + THE_LATEST_MAJOR_VERSION
                + " but getCurrentMajorFormatVersion() is greater: "
                + getCurrentFormatMajorVersion();
        assertTrue(msg, THE_LATEST_MAJOR_VERSION >= getCurrentFormatMajorVersion());
    }

    @Test
    public void versionFiles_areConsistent() {
        // Test validates data installed in /versioned/ directory. It was introduced in tzdata6,
        // and it is targeted to Android V+ only.
        assumeTrue(Build.VERSION.SDK_INT >= Build.VERSION_CODES.VANILLA_ICE_CREAM);

        // Version in tz_version under versioned/N should be N.
        for (int version = MINIMAL_SUPPORTED_MAJOR_VERSION;
             version <= THE_LATEST_MAJOR_VERSION;
             ++version) {
            // Version in tz_version is zero padded.
            String expectedVersion = "%03d".formatted(version);
            assertEquals(expectedVersion, readMajorFormatVersionForVersion(version));
        }

        // IANA version should be the same across tz_version files.
        Set<File> versionFiles = new HashSet<>();
        versionFiles.add(TIME_ZONE_MODULE_VERSION_FILE);

        for (int version = MINIMAL_SUPPORTED_MAJOR_VERSION;
             version <= THE_LATEST_MAJOR_VERSION;
             ++version) {
            versionFiles.add(
                    new File("%s/%d/tz_version".formatted(VERSIONED_DATA_LOCATION, version)));
        }

        Map<String, String> ianaVersionInVersionFile = versionFiles.stream()
                .collect(toMap(File::toString, TimeZoneVersionTest::readTzDbVersionFrom));

        String msg = "Versions are not consistent: " + ianaVersionInVersionFile;
        assertEquals(msg, 1, Set.of(ianaVersionInVersionFile.values()).size());
    }

    private static int getCurrentFormatMajorVersion() {
        // TzDataSetVersion was moved from /libcore to /external/icu in S.
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            return TzDataSetVersion.currentFormatMajorVersion();
        } else {
            try {
                Class<?> libcoreTzDataSetVersion =
                        Class.forName("libcore.timezone.TzDataSetVersion");
                Method m = libcoreTzDataSetVersion.getDeclaredMethod("currentFormatMajorVersion");
                m.setAccessible(true);
                return (int) m.invoke(null);
            } catch (ReflectiveOperationException roe) {
                throw new AssertionError(roe);
            }
        }
    }

    /**
     * Reads up to {@code maxBytes} bytes from the specified file. The returned array can be
     * shorter than {@code maxBytes} if the file is shorter.
     */
    private static byte[] readBytes(File file, int maxBytes) {
        if (maxBytes <= 0) {
            throw new IllegalArgumentException("maxBytes ==" + maxBytes);
        }

        try (FileInputStream in = new FileInputStream(file)) {
            byte[] max = new byte[maxBytes];
            int bytesRead = in.read(max, 0, maxBytes);
            byte[] toReturn = new byte[bytesRead];
            System.arraycopy(max, 0, toReturn, 0, bytesRead);
            return toReturn;
        } catch (IOException ioe) {
            throw new UncheckedIOException(ioe);
        }
    }

    private static String readTzDbVersionFrom(File file) {
        byte[] versionBytes = readBytes(file, 13);
        assertEquals(13, versionBytes.length);

        String versionString = new String(versionBytes, StandardCharsets.US_ASCII);
        // Format is: xxx.yyy|zzzzz|...., we want zzzzz
        String[] dataSetVersionComponents = versionString.split("\\|");
        return dataSetVersionComponents[1];
    }

    private static String readTzDbVersionFromModuleVersionFile() {
        return readTzDbVersionFrom(TIME_ZONE_MODULE_VERSION_FILE);
    }

    private static String readMajorFormatVersionFrom(File file) {
        byte[] versionBytes = readBytes(file, 7);
        assertEquals(7, versionBytes.length);

        String versionString = new String(versionBytes, StandardCharsets.US_ASCII);
        // Format is: xxx.yyy|zzzz|.... we want xxx
        String[] dataSetVersionComponents = versionString.split("\\.");
        return dataSetVersionComponents[0];
    }

    private static String readMajorFormatVersionForVersion(int version) {
        File tzVersion;
        if (version >= 8) {
            tzVersion = new File(
                    "%s/%d/tz_version".formatted(VERSIONED_DATA_LOCATION, version));
        } else {
            tzVersion = TIME_ZONE_MODULE_VERSION_FILE;
        }
        return readMajorFormatVersionFrom(tzVersion);
    }
}
