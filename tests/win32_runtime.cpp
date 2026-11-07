#include "win32_runtime.h"
#include "teb.h"

#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

#define WINAPI __attribute__((ms_abi))

template <typename T> T api(const char* name) {
    void* p = ffxvk::runtimeStandIn("kernel32.dll", name);
    assert(p);
    return reinterpret_cast<T>(p);
}

using CreateEvent = void* WINAPI (*)(void*, int, int, const char16_t*);
using EventOperation = int WINAPI (*)(void*);
using Wait = uint32_t WINAPI (*)(void*, uint32_t);
using Entry = uint32_t WINAPI (*)(void*);
using CreateThread = void* WINAPI (*)(void*, size_t, Entry, void*, uint32_t, uint32_t*);
using ExitCode = int WINAPI (*)(void*, uint32_t*);
using Critical = void WINAPI (*)(void*);

struct Job {
    void* event;
    std::atomic<bool> ran = false;
    std::atomic<bool> done = false;
};
static uint32_t WINAPI threadEntry(void* p) {
    auto& job = *static_cast<Job*>(p);
    // Accessing lastError exercises the TEB on this newly created Windows thread.
    ffxvk::lastError() = 1234;
    job.ran = true;
    assert(api<Wait>("WaitForSingleObject")(job.event, 0xffffffffu) == 0);
    job.done = true;
    return 42;
}

int main() {
    auto createEvent = api<CreateEvent>("CreateEventW");
    auto setEvent = api<EventOperation>("SetEvent");
    auto resetEvent = api<EventOperation>("ResetEvent");
    auto close = api<EventOperation>("CloseHandle");
    auto wait = api<Wait>("WaitForSingleObject");
    assert(!ffxvk::runtimeStandIn("user32.dll", "CreateThread"));

    void* automatic = createEvent(nullptr, 0, 1, nullptr);
    assert(automatic && wait(automatic, 0) == 0);
    assert(wait(automatic, 0) == 258);
    const auto before = std::chrono::steady_clock::now();
    assert(wait(automatic, 20) == 258);
    assert(std::chrono::steady_clock::now() - before >= std::chrono::milliseconds(15));
    std::atomic<int> released = 0;
    std::vector<std::thread> waiters;
    for (int i = 0; i != 2; ++i) waiters.emplace_back([&] { assert(wait(automatic, 1000) == 0); ++released; });
    assert(setEvent(automatic));
    while (released.load() == 0) std::this_thread::yield();
    assert(released == 1);
    assert(setEvent(automatic));
    for (auto& t : waiters) t.join();
    assert(released == 2);
    assert(close(automatic));
    assert(wait(automatic, 0) == 0xffffffffu && ffxvk::lastError() == 6);
    assert(!setEvent(automatic));
    assert(!close(automatic));

    void* manual = createEvent(nullptr, 1, 0, nullptr);
    assert(wait(manual, 0) == 258);
    assert(setEvent(manual));
    assert(wait(manual, 0) == 0 && wait(manual, 0) == 0);
    assert(resetEvent(manual));
    assert(wait(manual, 0) == 258);
    assert(!createEvent(nullptr, 0, 0, u"named") && ffxvk::lastError() == 50);

    Job job{manual};
    uint32_t tid = 0;
    void* thread = api<CreateThread>("CreateThread")(nullptr, 0, threadEntry, &job, 0, &tid);
    assert(thread && tid != 0);
    uint32_t code = 0;
    assert(api<ExitCode>("GetExitCodeThread")(thread, &code) && code == 259);
    assert(wait(thread, 0) == 258);
    assert(setEvent(manual));
    assert(wait(thread, 1000) == 0);
    assert(wait(thread, 0) == 0); // thread completion is permanently signaled
    assert(api<ExitCode>("GetExitCodeThread")(thread, &code) && code == 42);
    assert(job.ran);
    assert(close(thread));
    assert(close(manual));

    // Closing a live thread handle must leave the thread and its arguments intact.
    void* finish = createEvent(nullptr, 1, 0, nullptr);
    Job detached{finish};
    thread = api<CreateThread>("CreateThread")(nullptr, 0, threadEntry, &detached, 0, nullptr);
    assert(thread && close(thread));
    while (!detached.ran.load()) std::this_thread::yield();
    assert(setEvent(finish));
    // The entry point no longer touches its argument after publishing done.
    while (!detached.done.load()) std::this_thread::yield();
    assert(close(finish));

    alignas(8) std::array<unsigned char, 48> storage{};
    std::memset(storage.data() + 40, 0xa5, 8);
    auto initialize = api<Critical>("InitializeCriticalSection");
    auto enter = api<Critical>("EnterCriticalSection");
    auto leave = api<Critical>("LeaveCriticalSection");
    auto destroy = api<Critical>("DeleteCriticalSection");
    initialize(storage.data());
    enter(storage.data()); enter(storage.data()); leave(storage.data()); leave(storage.data());
    int counter = 0;
    std::vector<std::thread> workers;
    for (int i = 0; i != 4; ++i) workers.emplace_back([&] {
        for (int n = 0; n != 1000; ++n) {
            enter(storage.data()); ++counter; leave(storage.data());
        }
    });
    for (auto& worker : workers) worker.join();
    assert(counter == 4000);
    destroy(storage.data());
    for (size_t i = 40; i != storage.size(); ++i) assert(storage[i] == 0xa5);

    using Alloc = void* WINAPI (*)(uint32_t, size_t);
    auto alloc = api<Alloc>("LocalAlloc");
    auto* bytes = static_cast<unsigned char*>(alloc(0x40, 64));
    assert(bytes);
    for (int i = 0; i != 64; ++i) assert(bytes[i] == 0);
    std::free(bytes);
    assert(!alloc(2, 10) && ffxvk::lastError() == 50);
}
