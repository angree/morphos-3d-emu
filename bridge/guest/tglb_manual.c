/* SPDX-License-Identifier: MIT */
/* TGLBridge guest: the hand-written tinygl.library entries - context/GLA, swap, client vertex
 * arrays (+ glLockArraysEXT), pixel transfers (textures, glDrawPixels, glReadPixels), gets.
 * Payload layouts are mirrored in host/tglhost.c (search for the SLOT_ name). */
#include "tglb_internal.h"

#define CTXOK(c) ((c) && (c)->magic == TGLB_MAGIC)

/* ------------------------------------------------------------------ pixel layout */
/* bytes per pixel and swap element size for (format,type); 0 = unsupported */
static int pix_layout(unsigned int fmt, unsigned int type, int *esz)
{
	int comps;
	switch (type) {
	case 0x8032: case 0x8362: /* UNSIGNED_BYTE_3_3_2 / 2_3_3_REV */
		*esz = 1; return 1;
	case 0x8363: case 0x8364: case 0x8033: case 0x8365: case 0x8034: case 0x8366: /* 565 565R 4444 4444R 5551 1555R */
		*esz = 2; return 2;
	case 0x8035: case 0x8367: case 0x8036: case 0x8368: /* 8888 8888R 1010102 2101010R */
		*esz = 4; return 4;
	}
	switch (fmt) {
	case 0x1903: case 0x1904: case 0x1905: case 0x1906: case 0x1909: case 0x1902: case 0x1901: case 0x1900:
		comps = 1; break;   /* RED GREEN BLUE ALPHA LUMINANCE DEPTH STENCIL COLOR_INDEX */
	case 0x190A: comps = 2; break;                    /* LUMINANCE_ALPHA */
	case 0x1907: case 0x80E0: comps = 3; break;       /* RGB BGR */
	case 0x1908: case 0x80E1: case 0x8000: comps = 4; break; /* RGBA BGRA ABGR_EXT */
	default: return 0;
	}
	switch (type) {
	case 0x1400: case 0x1401: *esz = 1; return comps;       /* BYTE UBYTE */
	case 0x1402: case 0x1403: *esz = 2; return comps * 2;   /* SHORT USHORT */
	case 0x1404: case 0x1405: case 0x1406: *esz = 4; return comps * 4; /* INT UINT FLOAT */
	default: return 0;
	}
}

static unsigned int row_stride(int rowpix, int bpp, int esz, int align)
{
	unsigned int b = (unsigned int)rowpix * bpp;
	if (esz >= align) return b;
	return (b + align - 1) / align * align;
}

/* send w*h pixels from `pix` tight (rows of w*bpp), honouring the guest's unpack state */
static void send_pixels(GLContext *c, const unsigned char *pix, int w, int h, int bpp, int esz)
{
	int rowpix = c->unpack_rowlen > 0 ? c->unpack_rowlen : w;
	unsigned int stride = row_stride(rowpix, bpp, esz, c->unpack_align);
	unsigned int rowb = (unsigned int)w * bpp;
	int y;
	pix += c->unpack_skiprows * stride + c->unpack_skippix * bpp;
	if (stride == rowb) {
		tglb_append(c, pix, rowb * h);
	} else {
		for (y = 0; y < h; y++) tglb_append(c, pix + y * stride, rowb);
	}
	tglb_pad(c, rowb * h);
}

/* common body for TexImage/TexSubImage/DrawPixels: nfixed u32 already include has/esz/nbytes */
static void pixel_cmd(GLContext *c, int slot, const unsigned int *fixed, int nfixed,
                      int w, int h, unsigned int fmt, unsigned int type, const void *pix)
{
	int esz = 1, bpp = pix_layout(fmt, type, &esz), i;
	unsigned int nbytes = (pix && bpp && w > 0 && h > 0) ? (unsigned int)w * h * bpp : 0;
	unsigned char *p;
	if (!bpp && pix) { tglb_unimpl(c, slot); return; }
	p = tglb_cmd(c, slot, 4 * (nfixed + 3));
	((unsigned int *)(p - 8))[1] = 4 * (nfixed + 3) + ((nbytes + 3) & ~3u);
	for (i = 0; i < nfixed; i++) p = tglb_put32(p, fixed[i]);
	p = tglb_put32(p, nbytes ? 1 : 0);
	p = tglb_put32(p, (unsigned int)esz);
	p = tglb_put32(p, nbytes);
	if (nbytes) send_pixels(c, pix, w, h, bpp, esz);
}

void m_GLTexImage2D(GLContext *c, GLenum target, GLint level, GLint ifmt, GLsizei w, GLsizei h,
                    GLint border, GLenum fmt, GLenum type, const void *pix)
{
	unsigned int f[8];
	if (!CTXOK(c)) return;
	f[0] = target; f[1] = level; f[2] = ifmt; f[3] = w; f[4] = h; f[5] = border; f[6] = fmt; f[7] = type;
	pixel_cmd(c, SLOT_GLTexImage2D, f, 8, w, h, fmt, type, pix);
}

void m_GLTexSubImage2D(GLContext *c, GLenum target, GLint level, GLint x, GLint y, GLsizei w, GLsizei h,
                       GLenum fmt, GLenum type, const void *pix)
{
	unsigned int f[8];
	if (!CTXOK(c)) return;
	f[0] = target; f[1] = level; f[2] = x; f[3] = y; f[4] = w; f[5] = h; f[6] = fmt; f[7] = type;
	pixel_cmd(c, SLOT_GLTexSubImage2D, f, 8, w, h, fmt, type, pix);
}

void m_GLTexImage1D(GLContext *c, GLenum target, GLint level, GLint ifmt, GLsizei w, GLint border,
                    GLenum fmt, GLenum type, const void *pix)
{
	unsigned int f[7];
	if (!CTXOK(c)) return;
	f[0] = target; f[1] = level; f[2] = ifmt; f[3] = w; f[4] = border; f[5] = fmt; f[6] = type;
	pixel_cmd(c, SLOT_GLTexImage1D, f, 7, w, 1, fmt, type, pix);
}

void m_GLTexSubImage1D(GLContext *c, GLenum target, GLint level, GLint x, GLsizei w,
                       GLenum fmt, GLenum type, const void *pix)
{
	unsigned int f[6];
	if (!CTXOK(c)) return;
	f[0] = target; f[1] = level; f[2] = x; f[3] = w; f[4] = fmt; f[5] = type;
	pixel_cmd(c, SLOT_GLTexSubImage1D, f, 6, w, 1, fmt, type, pix);
}

void m_GLDrawPixels(GLContext *c, GLsizei w, GLsizei h, GLenum fmt, GLenum type, const void *pix)
{
	unsigned int f[4];
	if (!CTXOK(c)) return;
	f[0] = w; f[1] = h; f[2] = fmt; f[3] = type;
	pixel_cmd(c, SLOT_GLDrawPixels, f, 4, w, h, fmt, type, pix);
}

void m_GLReadPixels(GLContext *c, GLint x, GLint y, GLsizei w, GLsizei h, GLenum fmt, GLenum type, void *out)
{
	int esz = 1, bpp, yy, rowpix;
	unsigned int len, rowb, stride;
	unsigned char *p, *dst;
	if (!CTXOK(c)) return;
	bpp = pix_layout(fmt, type, &esz);
	if (!bpp || w <= 0 || h <= 0) { tglb_unimpl(c, SLOT_GLReadPixels); return; }
	p = tglb_cmd(c, SLOT_GLReadPixels, 24);
	p = tglb_put32(p, x); p = tglb_put32(p, y); p = tglb_put32(p, w); p = tglb_put32(p, h);
	p = tglb_put32(p, fmt); p = tglb_put32(p, type);
	len = tglb_reply_begin(c);
	rowb = (unsigned int)w * bpp;
	rowpix = c->pack_rowlen > 0 ? c->pack_rowlen : w;
	stride = row_stride(rowpix, bpp, esz, c->pack_align);
	dst = (unsigned char *)out + c->pack_skiprows * stride + c->pack_skippix * bpp;
	for (yy = 0; yy < h && len >= rowb; yy++) {
		tglb_recv_raw(c, dst + yy * stride, rowb);
		len -= rowb;
	}
	tglb_reply_skip(c, len);
}

void m_GLPixelStorei(GLContext *c, GLenum pname, GLint v)
{
	if (!CTXOK(c)) return;
	/* not forwarded: the guest packs pixel data tight, the host always uses alignment 1 */
	switch (pname) {
	case 0x0CF5: c->unpack_align = v; break;
	case 0x0CF2: c->unpack_rowlen = v; break;
	case 0x0CF3: c->unpack_skiprows = v; break;
	case 0x0CF4: c->unpack_skippix = v; break;
	case 0x0D05: c->pack_align = v; break;
	case 0x0D02: c->pack_rowlen = v; break;
	case 0x0D03: c->pack_skiprows = v; break;
	case 0x0D04: c->pack_skippix = v; break;
	default: tglb_unimpl(c, SLOT_GLPixelStorei); break;  /* SWAP_BYTES, LSB_FIRST, IMAGE_HEIGHT... */
	}
}

void m_GLPixelStoref(GLContext *c, GLenum pname, GLfloat v)
{
	m_GLPixelStorei(c, pname, (GLint)v);
}

/* ------------------------------------------------------------------ textures */
void m_GLGenTextures(GLContext *c, GLsizei n, GLuint *names)
{
	unsigned int len;
	int i;
	if (!CTXOK(c) || n <= 0) return;
	tglb_put32(tglb_cmd(c, SLOT_GLGenTextures, 4), n);
	len = tglb_reply_begin(c);
	for (i = 0; i < n; i++) {
		if (len >= 4) { tglb_recv_raw(c, &names[i], 4); len -= 4; }
		else names[i] = 0;
	}
	tglb_reply_skip(c, len);
}

void m_GLDeleteTextures(GLContext *c, GLsizei n, const GLuint *names)
{
	unsigned char *p;
	if (!CTXOK(c) || n <= 0) return;
	p = tglb_cmd(c, SLOT_GLDeleteTextures, 4);
	((unsigned int *)(p - 8))[1] = 4 + 4 * n;
	tglb_put32(p, n);
	tglb_append(c, names, 4 * n);
}

/* ------------------------------------------------------------------ simple syncs */
void m_GLFlush(GLContext *c)
{
	if (!CTXOK(c)) return;
	tglb_cmd(c, SLOT_GLFlush, 0);
	tglb_flush(c);
}

void m_GLFinish(GLContext *c)
{
	if (!CTXOK(c)) return;
	tglb_cmd(c, SLOT_GLFinish, 0);
	tglb_sync_u32(c);
}

const GLubyte *m_GLGetString(GLContext *c, GLenum name)
{
	int i, slot = -1;
	unsigned int len;
	char *s;
	if (!CTXOK(c)) return NULL;
	for (i = 0; i < TGLB_NSTR; i++) {
		if (c->str_val[i] && c->str_name[i] == name) return (const GLubyte *)c->str_val[i];
		if (!c->str_val[i] && slot < 0) slot = i;
	}
	tglb_put32(tglb_cmd(c, SLOT_GLGetString, 4), name);
	len = tglb_reply_begin(c);
	if (slot < 0 || !len) { tglb_reply_skip(c, len); return NULL; }
	s = AllocVec(len + 1, MEMF_PUBLIC);
	if (!s) { tglb_reply_skip(c, len); return NULL; }
	tglb_recv_raw(c, s, len);
	s[len] = 0;
	c->str_name[slot] = name;
	c->str_val[slot] = s;
	return (const GLubyte *)s;
}

/* answered in the guest (state the host never sees); returns 1 if handled */
static int local_get(GLContext *c, GLenum pname, int *v)
{
	switch (pname) {
	case 0x0CF5: *v = c->unpack_align; return 1;
	case 0x0CF2: *v = c->unpack_rowlen; return 1;
	case 0x0CF3: *v = c->unpack_skiprows; return 1;
	case 0x0CF4: *v = c->unpack_skippix; return 1;
	case 0x0D05: *v = c->pack_align; return 1;
	case 0x0D02: *v = c->pack_rowlen; return 1;
	case 0x0D03: *v = c->pack_skiprows; return 1;
	case 0x0D04: *v = c->pack_skippix; return 1;
	case 0x84E1: *v = 0x84C0 + c->client_unit; return 1;   /* CLIENT_ACTIVE_TEXTURE */
	}
	return 0;
}

/* host reply: u32 n, n values of 4 bytes (8 for GetDoublev) */
static unsigned int get_begin(GLContext *c, int slot, GLenum pname)
{
	unsigned int len, n = 0;
	tglb_put32(tglb_cmd(c, slot, 4), pname);
	len = tglb_reply_begin(c);
	if (len >= 4) { tglb_recv_raw(c, &n, 4); }
	return n;
}

void m_GLGetIntegerv(GLContext *c, GLenum pname, GLint *out)
{
	unsigned int n, i;
	int v;
	if (!CTXOK(c)) return;
	if (local_get(c, pname, &v)) { out[0] = v; return; }
	n = get_begin(c, SLOT_GLGetIntegerv, pname);
	for (i = 0; i < n; i++) tglb_recv_raw(c, &out[i], 4);
}

void m_GLGetFloatv(GLContext *c, GLenum pname, GLfloat *out)
{
	unsigned int n, i;
	int v;
	if (!CTXOK(c)) return;
	if (local_get(c, pname, &v)) { out[0] = (GLfloat)v; return; }
	n = get_begin(c, SLOT_GLGetFloatv, pname);
	for (i = 0; i < n; i++) tglb_recv_raw(c, &out[i], 4);
}

void m_GLGetDoublev(GLContext *c, GLenum pname, GLdouble *out)
{
	unsigned int n, i;
	int v;
	if (!CTXOK(c)) return;
	if (local_get(c, pname, &v)) { out[0] = (GLdouble)v; return; }
	n = get_begin(c, SLOT_GLGetDoublev, pname);
	for (i = 0; i < n; i++) tglb_recv_raw(c, &out[i], 8);
}

void m_GLGetBooleanv(GLContext *c, GLenum pname, GLboolean *out)
{
	unsigned int n, i, b;
	int v;
	if (!CTXOK(c)) return;
	if (local_get(c, pname, &v)) { out[0] = v != 0; return; }
	n = get_begin(c, SLOT_GLGetBooleanv, pname);
	for (i = 0; i < n; i++) { tglb_recv_raw(c, &b, 4); out[i] = (GLboolean)(b != 0); }
}

/* ------------------------------------------------------------------ client arrays */
static int type_size(unsigned int t)
{
	switch (t) {
	case 0x1400: case 0x1401: return 1;
	case 0x1402: case 0x1403: return 2;
	case 0x1404: case 0x1405: case 0x1406: return 4;
	case 0x140A: return 8;
	}
	return 0;
}

static void set_array(GLContext *c, int id, int size, GLenum type, GLsizei stride, const void *ptr)
{
	struct tglb_array *a = &c->arr[id];
	a->size = size; a->type = type; a->stride = stride; a->ptr = ptr; a->sent = 0;
}

void m_GLVertexPointer(GLContext *c, GLint size, GLenum type, GLsizei stride, const void *ptr)
{ if (CTXOK(c)) set_array(c, ARR_VERTEX, size, type, stride, ptr); }
void m_GLColorPointer(GLContext *c, GLint size, GLenum type, GLsizei stride, const void *ptr)
{ if (CTXOK(c)) set_array(c, ARR_COLOR, size, type, stride, ptr); }
void m_GLNormalPointer(GLContext *c, GLenum type, GLsizei stride, const void *ptr)
{ if (CTXOK(c)) set_array(c, ARR_NORMAL, 3, type, stride, ptr); }
void m_GLTexCoordPointer(GLContext *c, GLint size, GLenum type, GLsizei stride, const void *ptr)
{ if (CTXOK(c)) set_array(c, ARR_TEX0 + c->client_unit, size, type, stride, ptr); }

static int cap_to_id(GLContext *c, GLenum cap)
{
	switch (cap) {
	case 0x8074: return ARR_VERTEX;
	case 0x8075: return ARR_NORMAL;
	case 0x8076: return ARR_COLOR;
	case 0x8078: return ARR_TEX0 + c->client_unit;
	}
	return -1;
}

static void client_state(GLContext *c, int slot, GLenum cap, int on)
{
	int id = cap_to_id(c, cap);
	if (id >= 0) { c->arr[id].enabled = on; c->arr[id].sent = 0; }
	tglb_put32(tglb_cmd(c, slot, 4), cap);
}

void m_GLEnableClientState(GLContext *c, GLenum cap)
{ if (CTXOK(c)) client_state(c, SLOT_GLEnableClientState, cap, 1); }
void m_GLDisableClientState(GLContext *c, GLenum cap)
{ if (CTXOK(c)) client_state(c, SLOT_GLDisableClientState, cap, 0); }

void m_GLClientActiveTexture(GLContext *c, GLenum tex)
{
	int u;
	if (!CTXOK(c)) return;
	u = (int)tex - 0x84C0;
	if (u < 0 || u >= TGLB_MAXUNITS) u = 0;
	c->client_unit = u;
	tglb_put32(tglb_cmd(c, SLOT_GLClientActiveTexture, 4), tex);
}

void m_GLLockArraysEXT(GLContext *c, GLint first, GLsizei count)
{
	int i;
	if (!CTXOK(c)) return;
	c->locked = 1; c->lock_first = first; c->lock_count = count;
	for (i = 0; i < ARR_COUNT; i++) c->arr[i].sent = 0;
}

void m_GLUnlockArraysEXT(GLContext *c)
{
	int i;
	if (!CTXOK(c)) return;
	c->locked = 0;
	for (i = 0; i < ARR_COUNT; i++) c->arr[i].sent = 0;
}

/* T73 strict mode: STRICT_STRIDE_* - real TinyGL (Radeon) drew interleaved arrays wrong (wow3d 3309/3315 terrain
 * shards). The exact driver behaviour is unknown; the emulation reads a strided array as if it were tight. */
static int quirk_tight(GLContext *c, int id)
{
	if (id == ARR_VERTEX) return (c->strict & STRICT_STRIDE_XYZ) != 0;
	return (c->strict & STRICT_STRIDE_TCCOL) != 0;
}

/* upload the enabled arrays of `arr` for vertices [first, first+count); dedupe = use the per-lock `sent` flags */
static void send_arrays_st(GLContext *c, struct tglb_array *arr, int locked, int lock_first, int lock_count,
                           int dedupe, int first, int count)
{
	int i, use_lock = 0;
	if (locked && first >= lock_first && first + count <= lock_first + lock_count) {
		use_lock = 1;
		first = lock_first;
		count = lock_count;
	}
	for (i = 0; i < ARR_COUNT; i++) {
		struct tglb_array *a = &arr[i];
		int ts, elem, stride, v;
		unsigned int nbytes;
		unsigned char *p;
		const unsigned char *src;
		if (!a->enabled || !a->ptr) continue;
		if (dedupe && use_lock && a->sent) continue;
		ts = type_size(a->type);
		if (!ts) { tglb_unimpl(c, SLOT_GLVertexPointer); continue; }
		elem = ts * a->size;
		stride = a->stride ? a->stride : elem;
		if (stride != elem && quirk_tight(c, i)) { stride = elem; c->st_quirk++; }
		nbytes = (unsigned int)elem * count;
		p = tglb_cmd(c, OP_ARRAY, 20);
		((unsigned int *)(p - 8))[1] = 20 + ((nbytes + 3) & ~3u);
		p = tglb_put32(p, i); p = tglb_put32(p, a->size); p = tglb_put32(p, a->type);
		p = tglb_put32(p, first); p = tglb_put32(p, count);
		src = a->ptr + (unsigned int)first * stride;
		if (stride == elem) {
			tglb_append(c, src, nbytes);
		} else if (elem <= 64) {
			for (v = 0; v < count; v++) {
				unsigned char *d = tglb_reserve(c, elem);
				tglb_memcpy(d, src + (unsigned int)v * stride, elem);
			}
		} else {
			for (v = 0; v < count; v++) tglb_append(c, src + (unsigned int)v * stride, elem);
		}
		tglb_pad(c, nbytes);
		if (dedupe) a->sent = use_lock;
	}
}

static void send_arrays(GLContext *c, int first, int count)
{
	send_arrays_st(c, c->arr, c->locked, c->lock_first, c->lock_count, 1, first, count);
}

static void emit_drawarrays(GLContext *c, GLenum mode, GLint first, GLsizei count)
{
	unsigned char *p = tglb_cmd(c, SLOT_GLDrawArrays, 12);
	p = tglb_put32(p, mode); p = tglb_put32(p, first); tglb_put32(p, count);
}

static void index_range(GLenum type, const void *idx, int count, unsigned int *mn, unsigned int *mx)
{
	unsigned int lo = 0xffffffffu, hi = 0;
	int i;
	if (type == 0x1403) {
		const unsigned short *s = idx;
		for (i = 0; i < count; i++) { unsigned int v = s[i]; if (v < lo) lo = v; if (v > hi) hi = v; }
	} else if (type == 0x1405) {
		const unsigned int *s = idx;
		for (i = 0; i < count; i++) { unsigned int v = s[i]; if (v < lo) lo = v; if (v > hi) hi = v; }
	} else {
		const unsigned char *s = idx;
		for (i = 0; i < count; i++) { unsigned int v = s[i]; if (v < lo) lo = v; if (v > hi) hi = v; }
	}
	*mn = lo; *mx = hi;
}

static int idx_size(GLenum type) { return type == 0x1403 ? 2 : type == 0x1405 ? 4 : 1; }

static void emit_elements(GLContext *c, int slot, GLenum mode, GLsizei count, GLenum type, const void *idx)
{
	unsigned int nbytes = (unsigned int)count * idx_size(type);
	unsigned char *p = tglb_cmd(c, slot, 16);
	((unsigned int *)(p - 8))[1] = 16 + ((nbytes + 3) & ~3u);
	p = tglb_put32(p, mode); p = tglb_put32(p, count); p = tglb_put32(p, type); tglb_put32(p, nbytes);
	tglb_append(c, idx, nbytes);
	tglb_pad(c, nbytes);
}

static void draw_elements(GLContext *c, int slot, GLenum mode, unsigned int lo, unsigned int hi,
                          GLsizei count, GLenum type, const void *idx)
{
	send_arrays(c, (int)lo, (int)(hi - lo + 1));
	emit_elements(c, slot, mode, count, type, idx);
}

/* ------------------------------------------------------------------ T73 strict: deferred draws + canary */
static unsigned int fnv(unsigned int h, const unsigned char *s, unsigned int n)
{
	while (n--) h = (h ^ *s++) * 16777619u;
	return h;
}

static unsigned int sum_array(const struct tglb_array *a, unsigned int first, unsigned int count)
{
	unsigned int h = 2166136261u, elem, stride, v;
	const unsigned char *s;
	int ts = type_size(a->type);
	if (!a->enabled || !a->ptr || !ts) return 0;
	elem = (unsigned int)ts * a->size;
	stride = a->stride ? (unsigned int)a->stride : elem;
	s = a->ptr + first * stride;
	if (stride == elem) return fnv(h, s, elem * count);
	for (v = 0; v < count; v++, s += stride) h = fnv(h, s, elem);
	return h;
}

/* the vertex range of a record (as seen at the draw call) */
static void pend_range(const struct tglb_pend *p, unsigned int *lo, unsigned int *hi)
{
	if (p->kind == 1) { *lo = (unsigned int)p->first; *hi = (unsigned int)(p->first + p->count - 1); return; }
	*lo = p->lo; *hi = p->hi;
}

static void pend_sums(const struct tglb_pend *p, unsigned int *out)
{
	unsigned int lo, hi;
	int i;
	pend_range(p, &lo, &hi);
	for (i = 0; i < ARR_COUNT; i++) out[i] = hi >= lo ? sum_array(&p->arr[i], lo, hi - lo + 1) : 0;
	out[ARR_COUNT] = p->kind == 0 ? fnv(2166136261u, p->idx, (unsigned int)p->count * idx_size(p->type)) : 0;
}

static struct tglb_pend *pend_new(GLContext *c)
{
	struct tglb_pend *p;
	if (!c->pend) return NULL;
	if (c->npend >= c->pend_cap) tglb_flush(c);   /* a full queue is a flush point */
	p = &c->pend[c->npend++];
	tglb_memset(p, 0, sizeof(*p));
	p->off = c->used;
	p->draw_no = c->draw_no;
	tglb_memcpy(p->arr, c->arr, sizeof(p->arr));
	p->locked = c->locked; p->lock_first = c->lock_first; p->lock_count = c->lock_count;
	return p;
}

static const char *arr_name(int i)
{
	static const char *n[ARR_COUNT] = { "vertex", "color", "normal", "tex0", "tex1", "tex2", "tex3", "tex4", "tex5", "tex6", "tex7" };
	return i < ARR_COUNT ? n[i] : "indices";
}

static void canary_check(GLContext *c, struct tglb_pend *p)
{
	unsigned int now[ARR_COUNT + 1];
	int i, bad = 0;
	pend_sums(p, now);
	for (i = 0; i <= ARR_COUNT; i++) {
		if (now[i] == p->sum[i]) continue;
		bad++;
		{   /* log each changed buffer (array kind + address) once: up to 128 different ones */
			unsigned int lo, hi, key, k;
			const struct tglb_array *a = i < ARR_COUNT ? &p->arr[i] : NULL;
			key = (unsigned int)(a ? (unsigned long)a->ptr : (unsigned long)p->idx) ^ (unsigned int)i;
			for (k = 0; k < c->canary_logged && c->canary_seen[k] != key; k++) ;
			if (k < c->canary_logged || c->canary_logged >= 128) continue;
			c->canary_seen[c->canary_logged++] = key;
			pend_range(p, &lo, &hi);
			tglb_log("tglbridge strict: CANARY frame %lu draw %lu: %s %p (stride %ld, verts %lu..%lu, %ld indices) changed between the draw call and the flush",
			         (unsigned long)c->swaps + 1, (unsigned long)p->draw_no, arr_name(i),
			         a ? (void *)a->ptr : (void *)p->idx, a ? (long)a->stride : 0L, (unsigned long)lo, (unsigned long)hi,
			         (long)p->count);
		}
	}
	if (bad) { c->st_canary_bad++; c->st_fr_bad++; }
	c->st_canary_draws++;
}

static void emit_pend(GLContext *c, struct tglb_pend *p)
{
	unsigned int lo, hi;
	const void *idx = p->idx_copy ? p->idx_copy : p->idx;
	if (p->kind == 1) {
		send_arrays_st(c, p->arr, p->locked, p->lock_first, p->lock_count, 0, p->first, p->count);
		emit_drawarrays(c, p->mode, p->first, p->count);
		return;
	}
	if (!p->hi_given) index_range(p->type, idx, p->count, &lo, &hi);   /* lazy indices: the range of what is there now */
	else { lo = p->lo; hi = p->hi; }
	if (hi < lo) return;
	send_arrays_st(c, p->arr, p->locked, p->lock_first, p->lock_count, 0, (int)lo, (int)(hi - lo + 1));
	emit_elements(c, SLOT_GLDrawElements, p->mode, p->count, p->type, idx);
}

/* flush point: splice the deferred draws into the stream at their places, reading guest memory now */
void tglb_resolve(GLContext *c)
{
	unsigned char *mainb = c->buf;
	unsigned int used = c->used, pos = 0;
	int k;
	c->in_resolve = 1;
	for (k = 0; k < c->npend; k++) {
		struct tglb_pend *p = &c->pend[k];
		if (c->strict & STRICT_CANARY) canary_check(c, p);
		if (!p->emitted && c->buf2) {
			if (p->off > pos) { tglb_send_raw(c, mainb + pos, p->off - pos); pos = p->off; }
			c->buf = c->buf2; c->used = 0;
			emit_pend(c, p);
			tglb_flush(c);                         /* in_resolve: plain send of buf2 */
			c->buf = mainb;
		}
		if (p->idx_copy) { FreeVec(p->idx_copy); p->idx_copy = NULL; }
	}
	if (used > pos) tglb_send_raw(c, mainb + pos, used - pos);
	c->buf = mainb; c->used = 0; c->npend = 0;
	c->in_resolve = 0;
	c->owed += c->swaps_in_buf; c->swaps_in_buf = 0; c->last_off = -1; c->split = 0;
	c->st_resolves++; c->st_fr_resolves++;
}

void tglb_strict_init(GLContext *c)
{
	unsigned char *p;
	int maxtex, texmb;
	c->strict = (unsigned int)tglb_getenv_int("TGLBRIDGE_STRICT", 0);
	if (!c->strict) return;
	if (c->strict & (STRICT_LAZY_INDICES | STRICT_LAG_SWAP)) c->strict |= STRICT_LAZY_ARRAYS;
	maxtex = tglb_getenv_int("TGLBRIDGE_MAXTEX", 2048);
	texmb = tglb_getenv_int("TGLBRIDGE_TEXMB", 96);
	c->lag_n = tglb_getenv_int("TGLBRIDGE_LAG", 3);
	c->lazy_min = tglb_getenv_int("TGLBRIDGE_LAZYMIN", 0);
	if (c->lag_n < 1) c->lag_n = 1;
	if (c->strict & (STRICT_LAZY_ARRAYS | STRICT_CANARY)) {
		c->pend_cap = 8192;
		c->pend = AllocVec(sizeof(struct tglb_pend) * c->pend_cap, MEMF_PUBLIC);
		c->buf2 = AllocVec(TGLB_BUFSZ, MEMF_PUBLIC);
		if (!c->pend || !c->buf2) {
			if (c->pend) FreeVec(c->pend);
			if (c->buf2) FreeVec(c->buf2);
			c->pend = NULL; c->buf2 = NULL; c->pend_cap = 0;
			c->strict &= ~(STRICT_LAZY_ARRAYS | STRICT_LAZY_INDICES | STRICT_CANARY | STRICT_LAG_SWAP);
			tglb_log("tglbridge strict: no memory for the deferred-draw queue, lazy/canary off");
		}
	}
	tglb_log("tglbridge strict: flags 0x%lx (lazy arrays %ld, lazy indices %ld, stride quirk tc/col %ld xyz %ld, limits %ld (max tex %ld, budget %ld MB), canary %ld, lag swap %ld frames %ld, lazy from %ld vertices)",
	         (unsigned long)c->strict, (long)!!(c->strict & STRICT_LAZY_ARRAYS), (long)!!(c->strict & STRICT_LAZY_INDICES),
	         (long)!!(c->strict & STRICT_STRIDE_TCCOL), (long)!!(c->strict & STRICT_STRIDE_XYZ), (long)!!(c->strict & STRICT_LIMITS),
	         (long)maxtex, (long)texmb, (long)!!(c->strict & STRICT_CANARY), (long)!!(c->strict & STRICT_LAG_SWAP), (long)c->lag_n, (long)c->lazy_min);
	p = tglb_cmd(c, OP_STRICT, 12);
	p = tglb_put32(p, c->strict); p = tglb_put32(p, (unsigned int)maxtex); tglb_put32(p, (unsigned int)texmb * 1024u);
}

void tglb_strict_free(GLContext *c)
{
	int k;
	for (k = 0; k < c->npend; k++) if (c->pend[k].idx_copy) FreeVec(c->pend[k].idx_copy);
	c->npend = 0;
	if (c->pend) FreeVec(c->pend);
	if (c->buf2) FreeVec(c->buf2);
	c->pend = NULL; c->buf2 = NULL;
}

static char *put_num(char *o, char *e, unsigned long v)
{
	char t[12];
	int n = 0;
	do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v && n < 12);
	while (n && o < e) *o++ = t[--n];
	return o;
}

/* per swap: a bad frame (canary) or every 100th swap -> guest log + a note in the host log */
static void strict_frame(GLContext *c)
{
	if (!c->strict) return;
	if (c->st_fr_bad) c->st_canary_frames++;
	if (c->st_fr_bad || ((c->swaps % 100) == 0 && c->swaps)) {
		const char *t = c->st_fr_bad ? "strict: CANARY BAD FRAME" : "strict: summary";
		static const char *lbl[6] = { " frame ", " bad_draws ", " draws ", " total_bad ", " bad_frames ", " deferred " };
		unsigned long v[6];
		char m[200], *o = m, *e = m + sizeof(m) - 1;
		int j;
		tglb_log("tglbridge %s frame %lu: %lu of %lu draws changed before the flush; totals: deferred %lu, resolves %lu, early queries %lu, canary checked %lu bad %lu in %lu frames, stride-quirk arrays %lu",
		         t, (unsigned long)c->swaps, c->st_fr_bad, (unsigned long)c->draw_no, c->st_deferred, c->st_resolves, c->st_oob,
		         c->st_canary_draws, c->st_canary_bad, c->st_canary_frames, c->st_quirk);
		v[0] = c->swaps; v[1] = c->st_fr_bad; v[2] = c->draw_no; v[3] = c->st_canary_bad; v[4] = c->st_canary_frames;
		v[5] = c->st_deferred;
		while (*t && o < e) *o++ = *t++;
		for (j = 0; j < 6; j++) {
			const char *l = lbl[j];
			while (*l && o < e) *o++ = *l++;
			o = put_num(o, e, v[j]);
		}
		*o = 0;
		tglb_note(c, m);
	}
	c->st_fr_bad = 0; c->st_fr_deferred = 0; c->st_fr_resolves = 0;
	c->draw_no = 0;
}

void m_GLDrawArrays(GLContext *c, GLenum mode, GLint first, GLsizei count)
{
	struct tglb_pend *p;
	if (!CTXOK(c) || count <= 0) return;
	c->draw_no++;
	if ((c->strict & STRICT_LAZY_ARRAYS) && count >= c->lazy_min && (p = pend_new(c)) != NULL) {
		p->kind = 1; p->mode = mode; p->first = first; p->count = count;
		if (c->strict & STRICT_CANARY) pend_sums(p, p->sum);
		c->st_deferred++; c->st_fr_deferred++;
		return;
	}
	send_arrays(c, first, count);
	emit_drawarrays(c, mode, first, count);
	if ((c->strict & STRICT_CANARY) && (p = pend_new(c)) != NULL) {
		p->kind = 1; p->mode = mode; p->first = first; p->count = count; p->emitted = 1;
		pend_sums(p, p->sum);
	}
}

/* lo/hi: have_range = known now; hi_given = final for the draw too (glDrawRangeElements) */
static void elements(GLContext *c, GLenum mode, GLsizei count, GLenum type, const void *idx,
                     unsigned int lo, unsigned int hi, int have_range, int hi_given)
{
	struct tglb_pend *p;
	c->draw_no++;
	if ((c->strict & STRICT_LAZY_ARRAYS) && c->lazy_min > 0 && !have_range) {
		index_range(type, idx, count, &lo, &hi);
		have_range = 1;
	}
	if ((c->strict & STRICT_LAZY_ARRAYS) && (!have_range || hi < lo || (int)(hi - lo + 1) >= c->lazy_min) &&
	    (p = pend_new(c)) != NULL) {
		p->kind = 0; p->mode = mode; p->count = count; p->type = type; p->idx = idx;
		if (!have_range && (!(c->strict & STRICT_LAZY_INDICES) || (c->strict & STRICT_CANARY))) {
			index_range(type, idx, count, &lo, &hi);
			have_range = 1;
		}
		p->lo = lo; p->hi = hi;
		p->hi_given = hi_given;
		if (!(c->strict & STRICT_LAZY_INDICES)) {          /* indices read now, arrays later */
			unsigned int n = (unsigned int)count * idx_size(type);
			p->idx_copy = AllocVec(n + 4, MEMF_PUBLIC);
			if (p->idx_copy) tglb_memcpy(p->idx_copy, idx, n);
			p->hi_given = 1;                               /* range of the indices as they were at the call */
		}
		if (c->strict & STRICT_CANARY) pend_sums(p, p->sum);
		c->st_deferred++; c->st_fr_deferred++;
		return;
	}
	if (!have_range) index_range(type, idx, count, &lo, &hi);
	draw_elements(c, SLOT_GLDrawElements, mode, lo, hi, count, type, idx);
	if ((c->strict & STRICT_CANARY) && (p = pend_new(c)) != NULL) {
		p->kind = 0; p->mode = mode; p->count = count; p->type = type; p->idx = idx; p->emitted = 1;
		p->lo = lo; p->hi = hi;
		pend_sums(p, p->sum);
	}
}

void m_GLDrawElements(GLContext *c, GLenum mode, GLsizei count, GLenum type, const void *idx)
{
	if (!CTXOK(c) || count <= 0 || !idx) { if (CTXOK(c) && !idx && count > 0) tglb_unimpl(c, SLOT_GLDrawElements); return; }
	elements(c, mode, count, type, idx, 0, 0, 0, 0);
}

void m_GLDrawRangeElements(GLContext *c, GLenum mode, GLuint start, GLuint end, GLsizei count,
                           GLenum type, const void *idx)
{
	if (!CTXOK(c) || count <= 0 || !idx || end < start) return;
	/* sent as a plain glDrawElements: the host does not need the hint */
	elements(c, mode, count, type, idx, start, end, 1, 1);
}

void m_GLArrayElement(GLContext *c, GLint i)
{
	if (!CTXOK(c)) return;
	send_arrays(c, i, 1);
	tglb_put32(tglb_cmd(c, SLOT_GLArrayElement, 4), i);
}

/* ------------------------------------------------------------------ context / GLA */
GLContext *m_GLInit(void)
{
	GLContext *c;
	tglb_trace("GLInit");
	c = tglb_connect();
	tglb_trace("GLInit -> %p", c);
	return c;
}

void m_GLClose(GLContext *c)
{
	if (!CTXOK(c)) return;
	tglb_disconnect(c);
}

void *m_TGLGetContexts(void) { return NULL; }

void *m_TGLGetProcAddress(GLContext **pc, const char *name)
{
	(void)name;
	if (pc) tglb_unimpl(*pc, SLOT_TGLGetProcAddress);
	return NULL;
}

void m_TGLEnableNewExtensions(GLContext *c, unsigned int v) { (void)c; (void)v; }
void m_TGLSetContextVersion(GLContext *c, unsigned int v) { if (CTXOK(c)) c->ctxversion = v; }
unsigned int m_TGLGetContextVersion(GLContext *c) { return CTXOK(c) ? c->ctxversion : 0; }

static int create(GLContext *c, int kind, int w, int h)
{
	unsigned char *p = tglb_cmd(c, OP_CREATE, 16);
	p = tglb_put32(p, kind); p = tglb_put32(p, w); p = tglb_put32(p, h); tglb_put32(p, 0);
	tglb_trace("CREATE kind %ld %ldx%ld", (long)kind, (long)w, (long)h);
	if (tglb_sync_u32(c) != 1) return 0;
	c->kind = kind; c->w = w; c->h = h;
	tglb_log("tglbridge: context %ldx%ld kind %ld", (long)w, (long)h, (long)kind);
	return 1;
}

int m_GLAInitializeContextWindowed(GLContext *c, void *window)
{
	struct Window *win = window;
	if (!CTXOK(c) || !win) return 0;
	c->win = win; c->scr = NULL;
	return create(c, 1, win->Width - win->BorderLeft - win->BorderRight,
	              win->Height - win->BorderTop - win->BorderBottom);
}

int m_GLAReinitializeContextWindowed(GLContext *c, void *window)
{
	struct Window *win = window;
	unsigned char *p;
	if (!CTXOK(c) || !win) return 0;
	c->win = win;
	c->w = win->Width - win->BorderLeft - win->BorderRight;
	c->h = win->Height - win->BorderTop - win->BorderBottom;
	p = tglb_cmd(c, OP_RESIZE, 8);
	p = tglb_put32(p, c->w); tglb_put32(p, c->h);
	return 1;
}

int m_GLAInitializeContextScreen(GLContext *c, void *screen)
{
	struct Screen *scr = screen;
	if (!CTXOK(c) || !scr) return 0;
	c->scr = scr; c->win = NULL;
	return create(c, 2, scr->Width, scr->Height);
}

int m_GLAInitializeContextBitMap(GLContext *c, void *bm)
{
	(void)bm;
	if (CTXOK(c)) tglb_unimpl(c, SLOT_GLAInitializeContextBitMap);
	return 0;
}

int m_GLAInitializeContext(GLContext *c, struct TagItem *tags)
{
	struct TagItem *t;
	void *scr = NULL, *win = NULL, *bm = NULL;
	if (!CTXOK(c)) return 0;
	for (t = tags; t && t->ti_Tag != TAG_DONE; t++) {
		if (t->ti_Tag == TGL_CONTEXT_SCREEN) scr = (void *)t->ti_Data;
		else if (t->ti_Tag == TGL_CONTEXT_WINDOW) win = (void *)t->ti_Data;
		else if (t->ti_Tag == TGL_CONTEXT_BITMAP) bm = (void *)t->ti_Data;
		/* TGL_CONTEXT_STENCIL / NODEPTH: the host always has depth+stencil */
	}
	if (scr) return m_GLAInitializeContextScreen(c, scr);
	if (win) return m_GLAInitializeContextWindowed(c, win);
	if (bm) return m_GLAInitializeContextBitMap(c, bm);
	return 0;
}

static void destroy(GLContext *c)
{
	if (!CTXOK(c) || !c->kind) return;
	tglb_cmd(c, OP_DESTROY, 0);
	tglb_flush(c);
	c->kind = 0; c->win = NULL; c->scr = NULL;
}
void m_GLADestroyContext(GLContext *c) { destroy(c); }
void m_GLADestroyContextWindowed(GLContext *c) { destroy(c); }
void m_GLADestroyContextScreen(GLContext *c) { destroy(c); }
void m_GLADestroyContextBitMap(GLContext *c) { destroy(c); }

void m_GLASetSync(GLContext *c, int enable)
{
	if (!CTXOK(c)) return;
	tglb_put32(tglb_cmd(c, OP_SETSYNC, 4), enable ? 1 : 0);
}

void m_GLASetAttr(GLContext *c, unsigned int a, unsigned int v)
{
	(void)a; (void)v;
	if (CTXOK(c)) tglb_unimpl(c, SLOT_GLASetAttr);
}

void m_GLASwapBuffers(GLContext *c)
{
	unsigned int len, hdr[3], need;
	int mirror;
	if (!CTXOK(c)) return;
	c->swaps++;
	if (c->swaps <= 3) tglb_trace("swap %lu", (unsigned long)c->swaps);
	mirror = c->mirror_every > 0 && (c->swaps % c->mirror_every) == 0 && CyberGfxBase && (c->win || c->scr);
	strict_frame(c);
	if ((c->strict & STRICT_LAG_SWAP) && !mirror && c->owed + c->swaps_in_buf < c->lag_n) {
		/* T73: the swap returns at once; the deferred draws are read up to lag_n frames later */
		tglb_put32(tglb_cmd(c, OP_SWAP, 4), 0);
		c->swaps_in_buf++;
		return;
	}
	tglb_put32(tglb_cmd(c, OP_SWAP, 4), mirror);
	len = tglb_reply_begin(c);
	if (len < 12) { tglb_reply_skip(c, len); return; }
	tglb_recv_raw(c, hdr, 12);
	len -= 12;
	need = hdr[1] * hdr[2] * 4;
	if (mirror && len >= need && need) {
		struct RastPort *rp;
		int dx, dy;
		if (c->mirror_size < need) {
			if (c->mirror_buf) FreeVec(c->mirror_buf);
			c->mirror_buf = AllocVec(need, MEMF_PUBLIC);
			c->mirror_size = c->mirror_buf ? need : 0;
		}
		if (c->mirror_buf) {
			tglb_recv_raw(c, c->mirror_buf, need);
			len -= need;
			if (c->win) { rp = c->win->RPort; dx = c->win->BorderLeft; dy = c->win->BorderTop; }
			else { rp = &c->scr->RastPort; dx = 0; dy = 0; }
			WritePixelArray(c->mirror_buf, 0, 0, hdr[1] * 4, rp, dx, dy, hdr[1], hdr[2], RECTFMT_ARGB);
		}
	}
	tglb_reply_skip(c, len);
}
