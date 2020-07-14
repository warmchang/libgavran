#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include "errors.h"

// tag::defer[]
// <1>
struct cancel_defer {
  void** target;
  size_t* cancelled;
};

#define CONCAT_(x, y) x##y
#define CONCAT(x, y) CONCAT_(x, y)

// <2>
#define try_defer(func, var, cancel_marker)                  \
  struct cancel_defer CONCAT(defer_, __COUNTER__)            \
      __attribute__((unused, __cleanup__(defer_##func))) = { \
          .target = (void*)&var, .cancelled = &cancel_marker};

// <3>
#define defer(func, var)                                          \
  struct cancel_defer CONCAT(defer_, __COUNTER__) __attribute__(( \
      unused, __cleanup__(defer_##func))) = {.target = (void*)&var};

// <4>
#define enable_defer(func) enable_defer_imp(func, 0, (void*), "%p")

#define enable_defer_imp(func, failcode, convert, format)    \
  static inline void defer_##func(struct cancel_defer* cd) { \
    if (cd->cancelled && *cd->cancelled) return;             \
    if (func(convert(cd->target)) == failcode) {             \
      errors_push(EINVAL, msg("Failure on " #func),          \
                  with(convert(*cd->target), format));       \
    }                                                        \
  }                                                          \
  void enable_semicolon_after_macro_##__LINE__(void)

// end::defer[]

// tag::defer_free[]
static inline void defer_free(struct cancel_defer* cd) {
  if (cd->cancelled && *cd->cancelled) return;
  free(*cd->target);
}
// end::defer_free[]

result_t allocate(void** buffer, size_t size);

result_t allocate_clear(void** buffer, size_t size);

result_t reallocate(void** buffer, size_t new_size);
