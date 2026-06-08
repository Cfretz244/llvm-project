# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

The **LLVM monorepo** (`llvm/llvm-project`): a toolkit for building compilers, optimizers, and runtimes. The repo root holds each component as a top-level directory. **The core library is `llvm/`**; everything else is a sub-project or runtime that builds against it.

A crucial structural fact that trips people up: **you do not point CMake at the repo root.** You point it at the `llvm/` subdirectory (or `runtimes/` for a runtimes-only build). The repo root has no top-level `CMakeLists.txt` for the build.

## Build system

LLVM builds with **CMake + Ninja**. Configure points at `llvm/`, and components are turned on with two distinct lists:

- **`LLVM_ENABLE_PROJECTS`** — tools/compilers that build *alongside* core LLVM with the host compiler. Valid values: `clang`, `clang-tools-extra`, `lld`, `lldb`, `mlir`, `flang`, `polly`, `bolt`, `cross-project-tests`.
- **`LLVM_ENABLE_RUNTIMES`** — libraries that are built *by the just-built Clang* (bootstrapped). Valid values: `libcxx`, `libcxxabi`, `libunwind`, `compiler-rt`, `openmp`, `offload`, `libc`, `flang-rt`, `libclc`, `llvm-libgcc`, `libsycl`, `orc-rt`.

Never list the same component in both. Runtimes go in `RUNTIMES` because they must be compiled with the new toolchain, not the host one.

### Canonical configure + build

```bash
cmake -S llvm -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DLLVM_ENABLE_PROJECTS="clang;lld"
ninja -C build                 # build everything configured
ninja -C build clang opt llc   # build only specific tool targets
```

`cmake --build build` also works but `ninja -C build <target>` is the idiom. Building a named target (`clang`, `opt`, `llc`, `lld`, `clang-format`, …) builds only that tool and its deps — much faster than the default `all`.

### Build configurations (`CMAKE_BUILD_TYPE`)

| Type | Optimized | Debug info | Assertions | Use for |
|------|-----------|-----------|-----------|---------|
| `Release` | yes | no | no | Default; usable toolchain, fast build |
| `Debug` | no | yes | yes | Debugging LLVM itself (large: 15–20 GB, slow tools) |
| `RelWithDebInfo` | yes | yes | no | Profiling / debugging optimized code |
| `MinSizeRel` | size | no | no | Smallest binaries |

Independent of build type, set **`-DLLVM_ENABLE_ASSERTIONS=ON`** for any development build — assertions catch a huge class of IR/codegen bugs and are off by default in Release. Most LLVM devs run **Release + assertions**.

### Important CMake options

| Option | Purpose |
|--------|---------|
| `LLVM_TARGETS_TO_BUILD` | Backends to build. `"host"` = native only (fast); `"all"` = every target; or e.g. `"X86;AArch64;RISCV"`. Default is all — restrict it to cut build time. |
| `LLVM_ENABLE_ASSERTIONS` | Turn on assertions (recommended for dev). |
| `LLVM_USE_LINKER=lld` | Use lld instead of the default linker — dramatically faster links. |
| `LLVM_OPTIMIZED_TABLEGEN=ON` | Build TableGen optimized even in Debug builds; large speedup for Debug configs. |
| `LLVM_PARALLEL_LINK_JOBS=N` | Cap concurrent links (link is the RAM bottleneck). |
| `LLVM_RAM_PER_LINK_JOB=MB` | Auto-limit link jobs by available RAM instead of a fixed N. |
| `LLVM_CCACHE_BUILD=ON` | Route compiles through ccache for fast rebuilds. |
| `LLVM_USE_SPLIT_DWARF=ON` | Split debug info to reduce link-time memory (Debug/ELF). |
| `BUILD_SHARED_LIBS=ON` | Each component as a `.so` — faster incremental dev links, discouraged for release. |
| `LLVM_BUILD_LLVM_DYLIB` / `LLVM_LINK_LLVM_DYLIB` | Single `libLLVM.so`; link tools against it. |
| `CMAKE_INSTALL_PREFIX=<dir>` | Install location for `ninja install`. |

### Two-stage / bootstrap builds

`-DCLANG_ENABLE_BOOTSTRAP=ON` builds Clang with the host compiler (stage1), then rebuilds it with that Clang (stage2). `ninja stage2` drives it. Pass host-build options through with `CLANG_BOOTSTRAP_PASSTHROUGH=...`; prefix stage2-only options with `BOOTSTRAP_`. Curated multi-stage/PGO recipes live in `clang/cmake/caches/*.cmake` (used via `cmake -C <cache>.cmake`).

## Building on THIS machine (Apple Silicon macOS)

This laptop: **arm64 (Apple Silicon), 10 cores, 64 GB RAM, macOS 26.5, Xcode + Apple Clang 17, ~1.3 TB free.** Homebrew and CMake are installed; **Ninja and ccache are not** — install them first. The native LLVM target here is `AArch64`.

### One-time setup

```bash
brew install ninja ccache
```

Apple Clang (`/usr/bin/clang`) is the bootstrap compiler; the macOS SDK comes from Xcode (`xcrun --show-sdk-path`).

### Full custom toolchain + libc++ (recommended)

This produces a working `clang`/`clang++`/`lld` plus a from-source libc++/libc++abi/libunwind, installed under `~/llvm-toolchain`. With 64 GB RAM and 10 cores the defaults are fine; assertions on for dev.

```bash
cmake -S llvm -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DLLVM_ENABLE_ASSERTIONS=ON \
  -DLLVM_ENABLE_PROJECTS="clang;lld" \
  -DLLVM_ENABLE_RUNTIMES="libcxx;libcxxabi;libunwind" \
  -DLLVM_TARGETS_TO_BUILD="AArch64" \
  -DLLVM_CCACHE_BUILD=ON \
  -DLLVM_OPTIMIZED_TABLEGEN=ON \
  -DCMAKE_INSTALL_PREFIX="$HOME/llvm-toolchain"
  # add -DLLVM_USE_LINKER=lld ONLY if an lld is already on PATH (see note below)

ninja -C build                 # ~all targets; first build is long
ninja -C build install         # installs clang/lld + libc++ headers & libs
```

Notes specific to this setup (verified by configuring on this machine):
- **`LLVM_USE_LINKER=lld` is omitted above on purpose.** On a clean tree it is a *hard configure error* (`Host compiler does not support '-fuse-ld=lld'`) because Apple Clang can't find an lld that doesn't exist yet — not a silent fallback. The system linker (`ld-classic`, found automatically) works fine. Only add `-DLLVM_USE_LINKER=lld` after you have an lld on `PATH` (e.g. `brew install lld`, or a prior install of this toolchain).
- The runtimes (`libcxx;libcxxabi;libunwind`) are compiled by the freshly built Clang automatically as part of the same `ninja` invocation — no separate command needed.
- On Apple Silicon, building only `AArch64` roughly halves codegen build time vs. `all`. Add `X86` if you need to cross-test x86 codegen.

### Using the freshly built libc++

Verified working on this machine. The **`-isysroot` is required**: unlike Apple's clang, a from-source clang does not bake in the macOS SDK path, so without it the C library headers (`ctype.h`, `mbstate_t`, …) aren't found and the compile fails.

```bash
TC=~/llvm-toolchain
$TC/bin/clang++ -std=c++20 -stdlib=libc++ \
  -isysroot "$(xcrun --show-sdk-path)" \
  -nostdinc++ -isystem $TC/include/c++/v1 \
  -L $TC/lib -Wl,-rpath,$TC/lib \
  hello.cpp -o hello
```

Confirm it linked *this* toolchain's runtime, not the system one, with `otool -L hello` — it should list `@rpath/libc++.1.dylib` and `@rpath/libunwind.1.dylib`. (A benign `ld: warning: reexported library ... libunwind.1.dylib ... will be linked directly` is expected and harmless.)

### Fast iteration build (tools only, no runtimes)

When you only need to hack on LLVM/Clang and not the C++ runtime, skip runtimes for a much faster cycle:

```bash
cmake -S llvm -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DLLVM_ENABLE_ASSERTIONS=ON -DLLVM_ENABLE_PROJECTS="clang" \
  -DLLVM_TARGETS_TO_BUILD="AArch64" -DLLVM_CCACHE_BUILD=ON
ninja -C build clang
```

## Testing

LLVM has two test systems: **lit** (regression tests, the bulk) and **googletest** (C++ unit tests). Both surface as `check-*` Ninja targets.

### Running tests via Ninja

```bash
ninja -C build check-all        # everything that's configured
ninja -C build check-llvm       # LLVM regression tests
ninja -C build check-clang      # Clang regression tests
ninja -C build check-llvm-unit  # LLVM googletest unit tests
ninja -C build check-cxx        # libc++ tests (when libcxx is a runtime)
```

Per-area targets exist too: `check-llvm-codegen`, `check-llvm-transforms`, `check-llvm-analysis`, etc. A `check-*` target builds the tools the tests need first, so prefer it over running lit on stale binaries.

### Running a single test or subset

Regression tests run through **`build/bin/llvm-lit`**:

```bash
build/bin/llvm-lit -v llvm/test/Transforms/InstCombine/add.ll        # one file, verbose
build/bin/llvm-lit -v llvm/test/Transforms/InstCombine/              # a directory
build/bin/llvm-lit -v --filter="add" llvm/test/Transforms/           # regex over test paths
```

Useful flags: `-v` (show RUN line + output on failure), `--filter REGEXP`, `-j N`, `-a` (show output for all tests). `LIT_FILTER=<regex>` / `LIT_OPTS=...` env vars apply to lit runs launched via Ninja.

Gotcha (verified): running `build/bin/llvm-lit` directly **fails fatally** if `llvm-config` and the tools a test substitutes (`opt`, `llc`, `FileCheck`, `llvm-readobj`, …) aren't built yet — lit calls `llvm-config --assertion-mode --build-mode` during setup. Either build the deps first (`ninja -C build opt FileCheck llvm-config`) or, simpler, run the matching `ninja -C build check-...` target, which builds every dependency before invoking lit.

Unit tests are plain googletest binaries:

```bash
build/unittests/ADT/ADTTests --gtest_filter='SmallVectorTest.*'
build/unittests/ADT/ADTTests --gtest_list_tests
```

### How regression tests work (lit + FileCheck)

A test is a source file (`.ll`, `.c`, `.s`, `.mir`, `.test`, …) with `RUN:` lines run by lit, whose output is checked by **FileCheck** against `CHECK:` patterns in the same file:

```llvm
; RUN: opt -S -passes=instcombine < %s | FileCheck %s
define i32 @f(i32 %x) {
; CHECK-LABEL: @f(
; CHECK-NEXT: ret i32 %x
  %a = add i32 %x, 0
  ret i32 %a
}
```

`%s` = the test file. FileCheck directives: `CHECK`, `CHECK-NEXT` (immediately next line), `CHECK-LABEL` (anchor/reset), `CHECK-NOT`, `CHECK-DAG` (any order), `CHECK-SAME`. `{{regex}}` matches a regex, `[[VAR:pattern]]`/`[[VAR]]` capture and reuse. `--check-prefixes=A,B` runs multiple expectation sets from one file. lit config lives in `llvm/test/lit.cfg.py` + generated `lit.site.cfg.py`, with per-directory `lit.local.cfg` overrides.

### Auto-generating CHECK lines

Do NOT hand-write CHECK lines for codegen/IR tests — generate them. Pick the script by the tool the RUN line drives:

| Script (`llvm/utils/`) | For tests that run |
|------------------------|--------------------|
| `update_test_checks.py` | `opt` (IR → IR) |
| `update_llc_test_checks.py` | `llc` (IR → asm) |
| `update_cc_test_checks.py` | `clang`/`clang++` (`-emit-llvm`) |
| `update_mir_test_checks.py` | MIR-level tests |
| `update_mc_test_checks.py` | `llvm-mc` |

```bash
llvm/utils/update_test_checks.py --tool-binary=build/bin/opt llvm/test/Transforms/InstCombine/foo.ll
```

Best practice: regenerate checks on the *baseline* and commit, then apply your code change and regenerate again — so the test diff shows exactly what your change did to codegen/IR.

## Architecture

### The pipeline

```
source ──Clang/Flang──▶ LLVM IR ──opt (passes)──▶ LLVM IR ──llc (CodeGen)──▶ MCInst ──MC──▶ asm/object
```

**LLVM IR is the central abstraction** that decouples every frontend from every backend. It is typed, SSA-form, target-independent, and round-trippable between text (`.ll`) and bitcode (`.bc`). The top container is a **`Module`** (globals + functions + metadata); a `Function` holds `BasicBlock`s holding `Instruction`s. Core classes: `llvm/include/llvm/IR/` (`Module.h`, `Function.h`, `Instruction.h`, `Type.h`, `Value.h`).

### Key directories under `llvm/`

- `lib/IR`, `include/llvm/IR` — the IR itself (Module/Function/Instruction/Type/Constant/Metadata).
- `lib/Analysis` — analyses that compute properties without mutating IR (alias analysis, `LoopInfo`, dominators, ScalarEvolution).
- `lib/Transforms/{Scalar,IPO,InstCombine,Vectorize,Utils}` — the optimizer. `Scalar` = function-local passes; `IPO` = interprocedural (inlining, LTO); `InstCombine` = peephole; `Vectorize` = loop/SLP vectorization; `Utils` = shared helpers.
- `lib/CodeGen` — target-independent codegen: `MachineFunction`/`MachineInstr`, instruction selection, register allocation, scheduling.
- `lib/Target/<Arch>` — per-backend code (X86, AArch64, RISCV, …).
- `lib/MC` — the Machine Code layer (`MCInst`, asm parsing, encoding, object emission).
- `lib/Passes` — the **new pass manager**: `PassBuilder` constructs pipelines; `AnalysisManager` caches/invalidates analyses.
- `lib/ExecutionEngine` — JIT (ORC) and interpreter.

### TableGen (critical to grok)

`.td` files are a declarative DSL. The TableGen tool (`llvm/lib/TableGen` runtime, `llvm/utils/TableGen` backends/emitters) reads them and **generates C++** at build time. This is how LLVM scales to 20+ backends without hand-writing boilerplate: instruction definitions, encodings, register classes, calling conventions, instruction-selection patterns, intrinsics, subtarget features, and (in Clang) diagnostics are all `.td`-defined. A backend like X86 is dozens of `.td` files (e.g. `X86InstrInfo.td`) plus C++ glue. When IR-level behavior seems to come from "nowhere," it's often generated from a `.td` file — search the `.td` sources, not just `.cpp`.

### Pass manager

The **new PM** (`include/llvm/IR/PassManager.h`, `include/llvm/Passes/PassBuilder.h`) is concept-based: any type with `run(IRUnit&, AnalysisManager&)` is a pass — no base class. `PassManager<IRUnitT>` is templated over Module/Function/Loop/CGSCC/MachineFunction. Passes register by string name; `opt -passes="..."` and `PassBuilder` parse those strings. The **legacy PM** (`LegacyPassManager.h`) still backs parts of the codegen pipeline.

### Target / backend structure

`TargetMachine` (`include/llvm/Target/TargetMachine.h`) is the facade to all target-specific services; each backend subclasses it (`X86TargetMachine`, etc.). Two instruction-selection paths:
- **SelectionDAG** (`include/llvm/CodeGen/SelectionDAG.h`) — the mature path: IR → DAG of `SDNode`s → `.td` pattern match → machine instrs. Per-target `*ISelLowering.cpp` + `*ISelDAGToDAG.cpp`.
- **GlobalISel** (`include/llvm/CodeGen/GlobalISel/`) — newer, more uniform: IRTranslator → Legalizer → RegBankSelect → InstructionSelect over generic `G_*` opcodes.

Then register allocation (`LiveIntervals`, greedy/fast allocators), scheduling, and lowering through the MC layer to asm/object.

### Key tools (`llvm/tools/`)

- `opt` — run IR passes on `.ll`/`.bc`; drives the optimizer.
- `llc` — IR → assembly/object; drives CodeGen.
- `llvm-mc` — standalone assembler/disassembler at the MC layer.
- `llvm-as` / `llvm-dis` — text IR ↔ bitcode.
- `lli` — execute IR via the JIT/interpreter.

## Sub-projects

Built via `LLVM_ENABLE_PROJECTS`:
- **clang** — C/C++/ObjC frontend → LLVM IR. Driver in `clang/lib/Driver`, parser/sema in `clang/lib/{Parse,Sema}`, IR emission in `clang/lib/CodeGen`. Also hosts the static analyzer.
- **clang-tools-extra** — clang-tidy, clangd (LSP), clang-doc, etc., built on Clang's libraries.
- **mlir** — Multi-Level IR framework: *dialects* (`mlir/include/mlir/Dialect`), the pattern-rewrite/dialect-conversion infrastructure, and pass framework. Flang lowers through MLIR.
- **lld** — fast modular linker; per-format dirs `lld/{ELF,MachO,COFF,wasm}`, shared code in `lld/Common`.
- **lldb** — debugger built on LLVM/Clang; public API in `lldb/include/lldb`, core in `lldb/source`.
- **flang** — Fortran frontend (`flang/lib/{Parser,Semantics,Lower}`), lowers to MLIR/LLVM IR.
- **polly** — polyhedral loop optimizer.
- **bolt** — post-link binary optimizer driven by perf profiles (x86-64 / AArch64 ELF).

Built via `LLVM_ENABLE_RUNTIMES` (compiled by the just-built Clang; orchestrated by `runtimes/CMakeLists.txt`):
- **libcxx / libcxxabi / libunwind** — the C++ standard library, Itanium C++ ABI (exceptions/RTTI), and stack unwinder. libcxx depends on libcxxabi which depends on libunwind.
- **compiler-rt** — sanitizers (asan/tsan/msan/ubsan), `builtins`, profiling runtime.
- **libc** — LLVM's own modular C standard library.
- **openmp / offload** — OpenMP runtime and accelerator/GPU offload.

## Coding standards (LLVM-specific)

Full rules in `llvm/docs/CodingStandards.rst`. The non-obvious, project-specific ones:

- **Naming**: types and *variables* are `UpperCamelCase` (`SmallVector`, `Leader`); functions/methods are `lowerCamelCase` (`openFile`, `isFoo`); enumerators are `Prefix_Value`. (Note variables are Upper-camel — unusual.)
- **No C++ exceptions, no RTTI.** Use LLVM's hand-rolled RTTI — `isa<>`, `cast<>`, `dyn_cast<>` — not `dynamic_cast`. Adding RTTI to a class is opt-in (`classof`).
- **Prefer LLVM ADTs over std**: `SmallVector`, `StringRef`, `ArrayRef`, `DenseMap`, `StringMap` over the `std::` equivalents. Use `raw_ostream`, never `<iostream>`, in library code.
- **No static constructors** (no globals with non-trivial ctors/dtors).
- **`auto` sparingly** — only where the type is obvious (e.g. `dyn_cast` result, iterators); watch for accidental copies (`auto` vs `auto&`/`auto*`).
- **Doxygen with `///`**; `\param`/`\returns`/`\p`. 80-column limit. C++-style casts, never C-style.

Format only your changed lines with **`git clang-format`** (config: `.clang-format`, `BasedOnStyle: LLVM`); keep formatting-only churn in separate commits. `.clang-tidy` at the root encodes the naming rules above.

## Contribution workflow

LLVM uses **GitHub pull requests** (Phabricator is retired/read-only). Branch from `origin/main`, keep history **linear (no merge commits)**, rebase rather than merge. PRs land via GitHub **"Squash and Merge"**, so the PR title + description become the final commit message — write them as the changelog entry. Include tests with every functional change. Reviewers/maintainers are listed in each component's `Maintainers.md`. CI is Buildkite; don't force-merge over failures attributable to your change.
