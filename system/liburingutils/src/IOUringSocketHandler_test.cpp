/*
 * Copyright (C) 2025 The Android Open Source Project
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

#include <IOUringSocketHandler/IOUringSocketHandler.h>

#include <gtest/gtest.h>

class IOUringSocketHandlerTest : public testing::Test {
public:
    bool IsIouringEnabled() {
        return IOUringSocketHandler::isIouringEnabled();
    }

protected:
    std::unique_ptr<IOUringSocketHandler> handler_;
    void InitializeHandler(int socket_fd = 1);
    int queue_depth_ = 10;
};

void IOUringSocketHandlerTest::InitializeHandler(int socket_fd) {
    handler_ = std::make_unique<IOUringSocketHandler>(socket_fd);
}

TEST_F(IOUringSocketHandlerTest, SetupIoUring) {
    if (!IsIouringEnabled()) {
        return;
    }
    InitializeHandler();
    EXPECT_TRUE(handler_->SetupIoUring(queue_depth_));
}

TEST_F(IOUringSocketHandlerTest, AllocateAndRegisterBuffers) {
    if (!IsIouringEnabled()) {
        return;
    }
    InitializeHandler();
    EXPECT_TRUE(handler_->SetupIoUring(queue_depth_));
    EXPECT_TRUE(handler_->AllocateAndRegisterBuffers(8, 4096));
}

TEST_F(IOUringSocketHandlerTest, MultipleAllocateAndRegisterBuffers) {
    if (!IsIouringEnabled()) {
        return;
    }
    InitializeHandler();

    EXPECT_TRUE(handler_->SetupIoUring(queue_depth_));

    EXPECT_TRUE(handler_->AllocateAndRegisterBuffers(4, 4096));
    handler_->DeRegisterBuffers();

    EXPECT_TRUE(handler_->AllocateAndRegisterBuffers(2, 1024*1024L));
    handler_->DeRegisterBuffers();

    EXPECT_TRUE(handler_->AllocateAndRegisterBuffers(32, 1024));
    handler_->DeRegisterBuffers();

    // num_buffers should be power of 2
    EXPECT_FALSE(handler_->AllocateAndRegisterBuffers(5, 4096));
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
