/*
 *
 * Copyright 2015, Google Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *     * Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 * copyright notice, this list of conditions and the following disclaimer
 * in the documentation and/or other materials provided with the
 * distribution.
 *     * Neither the name of Google Inc. nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

#include "src/core/lib/surface/channel.h"

#include <grpc/support/alloc.h>
#include <grpc/support/log.h>

#include "src/core/ext/filters/client_channel/client_channel.h"
#include "src/core/lib/iomgr/timer.h"
#include "src/core/lib/surface/api_trace.h"
#include "src/core/lib/surface/completion_queue.h"

grpc_connectivity_state grpc_channel_check_connectivity_state(
    grpc_channel *channel, int try_to_connect) {
  /* forward through to the underlying client channel */
  grpc_channel_element *client_channel_elem =
      grpc_channel_stack_last_element(grpc_channel_get_channel_stack(channel));
  grpc_exec_ctx exec_ctx = GRPC_EXEC_CTX_INIT;
  grpc_connectivity_state state;
  GRPC_API_TRACE(
      "grpc_channel_check_connectivity_state(channel=%p, try_to_connect=%d)", 2,
      (channel, try_to_connect));
  if (client_channel_elem->filter == &grpc_client_channel_filter) {
    state = grpc_client_channel_check_connectivity_state(
        &exec_ctx, client_channel_elem, try_to_connect);
    grpc_exec_ctx_finish(&exec_ctx);
    return state;
  }
  gpr_log(GPR_ERROR,
          "grpc_channel_check_connectivity_state called on something that is "
          "not a client channel, but '%s'",
          client_channel_elem->filter->name);
  grpc_exec_ctx_finish(&exec_ctx);
  return GRPC_CHANNEL_SHUTDOWN;
}

typedef enum {
  WAITING,
  READY_TO_CALL_BACK,
  CALLING_BACK_AND_FINISHED,
} callback_phase;

typedef struct {
  gpr_mu mu;
  callback_phase phase;
  grpc_closure on_complete;
  grpc_closure on_timeout;
  grpc_closure watcher_timer_init;
  grpc_timer alarm;
  grpc_connectivity_state state;
  grpc_completion_queue *cq;
  grpc_cq_completion completion_storage;
  grpc_channel *channel;
  grpc_error *error;
  void *tag;
} state_watcher;

static void delete_state_watcher(grpc_exec_ctx *exec_ctx, state_watcher *w) {
  grpc_channel_element *client_channel_elem = grpc_channel_stack_last_element(
      grpc_channel_get_channel_stack(w->channel));
  if (client_channel_elem->filter == &grpc_client_channel_filter) {
    GRPC_CHANNEL_INTERNAL_UNREF(exec_ctx, w->channel,
                                "watch_channel_connectivity");
  } else {
    abort();
  }
  gpr_mu_destroy(&w->mu);
  gpr_free(w);
}

static void finished_completion(grpc_exec_ctx *exec_ctx, void *pw,
                                grpc_cq_completion *ignored) {
  int delete = 0;
  state_watcher *w = pw;
  gpr_mu_lock(&w->mu);
  switch (w->phase) {
    case WAITING:
    case READY_TO_CALL_BACK:
      GPR_UNREACHABLE_CODE(return );
    case CALLING_BACK_AND_FINISHED:
      delete = 1;
      break;
  }
  gpr_mu_unlock(&w->mu);

  if (delete) {
    delete_state_watcher(exec_ctx, w);
  }
}

static void partly_done(grpc_exec_ctx *exec_ctx, state_watcher *w,
                        bool due_to_completion, grpc_error *error) {
  if (due_to_completion) {
    grpc_timer_cancel(exec_ctx, &w->alarm);
  } else {
    grpc_channel_element *client_channel_elem = grpc_channel_stack_last_element(
        grpc_channel_get_channel_stack(w->channel));
    grpc_client_channel_watch_connectivity_state(exec_ctx, client_channel_elem,
                                                 grpc_cq_pollset(w->cq), NULL,
                                                 &w->on_complete, NULL);
  }

  gpr_mu_lock(&w->mu);

  if (due_to_completion) {
    if (GRPC_TRACER_ON(grpc_trace_operation_failures)) {
      GRPC_LOG_IF_ERROR("watch_completion_error", GRPC_ERROR_REF(error));
    }
    GRPC_ERROR_UNREF(error);
    error = GRPC_ERROR_NONE;
  } else {
    if (error == GRPC_ERROR_NONE) {
      error = GRPC_ERROR_CREATE_FROM_STATIC_STRING(
          "Timed out waiting for connection state change");
    } else if (error == GRPC_ERROR_CANCELLED) {
      error = GRPC_ERROR_NONE;
    }
  }
  switch (w->phase) {
    case WAITING:
      GRPC_ERROR_REF(error);
      w->error = error;
      w->phase = READY_TO_CALL_BACK;
      break;
    case READY_TO_CALL_BACK:
      if (error != GRPC_ERROR_NONE) {
        GPR_ASSERT(!due_to_completion);
        GRPC_ERROR_UNREF(w->error);
        GRPC_ERROR_REF(error);
        w->error = error;
      }
      w->phase = CALLING_BACK_AND_FINISHED;
      grpc_cq_end_op(exec_ctx, w->cq, w->tag, w->error, finished_completion, w,
                     &w->completion_storage);
      break;
    case CALLING_BACK_AND_FINISHED:
      GPR_UNREACHABLE_CODE(return );
      break;
  }
  gpr_mu_unlock(&w->mu);

  GRPC_ERROR_UNREF(error);
}

static void watch_complete(grpc_exec_ctx *exec_ctx, void *pw,
                           grpc_error *error) {
  partly_done(exec_ctx, pw, true, GRPC_ERROR_REF(error));
}

static void timeout_complete(grpc_exec_ctx *exec_ctx, void *pw,
                             grpc_error *error) {
  partly_done(exec_ctx, pw, false, GRPC_ERROR_REF(error));
}

int grpc_channel_num_external_connectivity_watchers(grpc_channel *channel) {
  grpc_channel_element *client_channel_elem =
      grpc_channel_stack_last_element(grpc_channel_get_channel_stack(channel));
  return grpc_client_channel_num_external_connectivity_watchers(
      client_channel_elem);
}

typedef struct watcher_timer_init_arg {
  state_watcher *w;
  gpr_timespec deadline;
} watcher_timer_init_arg;

static void watcher_timer_init(grpc_exec_ctx *exec_ctx, void *arg,
                               grpc_error *error_ignored) {
  watcher_timer_init_arg *wa = (watcher_timer_init_arg *)arg;

  grpc_timer_init(exec_ctx, &wa->w->alarm,
                  gpr_convert_clock_type(wa->deadline, GPR_CLOCK_MONOTONIC),
                  &wa->w->on_timeout, gpr_now(GPR_CLOCK_MONOTONIC));
  gpr_free(wa);
}

void grpc_channel_watch_connectivity_state(
    grpc_channel *channel, grpc_connectivity_state last_observed_state,
    gpr_timespec deadline, grpc_completion_queue *cq, void *tag) {
  grpc_channel_element *client_channel_elem =
      grpc_channel_stack_last_element(grpc_channel_get_channel_stack(channel));
  grpc_exec_ctx exec_ctx = GRPC_EXEC_CTX_INIT;
  state_watcher *w = gpr_malloc(sizeof(*w));

  GRPC_API_TRACE(
      "grpc_channel_watch_connectivity_state("
      "channel=%p, last_observed_state=%d, "
      "deadline=gpr_timespec { tv_sec: %" PRId64
      ", tv_nsec: %d, clock_type: %d }, "
      "cq=%p, tag=%p)",
      7, (channel, (int)last_observed_state, deadline.tv_sec, deadline.tv_nsec,
          (int)deadline.clock_type, cq, tag));

  grpc_cq_begin_op(cq, tag);

  gpr_mu_init(&w->mu);
  grpc_closure_init(&w->on_complete, watch_complete, w,
                    grpc_schedule_on_exec_ctx);
  grpc_closure_init(&w->on_timeout, timeout_complete, w,
                    grpc_schedule_on_exec_ctx);
  w->phase = WAITING;
  w->state = last_observed_state;
  w->cq = cq;
  w->tag = tag;
  w->channel = channel;
  w->error = NULL;

  watcher_timer_init_arg *wa = gpr_malloc(sizeof(watcher_timer_init_arg));
  wa->w = w;
  wa->deadline = deadline;
  grpc_closure_init(&w->watcher_timer_init, watcher_timer_init, wa,
                    grpc_schedule_on_exec_ctx);

  if (client_channel_elem->filter == &grpc_client_channel_filter) {
    GRPC_CHANNEL_INTERNAL_REF(channel, "watch_channel_connectivity");
    grpc_client_channel_watch_connectivity_state(
        &exec_ctx, client_channel_elem, grpc_cq_pollset(cq), &w->state,
        &w->on_complete, &w->watcher_timer_init);
  } else {
    abort();
  }

  grpc_exec_ctx_finish(&exec_ctx);
}
