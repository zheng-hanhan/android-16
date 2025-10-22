/*
 * Copyright (C) 2016 The Android Open Source Project
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

#define LOG_TAG "hwservicemanager"

#include <utils/Log.h>

#include <inttypes.h>
#include <unistd.h>
#include <sys/timerfd.h>

#include <android/hidl/token/1.0/ITokenManager.h>
#include <cutils/properties.h>
#include <hidl/HidlBinderSupport.h>
#include <hidl/HidlTransportSupport.h>
#include <hidl/ServiceManagement.h>
#include <hidl/Status.h>
#include <hwbinder/IPCThreadState.h>
#include <hwbinder/ProcessState.h>
#include <utils/Errors.h>
#include <utils/Looper.h>
#include <utils/StrongPointer.h>

#include "ServiceManager.h"
#include "TokenManager.h"
#include "Vintf.h"

// libutils:
using android::sp;
using android::Looper;
using android::LooperCallback;

// libhwbinder:
using android::hardware::BHwBinder;
using android::hardware::IBinder;
using android::hardware::IPCThreadState;
using android::hardware::ProcessState;

// libhidl
using android::hardware::handleTransportPoll;
using android::hardware::setRequestingSid;
using android::hardware::HidlReturnRestriction;
using android::hardware::setProcessHidlReturnRestriction;
using android::hardware::setupTransportPolling;
using android::hardware::toBinder;

// implementations
using android::hidl::manager::implementation::ServiceManager;
using android::hidl::manager::V1_0::IServiceManager;
using android::hidl::token::V1_0::implementation::TokenManager;

static std::string serviceName = "default";

class HwBinderCallback : public LooperCallback {
public:
    static sp<HwBinderCallback> setupTo(const sp<Looper>& looper) {
        sp<HwBinderCallback> cb = new HwBinderCallback;

        int fdHwBinder = setupTransportPolling();
        LOG_ALWAYS_FATAL_IF(fdHwBinder < 0, "Failed to setupTransportPolling: %d", fdHwBinder);

        // Flush after setupPolling(), to make sure the binder driver
        // knows about this thread handling commands.
        IPCThreadState::self()->flushCommands();

        int ret = looper->addFd(fdHwBinder,
                                Looper::POLL_CALLBACK,
                                Looper::EVENT_INPUT,
                                cb,
                                nullptr /*data*/);
        LOG_ALWAYS_FATAL_IF(ret != 1, "Failed to add binder FD to Looper");

        return cb;
    }

    int handleEvent(int fd, int /*events*/, void* /*data*/) override {
        handleTransportPoll(fd);
        return 1;  // Continue receiving callbacks.
    }
};

// LooperCallback for IClientCallback
class ClientCallbackCallback : public LooperCallback {
public:
    static sp<ClientCallbackCallback> setupTo(const sp<Looper>& looper, const sp<ServiceManager>& manager) {
        sp<ClientCallbackCallback> cb = new ClientCallbackCallback(manager);

        int fdTimer = timerfd_create(CLOCK_MONOTONIC, 0 /*flags*/);
        LOG_ALWAYS_FATAL_IF(fdTimer < 0, "Failed to timerfd_create: fd: %d err: %d", fdTimer, errno);

        itimerspec timespec {
            .it_interval = {
                .tv_sec = 5,
                .tv_nsec = 0,
            },
            .it_value = {
                .tv_sec = 5,
                .tv_nsec = 0,
            },
        };

        int timeRes = timerfd_settime(fdTimer, 0 /*flags*/, &timespec, nullptr);
        LOG_ALWAYS_FATAL_IF(timeRes < 0, "Failed to timerfd_settime: res: %d err: %d", timeRes, errno);

        int addRes = looper->addFd(fdTimer,
                                   Looper::POLL_CALLBACK,
                                   Looper::EVENT_INPUT,
                                   cb,
                                   nullptr);
        LOG_ALWAYS_FATAL_IF(addRes != 1, "Failed to add client callback FD to Looper");

        return cb;
    }

    int handleEvent(int fd, int /*events*/, void* /*data*/) override {
        uint64_t expirations;
        int ret = read(fd, &expirations, sizeof(expirations));
        if (ret != sizeof(expirations)) {
            ALOGE("Read failed to callback FD: ret: %d err: %d", ret, errno);
        }

        mManager->handleClientCallbacks();
        return 1;  // Continue receiving callbacks.
    }
private:
    ClientCallbackCallback(const sp<ServiceManager>& manager) : mManager(manager) {}
    sp<ServiceManager> mManager;
};

int main() {
    // If hwservicemanager crashes, the system may be unstable and hard to debug. This is both why
    // we log this and why we care about this at all.
    setProcessHidlReturnRestriction(HidlReturnRestriction::ERROR_IF_UNCHECKED);

    // TODO(b/36424585): make fatal
    ProcessState::self()->setCallRestriction(ProcessState::CallRestriction::ERROR_IF_NOT_ONEWAY);
    auto transport = android::hardware::getTransport(ServiceManager::descriptor, serviceName);
    if (transport == android::vintf::Transport::EMPTY) {
        ALOGI("HIDL is not supported on this device so hwservicemanager is not needed");
        int rc = property_set("hwservicemanager.disabled", "true");
        if (rc) {
            LOG_ALWAYS_FATAL("Failed to set \"hwservicemanager.disabled\" (error %d).\"", rc);
        }
        // wait here for init to see the proprty and shut us down
        while (true) {
            ALOGW("Waiting on init to shut this process down.");
            sleep(10);
        }
    }

    sp<ServiceManager> manager = new ServiceManager();
    setRequestingSid(manager, true);

    if (!manager->add(serviceName, manager).withDefault(false)) {
        ALOGE("Failed to register hwservicemanager with itself.");
    }

    // Check to make sure we should be registering tokenManager first. Only if
    // it's declared in the manifest.
    sp<TokenManager> tokenManager;
    if (android::vintf::Transport::EMPTY !=
        android::hardware::getTransport(TokenManager::descriptor, serviceName)) {
        tokenManager = new TokenManager();
        if (!manager->add(serviceName, tokenManager).withDefault(false)) {
            ALOGE("Failed to register ITokenManager with hwservicemanager.");
        }
    } else {
        ALOGW("Not registering android.hidl.token service because it is no longer supported");
    }

    // Tell IPCThreadState we're the service manager
    sp<IBinder> binder = toBinder<IServiceManager>(manager);
    sp<BHwBinder> service = static_cast<BHwBinder*>(binder.get()); // local binder object
    IPCThreadState::self()->setTheContextObject(service);
    // Then tell the kernel
    ProcessState::self()->becomeContextManager();

    int rc = property_set("hwservicemanager.ready", "true");
    if (rc) {
        ALOGE("Failed to set \"hwservicemanager.ready\" (error %d). "\
              "HAL services will not start!\n", rc);
    }

    sp<Looper> looper = Looper::prepare(0 /* opts */);

    (void)HwBinderCallback::setupTo(looper);
    (void)ClientCallbackCallback::setupTo(looper, manager);

    ALOGI("hwservicemanager is ready now.");

    while (true) {
        looper->pollAll(-1 /* timeoutMillis */);
    }

    return 0;
}
