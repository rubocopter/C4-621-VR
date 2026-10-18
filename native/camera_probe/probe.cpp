// Bounded camera-pipeline experiment. OpenXR runs in an external producer.
// Fixed yaw: observe 2s, yaw 10 degrees for 3s, observe 3s.
// Head tracking: observe 2s, consume published rotation for 15s, observe 3s.
// After automatic disable the DLL/trampoline remain allocated to avoid unsafe
// unloading while another thread might still be returning through the detour.
#include <windows.h>
#include <MinHook.h>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <share.h>
#include <cstddef>
#include <string>
#include "yaw.hpp"

using CopyCamera = void(__fastcall*)(void*, void*);
static CopyCamera original_copy = nullptr;
static HMODULE probe_module = nullptr;
static std::uintptr_t game_base = 0;
static std::atomic<bool> apply_offset{false};
static std::atomic<unsigned long long> calls{0}, matched{0}, applications{0}, exceptions{0};
static std::atomic<std::uintptr_t> destination{0};
static std::atomic<DWORD> last_thread{0};

#ifdef AC6_HEADTRACKING_PROBE
struct PosePacket {
    std::uint32_t magic, version, valid, sequence;
    std::uint64_t tick;
    float delta[9];
};
static_assert(offsetof(PosePacket, delta) == 24);
static HANDLE pose_mutex = nullptr;
static const PosePacket* shared_pose = nullptr;

static bool ReadPublishedDelta(float* output) noexcept {
    static thread_local float cache[9] = {1,0,0,0,1,0,0,0,1};
    static thread_local ULONGLONG last_tick = 0;
    const auto now = GetTickCount64();
    const DWORD waited = WaitForSingleObject(pose_mutex, 0);
    if (waited == WAIT_OBJECT_0 || waited == WAIT_ABANDONED) {
        PosePacket packet;
        std::memcpy(&packet, shared_pose, sizeof(packet));
        ReleaseMutex(pose_mutex);
        if (packet.magic != 0x36565241 || packet.version != 1 || packet.valid != 1 ||
            packet.tick > now || now - packet.tick > 250) {
            last_tick = 0;
            return false;
        }
        for (float value : packet.delta) if (!std::isfinite(value)) return false;
        for (int i = 0; i < 3; ++i) {
            for (int j = 0; j < 3; ++j) {
                float dot = 0;
                for (int k = 0; k < 3; ++k) dot += packet.delta[i*3+k] * packet.delta[j*3+k];
                if (std::abs(dot - (i == j ? 1.f : 0.f)) > 0.001f) return false;
            }
        }
        std::memcpy(cache, packet.delta, sizeof(cache));
        last_tick = packet.tick;
    } else if (waited != WAIT_TIMEOUT) {
        return false;
    }
    if (!last_tick || now - last_tick > 250) return false;
    std::memcpy(output, cache, sizeof(cache));
    return true;
}
#endif

static bool WaitOrCancel(DWORD milliseconds) {
    const auto until = GetTickCount64() + milliseconds;
    while (GetTickCount64() < until) {
        if (GetAsyncKeyState(VK_F12) & 0x8000) {
            apply_offset.store(false);
            return false;
        }
        Sleep(10);
    }
    return true;
}

static void* SourceCamera() noexcept {
    __try {
        const auto manager = *reinterpret_cast<std::uintptr_t*>(game_base + 0x4e09e80);
        return manager ? *reinterpret_cast<void**>(manager + 8) : nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

static bool ApplyOffset(void* camera) noexcept {
    __try {
        float matrix[16];
        auto address = static_cast<unsigned char*>(camera) + 0x10;
        std::memcpy(matrix, address, sizeof(matrix));
        for (float value : matrix) if (!std::isfinite(value)) return false;
        if (std::abs(matrix[15] - 1.f) > 0.001f) return false;
#ifdef AC6_HEADTRACKING_PROBE
        float delta[9];
        if (!ReadPublishedDelta(delta)) return false;
        ApplyRotation(matrix, delta);
#else
        ApplyYaw(matrix, 0.17453292519943295f);
#endif
        // Preserve all translation and FOV bytes. This runs after original_copy
        // and before its caller resumes updating camera-derived state.
        std::memcpy(address, matrix, 12 * sizeof(float));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ++exceptions;
        apply_offset.store(false);
        return false;
    }
}

static void __fastcall CameraCopy(void* target, void* source) {
    original_copy(target, source);
    ++calls;
    if (source == nullptr || target == source || source != SourceCamera()) return;
    ++matched;
    last_thread.store(GetCurrentThreadId());
    std::uintptr_t empty = 0;
    destination.compare_exchange_strong(empty, reinterpret_cast<std::uintptr_t>(target));
    if (apply_offset.load() && destination.load() == reinterpret_cast<std::uintptr_t>(target)) {
        if (ApplyOffset(target)) ++applications;
    }
}

static void Log(FILE* file, const char* phase, MH_STATUS status = MH_OK) {
    std::fprintf(file,
        "{\"phase\":\"%s\",\"tick_ms\":%llu,\"pid\":%lu,\"status\":\"%s\","
        "\"calls\":%llu,\"matched_source_calls\":%llu,\"applications\":%llu,"
        "\"exceptions\":%llu,\"destination\":\"0x%llx\",\"last_callback_thread\":%lu}\n",
        phase, GetTickCount64(), GetCurrentProcessId(), MH_StatusToString(status), calls.load(),
        matched.load(), applications.load(), exceptions.load(),
        static_cast<unsigned long long>(destination.load()), last_thread.load());
    std::fflush(file);
}

static DWORD WINAPI Worker(void*) {
    wchar_t path[32768];
    const DWORD length = GetModuleFileNameW(probe_module, path, 32768);
    if (!length || length >= 32768) return 1;
    wchar_t* name = std::wcsrchr(path, L'\\');
    if (!name) return 1;
    ++name;
#ifdef AC6_HEADTRACKING_CALIBRATED_PROBE
    std::wstring stem(name);
    if(stem.size()<9 || stem.substr(0,4)!=L"ac6_" || stem.substr(stem.size()-4)!=L".dll") return 1;
    stem=stem.substr(4,stem.size()-8);
    swprintf_s(name, 32768 - (name - path), L"%ls_%lu.jsonl",stem.c_str(),GetCurrentProcessId());
#elif defined(AC6_HEADTRACKING_DISPLAY_PROBE)
    swprintf_s(name, 32768 - (name - path), L"headtracking_display_probe_%lu.jsonl", GetCurrentProcessId());
#elif defined(AC6_HEADTRACKING_PROBE)
    swprintf_s(name, 32768 - (name - path), L"headtracking_probe_%lu.jsonl", GetCurrentProcessId());
#else
    swprintf_s(name, 32768 - (name - path), L"camera_probe_%lu.jsonl", GetCurrentProcessId());
#endif
    FILE* file = _wfsopen(path, L"wx", _SH_DENYNO);
    if (!file) return 1;
    game_base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(L"armoredcore6.exe"));
    // Loader state and live bytes must match the researched single-player host.
    if (!game_base || !GetModuleHandleW(L"ac6_coop.dll") ||
        GetModuleHandleW(L"EasyAntiCheat_EOS.dll") || GetModuleHandleW(L"EasyAntiCheat.dll")) {
        Log(file, "host_rejected"); std::fclose(file); return 1;
    }
    const unsigned char expected[] = {0x48,0x89,0x5c,0x24,0x08,0x57,0x48,0x83,0xec,0x20,0x48,0x8b,0xf9,0x48,0x8b,0xda};
    auto target = reinterpret_cast<void*>(game_base + 0x6bfe50);
    if (std::memcmp(target, expected, sizeof(expected)) != 0) {
        Log(file, "prologue_rejected"); std::fclose(file); return 1;
    }
#ifdef AC6_HEADTRACKING_PROBE
    wchar_t mapping_name[128], mutex_name[128];
    swprintf_s(mapping_name, L"Local\\AC6VRPose-%lu", GetCurrentProcessId());
    swprintf_s(mutex_name, L"Local\\AC6VRPoseMutex-%lu", GetCurrentProcessId());
    HANDLE mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, mapping_name);
    pose_mutex = OpenMutexW(SYNCHRONIZE | MUTEX_MODIFY_STATE, FALSE, mutex_name);
    if (mapping) shared_pose = static_cast<const PosePacket*>(MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 128));
    if (!mapping || !pose_mutex || !shared_pose) {
        Log(file, "pose_transport_unavailable"); std::fclose(file); return 1;
    }
    // Mapping and mutex remain allocated with the inert DLL after disable;
    // a callback returning concurrently can never touch an unmapped packet.
#endif
    auto status = MH_Initialize();
    if (status != MH_OK) { Log(file, "initialize_failed", status); std::fclose(file); return 1; }
    status = MH_CreateHook(target, reinterpret_cast<void*>(&CameraCopy), reinterpret_cast<void**>(&original_copy));
    if (status != MH_OK) { Log(file, "create_failed", status); std::fclose(file); return 1; }
    status = MH_EnableHook(target);
    if (status != MH_OK) { Log(file, "enable_failed", status); std::fclose(file); return 1; }
    Log(file, "observing");
    const bool continue_test = WaitOrCancel(2000);
    if (!continue_test) {
        Log(file, "cancelled");
    } else if (matched.load() == 0) {
        Log(file, "no_camera_calls_skip_offset");
    } else {
        apply_offset.store(true);
        Log(file, "offset_started");
#ifdef AC6_HEADTRACKING_PROBE
        const bool completed = WaitOrCancel(15000);
#else
        const bool completed = WaitOrCancel(3000);
#endif
        apply_offset.store(false);
        Log(file, "offset_finished");
        if (completed) WaitOrCancel(3000);
    }
    apply_offset.store(false);
    status = MH_DisableHook(target);
    Log(file, status == MH_OK ? "disabled" : "disable_failed", status);
    // Leave disabled trampoline and DLL mapped; no self-unload race.
    std::fclose(file);
    return status == MH_OK ? 0 : 1;
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        probe_module = module;
        HANDLE thread = CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);
        if (!thread) return FALSE;
        CloseHandle(thread);
    }
    return TRUE;
}
