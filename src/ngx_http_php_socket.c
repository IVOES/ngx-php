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
#include "ngx_http_php_sleep.h"
#include "ngx_http_php_socket.h"
#include "ngx_http_php_zend_uthread.h"

static void ngx_http_php_socket_handler(ngx_event_t *event);

static void ngx_http_php_socket_dummy_handler(ngx_http_request_t *r, 
    ngx_http_php_socket_upstream_t *u);

static void ngx_http_php_socket_resolve_handler(ngx_resolver_ctx_t *ctx);

static int ngx_http_php_socket_resolve_retval_handler(ngx_http_request_t *r, 
    ngx_http_php_socket_upstream_t *u);

static ngx_int_t ngx_php_http_socket_test_connect(ngx_connection_t *c);

static void ngx_http_php_socket_connected_handler(ngx_http_request_t *r, 
    ngx_http_php_socket_upstream_t *u);

static ngx_int_t ngx_http_php_socket_get_peer(ngx_peer_connection_t *pc, 
    void *data);

static void ngx_http_php_socket_free_peer(ngx_peer_connection_t *pc,
    void *data, ngx_uint_t state);

static void ngx_http_php_socket_finalize(ngx_http_request_t *r, 
    ngx_http_php_socket_upstream_t *u);

static ngx_int_t ngx_http_php_socket_upstream_send(ngx_http_request_t *r, 
    ngx_http_php_socket_upstream_t *u);

static void ngx_http_php_socket_send_handler(ngx_http_request_t *r, 
    ngx_http_php_socket_upstream_t *u);

static ngx_int_t ngx_http_php_socket_upstream_recv(ngx_http_request_t *r, 
    ngx_http_php_socket_upstream_t *u);

static void ngx_http_php_socket_upstream_recv_handler(ngx_http_request_t *r, 
    ngx_http_php_socket_upstream_t *u);

//static void ngx_http_php_socket_upstream_recv_wait_handler(ngx_http_request_t *r, 
//    ngx_http_php_socket_upstream_t *u);


static void
ngx_http_php_socket_handler(ngx_event_t *ev)
{
    ngx_connection_t                    *c;
    ngx_http_request_t                  *r;
    ngx_http_php_socket_upstream_t      *u;

    c = ev->data;
    u = c->data;
    r = u->request;

    ngx_php_debug("php socket handler, ev->data: %p, ev->write: %d, ev->read: %d", ev->data, ev->write, !ev->write);

    if (ev->write) {
        u->write_event_handler(r, u);
    }else {
        u->read_event_handler(r, u);
    }

    ngx_http_php_zend_uthread_resume(r);

}

static void 
ngx_http_php_socket_dummy_handler(ngx_http_request_t *r, 
    ngx_http_php_socket_upstream_t *u)
{
    ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, 
                    "ngx_php tcp socket dummy handler");
    ngx_php_debug("php socket dummy handler");
}

static ngx_int_t 
ngx_http_php_socket_get_peer(ngx_peer_connection_t *pc, 
    void *data)
{
    ngx_http_php_keepalive_conf_t   *kc = data;
    ngx_int_t                       rc;

    if (kc->max_cached) {
        rc = ngx_http_php_keepalive_get_peer(pc, kc);
        if (rc != NGX_OK) {
            return NGX_DONE;
        }
    }

    return NGX_OK;
}

static void 
ngx_http_php_socket_free_peer(ngx_peer_connection_t *pc,void *data, ngx_uint_t state)
{
    ngx_http_php_keepalive_conf_t   *kc = data;

    if (kc->max_cached) {
        ngx_http_php_keepalive_free_peer(pc, data, state);
    }
}

static void 
ngx_http_php_socket_resolve_handler(ngx_resolver_ctx_t *ctx)
{
    ngx_http_request_t              *r;
    //ngx_connection_t                *c;
    ngx_http_upstream_resolved_t    *ur;
    ngx_http_php_socket_upstream_t  *u;
    u_char                          *p;
    size_t                          len;
    //ngx_http_php_ctx_t              *php_ctx;

    socklen_t                        socklen;
    struct sockaddr                 *sockaddr;

    ngx_uint_t                      i;

    u = ctx->data;
    r = u->request;
    //c = r->connection;
    ur = u->resolved;

    ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                   "php socket resolve handler");

    ngx_php_debug("php socket resolve handler");

    if (ctx->state) {
        return ;
    }

    ur->naddrs = ctx->naddrs;
    ur->addrs = ctx->addrs;

#if (NGX_DEBUG)
    {
    u_char      text[NGX_SOCKADDR_STRLEN];
    ngx_str_t   addr;
    ngx_uint_t  i;

    addr.data = text;

    for (i = 0; i < ctx->naddrs; i++ ) {
        addr.len = ngx_sock_ntop(ur->addrs[i].sockaddr, ur->addrs[i].socklen,
                                 text, NGX_SOCKADDR_STRLEN, 0);

        ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, 
                       "name was resolved to %V", &addr);
    }
    }
#endif

    if (ur->naddrs == 1) {
        i = 0;
    }else {
        i = ngx_random() % ur->naddrs;
    }

    socklen = ur->addrs[i].socklen;

    sockaddr = ngx_palloc(r->pool, socklen);
    if (sockaddr == NULL) {

    }

    ngx_memcpy(sockaddr, ur->addrs[i].sockaddr, socklen);

    switch (sockaddr->sa_family) {
#if (NGX_HAVE_INET6)
    case AF_INET6:
        ((struct sockaddr_in6 *) sockaddr)->sin6_port = htons(ur->port);
        break;
#endif
    default: /* AF_INET */
        ((struct sockaddr_in *) sockaddr)->sin_port = htons(ur->port);
    }

    p = ngx_pnalloc(r->pool, NGX_SOCKADDR_STRLEN);
    if (p == NULL) {
        return ;
    }

    len = ngx_sock_ntop(sockaddr, socklen, p, NGX_SOCKADDR_STRLEN, 1);
    ur->sockaddr = sockaddr;
    ur->socklen = socklen;

    ur->host.data = p;
    ur->host.len = len;
    ur->naddrs = 1;

    ngx_resolve_name_done(ctx);
    ur->ctx = NULL;

    ngx_http_php_socket_resolve_retval_handler(r, u);

    return ;
}

static int 
ngx_http_php_socket_resolve_retval_handler(ngx_http_request_t *r, 
    ngx_http_php_socket_upstream_t *u)
{
    ngx_int_t                       rc;
    ngx_http_php_ctx_t              *ctx;
    ngx_peer_connection_t           *peer;
    ngx_connection_t                *c;
    ngx_http_upstream_resolved_t    *ur;
    ngx_http_php_srv_conf_t         *pscf;

    ctx = ngx_http_get_module_ctx(r, ngx_http_php_module);
    pscf = ngx_http_get_module_srv_conf(r, ngx_http_php_module);

    peer = &u->peer;

    ngx_php_debug("%p", peer);

    ur = u->resolved;

    if (ur->sockaddr) {
        peer->sockaddr = ur->sockaddr;
        peer->socklen = ur->socklen;
        peer->name = &ur->host;
    }else {
        return NGX_ERROR;
    }

    peer->data = pscf->keepalive_conf;
    peer->get = ngx_http_php_socket_get_peer;
    peer->free = ngx_http_php_socket_free_peer;

    rc = ngx_event_connect_peer(peer);

    ngx_php_debug("rc: %d %p, peer->cached:%d", (int)rc, ctx->generator_closure, peer->cached);

    if (rc == NGX_ERROR) {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "connect peer error.");
        return NGX_ERROR;
    }

    if (rc == NGX_BUSY) {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "no live upstreams");
        return NGX_ERROR;
    }

    if (rc == NGX_DECLINED) {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "connect peer declined.");
        return NGX_ERROR;
    }

    /* rc == NGX_OK || rc == NGX_AGAIN || rc == NGX_DONE */

    //ctx->phase_status = NGX_AGAIN;

    c = peer->connection;
    c->data = u;
    u->request = r;

    c->write->handler = ngx_http_php_socket_handler;
    c->read->handler = ngx_http_php_socket_handler;

    u->write_event_handler = (ngx_http_php_socket_upstream_handler_pt )ngx_http_php_socket_connected_handler;
    u->read_event_handler = (ngx_http_php_socket_upstream_handler_pt )ngx_http_php_socket_connected_handler;

    c->sendfile &= r->connection->sendfile;

    if (c->pool == NULL) {

        /* we need separate pool here to be able to cache SSL connections */

        c->pool = ngx_create_pool(128, r->connection->log);
        if (c->pool == NULL) {
            return NGX_ERROR;
        }
    }

    c->log = r->connection->log;
    c->pool->log = c->log;
    c->read->log = c->log;
    c->write->log = c->log;

    /* init or reinit the ngx_output_chain() and ngx_chain_writer() contexts */

    if (rc == NGX_OK) {
        ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, 
                       "php socket connected: fd:%d", (int) c->fd);
        ngx_php_debug("php socket connected: fd:%d", (int) c->fd);

        ctx->phase_status = NGX_OK;

        if (ngx_handle_write_event(c->write, 0) != NGX_OK) {

            return NGX_ERROR;
        }

        if (ngx_handle_read_event(c->read, 0) != NGX_OK) {

            return NGX_ERROR;
        }

        u->read_event_handler = (ngx_http_php_socket_upstream_handler_pt) ngx_http_php_socket_dummy_handler;
        u->write_event_handler = (ngx_http_php_socket_upstream_handler_pt) ngx_http_php_socket_dummy_handler;
    
        return NGX_OK;
    }

    // rc == NGX_AGAIN
    
    ctx->phase_status = NGX_AGAIN;
    
    ngx_php_debug("c->read->active:%d,c->read->timer_set:%d,c->read->ready:%d", c->read->active, c->read->timer_set, c->read->ready);
    ngx_php_debug("c->write->active:%d,c->write->timer_set:%d,c->write->ready:%d", c->write->active, c->write->timer_set, c->write->ready);

    if (rc == NGX_AGAIN){
        ngx_add_timer(c->write, u->connect_timeout);
    }

    return NGX_AGAIN;
}

static ngx_int_t
ngx_php_http_socket_test_connect(ngx_connection_t *c)
{
    int        err;
    socklen_t  len;

#if (NGX_HAVE_KQUEUE)

    if (ngx_event_flags & NGX_USE_KQUEUE_EVENT)  {
        if (c->write->pending_eof || c->read->pending_eof) {
            if (c->write->pending_eof) {
                err = c->write->kq_errno;

            } else {
                err = c->read->kq_errno;
            }

            c->log->action = "connecting to upstream";
            (void) ngx_connection_error(c, err,
                                    "kevent() reported that connect() failed");
            return NGX_ERROR;
        }

    } else
#endif
    {
        err = 0;
        len = sizeof(int);

        /*
         * BSDs and Linux return 0 and set a pending error in err
         * Solaris returns -1 and sets errno
         */

        if (getsockopt(c->fd, SOL_SOCKET, SO_ERROR, (void *) &err, &len)
            == -1)
        {
            err = ngx_socket_errno;
        }

        if (err) {
            c->log->action = "connecting to upstream";
            (void) ngx_connection_error(c, err, "connect() failed");
            return NGX_ERROR;
        }
    }

    return NGX_OK;
}

static void 
ngx_http_php_socket_finalize(ngx_http_request_t *r, 
    ngx_http_php_socket_upstream_t *u)
{
    ngx_connection_t        *c;

    ngx_php_debug("request: %p, u: %p, u->cleanup: %p", r, u, u->cleanup);

    ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                   "php finalize socket");

    if (u->cleanup) {
        *u->cleanup = NULL;
        u->cleanup = NULL;
    }

    if (u->resolved && u->resolved->ctx) {
        ngx_resolve_name_done(u->resolved->ctx);
        u->resolved->ctx = NULL;
    }

    if (u->peer.free && u->peer.sockaddr) {
        u->peer.free(&u->peer, u->peer.data, 0);
        u->peer.sockaddr = NULL;
        //return ;
    }

    c = u->peer.connection;
    if (c) {
        ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                       "close http upstream connection: %d",
                       c->fd);

        if (c->pool) {
            ngx_destroy_pool(c->pool);
            c->pool = NULL;
        }

        ngx_close_connection(c);
    }

    ngx_php_debug("socket end");

}

static void 
ngx_http_php_socket_connected_handler(ngx_http_request_t *r, 
    ngx_http_php_socket_upstream_t *u)
{
    ngx_connection_t            *c;

    c = u->peer.connection;

    ngx_php_debug("php socket connected handler");

    if (ngx_php_http_socket_test_connect(c) != NGX_OK) {
        // TODO connect faild handler
    }

    if (c->write->timedout) {
        ngx_php_debug("php socket connecte timedout");
    }

    if (c->write->timer_set) {
        ngx_del_timer(c->write);
    }

    u->read_event_handler = (ngx_http_php_socket_upstream_handler_pt) ngx_http_php_socket_dummy_handler;
    u->write_event_handler = (ngx_http_php_socket_upstream_handler_pt) ngx_http_php_socket_dummy_handler;
}

static ngx_int_t 
ngx_http_php_socket_upstream_send(ngx_http_request_t *r, 
    ngx_http_php_socket_upstream_t *u)
{
    ngx_int_t           n;
    ngx_connection_t    *c;
    ngx_http_php_ctx_t  *ctx;
    ngx_buf_t           *b;

    c = u->peer.connection;

    ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, 
                   "php socket send data");
    ngx_php_debug("php socket send data");

    ctx = ngx_http_get_module_ctx(r, ngx_http_php_module);
    if (ctx == NULL) {
        return NGX_ERROR;
    }

    b = u->request_bufs->buf;

    for (;;) {
        //ngx_php_debug("%s, %d", b->pos, (int)(b->last - b->pos));
        n = c->send(c, b->pos, b->last - b->pos);
    
        if (n >= 0) {
            b->pos += n;

            if (b->pos == b->last) {
                ngx_log_debug0(NGX_LOG_DEBUG_HTTP, c->log, 0, 
                               "php socket send all the data");
                ngx_php_debug("php socket send all the data");

                if (c->write->timer_set) {
                    ngx_del_timer(c->write);
                }

                ngx_chain_update_chains(r->pool, &u->free_bufs, &u->busy_bufs, &u->request_bufs,
                    (ngx_buf_tag_t) &ngx_http_php_module);

                u->write_event_handler = (ngx_http_php_socket_upstream_handler_pt) ngx_http_php_socket_dummy_handler;

                if (ngx_handle_write_event(c->write, 0) != NGX_OK) {
                    
                    return NGX_ERROR;
                }

                //ngx_http_php_socket_handler(c->write);
                return NGX_OK;
            }

            /* keep sending more data */
            continue;
        }

        /* NGX_ERROR || NGX_AGAIN */
        break;
    }

    if (n == NGX_ERROR) {

        return NGX_ERROR;
    }

    /* n == NGX_AGAIN */

    ctx->phase_status = NGX_AGAIN;

    u->write_event_handler = (ngx_http_php_socket_upstream_handler_pt) ngx_http_php_socket_send_handler;

    ngx_add_timer(c->write, u->write_timeout);

    if (ngx_handle_write_event(c->write, 0) != NGX_OK) {
        return NGX_ERROR;
    }

    return NGX_AGAIN;


}

static void 
ngx_http_php_socket_send_handler(ngx_http_request_t *r, 
    ngx_http_php_socket_upstream_t *u)
{
    ngx_connection_t                    *c;
    ngx_http_php_loc_conf_t             *plcf;

    c = u->peer.connection;

    ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, 
                   "php socket send handler.");
    ngx_php_debug("php socket send handler.");

    if (c->write->timedout) {
        plcf = ngx_http_get_module_loc_conf(r, ngx_http_php_module);

        if (plcf->log_socket_errors) {
            ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, 
                          "php socket write timed out.");
        }

        return ;
    }

    if (u->request_bufs) {
        (void) ngx_http_php_socket_upstream_send(r, u);
    }

}

static ngx_int_t 
ngx_http_php_socket_upstream_recv(ngx_http_request_t *r, 
    ngx_http_php_socket_upstream_t *u)
{
    //ngx_int_t                         rc;
    ngx_connection_t                    *c;
    ngx_event_t                         *rev;
    ngx_buf_t                           *b;
    size_t                              size;
    ssize_t                             n = 0;
    unsigned                            read;

    ngx_http_php_ctx_t                  *ctx;

    ctx = ngx_http_get_module_ctx(r, ngx_http_php_module);

    c = u->peer.connection;
    rev = c->read;

    ngx_log_debug0(NGX_LOG_DEBUG_HTTP, c->log, 0, 
                   "php socket receive data");
    ngx_php_debug("php socket receive data");

    b = &u->buffer;
    read = 0;

    if (b->start == NULL) {
        b->start = ngx_palloc(r->pool, u->buffer_size);

        b->pos = b->start;
        b->last = b->start;
        b->end = b->start + u->buffer_size;
        b->temporary = 1;
    }

    for (;;) {
        if (read && !rev->ready) {
            //rc = NGX_AGAIN;
            break;
        }

        size = b->end - b->last;

        if (size == 0) {
            b->start = NULL;
            break;
        }

#if 0
        if (u->enabled_receive_page && rev->active && !rev->ready) {
            ngx_php_debug("c->read->active: %d, c->read->ready: %d, c->read->eof: %d, c->read->write: %d, c->read->posted: %d\n", 
                        c->read->active, c->read->ready, c->read->eof, c->read->write, c->read->posted);
            Z_LVAL_P(ctx->recv_code) = NGX_AGAIN;
            return NGX_AGAIN;
        }
#endif

#if 1
        if ( rev->active && !rev->ready ) {
            n = NGX_AGAIN;
            break;
        }
#endif

        n = c->recv(c, b->last, size);
        //ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "%d", n);
        ngx_php_debug("n = c->recv: %d\n", (int)n);

#if 1
        if (u->enabled_receive_page && rev->active && !rev->ready) {
            ngx_php_debug("recv ready: %d", rev->ready);
            //rc = NGX_AGAIN;

            Z_LVAL_P(ctx->recv_code) = n;

            if (n == NGX_ERROR) {
                ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, 
                              "php socket recv error");
                n = -1;
                return NGX_ERROR;
            }

            if ( n > 0 ) {
                b->last += n;
                b->start = NULL;

                ngx_php_debug("buf write in php var.");
                ZVAL_STRINGL(ctx->recv_buf, (char *)b->pos, b->last - b->pos);
                break;
            }

            if (n == NGX_AGAIN) {
                //rc = NGX_AGAIN;
                break;
            }

            if (n == NGX_OK) {
                b->start = NULL;
                break;
            }

            b->last += n;
            b->start = NULL;
            break;

            /*if (u->enabled_receive_page) {
                if (n >= 0 && n <= (int)u->buffer_size) { 
                    b->last += n;
                    b->start = NULL;

                    ngx_php_debug("buf write in php var.");
                    ZVAL_STRINGL(ctx->recv_buf, (char *)b->pos, b->last - b->pos);
                    
                    if (n == (int)u->buffer_size) {
                        n = NGX_AGAIN;
                    }else {
                        u->enabled_receive_page = 0;
                        n = NGX_OK;
                    }
                    Z_LVAL_P(ctx->recv_code) = n;
                    return n;
                }

                if (u->receive_threshold < 1024) {
                    //ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "receive_threshold %d", u->receive_threshold);
                    u->receive_threshold++;
                    n = NGX_AGAIN;
                }else {
                    u->enabled_receive_page = 0;
                    n = NGX_OK;
                }
                Z_LVAL_P(ctx->recv_code) = n;
                return n;
            }

            break;*/
            /*if (ngx_handle_read_event(rev, 0) != NGX_OK) {
                n = -1;
                break;
            }*/

            //ngx_add_timer(rev, 1000);
        }
#endif

        ngx_log_debug4(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                       "rev->active : %d, rev->ready : %d, n = c->recv : %d, b->last : %s", rev->active, rev->ready, n, b->last);

        read = 1;

        ngx_php_debug("recv: %s, %d, %d", b->pos, (int)n, (int) size);
        //printf("*%d, recv: %s, %d, %d\n", (int)r->connection->log->connection, b->pos, (int)n, (int) size);

        if (n == NGX_ERROR) {
            ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, 
                          "php socket recv error");
            n = -1;
            return NGX_ERROR;
        }

        if (n > 0) {
            b->last += n;
            b->start = NULL;

            ngx_php_debug("buf write in php var.");
            ZVAL_STRINGL(ctx->recv_buf, (char *)b->pos, b->last - b->pos);
            //ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "===>%d", n);
            return NGX_AGAIN;
        }

        if (n == NGX_AGAIN) {
            //rc = NGX_AGAIN;
            break;
        }

        if (n == NGX_OK) {
            b->start = NULL;
            break;
        }

        b->last += n;
        b->start = NULL;
        break;

        /*if (n > 0) {
            b->last += n;
            continue;
        }*/

    }

    ngx_php_debug("%d\n", (int)n);
    //ngx_php_debug("recv: %*s, %p, %p, %p, %p",(int)(b->last - b->pos),b->pos, b->pos, b->end, b->start, b->last);

    ctx->phase_status = NGX_AGAIN;

    ngx_php_debug("c->read->active:%d,c->read->timer_set:%d", rev->active, rev->timer_set);

#if 1
    if (ngx_handle_read_event(rev, 0) != NGX_OK) {
        
        return NGX_ERROR;
    }
#endif

    if (rev->active) {
        ngx_add_timer(rev, u->read_timeout);
    }else if (rev->timer_set) {
        ngx_del_timer(rev);
    }

    return n;

}

static void 
ngx_http_php_socket_upstream_recv_handler(ngx_http_request_t *r, 
    ngx_http_php_socket_upstream_t *u)
{
    ngx_connection_t                    *c;
    //ngx_http_php_loc_conf_t             *plcf;

    c = u->peer.connection;

    ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, 
                   "php socket receive handler.");
    ngx_php_debug("php socket receive handler.");

    if (c->read->timedout) {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, 
                          "php socket read timed out.");
        return ;
    }

    u->enabled_receive = 1;

#if 1
    if (c->read->timer_set) {
        ngx_del_timer(c->read);
    }
#endif

    if (u->buffer.start != NULL) {
        (void) ngx_http_php_socket_upstream_recv(r, u);
    }

}

ngx_int_t 
ngx_http_php_socket_connect(ngx_http_request_t *r)
{
    ngx_http_php_ctx_t                  *ctx;
    //ngx_http_php_loc_conf_t             *plcf;
    //ngx_str_t                           host;
    //int                                 port;
    ngx_resolver_ctx_t                  *rctx, temp;
    ngx_http_core_loc_conf_t            *clcf;

    ngx_url_t                           url;

    ngx_int_t                           rc;
    ngx_peer_connection_t               *peer;

    ngx_http_php_socket_upstream_t      *u;

    ngx_connection_t                    *c;

    c = r->connection;

    ctx = ngx_http_get_module_ctx(r, ngx_http_php_module);

    if (ctx->upstream == NULL){
        ctx->upstream = ngx_pcalloc(r->pool, sizeof(ngx_http_php_socket_upstream_t));
    }

    if (c->read->timer_set) {
        ngx_del_timer(c->read);
    }

    //r->keepalive = 0;

    u = ctx->upstream;

    if (u->connect_timeout <= 0) {
        u->connect_timeout = 60000;
    }

    if (u->read_timeout <= 0) {
        u->read_timeout = 60000;
    }
    
    if (u->write_timeout <= 0) {
        u->write_timeout = 60000;
    }

    u->enabled_receive = 0;

    u->receive_threshold = 0;

    u->enabled_receive_page = 0;

    u->request = r;

    peer = &u->peer;

    peer->log = r->connection->log;
    peer->log_error = NGX_ERROR_ERR;

    ngx_php_debug("php peer connection log: %p %p", peer->log, peer);

    ngx_memzero(&url, sizeof(ngx_url_t));

    url.url.len = ctx->host.len;
    url.url.data = ctx->host.data;
    url.default_port = (in_port_t) ctx->port;
    url.no_resolve = 1;

    if (ngx_parse_url(r->pool, &url) != NGX_OK) {
        if (url.err) {
            ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                          "%s in upstream \"%V\"", url.err, &url.url);
        }else {
            ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, 
                          "failed to parse host name \"%s\"", ctx->host.data);
        }
        return NGX_ERROR;
    }

    u->resolved = ngx_pcalloc(r->pool, sizeof(ngx_http_upstream_resolved_t));
    if (u->resolved == NULL) {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "ngx_pcalloc resolved error. %s.", strerror(errno));
        return NGX_ERROR;
    }

    if (url.addrs && url.addrs[0].sockaddr) {
        ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, 
                       "php socket network address given directly");

        u->resolved->sockaddr = url.addrs[0].sockaddr;
        u->resolved->socklen = url.addrs[0].socklen;
        u->resolved->naddrs = 1;
        u->resolved->host = url.addrs[0].name;
    } else {
        u->resolved->host = ctx->host;
        u->resolved->port = (in_port_t) ctx->port;
    }

    // Already real ip address, is not url and not resolve.
    if (u->resolved->sockaddr) {
        rc = ngx_http_php_socket_resolve_retval_handler(r, u);
        if (rc == NGX_AGAIN) {
            return NGX_AGAIN;
        }

        return rc;
    }

    clcf = ngx_http_get_module_loc_conf(r, ngx_http_core_module);

    temp.name = ctx->host;
    rctx = ngx_resolve_start(clcf->resolver, &temp);
    if (rctx == NULL) {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, 
                      "failed to start the resolver");
        return NGX_ERROR;
    }

    if (rctx == NGX_NO_RESOLVER) {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, 
                      "no resolver defined to resolve \"%s\"", ctx->host.data);
        return NGX_ERROR;
    }

    rctx->name = ctx->host;
    rctx->handler = ngx_http_php_socket_resolve_handler;
    rctx->data = u;
    rctx->timeout = clcf->resolver_timeout;

    u->resolved->ctx = rctx;

    if (ngx_resolve_name(rctx) != NGX_OK) {
        ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                       "php tcp socket fail to run resolver immediately");
        return NGX_ERROR;
    }

    return NGX_OK;   
}

void 
ngx_http_php_socket_close(ngx_http_request_t *r)
{
    ngx_http_php_socket_upstream_t      *u;
    ngx_http_php_ctx_t                  *ctx;

    ctx = ngx_http_get_module_ctx(r, ngx_http_php_module);

    u = ctx->upstream;

    if (u == NULL || 
        u->peer.connection == NULL )
    {
        return ;
    }

    u->enabled_receive = 0;

    if (u->request != r) {

    }

    //r->connection->write->handler = ngx_http_php_socket_handler;

    ngx_php_debug("u->peer.connected: %p, r->connection: %p", u->peer.connection, r->connection);

    ngx_http_php_socket_finalize(r, u);

    ctx->delay_time = 0;

    ngx_http_php_sleep(r);

    return ;
}

ngx_int_t 
ngx_http_php_socket_send(ngx_http_request_t *r)
{
    ngx_int_t                           rc;
    ngx_connection_t                    *c;
    ngx_http_php_ctx_t                  *ctx;
    ngx_http_php_socket_upstream_t      *u;

    ctx = ngx_http_get_module_ctx(r, ngx_http_php_module);

    u = ctx->upstream;

    if (u == NULL || u->peer.connection == NULL) {

        return NGX_ERROR;
    }

    u->enabled_receive = 0;

    c = u->peer.connection;

    if (c->tcp_nodelay) {

    }

    if (u->request != r) {

        return NGX_ERROR;
    }

    rc = ngx_http_php_socket_upstream_send(r, u);

    ngx_php_debug("socket send returned %d", (int)rc);

    if (rc == NGX_ERROR) {

        return NGX_ERROR;
    }

    if (rc == NGX_OK) {

        return NGX_OK;
    }

    /* rc == NGX_AGAIN */

    return NGX_AGAIN;
}

ngx_int_t 
ngx_http_php_socket_recv(ngx_http_request_t *r)
{
    ngx_int_t                           rc;
    ngx_http_php_ctx_t                  *ctx;
    ngx_http_php_socket_upstream_t      *u;
    ngx_connection_t                    *c;
    ngx_event_t                         *rev;

    ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, 
                   "php tcp receive");
    ngx_php_debug("php socket receive");

    ctx = ngx_http_get_module_ctx(r, ngx_http_php_module);

    u = ctx->upstream;

    if (u == NULL || u->peer.connection == NULL) {

        return NGX_ERROR;
    }

    if (u->request != r) {

        return NGX_ERROR;
    }

    c = u->peer.connection;
    rev = c->read;

    rc = ngx_http_php_socket_upstream_recv(r, u);

    ngx_php_debug("%d", u->enabled_receive);

    if (u->enabled_receive == 0) {
        u->enabled_receive = 1;
    }else {
        //ctx->delay_time = 0;
        //ngx_http_php_sleep(r);
#if 1
        ngx_php_debug("c->read->active: %d, c->read->ready: %d, c->read->eof: %d, c->read->write: %d, %d\n", 
            c->read->active, c->read->ready, c->read->eof, c->read->write, (int)rc);

        ngx_php_debug("r->connection->read->active: %d, r->connection->read->ready: %d, r->connection->read->eof: %d, r->connection->read->write: %d", 
            r->connection->read->active, r->connection->read->ready, r->connection->read->eof, r->connection->read->write);

        ngx_php_debug("c->write->active: %d, c->write->ready: %d, c->write->eof: %d, c->write->write: %d", 
            c->write->active, c->write->ready, c->write->eof, c->write->write);
#endif

        if (rev->write == 0) {
            if ( ngx_add_event(rev, NGX_READ_EVENT, NGX_CLEAR_EVENT) == NGX_ERROR) {
                return NGX_ERROR;
            }
        }
    }

    if (rc == NGX_AGAIN) {
        
        //return NGX_AGAIN;
    }

    if (rc == NGX_ERROR) {

    }

    if (rc == NGX_OK) {

    }

    /* rc == NGX_AGAIN */

    u->read_event_handler = (ngx_http_php_socket_upstream_handler_pt) ngx_http_php_socket_upstream_recv_handler;

    return rc;
}

ngx_int_t 
ngx_http_php_socket_recv_wait(ngx_http_request_t *r)
{
    ngx_int_t                           rc;
    ngx_http_php_ctx_t                  *ctx;
    ngx_http_php_socket_upstream_t      *u;
    ngx_connection_t                    *c;
    ngx_event_t                         *rev;

    ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, 
                   "php tcp receive wait");
    ngx_php_debug("php socket receive wait");

    ctx = ngx_http_get_module_ctx(r, ngx_http_php_module);

    u = ctx->upstream;

    if (u == NULL || u->peer.connection == NULL) {

        return NGX_ERROR;
    }

    if (u->request != r) {

        return NGX_ERROR;
    }

    c = u->peer.connection;
    rev = c->read;

    rc = ngx_http_php_socket_upstream_recv(r, u);

    ngx_php_debug("%d", u->enabled_receive);

    if (u->enabled_receive == 0) {
        u->enabled_receive = 1;
    }else {
        //ctx->delay_time = 0;
        //ngx_http_php_sleep(r);
        ngx_php_debug("c->read->active: %d, c->read->ready: %d, c->read->eof: %d, c->read->write: %d", 
            c->read->active, c->read->ready, c->read->eof, c->read->write);

        ngx_php_debug("r->connection->read->active: %d, r->connection->read->ready: %d, r->connection->read->eof: %d, r->connection->read->write: %d", 
            r->connection->read->active, r->connection->read->ready, r->connection->read->eof, r->connection->read->write);

        ngx_php_debug("c->write->active: %d, c->write->ready: %d, c->write->eof: %d, c->write->write: %d", 
            c->write->active, c->write->ready, c->write->eof, c->write->write);

        if (u->wait_receive == 0){
            if ( ngx_add_event(rev, NGX_READ_EVENT, NGX_CLEAR_EVENT) == NGX_ERROR) {
                return NGX_ERROR;
            }
            //u->wait_receive = 1;
        }
    }

    if (rc == NGX_AGAIN) {
        
        //return NGX_AGAIN;
    }

    if (rc == NGX_ERROR) {

    }

    if (rc == NGX_OK) {

    }

    /* rc == NGX_AGAIN */

    u->read_event_handler = (ngx_http_php_socket_upstream_handler_pt) ngx_http_php_socket_upstream_recv_handler;

    return NGX_OK;
}

ngx_int_t 
ngx_http_php_socket_recv_sync(ngx_http_request_t *r)
{
    ngx_int_t                           rc;
    ngx_http_php_ctx_t                  *ctx;
    ngx_http_php_socket_upstream_t      *u;

    ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, 
                   "php tcp receive sync");
    ngx_php_debug("php socket receive sync");

    ctx = ngx_http_get_module_ctx(r, ngx_http_php_module);

    u = ctx->upstream;

    if (u == NULL || u->peer.connection == NULL) {

        return NGX_ERROR;
    }

    if (u->request != r) {

        return NGX_ERROR;
    }

    rc = ngx_http_php_socket_upstream_recv(r, u);

    if (rc == NGX_AGAIN) {

        return NGX_AGAIN;
    }

    if (rc == NGX_ERROR) {

        return NGX_ERROR;
    }

    if (rc == NGX_OK) {

        return NGX_OK;
    }

    /* rc == NGX_AGAIN */

    u->read_event_handler = (ngx_http_php_socket_upstream_handler_pt) ngx_http_php_socket_upstream_recv_handler;

    return NGX_OK;
}

void 
ngx_http_php_socket_clear(ngx_http_request_t *r)
{
    ngx_http_php_socket_upstream_t      *u;
    ngx_http_php_ctx_t                  *ctx;

    ctx = ngx_http_get_module_ctx(r, ngx_http_php_module);

    u = ctx->upstream;

    if (u == NULL || 
        u->peer.connection == NULL )
    {
        return ;
    }

    u->enabled_receive = 0;

    if (u->request != r) {

    }

    ngx_php_debug("u->peer.connected: %p, r->connection: %p", u->peer.connection, r->connection);

    ngx_http_php_socket_finalize(r, u);

    return ;
}





