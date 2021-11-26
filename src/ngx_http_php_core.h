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

#ifndef __NGX_HTTP_PHP_CORE_H__
#define __NGX_HTTP_PHP_CORE_H__

#include <ngx_http.h>
#include <php_embed.h>
#include "php/impl/php_ngx.h"
#include "php/impl/php_ngx_sockets.h"

#include "ngx_http_php_socket.h"

#define OUTPUT_CONTENT  1<<0
#define OUTPUT_OPCODE   1<<1
#define OUTPUT_STACK    1<<2

extern ngx_http_request_t *ngx_php_request;

typedef struct ngx_http_php_state_s {
    unsigned php_init;
    unsigned php_shutdown;
} ngx_http_php_state_t;

typedef enum code_type_s {
    NGX_HTTP_PHP_CODE_TYPE_FILE,
    NGX_HTTP_PHP_CODE_TYPE_STRING
} code_type_t;

typedef struct ngx_http_php_code_s {
    union code {
        char *file;
        char *string;
    } code;
    code_type_t code_type;
    ngx_str_t code_id;
} ngx_http_php_code_t;

typedef struct {
    ngx_http_php_code_t *code;
    ngx_str_t key;
    ngx_str_t handler;
} ngx_http_php_variable_t;

#if defined(NDK) && NDK
typedef struct {
    size_t size;
    ngx_str_t var_name;
    ngx_str_t script;
    ngx_http_php_code_t *code;
    ngx_str_t result;
} ngx_http_php_set_var_data_t;
#endif

typedef struct ngx_http_php_rputs_chain_list_s {
    ngx_chain_t **last;
    ngx_chain_t *out;
} ngx_http_php_rputs_chain_list_t;

typedef struct ngx_http_php_capture_node_s {
    ngx_str_t capture_uri;
    ngx_buf_t *capture_buf;
    ngx_str_t capture_str;
} ngx_http_php_capture_node_t;

typedef struct ngx_http_php_ctx_s {
    ngx_http_php_rputs_chain_list_t *rputs_chain;
    size_t body_length;
    ngx_str_t request_body_ctx;
    unsigned request_body_more : 1;
    unsigned read_request_body_done : 1;

    unsigned enable_async : 1;
    unsigned enable_thread : 1;
    unsigned is_capture_multi : 1;
    unsigned is_capture_multi_complete : 1;

    ngx_str_t capture_uri;
    ngx_buf_t *capture_buf;
    ngx_str_t capture_str;
    zval *closure;

    ngx_array_t *capture_multi;
    ngx_uint_t capture_multi_complete_total;

    //pthread_mutex_t mutex;
    //pthread_cond_t cond;
    //pthread_t pthread_id;

    ngx_int_t error;

    unsigned output_type;
    unsigned opcode_logo;
    unsigned stack_logo;
    ngx_uint_t stack_depth;
    
    unsigned rewrite_phase : 1;
    unsigned access_phase : 1;
    unsigned content_phase : 1;

    ngx_int_t phase_status;

    zval *generator_closure;

    ngx_int_t delay_time;
    ngx_event_t sleep;

    // socket
    php_ngx_socket_t *php_socket;
    ngx_http_php_socket_upstream_t  *upstream;
    ngx_str_t   host;
    in_port_t   port;
    zval *recv_buf;
    zval *recv_code;

    unsigned end_of_request : 1;

} ngx_http_php_ctx_t;


ngx_http_php_code_t *ngx_http_php_code_from_file(ngx_pool_t *pool, ngx_str_t *code_file_path);
ngx_http_php_code_t *ngx_http_php_code_from_string(ngx_pool_t *pool, ngx_str_t *code_str);

#define NGX_HTTP_PHP_NGX_INIT ngx_http_php_request_init(r);   \
        php_ngx_request_init();                                 \
        zend_first_try {

#define NGX_HTTP_PHP_NGX_SHUTDOWN } zend_catch {        \
        } zend_end_try();                               \
        ngx_http_php_request_clean();           \
        php_ngx_request_shutdown();

// php_ngx run
//ngx_int_t ngx_php_embed_run(ngx_http_request_t *r, ngx_http_php_code_t *code);
ngx_int_t ngx_php_ngx_run(ngx_http_request_t *r, ngx_http_php_state_t *state, ngx_http_php_code_t *code);

ngx_int_t ngx_php_eval_code(ngx_http_request_t *r, ngx_http_php_state_t *state, ngx_http_php_code_t *code);
ngx_int_t ngx_php_eval_file(ngx_http_request_t *r, ngx_http_php_state_t *state, ngx_http_php_code_t *code);

ngx_int_t ngx_php_get_request_status();
ngx_int_t ngx_php_set_request_status(ngx_int_t rc);

// php_ngx sapi call_back
size_t ngx_http_php_code_ub_write(const char *str, size_t str_length );
void ngx_http_php_code_flush(void *server_context);
void ngx_http_php_code_log_message(char *message);
void ngx_http_php_code_register_server_variables(zval *track_vars_array );
int ngx_http_php_code_read_post(char *buffer, uint count_bytes );
char *ngx_http_php_code_read_cookies();
int ngx_http_php_code_header_handler(sapi_header_struct *sapi_header, sapi_header_op_enum op, sapi_headers_struct *sapi_headers );

#if PHP_MAJOR_VERSION >= 8
#if #if PHP_MINOR_VERSION > 0
extern void (*old_zend_error_cb)(int, zend_string *, const uint32_t, zend_string *);
void ngx_php_error_cb(int type, zend_string *error_filename, const uint32_t error_lineno, zend_string *message);
#else
extern void (*old_zend_error_cb)(int, const char *, const uint32_t, zend_string *);
void ngx_php_error_cb(int type, const char *error_filename, const uint32_t error_lineno, zend_string *message);
#endif
#else
extern void (*old_zend_error_cb)(int, const char *, const uint, const char *, va_list);
void ngx_php_error_cb(int type, const char *error_filename, const uint error_lineno, const char *format, va_list args);
#endif

#endif
