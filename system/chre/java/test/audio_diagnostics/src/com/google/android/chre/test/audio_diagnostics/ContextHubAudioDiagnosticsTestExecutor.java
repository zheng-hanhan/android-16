/*
 * Copyright (C) 2024 The Android Open Source Project
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

package com.google.android.chre.test.audiodiagnostics;

import android.hardware.location.NanoAppBinary;
import android.media.AudioFormat;
import android.media.AudioRecord;
import android.media.MediaRecorder;
import android.util.Log;

import com.google.android.chre.test.chqts.ContextHubChreApiTestExecutor;
import com.google.android.utils.chre.ChreApiTestUtil;

import org.junit.Assert;

import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.ShortBuffer;
import java.util.List;
import java.util.concurrent.Future;
import java.util.concurrent.TimeUnit;

import dev.chre.rpc.proto.ChreApiTest;

/**
 * A class that can execute the CHRE audio diagnostics test.
 */
public class ContextHubAudioDiagnosticsTestExecutor extends ContextHubChreApiTestExecutor {
    private static final String TAG = "ContextHubAudioDiagnosticsTestExecutor";
    private static final int CHRE_EVENT_AUDIO_DATA = 0x0330 + 1;
    // Amount of time in a single audio data packet
    private static final long AUDIO_DATA_TIMEOUT_NS = 2000000000L; // 2s
    private static final int GATHER_SINGLE_AUDIO_EVENT = 1;
    private static final int CHRE_MIC_HANDLE = 0;
    private static final int DC_OFFSET_LIMIT = 15;
    private static final int RMSE_ERROR_DB = 3;
    private static final double RMSE_TARGET_DB = 22;
    private static final double PEAK_AMP_TARGET_DBFS = 19.5;
    private static final double MAX_SIGNED_SHORT = 32767;

    public ContextHubAudioDiagnosticsTestExecutor(NanoAppBinary nanoapp) {
        super(nanoapp);
    }

    /**
     * Runs the audio enable/disable test.
     */
    public void runChreAudioEnableDisableTest() throws Exception {
        enableChreAudio();

        ChreApiTest.ChreAudioDataEvent audioEvent =
                new ChreApiTestUtil().gatherAudioDataEvent(
                    getRpcClient(),
                    CHRE_EVENT_AUDIO_DATA,
                    GATHER_SINGLE_AUDIO_EVENT,
                    AUDIO_DATA_TIMEOUT_NS);
        disableChreAudio();

        Assert.assertNotNull(audioEvent);
        ChreApiTestUtil.writeDataToFile(audioEvent.getSamples().toByteArray(),
                "audio_enable_disable_test_data.bin", mContext);
        Assert.assertTrue(audioEvent.getStatus());
    }

    /**
     * Runs the audio RMSE test.
     */
    public void runAudioDiagnosticsRmseTest() throws Exception {
        enableChreAudio();

        ChreApiTest.ChreAudioDataEvent audioEvent =
                new ChreApiTestUtil().gatherAudioDataEvent(
                    getRpcClient(),
                    CHRE_EVENT_AUDIO_DATA,
                    GATHER_SINGLE_AUDIO_EVENT,
                    AUDIO_DATA_TIMEOUT_NS);
        Assert.assertNotNull(audioEvent);
        Assert.assertEquals(audioEvent.getStatus(), true);

        disableChreAudio();

        // Test logic - calculate RMSE
        ByteBuffer chreBuffer = ByteBuffer.allocate(audioEvent.getSamples().size());
        chreBuffer.order(ByteOrder.LITTLE_ENDIAN);
        audioEvent.getSamples().copyTo(chreBuffer);
        chreBuffer.rewind();
        ShortBuffer chreShortBuffer = chreBuffer.asShortBuffer();
        List<Double> chreRMSE = calculateRMSE(chreShortBuffer);
        double chreRmseDb = Math.abs(chreRMSE.get(1));
        double chrePeakAmplitudeDb = Math.abs(chreRMSE.get(2));

        ChreApiTestUtil.writeDataToFile(chreBuffer.array(), "audio_rmse_test_data.bin",
                mContext);

        Log.i(TAG, "RMSE: " + chreRmseDb + " dB");
        Log.i(TAG, "Peak Amplitude: " + chrePeakAmplitudeDb + " dB");
        Assert.assertEquals(RMSE_TARGET_DB, chreRmseDb, RMSE_ERROR_DB);
        Assert.assertEquals(PEAK_AMP_TARGET_DBFS, chrePeakAmplitudeDb,
                            RMSE_ERROR_DB);
    }

    /**
     * Runs the audio DC offset test.
     */
    public void runAudioDiagnosticsDcOffsetTest() throws Exception {
        long runningSampleAvg = 0;
        long samplesRead = 0;
        enableChreAudio();

        ChreApiTest.ChreAudioDataEvent audioEvent =
                new ChreApiTestUtil().gatherAudioDataEvent(
                    getRpcClient(),
                    CHRE_EVENT_AUDIO_DATA,
                    GATHER_SINGLE_AUDIO_EVENT,
                    AUDIO_DATA_TIMEOUT_NS);
        Assert.assertNotNull(audioEvent);
        Assert.assertEquals(audioEvent.getStatus(), true);

        disableChreAudio();

        // Test logic - Get the average of all samples
        ByteBuffer chreBuffer = ByteBuffer.allocate(audioEvent.getSamples().size());
        chreBuffer.order(ByteOrder.LITTLE_ENDIAN);
        audioEvent.getSamples().copyTo(chreBuffer);
        chreBuffer.rewind();
        ShortBuffer chreShortBuffer = chreBuffer.asShortBuffer();
        while (chreShortBuffer.hasRemaining()) {
            short sample = chreShortBuffer.get();
            runningSampleAvg += sample;
            samplesRead += 1;
        }
        runningSampleAvg /= samplesRead;

        ChreApiTestUtil.writeDataToFile(chreBuffer.array(),
                                        "audio_dc_offset_test_data.bin", mContext);

        Log.i(TAG, "DC Offset: " + runningSampleAvg);
        Assert.assertTrue("DC offset " + runningSampleAvg + " > threshold " + DC_OFFSET_LIMIT,
                runningSampleAvg <= DC_OFFSET_LIMIT);
    }

    /**
     * Runs the audio concurrency test. Makes sure AP and CHRE concurrent audio
     * have the same RMSE.
     */
    public void runAudioDiagnosticsConcurrencyTest() throws Exception {
        // Set up recording from AP.
        // Hold the mic for 2 seconds
        int samplingRateHz = 16000; // 16 KHz
        int durationSeconds = 2;
        int minBufferSize = AudioRecord.getMinBufferSize(
                samplingRateHz, AudioFormat.CHANNEL_IN_MONO, AudioFormat.ENCODING_PCM_16BIT);
        int size = Math.max(minBufferSize, samplingRateHz * durationSeconds * 2);
        AudioRecord record = new AudioRecord(
                MediaRecorder.AudioSource.MIC, samplingRateHz,
                AudioFormat.CHANNEL_IN_MONO, AudioFormat.ENCODING_PCM_16BIT, size);
        Assert.assertTrue("AudioRecord could not be initialized.",
                record.getState() == AudioRecord.STATE_INITIALIZED);

        // Set up recording from CHRE.
        enableChreAudio();

        // Start async process of collecting the CHRE audio event
        // Allow double the time to time out in case AP audio recording cuts off
        // CHRE event delivery
        Future<List<ChreApiTest.GeneralEventsMessage>> audioEventsFuture =
                new ChreApiTestUtil().gatherEvents(getRpcClient(),
                                                   CHRE_EVENT_AUDIO_DATA,
                                                   GATHER_SINGLE_AUDIO_EVENT,
                                                   AUDIO_DATA_TIMEOUT_NS * 2);
        // Start synchronous AP recording
        record.startRecording();
        // Collect the audio events from the async call
        List<ChreApiTest.GeneralEventsMessage> audioEvents =
                audioEventsFuture.get(2 * AUDIO_DATA_TIMEOUT_NS,
                                      TimeUnit.NANOSECONDS);

        // Process the collected events into one CHRE audio data event.
        ChreApiTest.ChreAudioDataEvent audioEvent =
                new ChreApiTestUtil().processAudioDataEvents(audioEvents);
        Assert.assertNotNull(audioEvent);
        Assert.assertEquals(audioEvent.getStatus(), true);

        // disable & release both CHRE and AP recording
        disableChreAudio();

        byte[] apBuf = new byte[size];
        record.read(apBuf, /* offsetInBytes= */ 0, size);
        record.release();

        // calculate CHRE's audio RMSE
        ByteBuffer chreBuffer = ByteBuffer.allocate(audioEvent.getSamples().size());
        chreBuffer.order(ByteOrder.LITTLE_ENDIAN);
        audioEvent.getSamples().copyTo(chreBuffer);
        chreBuffer.rewind();
        ShortBuffer chreShortBuffer = chreBuffer.asShortBuffer();
        List<Double> chreRMSE = calculateRMSE(chreShortBuffer);

        // calculate AP's audio RMSE
        ByteBuffer apBuffer = ByteBuffer.allocate(size);
        apBuffer.order(ByteOrder.LITTLE_ENDIAN);
        apBuffer.put(apBuf);
        apBuffer.rewind();
        ShortBuffer apShortBuffer = apBuffer.asShortBuffer();
        List<Double> apRMSE = calculateRMSE(apShortBuffer);

        ChreApiTestUtil.writeDataToFile(chreBuffer.array(),
                                        "audio_concurrency_chre_test_data.bin",
                                        mContext);
        ChreApiTestUtil.writeDataToFile(apBuf,
                                        "audio_concurrency_ap_test_data.bin",
                                        mContext);

        Log.i(TAG, "CHRE RMSE: " + chreRMSE.get(1) + " dB");
        Log.i(TAG, "AP RMSE: " + apRMSE.get(1) + " dB");

        Assert.assertEquals(RMSE_TARGET_DB,
                            Math.abs(chreRMSE.get(1)),
                            RMSE_ERROR_DB);
    }

    /**
     * Calculates RMSE of a given buffer of 16-bit PCM samples.
     *
     * @param audioSamples  Buffer of 16-bit PCM audio samples.
     * @return              A list of RMSE values. The first entry will be the
     *                      linear value, the second will be the decibel, and
     *                      the third will be the peak amplitude in decibel.
     */
    private List<Double> calculateRMSE(ShortBuffer audioSamples) throws Exception {
        double runningSquareSum = 0;
        double samplesRead = 0;
        double peakSample = 0;

        // rewind the buffer to make sure we start at the beginning
        audioSamples.rewind();
        while (audioSamples.hasRemaining()) {
            short sample = audioSamples.get();
            peakSample = Math.abs(sample) > peakSample ? Math.abs(sample) : peakSample;
            double scaledSample = sample / MAX_SIGNED_SHORT;
            runningSquareSum += scaledSample * scaledSample;
            samplesRead += 1;
        }
        double rmseLinear = Math.sqrt(runningSquareSum / samplesRead);
        double rmseDecibel = 20 * Math.log10(Math.abs(rmseLinear));
        double peakSampleDb = 20 * Math.log10(peakSample / MAX_SIGNED_SHORT);

        return List.of(rmseLinear, rmseDecibel, peakSampleDb);
    }

    /**
     * Enables CHRE audio for the first audio source found.
     * Asserts that the RPC call comes back successfully.
     */
    private void enableChreAudio() throws Exception {
        ChreApiTest.ChreHandleInput inputHandle =
                ChreApiTest.ChreHandleInput.newBuilder()
                .setHandle(CHRE_MIC_HANDLE).build();
        ChreApiTest.ChreAudioGetSourceOutput audioSrcResponse =
                ChreApiTestUtil.callUnaryRpcMethodSync(getRpcClient(),
                        "chre.rpc.ChreApiTestService.ChreAudioGetSource", inputHandle);
        Assert.assertNotNull(audioSrcResponse);
        Assert.assertTrue(audioSrcResponse.getStatus());

        ChreApiTest.ChreAudioConfigureSourceInput inputSrc =
                ChreApiTest.ChreAudioConfigureSourceInput.newBuilder()
                        .setHandle(CHRE_MIC_HANDLE)
                        .setEnable(true)
                        .setBufferDuration(audioSrcResponse.getMinBufferDuration())
                        .setDeliveryInterval(audioSrcResponse.getMinBufferDuration())
                        .build();
        ChreApiTest.Status audioEnableresponse =
                ChreApiTestUtil.callUnaryRpcMethodSync(getRpcClient(),
                "chre.rpc.ChreApiTestService.ChreAudioConfigureSource", inputSrc);

        Assert.assertNotNull(audioEnableresponse);
        Assert.assertTrue(audioEnableresponse.getStatus());
    }

    /**
     * Disables CHRE audio for the first audio source found.
     * Asserts that the RPC call comes back successfully.
     */
    private void disableChreAudio() throws Exception {
        ChreApiTest.ChreHandleInput inputHandle =
                ChreApiTest.ChreHandleInput.newBuilder()
                .setHandle(CHRE_MIC_HANDLE).build();
        ChreApiTest.ChreAudioGetSourceOutput audioSrcResponse =
                ChreApiTestUtil.callUnaryRpcMethodSync(getRpcClient(),
                        "chre.rpc.ChreApiTestService.ChreAudioGetSource", inputHandle);
        Assert.assertNotNull(audioSrcResponse);
        Assert.assertTrue(audioSrcResponse.getStatus());

        ChreApiTest.ChreAudioConfigureSourceInput inputSrc =
                ChreApiTest.ChreAudioConfigureSourceInput.newBuilder()
                        .setHandle(CHRE_MIC_HANDLE)
                        .setEnable(false)
                        .setBufferDuration(0)
                        .setDeliveryInterval(0)
                        .build();
        ChreApiTest.Status audioDisableresponse =
                ChreApiTestUtil.callUnaryRpcMethodSync(getRpcClient(),
                "chre.rpc.ChreApiTestService.ChreAudioConfigureSource", inputSrc);

        Assert.assertNotNull(audioDisableresponse);
        Assert.assertTrue(audioDisableresponse.getStatus());
    }
}
