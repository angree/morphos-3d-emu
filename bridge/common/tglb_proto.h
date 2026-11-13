/* SPDX-License-Identifier: MIT */
/* TGLBridge wire protocol (guest MorphOS/PPC big-endian  ->  host Windows/x64 little-endian).
 *
 * Every value on the wire is BIG-ENDIAN (the guest writes native, the host swaps).
 * Command:  u32 opcode, u32 payload_bytes, payload (padded to a multiple of 4).
 *   opcode < 0x1000  = tinygl.library slot number (LVO/6, see gen_slotnums.h); the payload
 *                      layout is generated (auto/fixv/pnamev) or documented in tglb_manual.c.
 *   opcode >= 0xF000 = protocol message (below).
 * Reply (only to synchronous commands): u32 payload_bytes, payload.
 *
 * Generated payload layouts:
 *   auto   : each argument in order, 4 bytes (8 for double / 64-bit ints)
 *   fixv   : scalars..., u32 n, n elements (element size of the pointer type), padded
 *   pnamev : same as fixv; n comes from the pname
 */
#ifndef TGLB_PROTO_H
#define TGLB_PROTO_H

#define TGLB_MAGIC        0x54474C42u   /* 'TGLB' */
#define TGLB_PROTO_VER    1
#define TGLB_DEFAULT_PORT 7654

#define OP_HELLO      0xF001  /* u32 magic, u32 version, char name[32]          -> reply u32 ok */
#define OP_UNIMPL     0xF002  /* u32 slot  (guest called an unimplemented entry; host logs once) */
#define OP_CREATE     0xF003  /* u32 kind(1 win,2 screen), u32 w, u32 h, u32 flags -> reply u32 ok */
#define OP_RESIZE     0xF004  /* u32 w, u32 h */
#define OP_DESTROY    0xF005  /* (none) */
#define OP_SWAP       0xF006  /* u32 mirror -> reply u32 swapno, u32 w, u32 h, [w*h*4 ARGB if mirror] */
#define OP_SETSYNC    0xF007  /* u32 enable */
#define OP_ARRAY      0xF008  /* u32 id, u32 size, u32 type, u32 first, u32 count, data (tight) */
#define OP_BYE        0xF009  /* (none) */
#define OP_NOTE       0xF00A  /* u32 len, text  - guest diagnostic line for the host log */
#define OP_STRICT     0xF00B  /* u32 flags, u32 max_texture_size, u32 texture_budget_kb (T73 strict mode) */

/* T73 strict / "real TinyGL" mode: ENV:TGLBRIDGE_STRICT bitmask (guest), sent to the host with OP_STRICT */
#define STRICT_LAZY_ARRAYS  0x01  /* client vertex arrays are read at the next flush point, not at the draw call */
#define STRICT_LAZY_INDICES 0x02  /* the index array of glDrawElements is read late too (implies the range scan) */
#define STRICT_STRIDE_TCCOL 0x04  /* texcoord/colour/normal arrays with a non-tight stride are read as if tight */
#define STRICT_STRIDE_XYZ   0x08  /* the vertex (position) array with a non-tight stride is read as if tight */
#define STRICT_LIMITS       0x10  /* host: max texture size, texture memory budget (uploads fail), index stats */
#define STRICT_CANARY       0x20  /* checksum every array + indices at the draw and again at the flush; log changes */
#define STRICT_LAG_SWAP     0x40  /* swap does not wait: late reads may happen up to one frame after the draw */

#define ARR_VERTEX 0
#define ARR_COLOR  1
#define ARR_NORMAL 2
#define ARR_TEX0   3          /* ARR_TEX0 + unit, units 0..7 */
#define ARR_COUNT  11

#endif
