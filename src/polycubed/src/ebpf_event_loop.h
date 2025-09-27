#pragma once

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <condition_variable>
#include <atomic>

#include <spdlog/spdlog.h>
#include <libbpf.h>

#include <api/BPF.h>

namespace polycube {
namespace polycubed {

class EbpfEventLoop {
 public:
  typedef std::function<void(void *, void *, int)> PerfCallback;
  typedef std::function<void(void *, void *, size_t)> RingCallback;

  static EbpfEventLoop &get_instance();

  // Starts the event loop thread. It is automatically invoked on register if
  // the loop is not running yet.
  void Start();
  void Stop();

  int RegisterPerfBuffer(int map_fd, const PerfCallback &callback,
                         void *cookie, size_t page_cnt = 64);

  int RegisterRingBuffer(int map_fd, const RingCallback &callback,
                         void *cookie);

  void Unregister(int handle);

 private:
  EbpfEventLoop();
  ~EbpfEventLoop();

  EbpfEventLoop(const EbpfEventLoop &) = delete;
  EbpfEventLoop &operator=(const EbpfEventLoop &) = delete;

  class EventSource {
   public:
    explicit EventSource(int id);
    virtual ~EventSource();
    virtual int Poll() = 0;
    int id() const;

   private:
    int id_;
  };

  class PerfEventSource : public EventSource {
   public:
    static std::shared_ptr<PerfEventSource> Create(
        int id, int map_fd, const PerfCallback &callback, void *cookie,
        size_t page_cnt, std::shared_ptr<spdlog::logger> logger, int *err);
    ~PerfEventSource() override;
    int Poll() override;

   private:
    PerfEventSource(int id, int map_fd, const PerfCallback &callback,
                    void *cookie, size_t page_cnt,
                    std::shared_ptr<spdlog::logger> logger, int *err);
    static void SampleTrampoline(void *ctx, int cpu, void *data, __u32 size);
    static void LostTrampoline(void *ctx, int cpu, __u64 lost);

    struct perf_buffer *buffer_;
    PerfCallback callback_;
    void *cookie_;
    std::shared_ptr<spdlog::logger> logger_;
  };

  class RingEventSource : public EventSource {
   public:
    static std::shared_ptr<RingEventSource> Create(
        int id, int map_fd, const RingCallback &callback, void *cookie,
        std::shared_ptr<spdlog::logger> logger, int *err);
    ~RingEventSource() override;
    int Poll() override;

   private:
    RingEventSource(int id, int map_fd, const RingCallback &callback,
                    void *cookie, std::shared_ptr<spdlog::logger> logger,
                    int *err);
    static int SampleTrampoline(void *ctx, void *data, size_t size);

    struct ring_buffer *buffer_;
    RingCallback callback_;
    void *cookie_;
    std::shared_ptr<spdlog::logger> logger_;
  };

  void Run();
  std::shared_ptr<spdlog::logger> GetLogger();

  std::atomic<int> next_id_;
  std::map<int, std::shared_ptr<EventSource>> sources_;
  std::mutex mutex_;
  std::condition_variable cv_;
  std::thread worker_;
  bool running_;
  bool stop_requested_;
  bool sources_changed_;
  int poll_timeout_ms_;
  std::shared_ptr<spdlog::logger> logger_;
};

}  // namespace polycubed
}  // namespace polycube
