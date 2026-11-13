/* SPDX-License-Identifier: MIT */
/* tglhost - TGLBridge host renderer for Windows.
 *
 * Listens on 127.0.0.1:<port> (QEMU user networking maps the guest's 10.0.2.2 there), accepts
 * one TCP connection per guest GLContext, and executes the guest's TinyGL command stream with the
 * real OpenGL driver. Every guest context renders into an FBO of the guest window's size; on
 * swap the FBO is blitted to a host window (shown without activation, or not at all with
 * --hidden) and can be copied back to the guest (mirror) or saved as a BMP (--shot).
 *
 *   tglhost [--port 7654] [--hidden] [--shot 60,120] [--shotdir dir] [--log tglhost.log]
 *           [--exit-after N]   (exit after N guest connections have closed)
 *           [--shot-every N] [--shot-from S]   (save every Nth frame of every client from swap S: frame-by-frame checks)
 * Strict mode (guest ENV:TGLBRIDGE_STRICT, sent with OP_STRICT): STRICT_LIMITS is enforced here (max texture size,
 * texture memory budget, index statistics); everything else is done by the guest library.
 *           [--glcheck]        (debug: glGetError after every command, first error per opcode logged;
 *                               this swallows errors the guest would otherwise see)
 */
#include "tglhost.h"
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "opengl32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")

#define GL_FRAMEBUFFER 0x8D40
#define GL_READ_FRAMEBUFFER 0x8CA8
#define GL_DRAW_FRAMEBUFFER 0x8CA9
#define GL_RENDERBUFFER 0x8D41
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_DEPTH_STENCIL_ATTACHMENT 0x821A
#define GL_DEPTH24_STENCIL8 0x88F0
#define GL_RGBA8_ 0x8058
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#define GL_BGRA_ 0x80E1
#define GL_UNSIGNED_INT_8_8_8_8_ 0x8035
#define GL_TEXTURE0_ 0x84C0

typedef void (APIENTRY *PFNGEN)(GLsizei, GLuint *);
typedef void (APIENTRY *PFNBIND)(GLenum, GLuint);
typedef void (APIENTRY *PFNFBRB)(GLenum, GLenum, GLenum, GLuint);
typedef void (APIENTRY *PFNRBST)(GLenum, GLenum, GLsizei, GLsizei);
typedef void (APIENTRY *PFNBLIT)(GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum);
typedef GLenum (APIENTRY *PFNCHK)(GLenum);
typedef void (APIENTRY *PFNDEL)(GLsizei, const GLuint *);
typedef BOOL (APIENTRY *PFNSWAPINT)(int);
typedef void (APIENTRY *PFNCAT)(GLenum);

static PFNGEN p_glGenFramebuffers, p_glGenRenderbuffers;
static PFNBIND p_glBindFramebuffer, p_glBindRenderbuffer;
static PFNFBRB p_glFramebufferRenderbuffer;
static PFNRBST p_glRenderbufferStorage;
static PFNBLIT p_glBlitFramebuffer;
static PFNCHK p_glCheckFramebufferStatus;
static PFNDEL p_glDeleteFramebuffers, p_glDeleteRenderbuffers;
static PFNSWAPINT p_wglSwapIntervalEXT;
static PFNCAT p_glClientActiveTexture;

static FILE *g_log;
static int g_glcheck;
static int g_hidden, g_port = TGLB_DEFAULT_PORT, g_exit_after = -1, g_closed;
static int g_shots[64], g_nshots, g_shot_every, g_shot_from;   /* T73: --shot-every N [--shot-from S] */
static char g_shotdir[MAX_PATH] = "shots";
static HMODULE g_gl32, g_glu32;
static void *g_fp[TGLB_NSLOTS];
static void *lookup_name(const char *tgl);
static unsigned char g_logged[TGLB_NSLOTS];   /* 1 = unimplemented logged, 2 = host-missing logged */
static char g_host_renderer[256], g_host_version[256], g_host_vendor[256];
static Client *g_cur;

#define MAXCL 8
static Client *g_cl[MAXCL];
static int g_next_id = 1;

/* ------------------------------------------------------------------ log */
static double now_s(void)
{
	static LARGE_INTEGER f;
	LARGE_INTEGER c;
	if (!f.QuadPart) QueryPerformanceFrequency(&f);
	QueryPerformanceCounter(&c);
	return (double)c.QuadPart / (double)f.QuadPart;
}

void hlog(const char *fmt, ...)
{
	char b[1024];
	va_list ap;
	SYSTEMTIME st;
	GetLocalTime(&st);
	va_start(ap, fmt);
	vsnprintf(b, sizeof(b), fmt, ap);
	va_end(ap);
	printf("[%02d:%02d:%02d.%03d] %s\n", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, b);
	fflush(stdout);
	if (g_log) {
		fprintf(g_log, "[%02d:%02d:%02d.%03d] %s\n", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, b);
		fflush(g_log);
	}
}

/* ------------------------------------------------------------------ GL function lookup */
static void *wglproc(const char *n)
{
	void *p = (void *)wglGetProcAddress(n);
	if (p == (void *)0 || p == (void *)1 || p == (void *)2 || p == (void *)3 || p == (void *)-1) p = NULL;
	if (!p && g_gl32) p = (void *)GetProcAddress(g_gl32, n);
	if (!p && g_glu32) p = (void *)GetProcAddress(g_glu32, n);
	return p;
}

static void *lookup_name(const char *tgl)
{
	char n[128], t[140];
	size_t L;
	void *p;
	/* GLA* context calls and TGL* are handled by hand; glAlphaFunc, glActiveTexture... also start with "GLA" */
	if (!strncmp(tgl, "GLUT", 4) || !strncmp(tgl, "TGL", 3)) return NULL;
	if (!strncmp(tgl, "GLU", 3) && tgl[3] >= 'A' && tgl[3] <= 'Z') snprintf(n, sizeof(n), "glu%s", tgl + 3);
	else if (!strncmp(tgl, "GL", 2)) snprintf(n, sizeof(n), "gl%s", tgl + 2);
	else return NULL;
	if ((p = wglproc(n))) return p;
	snprintf(t, sizeof(t), "%sARB", n); if ((p = wglproc(t))) return p;
	snprintf(t, sizeof(t), "%sEXT", n); if ((p = wglproc(t))) return p;
	L = strlen(n);
	if (L > 3 && (!strcmp(n + L - 3, "ARB") || !strcmp(n + L - 3, "EXT"))) {
		snprintf(t, sizeof(t), "%.*s", (int)(L - 3), n);
		if ((p = wglproc(t))) return p;
	}
	return NULL;
}

void *resolve(Client *cl, int slot)
{
	(void)cl;
	if (slot < 0 || slot >= TGLB_NSLOTS) return NULL;
	if (!g_fp[slot]) {
		void *p = lookup_name(g_slot_name[slot]);
		g_fp[slot] = p ? p : (void *)1;
	}
	if (g_fp[slot] == (void *)1) {
		if (!(g_logged[slot] & 2)) {
			g_logged[slot] |= 2;
			hlog("NOT AVAILABLE ON HOST (logged once): %s - call dropped", g_slot_name[slot]);
		}
		return NULL;
	}
	return g_fp[slot];
}

/* ------------------------------------------------------------------ socket I/O */
static int send_all(Client *cl, const void *p, size_t n)
{
	const char *b = (const char *)p;
	while (n) {
		int r = send(cl->s, b, (int)(n > 1 << 30 ? 1 << 30 : n), 0);
		if (r <= 0) { cl->dead = 1; return -1; }
		b += r; n -= r;
	}
	return 0;
}

static void reply(Client *cl, const void *payload, unsigned int n)
{
	unsigned char h[4];
	h[0] = (unsigned char)(n >> 24); h[1] = (unsigned char)(n >> 16); h[2] = (unsigned char)(n >> 8); h[3] = (unsigned char)n;
	send_all(cl, h, 4);
	if (n) send_all(cl, payload, n);
}

static void put_be32(unsigned char *p, uint32_t v)
{
	p[0] = (unsigned char)(v >> 24); p[1] = (unsigned char)(v >> 16); p[2] = (unsigned char)(v >> 8); p[3] = (unsigned char)v;
}

void reply_u32(Client *cl, unsigned int v)
{
	unsigned char b[4];
	put_be32(b, v);
	reply(cl, b, 4);
}

/* ------------------------------------------------------------------ window + context */
static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l)
{
	if (m == WM_CLOSE) return 0;          /* the guest owns the lifetime */
	if (m == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
	return DefWindowProcA(h, m, w, l);
}

static void make_current(Client *cl)
{
	if (g_cur != cl) {
		wglMakeCurrent(cl->hdc, cl->rc);
		g_cur = cl;
	}
}

static void load_ext(void)
{
	static int done;
	if (done) return;
	done = 1;
	p_glGenFramebuffers = (PFNGEN)wglproc("glGenFramebuffers");
	p_glGenRenderbuffers = (PFNGEN)wglproc("glGenRenderbuffers");
	p_glBindFramebuffer = (PFNBIND)wglproc("glBindFramebuffer");
	p_glBindRenderbuffer = (PFNBIND)wglproc("glBindRenderbuffer");
	p_glFramebufferRenderbuffer = (PFNFBRB)wglproc("glFramebufferRenderbuffer");
	p_glRenderbufferStorage = (PFNRBST)wglproc("glRenderbufferStorage");
	p_glBlitFramebuffer = (PFNBLIT)wglproc("glBlitFramebuffer");
	p_glCheckFramebufferStatus = (PFNCHK)wglproc("glCheckFramebufferStatus");
	p_glDeleteFramebuffers = (PFNDEL)wglproc("glDeleteFramebuffers");
	p_glDeleteRenderbuffers = (PFNDEL)wglproc("glDeleteRenderbuffers");
	p_wglSwapIntervalEXT = (PFNSWAPINT)wglproc("wglSwapIntervalEXT");
	p_glClientActiveTexture = (PFNCAT)wglproc("glClientActiveTexture");
	snprintf(g_host_vendor, sizeof(g_host_vendor), "%s", (const char *)glGetString(GL_VENDOR));
	snprintf(g_host_renderer, sizeof(g_host_renderer), "%s", (const char *)glGetString(GL_RENDERER));
	snprintf(g_host_version, sizeof(g_host_version), "%s", (const char *)glGetString(GL_VERSION));
	hlog("host GL: %s | %s | %s", g_host_vendor, g_host_renderer, g_host_version);
	{
		/* resolve every forwarded entry now: wglGetProcAddress inside glBegin/glEnd raises
		 * GL_INVALID_OPERATION on NVIDIA (measured), so never look up lazily */
		int i, ok = 0, miss = 0;
		for (i = 0; i < TGLB_NSLOTS; i++) {
			char k = g_slot_class[i];
			if (k != 'A' && k != 'V' && k != 'P') continue;
			g_fp[i] = lookup_name(g_slot_name[i]);
			if (g_fp[i]) ok++; else { g_fp[i] = (void *)1; miss++; }
		}
		hlog("forwarded entry points resolved: %d, missing on host: %d (logged when called)", ok, miss);
	}
	if (!p_glGenFramebuffers || !p_glBlitFramebuffer) hlog("FATAL: host GL has no framebuffer objects");
}

static void fbo_setup(Client *cl, int w, int h)
{
	GLenum st;
	if (w < 1) w = 1;
	if (h < 1) h = 1;
	if (!cl->fbo) {
		p_glGenFramebuffers(1, &cl->fbo);
		p_glGenRenderbuffers(1, &cl->rb_color);
		p_glGenRenderbuffers(1, &cl->rb_depth);
	}
	p_glBindRenderbuffer(GL_RENDERBUFFER, cl->rb_color);
	p_glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8_, w, h);
	p_glBindRenderbuffer(GL_RENDERBUFFER, cl->rb_depth);
	p_glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, w, h);
	p_glBindRenderbuffer(GL_RENDERBUFFER, 0);
	p_glBindFramebuffer(GL_FRAMEBUFFER, cl->fbo);
	p_glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, cl->rb_color);
	p_glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, cl->rb_depth);
	st = p_glCheckFramebufferStatus(GL_FRAMEBUFFER);
	if (st != GL_FRAMEBUFFER_COMPLETE) hlog("client %d: FBO incomplete 0x%x", cl->id, st);
	glDrawBuffer(GL_COLOR_ATTACHMENT0);
	glReadBuffer(GL_COLOR_ATTACHMENT0);
	cl->w = w; cl->h = h;
}

static int ctx_create(Client *cl)
{
	static int registered;
	PIXELFORMATDESCRIPTOR pfd;
	int pf;
	if (!registered) {
		WNDCLASSA wc;
		memset(&wc, 0, sizeof(wc));
		wc.style = CS_OWNDC;
		wc.lpfnWndProc = wndproc;
		wc.hInstance = GetModuleHandleA(NULL);
		wc.hCursor = LoadCursor(NULL, IDC_ARROW);
		wc.lpszClassName = "TGLBridgeHost";
		RegisterClassA(&wc);
		registered = 1;
	}
	cl->hwnd = CreateWindowExA(WS_EX_NOACTIVATE | WS_EX_APPWINDOW, "TGLBridgeHost", "TGLBridge",
	                           WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
	                           CW_USEDEFAULT, CW_USEDEFAULT, 200, 200, NULL, NULL, GetModuleHandleA(NULL), NULL);
	if (!cl->hwnd) { hlog("CreateWindow failed %lu", GetLastError()); return 0; }
	cl->hdc = GetDC(cl->hwnd);
	memset(&pfd, 0, sizeof(pfd));
	pfd.nSize = sizeof(pfd);
	pfd.nVersion = 1;
	pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
	pfd.iPixelType = PFD_TYPE_RGBA;
	pfd.cColorBits = 32;
	pfd.cDepthBits = 24;
	pfd.cStencilBits = 8;
	pf = ChoosePixelFormat(cl->hdc, &pfd);
	if (!pf || !SetPixelFormat(cl->hdc, pf, &pfd)) { hlog("pixel format failed"); return 0; }
	cl->rc = wglCreateContext(cl->hdc);
	if (!cl->rc) { hlog("wglCreateContext failed"); return 0; }
	g_cur = NULL;
	make_current(cl);
	load_ext();
	if (!p_glGenFramebuffers) return 0;
	fbo_setup(cl, 64, 64);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	glPixelStorei(GL_PACK_ALIGNMENT, 1);
	if (p_wglSwapIntervalEXT) p_wglSwapIntervalEXT(0);
	cl->t_last = now_s();
	return 1;
}

static void window_size(Client *cl, int w, int h, int kind)
{
	RECT r = { 0, 0, w, h };
	char title[160];
	DWORD style = (DWORD)GetWindowLongA(cl->hwnd, GWL_STYLE);
	AdjustWindowRectEx(&r, style, FALSE, (DWORD)GetWindowLongA(cl->hwnd, GWL_EXSTYLE));
	SetWindowPos(cl->hwnd, NULL, 0, 0, r.right - r.left, r.bottom - r.top,
	             SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
	snprintf(title, sizeof(title), "TGLBridge #%d %s %dx%d (%s)", cl->id, cl->name, w, h, kind == 2 ? "screen" : "window");
	SetWindowTextA(cl->hwnd, title);
	if (!g_hidden && !cl->shown) {
		ShowWindow(cl->hwnd, SW_SHOWNOACTIVATE);
		cl->shown = 1;
	}
}

static void client_free(Client *cl)
{
	int i;
	if (cl->rc) {
		if (g_cur == cl) { wglMakeCurrent(NULL, NULL); g_cur = NULL; }
		wglDeleteContext(cl->rc);
	}
	if (cl->hdc) ReleaseDC(cl->hwnd, cl->hdc);
	if (cl->hwnd) DestroyWindow(cl->hwnd);
	closesocket(cl->s);
	for (i = 0; i < ARR_COUNT; i++) free(cl->abuf[i]);
	free(cl->in);
	free(cl->texlv);
	free(cl);
}

/* ------------------------------------------------------------------ BMP shot */
static void save_bmp(Client *cl, const char *path)
{
	int w = cl->w, h = cl->h, y, x, rowb = (w * 3 + 3) & ~3;
	unsigned char *px = (unsigned char *)malloc((size_t)w * h * 4), *row = (unsigned char *)malloc(rowb);
	FILE *f;
	BITMAPFILEHEADER fh;
	BITMAPINFOHEADER ih;
	if (!px || !row) { free(px); free(row); return; }
	glReadPixels(0, 0, w, h, GL_BGRA_, GL_UNSIGNED_BYTE, px);
	f = fopen(path, "wb");
	if (!f) { hlog("cannot write %s", path); free(px); free(row); return; }
	memset(&fh, 0, sizeof(fh)); memset(&ih, 0, sizeof(ih));
	fh.bfType = 0x4D42;
	fh.bfOffBits = sizeof(fh) + sizeof(ih);
	fh.bfSize = fh.bfOffBits + rowb * h;
	ih.biSize = sizeof(ih); ih.biWidth = w; ih.biHeight = h; ih.biPlanes = 1; ih.biBitCount = 24;
	fwrite(&fh, sizeof(fh), 1, f); fwrite(&ih, sizeof(ih), 1, f);
	for (y = 0; y < h; y++) {             /* GL rows are bottom-up, like BMP */
		memset(row, 0, rowb);
		for (x = 0; x < w; x++) memcpy(row + x * 3, px + ((size_t)y * w + x) * 4, 3);
		fwrite(row, rowb, 1, f);
	}
	fclose(f);
	free(px); free(row);
	hlog("client %d: saved %s (%dx%d, swap %u)", cl->id, path, w, h, cl->swaps);
}

/* ------------------------------------------------------------------ manual slots */
static int get_count(GLenum pname)
{
	switch (pname) {
	case 0x0BA2: case 0x0C10: case 0x0C22: case 0x0C23: case 0x0B00: case 0x0B03: case 0x0B66:
	case 0x0B53: case 0x8005: case 0x0B07: case 0x0B80: case 0x0B04: case 0x0B05:
		return 4;   /* VIEWPORT SCISSOR_BOX COLOR_CLEAR_VALUE COLOR_WRITEMASK CURRENT_COLOR CURRENT_TEXCOORDS FOG_COLOR LIGHT_MODEL_AMBIENT BLEND_COLOR RASTER_POS ACCUM_CLEAR CURRENT_INDEX? */
	case 0x0B02: return 3;                                   /* CURRENT_NORMAL */
	case 0x0B70: case 0x0D3A: case 0x846D: case 0x846E: case 0x0B12: case 0x0B22: case 0x0B40:
		return 2;   /* DEPTH_RANGE MAX_VIEWPORT_DIMS ALIASED_POINT/LINE_RANGE POINT/LINE_WIDTH_RANGE POLYGON_MODE */
	case 0x0BA6: case 0x0BA7: case 0x0BA8: case 0x84E3: case 0x84E4: case 0x84E5: case 0x84E6:
		return 16;  /* MODELVIEW/PROJECTION/TEXTURE_MATRIX + TRANSPOSE_* */
	}
	return 1;
}

static int pix_esz(GLenum type)
{
	switch (type) {
	case 0x1402: case 0x1403: case 0x8363: case 0x8364: case 0x8033: case 0x8365: case 0x8034: case 0x8366: return 2;
	case 0x1404: case 0x1405: case 0x1406: case 0x8035: case 0x8367: case 0x8036: case 0x8368: return 4;
	}
	return 1;
}

static int type_size(GLenum t)
{
	switch (t) {
	case 0x1400: case 0x1401: return 1;
	case 0x1402: case 0x1403: return 2;
	case 0x1404: case 0x1405: case 0x1406: return 4;
	case 0x140A: return 8;
	}
	return 0;
}

static void log_unimpl(int slot, const char *why)
{
	if (slot < 0 || slot >= TGLB_NSLOTS) { hlog("bad slot %d", slot); return; }
	if (g_logged[slot] & 1) return;
	g_logged[slot] |= 1;
	hlog("UNIMPLEMENTED (logged once): %s - %s", g_slot_name[slot], why);
}

static void set_array_pointer(Client *cl, int id, int size, GLenum type, const void *base)
{
	if (id == ARR_VERTEX) glVertexPointer(size, type, 0, base);
	else if (id == ARR_COLOR) glColorPointer(size, type, 0, base);
	else if (id == ARR_NORMAL) glNormalPointer(type, 0, base);
	else if (id >= ARR_TEX0 && id < ARR_TEX0 + 8) {
		if (p_glClientActiveTexture) p_glClientActiveTexture(GL_TEXTURE0_ + (id - ARR_TEX0));
		glTexCoordPointer(size, type, 0, base);
		if (p_glClientActiveTexture) p_glClientActiveTexture(GL_TEXTURE0_ + cl->client_unit);
	}
}

static const char *ext_string(void)
{
	return "GL_ARB_multitexture GL_EXT_compiled_vertex_array GL_EXT_packed_pixels GL_EXT_bgra "
	       "GL_EXT_texture_edge_clamp GL_SGIS_texture_edge_clamp GL_ARB_texture_env_add GL_EXT_texture_env_add "
	       "GL_ARB_texture_env_combine GL_EXT_texture_env_combine GL_ARB_texture_env_dot3 GL_EXT_blend_color "
	       "GL_EXT_blend_minmax GL_EXT_blend_subtract GL_EXT_draw_range_elements GL_ARB_texture_mirrored_repeat "
	       "GL_EXT_texture_lod_bias GL_EXT_stencil_wrap GL_ARB_texture_border_clamp GL_SGIS_generate_mipmap "
	       "GL_EXT_texture_filter_anisotropic";
}

/* ------------------------------------------------------------------ T73 strict: real-TinyGL limits */
/* bytes per texel as the card would store it (16-bit formats stay 16-bit, everything else 32-bit) */
static unsigned int tex_bpp(GLint ifmt, GLenum type)
{
	switch (type) {
	case 0x8363: case 0x8364: case 0x8033: case 0x8365: case 0x8034: case 0x8366: return 2;
	}
	switch (ifmt) {
	case 0x8050: case 0x8D62: case 0x8056: case 0x8057: case 2: case 0x190A: return 2;   /* RGB5 RGB565 RGBA4 RGB5_A1 LA */
	case 1: case 0x1906: case 0x1909: case 0x803C: case 0x8040: return 1;               /* L A ALPHA8 LUMINANCE8 */
	}
	return 4;
}

static unsigned int *tex_slot(Client *cl, GLuint id, int level)
{
	if (level < 0 || level > 15 || id == 0 || id > (1u << 20)) return NULL;
	if (id >= cl->ntexlv) {
		unsigned int n = cl->ntexlv ? cl->ntexlv : 1024;
		void *q;
		while (n <= id) n *= 2;
		q = realloc(cl->texlv, (size_t)n * sizeof(*cl->texlv));
		if (!q) return NULL;
		memset((unsigned char *)q + (size_t)cl->ntexlv * sizeof(*cl->texlv), 0, (size_t)(n - cl->ntexlv) * sizeof(*cl->texlv));
		cl->texlv = (unsigned int (*)[16])q;
		cl->ntexlv = n;
	}
	return &cl->texlv[id][level];
}

/* returns 1 if the upload must fail (STRICT_LIMITS): too big, or over the texture memory budget */
static int strict_tex_refuse(Client *cl, uint32_t target, int level, GLint ifmt, int w, int h, GLenum type)
{
	GLint bound = 0;
	unsigned int *slot, need;
	if (!(cl->strict & STRICT_LIMITS) || target != GL_TEXTURE_2D) return 0;
	if ((unsigned int)w > cl->max_tex || (unsigned int)h > cl->max_tex) {
		if (cl->tex_refused_size++ < 30)
			hlog("client %d: strict: glTexImage2D %dx%d level %d is over the max texture size %u - upload fails (GL_INVALID_VALUE)",
			     cl->id, w, h, level, cl->max_tex);
		return 1;
	}
	glGetIntegerv(0x8069 /* TEXTURE_BINDING_2D */, &bound);
	slot = tex_slot(cl, (GLuint)bound, level);
	if (!slot) return 0;
	need = (unsigned int)w * (unsigned int)h * tex_bpp(ifmt, type);
	if (cl->tex_budget_kb && cl->tex_bytes - *slot + need > (unsigned long long)cl->tex_budget_kb * 1024u) {
		if (cl->tex_refused_mem++ < 30)
			hlog("client %d: strict: glTexImage2D tex %d %dx%d level %d (%u KB) does not fit the texture budget (%llu of %u KB used) - upload fails",
			     cl->id, bound, w, h, level, need >> 10, cl->tex_bytes >> 10, cl->tex_budget_kb);
		return 1;
	}
	cl->tex_bytes = cl->tex_bytes - *slot + need;
	*slot = need;
	if (cl->tex_bytes > cl->tex_peak) cl->tex_peak = cl->tex_bytes;
	return 0;
}

static void strict_tex_delete(Client *cl, GLuint id)
{
	int l;
	if (!cl->texlv || id == 0 || id >= cl->ntexlv) return;
	for (l = 0; l < 16; l++) { cl->tex_bytes -= cl->texlv[id][l]; cl->texlv[id][l] = 0; }
}

static void strict_idx(Client *cl, GLsizei count, GLenum type)
{
	if ((unsigned int)count > cl->max_idx_count) cl->max_idx_count = (unsigned int)count;
	if (type != GL_UNSIGNED_SHORT && !(cl->logged_idx & 1)) {
		cl->logged_idx |= 1;
		hlog("client %d: strict: glDrawElements with index type 0x%x (not GL_UNSIGNED_SHORT) - untested on real TinyGL", cl->id, type);
	}
	if (count > 65535 && !(cl->logged_idx & 2)) {
		cl->logged_idx |= 2;
		hlog("client %d: strict: glDrawElements with %d indices in one call (> 65535)", cl->id, (int)count);
	}
}

/* returns 1 if handled */
static int manual(Client *cl, int slot, unsigned char *p0, unsigned int len)
{
	const unsigned char *p = p0;
	switch (slot) {
	case SLOT_GLFlush: glFlush(); return 1;
	case SLOT_GLFinish: glFinish(); reply_u32(cl, 0); return 1;
	case SLOT_GLGetString: {
		GLenum n = rd_u32(&p);
		char s[512];
		if (n == GL_VENDOR) snprintf(s, sizeof(s), "TGLBridge (host: %s)", g_host_vendor);
		else if (n == GL_RENDERER) snprintf(s, sizeof(s), "TGLBridge -> %s", g_host_renderer);
		else if (n == GL_VERSION) snprintf(s, sizeof(s), "1.5 TGLBridge (host %s)", g_host_version);
		else if (n == GL_EXTENSIONS) snprintf(s, sizeof(s), "%s", ext_string());
		else { const char *h = (const char *)glGetString(n); snprintf(s, sizeof(s), "%s", h ? h : ""); }
		reply(cl, s, (unsigned int)strlen(s));
		return 1;
	}
	case SLOT_GLGetIntegerv: case SLOT_GLGetFloatv: case SLOT_GLGetBooleanv: case SLOT_GLGetDoublev: {
		GLenum pn = rd_u32(&p);
		int n = get_count(pn), i, esz = slot == SLOT_GLGetDoublev ? 8 : 4;
		unsigned char out[4 + 16 * 8];
		put_be32(out, n);
		if (slot == SLOT_GLGetIntegerv) {
			GLint v[16];
			glGetIntegerv(pn, v);
			if (pn == 0x84E2 && v[0] > 8) v[0] = 8;   /* MAX_TEXTURE_UNITS: the guest tracks 8 */
			if (pn == 0x0D33 && (cl->strict & STRICT_LIMITS) && (unsigned int)v[0] > cl->max_tex) v[0] = (GLint)cl->max_tex;
			for (i = 0; i < n; i++) put_be32(out + 4 + i * 4, (uint32_t)v[i]);
		} else if (slot == SLOT_GLGetFloatv) {
			GLfloat v[16];
			glGetFloatv(pn, v);
			if (pn == 0x0D33 && (cl->strict & STRICT_LIMITS) && v[0] > (GLfloat)cl->max_tex) v[0] = (GLfloat)cl->max_tex;
			for (i = 0; i < n; i++) { uint32_t u; memcpy(&u, &v[i], 4); put_be32(out + 4 + i * 4, u); }
		} else if (slot == SLOT_GLGetBooleanv) {
			GLboolean v[16];
			glGetBooleanv(pn, v);
			for (i = 0; i < n; i++) put_be32(out + 4 + i * 4, v[i]);
		} else {
			GLdouble v[16];
			glGetDoublev(pn, v);
			for (i = 0; i < n; i++) {
				uint64_t u; memcpy(&u, &v[i], 8);
				put_be32(out + 4 + i * 8, (uint32_t)(u >> 32)); put_be32(out + 8 + i * 8, (uint32_t)u);
			}
		}
		reply(cl, out, 4 + n * esz);
		return 1;
	}
	case SLOT_GLTexImage2D: case SLOT_GLTexSubImage2D: case SLOT_GLTexImage1D: case SLOT_GLTexSubImage1D:
	case SLOT_GLDrawPixels: {
		uint32_t f[8];
		int nf = slot == SLOT_GLTexImage1D ? 7 : slot == SLOT_GLTexSubImage1D ? 6 : slot == SLOT_GLDrawPixels ? 4 : 8, i;
		int has, esz;
		unsigned int nbytes;
		unsigned char *data;
		for (i = 0; i < nf; i++) f[i] = rd_u32(&p);
		has = rd_u32(&p); esz = rd_u32(&p); nbytes = rd_u32(&p);
		data = has ? (unsigned char *)p : NULL;
		if (data && (size_t)(p - p0) + nbytes > len) { hlog("short pixel payload"); return 1; }
		if (data && esz > 1) swap_inplace(data, nbytes, esz);
		if (slot == SLOT_GLTexImage2D && strict_tex_refuse(cl, f[0], (int)f[1], (GLint)f[2], (int)f[3], (int)f[4], f[7]))
			glTexImage2D(f[0], f[1], f[2], -1, -1, f[5], f[6], f[7], NULL);   /* fails like the real one: GL_INVALID_VALUE */
		else if (slot == SLOT_GLTexImage2D)
			glTexImage2D(f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7], data);
		else if (slot == SLOT_GLTexSubImage2D)
			glTexSubImage2D(f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7], data);
		else if (slot == SLOT_GLTexImage1D)
			glTexImage1D(f[0], f[1], f[2], f[3], f[4], f[5], f[6], data);
		else if (slot == SLOT_GLTexSubImage1D)
			glTexSubImage1D(f[0], f[1], f[2], f[3], f[4], f[5], data);
		else
			glDrawPixels(f[0], f[1], f[2], f[3], data);
		return 1;
	}
	case SLOT_GLReadPixels: {
		GLint x = rd_s32(&p), y = rd_s32(&p);
		GLsizei w = rd_s32(&p), h = rd_s32(&p);
		GLenum fmt = rd_u32(&p), type = rd_u32(&p);
		GLint bpp;
		size_t n;
		unsigned char *buf;
		int comps = (fmt == GL_RGBA || fmt == GL_BGRA_ || fmt == 0x8000) ? 4 : (fmt == GL_RGB || fmt == 0x80E0) ? 3 :
		            fmt == GL_LUMINANCE_ALPHA ? 2 : 1;
		int esz = pix_esz(type);
		switch (type) {
		case 0x8032: case 0x8362: bpp = 1; break;
		case 0x8363: case 0x8364: case 0x8033: case 0x8365: case 0x8034: case 0x8366: bpp = 2; break;
		case 0x8035: case 0x8367: case 0x8036: case 0x8368: bpp = 4; break;
		default: bpp = comps * esz;
		}
		n = (size_t)w * h * bpp;
		buf = (unsigned char *)malloc(n ? n : 1);
		glReadPixels(x, y, w, h, fmt, type, buf);
		if (esz > 1) swap_inplace(buf, n, esz);
		reply(cl, buf, (unsigned int)n);
		free(buf);
		return 1;
	}
	case SLOT_GLGenTextures: {
		int n = rd_s32(&p), i;
		GLuint *t;
		unsigned char *o;
		if (n < 0 || n > 65536) n = 0;
		t = (GLuint *)malloc(sizeof(GLuint) * (n + 1));
		o = (unsigned char *)malloc(4 * (n + 1));
		glGenTextures(n, t);
		for (i = 0; i < n; i++) put_be32(o + 4 * i, t[i]);
		reply(cl, o, 4 * n);
		free(t); free(o);
		return 1;
	}
	case SLOT_GLDeleteTextures: {
		int n = rd_s32(&p), i;
		GLuint t[256];
		while (n > 0) {
			int k = n > 256 ? 256 : n;
			for (i = 0; i < k; i++) { t[i] = rd_u32(&p); strict_tex_delete(cl, t[i]); }
			glDeleteTextures(k, t);
			n -= k;
		}
		return 1;
	}
	case SLOT_GLEnableClientState: glEnableClientState(rd_u32(&p)); return 1;
	case SLOT_GLDisableClientState: glDisableClientState(rd_u32(&p)); return 1;
	case SLOT_GLClientActiveTexture: {
		GLenum t = rd_u32(&p);
		cl->client_unit = (int)(t - GL_TEXTURE0_);
		if (cl->client_unit < 0 || cl->client_unit > 7) cl->client_unit = 0;
		if (p_glClientActiveTexture) p_glClientActiveTexture(t);
		return 1;
	}
	case SLOT_GLDrawArrays: {
		GLenum m = rd_u32(&p); GLint first = rd_s32(&p); GLsizei count = rd_s32(&p);
		glDrawArrays(m, first, count);
		return 1;
	}
	case SLOT_GLDrawElements: {
		GLenum m = rd_u32(&p); GLsizei count = rd_s32(&p); GLenum type = rd_u32(&p);
		unsigned int nbytes = rd_u32(&p);
		unsigned char *idx = (unsigned char *)p;
		if ((size_t)(p - p0) + nbytes > len) { hlog("short index payload"); return 1; }
		swap_inplace(idx, nbytes, type == GL_UNSIGNED_SHORT ? 2 : type == GL_UNSIGNED_INT ? 4 : 1);
		if (cl->strict & STRICT_LIMITS) strict_idx(cl, count, type);
		glDrawElements(m, count, type, idx);
		return 1;
	}
	case SLOT_GLArrayElement: glArrayElement(rd_s32(&p)); return 1;
	/* host-side remaps: the guest's "window system framebuffer" is our FBO */
	case SLOT_GLBindFramebuffer: case SLOT_GLBindFramebufferEXT: {
		GLenum target = rd_u32(&p); GLuint fb = rd_u32(&p);
		if (p_glBindFramebuffer) p_glBindFramebuffer(target, fb ? fb : cl->fbo);
		return 1;
	}
	case SLOT_GLDrawBuffer: case SLOT_GLReadBuffer: {
		GLenum b = rd_u32(&p);
		if (b == GL_FRONT || b == GL_BACK || b == GL_FRONT_LEFT || b == GL_BACK_LEFT || b == GL_FRONT_AND_BACK || b == GL_LEFT)
			b = GL_COLOR_ATTACHMENT0;
		if (slot == SLOT_GLDrawBuffer) glDrawBuffer(b); else glReadBuffer(b);
		return 1;
	}
	}
	return 0;
}

/* ------------------------------------------------------------------ swap */
static void do_swap(Client *cl, int mirror)
{
	int i;
	double t;
	cl->swaps++;
	if (g_shot_every > 0 && cl->swaps >= (unsigned int)g_shot_from && (cl->swaps % (unsigned int)g_shot_every) == 0) {
		char path[MAX_PATH];
		snprintf(path, sizeof(path), "%s\\client%d_frame%05u.bmp", g_shotdir, cl->id, cl->swaps);
		save_bmp(cl, path);
	}
	for (i = 0; i < g_nshots; i++)
		if ((unsigned int)g_shots[i] == cl->swaps) {
			char path[MAX_PATH];
			snprintf(path, sizeof(path), "%s\\client%d_frame%u.bmp", g_shotdir, cl->id, cl->swaps);
			save_bmp(cl, path);
		}
	if (!g_hidden && cl->shown) {
		p_glBindFramebuffer(GL_READ_FRAMEBUFFER, cl->fbo);
		p_glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
		glDrawBuffer(GL_BACK);
		p_glBlitFramebuffer(0, 0, cl->w, cl->h, 0, 0, cl->w, cl->h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
		SwapBuffers(cl->hdc);
		p_glBindFramebuffer(GL_FRAMEBUFFER, cl->fbo);
		glDrawBuffer(GL_COLOR_ATTACHMENT0);
	}
	if (mirror) {
		size_t n = (size_t)cl->w * cl->h * 4;
		unsigned char *buf = (unsigned char *)malloc(12 + n), *px = (unsigned char *)malloc(n);
		int y;
		/* BGRA + UNSIGNED_INT_8_8_8_8 on a little-endian host = bytes A,R,G,B (RECTFMT_ARGB) */
		glReadPixels(0, 0, cl->w, cl->h, GL_BGRA_, GL_UNSIGNED_INT_8_8_8_8_, px);
		put_be32(buf, cl->swaps); put_be32(buf + 4, cl->w); put_be32(buf + 8, cl->h);
		for (y = 0; y < cl->h; y++)        /* GL bottom-up -> Amiga top-down */
			memcpy(buf + 12 + (size_t)y * cl->w * 4, px + (size_t)(cl->h - 1 - y) * cl->w * 4, (size_t)cl->w * 4);
		reply(cl, buf, (unsigned int)(12 + n));
		free(buf); free(px);
	} else {
		unsigned char b[12];
		put_be32(b, cl->swaps); put_be32(b + 4, cl->w); put_be32(b + 8, cl->h);
		reply(cl, b, 12);
	}
	t = now_s();
	if (t - cl->t_last >= 2.0) {
		double dt = t - cl->t_last;
		char title[200];
		hlog("client %d: %.1f fps, %.0f KB/s in, %.0f cmds/frame (swap %u)", cl->id,
		     (cl->swaps - cl->swaps_last) / dt, (cl->bytes_in - cl->bytes_last) / dt / 1024.0,
		     (double)(cl->cmds - cl->cmds_last) / (cl->swaps - cl->swaps_last ? cl->swaps - cl->swaps_last : 1), cl->swaps);
		if ((cl->strict & STRICT_LIMITS) && (cl->swaps / 50) != (cl->swaps_last / 50))
			hlog("client %d: strict limits: textures %llu KB (peak %llu KB, budget %u KB), refused: %u too big, %u over budget; max indices/draw %u, max vertices/array %u",
			     cl->id, cl->tex_bytes >> 10, cl->tex_peak >> 10, cl->tex_budget_kb, cl->tex_refused_size, cl->tex_refused_mem,
			     cl->max_idx_count, cl->max_vcount);
		snprintf(title, sizeof(title), "TGLBridge #%d %s %dx%d - %.1f fps", cl->id, cl->name, cl->w, cl->h,
		         (cl->swaps - cl->swaps_last) / dt);
		SetWindowTextA(cl->hwnd, title);
		cl->t_last = t; cl->swaps_last = cl->swaps; cl->bytes_last = cl->bytes_in; cl->cmds_last = cl->cmds;
	}
}

/* ------------------------------------------------------------------ command loop */
static void exec_cmd2(Client *cl, uint32_t op, unsigned char *p, uint32_t len);
static void exec_cmd(Client *cl, uint32_t op, unsigned char *p, uint32_t len)
{
	static unsigned char seen[0x10000];
	static int in_begin;
	exec_cmd2(cl, op, p, len);
	if (op == SLOT_GLBegin) in_begin = 1;
	if (op == SLOT_GLEnd) in_begin = 0;
	if (g_glcheck && !in_begin && cl->rc && !cl->dead && op != SLOT_GLGetError) {
		GLenum e = glGetError();
		if (e && !seen[op & 0xffff]) {
			seen[op & 0xffff] = 1;
			hlog("glcheck: GL error 0x%x after %s (op 0x%x) - logged once", e,
			     op < (uint32_t)TGLB_NSLOTS ? g_slot_name[op] : "protocol message", op);
		}
	}
}

static void exec_cmd2(Client *cl, uint32_t op, unsigned char *p, uint32_t len)
{
	const unsigned char *q = p;
	cl->cmds++;
	if (op == OP_HELLO) {
		uint32_t magic = rd_u32(&q), ver = rd_u32(&q);
		memcpy(cl->name, q, 31); cl->name[31] = 0;
		if (magic != TGLB_MAGIC || ver != TGLB_PROTO_VER) {
			hlog("client %d: bad HELLO magic %08x ver %u", cl->id, magic, ver);
			reply_u32(cl, 0); cl->dead = 1; return;
		}
		if (!ctx_create(cl)) { reply_u32(cl, 0); cl->dead = 1; return; }
		hlog("client %d: HELLO from guest task \"%s\"", cl->id, cl->name);
		reply_u32(cl, 1);
		return;
	}
	if (!cl->rc) { hlog("client %d: command 0x%x before HELLO", cl->id, op); cl->dead = 1; return; }
	make_current(cl);
	switch (op) {
	case OP_UNIMPL: log_unimpl((int)rd_u32(&q), "no bridge implementation in the guest library"); return;
	case OP_NOTE: {
		uint32_t n = rd_u32(&q);
		hlog("client %d: guest note: %.*s", cl->id, (int)n, q);
		return;
	}
	case OP_CREATE: {
		int kind = rd_s32(&q), w = rd_s32(&q), h = rd_s32(&q);
		fbo_setup(cl, w, h);
		glViewport(0, 0, w, h);
		glScissor(0, 0, w, h);
		cl->kind = kind;
		window_size(cl, w, h, kind);
		hlog("client %d: context %dx%d (%s)", cl->id, w, h, kind == 2 ? "screen" : "window");
		reply_u32(cl, 1);
		return;
	}
	case OP_RESIZE: {
		int w = rd_s32(&q), h = rd_s32(&q);
		fbo_setup(cl, w, h);
		window_size(cl, w, h, cl->kind);
		hlog("client %d: resize %dx%d", cl->id, w, h);
		return;
	}
	case OP_DESTROY:
		hlog("client %d: context destroyed after %u swaps", cl->id, cl->swaps);
		{   /* always keep the last frame as evidence */
			char path[MAX_PATH];
			snprintf(path, sizeof(path), "%s\\client%d_last.bmp", g_shotdir, cl->id);
			save_bmp(cl, path);
		}
		cl->kind = 0;
		return;
	case OP_SWAP: do_swap(cl, (int)rd_u32(&q)); return;
	case OP_SETSYNC: {
		int en = (int)rd_u32(&q);
		if (p_wglSwapIntervalEXT) p_wglSwapIntervalEXT(en ? 1 : 0);
		cl->vsync = en;
		return;
	}
	case OP_ARRAY: {
		int id = rd_s32(&q), size = rd_s32(&q);
		GLenum type = rd_u32(&q);
		int first = rd_s32(&q), count = rd_s32(&q);
		int ts = type_size(type);
		size_t n = (size_t)ts * size * count;
		if (id < 0 || id >= ARR_COUNT || !ts || (size_t)(q - p) + n > len) { hlog("bad OP_ARRAY"); return; }
		if (cl->acap[id] < n) {
			free(cl->abuf[id]);
			cl->abuf[id] = malloc(n + 64);
			cl->acap[id] = n;
		}
		memcpy(cl->abuf[id], q, n);
		if ((unsigned int)count > cl->max_vcount) cl->max_vcount = (unsigned int)count;
		swap_inplace((unsigned char *)cl->abuf[id], n, ts);
		set_array_pointer(cl, id, size, type, (unsigned char *)cl->abuf[id] - (ptrdiff_t)first * ts * size);
		return;
	}
	case OP_BYE: hlog("client %d: BYE", cl->id); cl->dead = 1; return;
	case OP_STRICT:
		cl->strict = rd_u32(&q); cl->max_tex = rd_u32(&q); cl->tex_budget_kb = rd_u32(&q);
		if (!cl->max_tex) cl->max_tex = 2048;
		hlog("client %d: STRICT mode 0x%x (lazy arrays %d, lazy indices %d, stride quirk tc/col %d xyz %d, limits %d: max tex %u, budget %u KB, canary %d, lag swap %d)",
		     cl->id, cl->strict, !!(cl->strict & STRICT_LAZY_ARRAYS), !!(cl->strict & STRICT_LAZY_INDICES),
		     !!(cl->strict & STRICT_STRIDE_TCCOL), !!(cl->strict & STRICT_STRIDE_XYZ), !!(cl->strict & STRICT_LIMITS),
		     cl->max_tex, cl->tex_budget_kb, !!(cl->strict & STRICT_CANARY), !!(cl->strict & STRICT_LAG_SWAP));
		return;
	}
	if (op < (uint32_t)TGLB_NSLOTS) {
		if (manual(cl, (int)op, p, len)) return;
		if (gen_dispatch(cl, (int)op, p, len)) return;
		log_unimpl((int)op, "no host handler");
		if (gen_is_sync((int)op)) reply_u32(cl, 0);
		return;
	}
	hlog("client %d: unknown opcode 0x%x", cl->id, op);
}

static void client_read(Client *cl)
{
	size_t off = 0;
	int r;
	if (cl->in_cap - cl->in_len < 65536) {
		cl->in_cap = cl->in_cap ? cl->in_cap * 2 : 1 << 20;
		cl->in = (unsigned char *)realloc(cl->in, cl->in_cap);
	}
	r = recv(cl->s, (char *)cl->in + cl->in_len, (int)(cl->in_cap - cl->in_len), 0);
	if (r <= 0) { cl->dead = 1; return; }
	cl->in_len += r;
	cl->bytes_in += r;
	while (cl->in_len - off >= 8 && !cl->dead) {
		uint32_t op = be32(cl->in + off), len = be32(cl->in + off + 4);
		if (cl->in_len - off - 8 < len) {
			if (8 + (size_t)len > cl->in_cap) {   /* grow for a big command */
				while (8 + (size_t)len > cl->in_cap) cl->in_cap *= 2;
				cl->in = (unsigned char *)realloc(cl->in, cl->in_cap);
			}
			break;
		}
		exec_cmd(cl, op, cl->in + off + 8, len);
		off += 8 + len;
	}
	if (off) {
		memmove(cl->in, cl->in + off, cl->in_len - off);
		cl->in_len -= off;
	}
}

static void parse_shots(const char *s)
{
	while (*s && g_nshots < 64) {
		g_shots[g_nshots++] = atoi(s);
		while (*s && *s != ',') s++;
		if (*s == ',') s++;
	}
}

int main(int argc, char **argv)
{
	WSADATA wd;
	SOCKET ls;
	struct sockaddr_in sa;
	int i, one = 1;
	const char *logpath = "tglhost.log";
	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--port") && i + 1 < argc) g_port = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--hidden")) g_hidden = 1;
		else if (!strcmp(argv[i], "--glcheck")) g_glcheck = 1;
		else if (!strcmp(argv[i], "--shot") && i + 1 < argc) parse_shots(argv[++i]);
		else if (!strcmp(argv[i], "--shotdir") && i + 1 < argc) snprintf(g_shotdir, sizeof(g_shotdir), "%s", argv[++i]);
		else if (!strcmp(argv[i], "--log") && i + 1 < argc) logpath = argv[++i];
		else if (!strcmp(argv[i], "--exit-after") && i + 1 < argc) g_exit_after = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--shot-every") && i + 1 < argc) g_shot_every = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--shot-from") && i + 1 < argc) g_shot_from = atoi(argv[++i]);
		else { fprintf(stderr, "usage: tglhost [--port N] [--hidden] [--shot a,b,c] [--shotdir dir] [--log file] [--exit-after N] [--shot-every N] [--shot-from S]\n"); return 1; }
	}
	g_log = fopen(logpath, "a");
	CreateDirectoryA(g_shotdir, NULL);
	g_gl32 = LoadLibraryA("opengl32.dll");
	g_glu32 = LoadLibraryA("glu32.dll");
	WSAStartup(MAKEWORD(2, 2), &wd);
	ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, (const char *)&one, sizeof(one));
	memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_port = htons((u_short)g_port);
	sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (bind(ls, (struct sockaddr *)&sa, sizeof(sa)) || listen(ls, 4)) {
		hlog("cannot listen on 127.0.0.1:%d (error %d)", g_port, WSAGetLastError());
		return 1;
	}
	hlog("tglhost listening on 127.0.0.1:%d (pid %lu)%s", g_port, GetCurrentProcessId(), g_hidden ? " hidden" : "");
	for (;;) {
		fd_set rs;
		struct timeval tv = { 0, 2000 };
		MSG msg;
		int maxfd = 0;
		while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageA(&msg); }
		FD_ZERO(&rs);
		FD_SET(ls, &rs);
		for (i = 0; i < MAXCL; i++) if (g_cl[i]) FD_SET(g_cl[i]->s, &rs);
		if (select(maxfd, &rs, NULL, NULL, &tv) <= 0) continue;
		if (FD_ISSET(ls, &rs)) {
			SOCKET s = accept(ls, NULL, NULL);
			if (s != INVALID_SOCKET) {
				int slot = -1, bs = 4 << 20;
				for (i = 0; i < MAXCL; i++) if (!g_cl[i]) { slot = i; break; }
				if (slot < 0) { closesocket(s); }
				else {
					Client *cl = (Client *)calloc(1, sizeof(Client));
					cl->s = s;
					cl->id = g_next_id++;
					setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof(one));
					setsockopt(s, SOL_SOCKET, SO_RCVBUF, (const char *)&bs, sizeof(bs));
					g_cl[slot] = cl;
					hlog("client %d: connected", cl->id);
				}
			}
		}
		for (i = 0; i < MAXCL; i++) {
			Client *cl = g_cl[i];
			if (!cl || !FD_ISSET(cl->s, &rs)) continue;
			client_read(cl);
			if (cl->dead) {
				hlog("client %d: closed (%u swaps, %llu bytes received)", cl->id, cl->swaps, cl->bytes_in);
				client_free(cl);
				g_cl[i] = NULL;
				g_closed++;
				if (g_exit_after > 0 && g_closed >= g_exit_after) { hlog("exit after %d clients", g_closed); return 0; }
			}
		}
	}
}
