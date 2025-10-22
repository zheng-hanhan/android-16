#
# Copyright (C) 2024 The Android Open Source Project
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#

import argparse
import os
import signal
import subprocess
import sys
import time

def path_exists(path: str):
  if path is None:
    return False
  return os.path.exists(os.path.expanduser(path))

def dir_exists(path: str):
  if path is None:
    return False
  return os.path.isdir(os.path.expanduser(path))

def convert_simpleperf_to_gecko(scripts_path, host_raw_trace_filename,
    host_gecko_trace_filename, symbols):
  expanded_symbols = os.path.expanduser(symbols)
  expanded_scripts_path = os.path.expanduser(scripts_path)
  print("Building binary cache, please wait. If no samples were recorded,"
        " the trace will be empty.")
  subprocess.run(("%s/binary_cache_builder.py -i %s -lib %s"
                  % (expanded_scripts_path, host_raw_trace_filename,
                     expanded_symbols)),
                 shell=True)
  subprocess.run(("%s/gecko_profile_generator.py -i %s > %s"
                  % (expanded_scripts_path, host_raw_trace_filename,
                     host_gecko_trace_filename)),
                 shell=True)
  if not path_exists(host_gecko_trace_filename):
    raise Exception("Gecko file was not created.")

def wait_for_process_or_ctrl_c(process):
  def signal_handler(sig, frame):
    print("Exiting...")
    process.kill()
    sys.exit()

  signal.signal(signal.SIGINT, signal_handler)
  signal.signal(signal.SIGTERM, signal_handler)

  process.wait()
  print("Process was killed.")

def wait_for_output(pattern, process, timeout):
  start_time = time.time()
  while time.time() - start_time < timeout:
    line = process.stdout.readline()
    if pattern in line.decode():
      process.stderr = None
      return False
  return True  # Timed out

def set_default_subparser(self, name):
  """
  A hack to add a default subparser to an argparse.ArgumentParser
  class. This will add the default subparser after all the global
  options in sys.argv.

  NOTE: Only works properly if all the global options have the
        'nargs' argument set to an integer.
  """
  subparser_found = False
  insertion_idx = 1

  # Get all global options
  global_opts = {}
  for action in self._actions:
      for opt in action.option_strings:
          global_opts[opt] = action.nargs

  for idx, arg in enumerate(sys.argv[1:]):
    if arg in ['-h', '--help']:
      break
    if arg in global_opts:
      insertion_idx = idx + global_opts[arg] + 2
  else:
    for action in self._subparsers._actions:
      if not isinstance(action, argparse._SubParsersAction):
        continue
      for sp_name in action._name_parser_map.keys():
        if sp_name in sys.argv[1:]:
          subparser_found = True
    if not subparser_found:
      # insert default subparser
      sys.argv.insert(insertion_idx, name)
