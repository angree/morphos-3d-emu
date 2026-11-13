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
};

/* globals of the library (set in LibInit, read-only afterwards) */
extern struct ExecBase *SysBase;
extern struct DosLibrary *DOSBase;
extern struct Library *CyberGfxBase;

/* tglb_lib.c */
void tglb_flush(GLContext *c);
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
