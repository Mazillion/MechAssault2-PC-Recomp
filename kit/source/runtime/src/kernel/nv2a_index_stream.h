/* Bounded storage for arbitrarily long NV097 indexed primitives. A FIFO
 * packet boundary is not a primitive boundary: connected topologies carry
 * their anchor/edge into the next draw, with triangle-strip winding intact. */
#pragma once
#include <stdint.h>
#include <string.h>

#define NV2A_INDEX_BATCH_SIZE 4096
typedef struct Nv2aIndexStream {
    uint32_t values[NV2A_INDEX_BATCH_SIZE];
    uint32_t count, primitive, batches;
    uint64_t total;
    uint32_t first;
} Nv2aIndexStream;

/* The callback consumes values synchronously and must not append to stream. */
typedef void (*Nv2aIndexDraw)(void *context, uint32_t primitive,
                              const uint32_t *values, uint32_t count);

static void nv2a_index_stream_begin(Nv2aIndexStream *stream, uint32_t primitive)
{
    stream->count = stream->batches = 0;
    stream->total = 0;
    stream->primitive = primitive;
}

static void nv2a_index_stream_flush(Nv2aIndexStream *stream, int final,
                                   Nv2aIndexDraw draw, void *context)
{
    uint32_t n = stream->count, used = n, keep = 0;
    uint32_t primitive = stream->primitive;
    uint32_t carry[3];
    if (!n) return;
    switch (primitive) {
    case 1: break;                                      /* points */
    case 2: used -= used % 2; break;                    /* lines */
    case 3:                                            /* line loop */
    case 4:                                            /* line strip */
        primitive = 4;
        if (!final) carry[keep++] = stream->values[n - 1];
        break;
    case 5: used -= used % 3; break;                    /* triangles */
    case 6:                                            /* triangle strip */
    case 9:                                            /* quad strip */
        /* Removing an even number of vertices preserves strip parity. */
        if (!final) {
            used &= ~1u;
            carry[keep++] = stream->values[used - 2];
            carry[keep++] = stream->values[used - 1];
        }
        break;
    case 7:                                            /* triangle fan */
    case 10:                                           /* polygon */
        if (!final) {
            carry[keep++] = stream->values[0];
            carry[keep++] = stream->values[n - 1];
        }
        break;
    case 8: used -= used % 4; break;                    /* quads */
    default: stream->count = 0; return;
    }
    if (!final) {
        for (uint32_t i = used; i < n; i++) carry[keep++] = stream->values[i];
    }
    if (used) {
        draw(context, primitive, stream->values, used);
        stream->batches++;
    }
    if (final && stream->primitive == 3 && stream->total > 1) {
        /* Close the original loop once, never the individual chunks. */
        stream->values[0] = stream->values[n - 1];
        stream->values[1] = stream->first;
        draw(context, 2, stream->values, 2);
        stream->batches++;
    }
    if (keep) memcpy(stream->values, carry, keep * sizeof(*carry));
    stream->count = keep;
}

static void nv2a_index_stream_append(Nv2aIndexStream *stream, uint32_t value,
                                    Nv2aIndexDraw draw, void *context)
{
    if (stream->count == NV2A_INDEX_BATCH_SIZE)
        nv2a_index_stream_flush(stream, 0, draw, context);
    if (!stream->total) stream->first = value;
    stream->total++;
    stream->values[stream->count++] = value;
}

static void nv2a_index_stream_end(Nv2aIndexStream *stream,
                                 Nv2aIndexDraw draw, void *context)
{
    nv2a_index_stream_flush(stream, 1, draw, context);
    stream->primitive = 0;
}

/* ARRAY_ELEMENT32 also supplies the single final index of odd-length 16-bit
 * meshes. It is part of the same primitive, not a new draw or packed pair. */
static void nv2a_index_stream_method(Nv2aIndexStream *stream, uint32_t method,
                                    uint32_t parameter, Nv2aIndexDraw draw, void *context)
{
    if (!stream->primitive) return;
    switch (method) {
    case 0x1800:
        nv2a_index_stream_append(stream, parameter & 0xFFFFu, draw, context);
        nv2a_index_stream_append(stream, parameter >> 16, draw, context);
        break;
    case 0x1808:
        nv2a_index_stream_append(stream, parameter, draw, context);
        break;
    case 0x1810: {
        uint32_t start = parameter & 0xFFFFFFu, count = (parameter >> 24) + 1;
        for (uint32_t i = 0; i < count; i++)
            nv2a_index_stream_append(stream, start + i, draw, context);
        break;
    }
    }
}
