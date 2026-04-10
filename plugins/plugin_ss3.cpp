#include <libCacheSim.h>

#include <algorithm>
#include <cstdint>
#include <list>
#include <unordered_map>

class S3SieveCache {
 private:
  enum Segment : uint8_t { Q1 = 0, Q2 = 1, Q3 = 2 };

  struct Entry {
    uint64_t size = 0;
    uint8_t seg = Q1;
    uint8_t visited = 0;
    uint8_t hits = 0;  // saturates at 2
    std::list<obj_id_t>::iterator it;
  };

  std::unordered_map<obj_id_t, Entry> entries_;

  // FIFO queues: newest at back, oldest at front.
  std::list<obj_id_t> q1_;
  std::list<obj_id_t> q2_;
  std::list<obj_id_t> q3_;

  uint64_t cache_size_;
  uint64_t used_bytes_ = 0;

  uint64_t q1_bytes_ = 0;
  uint64_t q2_bytes_ = 0;
  uint64_t q3_bytes_ = 0;

  uint64_t q1_target_;
  uint64_t q2_target_;
  uint64_t q3_target_;

  static constexpr uint8_t kMaxHits = 2;

  static uint64_t frac_bytes(uint64_t total, double frac) {
    return static_cast<uint64_t>(static_cast<double>(total) * frac);
  }

  void insert_back(obj_id_t id, Segment seg) {
    Entry &e = entries_[id];
    e.seg = seg;
    if (seg == Q1) {
      q1_.push_back(id);
      e.it = std::prev(q1_.end());
      q1_bytes_ += e.size;
    } else if (seg == Q2) {
      q2_.push_back(id);
      e.it = std::prev(q2_.end());
      q2_bytes_ += e.size;
    } else {
      q3_.push_back(id);
      e.it = std::prev(q3_.end());
      q3_bytes_ += e.size;
    }
  }

  void remove_from_queue(obj_id_t id) {
    auto mit = entries_.find(id);
    if (mit == entries_.end()) return;
    Entry &e = mit->second;

    if (e.seg == Q1) {
      q1_.erase(e.it);
      q1_bytes_ -= e.size;
    } else if (e.seg == Q2) {
      q2_.erase(e.it);
      q2_bytes_ -= e.size;
    } else {
      q3_.erase(e.it);
      q3_bytes_ -= e.size;
    }
  }

  obj_id_t erase_entry(obj_id_t id) {
    auto mit = entries_.find(id);
    if (mit == entries_.end()) return 0;

    uint64_t sz = mit->second.size;
    remove_from_queue(id);
    entries_.erase(mit);
    used_bytes_ -= sz;
    return id;
  }

  void move_to_back_same_queue(obj_id_t id) {
    auto mit = entries_.find(id);
    if (mit == entries_.end()) return;
    Entry &e = mit->second;

    if (e.seg == Q1) {
      q1_.splice(q1_.end(), q1_, e.it);
      e.it = std::prev(q1_.end());
    } else if (e.seg == Q2) {
      q2_.splice(q2_.end(), q2_, e.it);
      e.it = std::prev(q2_.end());
    } else {
      q3_.splice(q3_.end(), q3_, e.it);
      e.it = std::prev(q3_.end());
    }
  }

  void move_between_queues(obj_id_t id, Segment new_seg) {
    auto mit = entries_.find(id);
    if (mit == entries_.end()) return;

    remove_from_queue(id);
    mit->second.seg = new_seg;
    insert_back(id, new_seg);
  }

  // Oldest item in Q1 either dies or gets promoted.
  // Returns victim ID only if something is actually evicted.
  obj_id_t process_q1_overflow() {
    if (q1_bytes_ <= q1_target_ || q1_.empty()) return 0;

    obj_id_t id = q1_.front();
    auto mit = entries_.find(id);
    if (mit == entries_.end()) return 0;
    Entry &e = mit->second;

    if (e.visited || e.hits >= 1) {
      e.visited = 0;
      if (e.hits == 0) e.hits = 1;
      move_between_queues(id, Q2);
      return 0;
    }

    return erase_entry(id);
  }

  // SIEVE-style processing in Q2.
  // Oldest item gets one second chance; stronger items can graduate to Q3.
  obj_id_t process_q2_overflow() {
    if (q2_bytes_ <= q2_target_ || q2_.empty()) return 0;

    obj_id_t id = q2_.front();
    auto mit = entries_.find(id);
    if (mit == entries_.end()) return 0;
    Entry &e = mit->second;

    if (e.visited) {
      e.visited = 0;
      if (e.hits >= 2) {
        move_between_queues(id, Q3);
      } else {
        move_to_back_same_queue(id);
      }
      return 0;
    }

    if (e.hits >= 2) {
      move_between_queues(id, Q3);
      return 0;
    }

    return erase_entry(id);
  }

  // SIEVE-style processing in Q3.
  obj_id_t process_q3_overflow() {
    if (q3_bytes_ <= q3_target_ || q3_.empty()) return 0;

    obj_id_t id = q3_.front();
    auto mit = entries_.find(id);
    if (mit == entries_.end()) return 0;
    Entry &e = mit->second;

    if (e.visited) {
      e.visited = 0;
      move_to_back_same_queue(id);
      return 0;
    }

    return erase_entry(id);
  }

  obj_id_t force_global_eviction() {
    // Prefer evicting from lower-value regions first.
    if (!q1_.empty()) {
      obj_id_t id = q1_.front();
      auto mit = entries_.find(id);
      if (mit != entries_.end()) {
        Entry &e = mit->second;
        if (e.visited || e.hits >= 1) {
          e.visited = 0;
          if (e.hits == 0) e.hits = 1;
          move_between_queues(id, Q2);
        } else {
          return erase_entry(id);
        }
      }
    }

    if (!q2_.empty()) {
      obj_id_t id = q2_.front();
      auto mit = entries_.find(id);
      if (mit != entries_.end()) {
        Entry &e = mit->second;
        if (e.visited) {
          e.visited = 0;
          if (e.hits >= 2) {
            move_between_queues(id, Q3);
          } else {
            move_to_back_same_queue(id);
          }
        } else if (e.hits >= 2) {
          move_between_queues(id, Q3);
        } else {
          return erase_entry(id);
        }
      }
    }

    if (!q3_.empty()) {
      obj_id_t id = q3_.front();
      auto mit = entries_.find(id);
      if (mit != entries_.end()) {
        Entry &e = mit->second;
        if (e.visited) {
          e.visited = 0;
          move_to_back_same_queue(id);
        } else {
          return erase_entry(id);
        }
      }
    }

    return 0;
  }

 public:
  explicit S3SieveCache(uint64_t cache_size) : cache_size_(cache_size) {
    // Hyperparameters to tune:
    // q1_frac in {0.04, 0.08, 0.12, 0.16}
    // q2_frac in {0.16, 0.24, 0.32}
    // q3 is the remainder
    const double q1_frac = 0.08;
    const double q2_frac = 0.24;

    q1_target_ = std::max<uint64_t>(1, frac_bytes(cache_size_, q1_frac));
    q2_target_ = std::max<uint64_t>(1, frac_bytes(cache_size_, q2_frac));
    q3_target_ = cache_size_ - q1_target_ - q2_target_;

    if (q3_target_ == 0) q3_target_ = 1;
  }

  void on_hit(obj_id_t id) {
    auto mit = entries_.find(id);
    if (mit == entries_.end()) return;

    Entry &e = mit->second;
    e.visited = 1;
    if (e.hits < kMaxHits) {
      ++e.hits;
    }
  }

  void on_miss(obj_id_t id, uint64_t size) {
    if (size > cache_size_) return;
    if (entries_.find(id) != entries_.end()) return;

    Entry e;
    e.size = size;
    e.seg = Q1;
    e.visited = 0;
    e.hits = 0;
    entries_[id] = e;
    used_bytes_ += size;
    insert_back(id, Q1);
  }

  obj_id_t evict() {
    // First, try to restore queue-local targets without evicting if possible.
    while (true) {
      bool progressed = false;
      obj_id_t victim = 0;

      if (q1_bytes_ > q1_target_) {
        victim = process_q1_overflow();
        if (victim != 0) return victim;
        progressed = true;
      }

      if (q2_bytes_ > q2_target_) {
        victim = process_q2_overflow();
        if (victim != 0) return victim;
        progressed = true;
      }

      if (q3_bytes_ > q3_target_) {
        victim = process_q3_overflow();
        if (victim != 0) return victim;
        progressed = true;
      }

      if (!progressed) break;
    }

    // If total bytes are still over capacity, force a global eviction.
    while (used_bytes_ > cache_size_) {
      obj_id_t victim = force_global_eviction();
      if (victim != 0) return victim;

      // Defensive fallback: hard-evict oldest from any non-empty queue.
      if (!q1_.empty()) return erase_entry(q1_.front());
      if (!q2_.empty()) return erase_entry(q2_.front());
      if (!q3_.empty()) return erase_entry(q3_.front());
      return 0;
    }

    // Defensive behavior if simulator asks for eviction when not strictly
    // needed.
    if (!q1_.empty()) return erase_entry(q1_.front());
    if (!q2_.empty()) return erase_entry(q2_.front());
    if (!q3_.empty()) return erase_entry(q3_.front());
    return 0;
  }

  void on_remove(obj_id_t id) {
    auto mit = entries_.find(id);
    if (mit == entries_.end()) return;
    erase_entry(id);
  }
};

extern "C" {

void *cache_init_hook(const common_cache_params_t params) {
  return new S3SieveCache(params.cache_size);
}

void cache_hit_hook(void *data, const request_t *req) {
  static_cast<S3SieveCache *>(data)->on_hit(req->obj_id);
}

void cache_miss_hook(void *data, const request_t *req) {
  static_cast<S3SieveCache *>(data)->on_miss(req->obj_id, req->obj_size);
}

obj_id_t cache_eviction_hook(void *data, const request_t * /*req*/) {
  return static_cast<S3SieveCache *>(data)->evict();
}

void cache_remove_hook(void *data, obj_id_t obj_id) {
  static_cast<S3SieveCache *>(data)->on_remove(obj_id);
}

void cache_free_hook(void *data) { delete static_cast<S3SieveCache *>(data); }

}  // extern "C"
