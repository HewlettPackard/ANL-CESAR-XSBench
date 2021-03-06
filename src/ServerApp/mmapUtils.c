/*
 * Copyright (C) 2020 Hewlett Packard Enterprise Development LP.
 * All rights reserved.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * BSD license below:
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 *   * Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *
 *   * Redistributions in binary form must reproduce the above
 *     copyright notice, this list of conditions and the following
 *     disclaimer in the documentation and/or other materials provided
 *     with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "mmapUtils.h"

static int              timeout = TIMEOUT;

static pthread_mutex_t  thread_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t   thread_cond0 = PTHREAD_COND_INITIALIZER;
static pthread_cond_t   thread_cond = PTHREAD_COND_INITIALIZER;
uint64_t                thread_count;
static int              thread_state;

enum {
        STATE_WAIT1,
        STATE_WAIT2,
        STATE_DONE,
};

static int update_error(int old, int new)
{
    if (new < 0 && old >= 0)
        old = new;

    return old;
}

static void stuff_free(struct stuff *stuff)
{
    int                 rc;

    if (!stuff)
        return;

    /* Unmap. */
    if (stuff->mdesc) {
        rc = stuff->ext_ops->munmap(stuff->mdesc);
        if (rc < 0)
            print_func_fi_err(__func__, __LINE__, "ext_munmap", "", rc);
    }

    fab_conn_free(&stuff->fab_conn);
    fab_conn_free(&stuff->fab_listener);
    fab_dom_free(&stuff->fab_dom);

    FD_CLOSE(stuff->sock_fd);
}

static int do_mem_setup(struct stuff *conn)
{
    int                 ret = -EEXIST;
    struct fab_conn     *fab_conn = &conn->fab_conn;
    const struct args   *args = conn->args;
    size_t              req = args->mmap_len * args->threads + page_size;

    ret = fab_mrmem_alloc(fab_conn, &fab_conn->mrmem, req, 0);
    if (ret < 0)
        goto done;
    memset(fab_conn->mrmem.mem, 0, req);
    /* Make sure there are no dirty lines in cache. */
    conn->ext_ops->commit(NULL, fab_conn->mrmem.mem, req, true, true, true);

 done:
    return ret;
}

static int do_mem_xchg(struct stuff *conn, bool client)
{
    int                 ret;
    struct fab_conn     *fab_conn = &conn->fab_conn;
    struct mem_wire_msg mem_msg;

    if (client) {
        ret = sock_recv_fixed_blob(conn->sock_fd, &mem_msg, sizeof(mem_msg));
        if (ret < 0)
            goto done;
        conn->remote_key = be64toh(mem_msg.remote_key);
    } else {
        mem_msg.remote_key = htobe64(fi_mr_key(fab_conn->mrmem.mr));
        ret = sock_send_blob(conn->sock_fd, &mem_msg, sizeof(mem_msg));
        if (ret < 0)
            goto done;
    }

 done:
    return ret;
}

static void *do_client_thread(void *targ)
{
    int                 ret = 0;
    struct thread_data  *thread = targ;
    struct stuff        *conn = thread->conn;
    const struct args   *args = conn->args;
    void                *buf = ((char *)conn->mdesc->addr +
                                args->mmap_len * thread->tidx);
    uint64_t            *p;
    uint64_t            *e;
    uint64_t            start;

    /* Only one thread synchronizes across the wire. */
    mutex_lock(&thread_mutex);
    thread_count++;
    if (thread_count == args->threads) {
        thread_state = STATE_WAIT2;
        cond_broadcast(&thread_cond);
    }
    while (thread_state == STATE_WAIT1)
        cond_wait(&thread_cond, &thread_mutex);
    thread_count--;
    mutex_unlock(&thread_mutex);

    /* Invalidate any prefetched cache lines. */
    conn->ext_ops->commit(conn->mdesc, buf, args->mmap_len, true, true, false);

    /* Total ramp. */
    p = buf;
    e = p + args->mmap_len / sizeof(*p);
    start = get_cycles(NULL);
    for (; p < e; p++) {
        thread->total += *p++;
    }
    thread->lat_read = get_cycles(NULL) - start;

    /* Only one thread synchronizes across the wire. */
    mutex_lock(&thread_mutex);
    thread_count++;
    if (thread->tidx) {
        if (thread_count == args->threads)
            cond_broadcast(&thread_cond0);
        while (thread_state == STATE_WAIT2)
            cond_wait(&thread_cond, &thread_mutex);
    } else if (thread_count != args->threads)
            cond_wait(&thread_cond0, &thread_mutex);
    thread_count--;
    mutex_unlock(&thread_mutex);

    if (!thread->tidx) {
        /* Final handshake. */
        ret = sock_send_blob(conn->sock_fd, NULL, 0);
        if (ret < 0)
            goto done;

        /* Let the threads run. */
        mutex_lock(&thread_mutex);
        thread_state = STATE_DONE;
        mutex_unlock(&thread_mutex);
        cond_broadcast(&thread_cond);
    }

 done:
    thread->status = ret;

    return NULL;
}

static int do_server_one(const struct args *oargs, int conn_fd)
{
    int                 ret;
    struct args         one_args = *oargs;
    struct args         *args = &one_args;
    struct stuff        stuff = {
        .args           = args,
        .sock_fd        = conn_fd,
        .dest_av        = FI_ADDR_UNSPEC
    };
    struct stuff        *conn = &stuff;
    struct fab_dom      *fab_dom = &conn->fab_dom;
    struct fab_conn     *fab_conn = &conn->fab_conn;
    struct fab_conn     *fab_listener = &conn->fab_listener;
    struct cli_wire_msg cli_msg;

    fab_dom_init(fab_dom);
    fab_conn_init(fab_dom, fab_conn);
    fab_conn_init(fab_dom, fab_listener);

    /* Get the client parameters over the socket. */
    ret = sock_recv_fixed_blob(conn->sock_fd, &cli_msg, sizeof(cli_msg));
    if (ret < 0)
        goto done;

    args->mmap_len = be64toh(cli_msg.mmap_len);
    args->threads = be64toh(cli_msg.threads);
    args->once_mode = !!cli_msg.once_mode;

    ret = fab_dom_setup(NULL, NULL, true, PROVIDER, NULL, EP_TYPE, fab_dom);
    if (ret < 0)
        goto done;

    /* Get ext ops and mmap remote region. */
    ret = fi_open_ops(&fab_dom->fabric->fid, FI_ZHPE_OPS_V1, 0,
                      (void **)&conn->ext_ops, NULL);
    if (ret < 0) {
        print_func_err(__func__, __LINE__, "fi_open_ops", FI_ZHPE_OPS_V1, ret);
        goto done;
    }

    ret = fab_ep_setup(fab_conn, NULL, 0, 0);
    if (ret < 0)
        goto done;
    ret = fab_av_xchg(fab_conn, conn->sock_fd, timeout, &conn->dest_av);
    if (ret < 0)
        goto done;

    /* Now let's exchange the memory parameters to the other side. */
    ret = do_mem_setup(conn);
    if (ret < 0)
        goto done;
    ret = do_mem_xchg(conn, false);
    if (ret < 0)
        goto done;

    /* Wait for reads. */
    ret = sock_recv_fixed_blob(conn->sock_fd, NULL, 0);
    if (ret < 0)
        goto done;

 done:
    stuff_free(conn);

    if (ret >= 0)
        ret = (cli_msg.once_mode ? 1 : 0);

    return ret;
}

int hack_do_server(const struct args *args)
{
    int                 ret;
    int                 listener_fd = -1;
    int                 conn_fd = -1;
    struct addrinfo     *resp = NULL;
    int                 oflags = 1;

    ret = do_getaddrinfo(NULL, args->service,
                         AF_INET6, SOCK_STREAM, true, &resp);
    if (ret < 0)
        goto done;
    listener_fd = socket(resp->ai_family, resp->ai_socktype,
                         resp->ai_protocol);
    if (listener_fd == -1) {
        ret = -errno;
        print_func_err(__func__, __LINE__, "socket", "", ret);
        goto done;
    }
    if (setsockopt(listener_fd, SOL_SOCKET, SO_REUSEADDR,
                   &oflags, sizeof(oflags)) == -1) {
        ret = -errno;
        print_func_err(__func__, __LINE__, "setsockopt", "", ret);
        goto done;
    }
    /* None of the usual: no polling; no threads; no cloexec; no nonblock. */
    if (bind(listener_fd, resp->ai_addr, resp->ai_addrlen) == -1) {
        ret = -errno;
        print_func_err(__func__, __LINE__, "bind", "", ret);
        goto done;
    }
    if (listen(listener_fd, BACKLOG) == -1) {
        ret = -errno;
        print_func_err(__func__, __LINE__, "listen", "", ret);
        goto done;
    }
    for (ret = 0; !ret;) {
        conn_fd = accept(listener_fd, NULL, NULL);
        if (conn_fd == -1) {
            ret = -errno;
            print_func_err(__func__, __LINE__, "accept", "", ret);
            goto done;
        }
        ret = do_server_one(args, conn_fd);
    }

 done:
    if (listener_fd != -1)
        close(listener_fd);
    if (resp)
        freeaddrinfo(resp);

    return ret;
}


int stop_client(struct stuff *conn)
{
    stuff_free(conn);

    return 0;
}

int do_client_work(const struct args *args, struct stuff *conn)
{
    int                 ret;
    struct thread_data  *threads = NULL;
    size_t              i;
    void                *retval;
    int                 rc;
    double              time_us;
    double              rate;

    /* Write/read mmap region  */
    threads = calloc(args->threads, sizeof(*threads));
    if (!threads) {
        ret = - ENOMEM;
        goto done;
    }

    ret = 0;
    thread_state = STATE_WAIT1;

    for (i = 0; i < args->threads; i++) {
        threads[i].conn = conn;
        threads[i].tidx = i;
        ret = -pthread_create(&threads[i].thread, NULL, do_client_thread,
                              &threads[i]);
        if (ret < 0) {
            print_func_err(__func__, __LINE__, "pthread_create", "", ret);
            break;
        }
    }
    for (i = 0; i < args->threads; i++) {
        if (!threads[i].conn)
            break;
        if (ret < 0)
            (void)pthread_cancel(threads[i].thread);
        rc = -pthread_join(threads[i].thread, &retval);
        ret = update_error(ret, rc);
        if (rc < 0)
            continue;
        if (retval == PTHREAD_CANCELED) {
            ret = update_error(ret, -EINTR);
            continue;
        }
        ret = update_error(ret, threads[i].status);
    }
    for (i = 0; i < args->threads; i++) {
        time_us = cycles_to_usec(threads[i].lat_read, 1);
        rate = (double)args->mmap_len / time_us;
        printf("%s:thread %lu len %lu time, us %.3f  MB/s %.3f\n",
               appname, i, args->mmap_len, time_us, rate);
    }

 done:
    free(threads);

    return ret;
}

int start_client(const struct args *args, struct stuff *conn)
{
    int                 ret;
    struct fab_dom      *fab_dom = &conn->fab_dom;
    struct fab_conn     *fab_conn = &conn->fab_conn;
    struct fab_conn     *fab_listener = &conn->fab_listener;
    struct cli_wire_msg cli_msg;

    /* Same initializations as in original do_client */ 
    conn->args = args;
    conn->sock_fd = -1;
    conn->dest_av = FI_ADDR_UNSPEC;

    fab_dom_init(fab_dom);
    fab_conn_init(fab_dom, fab_conn);
    fab_conn_init(fab_dom, fab_listener);

    ret = connect_sock(args->node, args->service);
    if (ret < 0)
        goto done;
    conn->sock_fd = ret;

    /* Write the ring parameters to the server. */
    cli_msg.mmap_len = htobe64(args->mmap_len);
    cli_msg.threads = htobe64(args->threads);
    cli_msg.once_mode = args->once_mode;

    ret = sock_send_blob(conn->sock_fd, &cli_msg, sizeof(cli_msg));
    if (ret < 0)
        goto done;

    ret = fab_dom_setup(NULL, NULL, true, PROVIDER, NULL, EP_TYPE, fab_dom);
    if (ret < 0)
        goto done;

    /* Get ext ops and mmap remote region. */
    ret = fi_open_ops(&fab_dom->fabric->fid, FI_ZHPE_OPS_V1, 0,
                      (void **)&conn->ext_ops, NULL);
    if (ret < 0) {
        print_func_err(__func__, __LINE__, "fi_open_ops", FI_ZHPE_OPS_V1, ret);
        goto done;
    }

    ret = fab_ep_setup(fab_conn, NULL, 0, 0);
    if (ret < 0)
        goto done;
    ret = fab_av_xchg(fab_conn, conn->sock_fd, timeout, &conn->dest_av);
    if (ret < 0)
        goto done;

    /* Now let's exchange the memory parameters to the other side. */
    ret = do_mem_xchg(conn, true);
    if (ret < 0)
        goto done;

    ret = conn->ext_ops->mmap(NULL, args->mmap_len * args->threads,
                             PROT_READ | PROT_WRITE,
                             MAP_SHARED, page_size, fab_conn->ep, conn->dest_av,
                             conn->remote_key, FI_ZHPE_MMAP_CACHE_WB,
                             &conn->mdesc);
    if (ret < 0) {
        print_func_err(__func__, __LINE__, "ext_mmap", FI_ZHPE_OPS_V1, ret);
        goto done;
    }

 done:
    return ret;
}
