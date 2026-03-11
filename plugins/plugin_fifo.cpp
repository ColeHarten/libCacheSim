#include <libCacheSim.h>

#include <deque>

class FifoCache {
 private:
  std::deque<obj_id_t> queue_;
  uint64_t cache_size_;

 public:
  FifoCache(uint64_t cache_size) : cache_size_(cache_size) {}

  void on_hit(obj_id_t id) {}

  void on_miss(obj_id_t id, uint64_t size) {
    if (size <= cache_size_) {
      queue_.push_back(id);
    }
  }

  obj_id_t evict() {
    if (queue_.empty()) {
      return 0;
    }
    obj_id_t victim = queue_.front();
    queue_.pop_front();
    return victim;
  }

  void on_remove(obj_id_t id) {
    for (auto it = queue_.begin(); it != queue_.end(); ++it) {
      if (*it == id) {
        queue_.erase(it);
        break;
      }
    }
  }
};

extern "C" {
void *cache_init_hook(const common_cache_params_t params) {
  return new FifoCache(params.cache_size);
}

void cache_hit_hook(void *data, const request_t *req) {
  static_cast<FifoCache *>(data)->on_hit(req->obj_id);
}

void cache_miss_hook(void *data, const request_t *req) {
  static_cast<FifoCache *>(data)->on_miss(req->obj_id, req->obj_size);
}

obj_id_t cache_eviction_hook(void *data, const request_t * /*req*/) {
  return static_cast<FifoCache *>(data)->evict();
}

void cache_remove_hook(void *data, obj_id_t obj_id) {
  static_cast<FifoCache *>(data)->on_remove(obj_id);
}

void cache_free_hook(void *data) {
  FifoCache *fifo_cache = (FifoCache *)data;
  delete fifo_cache;
}
}  // extern "C"
