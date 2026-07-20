/*
    nanobind/nb_python.h: Include CPython headers while temporarily disabling
    certain warnings. Also, disable dangerous preprocessor definitions.

    Copyright (c) 2022 Wenzel Jakob

    All rights reserved. Use of this source code is governed by a
    BSD-style license that can be found in the LICENSE file.
*/

/// Include Python header, disable linking to pythonX_d.lib on Windows in debug mode

#if defined(_MSC_VER)
#  pragma warning(push)
#  if defined(_DEBUG) && !defined(Py_DEBUG)
#    define NB_DEBUG_MARKER
#    undef _DEBUG
#  endif
#endif

#include <Python.h>
#include <frameobject.h>
#include <pythread.h>

#if defined(_Py_OPAQUE_PYOBJECT)
/* Free-threaded stable ABI ("abi3t", PEP 803): the limited API excludes
   <cpython/pylock.h>, so ``PyMutex`` is undeclared. It is nonetheless a
   documented, statically zero-initializable one-byte object whose lock/unlock
   routines are exported from libpython. Declare them ourselves so nanobind can
   keep embedding ``PyMutex`` by value (in ``ft_mutex``, ``nb_shard`` and
   ``nb_internals``). The inline uncontended fast path from pylock.h is
   unavailable here, so every call goes through the exported functions. */
extern "C" {
    typedef struct PyMutex { uint8_t _bits; } PyMutex;
    PyAPI_FUNC(void) PyMutex_Lock(PyMutex *m);
    PyAPI_FUNC(void) PyMutex_Unlock(PyMutex *m);
}
#endif

/* Python #defines overrides on all sorts of core functions, which
   tends to weak havok in C++ codebases that expect these to work
   like regular functions (potentially with several overloads) */
#if defined(isalnum)
#  undef isalnum
#  undef isalpha
#  undef islower
#  undef isspace
#  undef isupper
#  undef tolower
#  undef toupper
#endif

#if defined(copysign)
#  undef copysign
#endif

#if defined(setter)
#  undef setter
#endif

#if defined(getter)
#  undef getter
#endif

#if defined(_MSC_VER)
#  if defined(NB_DEBUG_MARKER)
#    define _DEBUG
#    undef NB_DEBUG_MARKER
#  endif
#  pragma warning(pop)
#endif

#if PY_VERSION_HEX < 0x03090000
#  error The nanobind library requires Python 3.9 (or newer)
#endif
