#include <libCacheSim.h>

#include <cmath>
#include <cstdint>
#include <limits>
#include <queue>
#include <unordered_map>

class BeladyPredictorCache {
 private:
  struct History {
    uint64_t last_access = 0;
    double ewma_gap = 0.0;
    uint64_t seen_count = 0;
  };

  struct ResidentEntry {
    uint64_t size = 0;
    double predicted_next = 0.0;
    uint64_t version = 0;
  };

  struct HeapNode {
    double predicted_next;
    obj_id_t id;
    uint64_t version;

    bool operator<(const HeapNode& other) const {
      // Max-heap: larger predicted_next should be evicted first.
      return predicted_next < other.predicted_next;
    }
  };

  static constexpr double kAlpha = 0.70;
  static constexpr double kColdStartGap = 1e18;

  uint64_t cache_size_;
  uint64_t now_ = 0;

  std::unordered_map<obj_id_t, History> history_;
  std::unordered_map<obj_id_t, ResidentEntry> resident_;
  std::priority_queue<HeapNode> eviction_heap_;

  void touch_time() { ++now_; }

  void update_history(obj_id_t id) {
    History& h = history_[id];

    if (h.seen_count == 0) {
      h.last_access = now_;
      h.seen_count = 1;
      h.ewma_gap = 0.0;
      return;
    }

    double gap = static_cast<double>(now_ - h.last_access);

    if (h.seen_count == 1) {
      h.ewma_gap = gap;
    } else {
      h.ewma_gap = kAlpha * gap + (1.0 - kAlpha) * h.ewma_gap;
    }

    h.last_access = now_;
    h.seen_count += 1;
  }

  double predict_next(obj_id_t id) const {
    auto it = history_.find(id);
    if (it == history_.end()) {
      return kColdStartGap;
    }

    const History& h = it->second;
    if (h.seen_count <= 1) {
      // We have essentially no recurrence information yet.
      return static_cast<double>(now_) + kColdStartGap;
    }

    return static_cast<double>(now_) + h.ewma_gap;
  }

  void refresh_resident_prediction(obj_id_t id) {
    auto it = resident_.find(id);
    if (it == resident_.end()) {
      return;
    }

    ResidentEntry& e = it->second;
    e.version += 1;
    e.predicted_next = predict_next(id);
    eviction_heap_.push(HeapNode{e.predicted_next, id, e.version});
  }

  void discard_stale_heap_nodes() {
    while (!eviction_heap_.empty()) {
      const HeapNode top = eviction_heap_.top();
      auto it = resident_.find(top.id);
      if (it == resident_.end() || it->second.version != top.version) {
        eviction_heap_.pop();
        continue;
      }
      break;
    }
  }

 public:
  explicit BeladyPredictorCache(uint64_t capacity) : cache_size_(capacity) {}

  void on_hit(obj_id_t id) {
    touch_time();
    update_history(id);
    refresh_resident_prediction(id);
  }

  void on_miss(obj_id_t id, uint64_t sz) {
    touch_time();

    if (sz == 0 || sz > cache_size_) {
      // Object cannot reside in cache, but we still learn from the access.
      update_history(id);
      return;
    }

    update_history(id);

    auto it = resident_.find(id);
    if (it != resident_.end()) {
      // Defensive: if simulator state and plugin state briefly disagree,
      // just refresh the resident metadata.
      it->second.size = sz;
      refresh_resident_prediction(id);
      return;
    }

    ResidentEntry e;
    e.size = sz;
    e.version = 1;
    e.predicted_next = predict_next(id);

    resident_[id] = e;
    eviction_heap_.push(HeapNode{e.predicted_next, id, e.version});
  }

  obj_id_t evict() {
    discard_stale_heap_nodes();

    if (eviction_heap_.empty()) {
      return 0;
    }

    obj_id_t victim = eviction_heap_.top().id;
    eviction_heap_.pop();
    return victim;
  }

  void on_remove(obj_id_t id) {
    auto it = resident_.find(id);
    if (it == resident_.end()) {
      return;
    }
    resident_.erase(it);
    // Lazy heap cleanup handles stale nodes.
  }
};

extern "C" {

void* cache_init_hook(const common_cache_params_t params) {
  return new BeladyPredictorCache(params.cache_size);
}

void cache_hit_hook(void* data, const request_t* req) {
  static_cast<BeladyPredictorCache*>(data)->on_hit(req->obj_id);
}

void cache_miss_hook(void* data, const request_t* req) {
  static_cast<BeladyPredictorCache*>(data)->on_miss(req->obj_id, req->obj_size);
}

obj_id_t cache_eviction_hook(void* data, const request_t* /*req*/) {
  return static_cast<BeladyPredictorCache*>(data)->evict();
}

void cache_remove_hook(void* data, obj_id_t obj_id) {
  static_cast<BeladyPredictorCache*>(data)->on_remove(obj_id);
}

void cache_free_hook(void* data) {
  delete static_cast<BeladyPredictorCache*>(data);
}

}  // extern "C"
