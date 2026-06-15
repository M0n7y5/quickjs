// ---------------------------------------------------------------------------
// qjs_shim.c — Thin C wrapper for QuickJS static-inline functions
// and the bridge callback.
//
// QuickJS defines many critical functions as `static inline` in quickjs.h,
// which means they don't appear as exported symbols in the compiled shared
// library. This shim re-exports them as real functions so Dart FFI can call
// them via @ffi.Native.
//
// It also implements the bridge_call() JS function that creates a Promise
// and queues the request payload for Dart to process asynchronously.
// ---------------------------------------------------------------------------

#include "quickjs.h"
#include <string.h>
#include <stdlib.h>

// ===========================================================================
// Re-exports of static-inline QuickJS functions
// ===========================================================================

JSValue qjs_new_bool(JSContext *ctx, int val) {
    return JS_NewBool(ctx, val);
}

JSValue qjs_new_int32(JSContext *ctx, int32_t val) {
    return JS_NewInt32(ctx, val);
}

JSValue qjs_new_int64(JSContext *ctx, int64_t val) {
    return JS_NewInt64(ctx, val);
}

JSValue qjs_new_uint32(JSContext *ctx, uint32_t val) {
    return JS_NewUint32(ctx, val);
}

JSValue qjs_new_float64(JSContext *ctx, double val) {
    return JS_NewFloat64(ctx, val);
}

void qjs_free_value(JSContext *ctx, JSValue val) {
    JS_FreeValue(ctx, val);
}

void qjs_free_value_rt(JSRuntime *rt, JSValue val) {
    JS_FreeValueRT(rt, val);
}

JSValue qjs_dup_value(JSContext *ctx, JSValue val) {
    return JS_DupValue(ctx, val);
}

JSValue qjs_null(void) {
    return JS_NULL;
}

JSValue qjs_undefined(void) {
    return JS_UNDEFINED;
}

JSValue qjs_exception(void) {
    return JS_EXCEPTION;
}

int qjs_value_get_tag(JSValue val) {
    return JS_VALUE_GET_TAG(val);
}

int qjs_value_get_norm_tag(JSValue val) {
    return JS_VALUE_GET_NORM_TAG(val);
}

int qjs_is_number(JSValue val) {
    return JS_IsNumber(val);
}

int qjs_is_big_int(JSContext *ctx, JSValue val) {
    return JS_IsBigInt(ctx, val);
}

int qjs_is_bool(JSValue val) {
    return JS_IsBool(val);
}

int qjs_is_null(JSValue val) {
    return JS_IsNull(val);
}

int qjs_is_undefined(JSValue val) {
    return JS_IsUndefined(val);
}

int qjs_is_exception(JSValue val) {
    return JS_IsException(val);
}

int qjs_is_string(JSValue val) {
    return JS_IsString(val);
}

int qjs_is_object(JSValue val) {
    return JS_IsObject(val);
}

int qjs_is_symbol(JSValue val) {
    return JS_IsSymbol(val);
}

JSValue qjs_new_c_function(JSContext *ctx, JSCFunction *func,
                           const char *name, int length) {
    return JS_NewCFunction(ctx, func, name, length);
}

const char *qjs_to_c_string(JSContext *ctx, JSValue val) {
    return JS_ToCString(ctx, val);
}

const char *qjs_to_c_string_len(JSContext *ctx, size_t *plen, JSValue val) {
    return JS_ToCStringLen(ctx, plen, val);
}

JSValue qjs_new_string(JSContext *ctx, const char *str) {
    return JS_NewString(ctx, str);
}

// ===========================================================================
// Bridge callback infrastructure
//
// The bridge allows JS to call async Dart functions. When JS calls
// fjs.bridge_call(payload):
//   1. This C function JSON-serializes the payload
//   2. Creates a JS Promise
//   3. Stores {json, resolve, reject} in a ring buffer
//   4. Returns the Promise to JS
//
// Dart later reads the queue, processes the request, and calls
// qjs_bridge_resolve/reject to settle the promise.
// ===========================================================================

#define QJS_BRIDGE_QUEUE_SIZE 64

typedef struct {
    char *payload_json;      // Heap-allocated JSON string (owned)
    size_t payload_len;      // Length of JSON string
    JSValue resolve;         // Promise resolve function (ref-counted)
    JSValue reject;          // Promise reject function (ref-counted)
} QjsBridgeRequest;

typedef struct {
    QjsBridgeRequest requests[QJS_BRIDGE_QUEUE_SIZE];
    int head;                // Next slot to read from
    int tail;                // Next slot to write to
    int count;               // Number of pending requests
    JSContext *ctx;           // Context for this bridge
} QjsBridgeQueue;

// Per-runtime bridge queue, stored via JS_SetRuntimeOpaque.
// We allocate one per runtime.

QjsBridgeQueue *qjs_bridge_queue_new(JSContext *ctx) {
    QjsBridgeQueue *q = (QjsBridgeQueue *)malloc(sizeof(QjsBridgeQueue));
    if (!q) return NULL;
    memset(q, 0, sizeof(QjsBridgeQueue));
    q->ctx = ctx;
    return q;
}

void qjs_bridge_queue_free(QjsBridgeQueue *q) {
    if (!q) return;
    // Free any remaining requests
    while (q->count > 0) {
        QjsBridgeRequest *req = &q->requests[q->head];
        free(req->payload_json);
        JS_FreeValue(q->ctx, req->resolve);
        JS_FreeValue(q->ctx, req->reject);
        q->head = (q->head + 1) % QJS_BRIDGE_QUEUE_SIZE;
        q->count--;
    }
    free(q);
}

// Called from JS: fjs.bridge_call(payload)
// Returns a Promise. Queues the request for Dart to process.
static JSValue qjs_bridge_call_impl(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv) {
    if (argc < 1) {
        return JS_ThrowTypeError(ctx, "bridge_call requires 1 argument");
    }

    JSRuntime *rt = JS_GetRuntime(ctx);
    QjsBridgeQueue *queue = (QjsBridgeQueue *)JS_GetRuntimeOpaque(rt);
    if (!queue) {
        return JS_ThrowInternalError(ctx, "Bridge not initialized");
    }

    if (queue->count >= QJS_BRIDGE_QUEUE_SIZE) {
        return JS_ThrowInternalError(ctx, "Bridge queue full");
    }

    // JSON-serialize the payload
    JSValue json_str = JS_JSONStringify(ctx, argv[0], JS_UNDEFINED, JS_UNDEFINED);
    if (JS_IsException(json_str)) {
        return JS_EXCEPTION;
    }

    size_t len;
    const char *cstr = JS_ToCStringLen(ctx, &len, json_str);
    JS_FreeValue(ctx, json_str);
    if (!cstr) {
        return JS_ThrowInternalError(ctx, "Failed to serialize bridge payload");
    }

    // Create promise
    JSValue resolving_funcs[2];
    JSValue promise = JS_NewPromiseCapability(ctx, resolving_funcs);
    if (JS_IsException(promise)) {
        JS_FreeCString(ctx, cstr);
        return JS_EXCEPTION;
    }

    // Copy the JSON string (JS_FreeCString will free cstr)
    char *payload_copy = (char *)malloc(len + 1);
    if (!payload_copy) {
        JS_FreeCString(ctx, cstr);
        JS_FreeValue(ctx, promise);
        JS_FreeValue(ctx, resolving_funcs[0]);
        JS_FreeValue(ctx, resolving_funcs[1]);
        return JS_ThrowOutOfMemory(ctx);
    }
    memcpy(payload_copy, cstr, len + 1);
    JS_FreeCString(ctx, cstr);

    // Enqueue
    QjsBridgeRequest *req = &queue->requests[queue->tail];
    req->payload_json = payload_copy;
    req->payload_len = len;
    req->resolve = resolving_funcs[0];  // Takes ownership of ref
    req->reject = resolving_funcs[1];   // Takes ownership of ref
    queue->tail = (queue->tail + 1) % QJS_BRIDGE_QUEUE_SIZE;
    queue->count++;

    return promise;
}

// ---------------------------------------------------------------------------
// Bridge queue accessors for Dart FFI
// ---------------------------------------------------------------------------

// Returns the number of pending bridge requests.
int qjs_bridge_pending_count(QjsBridgeQueue *q) {
    return q ? q->count : 0;
}

// Peeks at the next request's JSON payload. Returns NULL if empty.
// The returned string is owned by the queue — do NOT free it.
const char *qjs_bridge_peek_payload(QjsBridgeQueue *q, size_t *out_len) {
    if (!q || q->count == 0) {
        if (out_len) *out_len = 0;
        return NULL;
    }
    QjsBridgeRequest *req = &q->requests[q->head];
    if (out_len) *out_len = req->payload_len;
    return req->payload_json;
}

// Peeks at the i-th pending request's JSON payload (0-indexed from head).
// Returns NULL if index is out of range.
// The returned string is owned by the queue — do NOT free it.
const char *qjs_bridge_peek_payload_at(QjsBridgeQueue *q, int index,
                                        size_t *out_len) {
    if (!q || index < 0 || index >= q->count) {
        if (out_len) *out_len = 0;
        return NULL;
    }
    int slot = (q->head + index) % QJS_BRIDGE_QUEUE_SIZE;
    QjsBridgeRequest *req = &q->requests[slot];
    if (out_len) *out_len = req->payload_len;
    return req->payload_json;
}

// Resolves the next pending bridge request with a JSON string result.
// Frees the request and advances the queue head.
// Returns 0 on success, -1 on error.
int qjs_bridge_resolve(QjsBridgeQueue *q, const char *result_json) {
    if (!q || q->count == 0) return -1;

    QjsBridgeRequest *req = &q->requests[q->head];
    JSContext *ctx = q->ctx;

    // Clear any pending exception before parsing (a prior JS operation
    // may have left one that causes JS_ParseJSON to fail spuriously).
    if (JS_HasException(ctx)) {
        JSValue pending = JS_GetException(ctx);
        JS_FreeValue(ctx, pending);
    }

    // Parse the result JSON into a JS value
    JSValue result;
    if (result_json && result_json[0] != '\0') {
        result = JS_ParseJSON(ctx, result_json, strlen(result_json), "<bridge>");
        if (JS_IsException(result)) {
            // Clear the parse exception and fall back to raw string
            JSValue exc = JS_GetException(ctx);
            JS_FreeValue(ctx, exc);
            result = JS_NewString(ctx, result_json);
        }
    } else {
        result = JS_UNDEFINED;
    }

    // Call resolve(result)
    JSValue ret = JS_Call(ctx, req->resolve, JS_UNDEFINED, 1, &result);
    JS_FreeValue(ctx, ret);
    JS_FreeValue(ctx, result);

    // Cleanup
    free(req->payload_json);
    req->payload_json = NULL;
    JS_FreeValue(ctx, req->resolve);
    JS_FreeValue(ctx, req->reject);

    q->head = (q->head + 1) % QJS_BRIDGE_QUEUE_SIZE;
    q->count--;
    return 0;
}

// Rejects the next pending bridge request with an error message.
int qjs_bridge_reject(QjsBridgeQueue *q, const char *error_message) {
    if (!q || q->count == 0) return -1;

    QjsBridgeRequest *req = &q->requests[q->head];
    JSContext *ctx = q->ctx;

    // Wrap the message in a proper Error object so that JS catch(e) gets
    // an Error with e.message, not a plain string.
    JSValue err_str = JS_NewString(ctx, error_message ? error_message : "Unknown error");
    JSValue error_obj = JS_NewError(ctx);
    JS_SetPropertyStr(ctx, error_obj, "message", err_str);
    JSValue ret = JS_Call(ctx, req->reject, JS_UNDEFINED, 1, &error_obj);
    JS_FreeValue(ctx, ret);
    JS_FreeValue(ctx, error_obj);

    // Cleanup
    free(req->payload_json);
    req->payload_json = NULL;
    JS_FreeValue(ctx, req->resolve);
    JS_FreeValue(ctx, req->reject);

    q->head = (q->head + 1) % QJS_BRIDGE_QUEUE_SIZE;
    q->count--;
    return 0;
}

// ---------------------------------------------------------------------------
// Bridge setup: register fjs.bridge_call on globalThis
// ---------------------------------------------------------------------------

// Initializes the bridge for a runtime/context pair.
// Must be called after JS_NewContext but before any eval.
// Returns the queue pointer (Dart stores this for later access).
QjsBridgeQueue *qjs_bridge_init(JSRuntime *rt, JSContext *ctx) {
    QjsBridgeQueue *queue = qjs_bridge_queue_new(ctx);
    if (!queue) return NULL;

    // Store queue as runtime opaque
    JS_SetRuntimeOpaque(rt, queue);

    // Create globalThis.fjs = { bridge_call: <native fn> }
    JSValue fjs_obj = JS_NewObject(ctx);
    JSValue bridge_fn = JS_NewCFunction(ctx, qjs_bridge_call_impl, "bridge_call", 1);
    JS_SetPropertyStr(ctx, fjs_obj, "bridge_call", bridge_fn);

    JSValue global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global, "fjs", fjs_obj);
    JS_FreeValue(ctx, global);

    return queue;
}

// Cleans up the bridge queue. Call before JS_FreeContext/JS_FreeRuntime.
void qjs_bridge_cleanup(QjsBridgeQueue *q) {
    qjs_bridge_queue_free(q);
}

// ===========================================================================
// Execution timeout via JS_SetInterruptHandler
//
// QuickJS calls the interrupt handler approximately every 10,000
// branch/call/jump operations. We store a monotonic-clock deadline
// and return non-zero when it's exceeded, causing QuickJS to throw
// an uncatchable InternalError.
// ===========================================================================

#include <time.h>

typedef struct {
    int64_t deadline_ns;  // CLOCK_MONOTONIC nanoseconds; 0 = disabled
} QjsTimeoutState;

QjsTimeoutState *qjs_timeout_new(void) {
    QjsTimeoutState *s = (QjsTimeoutState *)malloc(sizeof(QjsTimeoutState));
    if (!s) return NULL;
    s->deadline_ns = 0;
    return s;
}

void qjs_timeout_free(QjsTimeoutState *s) {
    free(s);
}

void qjs_timeout_set(QjsTimeoutState *s, int64_t timeout_ms) {
    if (!s || timeout_ms <= 0) {
        if (s) s->deadline_ns = 0;
        return;
    }
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    s->deadline_ns = (int64_t)now.tv_sec * 1000000000LL
                   + (int64_t)now.tv_nsec
                   + timeout_ms * 1000000LL;
}

void qjs_timeout_clear(QjsTimeoutState *s) {
    if (s) s->deadline_ns = 0;
}

static int qjs_interrupt_handler(JSRuntime *rt, void *opaque) {
    QjsTimeoutState *s = (QjsTimeoutState *)opaque;
    if (!s || s->deadline_ns == 0) return 0;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    int64_t now_ns = (int64_t)now.tv_sec * 1000000000LL + (int64_t)now.tv_nsec;
    return now_ns >= s->deadline_ns;
}

void qjs_timeout_install(JSRuntime *rt, QjsTimeoutState *s) {
    JS_SetInterruptHandler(rt, qjs_interrupt_handler, s);
}
