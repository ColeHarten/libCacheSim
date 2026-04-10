#include <libCacheSim.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <queue>
#include <unordered_map>

class HyperbolicCache {
 private:
  struct Entry {
    uint64_t size = 0;
    uint64_t last_access = 0;
    uint32_t freq = 0;
    uint64_t version = 0;
    double score = 0.0;
  };

  struct HeapNode {
    double score;
    obj_id_t id;
    uint64_t version;

    bool operator<(const HeapNode& other) const {
      return score > other.score;
    }
  };

  uint64_t cache_size_;
  uint64_t now_ = 0;

  std::unordered_map<obj_id_t, Entry> resident_;
  std::priority_queue<HeapNode> victim_heap_;

  static constexpr double kEps = 1e-12;

  void tick() { ++now_; }

  static double compute_score(uint32_t freq, uint64_t size, uint64_t age) {
    if (freq == 0) freq = 1;
    if (size == 0) size = 1;
    if (age == 0) age = 1;

    return static_cast<double>(freq) /
           (static_cast<double>(size) * static_cast<double>(age));
  }

  double current_score(const Entry& e) const {
    uint64_t age = (now_ >= e.last_access) ? (now_ - e.last_access + 1) : 1;
    return compute_score(e.freq, e.size, age);
  }

  void push_heap(obj_id_t id, const Entry& e) {
    victim_heap_.push(HeapNode{e.score, id, e.version});
  }

  void discard_stale_top() {
    while (!victim_heap_.empty()) {
      const HeapNode top = victim_heap_.top();
      auto it = resident_.find(top.id);
      if (it == resident_.end() || it->second.version != top.version) {
        victim_heap_.pop();
        continue;
      }

      // Recompute lazily because age changes over time.
      double fresh_score = current_score(it->second);
      if (std::abs(fresh_score - top.score) > kEps) {
        victim_heap_.pop();
        it->second.version += 1;
        it->second.score = fresh_score;
        push_heap(top.id, it->second);
        continue;
      }

      break;
    }
  }

 public:
  explicit HyperbolicCache(uint64_t capacity) : cache_size_(capacity) {}

  void on_hit(obj_id_t id) {
    tick();

    auto it = resident_.find(id);
    if (it == resident_.end()) {
      return;
    }

    Entry& e = it->second;
    if (e.freq != std::numeric_limits<uint32_t>::max()) {
      ++e.freq;
    }
    e.last_access = now_;
    e.version += 1;
    e.score = current_score(e);
    push_heap(id, e);
  }

  void on_miss(obj_id_t id, uint64_t sz) {
    tick();

    if (sz == 0 || sz > cache_size_) {
      return;
    }

    auto it = resident_.find(id);
    if (it != resident_.end()) {
      // Defensive only.
      Entry& e = it->second;
      if (e.freq != std::numeric_limits<uint32_t>::max()) {
        ++e.freq;
      }
      e.last_access = now_;
      e.version += 1;
      e.score = current_score(e);
      push_heap(id, e);
      return;
    }

    Entry e;
    e.size = sz;
    e.last_access = now_;
    e.freq = 1;
    e.version = 1;
    e.score = current_score(e);

    resident_[id] = e;
    push_heap(id, e);
  }

  obj_id_t evict() {
    discard_stale_top();

    if (victim_heap_.empty()) {
      return 0;
    }

    obj_id_t victim = victim_heap_.top().id;
    victim_heap_.pop();
    return victim;
  }

  void on_remove(obj_id_t id) {
    auto it = resident_.find(id);
    if (it == resident_.end()) {
      return;
    }
    resident_.erase(it);
  }
};

extern "C" {

void* cache_init_hook(const common_cache_params_t params) {
  return new HyperbolicCache(params.cache_size);
}

void cache_hit_hook(void* data, const request_t* req) {
  static_cast<HyperbolicCache*>(data)->on_hit(req->obj_id);
}

void cache_miss_hook(void* data, const request_t* req) {
  static_cast<HyperbolicCache*>(data)->on_miss(req->obj_id, req->obj_size);
}

obj_id_t cache_eviction_hook(void* data, const request_t* /*req*/) {
  return static_cast<HyperbolicCache*>(data)->evict();
}

void cache_remove_hook(void* data, obj_id_t obj_id) {
  static_cast<HyperbolicCache*>(data)->on_remove(obj_id);
}

void cache_free_hook(void* data) { delete static_cast<HyperbolicCache*>(data); }

}  // extern "C"
