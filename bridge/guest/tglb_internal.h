/* SPDX-License-Identifier: MIT */
/* TGLBridge guest internals: the context, the command buffer and the wire helpers. */
#ifndef TGLB_INTERNAL_H
#define TGLB_INTERNAL_H

#include "tglb_types.h"
#include "../common/tglb_proto.h"
#include "gen_slotnums.h"
#include "gen_manual.h"

#define TGLB_BUFSZ (256 * 1024)
#define TGLB_MAXUNITS 8
#define TGLB_NSTR 8

struct tglb_array {
	int enabled;
	int size;               /* components */
	unsigned int type;
	int stride;             /* as given (0 = tight) */
	const unsigned char *ptr;
	int sent;               /* uploaded for the current lock */
};

/* T73: one draw call that is deferred (lazy reads) and/or watched (canary) until the next flush point */
struct tglb_pend {
	unsigned int off;                 /* offset in c->buf where the draw belongs in the stream */
	int kind;                         /* 0 glDrawElements, 1 glDrawArrays, 2 glArrayElement */
	int emitted;                      /* already serialised at the call (canary-only record) */
	int hi_given;                     /* lo/hi are final (glDrawRangeElements, or indices not copied) */
	unsigned int mode, type, lo, hi;
	int count, first;
	const void *idx;                  /* client index pointer (lazy) or idx_copy */
	void *idx_copy;
	struct tglb_array arr[ARR_COUNT];
	int locked, lock_first, lock_count;
	unsigned int sum[ARR_COUNT + 1];  /* canary: arrays + [ARR_COUNT] = indices */
	unsigned int draw_no;             /* draw number within the frame */
};

struct _GLContext {
	unsigned int magic;               /* 'TGLB' */
	struct Library *SocketBase;       /* opened by the task that called GLInit */
	struct Task *owner;
	long sock;
	int dead;                         /* connection lost: everything becomes a no-op */
	unsigned char *buf;
	unsigned int used;
	unsigned char scratch[64];        /* reply sink for small syncs */

	/* drawable */
	int kind;                         /* 0 none, 1 window, 2 screen */
	struct Window *win;
	struct Screen *scr;
	int w, h;
	int mirror_every;                 /* ENV:TGLBRIDGE_MIRROR (0 = off) */
	unsigned char *mirror_buf;
	unsigned int mirror_size;
	unsigned int swaps;
	unsigned int ctxversion;

	/* client state shadowed in the guest */
	struct tglb_array arr[ARR_COUNT];
	int client_unit;                  /* glClientActiveTexture - GL_TEXTURE0 */
	int locked, lock_first, lock_count;
	int unpack_align, unpack_rowlen, unpack_skiprows, unpack_skippix;
	int pack_align, pack_rowlen, pack_skiprows, pack_skippix;

	/* glGetString cache */
	unsigned int str_name[TGLB_NSTR];
	char *str_val[TGLB_NSTR];

	unsigned char unimpl_sent[(TGLB_NSLOTS + 7) / 8];

	/* T73 strict mode (ENV:TGLBRIDGE_STRICT) */
	unsigned int strict;
	struct tglb_pend *pend;
	int npend, pend_cap;
	int in_resolve;
	unsigned char *buf2;              /* serialisation buffer while resolving deferred draws */
	int owed;                         /* swap replies sent to the host and not read yet (STRICT_LAG_SWAP) */
	int swaps_in_buf;                 /* OP_SWAPs still in c->buf (not sent yet) */
	int lag_n;
	int lazy_min;                     /* ENV:TGLBRIDGE_LAZYMIN: draws using fewer vertices are read at the call (0 = all lazy) */                        /* ENV:TGLBRIDGE_LAG frames the "driver" may run behind (default 3) */
	unsigned int last_op;             /* last command in c->buf and its offset (-1 = gone/flushed) */
	int last_off;
	int split;                        /* the host got part of a command (buffer-full flush): no out-of-band queries */
	unsigned long st_oob;
	unsigned int draw_no;             /* draws in this frame */
	unsigned long st_deferred, st_resolves, st_canary_bad, st_canary_draws, st_canary_frames;
	unsigned long st_fr_bad, st_fr_deferred, st_fr_resolves, st_quirk;
	unsigned int canary_logged;
	unsigned int canary_seen[128];
};

/* globals of the library (set in LibInit, read-only afterwards) */
extern struct ExecBase *SysBase;
extern struct DosLibrary *DOSBase;
extern struct Library *CyberGfxBase;

/* tglb_lib.c */
void tglb_flush(GLContext *c);
void tglb_resolve(GLContext *c);      /* tglb_manual.c: serialise deferred draws, check canaries */
void tglb_strict_init(GLContext *c);  /* tglb_manual.c */
void tglb_strict_free(GLContext *c);
void tglb_send_raw(GLContext *c, const void *p, unsigned int n);
int tglb_recv_raw(GLContext *c, void *p, unsigned int n);
unsigned char *tglb_cmd(GLContext *c, unsigned int op, unsigned int payload);
unsigned char *tglb_reserve(GLContext *c, unsigned int n);
void tglb_append(GLContext *c, const void *src, unsigned int n);
void tglb_pad(GLContext *c, unsigned int n);
unsigned int tglb_sync_u32(GLContext *c);
unsigned int tglb_reply_begin(GLContext *c);
void tglb_reply_skip(GLContext *c, unsigned int n);
void tglb_unimpl(GLContext *c, int slot);
void tglb_note(GLContext *c, const char *s);
int tglb_pname_count(unsigned int pname);
void tglb_memcpy(void *d, const void *s, unsigned int n);
void tglb_memset(void *d, int v, unsigned int n);
int tglb_getenv_int(const char *name, int def);
int tglb_getenv_str(const char *name, char *buf, int size);
void tglb_log(const char *fmt, ...);
void tglb_trace(const char *fmt, ...);
extern int tglb_trace_on;
GLContext *tglb_connect(void);
void tglb_disconnect(GLContext *c);

/* entry points of the 4 standard vectors (tglb_lib.c) */
APTR tglb_LibOpen(void);
APTR tglb_LibClose(void);
APTR tglb_LibExpunge(void);
APTR tglb_LibNull(void);

static inline unsigned char *tglb_put32(unsigned char *p, unsigned int v)
{
	*(unsigned int *)p = v;   /* the guest is big-endian: native = wire */
	return p + 4;
}
static inline unsigned char *tglb_putf(unsigned char *p, float f)
{
	union { float f; unsigned int u; } x;
	x.f = f;
	*(unsigned int *)p = x.u;
	return p + 4;
}
static inline unsigned char *tglb_putd(unsigned char *p, double d)
{
	union { double d; unsigned int u[2]; } x;
	x.d = d;
	((unsigned int *)p)[0] = x.u[0];
	((unsigned int *)p)[1] = x.u[1];
	return p + 8;
}
static inline unsigned char *tglb_put64(unsigned char *p, unsigned long long v)
{
	((unsigned int *)p)[0] = (unsigned int)(v >> 32);
	((unsigned int *)p)[1] = (unsigned int)v;
	return p + 8;
}

#endif
