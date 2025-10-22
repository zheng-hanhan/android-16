/*
 * Copyright (C) 2019 The Android Open Source Project
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

package com.android.libcore.timezone.telephonylookup;

import static com.android.libcore.timezone.testing.TestUtils.assertContains;
import static com.android.libcore.timezone.testing.TestUtils.createFile;

import static junit.framework.Assert.assertEquals;

import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertTrue;

import com.android.libcore.timezone.telephonylookup.proto.TelephonyLookupProtoFile.MobileCountry;
import com.android.libcore.timezone.telephonylookup.proto.TelephonyLookupProtoFile.Network;
import com.android.libcore.timezone.telephonylookup.proto.TelephonyLookupProtoFile.TelephonyLookup;
import com.android.libcore.timezone.testing.TestUtils;

import com.google.protobuf.TextFormat;

import org.junit.After;
import org.junit.Before;
import org.junit.Test;

import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.Paths;
import java.util.List;
import java.util.stream.Collectors;

public class TelephonyLookupGeneratorTest {

    private Path tempDir;

    @Before
    public void setUp() throws Exception {
        tempDir = Files.createTempDirectory("TelephonyLookupGeneratorTest");
    }

    @After
    public void tearDown() throws Exception {
        TestUtils.deleteDir(tempDir);
    }

    @Test
    public void invalidTelephonyLookupFile() throws Exception {
        String telephonyLookupFile = createFile(tempDir, "THIS IS NOT A VALID FILE");
        String outputFile = Files.createTempFile(tempDir, "out", null /* suffix */).toString();

        TelephonyLookupGenerator telephonyLookupGenerator =
                new TelephonyLookupGenerator(telephonyLookupFile, outputFile);
        assertFalse(telephonyLookupGenerator.execute());

        assertFileIsEmpty(outputFile);
    }

    @Test
    public void networks_upperCaseCountryIsoCodeIsRejected() throws Exception {
        Network network = createNetwork("123", "456", "GB");
        checkGenerationFails(createTelephonyLookup(List.of(network), List.of()));
    }

    @Test
    public void networks_unknownCountryIsoCodeIsRejected() throws Exception {
        Network network = createNetwork("123", "456", "zx");
        checkGenerationFails(createTelephonyLookup(List.of(network), List.of()));
    }

    @Test
    public void networks_badMccIsRejected_nonNumeric() throws Exception {
        Network network = createNetwork("XXX", "456", "gb");
        checkGenerationFails(createTelephonyLookup(List.of(network), List.of()));
    }

    @Test
    public void networks_badMccIsRejected_tooShort() throws Exception {
        Network network = createNetwork("12", "456", "gb");
        checkGenerationFails(createTelephonyLookup(List.of(network), List.of()));
    }

    @Test
    public void networks_badMccIsRejected_tooLong() throws Exception {
        Network network = createNetwork("1234", "567", "gb");
        checkGenerationFails(createTelephonyLookup(List.of(network), List.of()));
    }

    @Test
    public void networks_badMncIsRejected_nonNumeric() throws Exception {
        Network network = createNetwork("123", "XXX", "gb");
        checkGenerationFails(createTelephonyLookup(List.of(network), List.of()));
    }

    @Test
    public void networks_badMncIsRejected_tooShort() throws Exception {
        Network network = createNetwork("123", "4", "gb");
        checkGenerationFails(createTelephonyLookup(List.of(network), List.of()));
    }

    @Test
    public void networks_badMncIsRejected_tooLong() throws Exception {
        Network network = createNetwork("123", "4567", "gb");
        checkGenerationFails(createTelephonyLookup(List.of(network), List.of()));
    }

    @Test
    public void networks_duplicateMccMncComboIsRejected() throws Exception {
        Network network1 = createNetwork("123", "456", "gb");
        Network network2 = createNetwork("123", "456", "us");
        checkGenerationFails(createTelephonyLookup(List.of(network1, network2), List.of()));
    }

    @Test
    public void mobileCountries_upperCaseCountryIsoCodeIsRejected() throws Exception {
        MobileCountry mobileCountry = createMobileCountry("123", List.of("GB"));
        checkGenerationFails(createTelephonyLookup(List.of(), List.of(mobileCountry)));
    }

    @Test
    public void mobileCountries_unknownCountryIsoCodeIsRejected() throws Exception {
        MobileCountry mobileCountry = createMobileCountry("123", List.of("gb", "zx"));
        checkGenerationFails(createTelephonyLookup(List.of(), List.of(mobileCountry)));
    }

    @Test
    public void mobileCountries_badMccIsRejected_nonNumeric() throws Exception {
        MobileCountry mobileCountry = createMobileCountry("XXX", List.of("gb"));
        checkGenerationFails(createTelephonyLookup(List.of(), List.of(mobileCountry)));
    }

    @Test
    public void mobileCountries_badMccIsRejected_tooShort() throws Exception {
        MobileCountry mobileCountry = createMobileCountry("12", List.of("gb"));
        checkGenerationFails(createTelephonyLookup(List.of(), List.of(mobileCountry)));
    }

    @Test
    public void mobileCountries_badMccIsRejected_tooLong() throws Exception {
        MobileCountry mobileCountry = createMobileCountry("1234", List.of("gb"));
        checkGenerationFails(createTelephonyLookup(List.of(), List.of(mobileCountry)));
    }

    @Test
    public void mobileCountries_duplicateMccComboIsRejected() throws Exception {
        MobileCountry mobileCountry1 = createMobileCountry("123", List.of("gb"));
        MobileCountry mobileCountry2 = createMobileCountry("123", List.of("gb"));
        checkGenerationFails(
                createTelephonyLookup(List.of(), List.of(mobileCountry1, mobileCountry2)));
    }

    @Test
    public void validDataCreatesFile() throws Exception {
        Network network1 = createNetwork("123", "456", "gb");
        Network network2 = createNetwork("123", "56", "us");
        MobileCountry mobileCountry1 = createMobileCountry("123", List.of("gb"));
        MobileCountry mobileCountry2 = createMobileCountry("456", List.of("us", "fr"));
        TelephonyLookup telephonyLookupProto =
                createTelephonyLookup(List.of(network1, network2),
                        List.of(mobileCountry1, mobileCountry2));

        String telephonyLookupXml = generateTelephonyLookupXml(telephonyLookupProto);
    assertContains(
        trimAndLinearize(telephonyLookupXml),
        "<network mcc=\"123\" mnc=\"456\" country=\"gb\"/>",
        "<network mcc=\"123\" mnc=\"56\" country=\"us\"/>",
        "<mobile_country mcc=\"123\"><country>gb</country></mobile_country>",
        "<mobile_country mcc=\"456\""
            + " default=\"us\"><country>us</country><country>fr</country></mobile_country>");
    }

    private void checkGenerationFails(TelephonyLookup telephonyLookup)
            throws Exception {
        String telephonyLookupFile = createTelephonyLookupFile(telephonyLookup);
        String outputFile = Files.createTempFile(tempDir, "out", null /* suffix */).toString();

        TelephonyLookupGenerator telephonyLookupGenerator =
                new TelephonyLookupGenerator(telephonyLookupFile, outputFile);
        assertFalse(telephonyLookupGenerator.execute());

        assertFileIsEmpty(outputFile);
    }

    private String createTelephonyLookupFile(
            TelephonyLookup telephonyLookup) throws Exception {
        return TestUtils.createFile(tempDir, TextFormat.printToString(telephonyLookup));
    }

    private String generateTelephonyLookupXml(
            TelephonyLookup telephonyLookup) throws Exception {
        String telephonyLookupFile = createTelephonyLookupFile(telephonyLookup);

        String outputFile = Files.createTempFile(tempDir, "out", null /* suffix */).toString();

        TelephonyLookupGenerator telephonyLookupGenerator =
                new TelephonyLookupGenerator(telephonyLookupFile, outputFile);
        assertTrue(telephonyLookupGenerator.execute());

        Path outputFilePath = Paths.get(outputFile);
        assertTrue(Files.exists(outputFilePath));

        return readFileToString(outputFilePath);
    }

    private static String readFileToString(Path file) throws IOException {
        return new String(Files.readAllBytes(file), StandardCharsets.UTF_8);
    }

    private static Network createNetwork(String mcc, String mnc,
            String isoCountryCode) {
        return Network.newBuilder()
                .setMcc(mcc)
                .setMnc(mnc)
                .setCountryIsoCode(isoCountryCode)
                .build();
    }

    private static MobileCountry createMobileCountry(
            String mcc, List<String> countryIsoCodes) {
        return MobileCountry.newBuilder()
                .setMcc(mcc)
                .addAllCountryIsoCodes(countryIsoCodes)
                .build();
    }

    private static TelephonyLookup createTelephonyLookup(
            List<Network> networks, List<MobileCountry> mobileCountries) {
        return TelephonyLookup.newBuilder()
                .addAllNetworks(networks)
                .addAllMobileCountries(mobileCountries)
                .build();
    }

    private static void assertFileIsEmpty(String outputFile) throws IOException {
        Path outputFilePath = Paths.get(outputFile);
        assertEquals(0, Files.size(outputFilePath));
    }

    private static String trimAndLinearize(String input) {
        return input.lines().map(String::trim).collect(Collectors.joining());
    }
}
