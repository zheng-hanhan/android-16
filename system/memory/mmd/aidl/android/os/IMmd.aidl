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

package android.os;

/**
 * Binder interface of mmd.
 *
 * IMmd is oneway asynchronous API. mmd uses any information passed from outside (e.g.
 * system_server) as hints. Hint producers don't need to wait until mmd consumes the hists.
 */
interface IMmd {
    /**
     * mmd starts zram maintenance operation (e.g. zram writeback, zram recompression) if
     * applicable.
     *
     * mmd expects this Binder is called on a good timing to execute the maintenance (e.g. while the
     * system is idle).
     *
     * This is oneway asynchronous API. mmd uses any information passed from outside (e.g.
     * system_server) as hints. Hint producers don't need to wait until mmd consumes the hists.
     */
    oneway void doZramMaintenanceAsync();

    /**
     * Whether mmd supports doZramMaintenance() call on the device.
     *
     * System, which don't utilize zram, should not call doZramMaintenance() because it is no-op and
     * useless.
     */
    boolean isZramMaintenanceSupported();
}
