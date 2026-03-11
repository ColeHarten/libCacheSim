#include <libCacheSim.h>

#include <iterator>
#include <list>
#include <unordered_map>

class LRUCache {
 private:
  uint64_t cache_size_;

  std::unordered_map<obj_id_t, std::list<obj_id_t>::iterator> pos_;
  std::list<obj_id_t> list_;

 public:
  LRUCache(uint64_t capacity) : cache_size_(capacity) {}

  void on_hit(obj_id_t id) {
    auto it = pos_.find(id);
    if (it == pos_.end()) {
      return;
    }
    list_.splice(list_.end(), list_, it->second);
  }

  void on_miss(obj_id_t id, uint64_t sz) {
    if (sz > cache_size_) return;

    auto existing = pos_.find(id);
    if (existing != pos_.end()) {
      list_.splice(list_.end(), list_, existing->second);
      return;
    }

    list_.push_back(id);
    pos_[id] = std::prev(list_.end());
  }

  obj_id_t evict() {
    if (list_.empty()) {
      return 0;
    }

    obj_id_t victim = list_.front();
    list_.pop_front();
    pos_.erase(victim);

    return victim;
  }

  void on_remove(obj_id_t id) {
    auto it = pos_.find(id);
    if (it == pos_.end()) {
      return;
    }
    list_.erase(it->second);
    pos_.erase(it);
  }
};

extern "C" {
void *cache_init_hook(const common_cache_params_t params) {
  return new LRUCache(params.cache_size);
}

void cache_hit_hook(void *data, const request_t *req) {
  static_cast<LRUCache *>(data)->on_hit(req->obj_id);
}

void cache_miss_hook(void *data, const request_t *req) {
  static_cast<LRUCache *>(data)->on_miss(req->obj_id, req->obj_size);
}

obj_id_t cache_eviction_hook(void *data, const request_t * /*req*/) {
  return static_cast<LRUCache *>(data)->evict();
}

void cache_remove_hook(void *data, obj_id_t obj_id) {
  static_cast<LRUCache *>(data)->on_remove(obj_id);
}

void cache_free_hook(void *data) {
  LRUCache *lru_cache = (LRUCache *)data;
  delete lru_cache;
}
}  // extern "C"
