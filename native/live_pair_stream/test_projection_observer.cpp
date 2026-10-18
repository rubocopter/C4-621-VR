// CPU only: never initialize MinHook, install hooks, or access a game process.
#include "projection_observer.hpp"
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

static void Require(bool ok, const char* why) {
    if (!ok) throw std::runtime_error(why);
}

static ProjectionSnapshot Sample(float fov = 0.7f) {
    ProjectionSnapshot s;
    s.view = 0x1234;
    s.fov = fov; s.near_plane = 0.25f; s.far_plane = 10000;
    s.width = 512; s.height = 256; s.aspect = 2;
    s.jitter_x = 0.125f; s.jitter_y = -0.25f;
    s.shift_x = 0.5f; s.shift_y = -0.75f;
    // Preserve every finite coefficient, including shapes not used by the GPU.
    for (unsigned i = 0; i < 16; ++i) s.matrix[i] = float(i) * 0.125f;
    return s;
}

static std::string Log(const frame_trace::ProjectionObserver& observer) {
    FILE* file = nullptr;
    Require(tmpfile_s(&file) == 0 && file, "Cannot open CPU diagnostic fixture");
    std::unique_ptr<FILE, decltype(&std::fclose)> close(file, &std::fclose);
    Require(observer.Write(file), "Diagnostic write failed");
    Require(std::fflush(file) == 0, "Cannot flush fixture");
    std::rewind(file);
    std::string result;
    char buffer[4096];
    while (const auto n = std::fread(buffer, 1, sizeof(buffer), file)) result.append(buffer, n);
    Require(!std::ferror(file), "Cannot read fixture");
    return result;
}

static std::vector<std::string> Rows(const std::string& log, const char* phase) {
    std::vector<std::string> result;
    std::size_t begin = 0;
    while (begin < log.size()) {
        auto end = log.find('\n', begin);
        Require(end != std::string::npos, "Truncated JSON line");
        auto row = log.substr(begin, end - begin);
        if (row.find(std::string("\"phase\":\"") + phase + "\"") != std::string::npos)
            result.push_back(row);
        begin = end + 1;
    }
    return result;
}

static const char* Value(const std::string& row, const char* field) {
    const auto key = std::string("\"") + field + "\":";
    const auto pos = row.find(key);
    Require(pos != std::string::npos, "Diagnostic field missing");
    return row.c_str() + pos + key.size();
}
static std::uint64_t Number(const std::string& row, const char* field) {
    return std::strtoull(Value(row, field), nullptr, 10);
}

static void PreservesAllViewsAndMetadata() {
    auto storage = std::make_unique<frame_trace::ProjectionObserver>();
    auto& observer = *storage;
    const auto s = Sample();
    Require(observer.Record(s, 0x5678, 7, 100, 120, 42), "Valid auxiliary view was filtered");
    Require(observer.Record(Sample(2.8f), 0x9999, 0, 121, 130, 43), "Second FOV or unthrottled call was filtered");
    observer.Stop();
    const auto rows = Rows(Log(observer), "projection_sample");
    Require(rows.size() == 2, "All supported views must be retained");
    const auto& row = rows[0];
    Require(Number(row, "call_ordinal") == 1 && Number(rows[1], "call_ordinal") == 2, "Call ordinals lost");
    Require(Number(row, "begin_qpc") == 100 && Number(row, "end_qpc") == 120 && Number(row, "thread") == 42, "Timing/thread metadata lost");
    Require(row.find("\"view\":\"0x1234\"") != std::string::npos && row.find("\"output\":\"0x5678\"") != std::string::npos, "View/output identity lost");
    Require(Number(row, "jitter_flag") == 7 && std::strncmp(Value(row, "jitter_enabled"), "true", 4) == 0, "Raw jitter flag lost");
    Require(std::strncmp(Value(rows[1], "jitter_enabled"), "false", 5) == 0, "Disabled jitter flag lost");
    for (const auto& r : rows) {
        Require(std::strncmp(Value(r, "frame_associated"), "false", 5) == 0, "Helper calls must not claim frame association");
        Require(std::strncmp(Value(r, "helper_output_only"), "true", 4) == 0 && std::strncmp(Value(r, "gpu_use_proven"), "false", 5) == 0, "Helper output misrepresented as GPU use");
        Require(r.find("frame_id") == std::string::npos, "Call ordinal mislabeled as frame ID");
    }
    const std::pair<const char*, double> fields[] = {
        {"fov_y", double(s.fov)}, {"near_plane", 0.25}, {"far_plane", 10000},
        {"aspect", 2}, {"width", 512}, {"height", 256}, {"projection_mode", 0},
        {"jitter_x", 0.125}, {"jitter_y", -0.25}, {"shift_x", 0.5}, {"shift_y", -0.75}
    };
    for (const auto& field : fields)
        Require(float(std::strtod(Value(row, field.first), nullptr)) == float(field.second), "Snapshot field changed");
    auto* cursor = Value(row, "matrix");
    Require(*cursor++ == '[', "Matrix must be an array");
    for (unsigned i = 0; i < 16; ++i) {
        char* end = nullptr;
        Require(float(std::strtod(cursor, &end)) == float(i) * 0.125f && end != cursor, "Full matrix changed");
        Require(*end == (i == 15 ? ']' : ','), "Matrix must contain exactly sixteen coefficients");
        cursor = end + 1;
    }
    const auto stack_count = Number(row, "stack_count");
    Require(stack_count > 0 && stack_count <= 8, "Per-sample stack missing or unbounded");
    Require(*Value(row, "stack") == '[', "Stack addresses missing");
}

static void InvalidDoesNotConsumeCapacityAndStopIsFinal() {
    auto storage = std::make_unique<frame_trace::ProjectionObserver>();
    auto& observer = *storage;
    auto invalid = [&](ProjectionSnapshot s) {
        Require(!observer.Record(s, 0x5678, 0, 1, 2, 3), "Invalid snapshot accepted");
    };
    auto s = Sample(); s.matrix[15] = std::numeric_limits<float>::quiet_NaN(); invalid(s);
    s = Sample(); s.fov = std::numeric_limits<float>::infinity(); invalid(s);
    s = Sample(); s.jitter_x = std::numeric_limits<float>::infinity(); invalid(s);
    s = Sample(); s.shift_y = std::numeric_limits<float>::quiet_NaN(); invalid(s);
    s = Sample(); s.projection_mode = 1; invalid(s);
    s = Sample(); s.near_plane = 0; invalid(s);
    s = Sample(); s.far_plane = s.near_plane; invalid(s);
    s = Sample(); s.width = 127; invalid(s);
    s = Sample(); s.height = 16385; invalid(s);
    s = Sample(); s.aspect = -1; invalid(s);
    Require(observer.Record(Sample(), 0x5678, 0, 3, 4, 5), "Invalid inputs consumed storage");
    observer.Stop();
    const auto first = Log(observer);
    Require(!observer.Record(Sample(), 0x5678, 0, 5, 6, 7), "Stop admitted a new sample");
    Require(first == Log(observer), "Stopped observer changed after rejected Record");
    const auto summary = Rows(first, "projection_summary").at(0);
    Require(Number(summary, "calls") == 11 && Number(summary, "invalid") == 10 && Number(summary, "samples") == 1, "Invalid/call counters wrong");
    Require(Number(Rows(first, "projection_sample").at(0), "call_ordinal") == 11, "Invalid calls must leave ordinal gaps");
}

static void OverflowIsBoundedAndConcurrentPublicationIsCoherent() {
    auto storage = std::make_unique<frame_trace::ProjectionObserver>();
    auto& observer = *storage;
    std::vector<std::thread> workers;
    for (unsigned t = 0; t < 8; ++t) workers.emplace_back([&, t] {
        for (unsigned i = 0; i < 80; ++i) {
            auto s = Sample();
            s.view = 0x10000 + t * 80 + i;
            s.shift_x = float(t * 80 + i);
            for (auto& coefficient : s.matrix) coefficient = s.shift_x;
            observer.Record(s, s.view + 0x10000, static_cast<unsigned char>(t),
                            std::int64_t(s.view), std::int64_t(s.view + 1), t + 1);
        }
    });
    for (auto& worker : workers) worker.join();
    observer.Stop();
    const auto log = Log(observer);
    const auto rows = Rows(log, "projection_sample");
    const auto summary = Rows(log, "projection_summary").at(0);
    Require(rows.size() == 256 && Number(summary, "samples") == 256, "Bounded sample storage exceeded or lost records");
    const auto calls = Number(summary, "calls");
    Require(calls >= 256 && calls <= 640 && Number(summary, "valid") == calls &&
            Number(summary, "overflow") == calls - 256 && Number(summary, "invalid") == 0, "Concurrent capacity cutoff accounting wrong");
    Require(std::strncmp(Value(summary, "sample_limit_reached"), "true", 4) == 0, "Capacity cutoff must be explicit");
    Require(Number(summary, "incomplete_slots") == 0 && Number(summary, "in_flight") == 0, "Completed publishers still incomplete");
    Require(std::strncmp(Value(summary, "snapshot_complete"), "true", 4) == 0, "Quiescent snapshot not marked complete");
    std::array<bool, 641> seen{};
    for (const auto& row : rows) {
        const auto ordinal = Number(row, "call_ordinal");
        Require(ordinal > 0 && ordinal <= 640 && !seen[ordinal], "Concurrent call ordinal duplicated");
        seen[ordinal] = true;
        const auto begin = Number(row, "begin_qpc");
        Require(Number(row, "end_qpc") == begin + 1, "Torn timing record");
        const auto marker = std::strtod(Value(row, "shift_x"), nullptr);
        Require(marker == double(begin - 0x10000), "Torn field publication");
        auto* cursor = Value(row, "matrix") + 1;
        for (unsigned i = 0; i < 16; ++i) {
            char* end = nullptr;
            Require(std::strtod(cursor, &end) == marker && end != cursor, "Torn matrix publication");
            cursor = end + 1;
        }
    }
}

static void PausedWindowIsOneShotAndCapacityQuietsAdmission() {
    auto storage = std::make_unique<frame_trace::ProjectionObserver>(false);
    auto& observer = *storage;
    Require(!observer.Record(Sample(), 0x5678, 0, 1, 2, 3), "Paused observer admitted a call");
    Require(observer.BeginWindow(), "Paused window could not start");
    Require(!observer.BeginWindow(), "A window may only start once");
    for (unsigned i = 0; i < 256; ++i)
        Require(observer.Record(Sample(), 0x5678, 255, i, i + 1, 3), "Capacity closed before 256 records");
    Require(!observer.Record(Sample(), 0x5678, 0, 300, 301, 3), "Capacity did not close admission");
    Require(!observer.BeginWindow(), "A full window reopened");
    observer.Stop();
    const auto log = Log(observer);
    const auto summary = Rows(log, "projection_summary").at(0);
    Require(Number(summary, "calls") == 256 && Number(summary, "overflow") == 0, "Quiet full window still counted later calls");
    Require(Number(Rows(log, "projection_sample").at(0), "jitter_flag") == 255, "Raw jitter byte was normalized");
    Require(!observer.HookRestored(), "CPU observer incorrectly claims an installed hook was restored");
    auto stopped = std::make_unique<frame_trace::ProjectionObserver>(false);
    stopped->Stop();
    Require(!stopped->BeginWindow(), "Stop must prevent delayed arming");
    const auto empty = Rows(Log(*stopped), "projection_summary").at(0);
    Require(Number(empty, "calls") == 0 && Number(empty, "samples") == 0, "Paused/stopped observer mutated counters");
}

namespace frame_trace {
// Model a scheduler-paused publisher after slot reservation and before ready.
// All waits live in this fixture, never in the production callback. A friend
// fixture avoids adding test-only pause/reset methods to the observer API.
struct ProjectionObserverTestPeer {
    static bool DelayedAdmittedRecord(ProjectionObserver& observer, std::atomic<bool>& admitted,
                                     std::atomic<bool>& release) {
        ProjectionObserver::Flight flight(observer);
        const auto ordinal = observer.calls_.fetch_add(1) + 1;
        admitted.store(true, std::memory_order_release);
        while (!release.load(std::memory_order_acquire)) std::this_thread::yield();
        return observer.Store(Sample(), 0x5678, 1, 100, 120, 42, ordinal);
    }
    static void SuspendedPublisher(ProjectionObserver& observer, std::atomic<bool>& reserved,
                                   std::atomic<bool>& release) {
        ProjectionObserver::Flight flight(observer);
        const auto ordinal = observer.calls_.fetch_add(1) + 1;
        const auto index = observer.valid_.fetch_add(1);
        auto& entry = observer.entries_[static_cast<std::size_t>(index)];
        entry.sample = Sample();
        entry.ordinal = ordinal;
        reserved.store(true, std::memory_order_release);
        while (!release.load(std::memory_order_acquire)) std::this_thread::yield();
        entry.output = 0x5678; entry.jitter = 1;
        entry.begin_qpc = 100; entry.end_qpc = 120; entry.thread = 42;
        entry.ready.store(true, std::memory_order_release);
    }
};
}

static void AlreadyAdmittedOverflowIsCountedExactlyOnce() {
    auto storage = std::make_unique<frame_trace::ProjectionObserver>();
    auto& observer = *storage;
    for (unsigned i = 0; i < 255; ++i)
        Require(observer.Record(Sample(), 0x5678, 0, i, i + 1, 1), "Lost capacity before overflow fixture");
    std::atomic<bool> admitted{false}, release{false};
    bool retained = true;
    std::thread publisher([&] {
        retained = frame_trace::ProjectionObserverTestPeer::DelayedAdmittedRecord(observer, admitted, release);
    });
    while (!admitted.load(std::memory_order_acquire)) std::this_thread::yield();
    const bool last = observer.Record(Sample(), 0x5678, 0, 300, 301, 1);
    release.store(true, std::memory_order_release);
    publisher.join();
    Require(last && !retained, "Already-admitted excess call did not overflow the full window");
    observer.Stop();
    const auto summary = Rows(Log(observer), "projection_summary").at(0);
    Require(Number(summary, "calls") == 257 && Number(summary, "valid") == 257 &&
            Number(summary, "overflow") == 1 && Number(summary, "samples") == 256,
            "Already-admitted overflow was not counted exactly once");
}

static void StopAndWriteSkipAnUnpublishedConcurrentSlot() {
    auto storage = std::make_unique<frame_trace::ProjectionObserver>();
    auto& observer = *storage;
    std::atomic<bool> reserved{false}, release{false};
    {
        std::thread publisher([&] {
            frame_trace::ProjectionObserverTestPeer::SuspendedPublisher(observer, reserved, release);
        });
        struct Join {
            std::atomic<bool>& release;
            std::thread& thread;
            ~Join() { release.store(true, std::memory_order_release); thread.join(); }
        } join{release, publisher};
        const auto deadline = GetTickCount64() + 5000;
        while (!reserved.load(std::memory_order_acquire) && GetTickCount64() < deadline) std::this_thread::yield();
        Require(reserved.load(), "Suspended-publisher fixture did not start");
        observer.Stop();
        // Publisher cannot complete until after Log returns: a waiting Stop or
        // Write would deadlock, and reading reserved payload would emit a row.
        const auto partial = Log(observer);
        Require(Rows(partial, "projection_sample").empty(), "Unpublished payload was read");
        const auto summary = Rows(partial, "projection_summary").at(0);
        Require(Number(summary, "incomplete_slots") == 1 && Number(summary, "in_flight") == 1, "Incomplete publication not reported");
        Require(std::strncmp(Value(summary, "snapshot_complete"), "false", 5) == 0, "In-flight snapshot falsely complete");
    }
    const auto complete = Log(observer);
    Require(Rows(complete, "projection_sample").size() == 1, "Late immutable publication was lost");
    const auto summary = Rows(complete, "projection_summary").at(0);
    Require(Number(summary, "incomplete_slots") == 0 && Number(summary, "in_flight") == 0 &&
            std::strncmp(Value(summary, "snapshot_complete"), "true", 4) == 0, "Late publication did not become complete");
}

int main() try {
    PreservesAllViewsAndMetadata();
    InvalidDoesNotConsumeCapacityAndStopIsFinal();
    OverflowIsBoundedAndConcurrentPublicationIsCoherent();
    PausedWindowIsOneShotAndCapacityQuietsAdmission();
    AlreadyAdmittedOverflowIsCountedExactlyOnce();
    StopAndWriteSkipAnUnpublishedConcurrentSlot();
    std::cout << "PASS: projection observer CPU retention, validation, bounded window and incomplete publication\n";
    return 0;
} catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
}
