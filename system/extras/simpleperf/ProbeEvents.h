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

#pragma once

#include <optional>
#include <string>
#include <vector>

namespace simpleperf {

class EventSelectionSet;

enum class ProbeEventType {
  kKprobe,
  kUprobe,
};

struct ProbeEvent {
  ProbeEventType type;
  std::string group_name;
  std::string event_name;
};

// Add kprobe events in /sys/kernel/debug/tracing/kprobe_events, and
// delete them in ProbeEvents::clear().
class ProbeEvents {
 public:
  ProbeEvents(EventSelectionSet& event_selection_set) : event_selection_set_(event_selection_set) {}
  ~ProbeEvents();

  static bool ParseProbeEventName(ProbeEventType type, const std::string& kprobe_cmd,
                                  ProbeEvent* event);
  bool IsProbeSupported(ProbeEventType type);

  // Accept kprobe cmd as in <linux_kernel>/Documentation/trace/kprobetrace.rst
  // or uprobe cmd as in <linux_kernel>/Documentation/trace/uprobetracer.rst.
  bool AddProbe(ProbeEventType type, const std::string& probe_cmd);
  // If not exist, add a kprobe tracepoint at the function entry.
  bool CreateProbeEventIfNotExist(const std::string& event_name);

 private:
  bool IsKprobeEvent(const std::string& event_name);
  bool IsEmpty() const { return probe_events_.empty(); }
  void Clear();
  bool WriteProbeCmd(ProbeEventType type, const std::string& probe_cmd);
  std::optional<std::string>& GetProbeControlPath(ProbeEventType type);

  EventSelectionSet& event_selection_set_;
  std::vector<ProbeEvent> probe_events_;
  std::optional<std::string> kprobe_control_path_;
  std::optional<std::string> uprobe_control_path_;
};

}  // namespace simpleperf
