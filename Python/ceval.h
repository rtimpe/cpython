#define _PY_INTERPRETER

#include "Python.h"
#include "pycore_abstract.h"      // _PyIndex_Check()
#include "pycore_audit.h"         // _PySys_Audit()
#include "pycore_backoff.h"
#include "pycore_call.h"          // _PyObject_CallNoArgs()
#include "pycore_cell.h"          // PyCell_GetRef()
#include "pycore_ceval.h"         // SPECIAL___ENTER__
#include "pycore_code.h"
#include "pycore_dict.h"
#include "pycore_emscripten_signal.h"  // _Py_CHECK_EMSCRIPTEN_SIGNALS
#include "pycore_floatobject.h"   // _PyFloat_ExactDealloc()
#include "pycore_frame.h"
#include "pycore_function.h"
#include "pycore_genobject.h"     // _PyCoro_GetAwaitableIter()
#include "pycore_import.h"        // _PyImport_IsDefaultImportFunc()
#include "pycore_instruments.h"
#include "pycore_interpframe.h"   // _PyFrame_SetStackPointer()
#include "pycore_interpolation.h" // _PyInterpolation_Build()
#include "pycore_intrinsics.h"
#include "pycore_jit.h"
#include "pycore_lazyimportobject.h"
#include "pycore_list.h"          // _PyList_GetItemRef()
#include "pycore_long.h"          // _PyLong_GetZero()
#include "pycore_moduleobject.h"  // PyModuleObject
#include "pycore_object.h"        // _PyObject_GC_TRACK()
#include "pycore_opcode_metadata.h" // EXTRA_CASES
#include "pycore_opcode_utils.h"  // MAKE_FUNCTION_*
#include "pycore_optimizer.h"     // _PyUOpExecutor_Type
#include "pycore_pyatomic_ft_wrappers.h" // FT_ATOMIC_*
#include "pycore_pyerrors.h"      // _PyErr_GetRaisedException()
#include "pycore_pystate.h"       // _PyInterpreterState_GET()
#include "pycore_range.h"         // _PyRangeIterObject
#include "pycore_setobject.h"     // _PySet_Update()
#include "pycore_sliceobject.h"   // _PyBuildSlice_ConsumeRefs
#include "pycore_sysmodule.h"     // _PySys_GetOptionalAttrString()
#include "pycore_template.h"      // _PyTemplate_Build()
#include "pycore_traceback.h"     // _PyTraceBack_FromFrame
#include "pycore_tuple.h"         // _PyTuple_ITEMS()
#include "pycore_uop_ids.h"       // Uops

#include "dictobject.h"
#include "frameobject.h"          // _PyInterpreterFrame_GetLine
#include "opcode.h"
#include "pydtrace.h"
#include "setobject.h"
#include "pycore_stackref.h"

#include <stdbool.h>              // bool

#if !defined(Py_BUILD_CORE)
#  error "ceval.c must be build with Py_BUILD_CORE define for best performance"
#endif

#if !defined(Py_DEBUG) && !defined(Py_TRACE_REFS)
// GH-89279: The MSVC compiler does not inline these static inline functions
// in PGO build in _PyEval_EvalFrameDefault(), because this function is over
// the limit of PGO, and that limit cannot be configured.
// Define them as macros to make sure that they are always inlined by the
// preprocessor.

#undef Py_IS_TYPE
#define Py_IS_TYPE(ob, type) \
    (_PyObject_CAST(ob)->ob_type == (type))

#undef Py_XDECREF
#define Py_XDECREF(arg) \
    do { \
        PyObject *xop = _PyObject_CAST(arg); \
        if (xop != NULL) { \
            Py_DECREF(xop); \
        } \
    } while (0)

#ifndef Py_GIL_DISABLED

#undef Py_DECREF
#define Py_DECREF(arg) \
    do { \
        PyObject *op = _PyObject_CAST(arg); \
        if (_Py_IsImmortal(op)) { \
            _Py_DECREF_IMMORTAL_STAT_INC(); \
            break; \
        } \
        _Py_DECREF_STAT_INC(); \
        if (--op->ob_refcnt == 0) { \
            _PyReftracerTrack(op, PyRefTracer_DESTROY); \
            destructor dealloc = Py_TYPE(op)->tp_dealloc; \
            (*dealloc)(op); \
        } \
    } while (0)

#undef _Py_DECREF_SPECIALIZED
#define _Py_DECREF_SPECIALIZED(arg, dealloc) \
    do { \
        PyObject *op = _PyObject_CAST(arg); \
        if (_Py_IsImmortal(op)) { \
            _Py_DECREF_IMMORTAL_STAT_INC(); \
            break; \
        } \
        _Py_DECREF_STAT_INC(); \
        if (--op->ob_refcnt == 0) { \
            _PyReftracerTrack(op, PyRefTracer_DESTROY); \
            destructor d = (destructor)(dealloc); \
            d(op); \
        } \
    } while (0)

#else // Py_GIL_DISABLED

#undef Py_DECREF
#define Py_DECREF(arg) \
    do { \
        PyObject *op = _PyObject_CAST(arg); \
        uint32_t local = _Py_atomic_load_uint32_relaxed(&op->ob_ref_local); \
        if (local == _Py_IMMORTAL_REFCNT_LOCAL) { \
            _Py_DECREF_IMMORTAL_STAT_INC(); \
            break; \
        } \
        _Py_DECREF_STAT_INC(); \
        if (_Py_IsOwnedByCurrentThread(op)) { \
            local--; \
            _Py_atomic_store_uint32_relaxed(&op->ob_ref_local, local); \
            if (local == 0) { \
                _Py_MergeZeroLocalRefcount(op); \
            } \
        } \
        else { \
            _Py_DecRefShared(op); \
        } \
    } while (0)

#undef _Py_DECREF_SPECIALIZED
#define _Py_DECREF_SPECIALIZED(arg, dealloc) Py_DECREF(arg)

#endif
#endif

#ifdef Py_DEBUG
static void
dump_item(_PyStackRef item)
{
    if (PyStackRef_IsNull(item)) {
        printf("<NULL>");
        return;
    }
    if (PyStackRef_IsMalformed(item)) {
        printf("<INVALID>");
        return;
    }
    if (PyStackRef_IsTaggedInt(item)) {
        printf("%" PRId64, (int64_t)PyStackRef_UntagInt(item));
        return;
    }
    PyObject *obj = PyStackRef_AsPyObjectBorrow(item);
    if (obj == NULL) {
        printf("<nil>");
        return;
    }
    // Don't call __repr__(), it might recurse into the interpreter.
    printf("<%s at %p>", Py_TYPE(obj)->tp_name, (void *)obj);
}

static void
dump_stack(_PyInterpreterFrame *frame, _PyStackRef *stack_pointer)
{
    _PyFrame_SetStackPointer(frame, stack_pointer);
    _PyStackRef *locals_base = _PyFrame_GetLocalsArray(frame);
    _PyStackRef *stack_base = _PyFrame_Stackbase(frame);
    PyObject *exc = PyErr_GetRaisedException();
    printf("    locals=[");
    for (_PyStackRef *ptr = locals_base; ptr < stack_base; ptr++) {
        if (ptr != locals_base) {
            printf(", ");
        }
        dump_item(*ptr);
    }
    printf("]\n");
    if (stack_pointer < stack_base) {
        printf("    stack=%d\n", (int)(stack_pointer-stack_base));
    }
    else {
        printf("    stack=[");
        for (_PyStackRef *ptr = stack_base; ptr < stack_pointer; ptr++) {
            if (ptr != stack_base) {
                printf(", ");
            }
            dump_item(*ptr);
        }
        printf("]\n");
    }
    fflush(stdout);
    PyErr_SetRaisedException(exc);
    _PyFrame_GetStackPointer(frame);
}

#if defined(_Py_TIER2) && !defined(_Py_JIT) && defined(Py_DEBUG)
static void
dump_cache_item(_PyStackRef cache, int position, int depth)
{
    if (position < depth) {
        dump_item(cache);
    }
    else {
        printf("---");
    }
}
#endif

static void
lltrace_instruction(_PyInterpreterFrame *frame,
                    _PyStackRef *stack_pointer,
                    _Py_CODEUNIT *next_instr,
                    int opcode,
                    int oparg)
{
    int offset = 0;
    if (frame->owner < FRAME_OWNED_BY_INTERPRETER) {
        dump_stack(frame, stack_pointer);
        offset = (int)(next_instr - _PyFrame_GetBytecode(frame));
    }
    const char *opname = _PyOpcode_OpName[opcode];
    assert(opname != NULL);
    if (OPCODE_HAS_ARG((int)_PyOpcode_Deopt[opcode])) {
        printf("%d: %s %d\n", offset * 2, opname, oparg);
    }
    else {
        printf("%d: %s\n", offset * 2, opname);
    }
    fflush(stdout);
}

static void
lltrace_resume_frame(_PyInterpreterFrame *frame)
{
    PyObject *fobj = PyStackRef_AsPyObjectBorrow(frame->f_funcobj);
    if (!PyStackRef_CodeCheck(frame->f_executable) ||
        fobj == NULL ||
        !PyFunction_Check(fobj)
    ) {
        printf("\nResuming frame.\n");
        return;
    }
    PyFunctionObject *f = (PyFunctionObject *)fobj;
    PyObject *exc = PyErr_GetRaisedException();
    PyObject *name = f->func_qualname;
    if (name == NULL) {
        name = f->func_name;
    }
    printf("\nResuming frame");
    if (name) {
        printf(" for ");
        if (PyObject_Print(name, stdout, 0) < 0) {
            PyErr_Clear();
        }
    }
    if (f->func_module) {
        printf(" in module ");
        if (PyObject_Print(f->func_module, stdout, 0) < 0) {
            PyErr_Clear();
        }
    }
    printf("\n");
    fflush(stdout);
    PyErr_SetRaisedException(exc);
}

static int
maybe_lltrace_resume_frame(_PyInterpreterFrame *frame, PyObject *globals)
{
    if (globals == NULL) {
        return 0;
    }
    if (frame->owner >= FRAME_OWNED_BY_INTERPRETER) {
        return 0;
    }
    int r = PyDict_Contains(globals, &_Py_ID(__lltrace__));
    if (r < 0) {
        PyErr_Clear();
        return 0;
    }
    int lltrace = r * 5;  // Levels 1-4 only trace uops
    if (!lltrace) {
        // Can also be controlled by environment variable
        char *python_lltrace = Py_GETENV("PYTHON_LLTRACE");
        if (python_lltrace != NULL && *python_lltrace >= '0') {
            lltrace = *python_lltrace - '0';  // TODO: Parse an int and all that
        }
    }
    if (lltrace >= 5) {
        lltrace_resume_frame(frame);
    }
    return lltrace;
}

#endif
#ifdef HAVE_ERRNO_H
#include <errno.h>
#endif
