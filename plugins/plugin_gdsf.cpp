#include <libCacheSim.h>

#include <cstdint>
#include <queue>
#include <unordered_map>
#include <vector>

class GDSFCache {
 private:
  struct Entry {
    uint64_t size;
    uint64_t freq;
    double priority;
    uint64_t version;
  };

  struct HeapNode {
    double priority;
    obj_id_t id;
    uint64_t version;

    bool operator<(const HeapNode& other) const {
      return priority > other.priority;
    }
  };

  uint64_t cache_size_;
  double aging_clock_ = 0.0;

  std::unordered_map<obj_id_t, Entry> entries_;
  std::priority_queue<HeapNode> min_heap_;

  static double score(double aging_clock, uint64_t freq, uint64_t size) {
    return aging_clock + static_cast<double>(freq) / static_cast<double>(size);
  }

  void push_heap(obj_id_t id, const Entry& e) {
    min_heap_.push(HeapNode{e.priority, id, e.version});
  }

  void discard_stale_heap_top() {
    while (!min_heap_.empty()) {
      const HeapNode top = min_heap_.top();
      auto it = entries_.find(top.id);
      if (it == entries_.end() || it->second.version != top.version) {
        min_heap_.pop();
        continue;
      }
      break;
    }
  }

 public:
  explicit GDSFCache(uint64_t capacity) : cache_size_(capacity) {}

  void on_hit(obj_id_t id) {
    auto it = entries_.find(id);
    if (it == entries_.end()) {
      return;
    }

    Entry& e = it->second;
    e.freq += 1;
    e.version += 1;
    e.priority = score(aging_clock_, e.freq, e.size);
    push_heap(id, e);
  }

  void on_miss(obj_id_t id, uint64_t size) {
    if (size == 0 || size > cache_size_) {
      return;
    }

    auto it = entries_.find(id);
    if (it != entries_.end()) {
      Entry& e = it->second;
      e.freq += 1;
      e.version += 1;
      e.priority = score(aging_clock_, e.freq, e.size);
      push_heap(id, e);
      return;
    }

    Entry e;
    e.size = size;
    e.freq = 1;
    e.version = 1;
    e.priority = score(aging_clock_, e.freq, e.size);

    entries_[id] = e;
    push_heap(id, e);
  }

  obj_id_t evict() {
    discard_stale_heap_top();

    if (min_heap_.empty()) {
      return 0;
    }

    HeapNode victim = min_heap_.top();
    min_heap_.pop();

    auto it = entries_.find(victim.id);
    if (it == entries_.end()) {
      return 0;
    }

    aging_clock_ = it->second.priority;

    return victim.id;
  }

  void on_remove(obj_id_t id) {
    auto it = entries_.find(id);
    if (it == entries_.end()) {
      return;
    }
    entries_.erase(it);
  }
};

extern "C" {

void* cache_init_hook(const common_cache_params_t params) {
  return new GDSFCache(params.cache_size);
}

void cache_hit_hook(void* data, const request_t* req) {
  static_cast<GDSFCache*>(data)->on_hit(req->obj_id);
}

void cache_miss_hook(void* data, const request_t* req) {
  static_cast<GDSFCache*>(data)->on_miss(req->obj_id, req->obj_size);
}

obj_id_t cache_eviction_hook(void* data, const request_t* /*req*/) {
  return static_cast<GDSFCache*>(data)->evict();
}

void cache_remove_hook(void* data, obj_id_t obj_id) {
  static_cast<GDSFCache*>(data)->on_remove(obj_id);
}

void cache_free_hook(void* data) { delete static_cast<GDSFCache*>(data); }

}  // extern "C"
