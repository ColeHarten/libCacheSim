#include <libCacheSim.h>

#include <chrono>
#include <random>
#include <unordered_map>
#include <vector>

class RRCache {
 private:
  uint64_t cache_size_;

  std::mt19937 engine;

  std::vector<obj_id_t> cache_;
  std::unordered_map<obj_id_t, size_t> pos_;

  inline void remove_at(size_t idx) {
    if (cache_.empty() || idx >= cache_.size()) {
      return;
    }

    const size_t last_idx = cache_.size() - 1;
    const obj_id_t removed_id = cache_[idx];

    if (idx != last_idx) {
      const obj_id_t moved_id = cache_[last_idx];
      cache_[idx] = moved_id;
      pos_[moved_id] = idx;
    }

    cache_.pop_back();
    pos_.erase(removed_id);
  }

 public:
  RRCache(uint64_t capacity)
      : cache_size_(capacity),
        engine(static_cast<unsigned int>(
            std::chrono::system_clock::now().time_since_epoch().count())) {}

  void on_hit(obj_id_t id) {}  // do nothing

  void on_miss(obj_id_t id, uint64_t sz) {
    if (sz > cache_size_) return;

    if (pos_.find(id) != pos_.end()) {
      return;
    }

    pos_[id] = cache_.size();
    cache_.push_back(id);
  }

  obj_id_t evict() {
    if (cache_.empty()) {
      return 0;
    }

    // Pick a uniformly random index and nerf it
    std::uniform_int_distribution<size_t> dist(0, cache_.size() - 1);

    const size_t victim_index = dist(engine);
    const obj_id_t victim_id = cache_[victim_index];

    remove_at(victim_index);

    return victim_id;
  }

  void on_remove(obj_id_t id) {
    auto it = pos_.find(id);
    if (it == pos_.end()) {
      return;
    }
    remove_at(it->second);
  }
};

extern "C" {
void *cache_init_hook(const common_cache_params_t params) {
  return new RRCache(params.cache_size);
}

void cache_hit_hook(void *data, const request_t *req) {
  static_cast<RRCache *>(data)->on_hit(req->obj_id);
}

void cache_miss_hook(void *data, const request_t *req) {
  static_cast<RRCache *>(data)->on_miss(req->obj_id, req->obj_size);
}

obj_id_t cache_eviction_hook(void *data, const request_t * /*req*/) {
  return static_cast<RRCache *>(data)->evict();
}

void cache_remove_hook(void *data, obj_id_t obj_id) {
  static_cast<RRCache *>(data)->on_remove(obj_id);
}

void cache_free_hook(void *data) {
  RRCache *rr_cache = (RRCache *)data;
  delete rr_cache;
}
}  // extern "C"
