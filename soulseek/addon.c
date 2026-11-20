#include <assert.h>
#include <bare.h>
#include <js.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <uv.h>

#include "soulseek.h"

typedef enum {
  SLSK_EV_LOG = 1,
  SLSK_EV_STATE_DONE = 2,
  SLSK_EV_FILES_DONE = 3,
  SLSK_EV_PROGRESS = 4,
  SLSK_EV_DOWNLOAD_DONE = 5
} slsk_ev_kind_t;

typedef struct bare_slsk_client bare_slsk_client_t;

typedef struct slsk_req {
  bare_slsk_client_t *owner;
  js_deferred_t *deferred;
  js_ref_t *on_progress_ref;
} slsk_req_t;

typedef struct slsk_ev {
  struct slsk_ev *next;
  slsk_ev_kind_t kind;
  slsk_req_t *req;
  char *err;

  /* LOG */
  char *level;
  char *event;
  char *message;

  /* STATE_DONE */
  slsk_state_t state;

  /* FILES_DONE */
  slsk_file_list_t *files;

  /* PROGRESS */
  uint64_t bytes_downloaded;
  uint64_t total_bytes;
  double speed_bytes_per_sec;
  int64_t queue_place;
  char *phase;

  /* DOWNLOAD_DONE */
  slsk_download_result_t *dl_result;
} slsk_ev_t;

struct bare_slsk_client {
  uv_async_t async;
  uv_mutex_t lock;
  slsk_ev_t *head;
  slsk_ev_t *tail;
  uint32_t pending_ops;

  slsk_client_t *rust_client;
  js_env_t *env;
  js_ref_t *logger_ref;
  js_deferred_teardown_t *teardown;

  bool closing;
  bool exiting;
};

static char *
dup_cstr(const char *s) {
  if (s == NULL) return NULL;
  size_t len = strlen(s);
  char *copy = (char *) malloc(len + 1);
  if (copy != NULL) {
    memcpy(copy, s, len + 1);
  }
  return copy;
}

static char *
get_js_utf8(js_env_t *env, js_value_t *val) {
  size_t len = 0;
  int err = js_get_value_string_utf8(env, val, NULL, 0, &len);
  if (err != 0) return NULL;
  char *buf = (char *) malloc(len + 1);
  if (buf == NULL) return NULL;
  err = js_get_value_string_utf8(env, val, (utf8_t *) buf, len + 1, NULL);
  if (err != 0) {
    free(buf);
    return NULL;
  }
  buf[len] = '\0';
  return buf;
}

static void
set_str_prop(js_env_t *env, js_value_t *obj, const char *key, const char *val) {
  js_value_t *v;
  int err = js_create_string_utf8(env, (const utf8_t *) (val ? val : ""), (size_t) -1, &v);
  assert(err == 0);
  err = js_set_named_property(env, obj, key, v);
  assert(err == 0);
}

static void
set_bool_prop(js_env_t *env, js_value_t *obj, const char *key, bool val) {
  js_value_t *v;
  int err = js_get_boolean(env, val, &v);
  assert(err == 0);
  err = js_set_named_property(env, obj, key, v);
  assert(err == 0);
}

static void
set_u32_prop(js_env_t *env, js_value_t *obj, const char *key, uint32_t val) {
  js_value_t *v;
  int err = js_create_uint32(env, val, &v);
  assert(err == 0);
  err = js_set_named_property(env, obj, key, v);
  assert(err == 0);
}

static void
set_i64_prop(js_env_t *env, js_value_t *obj, const char *key, int64_t val) {
  js_value_t *v;
  int err = js_create_int64(env, val, &v);
  assert(err == 0);
  err = js_set_named_property(env, obj, key, v);
  assert(err == 0);
}

static void
set_opt_i64_prop(js_env_t *env, js_value_t *obj, const char *key, int64_t val) {
  js_value_t *v;
  int err;
  if (val < 0) {
    err = js_get_null(env, &v);
  } else {
    err = js_create_int64(env, val, &v);
  }
  assert(err == 0);
  err = js_set_named_property(env, obj, key, v);
  assert(err == 0);
}

static void
set_double_prop(js_env_t *env, js_value_t *obj, const char *key, double val) {
  js_value_t *v;
  int err = js_create_double(env, val, &v);
  assert(err == 0);
  err = js_set_named_property(env, obj, key, v);
  assert(err == 0);
}

static js_value_t *
build_state_object(js_env_t *env, slsk_state_t st) {
  js_value_t *obj;
  int err = js_create_object(env, &obj);
  assert(err == 0);
  set_bool_prop(env, obj, "connected", st.connected != 0);
  set_bool_prop(env, obj, "logged_in", st.logged_in != 0);
  set_u32_prop(env, obj, "privileges", st.privileges);
  set_u32_prop(env, obj, "listen_port", (uint32_t) st.listen_port);
  return obj;
}

static js_value_t *
build_error_value(js_env_t *env, const char *msg) {
  js_value_t *code;
  js_value_t *message;
  js_value_t *err_val;
  int err = js_create_string_utf8(env, (const utf8_t *) "SLSK_ERR", (size_t) -1, &code);
  assert(err == 0);
  err = js_create_string_utf8(env, (const utf8_t *) (msg ? msg : "Soulseek error"), (size_t) -1, &message);
  assert(err == 0);
  err = js_create_error(env, code, message, &err_val);
  assert(err == 0);
  return err_val;
}

static void
free_event(slsk_ev_t *ev) {
  if (ev == NULL) return;
  free(ev->err);
  free(ev->level);
  free(ev->event);
  free(ev->message);
  free(ev->phase);
  if (ev->files != NULL) slsk_file_list_free(ev->files);
  if (ev->dl_result != NULL) slsk_download_result_free(ev->dl_result);
  free(ev);
}

static void
enqueue_event(bare_slsk_client_t *c, slsk_ev_t *ev) {
  uv_mutex_lock(&c->lock);
  if (c->closing) {
    uv_mutex_unlock(&c->lock);
    if (ev->req != NULL && (ev->kind == SLSK_EV_STATE_DONE || ev->kind == SLSK_EV_FILES_DONE || ev->kind == SLSK_EV_DOWNLOAD_DONE)) {
      free(ev->req);
      ev->req = NULL;
    }
    free_event(ev);
    return;
  }
  ev->next = NULL;
  if (c->tail != NULL) {
    c->tail->next = ev;
  } else {
    c->head = ev;
  }
  c->tail = ev;
  uv_mutex_unlock(&c->lock);

  uv_async_send(&c->async);
}

static void
begin_pending_op(bare_slsk_client_t *c) {
  if (c->pending_ops == 0 && !c->closing) {
    uv_ref((uv_handle_t *) &c->async);
  }
  c->pending_ops++;
}

static void
end_pending_op(bare_slsk_client_t *c) {
  if (c->pending_ops > 0) {
    c->pending_ops--;
    if (c->pending_ops == 0 && !c->closing) {
      uv_unref((uv_handle_t *) &c->async);
    }
  }
}

/* ========================= Rust -> Queue Bridges ========================= */

static void
rust_log_bridge(void *ctx, const char *level, const char *event, const char *message) {
  bare_slsk_client_t *c = (bare_slsk_client_t *) ctx;
  if (c == NULL) return;

  slsk_ev_t *ev = (slsk_ev_t *) calloc(1, sizeof(slsk_ev_t));
  if (ev == NULL) return;
  ev->kind = SLSK_EV_LOG;
  ev->level = dup_cstr(level);
  ev->event = dup_cstr(event);
  ev->message = dup_cstr(message);
  enqueue_event(c, ev);
}

static void
rust_state_done_bridge(void *req_ctx, const char *err, slsk_state_t state) {
  slsk_req_t *req = (slsk_req_t *) req_ctx;
  if (req == NULL) return;

  slsk_ev_t *ev = (slsk_ev_t *) calloc(1, sizeof(slsk_ev_t));
  if (ev == NULL) {
    free(req);
    return;
  }
  ev->kind = SLSK_EV_STATE_DONE;
  ev->req = req;
  ev->err = dup_cstr(err);
  ev->state = state;
  enqueue_event(req->owner, ev);
}

static void
rust_files_done_bridge(void *req_ctx, const char *err, slsk_file_list_t *list) {
  slsk_req_t *req = (slsk_req_t *) req_ctx;
  if (req == NULL) {
    if (list != NULL) slsk_file_list_free(list);
    return;
  }

  slsk_ev_t *ev = (slsk_ev_t *) calloc(1, sizeof(slsk_ev_t));
  if (ev == NULL) {
    if (list != NULL) slsk_file_list_free(list);
    free(req);
    return;
  }
  ev->kind = SLSK_EV_FILES_DONE;
  ev->req = req;
  ev->err = dup_cstr(err);
  ev->files = list;
  enqueue_event(req->owner, ev);
}

static void
rust_progress_bridge(
  void *req_ctx,
  uint64_t bytes_downloaded,
  uint64_t total_bytes,
  double speed_bytes_per_sec,
  int64_t queue_place,
  const char *phase
) {
  slsk_req_t *req = (slsk_req_t *) req_ctx;
  if (req == NULL) return;

  slsk_ev_t *ev = (slsk_ev_t *) calloc(1, sizeof(slsk_ev_t));
  if (ev == NULL) return;
  ev->kind = SLSK_EV_PROGRESS;
  ev->req = req;
  ev->bytes_downloaded = bytes_downloaded;
  ev->total_bytes = total_bytes;
  ev->speed_bytes_per_sec = speed_bytes_per_sec;
  ev->queue_place = queue_place;
  ev->phase = dup_cstr(phase);
  enqueue_event(req->owner, ev);
}

static void
rust_download_done_bridge(void *req_ctx, const char *err, slsk_download_result_t *result) {
  slsk_req_t *req = (slsk_req_t *) req_ctx;
  if (req == NULL) {
    if (result != NULL) slsk_download_result_free(result);
    return;
  }

  slsk_ev_t *ev = (slsk_ev_t *) calloc(1, sizeof(slsk_ev_t));
  if (ev == NULL) {
    if (result != NULL) slsk_download_result_free(result);
    free(req);
    return;
  }
  ev->kind = SLSK_EV_DOWNLOAD_DONE;
  ev->req = req;
  ev->err = dup_cstr(err);
  ev->dl_result = result;
  enqueue_event(req->owner, ev);
}

/* ========================= libuv Event Loop Drain ========================= */

static void
drain_events_on_loop(uv_async_t *handle) {
  bare_slsk_client_t *c = (bare_slsk_client_t *) handle->data;
  if (c == NULL) return;

  uv_mutex_lock(&c->lock);
  slsk_ev_t *batch = c->head;
  c->head = NULL;
  c->tail = NULL;
  uv_mutex_unlock(&c->lock);

  js_env_t *env = c->env;

  while (batch != NULL) {
    slsk_ev_t *ev = batch;
    batch = ev->next;

    if (!c->exiting && !c->closing) {
      js_handle_scope_t *scope;
      int err = js_open_handle_scope(env, &scope);
      assert(err == 0);

      switch (ev->kind) {
        case SLSK_EV_LOG: {
          if (c->logger_ref != NULL) {
            js_value_t *fn;
            if (js_get_reference_value(env, c->logger_ref, &fn) == 0 && fn != NULL) {
              js_value_t *obj;
              js_create_object(env, &obj);
              set_str_prop(env, obj, "level", ev->level);
              set_str_prop(env, obj, "event", ev->event);
              set_str_prop(env, obj, "message", ev->message);
              js_value_t *global;
              js_get_global(env, &global);
              js_value_t *argv[1] = { obj };
              js_call_function(env, global, fn, 1, argv, NULL);
            }
          }
          break;
        }
        case SLSK_EV_STATE_DONE: {
          slsk_req_t *req = ev->req;
          if (req != NULL && req->deferred != NULL) {
            if (ev->err != NULL) {
              js_reject_deferred(env, req->deferred, build_error_value(env, ev->err));
            } else {
              js_resolve_deferred(env, req->deferred, build_state_object(env, ev->state));
            }
          }
          end_pending_op(c);
          free(req);
          ev->req = NULL;
          break;
        }
        case SLSK_EV_FILES_DONE: {
          slsk_req_t *req = ev->req;
          if (req != NULL && req->deferred != NULL) {
            if (ev->err != NULL) {
              js_reject_deferred(env, req->deferred, build_error_value(env, ev->err));
            } else {
              size_t len = ev->files ? ev->files->len : 0;
              js_value_t *arr;
              js_create_array_with_length(env, len, &arr);
              for (size_t i = 0; i < len; i++) {
                slsk_file_t *f = &ev->files->items[i];
                js_value_t *item;
                js_create_object(env, &item);
                set_str_prop(env, item, "username", f->username);
                set_bool_prop(env, item, "slots", f->slots != 0);
                set_u32_prop(env, item, "queue", f->queue);
                set_u32_prop(env, item, "speed", f->speed);
                set_str_prop(env, item, "filename", f->filename);
                set_i64_prop(env, item, "size", (int64_t) f->size);
                set_opt_i64_prop(env, item, "bitrate", f->bitrate);
                set_opt_i64_prop(env, item, "sample_rate", f->sample_rate);
                set_opt_i64_prop(env, item, "bit_depth", f->bit_depth);
                set_opt_i64_prop(env, item, "length_secs", f->length_secs);
                js_set_element(env, arr, (uint32_t) i, item);
              }
              js_resolve_deferred(env, req->deferred, arr);
            }
          }
          end_pending_op(c);
          free(req);
          ev->req = NULL;
          break;
        }
        case SLSK_EV_PROGRESS: {
          slsk_req_t *req = ev->req;
          if (req != NULL && req->on_progress_ref != NULL) {
            js_value_t *fn;
            if (js_get_reference_value(env, req->on_progress_ref, &fn) == 0 && fn != NULL) {
              js_value_t *obj;
              js_create_object(env, &obj);
              set_i64_prop(env, obj, "bytes", (int64_t) ev->bytes_downloaded);
              set_i64_prop(env, obj, "total", (int64_t) ev->total_bytes);
              set_double_prop(env, obj, "speed", ev->speed_bytes_per_sec);
              set_opt_i64_prop(env, obj, "queue", ev->queue_place);
              set_str_prop(env, obj, "phase", ev->phase);
              js_value_t *global;
              js_get_global(env, &global);
              js_value_t *argv[1] = { obj };
              js_call_function(env, global, fn, 1, argv, NULL);
            }
          }
          break;
        }
        case SLSK_EV_DOWNLOAD_DONE: {
          slsk_req_t *req = ev->req;
          if (req != NULL) {
            if (req->on_progress_ref != NULL) {
              js_delete_reference(env, req->on_progress_ref);
              req->on_progress_ref = NULL;
            }
            if (req->deferred != NULL) {
              if (ev->err != NULL) {
                js_reject_deferred(env, req->deferred, build_error_value(env, ev->err));
              } else {
                js_value_t *obj;
                js_create_object(env, &obj);
                set_str_prop(env, obj, "path", ev->dl_result ? ev->dl_result->path : "");
                set_i64_prop(env, obj, "size", ev->dl_result ? (int64_t) ev->dl_result->size : 0);
                set_i64_prop(env, obj, "resumed_from", ev->dl_result ? (int64_t) ev->dl_result->resumed_from : 0);
                js_resolve_deferred(env, req->deferred, obj);
              }
            }
            end_pending_op(c);
            free(req);
            ev->req = NULL;
          }
          break;
        }
      }

      err = js_close_handle_scope(env, scope);
      assert(err == 0);
    } else if (ev->req != NULL && (ev->kind == SLSK_EV_STATE_DONE || ev->kind == SLSK_EV_FILES_DONE || ev->kind == SLSK_EV_DOWNLOAD_DONE)) {
      if (!c->exiting && ev->req->on_progress_ref != NULL) {
        js_delete_reference(env, ev->req->on_progress_ref);
      }
      free(ev->req);
      ev->req = NULL;
    }

    free_event(ev);
  }
}

/* ========================= Teardown & Close ========================= */

static void
on_async_closed(uv_handle_t *handle) {
  bare_slsk_client_t *c = (bare_slsk_client_t *) handle->data;
  if (c == NULL) return;

  uv_mutex_lock(&c->lock);
  slsk_ev_t *batch = c->head;
  c->head = NULL;
  c->tail = NULL;
  uv_mutex_unlock(&c->lock);

  while (batch != NULL) {
    slsk_ev_t *ev = batch;
    batch = ev->next;
    if (ev->req != NULL && (ev->kind == SLSK_EV_STATE_DONE || ev->kind == SLSK_EV_FILES_DONE || ev->kind == SLSK_EV_DOWNLOAD_DONE)) {
      if (!c->exiting && ev->req->on_progress_ref != NULL) {
        js_delete_reference(c->env, ev->req->on_progress_ref);
      }
      free(ev->req);
    }
    free_event(ev);
  }

  if (!c->exiting && c->logger_ref != NULL) {
    js_delete_reference(c->env, c->logger_ref);
    c->logger_ref = NULL;
  }

  uv_mutex_destroy(&c->lock);
  js_deferred_teardown_t *teardown = c->teardown;
  c->teardown = NULL;
  free(c);

  if (teardown != NULL) {
    js_finish_deferred_teardown_callback(teardown);
  }
}

static void
start_close_client(bare_slsk_client_t *c) {
  if (c == NULL) return;

  uv_mutex_lock(&c->lock);
  if (c->closing) {
    uv_mutex_unlock(&c->lock);
    return;
  }
  c->closing = true;
  uv_mutex_unlock(&c->lock);

  if (c->rust_client != NULL) {
    slsk_client_set_logger(c->rust_client, NULL, NULL);
    slsk_client_free(c->rust_client);
    c->rust_client = NULL;
  }

  uv_ref((uv_handle_t *) &c->async);
  uv_close((uv_handle_t *) &c->async, on_async_closed);
}

static void
on_env_teardown(js_deferred_teardown_t *handle, void *data) {
  (void) handle;
  bare_slsk_client_t *c = (bare_slsk_client_t *) data;
  if (c == NULL) return;
  c->exiting = true;
  start_close_client(c);
}

static bare_slsk_client_t *
unwrap_client(js_env_t *env, js_value_t *val) {
  bare_slsk_client_t *c = NULL;
  if (js_get_value_external(env, val, (void **) &c) != 0 || c == NULL || c->closing || c->rust_client == NULL) {
    js_throw_error(env, "SLSK_CLOSED", "Soulseek client handle is invalid or closed");
    return NULL;
  }
  return c;
}

/* ========================= Exported JS Functions ========================= */

static js_value_t *
bare_slsk_create(js_env_t *env, js_callback_info_t *info) {
  size_t argc = 1;
  js_value_t *argv[1];
  int err = js_get_callback_info(env, info, &argc, argv, NULL, NULL);
  assert(err == 0);

  uint32_t listen_port = 0;
  if (argc >= 1) {
    bool is_num = false;
    js_is_uint32(env, argv[0], &is_num);
    if (is_num) {
      js_get_value_uint32(env, argv[0], &listen_port);
    }
  }

  uv_loop_t *loop = NULL;
  err = js_get_env_loop(env, &loop);
  assert(err == 0);

  bare_slsk_client_t *c = (bare_slsk_client_t *) calloc(1, sizeof(bare_slsk_client_t));
  if (c == NULL) {
    js_throw_error(env, "ENOMEM", "Out of memory");
    return NULL;
  }

  err = uv_mutex_init(&c->lock);
  assert(err == 0);

  c->env = env;
  c->async.data = c;
  err = uv_async_init(loop, &c->async, drain_events_on_loop);
  assert(err == 0);
  /* Unref while idle so an unused handle never wedges Bare exit */
  uv_unref((uv_handle_t *) &c->async);

  c->rust_client = slsk_client_new((uint16_t) listen_port);

  err = js_add_deferred_teardown_callback(env, on_env_teardown, c, &c->teardown);
  assert(err == 0);

  js_value_t *ext;
  err = js_create_external(env, c, NULL, NULL, &ext);
  assert(err == 0);
  return ext;
}

static js_value_t *
bare_slsk_close(js_env_t *env, js_callback_info_t *info) {
  size_t argc = 1;
  js_value_t *argv[1];
  int err = js_get_callback_info(env, info, &argc, argv, NULL, NULL);
  assert(err == 0);
  if (argc < 1) return NULL;

  bare_slsk_client_t *c = NULL;
  if (js_get_value_external(env, argv[0], (void **) &c) == 0 && c != NULL) {
    start_close_client(c);
  }
  return NULL;
}

static js_value_t *
bare_slsk_state(js_env_t *env, js_callback_info_t *info) {
  size_t argc = 1;
  js_value_t *argv[1];
  int err = js_get_callback_info(env, info, &argc, argv, NULL, NULL);
  assert(err == 0);
  if (argc < 1) {
    js_throw_error(env, "EINVAL", "Expected client handle");
    return NULL;
  }

  bare_slsk_client_t *c = NULL;
  if (js_get_value_external(env, argv[0], (void **) &c) != 0 || c == NULL) {
    js_throw_error(env, "EINVAL", "Invalid client handle");
    return NULL;
  }
  if (c->closing || c->rust_client == NULL) {
    slsk_state_t empty = { 0, 0, 0, 0 };
    return build_state_object(env, empty);
  }

  slsk_state_t st = slsk_client_state(c->rust_client);
  return build_state_object(env, st);
}

static js_value_t *
bare_slsk_set_logger(js_env_t *env, js_callback_info_t *info) {
  size_t argc = 2;
  js_value_t *argv[2];
  int err = js_get_callback_info(env, info, &argc, argv, NULL, NULL);
  assert(err == 0);
  if (argc < 2) {
    js_throw_error(env, "EINVAL", "setLogger(handle, fn|null) requires 2 arguments");
    return NULL;
  }

  bare_slsk_client_t *c = unwrap_client(env, argv[0]);
  if (c == NULL) return NULL;

  slsk_client_set_logger(c->rust_client, NULL, NULL);
  if (c->logger_ref != NULL) {
    js_delete_reference(env, c->logger_ref);
    c->logger_ref = NULL;
  }

  bool is_fn = false;
  js_is_function(env, argv[1], &is_fn);
  if (is_fn) {
    err = js_create_reference(env, argv[1], 1, &c->logger_ref);
    assert(err == 0);
    slsk_client_set_logger(c->rust_client, rust_log_bridge, c);
  }

  return NULL;
}

static js_value_t *
bare_slsk_login(js_env_t *env, js_callback_info_t *info) {
  size_t argc = 5;
  js_value_t *argv[5];
  int err = js_get_callback_info(env, info, &argc, argv, NULL, NULL);
  assert(err == 0);
  if (argc < 5) {
    js_throw_error(env, "EINVAL", "login(handle, server, port, user, pass) requires 5 arguments");
    return NULL;
  }

  bare_slsk_client_t *c = unwrap_client(env, argv[0]);
  if (c == NULL) return NULL;

  char *server = get_js_utf8(env, argv[1]);
  uint32_t port = 0;
  js_get_value_uint32(env, argv[2], &port);
  char *user = get_js_utf8(env, argv[3]);
  char *pass = get_js_utf8(env, argv[4]);

  if (server == NULL || user == NULL || pass == NULL) {
    free(server);
    free(user);
    free(pass);
    js_throw_error(env, "EINVAL", "server, user, and pass must be strings");
    return NULL;
  }

  js_deferred_t *deferred;
  js_value_t *promise;
  err = js_create_promise(env, &deferred, &promise);
  assert(err == 0);

  slsk_req_t *req = (slsk_req_t *) calloc(1, sizeof(slsk_req_t));
  req->owner = c;
  req->deferred = deferred;

  begin_pending_op(c);
  slsk_client_login_async(
    c->rust_client,
    server,
    (uint16_t) port,
    user,
    pass,
    rust_state_done_bridge,
    req
  );

  memset(pass, 0, strlen(pass));
  free(server);
  free(user);
  free(pass);

  return promise;
}

static js_value_t *
bare_slsk_search(js_env_t *env, js_callback_info_t *info) {
  size_t argc = 3;
  js_value_t *argv[3];
  int err = js_get_callback_info(env, info, &argc, argv, NULL, NULL);
  assert(err == 0);
  if (argc < 3) {
    js_throw_error(env, "EINVAL", "search(handle, query, timeoutMs) requires 3 arguments");
    return NULL;
  }

  bare_slsk_client_t *c = unwrap_client(env, argv[0]);
  if (c == NULL) return NULL;

  char *query = get_js_utf8(env, argv[1]);
  int64_t timeout_ms = 5000;
  js_get_value_int64(env, argv[2], &timeout_ms);
  if (timeout_ms < 0) timeout_ms = 0;

  if (query == NULL) {
    js_throw_error(env, "EINVAL", "query must be a string");
    return NULL;
  }

  js_deferred_t *deferred;
  js_value_t *promise;
  err = js_create_promise(env, &deferred, &promise);
  assert(err == 0);

  slsk_req_t *req = (slsk_req_t *) calloc(1, sizeof(slsk_req_t));
  req->owner = c;
  req->deferred = deferred;

  begin_pending_op(c);
  slsk_client_search_async(
    c->rust_client,
    query,
    (uint64_t) timeout_ms,
    rust_files_done_bridge,
    req
  );

  free(query);
  return promise;
}

static js_value_t *
bare_slsk_browse(js_env_t *env, js_callback_info_t *info) {
  size_t argc = 3;
  js_value_t *argv[3];
  int err = js_get_callback_info(env, info, &argc, argv, NULL, NULL);
  assert(err == 0);
  if (argc < 2) {
    js_throw_error(env, "EINVAL", "browse(handle, username[, timeoutMs]) requires at least 2 arguments");
    return NULL;
  }

  bare_slsk_client_t *c = unwrap_client(env, argv[0]);
  if (c == NULL) return NULL;

  char *username = get_js_utf8(env, argv[1]);
  int64_t timeout_ms = 30000;
  if (argc >= 3) {
    bool is_num = false;
    js_is_number(env, argv[2], &is_num);
    if (is_num) {
      js_get_value_int64(env, argv[2], &timeout_ms);
    }
  }
  if (timeout_ms < 0) timeout_ms = 0;

  if (username == NULL) {
    js_throw_error(env, "EINVAL", "username must be a string");
    return NULL;
  }

  js_deferred_t *deferred;
  js_value_t *promise;
  err = js_create_promise(env, &deferred, &promise);
  assert(err == 0);

  slsk_req_t *req = (slsk_req_t *) calloc(1, sizeof(slsk_req_t));
  req->owner = c;
  req->deferred = deferred;

  begin_pending_op(c);
  slsk_client_browse_async(
    c->rust_client,
    username,
    (uint64_t) timeout_ms,
    rust_files_done_bridge,
    req
  );

  free(username);
  return promise;
}

static js_value_t *
bare_slsk_download(js_env_t *env, js_callback_info_t *info) {
  size_t argc = 5;
  js_value_t *argv[5];
  int err = js_get_callback_info(env, info, &argc, argv, NULL, NULL);
  assert(err == 0);
  if (argc < 4) {
    js_throw_error(env, "EINVAL", "download(handle, user, filename, destPath[, onProgress]) requires 4+ arguments");
    return NULL;
  }

  bare_slsk_client_t *c = unwrap_client(env, argv[0]);
  if (c == NULL) return NULL;

  char *username = get_js_utf8(env, argv[1]);
  char *filename = get_js_utf8(env, argv[2]);
  char *dest_path = get_js_utf8(env, argv[3]);

  if (username == NULL || filename == NULL || dest_path == NULL) {
    free(username);
    free(filename);
    free(dest_path);
    js_throw_error(env, "EINVAL", "username, filename, and destPath must be strings");
    return NULL;
  }

  js_deferred_t *deferred;
  js_value_t *promise;
  err = js_create_promise(env, &deferred, &promise);
  assert(err == 0);

  slsk_req_t *req = (slsk_req_t *) calloc(1, sizeof(slsk_req_t));
  req->owner = c;
  req->deferred = deferred;

  slsk_progress_cb prog_bridge = NULL;
  if (argc >= 5) {
    bool is_fn = false;
    js_is_function(env, argv[4], &is_fn);
    if (is_fn) {
      err = js_create_reference(env, argv[4], 1, &req->on_progress_ref);
      assert(err == 0);
      prog_bridge = rust_progress_bridge;
    }
  }

  begin_pending_op(c);
  slsk_client_download_async(
    c->rust_client,
    username,
    filename,
    dest_path,
    prog_bridge,
    rust_download_done_bridge,
    req
  );

  free(username);
  free(filename);
  free(dest_path);
  return promise;
}

static js_value_t *
soulseek_exports(js_env_t *env, js_value_t *exports) {
  int err;

#define V(name, fn) \
  { \
    js_value_t *val; \
    err = js_create_function(env, name, (size_t) -1, fn, NULL, &val); \
    assert(err == 0); \
    err = js_set_named_property(env, exports, name, val); \
    assert(err == 0); \
  }

  V("create", bare_slsk_create)
  V("close", bare_slsk_close)
  V("state", bare_slsk_state)
  V("setLogger", bare_slsk_set_logger)
  V("login", bare_slsk_login)
  V("search", bare_slsk_search)
  V("browse", bare_slsk_browse)
  V("download", bare_slsk_download)
#undef V

  return exports;
}

BARE_MODULE(soulseek, soulseek_exports)
