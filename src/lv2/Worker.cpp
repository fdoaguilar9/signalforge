#include "Worker.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace sf
{

namespace
{

// Response payloads seen in practice (NAM's model-swap messages, IR loader
// responses) are small fixed structs well under this; if a plugin somehow
// sends something bigger, we drop it and log rather than allocate on
// whichever thread is draining (which may be the audio thread).
constexpr uint32_t kMaxResponsePayload = 4096;

}  // namespace

Worker::Worker()
{
	schedule_data_.handle = this;
	schedule_data_.schedule_work = &Worker::ScheduleCallback;

	schedule_feature_.URI = LV2_WORKER__schedule;
	schedule_feature_.data = &schedule_data_;
}

Worker::~Worker()
{
	Stop();
}

void Worker::Bind(const LV2_Worker_Interface* interface, LV2_Handle plugin_handle)
{
	interface_ = interface;
	plugin_handle_ = plugin_handle;

	if (!running_)
	{
		running_ = true;
		thread_ = std::thread(&Worker::ThreadMain, this);
	}
}

void Worker::Stop()
{
	if (running_)
	{
		{
			std::lock_guard<std::mutex> lock(request_mutex_);
			running_ = false;
		}
		request_cv_.notify_all();

		if (thread_.joinable())
		{
			thread_.join();
		}
	}
}

void Worker::ThreadMain()
{
	for (;;)
	{
		std::vector<uint8_t> request;

		{
			std::unique_lock<std::mutex> lock(request_mutex_);
			request_cv_.wait(lock, [this] { return !request_queue_.empty() || !running_; });

			if (!running_ && request_queue_.empty())
			{
				return;
			}

			request = std::move(request_queue_.front());
			request_queue_.pop_front();
		}

		if (interface_ && interface_->work)
		{
			interface_->work(plugin_handle_, &Worker::RespondCallback, this, static_cast<uint32_t>(request.size()),
				request.data());
		}

		pending_work_.fetch_sub(1, std::memory_order_release);
	}
}

void Worker::WaitForPendingWork()
{
	while (HasPendingWork())
	{
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
}

LV2_Worker_Status Worker::ScheduleCallback(LV2_Worker_Schedule_Handle handle, uint32_t size, const void* data)
{
	auto* self = static_cast<Worker*>(handle);

	{
		std::lock_guard<std::mutex> lock(self->request_mutex_);
		const auto* bytes = static_cast<const uint8_t*>(data);
		self->request_queue_.emplace_back(bytes, bytes + size);
		self->pending_work_.fetch_add(1, std::memory_order_release);
	}
	self->request_cv_.notify_one();

	return LV2_WORKER_SUCCESS;
}

LV2_Worker_Status Worker::RespondCallback(LV2_Worker_Respond_Handle handle, uint32_t size, const void* data)
{
	auto* self = static_cast<Worker*>(handle);
	self->PushResponse(size, data);

	return LV2_WORKER_SUCCESS;
}

void Worker::PushResponse(uint32_t size, const void* data)
{
	if (size > kMaxResponsePayload)
	{
		std::fprintf(stderr, "Worker: dropping oversized response (%u bytes > %u max).\n", size, kMaxResponsePayload);
		return;
	}

	const size_t capacity = response_ring_.size();
	const size_t frame_size = sizeof(uint32_t) + size;

	size_t tail = response_tail_.load(std::memory_order_relaxed);

	// Wait for the consumer (audio thread) to free up space. This spins on
	// the worker thread, never on the audio thread, so it's safe even
	// though it isn't lock-free -- capacity is large relative to message
	// sizes, so this should never actually trigger in practice.
	for (;;)
	{
		const size_t head = response_head_.load(std::memory_order_acquire);
		const size_t available = (head + capacity - tail - 1) % capacity;

		if (available >= frame_size)
		{
			break;
		}

		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}

	auto write_bytes = [&](size_t pos, const void* src, size_t len)
	{
		const size_t first = std::min(len, capacity - pos);
		std::memcpy(&response_ring_[pos], src, first);
		if (len > first)
		{
			std::memcpy(&response_ring_[0], static_cast<const uint8_t*>(src) + first, len - first);
		}
	};

	write_bytes(tail, &size, sizeof(uint32_t));
	write_bytes((tail + sizeof(uint32_t)) % capacity, data, size);

	response_tail_.store((tail + frame_size) % capacity, std::memory_order_release);
}

void Worker::DrainResponses()
{
	const size_t capacity = response_ring_.size();
	const size_t tail = response_tail_.load(std::memory_order_acquire);
	size_t head = response_head_.load(std::memory_order_relaxed);

	auto read_bytes = [&](size_t pos, void* dst, size_t len)
	{
		const size_t first = std::min(len, capacity - pos);
		std::memcpy(dst, &response_ring_[pos], first);
		if (len > first)
		{
			std::memcpy(static_cast<uint8_t*>(dst) + first, &response_ring_[0], len - first);
		}
	};

	uint8_t payload[kMaxResponsePayload];

	while (head != tail)
	{
		uint32_t size = 0;
		read_bytes(head, &size, sizeof(uint32_t));

		const size_t payload_pos = (head + sizeof(uint32_t)) % capacity;
		read_bytes(payload_pos, payload, size);

		head = (head + sizeof(uint32_t) + size) % capacity;
		response_head_.store(head, std::memory_order_release);

		if (interface_ && interface_->work_response)
		{
			interface_->work_response(plugin_handle_, size, payload);
		}
	}
}

}  // namespace sf
