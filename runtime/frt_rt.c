/* ================================================================ fsht-03:03
 * registry 10.03.02 (ch. 25 §1): the runtime UMBRELLA.
 *
 * runtime/
 *     common/     target-independent core  (this file + frt_common.c)
 *     platforms/  linux/ windows/ wasm/ baremetal/
 *
 * The platform layer is selected from the COMPILE's target macros — the
 * driver compiles this file WITH the program's --target triple, so the
 * macro set selects the layer for the exact target (same-target
 * compilation, ch. 25 §1 MUST):
 *     - FRT_PLATFORM=<name>      explicit override (baremetal use)
 *     - _WIN32 / __CYGWIN__      the windows layer
 *     - __wasm__                 the wasm layer (wasi or standalone)
 *     - otherwise                the linux layer (pthreads/POSIX)
 * The runtime cache key includes the triple (see main.cpp).
 */
#if defined(FRT_PLATFORM_LINUX)
  #include "platforms/linux/frt_platform.c"
#elif defined(FRT_PLATFORM_WINDOWS)
  #include "platforms/windows/frt_platform.c"
#elif defined(FRT_PLATFORM_WASM)
  #include "platforms/wasm/frt_platform.c"
#elif defined(FRT_PLATFORM_BAREMETAL)
  #include "platforms/baremetal/frt_platform.c"
#elif defined(_WIN32) || defined(__CYGWIN__)
  #include "platforms/windows/frt_platform.c"
#elif defined(__wasm__)
  #include "platforms/wasm/frt_platform.c"
#else
  #include "platforms/linux/frt_platform.c"
#endif

#include "common/frt_common.c"
