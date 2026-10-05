#include "interp.h"
#include "ceval_macros.h" // TODO remove?

#if (defined(__GNUC__) && __GNUC__ >= 10 && !defined(__clang__)) && defined(__x86_64__)
/*
 * gh-129987: The SLP autovectorizer can cause poor code generation for
 * opcode dispatch in some GCC versions (observed in GCCs 12 through 15,
 * probably caused by https://gcc.gnu.org/bugzilla/show_bug.cgi?id=115777),
 * negating any benefit we get from vectorization elsewhere in the
 * interpreter loop. Disabling it significantly affected older GCC versions
 * (prior to GCC 9, 40% performance drop), so we have to selectively disable
 * it.
 */
#define DONT_SLP_VECTORIZE __attribute__((optimize ("no-tree-slp-vectorize", "no-omit-frame-pointer")))
#else
#define DONT_SLP_VECTORIZE
#endif

static const _Py_CODEUNIT _Py_INTERPRETER_TRAMPOLINE_INSTRUCTIONS[] = {
    /* Put a NOP at the start, so that the IP points into
    * the code, rather than before it */
    { .op.code = NOP, .op.arg = 0 },
    { .op.code = INTERPRETER_EXIT, .op.arg = 0 },  /* reached on return */
    { .op.code = NOP, .op.arg = 0 },
    { .op.code = INTERPRETER_EXIT, .op.arg = 0 },  /* reached on yield */
    { .op.code = RESUME, .op.arg = RESUME_OPARG_DEPTH1_MASK | RESUME_AT_FUNC_START },
    { .op.code = CACHE, .op.arg = 0 } /* RESUME's CACHE */
};

const _Py_CODEUNIT *_Py_INTERPRETER_TRAMPOLINE_INSTRUCTIONS_PTR = (_Py_CODEUNIT*)&_Py_INTERPRETER_TRAMPOLINE_INSTRUCTIONS;


PyObject* _Py_HOT_FUNCTION DONT_SLP_VECTORIZE
_PyEval_EvalFrameDefault(PyThreadState *tstate, _PyInterpreterFrame *frame, int throwflag)
{
    _Py_EnsureTstateNotNULL(tstate);
    check_invalid_reentrancy();
    CALL_STAT_INC(pyeval_calls);

#if USE_COMPUTED_GOTOS && !_Py_TAIL_CALL_INTERP
/* Import the static jump table */
#include "opcode_targets.h"
    void **opcode_targets = opcode_targets_table;
#endif

#ifdef Py_STATS
    int lastopcode = 0;
#endif
#if !_Py_TAIL_CALL_INTERP
    uint8_t opcode;    /* Current opcode */
    int oparg;         /* Current opcode argument, if any */
    assert(tstate->current_frame == NULL || tstate->current_frame->stackpointer != NULL);
#if !USE_COMPUTED_GOTOS
    uint8_t tracing_mode = 0;
    uint8_t dispatch_code;
#endif
#endif
    _PyEntryFrame entry;

    if (_Py_EnterRecursiveCallTstate(tstate, "")) {
        assert(frame->owner != FRAME_OWNED_BY_INTERPRETER);
        _PyEval_FrameClearAndPop(tstate, frame);
        return NULL;
    }

    /* Local "register" variables.
     * These are cached values from the frame and code object.  */
    _Py_CODEUNIT *next_instr;
    _PyStackRef *stack_pointer;
    entry.stack[0] = PyStackRef_NULL;
#ifdef Py_STACKREF_DEBUG
    entry.frame.f_funcobj = PyStackRef_None;
#elif defined(Py_DEBUG)
    /* Set these to invalid but identifiable values for debugging. */
    entry.frame.f_funcobj = (_PyStackRef){.bits = 0xaaa0};
    entry.frame.f_locals = (PyObject*)0xaaa1;
    entry.frame.frame_obj = (PyFrameObject*)0xaaa2;
    entry.frame.f_globals = (PyObject*)0xaaa3;
    entry.frame.f_builtins = (PyObject*)0xaaa4;
#endif
    entry.frame.f_executable = PyStackRef_None;
    entry.frame.instr_ptr = (_Py_CODEUNIT *)_Py_INTERPRETER_TRAMPOLINE_INSTRUCTIONS + 1;
    entry.frame.stackpointer = entry.stack;
    entry.frame.owner = FRAME_OWNED_BY_INTERPRETER;
    entry.frame.visited = 0;
    entry.frame.return_offset = 0;
#ifdef Py_DEBUG
    entry.frame.lltrace = 0;
    entry.frame.stackpointer_valid = 1;
#endif
    /* Push frame */
    entry.frame.previous = tstate->current_frame;
    frame->previous = &entry.frame;
    tstate->current_frame = frame;
    entry.frame.localsplus[0] = PyStackRef_NULL;
#ifdef _Py_TIER2
    if (tstate->current_executor != NULL) {
        assert(Py_TYPE(tstate->current_executor) == &_PyUOpExecutor_Type);
        entry.frame.localsplus[0] = PyStackRef_FromPyObjectNew(tstate->current_executor);
        tstate->current_executor = NULL;
    }
#endif

    /* support for generator.throw() */
    if (throwflag) {
        if (_Py_EnterRecursivePy(tstate)) {
            goto early_exit;
        }
#ifdef Py_GIL_DISABLED
        /* Load thread-local bytecode */
        if (frame->tlbc_index != ((_PyThreadStateImpl *)tstate)->tlbc_index) {
            _Py_CODEUNIT *bytecode =
                _PyEval_GetExecutableCode(tstate, _PyFrame_GetCode(frame));
            if (bytecode == NULL) {
                goto early_exit;
            }
            ptrdiff_t off = frame->instr_ptr - _PyFrame_GetBytecode(frame);
            frame->tlbc_index = ((_PyThreadStateImpl *)tstate)->tlbc_index;
            frame->instr_ptr = bytecode + off;
        }
#endif
        /* Because this avoids the RESUME, we need to update instrumentation */
        _Py_Instrument(_PyFrame_GetCode(frame), tstate->interp);
        next_instr = frame->instr_ptr;
        monitor_throw(tstate, frame, next_instr);
        stack_pointer = _PyFrame_GetStackPointer(frame);
        _PyFrame_StackPointerInvalidate(frame);
#if _Py_TAIL_CALL_INTERP
#   if Py_STATS
        return _TAIL_CALL_error(frame, stack_pointer, tstate, next_instr, instruction_funcptr_handler_table, 0, lastopcode);
#   else
        return _TAIL_CALL_error(frame, stack_pointer, tstate, next_instr, instruction_funcptr_handler_table, 0);
#   endif
#else
        goto error;
#endif
    }

#if _Py_TAIL_CALL_INTERP
#   if Py_STATS
        return _TAIL_CALL_start_frame(frame, NULL, tstate, NULL, instruction_funcptr_handler_table, 0, lastopcode);
#   else
        return _TAIL_CALL_start_frame(frame, NULL, tstate, NULL, instruction_funcptr_handler_table, 0);
#   endif
#else
    goto start_frame;
#   include "generated_cases.c.h"
#endif


early_exit:
    assert(_PyErr_Occurred(tstate));
    _Py_LeaveRecursiveCallPy(tstate);
    assert(frame->owner != FRAME_OWNED_BY_INTERPRETER);
    // GH-99729: We need to unlink the frame *before* clearing it:
    _PyInterpreterFrame *dying = frame;
    frame = tstate->current_frame = dying->previous;
    _PyEval_FrameClearAndPop(tstate, dying);
    frame->return_offset = 0;
    assert(frame->owner == FRAME_OWNED_BY_INTERPRETER);
    /* Restore previous frame and exit */
    tstate->current_frame = frame->previous;
    return NULL;
}
