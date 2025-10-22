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

#include "apexd_session.h"

#include <android-base/file.h>
#include <android-base/result-gmock.h>
#include <android-base/result.h>
#include <android-base/scopeguard.h>
#include <android-base/stringprintf.h>
#include <android-base/strings.h>
#include <errno.h>
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>

#include "apexd_test_utils.h"
#include "apexd_utils.h"
#include "session_state.pb.h"

namespace android {
namespace apex {
namespace {

using android::base::Join;
using android::base::make_scope_guard;
using android::base::testing::Ok;
using ::apex::proto::SessionState;
using ::testing::Not;
using ::testing::UnorderedElementsAre;

// TODO(b/170329726): add unit tests for apexd_sessions.h

TEST(ApexSessionManagerTest, CreateSession) {
  TemporaryDir td;
  auto manager = ApexSessionManager::Create(std::string(td.path));

  auto session = manager->CreateSession(239);
  ASSERT_RESULT_OK(session);
  ASSERT_EQ(239, session->GetId());
  std::string session_dir = std::string(td.path) + "/239";
  ASSERT_EQ(session_dir, session->GetSessionDir());
}

TEST(ApexSessionManagerTest, GetSessionsNoSessionReturnsError) {
  TemporaryDir td;
  auto manager = ApexSessionManager::Create(std::string(td.path));

  ASSERT_THAT(manager->GetSession(37), Not(Ok()));
}

TEST(ApexSessionManagerTest, GetSessionsReturnsErrorSessionNotCommitted) {
  TemporaryDir td;
  auto manager = ApexSessionManager::Create(std::string(td.path));

  auto session = manager->CreateSession(73);
  ASSERT_RESULT_OK(session);
  ASSERT_THAT(manager->GetSession(73), Not(Ok()));
}

TEST(ApexSessionManagerTest, CreateCommitGetSession) {
  TemporaryDir td;
  auto manager = ApexSessionManager::Create(std::string(td.path));

  auto session = manager->CreateSession(23);
  ASSERT_RESULT_OK(session);
  session->SetErrorMessage("error");
  ASSERT_RESULT_OK(session->UpdateStateAndCommit(SessionState::STAGED));

  auto same_session = manager->GetSession(23);
  ASSERT_RESULT_OK(same_session);
  ASSERT_EQ(23, same_session->GetId());
  ASSERT_EQ("error", same_session->GetErrorMessage());
  ASSERT_EQ(SessionState::STAGED, same_session->GetState());
}

TEST(ApexSessionManagerTest, GetSessionsNoSessionsCommitted) {
  TemporaryDir td;
  auto manager = ApexSessionManager::Create(std::string(td.path));

  ASSERT_RESULT_OK(manager->CreateSession(3));

  auto sessions = manager->GetSessions();
  ASSERT_EQ(0u, sessions.size());
}

TEST(ApexSessionManager, GetSessionsCommittedSessions) {
  TemporaryDir td;
  auto manager = ApexSessionManager::Create(std::string(td.path));

  auto session1 = manager->CreateSession(1543);
  ASSERT_RESULT_OK(session1);
  ASSERT_RESULT_OK(session1->UpdateStateAndCommit(SessionState::ACTIVATED));

  auto session2 = manager->CreateSession(179);
  ASSERT_RESULT_OK(session2);
  ASSERT_RESULT_OK(session2->UpdateStateAndCommit(SessionState::SUCCESS));

  // This sessions is not committed, it won't be returned in GetSessions.
  ASSERT_RESULT_OK(manager->CreateSession(101));

  auto sessions = manager->GetSessions();
  std::sort(
      sessions.begin(), sessions.end(),
      [](const auto& s1, const auto& s2) { return s1.GetId() < s2.GetId(); });

  ASSERT_EQ(2u, sessions.size());

  ASSERT_EQ(179, sessions[0].GetId());
  ASSERT_EQ(SessionState::SUCCESS, sessions[0].GetState());

  ASSERT_EQ(1543, sessions[1].GetId());
  ASSERT_EQ(SessionState::ACTIVATED, sessions[1].GetState());
}

TEST(ApexSessionManager, GetSessionsInState) {
  TemporaryDir td;
  auto manager = ApexSessionManager::Create(std::string(td.path));

  auto session1 = manager->CreateSession(43);
  ASSERT_RESULT_OK(session1);
  ASSERT_RESULT_OK(session1->UpdateStateAndCommit(SessionState::ACTIVATED));

  auto session2 = manager->CreateSession(41);
  ASSERT_RESULT_OK(session2);
  ASSERT_RESULT_OK(session2->UpdateStateAndCommit(SessionState::SUCCESS));

  auto session3 = manager->CreateSession(23);
  ASSERT_RESULT_OK(session3);
  ASSERT_RESULT_OK(session3->UpdateStateAndCommit(SessionState::SUCCESS));

  auto sessions = manager->GetSessionsInState(SessionState::SUCCESS);
  std::sort(
      sessions.begin(), sessions.end(),
      [](const auto& s1, const auto& s2) { return s1.GetId() < s2.GetId(); });

  ASSERT_EQ(2u, sessions.size());

  ASSERT_EQ(23, sessions[0].GetId());
  ASSERT_EQ(SessionState::SUCCESS, sessions[0].GetState());

  ASSERT_EQ(41, sessions[1].GetId());
  ASSERT_EQ(SessionState::SUCCESS, sessions[1].GetState());
}

TEST(ApexSessionManagerTest, GetStagedApexDirsSelf) {
  TemporaryDir td;
  auto manager = ApexSessionManager::Create(std::string(td.path));

  auto session = manager->CreateSession(239);
  ASSERT_RESULT_OK(session);

  ASSERT_THAT(session->GetStagedApexDirs("/path/to/staged_session_dir"),
              UnorderedElementsAre("/path/to/staged_session_dir/session_239"));
}

TEST(ApexSessionManagerTest, GetStagedApexDirsChildren) {
  TemporaryDir td;
  auto manager = ApexSessionManager::Create(std::string(td.path));

  auto session = manager->CreateSession(239);
  ASSERT_RESULT_OK(session);
  auto child_session_1 = manager->CreateSession(240);
  ASSERT_RESULT_OK(child_session_1);
  auto child_session_2 = manager->CreateSession(241);
  ASSERT_RESULT_OK(child_session_2);
  session->SetChildSessionIds({240, 241});

  ASSERT_THAT(session->GetStagedApexDirs("/path/to/staged_session_dir"),
              UnorderedElementsAre("/path/to/staged_session_dir/session_240",
                                   "/path/to/staged_session_dir/session_241"));
}

}  // namespace
}  // namespace apex
}  // namespace android
