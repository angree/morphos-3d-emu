/* SPDX-License-Identifier: MIT */
/* TGLBridge guest: tinygl.library replacement for MorphOS that forwards every GL call over
 * TCP (QEMU user networking, host = 10.0.2.2) to tglhost.exe on the Windows host.
 *
 * This file: library skeleton (ROMTag, Open/Close/Expunge, function array check), the TCP
 * channel (one socket per GLContext, bsdsocket opened by the task that calls GLInit), the
 * command buffer, ENV: settings and the debug log.
 *
 * Settings (ENV: variables, read at GLInit):
 *   TGLBRIDGE_HOST   host IPv4 address           (default 10.0.2.2 = QEMU user-net gateway)
 *   TGLBRIDGE_PORT   TCP port                     (default 7654)
 *   TGLBRIDGE_MIRROR copy every Nth frame back into the guest window (0 = off, default)
 *   TGLBRIDGE_LOG    file for the guest-side log  (default none; never put it on a vvfat disk)
 */
#include "tglb_internal.h"
#include <stdarg.h>

#define LIBNAME "tinygl.library"
#define LIBVER  53
#define LIBREV  11
#define LIBID   "tinygl.library 53.11 (02.11.2026) TGLBridge - GL forwarded to the host"

/* must be the first function in the binary: running the library as a program does nothing */
int __start(void) { return -1; }

const unsigned long __abox__ = 1;

struct TGLBBase {
	struct Library lib;
	BPTR seglist;
};

struct ExecBase *SysBase;
struct DosLibrary *DOSBase;
struct Library *CyberGfxBase;
static struct TGLBBase *g_base;
int tglb_vec_patched;     /* slots patched by hand after MakeLibrary (diagnostic) */

extern const APTR tglb_vectors[];
extern const APTR tglb_expect[];
extern const unsigned int tglb_nslots;

static struct Library *LibInit(struct Library *dummy, BPTR seglist, struct ExecBase *sb);

static const char libname[] = LIBNAME;
static const char libid[] = LIBID;

const struct Resident tglb_romtag = {
	RTC_MATCHWORD,
	(struct Resident *)&tglb_romtag,
	(APTR)(&tglb_romtag + 1),
	RTF_PPC | RTF_EXTENDED,
	LIBVER,
	NT_LIBRARY,
	0,
	(char *)libname,
	(char *)libid,
	(APTR)LibInit,
	LIBREV,
	NULL
};

/* ------------------------------------------------------------------ small libc */
void tglb_memcpy(void *d, const void *s, unsigned int n)
{
	unsigned char *dd = d; const unsigned char *ss = s;
	if ((((unsigned long)dd | (unsigned long)ss | n) & 3) == 0) {
		unsigned int *dw = (unsigned int *)dd; const unsigned int *sw = (const unsigned int *)ss;
		n >>= 2;
		while (n--) *dw++ = *sw++;
		return;
	}
	while (n--) *dd++ = *ss++;
}
void tglb_memset(void *d, int v, unsigned int n)
{
	unsigned char *dd = d;
	while (n--) *dd++ = (unsigned char)v;
}
/* gcc may emit calls to these for struct copies */
void *memcpy(void *d, const void *s, size_t n) { tglb_memcpy(d, s, n); return d; }
void *memset(void *d, int v, size_t n) { tglb_memset(d, v, n); return d; }
void *memmove(void *d, const void *s, size_t n)
{
	unsigned char *dd = d; const unsigned char *ss = s;
	if (dd < ss) while (n--) *dd++ = *ss++;
	else { dd += n; ss += n; while (n--) *--dd = *--ss; }
	return d;
}
static int tglb_strlen(const char *s) { int n = 0; while (s[n]) n++; return n; }

/* ------------------------------------------------------------------ env + log */
int tglb_getenv_str(const char *name, char *buf, int size)
{
	if (!DOSBase) return -1;
	return GetVar((CONST_STRPTR)name, (STRPTR)buf, size, 0);
}
int tglb_getenv_int(const char *name, int def)
{
	char b[32]; int n, v = 0, i = 0, neg = 0;
	n = tglb_getenv_str(name, b, sizeof(b));
	if (n <= 0) return def;
	if (b[0] == '-') { neg = 1; i = 1; }
	for (; b[i] >= '0' && b[i] <= '9'; i++) v = v * 10 + (b[i] - '0');
	return neg ? -v : v;
}

static char *fmt_u(char *o, char *e, unsigned long v, int base)
{
	char t[16]; int n = 0;
	do { t[n++] = "0123456789abcdef"[v % base]; v /= base; } while (v && n < 16);
	while (n && o < e) *o++ = t[--n];
	return o;
}
static int tglb_vfmt(char *out, int size, const char *f, va_list ap)
{
	char *o = out, *e = out + size - 1;
	for (; *f && o < e; f++) {
		if (*f != '%') { *o++ = *f; continue; }
		f++;
		if (*f == 'l') f++;
		switch (*f) {
		case 'd': { long v = va_arg(ap, long); if (v < 0) { *o++ = '-'; v = -v; } o = fmt_u(o, e, (unsigned long)v, 10); break; }
		case 'u': o = fmt_u(o, e, va_arg(ap, unsigned long), 10); break;
		case 'x': o = fmt_u(o, e, va_arg(ap, unsigned long), 16); break;
		case 'p': if (o < e - 2) { *o++ = '0'; *o++ = 'x'; } o = fmt_u(o, e, (unsigned long)va_arg(ap, void *), 16); break;
		case 's': { const char *s = va_arg(ap, const char *); if (!s) s = "(null)"; while (*s && o < e) *o++ = *s++; break; }
		case '%': *o++ = '%'; break;
		default: break;
		}
	}
	*o = 0;
	return (int)(o - out);
}

int tglb_trace_on;   /* ENV:TGLBRIDGE_TRACE=1 at the first open: trace lines go to the serial port */

void tglb_trace(const char *fmt, ...)
{
	char line[256];
	va_list ap; int n, i;
	if (!tglb_trace_on) return;
	va_start(ap, fmt);
	n = tglb_vfmt(line, sizeof(line) - 1, fmt, ap);
	va_end(ap);
	RawPutChar('['); RawPutChar('T'); RawPutChar(']'); RawPutChar(' ');
	for (i = 0; i < n; i++) RawPutChar((UBYTE)line[i]);
	RawPutChar(10);
}

void tglb_log(const char *fmt, ...)
{
	char path[128], line[256];
	va_list ap; BPTR fh; int n;
	if (!DOSBase || tglb_getenv_str("TGLBRIDGE_LOG", path, sizeof(path)) <= 0) return;
	va_start(ap, fmt);
	n = tglb_vfmt(line, sizeof(line) - 1, fmt, ap);
	va_end(ap);
	line[n++] = '\n';
	fh = Open((CONST_STRPTR)path, MODE_READWRITE);
	if (!fh) return;
	Seek(fh, 0, OFFSET_END);
	Write(fh, line, n);
	Close(fh);
}

/* ------------------------------------------------------------------ library vectors */
static struct Library *LibInit(struct Library *dummy, BPTR seglist, struct ExecBase *sb)
{
	struct TGLBBase *base;
	unsigned int i;
	(void)dummy;
	SysBase = sb;
	base = (struct TGLBBase *)MakeLibrary((APTR)tglb_vectors, NULL, NULL, sizeof(struct TGLBBase), 0);
	if (!base) return NULL;
	base->lib.lib_Node.ln_Type = NT_LIBRARY;
	base->lib.lib_Node.ln_Name = (char *)libname;
	base->lib.lib_Flags = LIBF_CHANGED | LIBF_SUMUSED;
	base->lib.lib_Version = LIBVER;
	base->lib.lib_Revision = LIBREV;
	base->lib.lib_IdString = (char *)libid;
	base->seglist = seglist;
	/* The SDK inlines read the SysV function pointer at base-(LVO-2). Check what MakeLibrary
	 * built and patch any slot that does not hold our function (CODE_ILLEGAL + pointer). */
	tglb_vec_patched = 0;
	for (i = 5; i < tglb_nslots; i++) {
		unsigned char *slot = (unsigned char *)base - 6 * i;
		if (*(APTR *)(slot + 2) != tglb_expect[i - 5]) {
			*(UWORD *)slot = 0x4afc;
			*(APTR *)(slot + 2) = tglb_expect[i - 5];
			tglb_vec_patched++;
		}
	}
	if (tglb_vec_patched) CacheClearU();
	g_base = base;
	AddLibrary(&base->lib);
	return &base->lib;
}

static BPTR do_expunge(struct TGLBBase *base)
{
	BPTR seg;
	if (base->lib.lib_OpenCnt) {
		base->lib.lib_Flags |= LIBF_DELEXP;
		return 0;
	}
	Remove(&base->lib.lib_Node);
	seg = base->seglist;
	if (CyberGfxBase) { CloseLibrary(CyberGfxBase); CyberGfxBase = NULL; }
	if (DOSBase) { CloseLibrary((struct Library *)DOSBase); DOSBase = NULL; }
	FreeMem((UBYTE *)base - base->lib.lib_NegSize, base->lib.lib_NegSize + base->lib.lib_PosSize);
	return seg;
}

APTR tglb_LibOpen(void)
{
	struct TGLBBase *base = (struct TGLBBase *)REG_A6;
	base->lib.lib_OpenCnt++;
	base->lib.lib_Flags &= ~LIBF_DELEXP;
	if (!DOSBase) DOSBase = (struct DosLibrary *)OpenLibrary((CONST_STRPTR)"dos.library", 36);
	if (!CyberGfxBase) CyberGfxBase = OpenLibrary((CONST_STRPTR)"cybergraphics.library", 40);
	tglb_trace_on = tglb_getenv_int("TGLBRIDGE_TRACE", 0);
	tglb_trace("LibOpen cnt %ld dos %p cgx %p", (long)base->lib.lib_OpenCnt, DOSBase, CyberGfxBase);
	return base;
}

APTR tglb_LibClose(void)
{
	struct TGLBBase *base = (struct TGLBBase *)REG_A6;
	if (base->lib.lib_OpenCnt) base->lib.lib_OpenCnt--;
	if (!base->lib.lib_OpenCnt && (base->lib.lib_Flags & LIBF_DELEXP))
		return (APTR)do_expunge(base);
	return NULL;
}

APTR tglb_LibExpunge(void)
{
	struct TGLBBase *base = (struct TGLBBase *)REG_A6;
	return (APTR)do_expunge(base);
}

APTR tglb_LibNull(void) { return NULL; }

/* ------------------------------------------------------------------ channel */
void tglb_send_raw(GLContext *c, const void *p, unsigned int n)
{
	struct Library *SocketBase = c->SocketBase;
	const char *b = p;
	if (c->dead) return;
	tglb_trace("send %lu bytes sock %ld", (unsigned long)n, c->sock);
	while (n) {
		long r = send(c->sock, (APTR)b, n, 0);
		if (r <= 0) { c->dead = 1; tglb_log("tglbridge: send failed (%ld), connection dead", r); return; }
		b += r; n -= r;
	}
}

int tglb_recv_raw(GLContext *c, void *p, unsigned int n)
{
	struct Library *SocketBase = c->SocketBase;
	unsigned char *b = p;
	if (c->dead) { tglb_memset(p, 0, n); return -1; }
	tglb_trace("recv %lu bytes", (unsigned long)n);
	while (n) {
		long r = recv(c->sock, b, n, 0);
		if (r <= 0) { c->dead = 1; tglb_log("tglbridge: recv failed (%ld), connection dead", r); tglb_memset(b, 0, n); return -1; }
		b += r; n -= r;
	}
	return 0;
}

void tglb_flush(GLContext *c)
{
	if (c->used) tglb_send_raw(c, c->buf, c->used);
	c->used = 0;
}

unsigned char *tglb_reserve(GLContext *c, unsigned int n)
{
	unsigned char *p;
	if (c->used + n > TGLB_BUFSZ) tglb_flush(c);
	p = c->buf + c->used;
	c->used += n;
	return p;
}

/* header + the first `payload` bytes reserved (payload must fit the buffer) */
unsigned char *tglb_cmd(GLContext *c, unsigned int op, unsigned int payload)
{
	unsigned int pad = (payload + 3) & ~3u;
	unsigned char *p = tglb_reserve(c, 8 + pad);
	((unsigned int *)p)[0] = op;
	((unsigned int *)p)[1] = pad;
	if (pad != payload) *(unsigned int *)(p + 8 + pad - 4) = 0;
	return p + 8;
}

void tglb_append(GLContext *c, const void *src, unsigned int n)
{
	const unsigned char *s = src;
	while (n) {
		unsigned int room = TGLB_BUFSZ - c->used, k;
		if (!room) { tglb_flush(c); room = TGLB_BUFSZ; }
		k = n < room ? n : room;
		tglb_memcpy(c->buf + c->used, s, k);
		c->used += k; s += k; n -= k;
	}
}

void tglb_pad(GLContext *c, unsigned int n)
{
	static const unsigned char z[4] = {0, 0, 0, 0};
	if (n & 3) tglb_append(c, z, 4 - (n & 3));
}

/* flush, then read the reply length */
unsigned int tglb_reply_begin(GLContext *c)
{
	unsigned int len = 0;
	tglb_flush(c);
	if (tglb_recv_raw(c, &len, 4) < 0) return 0;
	return len;
}

void tglb_reply_skip(GLContext *c, unsigned int n)
{
	unsigned char t[64];
	while (n) {
		unsigned int k = n < sizeof(t) ? n : sizeof(t);
		tglb_recv_raw(c, t, k);
		n -= k;
	}
}

unsigned int tglb_sync_u32(GLContext *c)
{
	unsigned int len = tglb_reply_begin(c), v = 0;
	if (len >= 4) { tglb_recv_raw(c, &v, 4); len -= 4; }
	tglb_reply_skip(c, len);
	return v;
}

void tglb_unimpl(GLContext *c, int slot)
{
	if (!c || c->magic != TGLB_MAGIC) return;
	if (c->unimpl_sent[slot >> 3] & (1 << (slot & 7))) return;
	c->unimpl_sent[slot >> 3] |= 1 << (slot & 7);
	tglb_log("tglbridge: unimplemented slot %ld called", (long)slot);
	tglb_put32(tglb_cmd(c, OP_UNIMPL, 4), (unsigned int)slot);
}

void tglb_note(GLContext *c, const char *s)
{
	unsigned int n = tglb_strlen(s);
	unsigned char *p = tglb_cmd(c, OP_NOTE, 4);
	/* rewrite the header length: 4 + n padded */
	((unsigned int *)(p - 8))[1] = 4 + ((n + 3) & ~3u);
	tglb_put32(p, n);
	tglb_append(c, s, n);
	tglb_pad(c, n);
}

/* number of values behind a pname for the gl*v setters */
int tglb_pname_count(unsigned int pname)
{
	switch (pname) {
	case 0x0B66: /* GL_FOG_COLOR */
	case 0x1200: case 0x1201: case 0x1202: case 0x1203: /* AMBIENT DIFFUSE SPECULAR POSITION */
	case 0x1600: case 0x1602: /* EMISSION AMBIENT_AND_DIFFUSE */
	case 0x0B53: /* LIGHT_MODEL_AMBIENT */
	case 0x2201: /* TEXTURE_ENV_COLOR */
	case 0x1004: /* TEXTURE_BORDER_COLOR */
	case 0x2501: case 0x2502: /* OBJECT_PLANE EYE_PLANE */
		return 4;
	case 0x1204: /* SPOT_DIRECTION */
	case 0x1603: /* COLOR_INDEXES */
	case 0x8129: /* POINT_DISTANCE_ATTENUATION */
		return 3;
	default:
		return 1;
	}
}

/* ------------------------------------------------------------------ connect */
static unsigned long parse_ip(const char *s)
{
	unsigned long ip = 0; int part = 0, k;
	for (k = 0; k < 4; k++) {
		part = 0;
		while (*s >= '0' && *s <= '9') part = part * 10 + (*s++ - '0');
		ip = (ip << 8) | (part & 255);
		if (*s == '.') s++;
	}
	return ip;
}

GLContext *tglb_connect(void)
{
	GLContext *c;
	struct Library *SocketBase;
	struct sockaddr_in sa;
	char host[64];
	int port, one = 1, bufsz = 512 * 1024;
	unsigned char *p;
	struct Task *me = FindTask(NULL);

	c = AllocVec(sizeof(*c), MEMF_PUBLIC | MEMF_CLEAR);
	if (!c) return NULL;
	c->magic = TGLB_MAGIC;
	c->owner = me;
	c->buf = AllocVec(TGLB_BUFSZ, MEMF_PUBLIC);
	if (!c->buf) { FreeVec(c); return NULL; }
	c->unpack_align = c->pack_align = 4;
	c->mirror_every = tglb_getenv_int("TGLBRIDGE_MIRROR", 0);

	c->SocketBase = SocketBase = OpenLibrary((CONST_STRPTR)"bsdsocket.library", 4);
	if (!SocketBase) {
		tglb_log("tglbridge: no bsdsocket.library (network not up?)");
		FreeVec(c->buf); FreeVec(c); return NULL;
	}
	if (tglb_getenv_str("TGLBRIDGE_HOST", host, sizeof(host)) <= 0) {
		host[0] = '1'; host[1] = '0'; host[2] = '.'; host[3] = '0'; host[4] = '.';
		host[5] = '2'; host[6] = '.'; host[7] = '2'; host[8] = 0;
	}
	port = tglb_getenv_int("TGLBRIDGE_PORT", TGLB_DEFAULT_PORT);
	c->sock = socket(AF_INET, SOCK_STREAM, 0);
	if (c->sock < 0) {
		tglb_log("tglbridge: socket() failed errno %ld", Errno());
		CloseLibrary(SocketBase); FreeVec(c->buf); FreeVec(c); return NULL;
	}
	tglb_trace("connect: socket %ld, host %s port %ld", c->sock, host, (long)port);
	setsockopt(c->sock, IPPROTO_TCP, 1 /* TCP_NODELAY */, &one, sizeof(one));
	(void)bufsz;
	tglb_memset(&sa, 0, sizeof(sa));
	sa.sin_len = sizeof(sa);
	sa.sin_family = AF_INET;
	sa.sin_port = port;               /* big-endian host: htons is the identity */
	sa.sin_addr.s_addr = parse_ip(host);
	if (connect(c->sock, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
		tglb_log("tglbridge: connect %s:%ld failed errno %ld", host, (long)port, Errno());
		CloseSocket(c->sock); CloseLibrary(SocketBase); FreeVec(c->buf); FreeVec(c); return NULL;
	}
	tglb_trace("connected, buf %p", c->buf);
	/* HELLO: magic, version, task name */
	p = tglb_cmd(c, OP_HELLO, 8 + 32);
	p = tglb_put32(p, TGLB_MAGIC);
	p = tglb_put32(p, TGLB_PROTO_VER);
	{
		const char *n = me->tc_Node.ln_Name ? me->tc_Node.ln_Name : "?";
		int i;
		for (i = 0; i < 31 && n[i]; i++) p[i] = n[i];
		for (; i < 32; i++) p[i] = 0;
	}
	tglb_trace("HELLO queued, used %lu", (unsigned long)c->used);
	if (tglb_sync_u32(c) != 1) {
		tglb_log("tglbridge: host refused HELLO");
		CloseSocket(c->sock); CloseLibrary(SocketBase); FreeVec(c->buf); FreeVec(c); return NULL;
	}
	tglb_log("tglbridge: connected to %s:%ld (vector slots patched: %ld)", host, (long)port, (long)tglb_vec_patched);
	if (tglb_vec_patched) {
		char m[80]; int k = 0; const char *t = "guest: jump table slots patched after MakeLibrary";
		while (t[k] && k < 79) { m[k] = t[k]; k++; } m[k] = 0;
		tglb_note(c, m);
	}
	return c;
}

void tglb_disconnect(GLContext *c)
{
	struct Library *SocketBase = c->SocketBase;
	int i;
	if (!c->dead) {
		tglb_cmd(c, OP_BYE, 0);
		tglb_flush(c);
	}
	CloseSocket(c->sock);
	CloseLibrary(SocketBase);
	for (i = 0; i < TGLB_NSTR; i++) if (c->str_val[i]) FreeVec(c->str_val[i]);
	if (c->mirror_buf) FreeVec(c->mirror_buf);
	c->magic = 0;
	FreeVec(c->buf);
	FreeVec(c);
}
