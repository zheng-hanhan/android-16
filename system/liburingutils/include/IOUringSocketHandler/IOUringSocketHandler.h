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

#pragma once

#include <memory>
#include <vector>

#include <liburing.h>

/*
 * IOUringSocketHandler is a helper class for using io_uring with a socket.
 *
 * Typical usage from a given thread:
 *
 * As a one time setup:
 * 1. Create an instance of IOUringSocketHandler with the socket file descriptor.
 * 2. Setup io_uring ring buffer.
 * 3. Allocate buffers for the ring buffer.
 * 4. Register buffers with io_uring.
 * 5. EnqueueMultishotRecvmsg() will submit the SQE to receive the data
 *
 * In the I/O path:
 *
 * 6. Receive data from the socket through ReceiveData()
 * 7. Release the buffer to io_uring.
 *
 * Note that the thread which sets up the io_uring instance should handle the
 * I/O through ReceiveData() call.
 */

class IOUringSocketHandler {
public:
    IOUringSocketHandler(int socket_fd);
    ~IOUringSocketHandler();

    // Setup io_uring ring buffer
    // queue_size: The size of the io_uring submission queue.
    //             Determines the maximum number of outstanding I/O requests.
    // return: true on success, false on failure (e.g., if io_uring_setup fails).
    //
    // This function initializes the io_uring context and sets up the submission
    // and completion queues.  It prepares the io_uring instance for I/O operations.
    // A larger queue_size allows for more concurrent I/O operations but consumes
    // more memory.
    bool SetupIoUring(int queue_size);

    // Allocate 'num_buffers' of size 'buf_size'
    //
    // num_buffers: The number of buffers to allocate - Should be power of 2.
    // buf_size: The size of each buffer in bytes.
    //
    // This function allocates a set of buffers that will be used for I/O operations
    // with io_uring. These buffers are typically used to hold data that is read from
    // or written to files or sockets. The allocated buffers are managed internally
    // and are later registered with io_uring.
    //
    // The num_buffers will be the payload for the caller. Internally, it
    // allocates additional metadata:
    //   a: sizeof(struct ucred) + sizeof(struct cmsghdr)
    //   b: sizeof(struct io_uring_recvmsg_out)
    // This allows sender to send the ucred credential information if required.
    //
    // This function also registers the allocated buffers with the io_uring instance.
    // Registering buffers allows the kernel to access them directly, avoiding the need
    // to copy data between user space and kernel space during I/O operations. This
    // improves performance.
    //
    // Please see additional details on how num_buffers will be used
    // by the io_uring: https://man7.org/linux/man-pages/man3/io_uring_setup_buf_ring.3.html
    bool AllocateAndRegisterBuffers(size_t num_buffers, size_t buf_size);

    // Free up registered buffers with the io_uring instance.
    //
    // All the buffers allocated using AllocateAndRegisterBuffers() API will be
    // freed and de-registered. Callers can then call
    // AllocateAndRegisterBuffers() to re-register new set of bufferes with the
    // ring.
    void DeRegisterBuffers();

    // ARM io_uring recvmsg opcode
    //
    // return: true on success, false on failure (e.g., if submission queue is full).
    //
    // This function enqueues a "multishot recvmsg" operation into the io_uring submission queue.
    // Multishot recvmsg allows receiving multiple messages from a socket with a single
    // io_uring submission. The function prepares the submission queue
    // entry (SQE) for the recvmsg operation.
    bool EnqueueMultishotRecvmsg();

    // Release the buffer to io_uring
    //
    // This function releases a buffer back to the io_uring subsystem after it has been
    // used for an I/O operation.  This makes the buffer available for reuse in subsequent
    // I/O operations.
    //
    // Additionally, when the buffer is released, a check is done to see if
    // there are more CQE entries available. If not, EnqueueMultishotRecvmsg()
    // is invoked so that the SQE submission is done for receiving next set of
    // I/O.
    void ReleaseBuffer();

    // Receive payload data of size payload_len. Additionally, receive
    // credential data.
    //
    // payload: A pointer to a void pointer.  This will be set to point to the received
    // payload data.
    //
    // payload_len: A reference to a size_t. This will be set to the length of the
    // received payload data.
    //
    // cred: A pointer to a struct ucred pointer. This will be set to point to the
    //       user credentials associated with the received data (if available).
    //       If the sender doesn't have credential information in the payload,
    //       then nullptr will be returned.
    //
    // This function retrieves the data received from a recvmsg operation. It extracts the payload
    // data and its length, as well as the user credentials associated with the sender. The
    // caller is responsible for freeing the allocated memory for the payload and credentials
    // when they are no longer needed.
    void ReceiveData(void** payload, size_t& payload_len, struct ucred** cred);

    // check if io_uring is supported
    //
    // return: true if io_uring is supported by the kernel, false otherwise.
    //
    // This function checks if the io_uring feature is supported by the underlying Linux kernel.
    static bool isIouringEnabled();

private:
    static bool isIouringSupportedByKernel();
    // Register buffers with io_uring
    //
    // return: true on success, false on failure (e.g., if io_uring_register_buffers fails).
    //
    // This function registers the previously allocated buffers with the io_uring instance.
    // Registering buffers allows the kernel to access them directly, avoiding the need
    // to copy data between user space and kernel space during I/O operations. This
    // improves performance.
    bool RegisterBuffers();

    struct uring_context {
        struct io_uring ring;
    };
    // Socket fd
    int socket_;
    std::unique_ptr<uring_context> mCtx;
    std::vector<std::unique_ptr<uint8_t[]>> buffers_;
    struct msghdr msg;
    int control_len_;
    size_t num_buffers_ = 0;
    int buffer_size_;
    int active_buffer_id_ = -1;
    struct io_uring_cqe* cqe;
    // A constant buffer group id as we don't support multiple buffer groups
    // yet.
    const int bgid_ = 7;
    struct io_uring_buf_ring* br_;
    bool registered_buffers_ = false;
    bool ring_setup_ = false;
};
