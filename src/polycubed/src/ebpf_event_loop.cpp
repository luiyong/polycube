#include "ebpf_event_loop.h"

#include <chrono>
#include <cstring>
#include <errno.h>
#include <utility>
#include <vector>

namespace polycube {
namespace polycubed {

EbpfEventLoop &EbpfEventLoop::get_instance() {
  static EbpfEventLoop instance;
  return instance;
}

EbpfEventLoop::EbpfEventLoop()
    : next_id_(1),
      running_(false),
      stop_requested_(false),
      sources_changed_(false),
      poll_timeout_ms_(100) {}

EbpfEventLoop::~EbpfEventLoop() {
  Stop();
}

void EbpfEventLoop::Start() {
  std::lock_guard<std::mutex> guard(mutex_);
  if (running_)
    return;

  stop_requested_ = false;
  running_ = true;
  worker_ = std::thread(&EbpfEventLoop::Run, this);
}

void EbpfEventLoop::Stop() {
  {
    std::lock_guard<std::mutex> guard(mutex_);
    if (!running_)
      return;
    stop_requested_ = true;
  }
  cv_.notify_all();
  if (worker_.joinable())
    worker_.join();
  running_ = false;
  stop_requested_ = false;
}

int EbpfEventLoop::RegisterPerfBuffer(int map_fd, const PerfCallback &callback,
                                      void *cookie, size_t page_cnt) {
  Start();

  int err = 0;
  int handle = next_id_.fetch_add(1);
  std::shared_ptr<PerfEventSource> source = PerfEventSource::Create(
      handle, map_fd, callback, cookie, page_cnt, GetLogger(), &err);
  if (!source || err) {
    return err ? err : -EINVAL;
  }

  {
    std::lock_guard<std::mutex> guard(mutex_);
    sources_.insert(std::make_pair(handle, source));
    sources_changed_ = true;
  }
  cv_.notify_all();
  return handle;
}

int EbpfEventLoop::RegisterRingBuffer(int map_fd, const RingCallback &callback,
                                      void *cookie) {
  Start();

  int err = 0;
  int handle = next_id_.fetch_add(1);
  std::shared_ptr<RingEventSource> source = RingEventSource::Create(
      handle, map_fd, callback, cookie, GetLogger(), &err);
  if (!source || err) {
    return err ? err : -EINVAL;
  }

  {
    std::lock_guard<std::mutex> guard(mutex_);
    sources_.insert(std::make_pair(handle, source));
    sources_changed_ = true;
  }
  cv_.notify_all();
  return handle;
}

void EbpfEventLoop::Unregister(int handle) {
  std::shared_ptr<EventSource> source;
  {
    std::lock_guard<std::mutex> guard(mutex_);
    std::map<int, std::shared_ptr<EventSource>>::iterator it =
        sources_.find(handle);
    if (it == sources_.end())
      return;
    source = it->second;
    sources_.erase(it);
    sources_changed_ = true;
  }
  cv_.notify_all();
}

EbpfEventLoop::EventSource::EventSource(int id) : id_(id) {}

EbpfEventLoop::EventSource::~EventSource() {}

int EbpfEventLoop::EventSource::id() const { return id_; }

std::shared_ptr<EbpfEventLoop::PerfEventSource>
EbpfEventLoop::PerfEventSource::Create(
    int id, int map_fd, const PerfCallback &callback, void *cookie,
    size_t page_cnt, std::shared_ptr<spdlog::logger> logger, int *err) {
  std::shared_ptr<PerfEventSource> source(
      new PerfEventSource(id, map_fd, callback, cookie, page_cnt, logger, err));
  if (err && *err)
    source.reset();
  return source;
}

EbpfEventLoop::PerfEventSource::PerfEventSource(
    int id, int map_fd, const PerfCallback &callback, void *cookie,
    size_t page_cnt, std::shared_ptr<spdlog::logger> logger, int *err)
    : EventSource(id),
      buffer_(nullptr),
      callback_(callback),
      cookie_(cookie),
      logger_(logger) {
  buffer_ = perf_buffer__new(map_fd, page_cnt, &PerfEventSource::SampleTrampoline,
                             &PerfEventSource::LostTrampoline, this, nullptr);
  if (!buffer_) {
    if (err)
      *err = -errno;
  } else if (err) {
    *err = 0;
  }
}

EbpfEventLoop::PerfEventSource::~PerfEventSource() {
  if (buffer_)
    perf_buffer__free(buffer_);
}

int EbpfEventLoop::PerfEventSource::Poll() {
  if (!buffer_)
    return -EINVAL;
  return perf_buffer__poll(buffer_, 0);
}

void EbpfEventLoop::PerfEventSource::SampleTrampoline(void *ctx, int cpu,
                                                      void *data,
                                                      __u32 size) {
  PerfEventSource *source = static_cast<PerfEventSource *>(ctx);
  if (!source)
    return;
  if (source->callback_)
    source->callback_(source->cookie_, data, static_cast<int>(size));
}

void EbpfEventLoop::PerfEventSource::LostTrampoline(void *ctx, int cpu,
                                                    __u64 lost) {
  PerfEventSource *source = static_cast<PerfEventSource *>(ctx);
  if (!source || !source->logger_)
    return;
  source->logger_->warn("Lost {} perf buffer events on CPU {}", lost, cpu);
}

std::shared_ptr<EbpfEventLoop::RingEventSource>
EbpfEventLoop::RingEventSource::Create(
    int id, int map_fd, const RingCallback &callback, void *cookie,
    std::shared_ptr<spdlog::logger> logger, int *err) {
  std::shared_ptr<RingEventSource> source(
      new RingEventSource(id, map_fd, callback, cookie, logger, err));
  if (err && *err)
    source.reset();
  return source;
}

EbpfEventLoop::RingEventSource::RingEventSource(
    int id, int map_fd, const RingCallback &callback, void *cookie,
    std::shared_ptr<spdlog::logger> logger, int *err)
    : EventSource(id),
      buffer_(nullptr),
      callback_(callback),
      cookie_(cookie),
      logger_(logger) {
  buffer_ = ring_buffer__new(map_fd, &RingEventSource::SampleTrampoline, this,
                             nullptr);
  if (!buffer_) {
    if (err)
      *err = -errno;
  } else if (err) {
    *err = 0;
  }
}

EbpfEventLoop::RingEventSource::~RingEventSource() {
  if (buffer_)
    ring_buffer__free(buffer_);
}

int EbpfEventLoop::RingEventSource::Poll() {
  if (!buffer_)
    return -EINVAL;
  return ring_buffer__poll(buffer_, 0);
}

int EbpfEventLoop::RingEventSource::SampleTrampoline(void *ctx, void *data,
                                                     size_t size) {
  RingEventSource *source = static_cast<RingEventSource *>(ctx);
  if (!source)
    return 0;
  if (source->callback_)
    source->callback_(source->cookie_, data, size);
  return 0;
}

void EbpfEventLoop::Run() {
  std::unique_lock<std::mutex> lock(mutex_);
  while (true) {
    if (stop_requested_)
      break;

    if (sources_.empty()) {
      cv_.wait(lock, [this]() { return stop_requested_ || !sources_.empty(); });
      if (stop_requested_)
        break;
    }

    std::vector<std::shared_ptr<EventSource>> snapshot;
    snapshot.reserve(sources_.size());
    for (std::map<int, std::shared_ptr<EventSource>>::const_iterator it =
             sources_.begin();
         it != sources_.end(); ++it) {
      snapshot.push_back(it->second);
    }

    lock.unlock();

    bool had_activity = false;
    for (size_t i = 0; i < snapshot.size(); ++i) {
      int ret = snapshot[i]->Poll();
      if (ret > 0) {
        had_activity = true;
      } else if (ret < 0 && ret != -EINTR) {
        std::shared_ptr<spdlog::logger> logger = GetLogger();
        if (logger)
          logger->error("Error polling eBPF buffer (handle {}): {}",
                        snapshot[i]->id(), std::strerror(-ret));
      }
    }

    lock.lock();

    if (!had_activity) {
      if (!stop_requested_) {
        cv_.wait_for(lock, std::chrono::milliseconds(poll_timeout_ms_),
                     [this]() { return stop_requested_ || sources_changed_; });
      }
    }
    sources_changed_ = false;
  }
}

std::shared_ptr<spdlog::logger> EbpfEventLoop::GetLogger() {
  if (!logger_)
    logger_ = spdlog::get("polycubed");
  return logger_;
}

}  // namespace polycubed
}  // namespace polycube

