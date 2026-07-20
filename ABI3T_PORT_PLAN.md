# nanobind abi3t (free-threaded stable ABI) port — implementation plan

> **STATUS: COMPLETE & VERIFIED against CPython 3.15.0b4t.** All phases (P0–P4)
> landed. abi3t test suite: **381 passed, 171 skipped** (the only "failures" are
> `test_stubs.py` missing-`.pyi` artifacts because the build-env stubgen step
> couldn't run — not code defects). `test_thread` free-threaded concurrency
> stress passes. Transparent free-threaded build: **168 passed, 0 failed** (no
> regression). Multi-module coexistence verified. Sections below are the design
> as executed; the "remaining work" framing is historical.
>
> Files changed: `src/nb_ft.{h,cpp}`, `include/nanobind/nb_python.h`,
> `include/nanobind/nb_defs.h`, `include/nanobind/nb_class.h`,
> `src/nb_internals.{h,cpp}`, `src/nb_type.cpp`, `src/nb_func.cpp`,
> `src/nb_ndarray.cpp`, `cmake/nanobind-config.cmake`.
>
> Notable deltas from the original plan: (A) instances use **absolute basicsize
> + a runtime header size** (`internals->inst_header_size`, probed once and
> cached per-module) rather than `PyObject_GetTypeData`, preserving the single
> contiguous `[header][fields][payload]` inheritance layout; only the fixed-size
> leaf objects (`nb_ndarray`, `nb_func`, `nb_bound_method`) use relative
> basicsize + `GetTypeData`. (B) vectorcall/dict/weak offsets are resolved by
> CPython via `Py_RELATIVE_OFFSET` members — no manual runtime offset math. (C)
> a CMake suffix bug (`.abi3tt.so`) was fixed. (D) `nb_inst_header_size` had to
> be shared across modules via the internals struct.

## 0. Status / what already works

- **Build system** (commit `18fb50b`): CMake targets abi3t, defines
  `Py_TARGET_ABI3T=0x030F0000`, tags extensions `.abi3t.so`. Done.
- **Refcount internals** (`src/nb_ft.h`, `src/nb_ft.cpp`, this branch): the
  `#elif defined(_Py_OPAQUE_PYOBJECT)` path forwards `make_immortal`,
  `nb_resurrect`, `nb_try_inc_ref`, `nb_enable_try_inc_ref` to libpython's
  exported helpers (`_Py_SetImmortal`, `_Py_NewReference`,
  `PyUnstable_{Try,EnableTry}IncRef`). Verified compiling/linking/running
  against CPython 3.15.0b4t. Done.

- **P0 — foundations** (this branch): `PyMutex` + module definition under
  abi3t. Done & verified (see below). The `PyMutex`/`PyModuleDef` compile-error
  categories are fully cleared; remaining abi3t build errors are exactly the
  P1–P3 object-model items.

### P0 outcome (settles open questions #1 and #5)

- **`PyMutex` (Decision E1 confirmed).** `<cpython/pylock.h>` documents `PyMutex`
  as a one-byte, zero-initializable object with exported `PyMutex_Lock/Unlock`.
  `include/nanobind/nb_python.h` now declares `typedef struct PyMutex { uint8_t
  _bits; } PyMutex;` + the two functions under `#if defined(_Py_OPAQUE_PYOBJECT)`,
  so `ft_mutex`/`nb_shard`/`nb_internals` keep embedding it by value unchanged.
  (The inline uncontended fast path is unavailable under the limited API; all
  calls go through the exported functions.) E2 not needed.
- **Module definition (PEP 803 export slots).** `PyModuleDef` is opaque under
  abi3t, so `NB_MODULE` (`include/nanobind/nb_defs.h`) now branches: the existing
  `PyModuleDef` + `PyInit_<name>` path for non-abi3t, and for abi3t a `PySlot`
  array returned from `PyModExport_<name>` (`Py_mod_abi` via `PyABIInfo_VAR`,
  `Py_mod_name`, `Py_mod_gil`=NOT_USED, `Py_mod_multiple_interpreters`=
  NOT_SUPPORTED, `Py_mod_exec`, `Py_mod_state_free`=`nb_module_free`). The shared
  `exec` phase is factored into `NB_MODULE_EXEC`. Four-field brace initializers
  keep it C++17- and `-Wmissing-field-initializers`-clean.
- **Verified** against CPython 3.15.0b4t: standalone abi3t modules and the real
  `NB_MODULE` macro compile under `-std=c++17 -pedantic-errors -Wall -Wextra
  -Werror`, export `PyModExport_<name>`, and import with no `RuntimeWarning`
  (GIL stays disabled). Non-abi3t FT build (`test_classes_ext`) confirmed not
  regressed by the macro refactor.

Everything below is the remaining work: it is a **core object-model
re-architecture**, not incremental fixes.

## 1. The one hard constraint

abi3t implies `_Py_OPAQUE_PYOBJECT`. Unlike regular abi3, **`PyObject` is a
fully opaque type**: no `PyObject_HEAD`, no `sizeof(PyObject)`, no `offsetof`
into any object, and `PyMutex` / `PyModuleDef` are opaque too.

Every nanobind object type embeds the header inline and does compile-time
layout math, all of which breaks:

| Struct | File:line | Uses |
|---|---|---|
| `nb_inst` | `nb_internals.h:119` | `PyObject_HEAD`, `sizeof(PyObject)`, `self+offset` payload |
| `nb_func` | `nb_internals.h:152` | `PyObject_VAR_HEAD`, `func_data` at `sizeof(nb_func)` |
| `nb_bound_method` | `nb_internals.h:167` | `PyObject_HEAD`, `offsetof(...)` members |
| `nb_ndarray` | `nb_internals.h:161` | `PyObject_HEAD` |
| `ft_mutex`, `nb_shard`, `nb_internals` | `nb_misc.h:44`, `nb_internals.h:322,521` | `PyMutex` by value |

## 2. Core mechanism decisions

nanobind **already** solves the analogous problem for *type* objects: under
`Py_LIMITED_API` it uses relative (negative) `tp_basicsize` and a runtime
`internals->type_data_offset` to reach `type_data` past the opaque
`PyHeapTypeObject` (`nb_internals.h:632-644`, `nb_type.cpp:1186-1192`). The port
applies the same pattern to *instances* and the func/method/ndarray objects.

Confirmed available under abi3t (compile-probed against 3.15.0b4t):
`PyObject_GetTypeData`, `PyType_GetTypeDataSize`, `PyType_GetSlot`,
`Py_TPFLAGS_ITEMS_AT_END`, `PyType_FromMetaclass`.
**Not** available: `PyObject_GetItemData` (cpython-only) — affects `nb_func`.

### Decision A — instance data via relative basicsize + `PyObject_GetTypeData`

For abi3t, create bound-instance types with **negative** `spec.basicsize`
(= `-(sizeof(nb_inst_fields) + payload + dict/weak slots)`), and reach the
nanobind region with `PyObject_GetTypeData(self, tp)` instead of `(char*)self +
compile-time-offset`.

- `nb_inst`'s two fields (`offset`, `state`) + the C++ payload live in that
  type-data region. Define a header-less `struct nb_inst_data { int32_t offset;
  nb_inst_state state; }` and, under abi3t, resolve it at runtime:
  `nb_inst_data *d = (nb_inst_data *) PyObject_GetTypeData(self, Py_TYPE(self))`.
- `inst_ptr` (`nb_internals.h:646`) becomes: base = `PyObject_GetTypeData(...)`,
  payload at `base + d->offset'`. `offset` now measured from the data region,
  not the object pointer.
- Keep the transparent struct + current fast path for **non**-abi3t builds
  behind `#ifdef`; abi3t gets the accessor-based path. A single pair of
  inline accessors (`nb_inst_data(self)` / `inst_ptr(self)`) hides the split so
  the ~40 call sites in section 3 of the survey change only via those helpers.

### Decision B — `nb_func` stops being a VAR object under abi3t

Because `PyObject_GetItemData` is unavailable, do **not** store the `func_data`
array as trailing items. Instead:

- abi3t `nb_func` type-data region = `{ vectorcall ptr, max_nargs, complexity,
  doc_uniform, func_data *records, uint32_t n_records }`.
- `records` is a `PyMem_Malloc`'d array grown on overload append
  (replaces the `PyType_GenericAlloc(..., prev_overloads+1)` + memmove logic at
  `nb_func.cpp:303,342-349,389`). Freed in `nb_func_dealloc`.
- `nb_func_data(o)` (`nb_internals.h:626`) returns `data->records` under abi3t.

`nb_bound_method` and `nb_ndarray` are fixed-size → Decision A applies directly
(relative basicsize + `PyObject_GetTypeData`).

### Decision C — vectorcall offset computed at runtime

`tp_vectorcall_offset` must point at the `vectorcall` pointer measured from the
object start. With an opaque header that offset = `header_size + position in
data region`. nanobind already has a `__vectorcalloffset__` member → 
`tp_vectorcall_offset` shim (`nb_type.cpp:1026`, `1212-1224`). For abi3t, set
that offset at type-creation time to `runtime_base_basicsize + offsetof-in-data`,
where `runtime_base_basicsize = PyType_GetTypeDataSize`-derived / queried via the
created type's own basicsize. `vectorcall` must be the **first** field of every
data region so the in-region offset is 0.

### Decision D — allocation uses only basicsize/itemsize-aware APIs

Drop raw `PyObject_New` / `PyObject_Malloc` / `PyObject_Realloc` / `PyObject_Init`
pointer math (`nb_type.cpp:122,172,175,194,205`; `nb_ndarray.cpp:1250`). Use
`PyType_GenericAlloc(tp, 0)` (non-GC and GC alike honor relative basicsize) and
`PyObject_GC_New`/`_Del` for the func/method/bound-method GC objects.

- **External-storage instances** (`inst_new_ext`, `nb_type.cpp:165-229`) can no
  longer `PyObject_Realloc` to append an indirection pointer. Instead always
  reserve one pointer-sized slot in the data region for the "indirect" case
  (tiny fixed cost), selected by `state.direct`. This also simplifies the 32-bit
  offset-overflow branch (`nb_type.cpp:194-206`).

### Decision E — `PyMutex` under abi3t

`PyMutex_Lock/Unlock/IsLocked` are exported and usable; only the struct is
hidden. Two options — plan recommends **E1**, falls back to **E2**:

- **E1 (recommended):** the free-threading stable ABI intends `PyMutex` to be a
  statically-embeddable 1-byte object (its documented init is `(PyMutex){0}`).
  Under abi3t declare `struct PyMutex { uint8_t _bits; };` ourselves (guarded to
  abi3t only) + `extern "C"` the three functions. Keeps by-value embedding and
  zero-init in `ft_mutex`, `nb_shard`, `nb_internals` unchanged. **Open
  question:** confirm the abi3t contract guarantees this size (see §5).
- **E2 (safe fallback):** store a `PyThread_type_lock` (opaque `void*`, fully
  stable ABI) allocated lazily / at shard/internals init. Heavier and requires
  lifetime management + touching every lock site; only if E1 is unsound.

## 3. Work breakdown (by file)

1. **`src/nb_internals.h`** — split every embedded-header struct into
   `#ifdef _Py_OPAQUE_PYOBJECT` (header-less data struct + accessor) vs. the
   existing transparent definition. Rewrite `inst_ptr`, `nb_func_data`,
   `nb_inst_state_write`. Add `nb_inst_data()` / `nb_func_data_region()`
   accessors. Handle `nb_shard`/`nb_internals` mutex per Decision E.
2. **`src/nb_type.cpp`** — instance alloc (`inst_new_int` 76-160, `inst_new_ext`
   165-229), `inst_dealloc` (410-431), basicsize computation (1358-1436) →
   relative basicsize, dict/weak slot placement (1501-1546), pool
   (`nb_pool_*` 274-372), all `->offset`/`->state` sites, vectorcall offset
   (1212-1224). Largest single file.
3. **`src/nb_func.cpp`** — `nb_func` de-VAR-ification (Decision B): alloc/realloc
   of `records`, all `nb_func_data()` readers, member access, dealloc free.
4. **`src/nb_internals.cpp`** — `PyMemberDef` arrays (58-62, 118-126) and
   `PyType_Spec`s (70-147): `offsetof` → runtime offsets / relative basicsize;
   `vectorcall` first field; ITEMS_AT_END removed from `nb_func`.
5. **`src/nb_ndarray.cpp`** — `nb_ndarray` alloc (1250) + `->th` access (Decision
   A); type spec (434-440) relative basicsize.
6. **`include/nanobind/nb_misc.h`** — `ft_mutex` per Decision E.
7. **`include/nanobind/nb_class.h`**, **`nb_cast.h`**, **`nb_lib.h`** — the
   public `inst_ptr<T>` / `nb_inst_ptr` / `nb_inst_state_read` wrappers already
   funnel through the C-ABI functions, so they need no change *if* the internal
   accessors preserve signatures. Verify.

## 4. Testing / verification

- Toolchain present: `python3.15t` = 3.15.0b4t (free-threaded). Build with
  `-DNB_TEST_STABLE_ABI=ON -DNB_TEST_FREE_THREADED=ON
  -DPython_EXECUTABLE=$(which python3.15t)`.
- Gate progress on the existing `tests/` pytest suite compiled as abi3t; start
  with `test_classes`, `test_functions`, `test_ndarray`, `test_holders`,
  `test_thread` (pool + FT refcount stress).
- Regression guard: the non-opaque FT build (`test_classes_ext` etc.) and the
  regular abi3 build must keep passing — every change is `#ifdef`-gated.
- `NB_ABORT_ON_LEAK` is already on in tests → catches refcount/layout mistakes.

## 5. Open questions / risks (rank-ordered)

1. ~~**PyMutex size guarantee under abi3t** (Decision E).~~ **RESOLVED in P0:**
   pylock.h documents it as a one-byte zero-initializable object; E1 implemented.
2. **`PyObject_GetTypeData` + inheritance.** nanobind lays out a single
   contiguous C++ payload for the most-derived type incl. base subobjects.
   Confirm `GetTypeData(obj, most_derived_nb_type)` returns a region sized to the
   full derived payload (it should, since basicsize is cumulative), and that the
   base-class-larger-than-derived bump (`nb_type.cpp:1409-1429`) still holds.
3. **Instance pool** parks raw `nb_inst*` and resurrects; with relative basicsize
   the parked block size is `tp_basicsize`-driven — verify `PyType_GenericAlloc`
   round-trips cleanly through `nb_resurrect` (already abi3t-correct) + GC
   re-track.
4. **`tp_vectorcall_offset` runtime value** (Decision C) — validate against a
   real vectorcall of a bound method/function.
5. ~~**`PyModuleDef`** opacity.~~ **RESOLVED in P0:** `NB_MODULE` uses PEP 803
   export slots (`PyModExport_<name>` returning `PySlot*`) under abi3t.

## 6. Effort & phasing

Large: realistically ~400-700 LOC of `#ifdef`-gated changes across 7 files, with
subtle FT-correctness surface. Suggested phases, each independently buildable:

- **P0** `PyMutex` + `PyModuleDef` (get *something* to compile & a trivial module
  to import under abi3t). Resolves risks #1, #5.
- **P1** Fixed-size objects: `nb_bound_method`, `nb_ndarray` (Decision A) — proves
  the `GetTypeData` accessor pattern end-to-end.
- **P2** Instances: `nb_inst` layout, alloc, pool, dict/weak (Decisions A, D).
- **P3** `nb_func` de-VAR + vectorcall offset (Decisions B, C).
- **P4** Full test-suite green; stress `test_thread`.

Recommendation: execute P0 first as a spike — it both unblocks compilation and
settles the biggest open question (PyMutex), after which the P1-P4 estimate can
be firmed up.
