/*
==============================================================================
Copyright (c) 2016-2020, rryqszq4 <rryqszq@gmail.com>
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

* Redistributions of source code must retain the above copyright notice, this
  list of conditions and the following disclaimer.

* Redistributions in binary form must reproduce the above copyright notice,
  this list of conditions and the following disclaimer in the documentation
  and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
==============================================================================
*/

#include "ngx_http_php_module.h"
#include "ngx_http_php8_zend_uthread.h"
#include "ngx_http_php_util.h"

#if PHP_MAJOR_VERSION >= 8

#include <zend_closures.h>
#include <zend_dtrace.h>
#include <zend_execute.h>

#ifdef HAVE_DTRACE
#define php_exception__thrown_semaphore 0
#endif /* HAVE_DTRACE */

static int ngx_http_php_zend_eval_stringl(char *str, size_t str_len, zval *retval_ptr, char *string_name);
static int ngx_http_php_zend_eval_stringl_ex(char *str, size_t str_len, zval *retval_ptr, char *string_name, int handle_exceptions);


static int ngx_http_php__call_user_function_impl(zval *object, zval *function_name, zval *retval_ptr, uint32_t param_count, zval params[], HashTable *named_params);
static int ngx_http_php_zend_call_function(zend_fcall_info *fci, zend_fcall_info_cache *fci_cache);
static void ngx_http_php_zend_throw_exception_internal(zend_object *exception);

static zend_always_inline uint32_t ngx_http_php_zend_get_arg_offset_by_name(
        zend_function *fbc, zend_string *arg_name, void **cache_slot);

static void ZEND_FASTCALL ngx_http_php_zend_param_must_be_ref(const zend_function *func, uint32_t arg_num);

static zval * ZEND_FASTCALL ngx_http_php_zend_handle_named_arg(
        zend_execute_data **call_ptr, zend_string *arg_name,
        uint32_t *arg_num_ptr, void **cache_slot);

static int ngx_http_php_zend_eval_stringl(char *str, size_t str_len, zval *retval_ptr, char *string_name) /* {{{ */
{
    zend_op_array *new_op_array;
    uint32_t original_compiler_options;
    int retval;
    zend_string *code_str;

    if (retval_ptr) {
        code_str = zend_string_concat3(
            "return ", sizeof("return ")-1, str, str_len, ";", sizeof(";")-1);
    } else {
        code_str = zend_string_init(str, str_len, 0);
    }

    /*printf("Evaluating '%s'\n", pv.value.str.val);*/

    original_compiler_options = CG(compiler_options);
    CG(compiler_options) = ZEND_COMPILE_DEFAULT_FOR_EVAL;
    new_op_array = zend_compile_string(code_str, string_name);
    CG(compiler_options) = original_compiler_options;

    if (new_op_array) {
        zval local_retval;

        EG(no_extensions)=1;

        new_op_array->scope = zend_get_executed_scope();

        zend_try {
            ZVAL_UNDEF(&local_retval);
            zend_execute(new_op_array, &local_retval);
        } zend_catch {
            destroy_op_array(new_op_array);
            efree_size(new_op_array, sizeof(zend_op_array));
            zend_bailout();
        } zend_end_try();

        if (Z_TYPE(local_retval) != IS_UNDEF) {
            if (retval_ptr) {
                ZVAL_COPY_VALUE(retval_ptr, &local_retval);
            } else {
                zval_ptr_dtor(&local_retval);
            }
        } else {
            if (retval_ptr) {
                ZVAL_NULL(retval_ptr);
            }
        }

        EG(no_extensions)=0;
        destroy_op_array(new_op_array);
        efree_size(new_op_array, sizeof(zend_op_array));
        retval = SUCCESS;
    } else {
        retval = FAILURE;
    }
    zend_string_release(code_str);
    return retval;
}
/* }}} */

static int ngx_http_php_zend_eval_stringl_ex(char *str, size_t str_len, zval *retval_ptr, char *string_name, int handle_exceptions) /* {{{ */
{
    int result;

    result = ngx_http_php_zend_eval_stringl(str, str_len, retval_ptr, string_name);
    if (handle_exceptions && EG(exception)) {
        zend_exception_error(EG(exception), E_ERROR);
        result = FAILURE;
    }
    return result;
}
/* }}} */


static int ngx_http_php__call_user_function_impl(zval *object, zval *function_name, zval *retval_ptr, uint32_t param_count, zval params[], HashTable *named_params) /* {{{ */
{
    zend_fcall_info fci;

    fci.size = sizeof(fci);
    fci.object = object ? Z_OBJ_P(object) : NULL;
    ZVAL_COPY_VALUE(&fci.function_name, function_name);
    fci.retval = retval_ptr;
    fci.param_count = param_count;
    fci.params = params;
    fci.named_params = named_params;

    return ngx_http_php_zend_call_function(&fci, NULL);
}

int ngx_http_php_zend_call_function(zend_fcall_info *fci, zend_fcall_info_cache *fci_cache) /* {{{ */
{
    uint32_t i;
    zend_execute_data *call, dummy_execute_data;
    zend_fcall_info_cache fci_cache_local;
    zend_function *func;
    uint32_t call_info;
    void *object_or_called_scope;
    zend_class_entry *orig_fake_scope;

    ZVAL_UNDEF(fci->retval);

    if (!EG(active)) {
        return FAILURE; /* executor is already inactive */
    }

    if (EG(exception)) {
        return FAILURE; /* we would result in an instable executor otherwise */
    }

    ZEND_ASSERT(fci->size == sizeof(zend_fcall_info));

    /* Initialize execute_data */
    if (!EG(current_execute_data)) {
        /* This only happens when we're called outside any execute()'s
         * It shouldn't be strictly necessary to NULL execute_data out,
         * but it may make bugs easier to spot
         */
        memset(&dummy_execute_data, 0, sizeof(zend_execute_data));
        EG(current_execute_data) = &dummy_execute_data;
    } else if (EG(current_execute_data)->func &&
               ZEND_USER_CODE(EG(current_execute_data)->func->common.type) &&
               EG(current_execute_data)->opline->opcode != ZEND_DO_FCALL &&
               EG(current_execute_data)->opline->opcode != ZEND_DO_ICALL &&
               EG(current_execute_data)->opline->opcode != ZEND_DO_UCALL &&
               EG(current_execute_data)->opline->opcode != ZEND_DO_FCALL_BY_NAME) {
        /* Insert fake frame in case of include or magic calls */
        dummy_execute_data = *EG(current_execute_data);
        dummy_execute_data.prev_execute_data = EG(current_execute_data);
        dummy_execute_data.call = NULL;
        dummy_execute_data.opline = NULL;
        dummy_execute_data.func = NULL;
        EG(current_execute_data) = &dummy_execute_data;
    }

    if (!fci_cache || !fci_cache->function_handler) {
        char *error = NULL;

        if (!fci_cache) {
            fci_cache = &fci_cache_local;
        }

        if (!zend_is_callable_ex(&fci->function_name, fci->object, IS_CALLABLE_CHECK_SILENT, NULL, fci_cache, &error)) {
            if (error) {
                zend_string *callable_name
                    = zend_get_callable_name_ex(&fci->function_name, fci->object);
                zend_error(E_WARNING, "Invalid callback %s, %s", ZSTR_VAL(callable_name), error);
                efree(error);
                zend_string_release_ex(callable_name, 0);
            }
            if (EG(current_execute_data) == &dummy_execute_data) {
                EG(current_execute_data) = dummy_execute_data.prev_execute_data;
            }
            return FAILURE;
        }

        ZEND_ASSERT(!error);
    }

    func = fci_cache->function_handler;
    if ((func->common.fn_flags & ZEND_ACC_STATIC) || !fci_cache->object) {
        fci->object = NULL;
        object_or_called_scope = fci_cache->called_scope;
        call_info = ZEND_CALL_TOP_FUNCTION | ZEND_CALL_DYNAMIC;
    } else {
        fci->object = fci_cache->object;
        object_or_called_scope = fci->object;
        call_info = ZEND_CALL_TOP_FUNCTION | ZEND_CALL_DYNAMIC | ZEND_CALL_HAS_THIS;
    }

    call = zend_vm_stack_push_call_frame(call_info,
        func, fci->param_count, object_or_called_scope);

    if (UNEXPECTED(func->common.fn_flags & ZEND_ACC_DEPRECATED)) {
        zend_deprecated_function(func);

        if (UNEXPECTED(EG(exception))) {
            zend_vm_stack_free_call_frame(call);
            if (EG(current_execute_data) == &dummy_execute_data) {
                EG(current_execute_data) = dummy_execute_data.prev_execute_data;
                zend_rethrow_exception(EG(current_execute_data));
            }
            return FAILURE;
        }
    }

    for (i=0; i<fci->param_count; i++) {
        zval *param = ZEND_CALL_ARG(call, i+1);
        zval *arg = &fci->params[i];
        if (UNEXPECTED(Z_ISUNDEF_P(arg))) {
            /* Allow forwarding undef slots. This is only used by Closure::__invoke(). */
            ZVAL_UNDEF(param);
            ZEND_ADD_CALL_FLAG(call, ZEND_CALL_MAY_HAVE_UNDEF);
            continue;
        }

        if (ARG_SHOULD_BE_SENT_BY_REF(func, i + 1)) {
            if (UNEXPECTED(!Z_ISREF_P(arg))) {
                if (!ARG_MAY_BE_SENT_BY_REF(func, i + 1)) {
                    /* By-value send is not allowed -- emit a warning,
                     * but still perform the call with a by-value send. */
                    ngx_http_php_zend_param_must_be_ref(func, i + 1);
                    if (UNEXPECTED(EG(exception))) {
                        ZEND_CALL_NUM_ARGS(call) = i;
cleanup_args:
                        zend_vm_stack_free_args(call);
                        zend_vm_stack_free_call_frame(call);
                        if (EG(current_execute_data) == &dummy_execute_data) {
                            EG(current_execute_data) = dummy_execute_data.prev_execute_data;
                        }
                        return FAILURE;
                    }
                }
            }
        } else {
            if (Z_ISREF_P(arg) &&
                !(func->common.fn_flags & ZEND_ACC_CALL_VIA_TRAMPOLINE)) {
                /* don't separate references for __call */
                arg = Z_REFVAL_P(arg);
            }
        }

        ZVAL_COPY(param, arg);
    }

    if (fci->named_params) {
        zend_string *name;
        zval *arg;
        uint32_t arg_num = ZEND_CALL_NUM_ARGS(call) + 1;
        zend_bool have_named_params = 0;
        ZEND_HASH_FOREACH_STR_KEY_VAL(fci->named_params, name, arg) {
            zval *target;
            if (name) {
                void *cache_slot[2] = {NULL, NULL};
                have_named_params = 1;
                target = ngx_http_php_zend_handle_named_arg(&call, name, &arg_num, cache_slot);
                if (!target) {
                    goto cleanup_args;
                }
            } else {
                if (have_named_params) {
                    zend_throw_error(NULL,
                        "Cannot use positional argument after named argument");
                    goto cleanup_args;
                }

                zend_vm_stack_extend_call_frame(&call, arg_num - 1, 1);
                target = ZEND_CALL_ARG(call, arg_num);
            }

            if (ARG_SHOULD_BE_SENT_BY_REF(func, arg_num)) {
                if (UNEXPECTED(!Z_ISREF_P(arg))) {
                    if (!ARG_MAY_BE_SENT_BY_REF(func, arg_num)) {
                        /* By-value send is not allowed -- emit a warning,
                         * but still perform the call with a by-value send. */
                        ngx_http_php_zend_param_must_be_ref(func, arg_num);
                        if (UNEXPECTED(EG(exception))) {
                            goto cleanup_args;
                        }
                    }
                }
            } else {
                if (Z_ISREF_P(arg) &&
                    !(func->common.fn_flags & ZEND_ACC_CALL_VIA_TRAMPOLINE)) {
                    /* don't separate references for __call */
                    arg = Z_REFVAL_P(arg);
                }
            }

            ZVAL_COPY(target, arg);
            if (!name) {
                ZEND_CALL_NUM_ARGS(call)++;
                arg_num++;
            }
        } ZEND_HASH_FOREACH_END();
    }

    if (UNEXPECTED(func->op_array.fn_flags & ZEND_ACC_CLOSURE)) {
        uint32_t call_info;

        GC_ADDREF(ZEND_CLOSURE_OBJECT(func));
        call_info = ZEND_CALL_CLOSURE;
        if (func->common.fn_flags & ZEND_ACC_FAKE_CLOSURE) {
            call_info |= ZEND_CALL_FAKE_CLOSURE;
        }
        ZEND_ADD_CALL_FLAG(call, call_info);
    }

    if (UNEXPECTED(ZEND_CALL_INFO(call) & ZEND_CALL_MAY_HAVE_UNDEF)) {
        if (zend_handle_undef_args(call) == FAILURE) {
            zend_vm_stack_free_args(call);
            zend_vm_stack_free_call_frame(call);
            if (EG(current_execute_data) == &dummy_execute_data) {
                EG(current_execute_data) = dummy_execute_data.prev_execute_data;
            }
            return SUCCESS;
        }
    }

    orig_fake_scope = EG(fake_scope);
    EG(fake_scope) = NULL;
    if (func->type == ZEND_USER_FUNCTION) {
        int call_via_handler = (func->common.fn_flags & ZEND_ACC_CALL_VIA_TRAMPOLINE) != 0;
        const zend_op *current_opline_before_exception = EG(opline_before_exception);
        uint32_t orig_jit_trace_num = EG(jit_trace_num);

        zend_init_func_execute_data(call, &func->op_array, fci->retval);
        zend_execute_ex(call);
        EG(jit_trace_num) = orig_jit_trace_num;
        EG(opline_before_exception) = current_opline_before_exception;
        if (call_via_handler) {
            /* We must re-initialize function again */
            fci_cache->function_handler = NULL;
        }
    } else {
        int call_via_handler = (func->common.fn_flags & ZEND_ACC_CALL_VIA_TRAMPOLINE) != 0;

        ZEND_ASSERT(func->type == ZEND_INTERNAL_FUNCTION);
        ZVAL_NULL(fci->retval);
        call->prev_execute_data = EG(current_execute_data);
        EG(current_execute_data) = call;
        if (EXPECTED(zend_execute_internal == NULL)) {
            /* saves one function call if zend_execute_internal is not used */
            func->internal_function.handler(call, fci->retval);
        } else {
            zend_execute_internal(call, fci->retval);
        }
        EG(current_execute_data) = call->prev_execute_data;
        zend_vm_stack_free_args(call);
        if (UNEXPECTED(ZEND_CALL_INFO(call) & ZEND_CALL_HAS_EXTRA_NAMED_PARAMS)) {
            zend_array_release(call->extra_named_params);
        }

        if (EG(exception)) {
            zval_ptr_dtor(fci->retval);
            ZVAL_UNDEF(fci->retval);
        }

        if (call_via_handler) {
            /* We must re-initialize function again */
            fci_cache->function_handler = NULL;
        }

        /* This flag is regularly checked while running user functions, but not internal
         * So see whether interrupt flag was set while the function was running... */
        if (EG(vm_interrupt)) {
            EG(vm_interrupt) = 0;
            if (EG(timed_out)) {
                zend_timeout();
            } else if (zend_interrupt_function) {
                zend_interrupt_function(EG(current_execute_data));
            }
        }
    }
    EG(fake_scope) = orig_fake_scope;

    zend_vm_stack_free_call_frame(call);

    if (EG(current_execute_data) == &dummy_execute_data) {
        EG(current_execute_data) = dummy_execute_data.prev_execute_data;
    }

    if (UNEXPECTED(EG(exception))) {
        if (UNEXPECTED(!EG(current_execute_data))) {
            ngx_http_php_zend_throw_exception_internal(NULL);
        } else if (EG(current_execute_data)->func &&
                   ZEND_USER_CODE(EG(current_execute_data)->func->common.type)) {
            zend_rethrow_exception(EG(current_execute_data));
        }
    }

    return SUCCESS;
}
/* }}} */


static void ngx_http_php_zend_throw_exception_internal(zend_object *exception) /* {{{ */
{
#ifdef HAVE_DTRACE
    if (DTRACE_EXCEPTION_THROWN_ENABLED()) {
        if (exception != NULL) {
            DTRACE_EXCEPTION_THROWN(ZSTR_VAL(exception->ce->name));
        } else {
            DTRACE_EXCEPTION_THROWN(NULL);
        }
    }
#endif /* HAVE_DTRACE */

    if (exception != NULL) {
        zend_object *previous = EG(exception);
        zend_exception_set_previous(exception, EG(exception));
        EG(exception) = exception;
        if (previous) {
            return;
        }
    }
    if (!EG(current_execute_data)) {
        if (exception && (exception->ce == zend_ce_parse_error || exception->ce == zend_ce_compile_error)) {
            return;
        }
        if (EG(exception)) {
            zend_exception_error(EG(exception), E_ERROR);
            zend_bailout();
        }

        // hack way !!!
        // zend_error_noreturn(E_CORE_ERROR, "Exception thrown without a stack frame");
    }

    if (zend_throw_exception_hook) {
        zend_throw_exception_hook(exception);
    }

    if (!EG(current_execute_data) || !EG(current_execute_data)->func ||
        !ZEND_USER_CODE(EG(current_execute_data)->func->common.type) ||
        EG(current_execute_data)->opline->opcode == ZEND_HANDLE_EXCEPTION) {
        /* no need to rethrow the exception */
        return;
    }
    EG(opline_before_exception) = EG(current_execute_data)->opline;
    EG(current_execute_data)->opline = EG(exception_op);
}

static zend_always_inline uint32_t ngx_http_php_zend_get_arg_offset_by_name(
        zend_function *fbc, zend_string *arg_name, void **cache_slot) {
    if (EXPECTED(*cache_slot == fbc)) {
        return *(uintptr_t *)(cache_slot + 1);
    }

    // TODO: Use a hash table?
    uint32_t num_args = fbc->common.num_args;
    if (EXPECTED(fbc->type == ZEND_USER_FUNCTION)
            || EXPECTED(fbc->common.fn_flags & ZEND_ACC_USER_ARG_INFO)) {
        for (uint32_t i = 0; i < num_args; i++) {
            zend_arg_info *arg_info = &fbc->op_array.arg_info[i];
            if (zend_string_equals(arg_name, arg_info->name)) {
                *cache_slot = fbc;
                *(uintptr_t *)(cache_slot + 1) = i;
                return i;
            }
        }
    } else {
        for (uint32_t i = 0; i < num_args; i++) {
            zend_internal_arg_info *arg_info = &fbc->internal_function.arg_info[i];
            size_t len = strlen(arg_info->name);
            if (len == ZSTR_LEN(arg_name) && !memcmp(arg_info->name, ZSTR_VAL(arg_name), len)) {
                *cache_slot = fbc;
                *(uintptr_t *)(cache_slot + 1) = i;
                return i;
            }
        }
    }

    if (fbc->common.fn_flags & ZEND_ACC_VARIADIC) {
        *cache_slot = fbc;
        *(uintptr_t *)(cache_slot + 1) = fbc->common.num_args;
        return fbc->common.num_args;
    }

    return (uint32_t) -1;
}

static void ZEND_FASTCALL ngx_http_php_zend_param_must_be_ref(const zend_function *func, uint32_t arg_num)
{
    const char *arg_name = get_function_arg_name(func, arg_num);

    zend_error(E_WARNING, "%s%s%s(): Argument #%d%s%s%s must be passed by reference, value given",
        func->common.scope ? ZSTR_VAL(func->common.scope->name) : "",
        func->common.scope ? "::" : "",
        ZSTR_VAL(func->common.function_name),
        arg_num,
        arg_name ? " ($" : "",
        arg_name ? arg_name : "",
        arg_name ? ")" : ""
    );
}

static zval * ZEND_FASTCALL ngx_http_php_zend_handle_named_arg(
        zend_execute_data **call_ptr, zend_string *arg_name,
        uint32_t *arg_num_ptr, void **cache_slot) {
    zend_execute_data *call = *call_ptr;
    zend_function *fbc = call->func;
    uint32_t arg_offset = ngx_http_php_zend_get_arg_offset_by_name(fbc, arg_name, cache_slot);
    if (UNEXPECTED(arg_offset == (uint32_t) -1)) {
        zend_throw_error(NULL, "Unknown named parameter $%s", ZSTR_VAL(arg_name));
        return NULL;
    }

    zval *arg;
    if (UNEXPECTED(arg_offset == fbc->common.num_args)) {
        /* Unknown named parameter that will be collected into a variadic. */
        if (!(ZEND_CALL_INFO(call) & ZEND_CALL_HAS_EXTRA_NAMED_PARAMS)) {
            ZEND_ADD_CALL_FLAG(call, ZEND_CALL_HAS_EXTRA_NAMED_PARAMS);
            call->extra_named_params = zend_new_array(0);
        }

        arg = zend_hash_add_empty_element(call->extra_named_params, arg_name);
        if (!arg) {
            zend_throw_error(NULL, "Named parameter $%s overwrites previous argument",
                ZSTR_VAL(arg_name));
            return NULL;
        }
        *arg_num_ptr = arg_offset + 1;
        return arg;
    }

    uint32_t current_num_args = ZEND_CALL_NUM_ARGS(call);
    // TODO: We may wish to optimize the arg_offset == current_num_args case,
    // which is probably common (if the named parameters are in order of declaration).
    if (arg_offset >= current_num_args) {
        uint32_t new_num_args = arg_offset + 1;
        ZEND_CALL_NUM_ARGS(call) = new_num_args;

        uint32_t num_extra_args = new_num_args - current_num_args;
        zend_vm_stack_extend_call_frame(call_ptr, current_num_args, num_extra_args);
        call = *call_ptr;

        arg = ZEND_CALL_VAR_NUM(call, arg_offset);
        if (num_extra_args > 1) {
            zval *zv = ZEND_CALL_VAR_NUM(call, current_num_args);
            do {
                ZVAL_UNDEF(zv);
                zv++;
            } while (zv != arg);
            ZEND_ADD_CALL_FLAG(call, ZEND_CALL_MAY_HAVE_UNDEF);
        }
    } else {
        arg = ZEND_CALL_VAR_NUM(call, arg_offset);
        if (UNEXPECTED(!Z_ISUNDEF_P(arg))) {
            zend_throw_error(NULL, "Named parameter $%s overwrites previous argument",
                ZSTR_VAL(arg_name));
            return NULL;
        }
    }

    *arg_num_ptr = arg_offset + 1;
    return arg;
}

zend_execute_data *zend_vm_stack_copy_call_frame(zend_execute_data *call, uint32_t passed_args, uint32_t additional_args) /* {{{ */
{
    zend_execute_data *new_call;
    int used_stack = (EG(vm_stack_top) - (zval*)call) + additional_args;

    /* copy call frame into new stack segment */
    new_call = zend_vm_stack_extend(used_stack * sizeof(zval));
    *new_call = *call;
    ZEND_ADD_CALL_FLAG(new_call, ZEND_CALL_ALLOCATED);

    if (passed_args) {
        zval *src = ZEND_CALL_ARG(call, 1);
        zval *dst = ZEND_CALL_ARG(new_call, 1);
        do {
            ZVAL_COPY_VALUE(dst, src);
            passed_args--;
            src++;
            dst++;
        } while (passed_args);
    }

    /* delete old call_frame from previous stack segment */
    EG(vm_stack)->prev->top = (zval*)call;

    /* delete previous stack segment if it became empty */
    if (UNEXPECTED(EG(vm_stack)->prev->top == ZEND_VM_STACK_ELEMENTS(EG(vm_stack)->prev))) {
        zend_vm_stack r = EG(vm_stack)->prev;

        EG(vm_stack)->prev = r->prev;
        efree(r);
    }

    return new_call;
}
/* }}} */

void 
ngx_http_php_zend_uthread_rewrite_inline_routine(ngx_http_request_t *r)
{
    ngx_http_php_ctx_t *ctx;
    ngx_http_php_loc_conf_t *plcf;
    ngx_str_t inline_code;

    plcf = ngx_http_get_module_loc_conf(r, ngx_http_php_module);
    ctx = ngx_http_get_module_ctx(r, ngx_http_php_module);

    ctx->phase_status = NGX_OK;

    ngx_php_request = r;

    ngx_php_set_request_status(NGX_DECLINED);

    inline_code.data = ngx_pnalloc(r->pool, sizeof("function ngx_rewrite_(){  }")-1 + ngx_strlen(plcf->rewrite_inline_code->code.string) + 32);

    inline_code.len = ngx_sprintf(inline_code.data, "function ngx_rewrite_%V(){ %*s }", 
                                        &(plcf->rewrite_inline_code->code_id), 
                                        ngx_strlen(plcf->rewrite_inline_code->code.string),
                                        plcf->rewrite_inline_code->code.string
                                    ) - inline_code.data;

    ngx_php_debug("%*s, %d", (int)inline_code.len, inline_code.data, (int)inline_code.len);

    zend_first_try {

        if (!plcf->enabled_rewrite_inline_compile){
            ngx_http_php_zend_eval_stringl_ex(
                (char *)inline_code.data, 
                inline_code.len, 
                NULL, 
                "ngx_php eval code", 
                1
            );
            plcf->enabled_rewrite_inline_compile = 1;
        }

        ngx_http_php_zend_uthread_create(r, "ngx_rewrite");

    }zend_end_try();
}

void 
ngx_http_php_zend_uthread_access_inline_routine(ngx_http_request_t *r)
{
    ngx_http_php_ctx_t *ctx;
    ngx_http_php_loc_conf_t *plcf;
    ngx_str_t inline_code;

    plcf = ngx_http_get_module_loc_conf(r, ngx_http_php_module);
    ctx = ngx_http_get_module_ctx(r, ngx_http_php_module);

    ctx->phase_status = NGX_OK;

    ngx_php_request = r;

    ngx_php_set_request_status(NGX_DECLINED);

    inline_code.data = ngx_pnalloc(r->pool, sizeof("function ngx_access_(){  }")-1 + ngx_strlen(plcf->access_inline_code->code.string) + 32);

    inline_code.len = ngx_sprintf(inline_code.data, "function ngx_access_%V(){ %*s }", 
                                        &(plcf->access_inline_code->code_id), 
                                        ngx_strlen(plcf->access_inline_code->code.string),
                                        plcf->access_inline_code->code.string
                                    ) - inline_code.data;

    ngx_php_debug("%*s, %d", (int)inline_code.len, inline_code.data, (int)inline_code.len);

    zend_first_try {

        if (!plcf->enabled_access_inline_compile){
            ngx_http_php_zend_eval_stringl_ex(
                (char *)inline_code.data, 
                inline_code.len, 
                NULL, 
                "ngx_php eval code", 
                1
            );
            plcf->enabled_access_inline_compile = 1;
        }

        ngx_http_php_zend_uthread_create(r, "ngx_access");

    }zend_end_try();
}

void 
ngx_http_php_zend_uthread_content_inline_routine(ngx_http_request_t *r)
{
    ngx_http_php_ctx_t *ctx;
    ngx_http_php_loc_conf_t *plcf;
    ngx_str_t inline_code;

    plcf = ngx_http_get_module_loc_conf(r, ngx_http_php_module);
    ctx = ngx_http_get_module_ctx(r, ngx_http_php_module);

    ctx->phase_status = NGX_OK;

    ngx_php_request = r;

    ngx_php_set_request_status(NGX_DECLINED);

#if 0 && (NGX_DEBUG)
    if (plcf->content_inline_code->code_id.data != NULL) {
        ngx_pfree(r->pool, plcf->content_inline_code->code_id.data);

        plcf->content_inline_code->code_id.data = ngx_pnalloc(r->pool, 32);
    if (plcf->content_inline_code->code_id.data == NULL) {
        // todo error log
        return ;
    }
    ngx_sprintf(plcf->content_inline_code->code_id.data, "%08xD%08xD%08xD%08xD",
                (uint32_t) ngx_random(), (uint32_t) ngx_random(),
                (uint32_t) ngx_random(), (uint32_t) ngx_random());
    }
#endif

    inline_code.data = ngx_pnalloc(r->pool, sizeof("function ngx_content_(){  }")-1 + ngx_strlen(plcf->content_inline_code->code.string) + 32);

    inline_code.len = ngx_sprintf(inline_code.data, "function ngx_content_%V(){ %*s }", 
                                        &(plcf->content_inline_code->code_id), 
                                        ngx_strlen(plcf->content_inline_code->code.string),
                                        plcf->content_inline_code->code.string
                                    ) - inline_code.data;

    ngx_php_debug("%*s, %d", (int)inline_code.len, inline_code.data, (int)inline_code.len);

    zend_first_try {
#if 0 && (NGX_DEBUG)
        ngx_http_php_zend_eval_stringl_ex(
            (char *)inline_code.data, 
            inline_code.len, 
            NULL, 
            "ngx_php eval code", 
            1
        );
            plcf->enabled_content_inline_compile = 1;
#else
        if (!plcf->enabled_content_inline_compile){
            //inline_code.data = (u_char *)str_replace((char *)inline_code.data, "ngx::sleep", "yield ngx::sleep");
            //inline_code.len = strlen((char *)inline_code.data);
            ngx_http_php_zend_eval_stringl_ex(
                (char *)inline_code.data, 
                inline_code.len, 
                NULL, 
                "ngx_php eval code", 
                1
            );
            plcf->enabled_content_inline_compile = 1;
        }
#endif        
        ngx_http_php_zend_uthread_create(r, "ngx_content");
    
    }zend_end_try();
}

void 
ngx_http_php_zend_uthread_log_inline_routine(ngx_http_request_t *r)
{
    //ngx_http_php_ctx_t *ctx;
    ngx_http_php_loc_conf_t *plcf;
    ngx_str_t inline_code;

    plcf = ngx_http_get_module_loc_conf(r, ngx_http_php_module);
    //ctx = ngx_http_get_module_ctx(r, ngx_http_php_module);

    //ctx->phase_status = NGX_OK;

    ngx_php_request = r;

    ngx_php_set_request_status(NGX_DECLINED);

    inline_code.data = ngx_pnalloc(r->pool, sizeof("function ngx_log_(){  }")-1 + ngx_strlen(plcf->log_inline_code->code.string) + 32);

    inline_code.len = ngx_sprintf(inline_code.data, "function ngx_log_%V(){ %*s }", 
                                        &(plcf->log_inline_code->code_id), 
                                        ngx_strlen(plcf->log_inline_code->code.string),
                                        plcf->log_inline_code->code.string
                                    ) - inline_code.data;

    ngx_php_debug("%*s, %d", (int)inline_code.len, inline_code.data, (int)inline_code.len);

    zend_first_try {

        if (!plcf->enabled_log_inline_compile){
            ngx_http_php_zend_eval_stringl_ex(
                (char *)inline_code.data, 
                inline_code.len, 
                NULL, 
                "ngx_php eval code", 
                1
            );
            plcf->enabled_log_inline_compile = 1;
        }
        
        ngx_http_php_zend_uthread_create(r, "ngx_log");
    
    }zend_end_try();
}

void 
ngx_http_php_zend_uthread_header_filter_inline_routine(ngx_http_request_t *r)
{
    //ngx_http_php_ctx_t          *ctx;
    ngx_http_php_loc_conf_t     *plcf;
    ngx_str_t                   inline_code;

    plcf = ngx_http_get_module_loc_conf(r, ngx_http_php_module);
    //ctx = ngx_http_get_module_ctx(r, ngx_http_php_module);

    ngx_php_request = r;

    inline_code.data = ngx_pnalloc(r->pool, sizeof("function ngx_header_filter_(){  }")-1 + ngx_strlen(plcf->header_filter_inline_code->code.string) + 32);

    inline_code.len = ngx_sprintf(inline_code.data, "function ngx_header_filter_%V(){ %*s }", 
                                        &(plcf->header_filter_inline_code->code_id), 
                                        ngx_strlen(plcf->header_filter_inline_code->code.string),
                                        plcf->header_filter_inline_code->code.string
                                    ) - inline_code.data;

    ngx_php_debug("%*s, %d", (int)inline_code.len, inline_code.data, (int)inline_code.len);

    zend_first_try {

        if (!plcf->enabled_header_filter_inline_compile) {
            ngx_http_php_zend_eval_stringl_ex(
                (char *)inline_code.data,
                inline_code.len,
                NULL,
                "ngx_php eval code",
                1
            );
            plcf->enabled_header_filter_inline_compile = 1;
        }

        ngx_http_php_zend_uthread_create(r, "ngx_header_filter");

    }zend_end_try();
}

void 
ngx_http_php_zend_uthread_body_filter_inline_routine(ngx_http_request_t *r)
{
    //ngx_http_php_ctx_t          *ctx;
    ngx_http_php_loc_conf_t     *plcf;
    ngx_str_t                   inline_code;

    plcf = ngx_http_get_module_loc_conf(r, ngx_http_php_module);
    //ctx = ngx_http_get_module_ctx(r, ngx_http_php_module);

    ngx_php_request = r;

    inline_code.data = ngx_pnalloc(r->pool, sizeof("function ngx_body_filter_(){  }")-1 + ngx_strlen(plcf->body_filter_inline_code->code.string) + 32);

    inline_code.len = ngx_sprintf(inline_code.data, "function ngx_body_filter_%V(){ %*s }", 
                                        &(plcf->body_filter_inline_code->code_id), 
                                        ngx_strlen(plcf->body_filter_inline_code->code.string),
                                        plcf->body_filter_inline_code->code.string
                                    ) - inline_code.data;

    ngx_php_debug("%*s, %d", (int)inline_code.len, inline_code.data, (int)inline_code.len);

    zend_first_try {

        if (!plcf->enabled_body_filter_inline_compile) {
            ngx_http_php_zend_eval_stringl_ex(
                (char *)inline_code.data,
                inline_code.len,
                NULL,
                "ngx_php eval code",
                1
            );
            plcf->enabled_body_filter_inline_compile = 1;
        }

        ngx_http_php_zend_uthread_create(r, "ngx_body_filter");

    }zend_end_try();
}

void 
ngx_http_php_zend_uthread_file_routine(ngx_http_request_t *r)
{   
    ngx_http_php_ctx_t *ctx;
    ngx_http_php_main_conf_t *pmcf;
    ngx_http_php_loc_conf_t *plcf;

    pmcf = ngx_http_get_module_main_conf(r, ngx_http_php_module);
    plcf = ngx_http_get_module_loc_conf(r, ngx_http_php_module);
    ctx = ngx_http_get_module_ctx(r, ngx_http_php_module);

    ctx->phase_status = NGX_OK;

    ngx_php_request = r;

    ngx_php_set_request_status(NGX_DECLINED);
    zend_first_try {

        ngx_php_eval_file(r, pmcf->state, plcf->rewrite_code);
        
    }zend_end_try();
}

void 
ngx_http_php_zend_uthread_create(ngx_http_request_t *r, char *func_prefix)
{
    zval func_main;
    //zval func_next;
    zval func_valid;
    zval retval;
    ngx_http_php_ctx_t *ctx;
    ngx_http_php_loc_conf_t *plcf;
    ngx_str_t func_name;

    ctx = ngx_http_get_module_ctx(r, ngx_http_php_module);

    if (ctx == NULL) {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "ngx_php ctx is nil at zend_uthread_create");
        return ;
    }
    
    ctx->generator_closure = (zval *)emalloc(sizeof(zval));

    plcf = ngx_http_get_module_loc_conf(r, ngx_http_php_module);

    //func_name.data = ngx_pnalloc(r->pool, strlen(func_prefix)+sizeof("_18446744073709551616")-1+NGX_TIME_T_LEN);
    //func_name.len = ngx_sprintf(func_name.data, "%s_%V", func_prefix, &(plcf->content_inline_code->code_id)) - func_name.data;

    func_name.data = ngx_pnalloc(r->pool, strlen(func_prefix) + 32);

    if (strcmp(func_prefix, "ngx_rewrite") == 0) {
        func_name.len = ngx_sprintf(func_name.data, "%s_%V", func_prefix, &(plcf->rewrite_inline_code->code_id)) - func_name.data;
    }else if (strcmp(func_prefix, "ngx_access") == 0) {
        func_name.len = ngx_sprintf(func_name.data, "%s_%V", func_prefix, &(plcf->access_inline_code->code_id)) - func_name.data;
    }else if (strcmp(func_prefix, "ngx_content") == 0) {
        func_name.len = ngx_sprintf(func_name.data, "%s_%V", func_prefix, &(plcf->content_inline_code->code_id)) - func_name.data;
    }else if (strcmp(func_prefix, "ngx_log") == 0) {
        func_name.len = ngx_sprintf(func_name.data, "%s_%V", func_prefix, &(plcf->log_inline_code->code_id)) - func_name.data;
    }else if (strcmp(func_prefix, "ngx_header_filter") == 0) {
        func_name.len = ngx_sprintf(func_name.data, "%s_%V", func_prefix, &(plcf->header_filter_inline_code->code_id)) - func_name.data;
    }else if (strcmp(func_prefix, "ngx_body_filter") == 0) {
        func_name.len = ngx_sprintf(func_name.data, "%s_%V", func_prefix, &(plcf->body_filter_inline_code->code_id)) - func_name.data;
    }else {
        func_name.len = 0;
    }

    ngx_php_debug("%*s", (int)func_name.len, func_name.data);

    zend_try {
        ZVAL_STRINGL(&func_main, (char *)func_name.data, func_name.len);
        ngx_http_php_call_user_function(EG(function_table), NULL, &func_main, ctx->generator_closure, 0, NULL);
        zval_ptr_dtor(&func_main);

        if ( !ctx->generator_closure ) {
            return ;
        }

        if (Z_TYPE_P(ctx->generator_closure) == IS_OBJECT){

            ZVAL_STRING(&func_valid, "valid");
            if (ngx_http_php_call_user_function(NULL, ctx->generator_closure, &func_valid, &retval, 0, NULL) == FAILURE)
            {
                php_error_docref(NULL , E_WARNING, "Failed calling valid");
                return ;
            }
            zval_ptr_dtor(&func_valid);

            ngx_php_debug("r:%p, closure:%p, retval:%d", r, ctx->generator_closure, Z_TYPE(retval));

            if (Z_TYPE(retval) == IS_TRUE){
                /*
                ZVAL_STRING(&func_next, "next");

                ngx_http_php_call_user_function(NULL, ctx->generator_closure, &func_next, &retval, 0, NULL );

                zval_ptr_dtor(&func_next);
                */
                ctx->phase_status = NGX_AGAIN;
            }else {
                ctx->phase_status = NGX_OK;
            }

        }else {
            ngx_php_debug("r:%p, closure:%p, retval:%d", r, ctx->generator_closure, Z_TYPE(retval));
            zval_ptr_dtor(ctx->generator_closure);
            efree(ctx->generator_closure);
            ctx->generator_closure = NULL;
        }
    }zend_catch {
        zval_ptr_dtor(&func_main);
        if ( ctx && ctx->generator_closure ){
            zval_ptr_dtor(ctx->generator_closure);
            efree(ctx->generator_closure);
            ctx->generator_closure = NULL;
        }
    }zend_end_try();
}

void 
ngx_http_php_zend_uthread_resume(ngx_http_request_t *r)
{
    ngx_php_request = r;

    ngx_http_php_ctx_t *ctx = ngx_http_get_module_ctx(r, ngx_http_php_module);

    if (ctx == NULL) {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "ngx_php ctx is nil at zend_uthread_resume");
        return ;
    }

    ngx_php_debug("ctx: %p", ctx);

    zend_try {
        zval *closure;
        zval func_next;
        zval func_valid;
        zval retval;

        closure = ctx->generator_closure;
        ngx_php_debug("closure: %p", closure);
        if (!closure) {
            if (ctx->upstream) {
                ngx_http_php_socket_clear(r);
            }
            return ;
        }

        // ngx_php_debug("uthread resume before.");

        ZVAL_STRING(&func_next, "next");
        if ( ngx_http_php_call_user_function(NULL, closure, &func_next, &retval, 0, NULL ) == FAILURE )
        {
            php_error_docref(NULL , E_WARNING, "Failed calling next");
            return ;
        }
        zval_ptr_dtor(&func_next);

        /*
        错误：变量‘ctx’能为‘longjmp’或‘vfork’所篡改 [-Werror=clobbered]
        错误：实参‘r’可能为‘longjmp’或‘vfork’所篡改 [-Werror=clobbered]
        */
        //r = ngx_php_request;
        //ctx = ngx_http_get_module_ctx(r, ngx_http_php_module);
        ngx_php_debug("%d, %p, %p", Z_TYPE_P(closure), r, ctx);
        if ( ctx->end_of_request ) {
            //zval_ptr_dtor(closure);
            //efree(closure);
            //closure = NULL;
            ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "End of request and zend uthread has be shutdown");
            return;
        }

        ZVAL_STRING(&func_valid, "valid");
        if ( ngx_http_php_call_user_function(NULL, closure, &func_valid, &retval, 0, NULL ) == FAILURE )
        {
            php_error_docref(NULL , E_WARNING, "Failed calling valid");
            return ;
        }
        zval_ptr_dtor(&func_valid);

        ngx_php_debug("r:%p, closure:%p, retval:%d,%d", r, closure, Z_TYPE(retval), IS_TRUE);

        // ngx_php_debug("uthread resume after.");

        if (Z_TYPE(retval) == IS_TRUE) {
            ctx->phase_status = NGX_AGAIN;
        }else {
            ctx->phase_status = NGX_OK;
            
            if ( ctx->generator_closure ) {
                zval_ptr_dtor(ctx->generator_closure);
                efree(ctx->generator_closure);
                ctx->generator_closure = NULL;
            }
            
            ngx_http_core_run_phases(r);
        }

    }zend_catch {
        if ( ctx && ctx->generator_closure ){
            zval_ptr_dtor(ctx->generator_closure);
            efree(ctx->generator_closure);
            ctx->generator_closure = NULL;
        }
    }zend_end_try();
}

void 
ngx_http_php_zend_uthread_exit(ngx_http_request_t *r)
{
    ngx_http_php_ctx_t *ctx;

    ngx_php_request = r;

    ctx = ngx_http_get_module_ctx(r, ngx_http_php_module);

    if (ctx == NULL) {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "ngx_php ctx is nil at zend_uthread_exit");
        return ;
    }

    if ( ctx && ctx->generator_closure ) {
        //ngx_http_php_zend_uthread_resume(r);
        ctx->phase_status = NGX_OK;
        zval_ptr_dtor(ctx->generator_closure);
        efree(ctx->generator_closure);
        ctx->generator_closure = NULL;
    }

    if ( ctx && ctx->upstream ) {
        ngx_http_php_socket_clear(r);
    }

    if ( ctx && ctx->php_socket ) {
        efree(ctx->php_socket);
        ctx->php_socket = NULL;
    }

    ngx_http_core_run_phases(r);

}

#endif

