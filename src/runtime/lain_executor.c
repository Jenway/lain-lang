#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <process.h>
#include <winsock2.h>
#include <mswsock.h>
#include <windows.h>
#else
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#if defined(__linux__)
#include <linux/io_uring.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#endif
#endif

typedef struct LainSockAddrIn {
    uint16_t family;
    uint16_t port;
    uint32_t addr;
    uint8_t zero[8];
} LainSockAddrIn;

typedef char lain_sockaddr_in_size_must_match[(sizeof(LainSockAddrIn) == 16) ? 1 : -1];

typedef struct LainWaker {
    void *data;
    void (*wake_fn)(void *);
    void *next_data;
    void (*next_wake_fn)(void *);
} LainWaker;

typedef void (*LainSuspendFn)(LainWaker);
typedef void (*LainTaskFn)(LainSuspendFn);
typedef void (*LainTaskWithFn)(uintptr_t, LainSuspendFn);

typedef enum LainTaskKind {
    LAIN_TASK_NO_ARG = 0,
    LAIN_TASK_WITH_ARG = 1,
    LAIN_TASK_WAKE = 2,
    LAIN_TASK_RESUME = 3,
} LainTaskKind;

typedef enum LainExecutorBackend {
    LAIN_EXECUTOR_THREAD = 0,
    LAIN_EXECUTOR_IO_URING = 1,
    LAIN_EXECUTOR_IOCP = 2,
} LainExecutorBackend;

typedef struct LainQueuedTask {
    LainTaskKind kind;
    LainSuspendFn suspend;
    union {
        LainTaskFn no_arg;
        struct {
            LainTaskWithFn task;
            uintptr_t data;
        } with_arg;
        LainWaker wake;
    } body;
    struct LainQueuedTask *next;
} LainQueuedTask;

static LainQueuedTask *lain_queue_head = NULL;
static LainQueuedTask *lain_queue_tail = NULL;
static uint32_t lain_executor_backend = LAIN_EXECUTOR_THREAD;
static uint32_t lain_executor_workers = 0;
static int lain_main_done = 0;

typedef enum LainTcpWaitKind {
    LAIN_TCP_WAIT_READABLE = 0,
    LAIN_TCP_WAIT_WRITABLE = 1,
    LAIN_TCP_WAIT_ACCEPT = 2,
} LainTcpWaitKind;

typedef struct LainPendingAccept {
    uintptr_t listener;
    uintptr_t accepted;
    struct LainPendingAccept *next;
} LainPendingAccept;

typedef struct LainListeningSocket {
    uintptr_t socket;
    struct LainListeningSocket *next;
} LainListeningSocket;

static LainPendingAccept *lain_pending_accepts = NULL;
static LainListeningSocket *lain_listening_sockets = NULL;

void lain_tcp_wait_readable_wake(void *data);
void lain_tcp_wait_writable_wake(void *data);

static void lain_enqueue_resume(LainWaker waker);

#if defined(_WIN32)
__declspec(thread) static int lain_suspend_pending = 0;
__declspec(thread) static void *lain_suspend_parent_state = NULL;
__declspec(thread) static void *lain_suspend_parent_resume = NULL;
__declspec(thread) static void *lain_suspend_parent_result = NULL;
#else
static _Thread_local int lain_suspend_pending = 0;
static _Thread_local void *lain_suspend_parent_state = NULL;
static _Thread_local void *lain_suspend_parent_resume = NULL;
static _Thread_local void *lain_suspend_parent_result = NULL;
#endif

void lain_suspend_mark_pending(void) {
    lain_suspend_pending = 1;
}

int32_t lain_suspend_take_pending(void) {
    int32_t pending = lain_suspend_pending;
    lain_suspend_pending = 0;
    return pending;
}

void lain_suspend_set_parent(void *state, void *resume, void *result) {
    lain_suspend_parent_state = state;
    lain_suspend_parent_resume = resume;
    lain_suspend_parent_result = result;
}

void lain_suspend_take_parent(void **state, void **resume, void **result) {
    if (state != NULL) {
        *state = lain_suspend_parent_state;
    }
    if (resume != NULL) {
        *resume = lain_suspend_parent_resume;
    }
    if (result != NULL) {
        *result = lain_suspend_parent_result;
    }
    lain_suspend_parent_state = NULL;
    lain_suspend_parent_resume = NULL;
    lain_suspend_parent_result = NULL;
}

#if defined(_WIN32)
static INIT_ONCE lain_executor_once = INIT_ONCE_STATIC_INIT;
static CRITICAL_SECTION lain_executor_lock;
static CONDITION_VARIABLE lain_executor_cond;

static BOOL CALLBACK lain_executor_init_once(
    PINIT_ONCE init_once,
    PVOID parameter,
    PVOID *context
) {
    (void)init_once;
    (void)parameter;
    (void)context;
    InitializeCriticalSection(&lain_executor_lock);
    InitializeConditionVariable(&lain_executor_cond);
    return TRUE;
}

static void lain_lock(void) {
    InitOnceExecuteOnce(&lain_executor_once, lain_executor_init_once, NULL, NULL);
    EnterCriticalSection(&lain_executor_lock);
}

static void lain_unlock(void) {
    LeaveCriticalSection(&lain_executor_lock);
}

static void lain_wait(void) {
    SleepConditionVariableCS(&lain_executor_cond, &lain_executor_lock, INFINITE);
}

static void lain_signal(void) {
    WakeConditionVariable(&lain_executor_cond);
}
#else
static pthread_mutex_t lain_executor_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t lain_executor_cond = PTHREAD_COND_INITIALIZER;

static void lain_lock(void) {
    pthread_mutex_lock(&lain_executor_lock);
}

static void lain_unlock(void) {
    pthread_mutex_unlock(&lain_executor_lock);
}

static void lain_wait(void) {
    pthread_cond_wait(&lain_executor_cond, &lain_executor_lock);
}

static void lain_signal(void) {
    pthread_cond_signal(&lain_executor_cond);
}
#endif

void lain_suspend_park_main(void) {
    lain_lock();
    while (!lain_main_done) {
        lain_wait();
    }
    lain_main_done = 0;
    lain_unlock();
}

void lain_suspend_complete_main(void) {
    lain_lock();
    lain_main_done = 1;
    lain_signal();
    lain_unlock();
}

static uint32_t lain_current_backend(void) {
    lain_lock();
    uint32_t backend = lain_executor_backend;
    lain_unlock();
    return backend;
}

static void lain_waker_wake(LainWaker waker) {
    if (waker.wake_fn != NULL) {
        waker.wake_fn(waker.data);
    }
    if (waker.next_wake_fn != NULL) {
        waker.next_wake_fn(waker.next_data);
    }
}

static void lain_waker_resume(LainWaker waker) {
    if (waker.next_wake_fn != NULL) {
        waker.next_wake_fn(waker.next_data);
    }
}

static void lain_run_task(LainQueuedTask *task) {
    if (task->kind == LAIN_TASK_NO_ARG) {
        if (task->body.no_arg != NULL) {
            task->body.no_arg(task->suspend);
        }
    } else if (task->kind == LAIN_TASK_WITH_ARG && task->body.with_arg.task != NULL) {
        task->body.with_arg.task(task->body.with_arg.data, task->suspend);
    } else if (task->kind == LAIN_TASK_WAKE) {
        lain_waker_wake(task->body.wake);
    } else if (task->kind == LAIN_TASK_RESUME) {
        lain_waker_resume(task->body.wake);
    }
    free(task);
}

static LainQueuedTask *lain_pop_task_locked(void) {
    while (lain_queue_head == NULL) {
        lain_wait();
    }

    LainQueuedTask *task = lain_queue_head;
    lain_queue_head = task->next;
    if (lain_queue_head == NULL) {
        lain_queue_tail = NULL;
    }
    task->next = NULL;
    return task;
}

static void lain_worker_loop(void) {
    for (;;) {
        lain_lock();
        LainQueuedTask *task = lain_pop_task_locked();
        lain_unlock();
        lain_run_task(task);
    }
}

#if defined(_WIN32)
static unsigned __stdcall lain_worker_main(void *arg) {
    (void)arg;
    lain_worker_loop();
    return 0;
}

static int lain_start_worker_locked(void) {
    uintptr_t handle = _beginthreadex(NULL, 0, lain_worker_main, NULL, 0, NULL);
    if (handle == 0) {
        return 0;
    }
    CloseHandle((HANDLE)handle);
    lain_executor_workers += 1;
    return 1;
}
#else
static void *lain_worker_main(void *arg) {
    (void)arg;
    lain_worker_loop();
    return NULL;
}

static int lain_start_worker_locked(void) {
    pthread_t thread;
    if (pthread_create(&thread, NULL, lain_worker_main, NULL) != 0) {
        return 0;
    }
    pthread_detach(thread);
    lain_executor_workers += 1;
    return 1;
}
#endif

static void lain_ensure_workers(uint32_t threads) {
    if (threads == 0) {
        threads = 1;
    }

    lain_lock();
    while (lain_executor_workers < threads) {
        if (!lain_start_worker_locked()) {
            break;
        }
    }
    lain_unlock();
}

static void lain_enqueue_or_run(LainQueuedTask *task) {
    if (task == NULL) {
        return;
    }

    lain_ensure_workers(lain_executor_workers == 0 ? 1 : lain_executor_workers);

    lain_lock();
    if (lain_executor_workers == 0) {
        lain_unlock();
        lain_run_task(task);
        return;
    }

    task->next = NULL;
    if (lain_queue_tail == NULL) {
        lain_queue_head = task;
        lain_queue_tail = task;
    } else {
        lain_queue_tail->next = task;
        lain_queue_tail = task;
    }
    lain_signal();
    lain_unlock();
}

static void lain_enqueue_resume(LainWaker waker) {
    LainQueuedTask *queued = (LainQueuedTask *)malloc(sizeof(LainQueuedTask));
    if (queued == NULL) {
        lain_waker_resume(waker);
        return;
    }

    queued->kind = LAIN_TASK_RESUME;
    queued->suspend = NULL;
    queued->body.wake = waker;
    queued->next = NULL;
    lain_enqueue_or_run(queued);
}

static int lain_store_pending_accept(uintptr_t listener, uintptr_t accepted) {
    LainPendingAccept *pending = (LainPendingAccept *)malloc(sizeof(LainPendingAccept));
    if (pending == NULL) {
        return 0;
    }

    pending->listener = listener;
    pending->accepted = accepted;

    lain_lock();
    pending->next = lain_pending_accepts;
    lain_pending_accepts = pending;
    lain_unlock();
    return 1;
}

static uintptr_t lain_take_pending_accept(uintptr_t listener) {
    uintptr_t accepted = 0;

    lain_lock();
    LainPendingAccept **cursor = &lain_pending_accepts;
    while (*cursor != NULL) {
        LainPendingAccept *pending = *cursor;
        if (pending->listener == listener) {
            *cursor = pending->next;
            accepted = pending->accepted;
            free(pending);
            break;
        }
        cursor = &pending->next;
    }
    lain_unlock();
    return accepted;
}

static void lain_register_listener(uintptr_t socket_handle) {
    LainListeningSocket *node = (LainListeningSocket *)malloc(sizeof(LainListeningSocket));
    if (node == NULL) {
        return;
    }

    node->socket = socket_handle;

    lain_lock();
    LainListeningSocket *cursor = lain_listening_sockets;
    while (cursor != NULL) {
        if (cursor->socket == socket_handle) {
            lain_unlock();
            free(node);
            return;
        }
        cursor = cursor->next;
    }
    node->next = lain_listening_sockets;
    lain_listening_sockets = node;
    lain_unlock();
}

static void lain_unregister_listener(uintptr_t socket_handle) {
    lain_lock();
    LainListeningSocket **cursor = &lain_listening_sockets;
    while (*cursor != NULL) {
        LainListeningSocket *node = *cursor;
        if (node->socket == socket_handle) {
            *cursor = node->next;
            free(node);
            break;
        }
        cursor = &node->next;
    }
    lain_unlock();
}

static int lain_is_listener(uintptr_t socket_handle) {
    int result = 0;

    lain_lock();
    LainListeningSocket *cursor = lain_listening_sockets;
    while (cursor != NULL) {
        if (cursor->socket == socket_handle) {
            result = 1;
            break;
        }
        cursor = cursor->next;
    }
    lain_unlock();
    return result;
}

#if defined(_WIN32)
typedef struct LainIocpWait {
    OVERLAPPED overlapped;
    LainWaker waker;
    uintptr_t socket_handle;
    LainTcpWaitKind kind;
    SOCKET accepted_socket;
    WSABUF buffer;
    char byte;
    char accept_buffer[(sizeof(struct sockaddr_in) + 16) * 2];
} LainIocpWait;

static HANDLE lain_iocp_port = NULL;
static int lain_iocp_started = 0;
static LPFN_ACCEPTEX lain_iocp_acceptex_fn = NULL;

static unsigned __stdcall lain_iocp_completion_main(void *arg) {
    (void)arg;

    for (;;) {
        DWORD bytes = 0;
        ULONG_PTR key = 0;
        LPOVERLAPPED overlapped = NULL;
        BOOL ok = GetQueuedCompletionStatus(
            lain_iocp_port,
            &bytes,
            &key,
            &overlapped,
            INFINITE
        );

        if (overlapped == NULL) {
            continue;
        }

        LainIocpWait *wait = (LainIocpWait *)overlapped;
        if (wait->kind == LAIN_TCP_WAIT_ACCEPT) {
            if (ok && wait->accepted_socket != INVALID_SOCKET) {
                SOCKET listener = (SOCKET)wait->socket_handle;
                (void)setsockopt(
                    wait->accepted_socket,
                    SOL_SOCKET,
                    SO_UPDATE_ACCEPT_CONTEXT,
                    (const char *)&listener,
                    (int)sizeof(listener)
                );
                if (!lain_store_pending_accept(
                        wait->socket_handle,
                        (uintptr_t)wait->accepted_socket
                    )) {
                    closesocket(wait->accepted_socket);
                }
            } else if (wait->accepted_socket != INVALID_SOCKET) {
                closesocket(wait->accepted_socket);
            }
        }

        lain_enqueue_resume(wait->waker);
        free(wait);
    }

    return 0;
}

static int lain_iocp_ensure_started(void) {
    lain_lock();

    if (lain_iocp_port == NULL) {
        lain_iocp_port = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
    }

    if (lain_iocp_port != NULL && !lain_iocp_started) {
        uintptr_t handle =
            _beginthreadex(NULL, 0, lain_iocp_completion_main, NULL, 0, NULL);
        if (handle != 0) {
            CloseHandle((HANDLE)handle);
            lain_iocp_started = 1;
        }
    }

    int ok = lain_iocp_port != NULL && lain_iocp_started;
    lain_unlock();
    return ok;
}

static int lain_iocp_associate(SOCKET socket_handle) {
    if (!lain_iocp_ensure_started()) {
        return 0;
    }

    HANDLE result = CreateIoCompletionPort((HANDLE)socket_handle, lain_iocp_port, 0, 0);
    if (result != NULL) {
        return 1;
    }

    return GetLastError() == ERROR_INVALID_PARAMETER;
}

static LPFN_ACCEPTEX lain_iocp_get_acceptex(SOCKET listener) {
    lain_lock();
    LPFN_ACCEPTEX cached = lain_iocp_acceptex_fn;
    lain_unlock();
    if (cached != NULL) {
        return cached;
    }

    GUID guid = WSAID_ACCEPTEX;
    LPFN_ACCEPTEX accept_ex = NULL;
    DWORD bytes = 0;
    int result = WSAIoctl(
        listener,
        SIO_GET_EXTENSION_FUNCTION_POINTER,
        &guid,
        (DWORD)sizeof(guid),
        &accept_ex,
        (DWORD)sizeof(accept_ex),
        &bytes,
        NULL,
        NULL
    );
    if (result == SOCKET_ERROR || accept_ex == NULL) {
        return NULL;
    }

    lain_lock();
    if (lain_iocp_acceptex_fn == NULL) {
        lain_iocp_acceptex_fn = accept_ex;
    }
    cached = lain_iocp_acceptex_fn;
    lain_unlock();
    return cached;
}

static int lain_iocp_schedule_accept(uintptr_t socket_handle, LainWaker waker) {
    SOCKET listener = (SOCKET)socket_handle;
    if (!lain_iocp_associate(listener)) {
        return 0;
    }

    LPFN_ACCEPTEX accept_ex = lain_iocp_get_acceptex(listener);
    if (accept_ex == NULL) {
        return 0;
    }

    LainIocpWait *wait = (LainIocpWait *)calloc(1, sizeof(LainIocpWait));
    if (wait == NULL) {
        return 0;
    }

    wait->waker = waker;
    wait->socket_handle = socket_handle;
    wait->kind = LAIN_TCP_WAIT_ACCEPT;
    wait->accepted_socket = WSASocket(
        AF_INET,
        SOCK_STREAM,
        IPPROTO_TCP,
        NULL,
        0,
        WSA_FLAG_OVERLAPPED
    );
    if (wait->accepted_socket == INVALID_SOCKET) {
        free(wait);
        return 0;
    }

    DWORD bytes = 0;
    BOOL ok = accept_ex(
        listener,
        wait->accepted_socket,
        wait->accept_buffer,
        0,
        sizeof(struct sockaddr_in) + 16,
        sizeof(struct sockaddr_in) + 16,
        &bytes,
        &wait->overlapped
    );
    if (!ok && WSAGetLastError() != ERROR_IO_PENDING) {
        closesocket(wait->accepted_socket);
        free(wait);
        return 0;
    }

    return 1;
}

static int lain_iocp_schedule_socket_wait(
    LainTcpWaitKind kind,
    uintptr_t socket_handle,
    LainWaker waker
) {
    SOCKET socket_value = (SOCKET)socket_handle;
    if (!lain_iocp_associate(socket_value)) {
        return 0;
    }

    LainIocpWait *wait = (LainIocpWait *)calloc(1, sizeof(LainIocpWait));
    if (wait == NULL) {
        return 0;
    }

    wait->waker = waker;
    wait->socket_handle = socket_handle;
    wait->kind = kind;
    wait->accepted_socket = INVALID_SOCKET;
    wait->buffer.buf = &wait->byte;
    wait->buffer.len = 0;

    DWORD bytes = 0;
    int result;
    if (kind == LAIN_TCP_WAIT_WRITABLE) {
        result = WSASend(socket_value, &wait->buffer, 1, &bytes, 0, &wait->overlapped, NULL);
    } else {
        DWORD flags = 0;
        result = WSARecv(
            socket_value,
            &wait->buffer,
            1,
            &bytes,
            &flags,
            &wait->overlapped,
            NULL
        );
    }

    if (result == SOCKET_ERROR && WSAGetLastError() != WSA_IO_PENDING) {
        free(wait);
        return 0;
    }

    return 1;
}

static int lain_iocp_schedule(
    LainTcpWaitKind kind,
    uintptr_t socket_handle,
    LainWaker waker
) {
    if (kind == LAIN_TCP_WAIT_ACCEPT) {
        return lain_iocp_schedule_accept(socket_handle, waker);
    }
    return lain_iocp_schedule_socket_wait(kind, socket_handle, waker);
}

static void lain_iocp_configure(void) {
    (void)lain_iocp_ensure_started();
}
#else
static int lain_iocp_schedule(
    LainTcpWaitKind kind,
    uintptr_t socket_handle,
    LainWaker waker
) {
    (void)kind;
    (void)socket_handle;
    (void)waker;
    return 0;
}

static void lain_iocp_configure(void) {}
#endif

#if defined(__linux__) && !defined(_WIN32)
typedef struct LainIoUringWait {
    LainWaker waker;
} LainIoUringWait;

typedef struct LainIoUring {
    int fd;
    uint32_t entries;
    uint32_t *sq_head;
    uint32_t *sq_tail;
    uint32_t *sq_ring_mask;
    uint32_t *sq_array;
    struct io_uring_sqe *sqes;
    uint32_t *cq_head;
    uint32_t *cq_tail;
    uint32_t *cq_ring_mask;
    struct io_uring_cqe *cqes;
} LainIoUring;

static LainIoUring lain_io_uring = {.fd = -1};
static pthread_mutex_t lain_io_uring_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t lain_io_uring_submit_lock = PTHREAD_MUTEX_INITIALIZER;
static int lain_io_uring_started = 0;

static void *lain_io_uring_completion_main(void *arg) {
    (void)arg;

    for (;;) {
        int result;
        do {
            result = (int)syscall(
                __NR_io_uring_enter,
                lain_io_uring.fd,
                0,
                1,
                IORING_ENTER_GETEVENTS,
                NULL,
                0
            );
        } while (result < 0 && errno == EINTR);

        if (result < 0) {
            continue;
        }

        uint32_t head = __atomic_load_n(lain_io_uring.cq_head, __ATOMIC_ACQUIRE);
        uint32_t tail = __atomic_load_n(lain_io_uring.cq_tail, __ATOMIC_ACQUIRE);
        while (head != tail) {
            struct io_uring_cqe *cqe =
                &lain_io_uring.cqes[head & *lain_io_uring.cq_ring_mask];
            LainIoUringWait *wait = (LainIoUringWait *)(uintptr_t)cqe->user_data;
            if (wait != NULL) {
                lain_enqueue_resume(wait->waker);
                free(wait);
            }
            head += 1;
        }
        __atomic_store_n(lain_io_uring.cq_head, head, __ATOMIC_RELEASE);
    }

    return NULL;
}

static int lain_io_uring_init_locked(void) {
    if (lain_io_uring.fd >= 0) {
        return 1;
    }

    struct io_uring_params params;
    memset(&params, 0, sizeof(params));
    int fd = (int)syscall(__NR_io_uring_setup, 256, &params);
    if (fd < 0) {
        return 0;
    }

    size_t sq_ring_size =
        params.sq_off.array + (size_t)params.sq_entries * sizeof(uint32_t);
    size_t cq_ring_size =
        params.cq_off.cqes + (size_t)params.cq_entries * sizeof(struct io_uring_cqe);
    if ((params.features & IORING_FEAT_SINGLE_MMAP) && cq_ring_size > sq_ring_size) {
        sq_ring_size = cq_ring_size;
    }

    void *sq_ring = mmap(
        NULL,
        sq_ring_size,
        PROT_READ | PROT_WRITE,
        MAP_SHARED,
        fd,
        IORING_OFF_SQ_RING
    );
    if (sq_ring == MAP_FAILED) {
        close(fd);
        return 0;
    }

    void *cq_ring = sq_ring;
    if ((params.features & IORING_FEAT_SINGLE_MMAP) == 0) {
        cq_ring = mmap(
            NULL,
            cq_ring_size,
            PROT_READ | PROT_WRITE,
            MAP_SHARED,
            fd,
            IORING_OFF_CQ_RING
        );
        if (cq_ring == MAP_FAILED) {
            munmap(sq_ring, sq_ring_size);
            close(fd);
            return 0;
        }
    }

    struct io_uring_sqe *sqes = mmap(
        NULL,
        (size_t)params.sq_entries * sizeof(struct io_uring_sqe),
        PROT_READ | PROT_WRITE,
        MAP_SHARED,
        fd,
        IORING_OFF_SQES
    );
    if (sqes == MAP_FAILED) {
        if ((params.features & IORING_FEAT_SINGLE_MMAP) == 0) {
            munmap(cq_ring, cq_ring_size);
        }
        munmap(sq_ring, sq_ring_size);
        close(fd);
        return 0;
    }

    lain_io_uring.fd = fd;
    lain_io_uring.entries = params.sq_entries;
    lain_io_uring.sq_head = (uint32_t *)((char *)sq_ring + params.sq_off.head);
    lain_io_uring.sq_tail = (uint32_t *)((char *)sq_ring + params.sq_off.tail);
    lain_io_uring.sq_ring_mask = (uint32_t *)((char *)sq_ring + params.sq_off.ring_mask);
    lain_io_uring.sq_array = (uint32_t *)((char *)sq_ring + params.sq_off.array);
    lain_io_uring.sqes = sqes;
    lain_io_uring.cq_head = (uint32_t *)((char *)cq_ring + params.cq_off.head);
    lain_io_uring.cq_tail = (uint32_t *)((char *)cq_ring + params.cq_off.tail);
    lain_io_uring.cq_ring_mask = (uint32_t *)((char *)cq_ring + params.cq_off.ring_mask);
    lain_io_uring.cqes = (struct io_uring_cqe *)((char *)cq_ring + params.cq_off.cqes);
    return 1;
}

static int lain_io_uring_ensure_started(void) {
    pthread_mutex_lock(&lain_io_uring_lock);
    int ok = lain_io_uring_init_locked();
    if (ok && !lain_io_uring_started) {
        pthread_t thread;
        if (pthread_create(&thread, NULL, lain_io_uring_completion_main, NULL) == 0) {
            pthread_detach(thread);
            lain_io_uring_started = 1;
        }
    }
    ok = ok && lain_io_uring_started;
    pthread_mutex_unlock(&lain_io_uring_lock);
    return ok;
}

static int lain_io_uring_schedule(
    LainTcpWaitKind kind,
    uintptr_t socket_handle,
    LainWaker waker
) {
    if (!lain_io_uring_ensure_started()) {
        return 0;
    }

    LainIoUringWait *wait = (LainIoUringWait *)malloc(sizeof(LainIoUringWait));
    if (wait == NULL) {
        return 0;
    }
    wait->waker = waker;

    pthread_mutex_lock(&lain_io_uring_submit_lock);
    uint32_t head = __atomic_load_n(lain_io_uring.sq_head, __ATOMIC_ACQUIRE);
    uint32_t tail = __atomic_load_n(lain_io_uring.sq_tail, __ATOMIC_RELAXED);
    if (tail - head >= lain_io_uring.entries) {
        pthread_mutex_unlock(&lain_io_uring_submit_lock);
        free(wait);
        return 0;
    }

    uint32_t slot = tail & *lain_io_uring.sq_ring_mask;
    struct io_uring_sqe *sqe = &lain_io_uring.sqes[slot];
    memset(sqe, 0, sizeof(*sqe));
    sqe->opcode = IORING_OP_POLL_ADD;
    sqe->fd = (int)socket_handle;
    sqe->poll_events = kind == LAIN_TCP_WAIT_WRITABLE ? POLLOUT : POLLIN;
    sqe->user_data = (uint64_t)(uintptr_t)wait;
    lain_io_uring.sq_array[slot] = slot;
    __atomic_store_n(lain_io_uring.sq_tail, tail + 1, __ATOMIC_RELEASE);

    int result;
    do {
        result = (int)syscall(
            __NR_io_uring_enter,
            lain_io_uring.fd,
            1,
            0,
            0,
            NULL,
            0
        );
    } while (result < 0 && errno == EINTR);

    if (result < 0) {
        __atomic_store_n(lain_io_uring.sq_tail, tail, __ATOMIC_RELEASE);
        pthread_mutex_unlock(&lain_io_uring_submit_lock);
        free(wait);
        return 0;
    }

    pthread_mutex_unlock(&lain_io_uring_submit_lock);
    return 1;
}

static void lain_io_uring_configure(void) {
    (void)lain_io_uring_ensure_started();
}
#else
static int lain_io_uring_schedule(
    LainTcpWaitKind kind,
    uintptr_t socket_handle,
    LainWaker waker
) {
    (void)kind;
    (void)socket_handle;
    (void)waker;
    return 0;
}

static void lain_io_uring_configure(void) {}
#endif

static int lain_backend_try_schedule_waker(LainWaker waker) {
    LainTcpWaitKind kind;
    if (waker.wake_fn == lain_tcp_wait_readable_wake) {
        kind = LAIN_TCP_WAIT_READABLE;
    } else if (waker.wake_fn == lain_tcp_wait_writable_wake) {
        kind = LAIN_TCP_WAIT_WRITABLE;
    } else {
        return 0;
    }

    uintptr_t socket_handle = (uintptr_t)waker.data;
    uint32_t backend = lain_current_backend();
    if (backend == LAIN_EXECUTOR_IO_URING) {
        return lain_io_uring_schedule(kind, socket_handle, waker);
    }
    if (backend == LAIN_EXECUTOR_IOCP) {
        if (kind == LAIN_TCP_WAIT_READABLE && lain_is_listener(socket_handle)) {
            kind = LAIN_TCP_WAIT_ACCEPT;
        }
        return lain_iocp_schedule(kind, socket_handle, waker);
    }
    return 0;
}

static void lain_configure_async_backend(uint32_t backend) {
    if (backend == LAIN_EXECUTOR_IO_URING) {
        lain_io_uring_configure();
    } else if (backend == LAIN_EXECUTOR_IOCP) {
        lain_iocp_configure();
    }
}

void lain_executor_configure(uint32_t backend, uint32_t threads) {
    lain_lock();
    lain_executor_backend = backend;
    lain_unlock();

    (void)lain_executor_backend;
    lain_configure_async_backend(backend);
    lain_ensure_workers(threads == 0 ? 1 : threads);
}

void lain_executor_spawn(LainTaskFn task, LainSuspendFn suspend) {
    LainQueuedTask *queued = (LainQueuedTask *)malloc(sizeof(LainQueuedTask));
    if (queued == NULL) {
        if (task != NULL) {
            task(suspend);
        }
        return;
    }

    queued->kind = LAIN_TASK_NO_ARG;
    queued->suspend = suspend;
    queued->body.no_arg = task;
    queued->next = NULL;
    lain_enqueue_or_run(queued);
}

void lain_executor_spawn_with(LainTaskWithFn task, uintptr_t data, LainSuspendFn suspend) {
    LainQueuedTask *queued = (LainQueuedTask *)malloc(sizeof(LainQueuedTask));
    if (queued == NULL) {
        if (task != NULL) {
            task(data, suspend);
        }
        return;
    }

    queued->kind = LAIN_TASK_WITH_ARG;
    queued->suspend = suspend;
    queued->body.with_arg.task = task;
    queued->body.with_arg.data = data;
    queued->next = NULL;
    lain_enqueue_or_run(queued);
}

void lain_executor_wake(LainWaker waker) {
    if (lain_backend_try_schedule_waker(waker)) {
        return;
    }

    LainQueuedTask *queued = (LainQueuedTask *)malloc(sizeof(LainQueuedTask));
    if (queued == NULL) {
        lain_waker_wake(waker);
        return;
    }

    queued->kind = LAIN_TASK_WAKE;
    queued->suspend = NULL;
    queued->body.wake = waker;
    queued->next = NULL;
    lain_enqueue_or_run(queued);
}

void lain_executor_wake_parts(
    void *data,
    void (*wake_fn)(void *),
    void *next_data,
    void (*next_wake_fn)(void *)
) {
    LainWaker waker;
    waker.data = data;
    waker.wake_fn = wake_fn;
    waker.next_data = next_data;
    waker.next_wake_fn = next_wake_fn;
    lain_executor_wake(waker);
}

static int lain_tcp_wait_readable(uintptr_t socket_handle) {
#if defined(_WIN32)
    fd_set read_set;
    FD_ZERO(&read_set);
    FD_SET((SOCKET)socket_handle, &read_set);
    return select(0, &read_set, NULL, NULL, NULL) > 0;
#else
    struct pollfd fd;
    fd.fd = (int)socket_handle;
    fd.events = POLLIN;
    fd.revents = 0;

    int result;
    do {
        result = poll(&fd, 1, -1);
    } while (result < 0 && errno == EINTR);
    return result > 0 && (fd.revents & (POLLIN | POLLHUP | POLLERR)) != 0;
#endif
}

static int lain_tcp_wait_writable(uintptr_t socket_handle) {
#if defined(_WIN32)
    fd_set write_set;
    FD_ZERO(&write_set);
    FD_SET((SOCKET)socket_handle, &write_set);
    return select(0, NULL, &write_set, NULL, NULL) > 0;
#else
    struct pollfd fd;
    fd.fd = (int)socket_handle;
    fd.events = POLLOUT;
    fd.revents = 0;

    int result;
    do {
        result = poll(&fd, 1, -1);
    } while (result < 0 && errno == EINTR);
    return result > 0 && (fd.revents & (POLLOUT | POLLHUP | POLLERR)) != 0;
#endif
}

void lain_tcp_wait_readable_wake(void *data) {
    (void)lain_tcp_wait_readable((uintptr_t)data);
}

void lain_tcp_wait_writable_wake(void *data) {
    (void)lain_tcp_wait_writable((uintptr_t)data);
}

int32_t lain_tcp_startup(void) {
#if defined(_WIN32)
    WSADATA data;
    return WSAStartup(MAKEWORD(2, 2), &data);
#else
    return 0;
#endif
}

uintptr_t lain_tcp_socket(int32_t af, int32_t kind, int32_t protocol) {
#if defined(_WIN32)
    SOCKET socket_handle = WSASocket(
        af,
        kind,
        protocol,
        NULL,
        0,
        WSA_FLAG_OVERLAPPED
    );
    if (socket_handle == INVALID_SOCKET) {
        return 0;
    }
    BOOL reuse = TRUE;
    (void)setsockopt(
        socket_handle,
        SOL_SOCKET,
        SO_REUSEADDR,
        (const char *)&reuse,
        (int)sizeof(reuse)
    );
    return (uintptr_t)socket_handle;
#else
    int fd = socket(af, kind, protocol);
    if (fd < 0) {
        return 0;
    }
    int reuse = 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, (socklen_t)sizeof(reuse));
    return (uintptr_t)fd;
#endif
}

int32_t lain_tcp_bind(uintptr_t socket_handle, const LainSockAddrIn *addr, int32_t len) {
    if (socket_handle == 0 || addr == NULL) {
        return -1;
    }

#if defined(_WIN32)
    return bind(
        (SOCKET)socket_handle,
        (const struct sockaddr *)addr,
        (int)len
    );
#else
    return bind(
        (int)socket_handle,
        (const struct sockaddr *)addr,
        (socklen_t)len
    );
#endif
}

int32_t lain_tcp_listen(uintptr_t socket_handle, int32_t backlog) {
    if (socket_handle == 0) {
        return -1;
    }

#if defined(_WIN32)
    int result = listen((SOCKET)socket_handle, backlog);
#else
    int result = listen((int)socket_handle, backlog);
#endif
    if (result == 0) {
        lain_register_listener(socket_handle);
    }
    return result;
}

uintptr_t lain_tcp_accept(uintptr_t socket_handle, void *addr, int32_t *len) {
    if (socket_handle == 0) {
        return 0;
    }

    uintptr_t pending = lain_take_pending_accept(socket_handle);
    if (pending != 0) {
        return pending;
    }

#if defined(_WIN32)
    int accepted_len = len == NULL ? 0 : (int)*len;
    SOCKET accepted = accept(
        (SOCKET)socket_handle,
        (struct sockaddr *)addr,
        len == NULL ? NULL : &accepted_len
    );
    if (len != NULL) {
        *len = (int32_t)accepted_len;
    }
    if (accepted == INVALID_SOCKET) {
        return 0;
    }
    return (uintptr_t)accepted;
#else
    socklen_t accepted_len = len == NULL ? 0 : (socklen_t)*len;
    int accepted = accept(
        (int)socket_handle,
        (struct sockaddr *)addr,
        len == NULL ? NULL : &accepted_len
    );
    if (len != NULL) {
        *len = (int32_t)accepted_len;
    }
    if (accepted < 0) {
        return 0;
    }
    return (uintptr_t)accepted;
#endif
}

int32_t lain_tcp_recv(uintptr_t socket_handle, uint8_t *buffer, int32_t len, int32_t flags) {
    if (socket_handle == 0 || buffer == NULL || len < 0) {
        return -1;
    }

#if defined(_WIN32)
    int result = recv((SOCKET)socket_handle, (char *)buffer, (int)len, (int)flags);
    return result;
#else
    return (int32_t)recv((int)socket_handle, buffer, (size_t)len, flags);
#endif
}

int32_t lain_tcp_send(uintptr_t socket_handle, const uint8_t *buffer, int32_t len, int32_t flags) {
    if (socket_handle == 0 || buffer == NULL || len < 0) {
        return -1;
    }

#if defined(_WIN32)
    int result = send((SOCKET)socket_handle, (const char *)buffer, (int)len, (int)flags);
    return result;
#else
    return (int32_t)send((int)socket_handle, buffer, (size_t)len, flags);
#endif
}

int32_t lain_tcp_close(uintptr_t socket_handle) {
    if (socket_handle == 0) {
        return -1;
    }
    lain_unregister_listener(socket_handle);

#if defined(_WIN32)
    return closesocket((SOCKET)socket_handle);
#else
    return close((int)socket_handle);
#endif
}

uint16_t lain_tcp_htons(uint16_t host_short) {
    return htons(host_short);
}

int32_t lain_tcp_shutdown(void) {
#if defined(_WIN32)
    return WSACleanup();
#else
    return 0;
#endif
}
