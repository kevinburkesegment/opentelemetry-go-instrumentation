// Copyright The OpenTelemetry Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "arguments.h"
#include "go_types.h"
#include "span_context.h"
#include "go_context.h"
#include "uprobe.h"

char __license[] SEC("license") = "Dual MIT/GPL";

#define MAX_SIZE 50
#define MAX_CONCURRENT 50

struct grpc_request_t
{
    BASE_SPAN_PROPERTIES
    char method[MAX_SIZE];
    char target[MAX_SIZE];
};

struct hpack_header_field
{
    struct go_string name;
    struct go_string value;
    bool sensitive;
};

struct
{
    __uint(type, BPF_MAP_TYPE_HASH);
    __type(key, void *);
    __type(value, struct grpc_request_t);
    __uint(max_entries, MAX_CONCURRENT);
} grpc_events SEC(".maps");

struct
{
    __uint(type, BPF_MAP_TYPE_HASH);
    __type(key, u32);
    __type(value, struct span_context);
    __uint(max_entries, MAX_CONCURRENT);
} streamid_to_span_contexts SEC(".maps");

struct
{
    __uint(type, BPF_MAP_TYPE_PERF_EVENT_ARRAY);
} events SEC(".maps");

// Injected in init
volatile const u64 clientconn_target_ptr_pos;
volatile const u64 httpclient_nextid_pos;
volatile const u64 headerFrame_streamid_pos;
volatile const u64 headerFrame_hf_pos;

// This instrumentation attaches uprobe to the following function:
// func (cc *ClientConn) Invoke(ctx context.Context, method string, args, reply interface{}, opts ...CallOption) error
SEC("uprobe/ClientConn_Invoke")
int uprobe_ClientConn_Invoke(struct pt_regs *ctx)
{
    // positions
    u64 clientconn_pos = 1;
    u64 method_ptr_pos = 4;
    u64 method_len_pos = 5;

    void *context_ptr = get_Go_context(ctx, 3, 0, true);
    if (context_ptr == NULL)
    {
        return 0;
    }
    // Get key
    void *key = get_consistent_key(ctx, context_ptr);
    void *grpcReq_ptr = bpf_map_lookup_elem(&grpc_events, &key);
    if (grpcReq_ptr != NULL)
    {
        bpf_printk("uprobe/ClientConn_Invoke already tracked with the current context");
        return 0;
    }

    struct grpc_request_t grpcReq = {};
    grpcReq.start_time = bpf_ktime_get_ns();

    // Read Method
    void *method_ptr = get_argument(ctx, method_ptr_pos);
    u64 method_len = (u64)get_argument(ctx, method_len_pos);
    u64 method_size = sizeof(grpcReq.method);
    method_size = method_size < method_len ? method_size : method_len;
    bpf_probe_read(&grpcReq.method, method_size, method_ptr);

    // Read ClientConn.Target
    void *clientconn_ptr = get_argument(ctx, clientconn_pos);
    if (!get_go_string_from_user_ptr((void*)(clientconn_ptr + clientconn_target_ptr_pos), grpcReq.target, sizeof(grpcReq.target)))
    {
        bpf_printk("target write failed, aborting ebpf probe");
        return 0;
    }

    // Get parent if exists 
    struct span_context *parent_span_ctx = get_parent_span_context(context_ptr);
    if (parent_span_ctx != NULL)
    {
        bpf_probe_read(&grpcReq.psc, sizeof(grpcReq.psc), parent_span_ctx);
        copy_byte_arrays(grpcReq.psc.TraceID, grpcReq.sc.TraceID, TRACE_ID_SIZE);
        generate_random_bytes(grpcReq.sc.SpanID, SPAN_ID_SIZE);
    }
    else
    {
        grpcReq.sc = generate_span_context();
    }

    // Write event
    bpf_map_update_elem(&grpc_events, &key, &grpcReq, 0);
    start_tracking_span(context_ptr, &grpcReq.sc);
    return 0;
}

UPROBE_RETURN(ClientConn_Invoke, struct grpc_request_t, grpc_events, events, 3, 0, true)

// func (l *loopyWriter) headerHandler(h *headerFrame) error
SEC("uprobe/loopyWriter_headerHandler")
int uprobe_LoopyWriter_HeaderHandler(struct pt_regs *ctx)
{
    void *headerFrame_ptr = get_argument(ctx, 2);
    u32 stream_id = 0;
    bpf_probe_read(&stream_id, sizeof(stream_id), (void *)(headerFrame_ptr + (headerFrame_streamid_pos)));
    void *sc_ptr = bpf_map_lookup_elem(&streamid_to_span_contexts, &stream_id);
    if (sc_ptr == NULL)
    {
        return 0;
    }

    bpf_map_delete_elem(&streamid_to_span_contexts, &stream_id);
    struct go_slice slice = {};
    struct go_slice_user_ptr slice_user_ptr = {};
    slice_user_ptr.array = (void *)(headerFrame_ptr + (headerFrame_hf_pos));
    slice_user_ptr.len = (void *)(headerFrame_ptr + (headerFrame_hf_pos + 8));
    slice_user_ptr.cap = (void *)(headerFrame_ptr + (headerFrame_hf_pos + 16));
    bpf_probe_read(&slice.array, sizeof(slice.array), slice_user_ptr.array);
    bpf_probe_read(&slice.len, sizeof(slice.len), slice_user_ptr.len);
    bpf_probe_read(&slice.cap, sizeof(slice.cap), slice_user_ptr.cap);

    struct span_context current_span_context = {};
    bpf_probe_read(&current_span_context, sizeof(current_span_context), sc_ptr);

    char tp_key[11] = "traceparent";
    struct go_string key_str = write_user_go_string(tp_key, sizeof(tp_key));
    if (key_str.len == 0) {
        bpf_printk("key write failed, aborting ebpf probe");
        return 0;
    }

    // Write headers
    char val[SPAN_CONTEXT_STRING_SIZE];
    span_context_to_w3c_string(&current_span_context, val);
    struct go_string val_str = write_user_go_string(val, sizeof(val));
    if (val_str.len == 0) {
        bpf_printk("val write failed, aborting ebpf probe");
        return 0;
    }
    struct hpack_header_field hf = {};
    hf.name = key_str;
    hf.value = val_str;
    append_item_to_slice(&slice, &hf, sizeof(hf), &slice_user_ptr);
    return 0;
}

SEC("uprobe/http2Client_NewStream")
// func (t *http2Client) NewStream(ctx context.Context, callHdr *CallHdr) (*Stream, error)
int uprobe_http2Client_NewStream(struct pt_regs *ctx)
{
    void *context_ptr = get_argument(ctx, 3);
    void *httpclient_ptr = get_argument(ctx, 1);
    u32 nextid = 0;
    bpf_probe_read(&nextid, sizeof(nextid), (void *)(httpclient_ptr + (httpclient_nextid_pos)));
    struct span_context *current_span_context = get_parent_span_context(context_ptr);
    if (current_span_context != NULL) {
        bpf_map_update_elem(&streamid_to_span_contexts, &nextid, current_span_context, 0);
    }

    return 0;
}