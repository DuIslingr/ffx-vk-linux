#include "win32_runtime.h"

#include "teb.h"

#include <pthread.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <unordered_map>

#define WINAPI __attribute__((ms_abi))

namespace ffxvk {
namespace {

constexpr uint32_t invalidHandle = 6, invalidParameter = 87, notEnoughMemory = 8,
                   notSupported = 50;
constexpr uint32_t infinite = 0xffffffffu, waitFailed = 0xffffffffu, waitTimeout = 258;

struct Handle {
    enum Kind { event, thread } kind;
    std::mutex mutex;
    std::condition_variable changed;
    bool signaled = false;
    bool manualReset = false;
    bool started = false;
    uint32_t tid = 0;
    uint32_t exitCode = 0;
    explicit Handle(Kind k) : kind(k) {}
};

std::mutex handlesMutex;
std::unordered_map<uintptr_t, std::shared_ptr<Handle>> handles;
// Never recycle an ID: a stale caller handle cannot refer to a later object.
uintptr_t nextHandle = 0x10000;

void* insert(const std::shared_ptr<Handle>& object) {
    std::lock_guard lock(handlesMutex);
    if (nextHandle > std::numeric_limits<uintptr_t>::max() - 4) return nullptr;
    const uintptr_t id = nextHandle;
    nextHandle += 4;
    handles.emplace(id, object);
    return reinterpret_cast<void*>(id);
}

std::shared_ptr<Handle> lookup(void* handle) {
    std::lock_guard lock(handlesMutex);
    const auto found = handles.find(reinterpret_cast<uintptr_t>(handle));
    return found == handles.end() ? nullptr : found->second;
}

WINAPI void* localAlloc(uint32_t flags, size_t size) {
    // LMEM_MOVEABLE requires a separate movable-memory API; these DLLs use fixed blocks.
    if (flags & ~0x40u) return lastError() = notSupported, nullptr;
    void* p = (flags & 0x40) ? std::calloc(1, size ? size : 1) : std::malloc(size ? size : 1);
    if (!p) lastError() = notEnoughMemory;
    return p;
}

WINAPI void* createEvent(void* security, int manualReset, int initialState, const char16_t* name) {
    // Named interprocess objects and inheritable handles have no native-host equivalent.
    if (security || (name && *name)) return lastError() = notSupported, nullptr;
    try {
        auto object = std::make_shared<Handle>(Handle::event);
        object->manualReset = manualReset != 0;
        object->signaled = initialState != 0;
        void* handle = insert(object);
        if (!handle) lastError() = notEnoughMemory;
        else lastError() = 0;
        return handle;
    } catch (const std::bad_alloc&) {
        return lastError() = notEnoughMemory, nullptr;
    }
}

WINAPI int setEvent(void* handle) {
    const auto object = lookup(handle);
    if (!object || object->kind != Handle::event) return lastError() = invalidHandle, 0;
    std::lock_guard lock(object->mutex);
    object->signaled = true;
    if (object->manualReset) object->changed.notify_all();
    else object->changed.notify_one();
    return 1;
}

WINAPI int resetEvent(void* handle) {
    const auto object = lookup(handle);
    if (!object || object->kind != Handle::event) return lastError() = invalidHandle, 0;
    std::lock_guard lock(object->mutex);
    object->signaled = false;
    return 1;
}

WINAPI uint32_t waitForSingleObject(void* handle, uint32_t timeout) {
    const auto object = lookup(handle);
    if (!object) return lastError() = invalidHandle, waitFailed;
    std::unique_lock lock(object->mutex);
    if (timeout == infinite) object->changed.wait(lock, [&] { return object->signaled; });
    else if (!object->changed.wait_for(lock, std::chrono::milliseconds(timeout), [&] { return object->signaled; }))
        return waitTimeout;
    if (object->kind == Handle::event && !object->manualReset) object->signaled = false;
    return 0;
}

WINAPI int closeHandle(void* handle) {
    std::lock_guard lock(handlesMutex);
    if (!handles.erase(reinterpret_cast<uintptr_t>(handle))) return lastError() = invalidHandle, 0;
    // Running threads and existing waiters retain shared ownership of the state.
    return 1;
}

using ThreadEntry = uint32_t WINAPI (*)(void*);
struct ThreadStart {
    std::shared_ptr<Handle> object;
    ThreadEntry entry;
    void* argument;
};

void* startThread(void* raw) {
    std::unique_ptr<ThreadStart> start(static_cast<ThreadStart*>(raw));
    auto object = start->object;
    ensureThread();
    {
        std::lock_guard lock(object->mutex);
        object->tid = uint32_t(syscall(SYS_gettid));
        object->started = true;
        object->changed.notify_all();
    }
    const uint32_t code = start->entry(start->argument);
    {
        std::lock_guard lock(object->mutex);
        object->exitCode = code;
        object->signaled = true;
        object->changed.notify_all();
    }
    return nullptr;
}

WINAPI void* createThread(void* security, size_t stackSize, ThreadEntry entry, void* argument,
                          uint32_t flags, uint32_t* threadId) {
    // CREATE_SUSPENDED cannot be represented by a normally running pthread.
    if (security || flags) return lastError() = notSupported, nullptr;
    if (!entry) return lastError() = invalidParameter, nullptr;
    try {
        auto object = std::make_shared<Handle>(Handle::thread);
        auto start = std::make_unique<ThreadStart>(ThreadStart{object, entry, argument});
        pthread_attr_t attr;
        if (pthread_attr_init(&attr)) return lastError() = notEnoughMemory, nullptr;
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        if (stackSize) {
            const size_t page = size_t(sysconf(_SC_PAGESIZE));
            const size_t minimum = std::max(stackSize, size_t(PTHREAD_STACK_MIN));
            if (minimum > std::numeric_limits<size_t>::max() - page + 1 ||
                pthread_attr_setstacksize(&attr, (minimum + page - 1) / page * page)) {
                pthread_attr_destroy(&attr);
                return lastError() = invalidParameter, nullptr;
            }
        }
        void* handle = insert(object);
        if (!handle) {
            pthread_attr_destroy(&attr);
            return lastError() = notEnoughMemory, nullptr;
        }
        pthread_t native;
        const int error = pthread_create(&native, &attr, startThread, start.get());
        pthread_attr_destroy(&attr);
        if (error) {
            closeHandle(handle);
            return lastError() = notEnoughMemory, nullptr;
        }
        start.release();
        std::unique_lock lock(object->mutex);
        object->changed.wait(lock, [&] { return object->started; });
        if (threadId) *threadId = object->tid;
        return handle;
    } catch (const std::bad_alloc&) {
        return lastError() = notEnoughMemory, nullptr;
    }
}

WINAPI int getExitCodeThread(void* handle, uint32_t* code) {
    const auto object = lookup(handle);
    if (!object || object->kind != Handle::thread) return lastError() = invalidHandle, 0;
    if (!code) return lastError() = invalidParameter, 0;
    std::lock_guard lock(object->mutex);
    *code = object->signaled ? object->exitCode : 259; // STILL_ACTIVE
    return 1;
}

uint32_t currentOrHandleTid(void* handle) {
    if (handle == reinterpret_cast<void*>(intptr_t(-2))) return uint32_t(syscall(SYS_gettid));
    const auto object = lookup(handle);
    if (!object || object->kind != Handle::thread) return 0;
    std::lock_guard lock(object->mutex);
    return object->signaled ? 0 : object->tid;
}

WINAPI int setThreadPriority(void* handle, int priority) {
    const uint32_t tid = currentOrHandleTid(handle);
    if (!tid) return lastError() = invalidHandle, 0;
    // Win32 priority classes have no exact Linux equivalent; nice preserves the direction.
    int niceValue;
    if (priority == -15) niceValue = 19; // THREAD_PRIORITY_IDLE
    else if (priority == 15) niceValue = -20; // THREAD_PRIORITY_TIME_CRITICAL
    else if (priority >= -2 && priority <= 2) niceValue = -priority * 5;
    else return lastError() = invalidParameter, 0;
    if (setpriority(PRIO_PROCESS, id_t(tid), niceValue) != 0)
        return lastError() = errno == EPERM || errno == EACCES ? 5 : invalidParameter, 0;
    return 1;
}

WINAPI int32_t setThreadDescription(void* handle, const char16_t* description) {
    const uint32_t tid = currentOrHandleTid(handle);
    if (!tid || !description) return int32_t(0x80070057u); // E_INVALIDARG
    std::string name;
    for (size_t i = 0; description[i]; ++i) {
        uint32_t c = description[i];
        if (c >= 0xd800 && c <= 0xdbff && description[i + 1] >= 0xdc00 && description[i + 1] <= 0xdfff)
            c = 0x10000 + ((c - 0xd800) << 10) + description[++i] - 0xdc00;
        std::string piece;
        if (c < 0x80) piece += char(c);
        else if (c < 0x800) piece += char(0xc0 | c >> 6), piece += char(0x80 | (c & 63));
        else if (c < 0x10000) piece += char(0xe0 | c >> 12), piece += char(0x80 | ((c >> 6) & 63)), piece += char(0x80 | (c & 63));
        else piece += char(0xf0 | c >> 18), piece += char(0x80 | ((c >> 12) & 63)), piece += char(0x80 | ((c >> 6) & 63)), piece += char(0x80 | (c & 63));
        if (name.size() + piece.size() > 15) break;
        name += piece;
    }
    FILE* file = std::fopen(("/proc/self/task/" + std::to_string(tid) + "/comm").c_str(), "w");
    if (!file) return int32_t(0x80070000u | uint32_t(errno));
    const bool wrote = std::fwrite(name.data(), 1, name.size(), file) == name.size();
    const bool closed = std::fclose(file) == 0;
    return wrote && closed ? 0 : int32_t(0x80004005u);
}

struct CriticalSection { std::recursive_mutex mutex; };
CriticalSection* criticalSection(void* storage) {
    CriticalSection* pointer;
    std::memcpy(&pointer, storage, sizeof pointer);
    return pointer;
}
WINAPI void initializeCriticalSection(void* storage) {
    auto* object = new CriticalSection;
    std::memset(storage, 0, 40); // sizeof(CRITICAL_SECTION), x86-64 Windows
    std::memcpy(storage, &object, sizeof object);
    const int32_t unlocked = -1;
    std::memcpy(static_cast<char*>(storage) + 8, &unlocked, sizeof unlocked);
}
WINAPI void enterCriticalSection(void* storage) { criticalSection(storage)->mutex.lock(); }
WINAPI void leaveCriticalSection(void* storage) { criticalSection(storage)->mutex.unlock(); }
WINAPI void deleteCriticalSection(void* storage) {
    delete criticalSection(storage);
    std::memset(storage, 0, 40);
}
WINAPI void sleep(uint32_t ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

} // namespace

void* runtimeStandIn(const std::string& dll, const std::string& name) {
    if (dll != "kernel32.dll") return nullptr;
#define ENTRY(text, function) if (name == text) return reinterpret_cast<void*>(&function)
    ENTRY("LocalAlloc", localAlloc);
    ENTRY("CreateEventW", createEvent);
    ENTRY("SetEvent", setEvent);
    ENTRY("ResetEvent", resetEvent);
    ENTRY("WaitForSingleObject", waitForSingleObject);
    ENTRY("CloseHandle", closeHandle);
    ENTRY("CreateThread", createThread);
    ENTRY("GetExitCodeThread", getExitCodeThread);
    ENTRY("SetThreadPriority", setThreadPriority);
    ENTRY("SetThreadDescription", setThreadDescription);
    ENTRY("InitializeCriticalSection", initializeCriticalSection);
    ENTRY("EnterCriticalSection", enterCriticalSection);
    ENTRY("LeaveCriticalSection", leaveCriticalSection);
    ENTRY("DeleteCriticalSection", deleteCriticalSection);
    ENTRY("Sleep", sleep);
#undef ENTRY
    return nullptr;
}

} // namespace ffxvk
