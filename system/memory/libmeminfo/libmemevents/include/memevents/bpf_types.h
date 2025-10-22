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

#ifndef MEM_EVENTS_BPF_TYES_H_
#define MEM_EVENTS_BPF_TYES_H_

#include <inttypes.h>

#define MEM_EVENT_PROC_NAME_LEN 16  // linux/sched.h
#define MEM_EVENTS_RINGBUF_SIZE 4096

typedef unsigned int mem_event_type_t;
/* Supported mem_event_type_t */
#define MEM_EVENT_OOM_KILL 0
#define MEM_EVENT_BASE MEM_EVENT_OOM_KILL
#define MEM_EVENT_DIRECT_RECLAIM_BEGIN 1
#define MEM_EVENT_DIRECT_RECLAIM_END 2
#define MEM_EVENT_KSWAPD_WAKE 3
#define MEM_EVENT_KSWAPD_SLEEP 4
#define MEM_EVENT_VENDOR_LMK_KILL 5
#define MEM_EVENT_UPDATE_ZONEINFO 6

// This always comes after the last valid event type
#define NR_MEM_EVENTS 7

/* BPF-Rb Paths */
#define MEM_EVENTS_AMS_RB "/sys/fs/bpf/memevents/map_bpfMemEvents_ams_rb"
#define MEM_EVENTS_LMKD_RB "/sys/fs/bpf/memevents/map_bpfMemEvents_lmkd_rb"
#define MEM_EVENTS_TEST_RB "/sys/fs/bpf/memevents/map_bpfMemEventsTest_rb"

/* BPF-Prog Paths */
#define MEM_EVENTS_AMS_OOM_MARK_VICTIM_TP \
    "/sys/fs/bpf/memevents/prog_bpfMemEvents_tracepoint_oom_mark_victim_ams"
#define MEM_EVENTS_LMKD_VMSCAN_DR_BEGIN_TP \
    "/sys/fs/bpf/memevents/"               \
    "prog_bpfMemEvents_tracepoint_vmscan_mm_vmscan_direct_reclaim_begin_lmkd"
#define MEM_EVENTS_LMKD_VMSCAN_DR_END_TP \
    "/sys/fs/bpf/memevents/prog_bpfMemEvents_tracepoint_vmscan_mm_vmscan_direct_reclaim_end_lmkd"
#define MEM_EVENTS_LMKD_VMSCAN_KSWAPD_WAKE_TP \
    "/sys/fs/bpf/memevents/prog_bpfMemEvents_tracepoint_vmscan_mm_vmscan_kswapd_wake_lmkd"
#define MEM_EVENTS_LMKD_VMSCAN_KSWAPD_SLEEP_TP \
    "/sys/fs/bpf/memevents/prog_bpfMemEvents_tracepoint_vmscan_mm_vmscan_kswapd_sleep_lmkd"
#define MEM_EVENTS_LMKD_TRIGGER_VENDOR_LMK_KILL_TP \
    "/sys/fs/bpf/memevents/"                       \
    "prog_bpfMemEvents_tracepoint_android_vendor_lmk_android_trigger_vendor_lmk_kill_lmkd"
#define MEM_EVENTS_LMKD_CALCULATE_TOTALRESERVE_PAGES_TP \
    "/sys/fs/bpf/memevents/prog_bpfMemEvents_tracepoint_kmem_mm_calculate_totalreserve_pages_lmkd"
#define MEM_EVENTS_TEST_OOM_MARK_VICTIM_TP \
    "/sys/fs/bpf/memevents/prog_bpfMemEventsTest_tracepoint_oom_mark_victim"

/* Number of kill reasons.  Currently,
*  kill reasons are values from 0 to 999.
*  This range is expected to cover all
*  foreseeable kill reasons.  If the number of
*  kill reasons exceeds this limit in the future,
*  this constant should be adjusted accordingly.
*/
#define NUM_VENDOR_LMK_KILL_REASON 1000

/* Struct to collect data from tracepoints */
struct mem_event_t {
    uint64_t type;

    union EventData {
        struct OomKill {
            uint32_t pid;
            uint64_t timestamp_ms;
            short oom_score_adj;
            uint32_t uid;
            char process_name[MEM_EVENT_PROC_NAME_LEN];
            uint64_t total_vm_kb;
            uint64_t anon_rss_kb;
            uint64_t file_rss_kb;
            uint64_t shmem_rss_kb;
            uint64_t pgtables_kb;
        } oom_kill;

        struct KswapdWake {
            uint32_t node_id;
            uint32_t zone_id;
            uint32_t alloc_order;
        } kswapd_wake;

        struct KswapdSleep {
            uint32_t node_id;
        } kswapd_sleep;

        struct VendorKill {
            uint32_t reason;
            short min_oom_score_adj;
        } vendor_kill;

        struct TotalReservePages {
            uint32_t num_pages;
        } reserve_pages;
    } event_data;
};

/* Expected args for tracepoints */

struct mark_victim_args {
    uint64_t __ignore;
    /* Actual fields start at offset 8 */
    uint32_t pid;
    uint32_t __data_loc_comm;
    uint64_t total_vm;
    uint64_t anon_rss;
    uint64_t file_rss;
    uint64_t shmem_rss;
    uint32_t uid;
    uint64_t pgtables;
    short oom_score_adj;
};

struct direct_reclaim_begin_args {
    char __ignore[24];
};

struct direct_reclaim_end_args {
    char __ignore[16];
};

struct kswapd_wake_args {
    uint64_t __ignore;
    /* Actual fields start at offset 8 */
    uint32_t nid;
    uint32_t zid;
    uint32_t order;
};

struct kswapd_sleep_args {
    uint64_t __ignore;
    /* Actual fields start at offset 8 */
    uint32_t nid;
};

struct vendor_lmk_kill_args {
    uint64_t __ignore;
    /* Actual fields start at offset 8 */
    uint32_t reason;
    short min_oom_score_adj;
};

struct calculate_totalreserve_pages_args {
    uint64_t __ignore;
    /* Actual fields start at offset 8 */
    uint64_t totalreserve_pages;
};
#endif /* MEM_EVENTS_BPF_TYES_H_ */
