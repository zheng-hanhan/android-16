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

#include "chre/platform/shared/nanoapp/tokenized_log.h"
#include "chre/core/event_loop_manager.h"
#include "chre/platform/log.h"
#include "chre/platform/shared/log_buffer_manager.h"
#include "chre_api/chre/re.h"

#include "pw_log_tokenized/config.h"
#include "pw_tokenizer/encode_args.h"
#include "pw_tokenizer/tokenize.h"

void platform_chrePwTokenizedLog(enum chreLogLevel level, uint32_t token,
                                 uint64_t types, ...) {
  va_list args;
  va_start(args, types);
  pw::tokenizer::EncodedMessage<pw::log_tokenized::kEncodingBufferSizeBytes>
      encodedMessage(token, types, args);
  va_end(args);

  chre::LogBufferManagerSingleton::get()->logNanoappTokenized(
      level,
      chre::EventLoopManagerSingleton::get()
          ->getEventLoop()
          .getCurrentNanoapp()
          ->getInstanceId(),
      encodedMessage.data_as_uint8(), encodedMessage.size());
}
