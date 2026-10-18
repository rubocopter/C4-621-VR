// CPU only. No MinHook initialization, hook installation, or game process.
#include "render_context_trace.hpp"
#include <array>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using frame_trace::ContextInvocation;
using frame_trace::CurrentContextInvocation;
using frame_trace::RenderContextObserver;

static void Require(bool ok, const char* why) {
    if (!ok) throw std::runtime_error(why);
}

static bool Same(const ContextInvocation& a, const ContextInvocation& b) {
    return a.ordinal == b.ordinal && a.scene == b.scene && a.context == b.context &&
           a.packet == b.packet && a.vtable == b.vtable && a.slot1_target == b.slot1_target &&
           a.slot5_target == b.slot5_target && a.thread == b.thread;
}

static void* __fastcall Slot(void*, void*, void*) { return nullptr; }
struct Context {
    std::array<std::uintptr_t, 6> vtable{};
    std::array<std::uintptr_t, 1> object{};
    Context() {
        vtable[1] = reinterpret_cast<std::uintptr_t>(&Slot);
        vtable[5] = reinterpret_cast<std::uintptr_t>(&Slot);
        object[0] = reinterpret_cast<std::uintptr_t>(vtable.data());
    }
    void* Get() { return object.data(); }
};

static std::string Log(const RenderContextObserver& observer) {
    FILE* file = nullptr;
    Require(tmpfile_s(&file) == 0 && file, "Cannot open CPU log fixture");
    std::unique_ptr<FILE, decltype(&std::fclose)> close(file, &std::fclose);
    Require(observer.Write(file), "Stopped observer must write diagnostics");
    Require(std::fflush(file) == 0, "Cannot flush CPU log");
    std::rewind(file);
    std::string result;
    char buffer[4096];
    while (const auto n = std::fread(buffer, 1, sizeof(buffer), file)) result.append(buffer, n);
    Require(!std::ferror(file), "Cannot read CPU log");
    return result;
}

static std::vector<std::string> Rows(const std::string& log, const char* phase) {
    std::vector<std::string> rows;
    std::size_t begin = 0;
    while (begin < log.size()) {
        const auto end = log.find('\n', begin);
        Require(end != std::string::npos, "Truncated JSON line");
        const auto row = log.substr(begin, end - begin);
        if (row.find(std::string("\"phase\":\"") + phase + "\"") != std::string::npos)
            rows.push_back(row);
        begin = end + 1;
    }
    return rows;
}

static const char* Value(const std::string& row, const char* field) {
    const auto key = std::string("\"") + field + "\":";
    const auto pos = row.find(key);
    Require(pos != std::string::npos, "Missing JSON field");
    return row.c_str() + pos + key.size();
}
static std::uint64_t Number(const std::string& row, const char* field) {
    char* end = nullptr;
    const auto* value = Value(row, field);
    const auto number = std::strtoull(value, &end, 10);
    Require(end != value && (*end == ',' || *end == '}'), "Malformed JSON integer");
    return number;
}
static bool Boolean(const std::string& row, const char* field) {
    const auto* value = Value(row, field);
    if (std::strncmp(value, "true", 4) == 0) return true;
    Require(std::strncmp(value, "false", 5) == 0, "Malformed JSON boolean");
    return false;
}
static std::uint64_t Address(const std::string& row, const char* field) {
    const auto* value = Value(row, field);
    Require(std::strncmp(value, "\"0x", 3) == 0, "Pointer must be a hex JSON string");
    char* end = nullptr;
    const auto address = std::strtoull(value + 3, &end, 16);
    Require(end != value + 3 && *end == '"', "Malformed address");
    return address;
}
static std::string Summary(const std::string& log) {
    const auto rows = Rows(log, "render_context_summary");
    Require(rows.size() == 1, "Expected one context summary");
    return rows[0];
}

// Missing forwarding, early publication, or any scope-field mixup breaks this.
static void ExactForwardingAndCausalIdentity() {
    RenderContextObserver observer;
    Context context;
    // Scene/packet deliberately unreadable: the observer must never dereference them.
    auto* scene = reinterpret_cast<void*>(1);
    auto* packet = reinterpret_cast<void*>(2);
    auto* expected = reinterpret_cast<void*>(3);
    ContextInvocation token;
    int calls = 0;
    const auto before = [] { LARGE_INTEGER q{}; QueryPerformanceCounter(&q); return q.QuadPart; }();
    const auto result = observer.Invoke(scene, context.Get(), packet, [&](void* s, void* c, void* p) -> void* {
        ++calls;
        Require(s == scene && c == context.Get() && p == packet, "Original ABI arguments changed");
        token = CurrentContextInvocation();
        Require(token.ordinal == 1 && token.scene == 1 && token.context == reinterpret_cast<std::uintptr_t>(c) &&
                token.packet == 2 && token.vtable == context.object[0] && token.slot1_target == context.vtable[1] &&
                token.slot5_target == context.vtable[5] && token.thread == GetCurrentThreadId(), "Invocation identity incorrect");
        observer.Stop();
        const auto partial = Log(observer);
        Require(Rows(partial, "render_context_invocation").empty(), "Published before original returned");
        const auto summary = Summary(partial);
        Require(Number(summary, "incomplete_slots") == 1 && Number(summary, "in_flight") == 1 &&
                !Boolean(summary, "snapshot_complete"), "Active original not reported incomplete");
        return expected;
    });
    const auto after = [] { LARGE_INTEGER q{}; QueryPerformanceCounter(&q); return q.QuadPart; }();
    Require(result == expected && calls == 1, "Original result/call count changed");
    Require(Same(CurrentContextInvocation(), {}), "TLS scope leaked after return");
    const auto log = Log(observer);
    Require(Boolean(Summary(log),"window_started"),"Initially observing window not retained after Stop");
    const auto rows = Rows(log, "render_context_invocation");
    Require(rows.size() == 1, "Completed invocation missing");
    const auto& row = rows[0];
    Require(Number(row, "ordinal") == token.ordinal && Number(row, "thread") == token.thread &&
            Address(row, "scene") == 1 && Address(row, "context") == token.context && Address(row, "packet") == 2 &&
            Address(row, "vtable") == token.vtable && Address(row, "slot1_target") == token.slot1_target &&
            Address(row, "slot5_target") == token.slot5_target, "Published identity differs from helper token");
    Require(Number(row, "begin_qpc") >= static_cast<std::uint64_t>(before) &&
            Number(row, "end_qpc") >= Number(row, "begin_qpc") &&
            Number(row, "end_qpc") <= static_cast<std::uint64_t>(after), "QPC does not enclose original");
    Require(!Boolean(row, "frame_associated") && !Boolean(row, "gpu_binding_observed") &&
            log.find("frame_id") == std::string::npos, "CPU ordinal misrepresented as GPU/frame identity");
    const auto summary = Summary(log);
    Require(Number(summary, "samples") == 1 && Number(summary, "incomplete_slots") == 0 &&
            Number(summary, "in_flight") == 0 && Boolean(summary, "snapshot_complete"), "Finished snapshot incomplete");
    Require(Log(observer) == log, "Published record was mutable");
}

// Reopening a paused/stopped window or recording a paused call breaks this.
static void PausedOneShotAndWriterGates() {
    RenderContextObserver observer(false);
    Context context;
    FILE* file = nullptr;
    Require(tmpfile_s(&file) == 0 && file, "Cannot open gate fixture");
    std::unique_ptr<FILE, decltype(&std::fclose)> close(file, &std::fclose);
    Require(!observer.Write(file) && !observer.Write(nullptr), "Write must require Stop and FILE");
    int calls = 0;
    auto original = [&](void*, void*, void*) -> void* {
        ++calls; Require(Same(CurrentContextInvocation(), {}), "Paused/stopped call has identity"); return context.Get();
    };
    Require(observer.Invoke(nullptr, context.Get(), nullptr, original) == context.Get(), "Paused forwarding changed");
    Require(observer.BeginWindow() && !observer.BeginWindow(), "BeginWindow must be one-shot");
    observer.Invoke(nullptr, context.Get(), nullptr, [](void*, void*, void*) -> void* {
        Require(CurrentContextInvocation().ordinal == 1, "Paused call consumed ordinal"); return nullptr;
    });
    observer.Stop();
    const auto log = Log(observer);
    Require(Boolean(Summary(log),"window_started"),"Successful BeginWindow not retained after Stop");
    Require(!observer.BeginWindow(), "Stopped observer reopened");
    observer.Invoke(nullptr, context.Get(), nullptr, original);
    Require(calls == 2 && Log(observer) == log, "Stopped invocation changed retained records");
    Require(!observer.HookRestored() && !observer.CreateHook(), "Stopped synthetic observer claimed hook evidence");
    RenderContextObserver never_started(false);
    never_started.Stop();
    Require(!Boolean(Summary(Log(never_started)),"window_started"),"Never-opened window claimed admission");
    Require(!never_started.BeginWindow() && !never_started.Write(nullptr), "Stop did not close paused observer");
}

// Failure to restore nested/reentrant identity or mask rejected calls breaks this.
static void NestedRestoreAndUnrecordedMasking() {
    RenderContextObserver outer, paused(false), stopped, full;
    Context context;
    stopped.Stop();
    for (unsigned i = 0; i < 64; ++i)
        full.Invoke(nullptr, context.Get(), nullptr, [](void*, void*, void*) -> void* { return nullptr; });
    outer.Invoke(nullptr, context.Get(), nullptr, [&](void*, void*, void*) -> void* {
        const auto parent = CurrentContextInvocation();
        outer.Invoke(context.Get(), context.Get(), nullptr, [&](void*, void*, void*) -> void* {
            const auto child = CurrentContextInvocation();
            Require(child.ordinal == 2 && child.scene == reinterpret_cast<std::uintptr_t>(context.Get()) &&
                    !Same(child, parent), "Reentrant invocation inherited outer token");
            return nullptr;
        });
        Require(Same(CurrentContextInvocation(), parent), "Nested invocation did not restore parent");
        auto masked = [&](void*, void*, void*) -> void* {
            Require(Same(CurrentContextInvocation(), {}), "Unrecorded inner invocation inherited parent"); return nullptr;
        };
        paused.Invoke(nullptr, context.Get(), nullptr, masked);
        stopped.Invoke(nullptr, context.Get(), nullptr, masked);
        full.Invoke(nullptr, context.Get(), nullptr, masked);
        outer.Invoke(nullptr, reinterpret_cast<void*>(1), nullptr, masked);
        Require(Same(CurrentContextInvocation(), parent), "Rejected invocation failed to restore parent");
        return nullptr;
    });
    outer.Stop();
    const auto rows = Rows(Log(outer), "render_context_invocation");
    Require(rows.size() == 2 && Number(rows[0], "ordinal") == 1 && Number(rows[1], "ordinal") == 2,
            "Reentrant calls not retained in reservation order");
    Require(Same(CurrentContextInvocation(), {}), "Nested scopes leaked to caller");
}

// Accepting unreadable context/vtable pointers, or consuming their slots, breaks this.
static void GuardedInvalidPointersDoNotConsumeCapacity() {
    RenderContextObserver observer;
    Context context;
    std::uintptr_t bad_vtable = 1;
    std::uintptr_t null_vtable = 0;
    std::array<std::uintptr_t, 6> null_slots{};
    auto empty_object = reinterpret_cast<std::uintptr_t>(null_slots.data());
    void* invalid[] = {nullptr, reinterpret_cast<void*>(1), &bad_vtable, &null_vtable, &empty_object};
    unsigned forwarded = 0;
    for (auto* c : invalid) observer.Invoke(nullptr, c, nullptr, [&](void*, void* actual, void*) -> void* {
        ++forwarded;
        Require(actual == c && Same(CurrentContextInvocation(), {}), "Invalid context not masked/forwarded");
        return c;
    });
    for (unsigned i = 0; i < 64; ++i) observer.Invoke(nullptr, context.Get(), nullptr, [&](void*, void*, void*) -> void* {
        Require(CurrentContextInvocation().ordinal == i + 6, "Invalid calls did not leave ordinal gaps"); return nullptr;
    });
    observer.Stop();
    const auto summary = Summary(Log(observer));
    Require(forwarded == 5 && Number(summary, "samples") == 64 && Number(summary, "invalid") == 5 &&
            Number(summary, "calls") == 69, "Invalid reads consumed record capacity");
}

// Publishing thrown calls, swallowing exceptions, or leaking TLS breaks this.
static void ExceptionsRestoreAndRejectPartialRecords() {
    struct Sentinel { int value; };
    RenderContextObserver observer;
    Context context;
    unsigned calls = 0;
    try {
        observer.Invoke(nullptr, context.Get(), nullptr, [&](void*, void*, void*) -> void* {
            ++calls;
            const auto parent = CurrentContextInvocation();
            try {
                observer.Invoke(nullptr, context.Get(), nullptr, [](void*, void*, void*) -> void* { throw Sentinel{42}; });
            } catch (const Sentinel& e) {
                Require(e.value == 42 && Same(CurrentContextInvocation(), parent), "Nested throw did not restore parent");
            }
            throw Sentinel{73};
        });
        Require(false, "Original exception swallowed");
    } catch (const Sentinel& e) { Require(e.value == 73, "Original exception changed"); }
    Require(calls == 1 && Same(CurrentContextInvocation(), {}), "Throw leaked TLS or repeated original");
    observer.Invoke(nullptr, context.Get(), nullptr, [](void*, void*, void*) -> void* {
        Require(CurrentContextInvocation().ordinal == 3, "Thrown slots/ordinals reused"); return nullptr;
    });
    observer.Stop();
    const auto log = Log(observer);
    Require(Rows(log, "render_context_invocation").size() == 1, "Exception published partial payload");
    const auto summary = Summary(log);
    Require(Number(summary, "samples") == 1 && Number(summary, "incomplete_slots") == 2 &&
            Number(summary, "in_flight") == 0 && !Boolean(summary, "snapshot_complete"),
            "Unpublished exception slots falsely complete");
}

// Global TLS, torn publication, or missing cutoff at slot 64 breaks this.
static void ConcurrentThreadsAndFiniteCapacity() {
    RenderContextObserver observer;
    Context context;
    std::atomic<unsigned> entered{0};
    std::atomic<bool> release{false}, failed{false};
    std::vector<std::thread> threads;
    for (unsigned t = 0; t < 8; ++t) threads.emplace_back([&, t] {
        auto* scene = reinterpret_cast<void*>(std::uintptr_t(100 + t));
        observer.Invoke(scene, context.Get(), nullptr, [&](void*, void*, void*) -> void* {
            const auto token = CurrentContextInvocation();
            entered.fetch_add(1);
            while (!release.load(std::memory_order_acquire)) std::this_thread::yield();
            if (!Same(CurrentContextInvocation(), token) || token.scene != 100 + t || !token.ordinal ||
                token.thread != GetCurrentThreadId()) failed.store(true);
            return nullptr;
        });
        if (!Same(CurrentContextInvocation(), {})) failed.store(true);
    });
    while (entered.load() != 8) std::this_thread::yield();
    Require(Same(CurrentContextInvocation(), {}), "Worker scope leaked into main thread");
    release.store(true, std::memory_order_release);
    for (auto& thread : threads) thread.join();
    Require(!failed.load(), "Thread scopes interfered");
    for (unsigned i = 8; i < 64; ++i)
        observer.Invoke(nullptr, context.Get(), nullptr, [](void*, void*, void*) -> void* { return nullptr; });
    unsigned forwarded = 0;
    for (unsigned i = 0; i < 10; ++i)
        observer.Invoke(nullptr, context.Get(), nullptr, [&](void*, void*, void*) -> void* {
            ++forwarded; Require(Same(CurrentContextInvocation(), {}), "Full observer published scope"); return nullptr;
        });
    observer.Stop();
    const auto log = Log(observer);
    const auto summary = Summary(log);
    const auto rows = Rows(log, "render_context_invocation");
    Require(forwarded == 10 && rows.size() == 64 && Number(summary, "calls") == 64 &&
            Number(summary, "overflow") == 0 && Boolean(summary, "sample_limit_reached") &&
            Boolean(summary, "snapshot_complete") && !observer.BeginWindow(), "Capacity did not close admission");
    std::array<bool, 65> seen{};
    for (const auto& row : rows) {
        const auto ordinal = Number(row, "ordinal");
        Require(ordinal > 0 && ordinal <= 64 && !seen[ordinal], "Concurrent ordinal duplicated");
        seen[ordinal] = true;
        Require(Address(row, "context") == reinterpret_cast<std::uintptr_t>(context.Get()) &&
                Address(row, "vtable") == context.object[0] && Number(row, "end_qpc") >= Number(row, "begin_qpc"),
                "Torn concurrent identity/timing record");
    }
}

// The original cannot finish until AFTER the first Write. A blocking writer
// would deadlock; reading reserved payload would invent a completed context.
static void DelayedOriginalIsIncompleteUntilPublished() {
    RenderContextObserver observer;
    Context context;
    std::atomic<bool> entered{false}, release{false};
    std::thread publisher([&] {
        observer.Invoke(nullptr, context.Get(), nullptr, [&](void*,void*,void*)->void* {
            entered.store(true,std::memory_order_release);
            while(!release.load(std::memory_order_acquire)) std::this_thread::yield();
            return nullptr;
        });
    });
    {
        struct Join { std::atomic<bool>& release; std::thread& publisher;
            ~Join() { release.store(true,std::memory_order_release);publisher.join(); }
        } join{release,publisher};
        const auto deadline=GetTickCount64()+5000;
        while(!entered.load(std::memory_order_acquire)&&GetTickCount64()<deadline) std::this_thread::yield();
        Require(entered.load(),"Delayed original did not enter");
        observer.Stop();
        const auto partial=Log(observer);
        Require(Rows(partial,"render_context_invocation").empty(),"Reserved context payload published early");
        const auto summary=Summary(partial);
        Require(Number(summary,"in_flight")==1&&Number(summary,"incomplete_slots")==1&&
                !Boolean(summary,"snapshot_complete"),"In-flight invocation falsely complete");
    }
    const auto complete=Log(observer);
    const auto summary=Summary(complete);
    Require(Rows(complete,"render_context_invocation").size()==1&&Boolean(summary,"snapshot_complete")&&
            Number(summary,"incomplete_slots")==0&&Number(summary,"in_flight")==0,
            "Late immutable invocation was not published");
}

int main() {
    unsigned failures = 0;
    const std::pair<const char*, void(*)()> tests[] = {
        {"exact forwarding and causal identity", ExactForwardingAndCausalIdentity},
        {"paused one-shot and writer gates", PausedOneShotAndWriterGates},
        {"nested restore and unrecorded masking", NestedRestoreAndUnrecordedMasking},
        {"guarded invalid pointers", GuardedInvalidPointersDoNotConsumeCapacity},
        {"exception partial-record rejection", ExceptionsRestoreAndRejectPartialRecords},
        {"concurrent thread isolation and finite capacity", ConcurrentThreadsAndFiniteCapacity},
        {"delayed original and nonblocking partial snapshot", DelayedOriginalIsIncompleteUntilPublished},
    };
    for (const auto& test : tests) {
        try { test.second(); std::cout << "PASS: " << test.first << '\n'; }
        catch (const std::exception& e) { ++failures; std::cerr << "FAIL: " << test.first << ": " << e.what() << '\n'; }
    }
    return failures ? 1 : 0;
}
