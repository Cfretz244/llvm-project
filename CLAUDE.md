# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

This is **Clang/P2996** — Bloomberg's experimental fork of the LLVM monorepo
([`bloomberg/clang-p2996`](https://github.com/bloomberg/clang-p2996)) implementing
**C++26 static reflection (WG21 P2996)** and a cluster of companion papers in `clang`
and `libc++`. It is, by a wide margin, **the most complete reflection implementation in
any Clang to date** — far ahead of what is in upstream `llvm/llvm-project`, where
reflection is still a parser-only skeleton.

This checkout tracks the fork's integration branch **`p2996`** (local branch
`reflection-p2996`, remote `bloomberg`). The base is **LLVM 21.0.0git** — roughly two
major versions behind upstream tip — because the fork is a deep feature branch, not a
continuously-rebased tree. Upstream `llvm/llvm-project` remains the `origin` remote.

> **Experimental, not production.** The fork's own `P2996.md` warns: sharp edges abound,
> crashes happen, memory use is wasteful. **Do not build production artifacts with it.**
> Some normal Clang facilities (AST dumps for reflections, `clang-tidy`, etc.) are
> deliberately stubbed.

Everything else below is standard LLVM. The structural fact that trips people up still
holds: **you do not point CMake at the repo root.** Point it at the `llvm/` subdirectory
(or `runtimes/` for a runtimes-only build). The repo root has no top-level `CMakeLists.txt`.

Remotes on this checkout: `bloomberg` (the p2996 fork, what `reflection-p2996` tracks),
`origin` (upstream `llvm/llvm-project`), and `fork` (`git@github.com:Cfretz244/llvm-project.git`,
the user's personal fork — note our local prove-out commits here are **not** pushed to it).

## This laptop: the reflection → Python bindings prove-out (read this)

This checkout is the **compiler half** of an ongoing investigation: using this fork's C++26
reflection to *automatically generate Python bindings* (and other reflection-driven tooling).
The umbrella repository is **`~/git/cpp26-reflect-nanobind`**, which pins this repo and the
binder as submodules and carries the overall project CLAUDE.md.

- **The toolchain is already built** at **`~/llvm-toolchain`** (clang/clang++/lld + a
  from-source libc++ providing `<meta>`/`<experimental/meta>`). You normally do **not** need
  to rebuild it; just use it. It was produced by the "Full reflection toolchain" build below.
- **The bindings generator** lives in a separate repo, **`~/git/nanobind`** (branch
  `mk-reflect`) — a reflection-driven nanobind binder. Its `CLAUDE.md` has the full story.
  That repo is the active development surface; this repo is the compiler it runs on.
- **Standalone reflection demos** from this prove-out (a minimal walk and a complete
  reflection-driven binary/JSON serializer) live in `~/git/cpp26-reflect-nanobind/examples/`.
- **Compiler gotcha discovered here and worked around in the binder**: a lambda whose
  *signature* names a spliced type (`[:type_of(x):]` as a param/return type), passed to a
  dependent call, crashes this fork's Itanium mangler (`UNREACHABLE … mangling a placeholder
  type`). Keep spliced types out of lambda signatures (use pointer-to-member / concrete
  types). A P3394 annotation value must also be a valid template argument (no `const char*`
  members — use a fixed-size char array).

To compile/run a reflection program against the prebuilt toolchain, see
"Using the toolchain to compile a reflection program" below (the `-isysroot` is mandatory).

## The reflection stack (read this first)

Reflection is **two pieces that must agree**: a `clang` that understands the `^^` operator
and metafunctions, **and** a `libc++` that provides `<experimental/meta>` (the `std::meta`
library). A stock system libc++ will *not* have `<experimental/meta>`, so **you must build
libc++ from this tree** — building only `clang` gets you the language operator but nothing
to `#include`. The "Full reflection toolchain" build below produces both.

### Enabling reflection when compiling

Reflection is off by default. Turn it on per-translation-unit with `-std=c++26` plus a
feature flag. The flags (defined in `clang/include/clang/Driver/Options.td`,
`LangOptions.def`):

| Flag | Enables | Paper |
|------|---------|-------|
| `-freflection` | Core reflection: `^^`, `std::meta::info`, splicers `[: :]`, all P2996 metafunctions | P2996 |
| `-fparameter-reflection` | Reflection of function parameters | P3096 |
| `-fexpansion-statements` | `template for` expansion statements (over init-lists / destructurables) | P1306 |
| `-fattribute-reflection` | Reflection of standard attributes | P3385 |
| `-fannotation-attributes` | Annotation attributes | P3394 |
| `-fentity-proxy-reflection` | Entity-proxy reflection | (proposed) |
| **`-freflection-latest`** | **Umbrella: turns on all of the above** plus consteval blocks (P3289), newer syntax (P3381), `define_static_*` (P3491), `define_enum` | all |

**For the most functional stack, use `-freflection-latest`.** It is the single flag that
unlocks every implemented feature. `-freflection` alone gets you conformant P2996 core only.
Not implemented: P3294 (token-sequence code injection).

### Minimal reflection program (verified — compiles and runs on this tree)

```cpp
// refl.cpp — build with: clang++ -std=c++26 -freflection-latest -stdlib=libc++ ...
#include <experimental/meta>
#include <print>

using namespace std::meta;

struct Point { int x; int y; };

int main() {
  // `std::meta::info` is a CONSTEVAL-ONLY type: you cannot store it in a plain
  // `constexpr` variable, pass it around at runtime, or call `.size()` on a
  // returned `vector<info>` outside a constant-evaluated context. The idiom is to
  // do all reflection at compile time. `define_static_array` lifts the (heap)
  // vector of reflections into static storage so `template for` can expand it.
  template for (constexpr auto member :
                define_static_array(
                    nonstatic_data_members_of(^^Point, access_context::current()))) {
    std::println("Point.{} : {}", identifier_of(member),
                 display_string_of(type_of(member)));
  }
}
// Output:
//   Point.x : int
//   Point.y : int
```

`^^Point` reflects an entity to a `std::meta::info`; `[: r :]` splices one back into
code. **Gotchas verified by actually compiling against this tree:** (1) the single-argument
`nonstatic_data_members_of(^^T)` overload is *deprecated* — pass
`access_context::current()` (P2996R10); (2) you cannot put a returned `vector<info>` into a
`constexpr` variable (it's heap-allocated → "not a constant expression") — wrap it in
`define_static_array`; (3) anything of consteval-only type must stay in a constant-evaluated
context (`template for`, `consteval`, `static_assert`), never touched at runtime. See
`P2996.md` at the repo root and `libcxx/test/std/experimental/reflection/` for many worked
examples; the issue tracker on GitHub lists known gaps.

### Where the implementation lives

- `clang/lib/Parse/ParseReflect.cpp` — parses the `^^` reflection operator and splicers.
- `clang/lib/Sema/SemaReflect.cpp`, `clang/include/clang/AST/Reflection.h` (+ `Reflection.cpp`) — semantic analysis and the AST representation (`CXXReflectExpr` in `clang/include/clang/AST/ExprCXX.h`).
- `clang/include/clang/AST/Metafunction.h`, `MetaActions.h`, and **`clang/lib/AST/ExprConstantMeta.cpp`** — the metafunction engine: constant-evaluates `std::meta::*` calls and performs splicing. This is the heart of the implementation.
- `clang/include/clang/Basic/DiagnosticMetafnKinds.td` — reflection/metafunction diagnostics.
- `libcxx/include/experimental/meta` (and `libcxx/include/meta`) — the `std::meta` library that user code includes.
- `LANGOPT(Reflection, …)` and siblings in `clang/include/clang/Basic/LangOptions.def`.

## Build system

LLVM builds with **CMake + Ninja**. Configure points at `llvm/`, and components are turned on with two distinct lists:

- **`LLVM_ENABLE_PROJECTS`** — tools/compilers that build *alongside* core LLVM with the host compiler. Valid values: `clang`, `clang-tools-extra`, `lld`, `lldb`, `mlir`, `flang`, `polly`, `bolt`, `cross-project-tests`.
- **`LLVM_ENABLE_RUNTIMES`** — libraries that are built *by the just-built Clang* (bootstrapped). Valid values: `libcxx`, `libcxxabi`, `libunwind`, `compiler-rt`, `openmp`, `offload`, `libc`, `flang-rt`, `libclc`, `llvm-libgcc`, `libsycl`, `orc-rt`.

Never list the same component in both. Runtimes go in `RUNTIMES` because they must be compiled with the new toolchain, not the host one. **For reflection you need `clang` in PROJECTS and `libcxx;libcxxabi;libunwind` in RUNTIMES** — that is what supplies `<experimental/meta>`.

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
| `Debug` | no | yes | yes | Debugging LLVM/Clang itself (large: 15–20 GB, slow tools) |
| `RelWithDebInfo` | yes | yes | no | Profiling / debugging optimized code |
| `MinSizeRel` | size | no | no | Smallest binaries |

Independent of build type, set **`-DLLVM_ENABLE_ASSERTIONS=ON`** for any development build — assertions catch a huge class of IR/codegen bugs and are off by default in Release. Most LLVM devs run **Release + assertions**. On this experimental fork assertions are especially worthwhile: the reflection paths assert liberally and a tripped assert is far more debuggable than the crash it would otherwise become.

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

### Full reflection toolchain — clang + libc++ (recommended)

**This is the build to use for reflection.** It produces a working `clang`/`clang++`/`lld`
plus a from-source libc++/libc++abi/libunwind (which is what carries `<experimental/meta>`),
installed under `~/llvm-toolchain`. With 64 GB RAM and 10 cores the defaults are fine;
assertions on for dev.

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
ninja -C build install         # installs clang/lld + libc++ headers (incl. <experimental/meta>) & libs
```

Notes specific to this setup (verified by configuring on this machine):
- **`LLVM_USE_LINKER=lld` is omitted above on purpose.** On a clean tree it is a *hard configure error* (`Host compiler does not support '-fuse-ld=lld'`) because Apple Clang can't find an lld that doesn't exist yet — not a silent fallback. The system linker (`ld-classic`, found automatically) works fine. Only add `-DLLVM_USE_LINKER=lld` after you have an lld on `PATH` (e.g. `brew install lld`, or a prior install of this toolchain).
- The runtimes (`libcxx;libcxxabi;libunwind`) are compiled by the freshly built Clang automatically as part of the same `ninja` invocation — no separate command needed. They are non-optional for reflection: they are where `<experimental/meta>` comes from.
- On Apple Silicon, building only `AArch64` roughly halves codegen build time vs. `all`. Add `X86` if you need to cross-test x86 codegen.

### Using the toolchain to compile a reflection program

Verified-shaped command for this machine. The **`-isysroot` is required**: unlike Apple's
clang, a from-source clang does not bake in the macOS SDK path, so without it the C library
headers (`ctype.h`, `mbstate_t`, …) aren't found and the compile fails. Note **`-std=c++26
-freflection-latest`** (the reflection switches) and the from-source libc++ that provides
`<experimental/meta>`:

```bash
TC=~/llvm-toolchain
$TC/bin/clang++ -std=c++26 -freflection-latest -stdlib=libc++ \
  -isysroot "$(xcrun --show-sdk-path)" \
  -nostdinc++ -isystem $TC/include/c++/v1 \
  -L $TC/lib -Wl,-rpath,$TC/lib \
  refl.cpp -o refl
```

Confirm it linked *this* toolchain's runtime, not the system one, with `otool -L refl` — it should list `@rpath/libc++.1.dylib` and `@rpath/libunwind.1.dylib`. (A benign `ld: warning: reexported library ... libunwind.1.dylib ... will be linked directly` is expected and harmless.) If `<experimental/meta>` is "file not found," you are picking up a system libc++ — check `-nostdinc++ -isystem $TC/include/c++/v1`.

### Fast iteration build (compiler only, no runtimes)

When you are hacking on the **compiler side** of reflection (parser/Sema/metafunction
engine) and don't need to run programs, skip runtimes for a much faster cycle. You can still
compile-test reflection front-end behavior with `-fsyntax-only` / `-Xclang -ast-dump` style
RUN lines that don't need `<experimental/meta>`:

```bash
cmake -S llvm -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DLLVM_ENABLE_ASSERTIONS=ON -DLLVM_ENABLE_PROJECTS="clang" \
  -DLLVM_TARGETS_TO_BUILD="AArch64" -DLLVM_CCACHE_BUILD=ON
ninja -C build clang
```

To actually `#include <experimental/meta>` and run a program, use the full toolchain build above.

## Testing

LLVM has two test systems: **lit** (regression tests, the bulk) and **googletest** (C++ unit tests). Both surface as `check-*` Ninja targets.

### Running tests via Ninja

```bash
ninja -C build check-all        # everything that's configured
ninja -C build check-llvm       # LLVM regression tests
ninja -C build check-clang      # Clang regression tests (includes clang/test/Reflection)
ninja -C build check-llvm-unit  # LLVM googletest unit tests
ninja -C build check-cxx        # libc++ tests (includes the std::meta reflection suite)
```

Reflection-specific tests live in **`clang/test/Reflection/`** (front-end behavior, run by
`check-clang`) and **`libcxx/test/std/experimental/reflection/`** (the `std::meta` library,
run by `check-cxx`). Expect this suite to be *mostly* green but not 100%: on a
Release+assertions AArch64 build, `clang/test/Reflection/` runs 15/16 passing, with
`splice-exprs.cpp` aborting on an assertion (`cast<>() argument of incompatible type` in
`Sema::CheckAddressOfOperand`) — a live example of the fork's documented sharp edges. A
reflection construct that trips an assertion is the norm here, not a sign your build is
broken. Per-area targets exist too: `check-llvm-codegen`,
`check-llvm-transforms`, `check-llvm-analysis`, etc. A `check-*` target builds the tools the
tests need first, so prefer it over running lit on stale binaries.

### Running a single test or subset

Regression tests run through **`build/bin/llvm-lit`**:

```bash
build/bin/llvm-lit -v clang/test/Reflection/                          # the reflection suite
build/bin/llvm-lit -v clang/test/Reflection/some-test.cpp             # one file, verbose
build/bin/llvm-lit -v --filter="reflect" clang/test/                  # regex over test paths
```

Useful flags: `-v` (show RUN line + output on failure), `--filter REGEXP`, `-j N`, `-a` (show output for all tests). `LIT_FILTER=<regex>` / `LIT_OPTS=...` env vars apply to lit runs launched via Ninja.

Gotcha (verified): running `build/bin/llvm-lit` directly **fails fatally** if `llvm-config` and the tools a test substitutes (`opt`, `llc`, `FileCheck`, `llvm-readobj`, …) aren't built yet — lit calls `llvm-config --assertion-mode --build-mode` during setup. Either build the deps first (`ninja -C build clang FileCheck llvm-config`) or, simpler, run the matching `ninja -C build check-...` target, which builds every dependency before invoking lit.

Unit tests are plain googletest binaries:

```bash
build/unittests/ADT/ADTTests --gtest_filter='SmallVectorTest.*'
build/unittests/ADT/ADTTests --gtest_list_tests
```

### How regression tests work (lit + FileCheck)

A test is a source file (`.cpp`, `.ll`, `.c`, `.s`, `.mir`, `.test`, …) with `RUN:` lines run by lit, whose output is checked by **FileCheck** against `CHECK:` patterns in the same file. A typical Clang front-end test:

```cpp
// RUN: %clang_cc1 -std=c++26 -freflection -fsyntax-only -verify %s
constexpr auto refl = ^^int;       // reflect a type into a std::meta::info
static_assert(refl == ^^int);      // reflections compare by identity
typename [: refl :] x = 42;        // splice it back as a TYPE: int x = 42;
static_assert(sizeof(x) == sizeof(int));
// expected-no-diagnostics
```

(Note the `typename` before a type splice — `[: r :]` defaults to expression context, and
splicing a *type* in expression position is ill-formed: "reflection not usable in a splice
expression." This snippet is verified to compile clean on this tree.)

`%s` = the test file; `%clang_cc1` runs the front-end directly. FileCheck directives: `CHECK`, `CHECK-NEXT` (immediately next line), `CHECK-LABEL` (anchor/reset), `CHECK-NOT`, `CHECK-DAG` (any order), `CHECK-SAME`. `{{regex}}` matches a regex, `[[VAR:pattern]]`/`[[VAR]]` capture and reuse. `--check-prefixes=A,B` runs multiple expectation sets from one file. `-verify` checks `expected-error`/`expected-note` comments instead. lit config lives in `llvm/test/lit.cfg.py` + generated `lit.site.cfg.py`, with per-directory `lit.local.cfg` overrides.

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
llvm/utils/update_cc_test_checks.py --clang=build/bin/clang clang/test/CodeGenCXX/foo.cpp
```

Best practice: regenerate checks on the *baseline* and commit, then apply your code change and regenerate again — so the test diff shows exactly what your change did to codegen/IR.

## Architecture

### The pipeline

```
source ──Clang/Flang──▶ LLVM IR ──opt (passes)──▶ LLVM IR ──llc (CodeGen)──▶ MCInst ──MC──▶ asm/object
```

**LLVM IR is the central abstraction** that decouples every frontend from every backend. It is typed, SSA-form, target-independent, and round-trippable between text (`.ll`) and bitcode (`.bc`). The top container is a **`Module`** (globals + functions + metadata); a `Function` holds `BasicBlock`s holding `Instruction`s. Core classes: `llvm/include/llvm/IR/` (`Module.h`, `Function.h`, `Instruction.h`, `Type.h`, `Value.h`).

Reflection lives almost entirely in the **Clang front-end** (parse → Sema → constant
evaluation), not in LLVM IR. A reflection (`std::meta::info`) is a compile-time value;
metafunctions are constant-evaluated and splices are resolved before IR is emitted, so by
the time code reaches LLVM IR the reflection has been "burned away" into ordinary constants
and instantiated declarations. See "Where the implementation lives" above for the files.

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

`.td` files are a declarative DSL. The TableGen tool (`llvm/lib/TableGen` runtime, `llvm/utils/TableGen` backends/emitters) reads them and **generates C++** at build time. This is how LLVM scales to 20+ backends without hand-writing boilerplate: instruction definitions, encodings, register classes, calling conventions, instruction-selection patterns, intrinsics, subtarget features, and (in Clang) diagnostics are all `.td`-defined. The reflection diagnostics, for instance, are generated from `clang/include/clang/Basic/DiagnosticMetafnKinds.td`. When behavior seems to come from "nowhere," it's often generated from a `.td` file — search the `.td` sources, not just `.cpp`.

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
- **clang** — C/C++/ObjC frontend → LLVM IR. Driver in `clang/lib/Driver`, parser/sema in `clang/lib/{Parse,Sema}`, IR emission in `clang/lib/CodeGen`. Also hosts the static analyzer **and, in this fork, the entire reflection implementation** (`Parse/ParseReflect.cpp`, `Sema/SemaReflect.cpp`, `AST/ExprConstantMeta.cpp`, …).
- **clang-tools-extra** — clang-tidy, clangd (LSP), clang-doc, etc., built on Clang's libraries. (Reflection support in these tools is incomplete on this fork.)
- **mlir** — Multi-Level IR framework: *dialects* (`mlir/include/mlir/Dialect`), the pattern-rewrite/dialect-conversion infrastructure, and pass framework. Flang lowers through MLIR.
- **lld** — fast modular linker; per-format dirs `lld/{ELF,MachO,COFF,wasm}`, shared code in `lld/Common`.
- **lldb** — debugger built on LLVM/Clang; public API in `lldb/include/lldb`, core in `lldb/source`.
- **flang** — Fortran frontend (`flang/lib/{Parser,Semantics,Lower}`), lowers to MLIR/LLVM IR.
- **polly** — polyhedral loop optimizer.
- **bolt** — post-link binary optimizer driven by perf profiles (x86-64 / AArch64 ELF).

Built via `LLVM_ENABLE_RUNTIMES` (compiled by the just-built Clang; orchestrated by `runtimes/CMakeLists.txt`):
- **libcxx / libcxxabi / libunwind** — the C++ standard library, Itanium C++ ABI (exceptions/RTTI), and stack unwinder. libcxx depends on libcxxabi which depends on libunwind. **On this fork, libcxx is where `<experimental/meta>` (the `std::meta` reflection library) lives** — see `libcxx/include/experimental/meta`.
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

**This is a fork, so the workflow differs from upstream LLVM.** Reflection changes target
**`bloomberg/clang-p2996`** (the `bloomberg` remote), branched from its **`p2996`**
integration branch — *not* `origin/main`. PRs and the issue tracker live at
[github.com/bloomberg/clang-p2996](https://github.com/bloomberg/clang-p2996). Keep history
linear (rebase, no merge commits) and include tests (`clang/test/Reflection/`,
`libcxx/test/std/experimental/reflection/`) with every functional change.

Upstream LLVM (`origin`, `llvm/llvm-project`) is a separate destination: it uses GitHub PRs
landed via **"Squash and Merge"** (Phabricator is retired), with reviewers listed in each
component's `Maintainers.md` and Buildkite CI. The long-term plan is for production-grade
reflection to land upstream incrementally; this fork is the reference it is being ported
from, so a change made here may later need re-submitting against modern upstream clang.
