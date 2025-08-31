/*
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.  Oracle designates this
 * particular file as subject to the "Classpath" exception as provided
 * by Oracle in the LICENSE file that accompanied this code.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 */

/*
 * This file is available under and governed by the GNU General Public
 * License version 2 only, as published by the Free Software Foundation.
 * However, the following notice accompanied the original version of this
 * file:
 *
 * Written by Doug Lea with assistance from members of JCP JSR-166
 * Expert Group and released to the public domain, as explained at
 * http://creativecommons.org/publicdomain/zero/1.0/
 */


package java.util.concurrent;

@SuppressWarnings({"unchecked", "deprecation", "all"})
public class ForkJoinWorkerThread extends java.lang.Thread {

@android.annotation.FlaggedApi(com.android.libcore.Flags.FLAG_OPENJDK_21_V1_APIS)
protected ForkJoinWorkerThread(java.lang.ThreadGroup group, java.util.concurrent.ForkJoinPool pool, boolean preserveThreadLocals) { throw new RuntimeException("Stub!"); }

protected ForkJoinWorkerThread(java.util.concurrent.ForkJoinPool pool) { throw new RuntimeException("Stub!"); }

public java.util.concurrent.ForkJoinPool getPool() { throw new RuntimeException("Stub!"); }

public int getPoolIndex() { throw new RuntimeException("Stub!"); }

@android.annotation.FlaggedApi(com.android.libcore.Flags.FLAG_OPENJDK_21_V1_APIS)
public int getQueuedTaskCount() { throw new RuntimeException("Stub!"); }

protected void onStart() { throw new RuntimeException("Stub!"); }

protected void onTermination(java.lang.Throwable exception) { throw new RuntimeException("Stub!"); }

public void run() { throw new RuntimeException("Stub!"); }
}

