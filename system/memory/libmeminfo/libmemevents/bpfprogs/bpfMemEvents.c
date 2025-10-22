/*
 * MM Events - eBPF programs
 *
 * Copyright 2024 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License
 * You may obtain a copy of the License at
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include <string.h>

#include <linux/bpf_perf_event.h>
#include <linux/oom.h>

#include <memevents/bpf_helpers.h>
#include <memevents/bpf_types.h>

DEFINE_BPF_RINGBUF(ams_rb, struct mem_event_t, MEM_EVENTS_RINGBUF_SIZE, DEFAULT_BPF_MAP_UID,
                   AID_SYSTEM, 0660)

DEFINE_BPF_RINGBUF(lmkd_rb, struct mem_event_t, MEM_EVENTS_RINGBUF_SIZE, DEFAULT_BPF_MAP_UID,
                   AID_SYSTEM, 0660)

DEFINE_BPF_PROG_KVER("tracepoint/oom/mark_victim/ams", AID_ROOT, AID_SYSTEM, tp_ams, KVER_5_10)
(struct mark_victim_args* args) {
    unsigned long long timestamp_ns = bpf_ktime_get_ns();
    struct mem_event_t* data = bpf_ams_rb_reserve();
    if (data == NULL) return 1;

    data->type = MEM_EVENT_OOM_KILL;
    data->event_data.oom_kill.pid = args->pid;
    data->event_data.oom_kill.oom_score_adj = args->oom_score_adj;
    data->event_data.oom_kill.uid = args->uid;
    data->event_data.oom_kill.timestamp_ms = timestamp_ns / 1000000;  // Convert to milliseconds
    data->event_data.oom_kill.total_vm_kb = args->total_vm;
    data->event_data.oom_kill.anon_rss_kb = args->anon_rss;
    data->event_data.oom_kill.file_rss_kb = args->file_rss;
    data->event_data.oom_kill.shmem_rss_kb = args->shmem_rss;
    data->event_data.oom_kill.pgtables_kb = args->pgtables;

    read_str((char*)args, args->__data_loc_comm, data->event_data.oom_kill.process_name,
             MEM_EVENT_PROC_NAME_LEN);

    bpf_ams_rb_submit(data);

    return 0;
}

DEFINE_BPF_PROG_KVER("tracepoint/vmscan/mm_vmscan_direct_reclaim_begin/lmkd", AID_ROOT, AID_SYSTEM,
                     tp_lmkd_dr_start, KVER_5_10)
(struct direct_reclaim_begin_args* __unused args) {
    struct mem_event_t* data = bpf_lmkd_rb_reserve();
    if (data == NULL) return 1;

    data->type = MEM_EVENT_DIRECT_RECLAIM_BEGIN;

    bpf_lmkd_rb_submit(data);

    return 0;
}

DEFINE_BPF_PROG_KVER("tracepoint/vmscan/mm_vmscan_direct_reclaim_end/lmkd", AID_ROOT, AID_SYSTEM,
                     tp_lmkd_dr_end, KVER_5_10)
(struct direct_reclaim_end_args* __unused args) {
    struct mem_event_t* data = bpf_lmkd_rb_reserve();
    if (data == NULL) return 1;

    data->type = MEM_EVENT_DIRECT_RECLAIM_END;

    bpf_lmkd_rb_submit(data);

    return 0;
}

DEFINE_BPF_PROG_KVER("tracepoint/vmscan/mm_vmscan_kswapd_wake/lmkd", AID_ROOT, AID_SYSTEM,
                     tp_lmkd_kswapd_wake, KVER_5_10)
(struct kswapd_wake_args* args) {
    struct mem_event_t* data = bpf_lmkd_rb_reserve();
    if (data == NULL) return 1;

    data->type = MEM_EVENT_KSWAPD_WAKE;
    data->event_data.kswapd_wake.node_id = args->nid;
    data->event_data.kswapd_wake.zone_id = args->zid;
    data->event_data.kswapd_wake.alloc_order = args->order;

    bpf_lmkd_rb_submit(data);

    return 0;
}

DEFINE_BPF_PROG_KVER("tracepoint/vmscan/mm_vmscan_kswapd_sleep/lmkd", AID_ROOT, AID_SYSTEM,
                     tp_lmkd_kswapd_sleep, KVER_5_10)
(struct kswapd_sleep_args* args) {
    struct mem_event_t* data = bpf_lmkd_rb_reserve();
    if (data == NULL) return 1;

    data->type = MEM_EVENT_KSWAPD_SLEEP;
    data->event_data.kswapd_wake.node_id = args->nid;

    bpf_lmkd_rb_submit(data);

    return 0;
}

DEFINE_BPF_PROG_KVER("tracepoint/android_vendor_lmk/android_trigger_vendor_lmk_kill/lmkd",
                     AID_ROOT, AID_SYSTEM, tp_lmkd_vendor_lmk_kill, KVER_6_1)
(struct vendor_lmk_kill_args* args) {
    struct mem_event_t* data;
    uint32_t reason = args->reason;
    short min_oom_score_adj = args->min_oom_score_adj;

    if (min_oom_score_adj < OOM_SCORE_ADJ_MIN ||
        min_oom_score_adj > OOM_SCORE_ADJ_MAX)
        return 0;

    if (reason < 0 || reason >= NUM_VENDOR_LMK_KILL_REASON)
        return 0;

    data = bpf_lmkd_rb_reserve();
    if (data == NULL) return 1;

    data->type = MEM_EVENT_VENDOR_LMK_KILL;
    data->event_data.vendor_kill.reason = reason;
    data->event_data.vendor_kill.min_oom_score_adj = min_oom_score_adj;

    bpf_lmkd_rb_submit(data);

    return 0;
}

DEFINE_BPF_PROG_KVER("tracepoint/kmem/mm_calculate_totalreserve_pages/lmkd", AID_ROOT, AID_SYSTEM,
                     tp_lmkd_calculate_totalreserve_pages, KVER_6_1)
(struct calculate_totalreserve_pages_args* __unused args) {
    struct mem_event_t* data = bpf_lmkd_rb_reserve();
    if (data == NULL) return 1;

    data->type = MEM_EVENT_UPDATE_ZONEINFO;

    bpf_lmkd_rb_submit(data);

    return 0;
}
// bpf_probe_read_str is GPL only symbol
LICENSE("GPL");
