/************************************************************\
 * Copyright 2014 Lawrence Livermore National Security, LLC
 * (c.f. AUTHORS, NOTICE.LLNS, COPYING)
 *
 * This file is part of the Flux resource manager framework.
 * For details, see https://github.com/flux-framework.
 *
 * SPDX-License-Identifier: LGPL-3.0
\************************************************************/

#if HAVE_CONFIG_H
#include "config.h"
#endif
#include <flux/core.h>
#include <assert.h>

#include "src/common/libczmqcontainers/czmq_containers.h"
#include "src/common/libutil/errno_safe.h"
#include "waitqueue.h"

struct handler {
    flux_msg_handler_f cb;
    flux_t *h;
    flux_msg_handler_t *mh;
    const flux_msg_t *msg;
    void *arg;
};

#define WAIT_MAGIC 0xafad7777
struct wait_struct {
    int magic;
    int usecount;
    wait_cb_f cb;
    void *cb_arg;
    struct handler hand; /* optional special case */
    int errnum;
    wait_error_f error_cb;
    void *error_arg;
};

#define WAITQUEUE_MAGIC 0xafad7778
struct waitqueue_struct {
    int magic;
    zlist_t *q;
    /* special counter, count entries on 'q' that have messages,
     * i.e. w->hand.msg */
    int msgs_on_queue;
};

int wait_get_usecount (wait_t *w)
{
    return w->usecount;
}

wait_t *wait_create (wait_cb_f cb, void *arg)
{
    wait_t *w = calloc (1, sizeof (*w));
    if (!w)
        return NULL;
    w->magic = WAIT_MAGIC;
    w->cb = cb;
    w->cb_arg = arg;
    return w;
}

wait_t *wait_create_msg_handler (flux_t *h,
                                 flux_msg_handler_t *mh,
                                 const flux_msg_t *msg,
                                 void *arg,
                                 flux_msg_handler_f cb)
{
    wait_t *w = wait_create (NULL, NULL);
    if (w) {
        w->hand.cb = cb;
        w->hand.arg = arg;
        w->hand.h = h;
        w->hand.mh = mh;
        w->hand.msg = flux_msg_incref (msg);
    }
    return w;
}

void wait_destroy (wait_t *w)
{
    if (w) {
        assert (w->magic == WAIT_MAGIC);
        assert (w->usecount == 0);
        flux_msg_decref (w->hand.msg);
        w->magic = ~WAIT_MAGIC;
        free (w);
    }
}

waitqueue_t *wait_queue_create (void)
{
    waitqueue_t *q = calloc (1, sizeof (*q));
    if (!q)
        return NULL;
    if (!(q->q = zlist_new ())) {
        free (q);
        errno = ENOMEM;
        return NULL;
    }
    q->magic = WAITQUEUE_MAGIC;
    return q;
}

void wait_queue_destroy (waitqueue_t *q)
{
    if (q) {
        wait_t *w;
        assert (q->magic == WAITQUEUE_MAGIC);
        while ((w = zlist_pop (q->q))) {
            if (--w->usecount == 0)
                wait_destroy (w);
        }
        zlist_destroy (&q->q);
        q->magic = ~WAITQUEUE_MAGIC;
        free (q);
    }
}

int wait_queue_length (waitqueue_t *q)
{
    assert (q->magic == WAITQUEUE_MAGIC);
    return zlist_size (q->q);
}

int wait_queue_msgs_count (waitqueue_t *q)
{
    assert (q->magic == WAITQUEUE_MAGIC);
    return q->msgs_on_queue;
}

int wait_queue_iter (waitqueue_t *q, wait_iter_cb_f cb, void *arg)
{
    wait_t *w;

    assert (q->magic == WAITQUEUE_MAGIC);
    w = zlist_first (q->q);
    while (w) {
        if (cb)
            cb (w, arg);
        w = zlist_next (q->q);
    }
    return 0;
}

int wait_addqueue (waitqueue_t *q, wait_t *w)
{
    assert (q->magic == WAITQUEUE_MAGIC);
    assert (w->magic == WAIT_MAGIC);
    if (zlist_append (q->q, w) < 0) {
        errno = ENOMEM;
        return -1;
    }
    w->usecount++;
    if (w->hand.msg)
        q->msgs_on_queue++;
    return 0;
}

static void wait_runone (wait_t *w)
{
    if (--w->usecount == 0) {
        if (w->cb)
            w->cb (w->cb_arg);
        else if (w->hand.cb)
            w->hand.cb (w->hand.h, w->hand.mh, w->hand.msg, w->hand.arg);
        wait_destroy (w);
    }
}

int wait_runqueue (waitqueue_t *q)
{
    assert (q->magic == WAITQUEUE_MAGIC);
    /* N.B. for safety on errors, we must copy all elements off of
     * q->q or none, otherwise it's not clear what's to be done
     * otherwise. e.g. if code was
     * while ((w = zlist_pop (q->q))) {
     *    if (zlist_append (cpy, w) < 0) {
     *        what to do on error here?
     *        pop off all of q?
     *        call wait_runone() on cpy but not on rest of q->q?
     *    }
     * }
     */
    if (zlist_size (q->q) > 0) {
        zlist_t *cpy = NULL;
        wait_t *w;
        if (!(cpy = zlist_dup (q->q))) {
            errno = ENOMEM;
            return -1;
        }
        zlist_purge (q->q);
        while ((w = zlist_pop (cpy))) {
            if (w->hand.msg)
                q->msgs_on_queue--;
            wait_runone (w);
        }
        zlist_destroy (&cpy);
    }
    return 0;
}

int wait_aux_set_errnum (wait_t *w, int errnum)
{
    if (w) {
        w->errnum = errnum;
        if (w->error_cb)
            w->error_cb (w, w->errnum, w->error_arg);
    }
    return 0;
}

int wait_aux_get_errnum (wait_t *w)
{
    if (w)
        return w->errnum;
    return -1;
}

int wait_set_error_cb (wait_t *w, wait_error_f cb, void *arg)
{
    if (w) {
        w->error_cb = cb;
        w->error_arg = arg;
    }
    return 0;
}

int wait_destroy_msg (waitqueue_t *q, wait_test_msg_f cb, void *arg)
{
    zlist_t *tmp = NULL;
    wait_t *w;
    int rc = -1;
    int count = 0;

    assert (q->magic == WAITQUEUE_MAGIC);

    w = zlist_first (q->q);
    while (w) {
        if (w->hand.msg && cb != NULL && cb (w->hand.msg, arg)) {
            if (!tmp && !(tmp = zlist_new ())) {
                errno = ENOMEM;
                goto error;
            }
            if (zlist_append (tmp, w) < 0) {
                errno = ENOMEM;
                goto error;
            }
            /* prevent wait_runone from restarting handler by clearing
             * callback function (i.e. if wait is on multiple
             * queues) */
            w->hand.cb = NULL;
            count++;
        }
        w = zlist_next (q->q);
    }
    rc = 0;
    if (tmp) {
        while ((w = zlist_pop (tmp))) {
            zlist_remove (q->q, w);
            q->msgs_on_queue--;
            if (--w->usecount == 0)
                wait_destroy (w);
        }
    }
    rc = count;
error:
    /* if an error occurs above in zlist_new() or zlist_append(),
     * simply destroy the tmp list.  Nothing has been removed off of
     * the original queue yet.  Allow user to handle error as they see
     * fit.
     */
    ERRNO_SAFE_WRAP (zlist_destroy, &tmp);
    return rc;
}

/*
 * vi:tabstop=4 shiftwidth=4 expandtab
 */

