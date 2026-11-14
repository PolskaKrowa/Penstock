# Installing Penstock

Penstock can be installed into a system prefix or a user-local prefix and
used from shell scripts, IDEs, and CMake-based projects. It needs a working
LLVM/Clang 19–23 toolchain at both build time and runtime; the compiler binary
records the selected LLVM prefix, while `LLVM_ROOT` can override it later.

## Build and install

On Linux, install CMake and the normal system development prerequisites. If
CMake does not find an LLVM 19–23 installation, it downloads the official
LLVM 23.1.4 SDK automatically (including `clang`, `clang++`, `opt` and the
plugin development files). To use a system toolchain instead, for example
LLVM 20 at `/usr/lib/llvm-20`:

The default CMake build also fetches Z3 and Alive2, builds them as static
libraries, and embeds them in Penstock. The selected backend needs a matching
LLVM development SDK (headers, `LLVMConfig.cmake` and libraries); CMake fetches
a same-major SDK when the local Clang installation does not include one.
Alive2's source build also needs `re2c` at build time. `re2c` is not used by
the installed compiler. Set `-DPENSTOCK_BUILD_EMBEDDED_SUPEROPT=OFF` only when
building a compiler without the embedded superoptimiser.

```sh
cmake -S . -B build -DLLVM_ROOT=/usr/lib/llvm-20 \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="$HOME/.local"
cmake --build build --parallel
cmake --install build
```

The downloaded SDK is kept in `build/_deps` and reused on subsequent
configures. Because Penstock records the selected `LLVM_ROOT` as its runtime
default, keep that SDK directory available after installation or set
`LLVM_ROOT` to another installed LLVM/Clang toolchain when running the
installed compiler. Set `-DPENSTOCK_FETCH_LLVM=OFF` to require an existing
local LLVM installation and prevent downloads.

### Build-time options

| Option | Default | Effect |
|---|---|---|
| `PENSTOCK_FETCH_LLVM` | `ON` | Download an official LLVM SDK when no suitable local toolchain is found; `OFF` requires local LLVM (offline builds) |
| `PENSTOCK_LLVM_VERSION` | `23.1.4` | LLVM release to fetch when no local toolchain exists |
| `PENSTOCK_LLVM_DOWNLOAD_DIR` | `build/_deps` | Download/extraction cache directory |
| `PENSTOCK_BUILD_OPT_PLUGIN` | `ON` | Build the loadable `opt` pass plugin (a matching SDK is fetched if the local clang lacks development files) |
| `PENSTOCK_BUILD_EMBEDDED_SUPEROPT` | `ON` | Bootstrap and embed Freshet superoptimisation with static Z3 and Alive2 |
| `PENSTOCK_BUILD_RTTI_LLVM` | `ON` | Build a private LLVM revision for Alive2 with RTTI/EH, even when the backend SDK already has them |
| `PENSTOCK_RTTI_LLVM_SOURCE_DIR` | empty | Use a local `llvm-project` checkout matching `PENSTOCK_ALIVE2_LLVM_GIT_TAG` |
| `PENSTOCK_ALIVE2_LLVM_BUILD_DIR` | `build/_deps/penstock-alive2-llvm-build` | Private LLVM build directory used by the verifier |
| `PENSTOCK_Z3_GIT_TAG` | `z3-4.15.3` | Z3 source revision fetched and statically linked |
| `PENSTOCK_ALIVE2_GIT_TAG` | `master` | Alive2 source revision fetched and statically linked |
| `PENSTOCK_ALIVE2_LLVM_GIT_TAG` | `main` | LLVM revision paired with Alive2 master |
| `PENSTOCK_FETCH_ZSTD` | `ON` | Fetch and build zstd when the selected LLVM SDK requires it and no system library is available |
| `PENSTOCK_ZSTD_VERSION` | `1.5.7` | Pinned zstd release for the fallback build |

The fetched LLVM archive is SHA-256-verified against GitHub release metadata
before extraction, and automatic downloads support Linux x86-64 and AArch64
(a local `LLVM_ROOT` works on other platforms). After building,
`cmake --build build --target check` runs the regression suite and
`--target bench` the AoS/SoA/fusion/cross-lane benchmarks; both export the
resolved `LLVM_ROOT` so the C baseline compiles with the same backend.

The installed files include:

- `~/.local/bin/penstock` — compiler driver;
- `~/.local/share/penstock/runtime/frt_rt.c` — bundled runtime source;
- `~/.local/lib/libfsh.a` and `~/.local/include/fsh/` — the C runtime API;
- `~/.local/lib/cmake/Penstock/` — CMake package configuration and imported targets.

Add `~/.local/bin` to `PATH` if it is not already there:

```sh
export PATH="$HOME/.local/bin:$PATH"
penstock --version
```

To install system-wide, choose the default prefix (often `/usr/local`) and
use the appropriate permissions when running `cmake --install`. To uninstall,
remove the installed files under the chosen prefix; CMake does not currently
write an uninstall target.

If the Clang installation moves or you want to use another supported backend,
set `LLVM_ROOT` to the LLVM prefix containing `bin/clang` and `bin/clang++`:

```sh
LLVM_ROOT=/usr/lib/llvm-21 penstock -O3 hello.fsh -o hello
```

Installed compiler scratch files and the generated runtime IR cache are kept
in `${XDG_CACHE_HOME}/penstock`, or `~/.cache/penstock` if
`XDG_CACHE_HOME` is unset. Set `PENSTOCK_CACHE_DIR` to override that location.
The cache is per-user; the installed runtime source remains read-only.

## Use from CMake

A consuming project can discover the installation with `find_package` and
call `penstock_add_executable`:

```cmake
cmake_minimum_required(VERSION 3.16)
project(MyFreshetApp LANGUAGES C CXX)

find_package(Penstock CONFIG REQUIRED)

penstock_add_executable(my_app
  src/main.fsh
  OPTIONS -O3 --portable
  DEPENDS src/constants.fsh)
```

The function creates an `ALL` build target named `my_app` and builds the
executable into `build/penstock-bin/my_app` (using your project's binary
directory as `build/`). It accepts
`OPTIONS` to pass additional compiler arguments and `DEPENDS` for other input
files that should trigger a rebuild. The source directory is automatically
added as an include directory; additional include directories can be passed
as `OPTIONS -I/path/to/includes`.

If CMake cannot find the package in a non-standard prefix, point it at the
installation's CMake package directory:

```sh
cmake -S . -B build -DCMAKE_PREFIX_PATH="$HOME/.local"
```

For a one-off command without the helper, the exported executable target can
be called directly:

```cmake
find_package(Penstock CONFIG REQUIRED)
add_custom_command(
  OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/my_app"
  COMMAND Penstock::compiler -O3 "${CMAKE_CURRENT_SOURCE_DIR}/src/main.fsh"
          -o "${CMAKE_CURRENT_BINARY_DIR}/my_app"
  DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/main.fsh"
  VERBATIM)
add_custom_target(my_app ALL
  DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/my_app")
```

### Linking the C runtime API

The C library is exported as `Penstock::fsh` and its public headers are
installed under `include/fsh`. A normal CMake C/C++ target can link it:

```cmake
find_package(Penstock CONFIG REQUIRED)
add_executable(host_tool host_tool.c)
target_link_libraries(host_tool PRIVATE Penstock::fsh)
```

Include either `<fsh.h>` or `<fsh/fsh.h>`; both the `include/fsh` and
`include` search paths are exported by the package.

## Cross-platform notes

The current compiler driver uses Linux-specific executable-path discovery
(`/proc/self/exe`), pthreads, and Linux CPU-affinity APIs. The install and
CMake package integration is therefore currently intended for Linux, matching
the current runtime implementation.
