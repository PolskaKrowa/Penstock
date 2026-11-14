# third_party — vendored dependencies (git submodules)

This directory holds the three dependencies Penstock compiles into the
dynamic libraries its `penstock` and `freshet-language-server` binaries link:

| Submodule            | Upstream                                  | Pinned at              | Used for |
|----------------------|-------------------------------------------|------------------------|----------|
| `third_party/llvm`   | https://github.com/llvm/llvm-project      | `main` (shallow clone) | the RTTI/EH-enabled `libLLVM.so` used by the Alive2 verifier and the O4 opt-plugin host |
| `third_party/alive2` | https://github.com/AliveToolkit/alive2    | `master`               | the in-process translation-validation shared libraries |
| `third_party/z3`     | https://github.com/Z3Prover/z3            | `z3-4.15.3`            | the SMT solver `libz3.so` behind the Freshet superoptimiser |

In a git checkout these are gitlinks: run

```sh
git submodule update --init        # add --depth 1 for the smallest llvm clone
```

to populate them (a fresh clone can use `git clone --recurse-submodules`).
The pinned commit recorded in the gitlink — not the `PENSTOCK_*_GIT_TAG`
CMake cache strings — selects what is built. See
[docs/superopt-guide.md](../docs/superopt-guide.md) for how to move a pin,
and how the shared libraries are linked into both binaries.

**Release tarballs ship this directory without the submodule contents.**
Without populated submodules the CMake configure automatically falls back to
fetching the same revisions via FetchContent, so the tarball build works
out of the box (network required) — or copy a populated `third_party/`
tree next to this file to build fully offline.

Note: the CMake configure patches `third_party/z3` and `third_party/alive2`
in place (CMake target namespacing and STATIC→SHARED conversion, idempotent;
see `cmake/PenstockSuperopt.cmake`). `git submodule update --checkout`
restores the pristine trees and the next configure reapplies the patches.
