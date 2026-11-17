#pragma once

// Compatibility shim for Android NDK libc++, which does not ship
// std::atomic_ref even in NDK r28c (Clang 19). ReXGlue v0.11's rex/types.h
// uses std::atomic_ref<T>::load/store/required_alignment/is_always_lock_free.
//
// Force-include this file with -include for the generated-code Android build
// and define BS_NEED_ATOMIC_REF_SHIM=1. Host builds with modern libstdc++/
// libc++ already provide std::atomic_ref and must NOT define the macro.

#ifdef BS_NEED_ATOMIC_REF_SHIM

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace std {

template <typename T>
class atomic_ref {
  static_assert(std::is_trivially_copyable_v<T>);
  static_assert(sizeof(T) == 1 || sizeof(T) == 2 || sizeof(T) == 4 || sizeof(T) == 8);

 public:
  using value_type = T;
  static constexpr std::size_t required_alignment = alignof(T);
  static constexpr bool is_always_lock_free = true;

  explicit atomic_ref(T& ref) noexcept : ptr_(&ref) {}
  atomic_ref(const atomic_ref&) = default;
  atomic_ref& operator=(const atomic_ref&) = delete;

  T load(std::memory_order order = std::memory_order_seq_cst) const noexcept {
    return __atomic_load_n(ptr_, to_gcc_order(order));
  }

  void store(T desired, std::memory_order order = std::memory_order_seq_cst) const noexcept {
    __atomic_store_n(ptr_, desired, to_gcc_order(order));
  }

  T exchange(T desired, std::memory_order order = std::memory_order_seq_cst) const noexcept {
    return __atomic_exchange_n(ptr_, desired, to_gcc_order(order));
  }

  bool compare_exchange_weak(T& expected, T desired,
                             std::memory_order success = std::memory_order_seq_cst,
                             std::memory_order failure = std::memory_order_seq_cst) const noexcept {
    return __atomic_compare_exchange_n(ptr_, &expected, desired, true,
                                       to_gcc_order(success), to_gcc_order(failure));
  }

 private:
  static constexpr int to_gcc_order(std::memory_order order) noexcept {
    switch (order) {
      case std::memory_order_relaxed: return __ATOMIC_RELAXED;
      case std::memory_order_consume: return __ATOMIC_CONSUME;
      case std::memory_order_acquire: return __ATOMIC_ACQUIRE;
      case std::memory_order_release: return __ATOMIC_RELEASE;
      case std::memory_order_acq_rel: return __ATOMIC_ACQ_REL;
      case std::memory_order_seq_cst:
      default: return __ATOMIC_SEQ_CST;
    }
  }

  T* ptr_;
};

}  // namespace std

// Android libc++ also omits the public <stop_token>/jthread surface that
// ReXGlue's rex/platform/thread.h needs. Provide a small, self-contained
// subset: stop_token/stop_source plus a jthread that requests stop and joins
// on destruction. This is only active when the forced-include compat macro is
// set, i.e. for Android generated-code builds.
#include <functional>
#include <memory>
#include <thread>
#include <tuple>
#include <type_traits>
#include <utility>

namespace std {

class stop_source;

class stop_token {
 public:
  stop_token() = default;
  [[nodiscard]] bool stop_requested() const noexcept {
    return state_ && state_->load(std::memory_order_acquire);
  }
  [[nodiscard]] bool stop_possible() const noexcept { return state_ != nullptr; }

 private:
  friend class stop_source;
  explicit stop_token(std::atomic<bool>* state) noexcept : state_(state) {}
  std::atomic<bool>* state_{nullptr};
};

class stop_source {
 public:
  stop_source() : state_(std::make_shared<std::atomic<bool>>(false)) {}
  [[nodiscard]] stop_token get_token() const noexcept {
    return stop_token(state_.get());
  }
  bool request_stop() noexcept {
    bool expected = false;
    return state_->compare_exchange_strong(expected, true, std::memory_order_acq_rel);
  }
  [[nodiscard]] bool stop_requested() const noexcept {
    return state_->load(std::memory_order_acquire);
  }

 private:
  std::shared_ptr<std::atomic<bool>> state_;
};

class jthread {
 public:
  jthread() noexcept = default;

  template <typename F, typename... Args,
            typename = std::enable_if_t<std::is_invocable_v<std::decay_t<F>, stop_token, Args...> ||
                                        std::is_invocable_v<std::decay_t<F>, Args...>>>
  explicit jthread(F&& func, Args&&... args)
      : source_(), thread_(make_body(std::forward<F>(func), std::forward<Args>(args)...)) {}

  jthread(const jthread&) = delete;
  jthread& operator=(const jthread&) = delete;
  jthread(jthread&&) noexcept = default;
  jthread& operator=(jthread&&) noexcept = default;

  ~jthread() {
    if (thread_.joinable()) {
      source_.request_stop();
      thread_.join();
    }
  }

  void join() { thread_.join(); }
  [[nodiscard]] bool joinable() const noexcept { return thread_.joinable(); }
  void detach() { thread_.detach(); }
  [[nodiscard]] std::thread::id get_id() const noexcept { return thread_.get_id(); }
  stop_source get_stop_source() noexcept { return source_; }
  stop_token get_stop_token() noexcept { return source_.get_token(); }
  bool request_stop() noexcept { return source_.request_stop(); }

 private:
  template <typename F, typename... Args>
  std::thread make_body(F&& func, Args&&... args) {
    stop_token token = source_.get_token();
    if constexpr (std::is_invocable_v<std::decay_t<F>, stop_token, Args...>) {
      return std::thread([fn = std::forward<F>(func), tok = token,
                          tup = std::make_tuple(std::forward<Args>(args)...)]() mutable {
        std::apply(fn, std::tuple_cat(std::make_tuple(tok), tup));
      });
    } else {
      return std::thread([fn = std::forward<F>(func),
                          tup = std::make_tuple(std::forward<Args>(args)...)]() mutable {
        std::apply(fn, tup);
      });
    }
  }

  stop_source source_;
  std::thread thread_;
};

}  // namespace std

#endif  // BS_NEED_ATOMIC_REF_SHIM
