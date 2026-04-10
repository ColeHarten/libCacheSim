#include <libCacheSim.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <list>
#include <unordered_map>
#include <vector>

class WTinyLFUCache {
 private:
  enum class Segment : uint8_t {
    WINDOW = 0,
    PROBATION = 1,
    PROTECTED = 2,
  };

  struct Entry {
    uint64_t size = 0;
    Segment seg = Segment::WINDOW;
    std::list<obj_id_t>::iterator it;
  };

  // ----------------------------
  // TinyLFU Count-Min Sketch
  // ----------------------------
  class CountMinSketch {
   private:
    static constexpr int kDepth = 4;
    size_t width_;
    std::vector<std::vector<uint16_t>> table_;
    uint64_t updates_ = 0;
    uint64_t reset_interval_;

    static uint64_t mix(uint64_t x) {
      x ^= x >> 33;
      x *= 0xff51afd7ed558ccdULL;
      x ^= x >> 33;
      x *= 0xc4ceb9fe1a85ec53ULL;
      x ^= x >> 33;
      return x;
    }

    size_t index(obj_id_t id, int row) const {
      uint64_t h = mix(static_cast<uint64_t>(id) + 0x9e3779b97f4a7c15ULL * row);
      return static_cast<size_t>(h % width_);
    }

    void age() {
      for (int r = 0; r < kDepth; ++r) {
        for (size_t c = 0; c < width_; ++c) {
          table_[r][c] >>= 1;
        }
      }
      updates_ = 0;
    }

   public:
    explicit CountMinSketch(size_t width = 1 << 15,
                            uint64_t reset_interval = 1 << 20)
        : width_(width),
          table_(kDepth, std::vector<uint16_t>(width_, 0)),
          reset_interval_(reset_interval) {}

    void increment(obj_id_t id) {
      for (int r = 0; r < kDepth; ++r) {
        uint16_t& cell = table_[r][index(id, r)];
        if (cell != UINT16_MAX) {
          ++cell;
        }
      }
      ++updates_;
      if (updates_ >= reset_interval_) {
        age();
      }
    }

    uint32_t estimate(obj_id_t id) const {
      uint32_t ans = UINT32_MAX;
      for (int r = 0; r < kDepth; ++r) {
        ans = std::min<uint32_t>(ans, table_[r][index(id, r)]);
      }
      return ans;
    }
  };

  // ----------------------------
  // Cache metadata
  // ----------------------------
  uint64_t cache_size_;

  uint64_t used_bytes_ = 0;

  uint64_t window_target_;
  uint64_t main_target_;
  uint64_t protected_target_;

  uint64_t window_bytes_ = 0;
  uint64_t probation_bytes_ = 0;
  uint64_t protected_bytes_ = 0;

  std::list<obj_id_t> window_;     // MRU at front, LRU at back
  std::list<obj_id_t> probation_;  // MRU at front, LRU at back
  std::list<obj_id_t> protected_;  // MRU at front, LRU at back

  std::unordered_map<obj_id_t, Entry> entries_;
  CountMinSketch sketch_;

  // Tune this: higher alpha penalizes large objects more.
  static constexpr double kSizeAlpha = 0.5;

  static constexpr double kWindowFrac = 0.02;     // 2% window
  static constexpr double kProtectedFrac = 0.80;  // 80% of main is protected

  // ----------------------------
  // Helpers
  // ----------------------------
  double score(obj_id_t id, uint64_t size) const {
    uint32_t freq = sketch_.estimate(id);
    double denom =
        std::pow(static_cast<double>(std::max<uint64_t>(1, size)), kSizeAlpha);
    return static_cast<double>(freq) / denom;
  }

  void move_to_front(std::list<obj_id_t>& lst,
                     std::list<obj_id_t>::iterator it) {
    if (it != lst.begin()) {
      lst.splice(lst.begin(), lst, it);
    }
  }

  void insert_front(obj_id_t id, Segment seg) {
    Entry& e = entries_[id];
    e.seg = seg;

    if (seg == Segment::WINDOW) {
      window_.push_front(id);
      e.it = window_.begin();
      window_bytes_ += e.size;
    } else if (seg == Segment::PROBATION) {
      probation_.push_front(id);
      e.it = probation_.begin();
      probation_bytes_ += e.size;
    } else {
      protected_.push_front(id);
      e.it = protected_.begin();
      protected_bytes_ += e.size;
    }
  }

  void remove_from_segment(obj_id_t id) {
    auto it = entries_.find(id);
    if (it == entries_.end()) return;

    Entry& e = it->second;
    if (e.seg == Segment::WINDOW) {
      window_.erase(e.it);
      window_bytes_ -= e.size;
    } else if (e.seg == Segment::PROBATION) {
      probation_.erase(e.it);
      probation_bytes_ -= e.size;
    } else {
      protected_.erase(e.it);
      protected_bytes_ -= e.size;
    }
  }

  void move_segment(obj_id_t id, Segment new_seg) {
    auto it = entries_.find(id);
    if (it == entries_.end()) return;
    remove_from_segment(id);
    it->second.seg = new_seg;
    insert_front(id, new_seg);
  }

  obj_id_t erase_entry(obj_id_t id) {
    auto it = entries_.find(id);
    if (it == entries_.end()) return 0;

    uint64_t sz = it->second.size;
    remove_from_segment(id);
    entries_.erase(it);
    used_bytes_ -= sz;
    return id;
  }

  void ensure_protected_limit() {
    while (protected_bytes_ > protected_target_ && !protected_.empty()) {
      obj_id_t demote = protected_.back();
      move_segment(demote, Segment::PROBATION);
    }
  }

  bool admit_candidate_over_victim(obj_id_t cand_id, uint64_t cand_size,
                                   obj_id_t victim_id,
                                   uint64_t victim_size) const {
    double cand_score = score(cand_id, cand_size);
    double victim_score = score(victim_id, victim_size);

    // Slight bias toward incumbents to reduce churn.
    return cand_score >= victim_score * 0.95;
  }

  obj_id_t evict_probation_lru() {
    if (probation_.empty()) return 0;
    obj_id_t victim = probation_.back();
    return erase_entry(victim);
  }

  obj_id_t evict_window_lru() {
    if (window_.empty()) return 0;
    obj_id_t victim = window_.back();
    return erase_entry(victim);
  }

  obj_id_t evict_protected_lru() {
    if (protected_.empty()) return 0;
    obj_id_t victim = protected_.back();
    return erase_entry(victim);
  }

 public:
  explicit WTinyLFUCache(uint64_t cache_size)
      : cache_size_(cache_size),
        window_target_(std::max<uint64_t>(
            1, static_cast<uint64_t>(cache_size * kWindowFrac))),
        main_target_(cache_size - window_target_),
        protected_target_(static_cast<uint64_t>(main_target_ * kProtectedFrac)),
        sketch_(1 << 15, 1 << 20) {}

  void on_hit(obj_id_t id) {
    sketch_.increment(id);

    auto it = entries_.find(id);
    if (it == entries_.end()) return;

    Entry& e = it->second;
    if (e.seg == Segment::WINDOW) {
      move_to_front(window_, e.it);
      e.it = window_.begin();
    } else if (e.seg == Segment::PROBATION) {
      // Probation hit => promote to protected
      move_segment(id, Segment::PROTECTED);
      ensure_protected_limit();
    } else {
      move_to_front(protected_, e.it);
      e.it = protected_.begin();
    }
  }

  void on_miss(obj_id_t id, uint64_t size) {
    sketch_.increment(id);

    if (size > cache_size_) {
      return;
    }

    // If simulator somehow reports a miss for something already present,
    // ignore.
    if (entries_.find(id) != entries_.end()) {
      return;
    }

    Entry e;
    e.size = size;
    entries_[id] = e;
    used_bytes_ += size;

    // New objects always enter the window.
    insert_front(id, Segment::WINDOW);
  }

  obj_id_t evict() {
    // First keep protected within its target.
    ensure_protected_limit();

    // If the window is too large, its LRU candidate tries to enter probation.
    while (window_bytes_ > window_target_ && !window_.empty()) {
      obj_id_t cand = window_.back();
      auto eit = entries_.find(cand);
      if (eit == entries_.end()) {
        break;
      }
      uint64_t cand_size = eit->second.size;

      // Remove from window for decision.
      remove_from_segment(cand);

      // If main has spare room and total cache size is okay, admit candidate
      // directly.
      if ((probation_bytes_ + protected_bytes_ + cand_size <= main_target_) &&
          (used_bytes_ <= cache_size_)) {
        insert_front(cand, Segment::PROBATION);
        continue;
      }

      // If probation is empty, either admit and evict later if needed, or
      // reject if overfull.
      if (probation_.empty()) {
        insert_front(cand, Segment::PROBATION);
        break;
      }

      obj_id_t victim = probation_.back();
      auto vit = entries_.find(victim);
      if (vit == entries_.end()) {
        break;
      }
      uint64_t victim_size = vit->second.size;

      if (admit_candidate_over_victim(cand, cand_size, victim, victim_size)) {
        insert_front(cand, Segment::PROBATION);
        return erase_entry(victim);
      } else {
        // Reject candidate entirely.
        entries_.erase(cand);
        used_bytes_ -= cand_size;
        return cand;
      }
    }

    // If still over capacity, evict in this order:
    // 1. probation LRU
    // 2. window LRU
    // 3. protected LRU
    if (used_bytes_ > cache_size_) {
      if (!probation_.empty()) return evict_probation_lru();
      if (!window_.empty()) return evict_window_lru();
      if (!protected_.empty()) return evict_protected_lru();
    }

    // Defensive fallback in case simulator calls eviction when not strictly
    // needed.
    if (!probation_.empty()) return evict_probation_lru();
    if (!window_.empty()) return evict_window_lru();
    if (!protected_.empty()) return evict_protected_lru();
    return 0;
  }

  void on_remove(obj_id_t id) {
    auto it = entries_.find(id);
    if (it == entries_.end()) return;

    remove_from_segment(id);
    used_bytes_ -= it->second.size;
    entries_.erase(it);
  }
};

extern "C" {

void* cache_init_hook(const common_cache_params_t params) {
  return new WTinyLFUCache(params.cache_size);
}

void cache_hit_hook(void* data, const request_t* req) {
  static_cast<WTinyLFUCache*>(data)->on_hit(req->obj_id);
}

void cache_miss_hook(void* data, const request_t* req) {
  static_cast<WTinyLFUCache*>(data)->on_miss(req->obj_id, req->obj_size);
}

obj_id_t cache_eviction_hook(void* data, const request_t* /*req*/) {
  return static_cast<WTinyLFUCache*>(data)->evict();
}

void cache_remove_hook(void* data, obj_id_t obj_id) {
  static_cast<WTinyLFUCache*>(data)->on_remove(obj_id);
}

void cache_free_hook(void* data) { delete static_cast<WTinyLFUCache*>(data); }

}  // extern "C"
