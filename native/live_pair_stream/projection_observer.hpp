#pragma once
#include "../projection_probe/snapshot.hpp"
#include <MinHook.h>
#include <array>
#include <atomic>
#include <cstdio>
#include <locale.h>

namespace frame_trace {

// Optional diagnostic only. Including this header installs nothing. The parent
// must opt in through its default-OFF build option; there is no transport/API
// that associates these helper results with a submitted GPU frame.
//
// Lifecycle (single control thread): retain this object AND its DLL/trampoline
// for process lifetime; MH_Initialize(); CreateHook(); parent enables ALL hooks.
// On shutdown: Stop(); parent disables hooks; Write(log). Stop/Write never wait
// for callbacks. Do not reset/reuse the object, remove its hook, uninitialize
// MinHook, or unload the DLL. Even a callback paused before entering Detour must
// still be able to call the retained original after disable.
class ProjectionObserver {
    enum class Window : unsigned char { Paused, Observing, Stopped };
public:
    static constexpr std::size_t Capacity = 256;
    static constexpr std::size_t StackDepth = 8;
    explicit ProjectionObserver(bool initially_observing = true) noexcept
        : window_(initially_observing ? Window::Observing : Window::Paused) {}
    ProjectionObserver(const ProjectionObserver&) = delete;
    ProjectionObserver& operator=(const ProjectionObserver&) = delete;

    // The parent owns MH_Initialize/Enable/Disable. This creates a DISABLED hook
    // only, and accepts exactly the historical helper fingerprint/ABI.
    bool CreateHook() noexcept {
        if (window_.load() == Window::Stopped || instance_.load(std::memory_order_acquire)) return false;
        const auto game = GetModuleHandleW(L"armoredcore6.exe");
        if (!game || game != GetModuleHandleW(nullptr) || !GetModuleHandleW(L"ac6_coop.dll") ||
            GetModuleHandleW(L"EasyAntiCheat_EOS.dll") || GetModuleHandleW(L"EasyAntiCheat.dll")) return false;
        auto* target = reinterpret_cast<void*>(reinterpret_cast<std::uintptr_t>(game) + 0x23a7790);
        if (!MatchesPrologue(target)) return false;
        if (MH_CreateHook(target, reinterpret_cast<void*>(&Detour),
                          reinterpret_cast<void**>(&original_)) != MH_OK) return false;
        target_ = target;
        // Publish original_ and the retained object before the parent enables.
        instance_.store(this, std::memory_order_release);
        return true;
    }

    // One transition from paused to observing, without clearing any storage.
    // For a producer use ProjectionObserver(false), create/enable while paused,
    // then BeginWindow() only when the stream becomes Armed.
    bool BeginWindow() noexcept {
        auto expected = Window::Paused;
        return window_.compare_exchange_strong(expected, Window::Observing);
    }

    // Check only after the parent's hook disable, on the control thread.
    // No created hook means there is no restoration evidence to report.
    bool HookRestored() const noexcept { return target_ && MatchesPrologue(target_); }

    // Synthetic CPU input uses the same admission, validation, stack capture,
    // capacity and publication path as Detour. True means retained; false means
    // stopped, invalid, or full. Constructed objects accept synthetic samples;
    // they never install a hook automatically. Invalid/overflow calls still
    // consume an ordinal, but invalid snapshots never consume a storage slot.
    bool Record(const ProjectionSnapshot& sample, std::uintptr_t outputId,
                unsigned char jitter, std::int64_t beginQpc, std::int64_t endQpc,
                std::uint32_t thread) noexcept {
        Flight flight(*this);
        if (window_.load() != Window::Observing) return false;
        const auto ordinal = calls_.fetch_add(1, std::memory_order_relaxed) + 1;
        return Store(sample, outputId, jitter, beginQpc, endQpc, thread, ordinal);
    }

    // Admission closes immediately. Already admitted callbacks may finish later.
    void Stop() noexcept { window_.store(Window::Stopped); }

    // Call after Stop and the parent's successful hook disable. A single acquire
    // load per slot chooses immutable, ready entries; unfinished slots are
    // skipped, never read or waited on. This is a partial diagnostic snapshot,
    // NOT a global instant across concurrent writers. Counters can be partial
    // when snapshot_complete is false. A later Write may include late publishers.
    // The caller owns/serializes FILE*, flush, close, and all output I/O.
    bool Write(FILE* file) const {
        if (!file || window_.load() != Window::Stopped) return false;
        const auto active = in_flight_.load();
        const auto calls = calls_.load(std::memory_order_relaxed);
        const auto invalid = invalid_.load(std::memory_order_relaxed);
        const auto valid = valid_.load(std::memory_order_relaxed);
        const auto limit = valid < Capacity ? static_cast<std::size_t>(valid) : Capacity;
        std::array<const Entry*, Capacity> selected{};
        std::size_t saved = 0;
        for (std::size_t i = 0; i < limit; ++i)
            if (entries_[i].ready.load(std::memory_order_acquire)) selected[saved++] = &entries_[i];

        // Locale allocation is confined to this control-thread writer. Keep
        // float JSON valid even if the process uses comma decimal separators.
        struct NumericLocale {
            _locale_t value = _create_locale(LC_NUMERIC, "C");
            ~NumericLocale() { if (value) _free_locale(value); }
        } locale;
        if (!locale.value) return false;
        auto print = [&](const char* format, auto... args) {
            return _fprintf_l(file, format, locale.value, args...) >= 0;
        };
        for (std::size_t i = 0; i < saved; ++i) {
            const auto& e = *selected[i];
            const auto& s = e.sample;
            if (!print("{\"phase\":\"projection_sample\",\"frame_associated\":false,"
                       "\"helper_output_only\":true,\"gpu_use_proven\":false,"
                       "\"call_ordinal\":%llu,\"begin_qpc\":%lld,\"end_qpc\":%lld,\"thread\":%u,"
                       "\"view\":\"0x%llx\",\"output\":\"0x%llx\",\"jitter_flag\":%u,\"jitter_enabled\":%s,"
                       "\"fov_y\":%.9g,\"near_plane\":%.9g,\"far_plane\":%.9g,\"aspect\":%.9g,"
                       "\"width\":%u,\"height\":%u,\"projection_mode\":%u,"
                       "\"jitter_x\":%.9g,\"jitter_y\":%.9g,\"shift_x\":%.9g,\"shift_y\":%.9g,\"matrix\":[",
                       static_cast<unsigned long long>(e.ordinal),
                       static_cast<long long>(e.begin_qpc), static_cast<long long>(e.end_qpc), e.thread,
                       static_cast<unsigned long long>(s.view), static_cast<unsigned long long>(e.output),
                       unsigned(e.jitter), e.jitter ? "true" : "false", s.fov, s.near_plane, s.far_plane,
                       s.aspect, s.width, s.height, s.projection_mode, s.jitter_x, s.jitter_y, s.shift_x, s.shift_y)) return false;
            for (unsigned j = 0; j < 16; ++j)
                if (!print(j ? ",%.9g" : "%.9g", s.matrix[j])) return false;
            if (!print("],\"stack_count\":%u,\"stack\":[", unsigned(e.stack_count))) return false;
            for (unsigned j = 0; j < e.stack_count; ++j)
                if (!print(j ? ",\"0x%llx\"" : "\"0x%llx\"", static_cast<unsigned long long>(e.stack[j]))) return false;
            if (!print("]}\n")) return false;
        }
        LARGE_INTEGER frequency{};
        QueryPerformanceFrequency(&frequency);
        return print("{\"phase\":\"projection_summary\",\"frame_associated\":false,"
                     "\"helper_output_only\":true,\"gpu_use_proven\":false,\"target_rva\":\"0x23a7790\","
                     "\"qpc_frequency\":%lld,\"capacity\":%u,\"calls\":%llu,\"invalid\":%llu,\"valid\":%llu,"
                     "\"overflow\":%llu,\"samples\":%u,\"sample_limit_reached\":%s,\"incomplete_slots\":%u,\"in_flight\":%llu,"
                     "\"snapshot_complete\":%s,\"writes_camera_or_matrix\":false,\"storage_retained\":true}\n",
                     frequency.QuadPart, unsigned(Capacity), static_cast<unsigned long long>(calls),
                     static_cast<unsigned long long>(invalid), static_cast<unsigned long long>(valid),
                     static_cast<unsigned long long>(valid > Capacity ? valid - Capacity : 0), unsigned(saved), valid >= Capacity ? "true" : "false",
                     unsigned(limit - saved), static_cast<unsigned long long>(active), active ? "false" : "true") &&
               !std::ferror(file);
    }

private:
    friend struct ProjectionObserverTestPeer; // deterministic suspended-publisher fixture, defined only in the CPU test
    using BuildProjection = void*(__fastcall*)(void*, void*, unsigned char);
    struct Entry {
        ProjectionSnapshot sample{};
        std::uintptr_t output = 0;
        std::uint64_t ordinal = 0;
        std::int64_t begin_qpc = 0, end_qpc = 0;
        std::uint32_t thread = 0;
        unsigned char jitter = 0;
        std::array<std::uintptr_t, StackDepth> stack{};
        USHORT stack_count = 0;
        std::atomic<bool> ready{false};
    };
    struct Flight {
        ProjectionObserver& owner;
        explicit Flight(ProjectionObserver& observer) noexcept : owner(observer) { owner.in_flight_.fetch_add(1); }
        ~Flight() { owner.in_flight_.fetch_sub(1); }
    };
    static_assert(std::atomic<bool>::is_always_lock_free && std::atomic<Window>::is_always_lock_free &&
                  std::atomic<std::uint64_t>::is_always_lock_free &&
                  std::atomic<ProjectionObserver*>::is_always_lock_free,
                  "The observation path requires lock-free atomics");
    std::array<Entry, Capacity> entries_{};
    std::atomic<Window> window_;
    std::atomic<std::uint64_t> calls_{0}, invalid_{0}, valid_{0}, in_flight_{0};
    BuildProjection original_ = nullptr;
    void* target_ = nullptr;
    inline static std::atomic<ProjectionObserver*> instance_{nullptr};

    static bool MatchesPrologue(const void* target) noexcept {
        constexpr unsigned char expected[16] = {
            0x48,0x8b,0xc4,0x48,0x89,0x58,0x08,0x48,0x89,0x70,0x10,0x48,0x89,0x78,0x18,0x55
        };
        __try { return std::memcmp(target, expected, sizeof(expected)) == 0; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    // Same supported-field bounds as the unchanged snapshot reader. No historic
    // FOV/near/far matching, no throttle, no GPU matrix-shape/calibration filter.
    // The full finite matrix is diagnostic evidence, not a GPU-validity claim.
    static bool Supported(const ProjectionSnapshot& s) noexcept {
        if (s.projection_mode != 0 || s.width < 128 || s.height < 128 || s.width > 16384 || s.height > 16384 ||
            !std::isfinite(s.fov) || s.fov <= 0 || s.fov >= 3.141593f ||
            !std::isfinite(s.near_plane) || s.near_plane <= 0 ||
            !std::isfinite(s.far_plane) || s.far_plane <= s.near_plane ||
            !std::isfinite(s.aspect) || s.aspect < 0 || s.aspect > 16 ||
            !std::isfinite(s.jitter_x) || !std::isfinite(s.jitter_y) ||
            !std::isfinite(s.shift_x) || !std::isfinite(s.shift_y)) return false;
        for (float value : s.matrix) if (!std::isfinite(value)) return false;
        return true;
    }

    bool Store(const ProjectionSnapshot& sample, std::uintptr_t output, unsigned char jitter,
               std::int64_t begin, std::int64_t end, std::uint32_t thread, std::uint64_t ordinal) noexcept {
        if (!sample.view || !output || !Supported(sample)) {
            invalid_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        const auto index = valid_.fetch_add(1, std::memory_order_relaxed);
        // Close admission as soon as the last slot is reserved. Only calls
        // already admitted can overflow; later hooks quietly call the original.
        if (index >= Capacity - 1) Stop();
        if (index >= Capacity) return false;
        auto& e = entries_[static_cast<std::size_t>(index)];
        e.sample = sample; e.output = output; e.jitter = jitter; e.ordinal = ordinal;
        e.begin_qpc = begin; e.end_qpc = end; e.thread = thread;
        void* stack[StackDepth]{};
        e.stack_count = CaptureStackBackTrace(1, static_cast<DWORD>(StackDepth), stack, nullptr);
        for (unsigned i = 0; i < e.stack_count; ++i) e.stack[i] = reinterpret_cast<std::uintptr_t>(stack[i]);
        // Never write this entry again. This is the only payload publication.
        e.ready.store(true, std::memory_order_release);
        return true;
    }

    static void* __fastcall Detour(void* view, void* output, unsigned char jitter) {
        auto& self = *instance_.load(std::memory_order_acquire);
        Flight flight(self); // covers the original AND post-call sampling
        const bool observe = self.window_.load() == Window::Observing;
        std::uint64_t ordinal = 0;
        LARGE_INTEGER begin{}, end{};
        std::uint32_t thread = 0;
        if (observe) {
            ordinal = self.calls_.fetch_add(1, std::memory_order_relaxed) + 1;
            thread = GetCurrentThreadId();
            QueryPerformanceCounter(&begin);
        }
        void* result = self.original_(view, output, jitter);
        if (observe) {
            QueryPerformanceCounter(&end);
            ProjectionSnapshot sample;
            if (ReadProjectionSnapshot(view, output, sample))
                self.Store(sample, reinterpret_cast<std::uintptr_t>(output), jitter,
                           begin.QuadPart, end.QuadPart, thread, ordinal);
            else self.invalid_.fetch_add(1, std::memory_order_relaxed);
        }
        return result; // preserve the original return, even if different from output
    }
};
}
