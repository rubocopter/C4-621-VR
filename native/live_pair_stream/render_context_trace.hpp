#pragma once
#include <windows.h>
#include <MinHook.h>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <utility>
#include "context_forwarding_trace.hpp"

namespace frame_trace {
struct ContextInvocation {
    std::uint64_t ordinal = 0;
    std::uintptr_t scene = 0, context = 0, packet = 0, vtable = 0, slot1_target = 0, slot5_target = 0;
    std::uint32_t thread = 0;
};
namespace render_context_detail {
inline thread_local ContextInvocation current{};
}
// A CPU invocation token only: never a frame ID or evidence of GPU binding.
inline ContextInvocation CurrentContextInvocation() noexcept { return render_context_detail::current; }

// Optional CPU diagnostic; inclusion/construction installs nothing. Single
// control-thread lifecycle, as for ProjectionObserver: retain this object AND
// its DLL/trampoline for process lifetime; parent owns MH_Initialize, enable,
// disable. CreateHook creates a DISABLED hook only. Stop before Write; disable
// before checking HookRestored. Never reset/reuse, remove hooks, uninitialize
// MinHook, or unload. A callback delayed before Detour still needs the original.
// Parent verifies the full executable hash before CreateHook.
//
// Known Win64 ABI: RCX=scene, RDX=context interface, R8=input packet. The routine
// builds local matrices, reads vtable[5], and calls vtable[1](context,localPacket,0).
// Raw full .pdata establishes [0x26e17f0,0x26e1da4); a partial parsed .pdata list
// is insufficient. The further opt-in forwarding reader follows only the pinned
// adapter layout at entry; never matrices, scene/packet contents or target code.
// Targets are candidates sampled at invocation entry; this does not observe
// an actual indirect dispatch or prove that the vtable stays unchanged.
class RenderContextObserver {
    enum class Window : unsigned char { Paused, Observing, Stopped };
public:
    static constexpr std::size_t Capacity = 64;
    explicit RenderContextObserver(bool initially_observing = true,ContextForwardingLayout forwarding={}) noexcept
        : window_(initially_observing ? Window::Observing : Window::Paused),forwarding_(forwarding) {}
    RenderContextObserver(const RenderContextObserver&) = delete;
    RenderContextObserver& operator=(const RenderContextObserver&) = delete;

    bool CreateHook() noexcept {
#if defined(_WIN64) && (defined(_M_X64) || defined(__x86_64__))
        if (window_.load() == Window::Stopped || instance_.load(std::memory_order_acquire)) return false;
        const auto game = GetModuleHandleW(L"armoredcore6.exe");
        if (!game || game != GetModuleHandleW(nullptr) || !GetModuleHandleW(L"ac6_coop.dll") ||
            GetModuleHandleW(L"EasyAntiCheat_EOS.dll") || GetModuleHandleW(L"EasyAntiCheat.dll")) return false;
        auto* target = reinterpret_cast<void*>(reinterpret_cast<std::uintptr_t>(game) + 0x26e17f0);
        if (!MatchesPrologue(target)) return false;
#ifdef AC6_CONTEXT_FORWARD_TRACE
        if(!LoadGameForwardingLayout(game,forwarding_)) return false;
#endif
        if (MH_CreateHook(target, reinterpret_cast<void*>(&Detour),
                          reinterpret_cast<void**>(&original_)) != MH_OK) return false;
        target_ = target;
        instance_.store(this, std::memory_order_release);
        return true;
#else
        return false;
#endif
    }

    bool BeginWindow() noexcept {
        auto expected = Window::Paused;
        return window_.compare_exchange_strong(expected, Window::Observing);
    }
    void Stop() noexcept { window_.store(Window::Stopped); }
    bool HookRestored() const noexcept { return target_ && MatchesPrologue(target_); }

    // Same callback path for synthetic CPU inputs and Detour. No allocation,
    // waits, file I/O, matrix reads or calls to virtual targets. Call the supplied
    // original exactly once, preserving its return and exception. Mask first,
    // even if paused/stopped/full/invalid; an unrecorded nested invocation must
    // never inherit the outer identity. Scope and flight restore on unwinding.
    template<class Original>
    void* Invoke(void* scene, void* context, void* packet, Original&& original) {
        Flight flight(*this);
        Scope scope;
        ContextInvocation token;
        std::size_t index = Capacity;
        if (window_.load() == Window::Observing) {
            token.ordinal = calls_.fetch_add(1, std::memory_order_relaxed) + 1;
            token.scene = reinterpret_cast<std::uintptr_t>(scene);
            token.context = reinterpret_cast<std::uintptr_t>(context);
            token.packet = reinterpret_cast<std::uintptr_t>(packet);
            if (ReadTargets(context, token)) {
                index = Reserve();
                if (index < Capacity) {
                    token.thread = GetCurrentThreadId();
                    render_context_detail::current = token;
                }
            } else invalid_.fetch_add(1, std::memory_order_relaxed);
        }
        LARGE_INTEGER begin{}, end{};
        if (index < Capacity) QueryPerformanceCounter(&begin);
        const auto forwarding=index<Capacity?ReadContextForwarding(token,forwarding_):ContextForwardingSnapshot{};
        void* result;
        try {
            result = std::forward<Original>(original)(scene, context, packet);
        } catch (...) {
            if (index < Capacity) QueryPerformanceCounter(&end);
            // Leave reserved slot unready, permanently. No partial record can
            // masquerade as a completed invocation after exception unwinding.
            throw;
        }
        if (index < Capacity) {
            QueryPerformanceCounter(&end);
            Publish(index, token, begin.QuadPart, end.QuadPart,forwarding);
        }
        return result;
    }

    // Nonblocking partial snapshot after Stop. Each slot is inspected once with
    // acquire; only immutable ready payloads are read. Late publishers may appear
    // in later Writes. Counters are diagnostic, not a global concurrent instant.
    // Caller serializes/owns FILE*, flush/close. Integers need no locale changes.
    bool Write(FILE* file) const {
        if (!file || window_.load() != Window::Stopped) return false;
        const auto active = in_flight_.load();
        const auto calls = calls_.load(std::memory_order_relaxed);
        const auto invalid = invalid_.load(std::memory_order_relaxed);
        const auto reserved = reserved_.load();
        const auto limit = reserved < Capacity ? static_cast<std::size_t>(reserved) : Capacity;
        std::array<const Entry*, Capacity> selected{};
        std::size_t saved = 0;
        for (std::size_t i = 0; i < limit; ++i)
            if (entries_[i].ready.load(std::memory_order_acquire)) selected[saved++] = &entries_[i];
        auto print = [&](const char* format, auto... args) { return std::fprintf(file, format, args...) >= 0; };
        for (std::size_t i = 0; i < saved; ++i) {
            const auto& e = *selected[i];
            const auto& c = e.identity;
            if (!print("{\"phase\":\"render_context_invocation\",\"ordinal\":%llu,"
                       "\"scene\":\"0x%llx\",\"context\":\"0x%llx\",\"packet\":\"0x%llx\","
                       "\"vtable\":\"0x%llx\",\"slot1_target\":\"0x%llx\",\"slot5_target\":\"0x%llx\","
                       "\"thread\":%u,\"begin_qpc\":%lld,\"end_qpc\":%lld,"
                       "\"targets_sampled_at\":\"invocation_entry\",\"virtual_dispatch_observed\":false,"
                       "\"frame_associated\":false,\"gpu_binding_observed\":false",
                       static_cast<unsigned long long>(c.ordinal), static_cast<unsigned long long>(c.scene),
                       static_cast<unsigned long long>(c.context), static_cast<unsigned long long>(c.packet),
                       static_cast<unsigned long long>(c.vtable), static_cast<unsigned long long>(c.slot1_target),
                       static_cast<unsigned long long>(c.slot5_target), unsigned(c.thread),
                       static_cast<long long>(e.begin_qpc), static_cast<long long>(e.end_qpc))) return false;
            if(forwarding_.Enabled()&&!WriteContextForwarding(file,e.forwarding)) return false;
            if(!print("}\n")) return false;
        }
        LARGE_INTEGER frequency{};
        QueryPerformanceFrequency(&frequency);
        return print("{\"phase\":\"render_context_summary\",\"target_rva\":\"0x26e17f0\","
                     "\"target_end_rva\":\"0x26e1da4\",\"qpc_frequency\":%lld,\"capacity\":%u,"
                     "\"calls\":%llu,\"invalid\":%llu,\"reserved\":%llu,\"overflow\":%llu,\"samples\":%u,"
                     "\"sample_limit_reached\":%s,\"incomplete_slots\":%u,\"in_flight\":%llu,\"snapshot_complete\":%s,"
                     "\"targets_sampled_at\":\"invocation_entry\",\"virtual_dispatch_observed\":false,"
                     "\"frame_associated\":false,\"gpu_binding_observed\":false,\"storage_retained\":true%s}\n",
                     static_cast<long long>(frequency.QuadPart), unsigned(Capacity),
                     static_cast<unsigned long long>(calls), static_cast<unsigned long long>(invalid),
                     static_cast<unsigned long long>(reserved),
                     static_cast<unsigned long long>(reserved > Capacity ? reserved - Capacity : 0), unsigned(saved),
                     reserved >= Capacity ? "true" : "false", unsigned(limit - saved),
                     static_cast<unsigned long long>(active), active == 0 && saved == limit ? "true" : "false",
                     forwarding_.Enabled()?",\"forwarding_enabled\":true":"") &&
               !std::ferror(file);
    }

private:
    friend struct RenderContextObserverTestPeer; // controlled CPU publishers, defined only in test
    using OriginalFn = void*(__fastcall*)(void*, void*, void*);
    struct Entry {
        ContextInvocation identity{};
        ContextForwardingSnapshot forwarding{};
        std::int64_t begin_qpc = 0, end_qpc = 0;
        std::atomic<bool> ready{false};
    };
    struct Flight {
        RenderContextObserver& owner;
        explicit Flight(RenderContextObserver& value) noexcept : owner(value) { owner.in_flight_.fetch_add(1); }
        ~Flight() { owner.in_flight_.fetch_sub(1); }
    };
    struct Scope {
        ContextInvocation previous = render_context_detail::current;
        Scope() noexcept { render_context_detail::current = {}; }
        ~Scope() { render_context_detail::current = previous; }
    };
    static_assert(std::atomic<bool>::is_always_lock_free && std::atomic<Window>::is_always_lock_free &&
                  std::atomic<std::uint64_t>::is_always_lock_free &&
                  std::atomic<RenderContextObserver*>::is_always_lock_free, "Callback requires lock-free atomics");
    std::array<Entry, Capacity> entries_{};
    std::atomic<Window> window_;
    ContextForwardingLayout forwarding_{};
    std::atomic<std::uint64_t> calls_{0}, invalid_{0}, reserved_{0}, in_flight_{0};
    OriginalFn original_ = nullptr;
    void* target_ = nullptr;
    inline static std::atomic<RenderContextObserver*> instance_{nullptr};

    // Admission was decided before validation. Already-admitted calls can reserve
    // after Stop; their excess is counted explicitly. New calls remain quiet.
    std::size_t Reserve() noexcept {
        const auto index = reserved_.fetch_add(1);
        if (index >= Capacity - 1) Stop();
        return index < Capacity ? static_cast<std::size_t>(index) : Capacity;
    }
    void Publish(std::size_t index, const ContextInvocation& token, std::int64_t begin, std::int64_t end,
                 const ContextForwardingSnapshot& forwarding) noexcept {
        auto& entry = entries_[index];
        entry.identity = token;
        entry.begin_qpc = begin;
        entry.end_qpc = end;
        entry.forwarding=forwarding;
        // Sole publication; payload is never modified again.
        entry.ready.store(true, std::memory_order_release);
    }

    // Trivial SEH-only reader: no objects requiring C++ unwinding in __try.
    // Stage into locals so a failed slot read never yields a partial identity.
    static bool ReadTargets(const void* context, ContextInvocation& token) noexcept {
        __try {
            if (!context) return false;
            const auto table = *static_cast<const std::uintptr_t*>(context);
            if (!table) return false;
            const auto* slots = reinterpret_cast<const std::uintptr_t*>(table);
            const auto slot1 = slots[1];
            const auto slot5 = slots[5];
            if (!slot1 || !slot5) return false;
            token.vtable = table;
            token.slot1_target = slot1;
            token.slot5_target = slot5;
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    static bool MatchesPrologue(const void* target) noexcept {
        constexpr unsigned char expected[16] = {
            0x40,0x55,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57,0x48,0x8d,0xac,0x24
        };
        __try { return std::memcmp(target, expected, sizeof(expected)) == 0; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    static void* __fastcall Detour(void* scene, void* context, void* packet) {
        auto& self = *instance_.load(std::memory_order_acquire);
        return self.Invoke(scene, context, packet, self.original_);
    }
};
}
