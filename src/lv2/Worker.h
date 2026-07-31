#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#include <lv2/core/lv2.h>
#include <lv2/worker/worker.h>

namespace sf
{

// worker:schedule implementation with a dedicated background thread per
// instance (one Worker = one plugin's worker thread, matching the LV2
// convention that work() may block/allocate as long as it's off the audio
// thread).
//
// - schedule_work() (called from the audio thread or setup code) pushes the
//   request onto a mutex-guarded queue and wakes the worker thread. The
//   critical section is a short enqueue, not the actual work -- a real
//   wait-free SPSC queue would be the gold-plated version, but for a single
//   host like this a brief lock on schedule is a documented, acceptable
//   tradeoff (the important part -- not blocking the audio thread on disk
//   I/O -- is what actually matters here).
// - The worker thread calls the plugin's work(), which may call
//   RespondCallback any number of times; those responses are written into a
//   lock-free SPSC ring buffer (worker thread = single producer).
// - DrainResponses() is called by the host once per audio block, at the
//   start of Run(), on the audio thread: it reads whatever responses are
//   available and calls the plugin's work_response() for each. This side
//   never blocks -- it just drains what's there, per the LV2 worker
//   extension's contract.
class Worker
{
public:
	Worker();
	~Worker();

	Worker(const Worker&) = delete;
	Worker& operator=(const Worker&) = delete;

	// Must be called once the plugin instance exists and its
	// LV2_Worker_Interface has been looked up, before anything can trigger
	// schedule_work() (e.g. before calling state restore or Run()). Starts
	// the worker thread.
	void Bind(const LV2_Worker_Interface* interface, LV2_Handle plugin_handle);

	// Stops the worker thread. Must be called before the plugin instance it
	// was bound to is freed. Safe to call more than once / if never bound.
	void Stop();

	const LV2_Feature* ScheduleFeature() const { return &schedule_feature_; }

	// Drains pending work responses and delivers them via the plugin's
	// work_response(), on whatever thread calls this (intended to be the
	// audio thread, once per block). Never blocks.
	void DrainResponses();

	// True if work has been scheduled but not yet finished executing.
	bool HasPendingWork() const { return pending_work_.load(std::memory_order_acquire) > 0; }

	// Blocks until no scheduled work is outstanding. Setup-time only -- must
	// never be called from the audio thread, since it waits on disk I/O.
	void WaitForPendingWork();

private:
	static LV2_Worker_Status ScheduleCallback(LV2_Worker_Schedule_Handle handle, uint32_t size, const void* data);
	static LV2_Worker_Status RespondCallback(LV2_Worker_Respond_Handle handle, uint32_t size, const void* data);

	void ThreadMain();
	void PushResponse(uint32_t size, const void* data);

	const LV2_Worker_Interface* interface_ = nullptr;
	LV2_Handle plugin_handle_ = nullptr;

	LV2_Worker_Schedule schedule_data_;
	LV2_Feature schedule_feature_;

	std::thread thread_;
	std::atomic<bool> running_{false};

	std::mutex request_mutex_;
	std::condition_variable request_cv_;
	std::deque<std::vector<uint8_t>> request_queue_;

	// Queued-but-not-yet-finished work items (queued + currently executing),
	// so WaitForPendingWork() can tell "idle" from "mid-work".
	std::atomic<int> pending_work_{0};

	// Lock-free SPSC byte ring buffer of framed responses: each frame is a
	// uint32_t size followed by that many bytes. Producer: worker thread
	// (PushResponse, via RespondCallback). Consumer: whoever calls
	// DrainResponses (the audio thread).
	static constexpr size_t kResponseRingCapacity = 1 << 16;  // 64 KiB
	std::vector<uint8_t> response_ring_ = std::vector<uint8_t>(kResponseRingCapacity);
	std::atomic<size_t> response_head_{0};
	std::atomic<size_t> response_tail_{0};
};

}  // namespace sf
