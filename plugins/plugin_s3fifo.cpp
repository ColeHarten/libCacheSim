#include <libCacheSim.h>

#include <algorithm>
#include <cstdint>
#include <list>
#include <unordered_map>
#include <unordered_set>

class S3FifoCache {
 private:
  enum class QueueType : uint8_t { SMALL = 0, MAIN = 1 };

  struct Node {
    QueueType queue;
    uint8_t freq;
    uint64_t size;
  };

  struct Location {
    QueueType queue;
    std::list<obj_id_t>::iterator it;
  };

  uint64_t cache_size_;

  size_t small_target_;
  size_t main_target_;
  size_t ghost_target_;

  std::list<obj_id_t> small_q_;
  std::list<obj_id_t> main_q_;
  std::list<obj_id_t> ghost_q_;  // keys only

  std::unordered_map<obj_id_t, Node> meta_;
  std::unordered_map<obj_id_t, Location> where_;

  std::unordered_map<obj_id_t, std::list<obj_id_t>::iterator> ghost_pos_;

  static constexpr uint8_t kMaxFreq = 3;

  static uint8_t inc_freq(uint8_t x) {
    return x < kMaxFreq ? static_cast<uint8_t>(x + 1) : kMaxFreq;
  }

  void ghost_insert(obj_id_t id) {
    auto it = ghost_pos_.find(id);
    if (it != ghost_pos_.end()) {
      ghost_q_.erase(it->second);
      ghost_pos_.erase(it);
    }

    ghost_q_.push_back(id);
    ghost_pos_[id] = std::prev(ghost_q_.end());

    while (ghost_q_.size() > ghost_target_) {
      obj_id_t old = ghost_q_.front();
      ghost_q_.pop_front();
      ghost_pos_.erase(old);
    }
  }

  bool in_ghost(obj_id_t id) const {
    return ghost_pos_.find(id) != ghost_pos_.end();
  }

  void ghost_remove(obj_id_t id) {
    auto it = ghost_pos_.find(id);
    if (it == ghost_pos_.end()) return;
    ghost_q_.erase(it->second);
    ghost_pos_.erase(it);
  }

  void insert_small(obj_id_t id, uint64_t sz, uint8_t freq = 0) {
    small_q_.push_back(id);
    meta_[id] = Node{QueueType::SMALL, freq, sz};
    where_[id] = Location{QueueType::SMALL, std::prev(small_q_.end())};
  }

  void insert_main(obj_id_t id, uint64_t sz, uint8_t freq = 0) {
    main_q_.push_back(id);
    meta_[id] = Node{QueueType::MAIN, freq, sz};
    where_[id] = Location{QueueType::MAIN, std::prev(main_q_.end())};
  }

  void erase_resident(obj_id_t id) {
    auto wit = where_.find(id);
    if (wit == where_.end()) return;

    if (wit->second.queue == QueueType::SMALL) {
      small_q_.erase(wit->second.it);
    } else {
      main_q_.erase(wit->second.it);
    }

    where_.erase(wit);
    meta_.erase(id);
  }

  void ensure_small_bound() {
    while (small_q_.size() > small_target_) {
      obj_id_t id = small_q_.front();
      small_q_.pop_front();

      auto mit = meta_.find(id);
      if (mit == meta_.end()) continue;

      Node node = mit->second;
      where_.erase(id);
      meta_.erase(id);

      if (node.freq == 0) {
        ghost_insert(id);
      } else {
        insert_main(id, node.size, 0);
      }
    }
  }

  void ensure_main_bound() {
    while (main_q_.size() > main_target_) {
      obj_id_t id = main_q_.front();
      main_q_.pop_front();

      auto mit = meta_.find(id);
      if (mit == meta_.end()) continue;

      Node& node = mit->second;
      where_.erase(id);

      if (node.freq == 0) {
        meta_.erase(id);
        ghost_insert(id);
      } else {
        node.freq -= 1;
        main_q_.push_back(id);
        where_[id] = Location{QueueType::MAIN, std::prev(main_q_.end())};
      }
    }
  }

 public:
  explicit S3FifoCache(uint64_t capacity) : cache_size_(capacity) {
    // Tunable static sizes.
    // Small queue intentionally small; ghost moderate.
    constexpr size_t kResidentSlots = 100000;
    small_target_ = std::max<size_t>(1, kResidentSlots / 10);  // 10%
    main_target_ = kResidentSlots - small_target_;             // 90%
    ghost_target_ = kResidentSlots;  // 100% of resident keys
  }

  void on_hit(obj_id_t id) {
    auto mit = meta_.find(id);
    if (mit == meta_.end()) return;

    mit->second.freq = inc_freq(mit->second.freq);
  }

  void on_miss(obj_id_t id, uint64_t sz) {
    if (sz > cache_size_) return;

    // Defensive: if already resident, just treat as a hit.
    auto mit = meta_.find(id);
    if (mit != meta_.end()) {
      mit->second.freq = inc_freq(mit->second.freq);
      return;
    }

    // Ghost hit: bypass SMALL, admit directly to MAIN.
    if (in_ghost(id)) {
      ghost_remove(id);
      insert_main(id, sz, 0);
      ensure_main_bound();
      return;
    }

    // Normal miss: admit to SMALL.
    insert_small(id, sz, 0);
    ensure_small_bound();
    ensure_main_bound();
  }

  obj_id_t evict() {
    while (!small_q_.empty()) {
      obj_id_t id = small_q_.front();
      small_q_.pop_front();

      auto mit = meta_.find(id);
      if (mit == meta_.end()) continue;

      Node node = mit->second;
      where_.erase(id);
      meta_.erase(id);

      if (node.freq == 0) {
        ghost_insert(id);
        return id;
      } else {
        insert_main(id, node.size, 0);
        ensure_main_bound();
      }
    }

    while (!main_q_.empty()) {
      obj_id_t id = main_q_.front();
      main_q_.pop_front();

      auto mit = meta_.find(id);
      if (mit == meta_.end()) continue;

      Node& node = mit->second;
      where_.erase(id);

      if (node.freq == 0) {
        meta_.erase(id);
        ghost_insert(id);
        return id;
      } else {
        node.freq -= 1;
        main_q_.push_back(id);
        where_[id] = Location{QueueType::MAIN, std::prev(main_q_.end())};
      }
    }

    return 0;
  }

  void on_remove(obj_id_t id) {
    erase_resident(id);
    ghost_remove(id);
  }
};

extern "C" {

void* cache_init_hook(const common_cache_params_t params) {
  return new S3FifoCache(params.cache_size);
}

void cache_hit_hook(void* data, const request_t* req) {
  static_cast<S3FifoCache*>(data)->on_hit(req->obj_id);
}

void cache_miss_hook(void* data, const request_t* req) {
  static_cast<S3FifoCache*>(data)->on_miss(req->obj_id, req->obj_size);
}

obj_id_t cache_eviction_hook(void* data, const request_t* /*req*/) {
  return static_cast<S3FifoCache*>(data)->evict();
}

void cache_remove_hook(void* data, obj_id_t obj_id) {
  static_cast<S3FifoCache*>(data)->on_remove(obj_id);
}

void cache_free_hook(void* data) { delete static_cast<S3FifoCache*>(data); }

}  // extern "C"
