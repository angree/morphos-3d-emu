/* SPDX-License-Identifier: MIT */
/* tglbtest - a plain TinyGL program (SDK headers only, nothing bridge-specific).
 *   tglbtest            triangle (immediate mode) + RGB565 textured quad (vertex arrays, glLockArraysEXT,
 *                       glDrawElements) + ARGB4444 blended/alpha-tested quad + fog; glReadPixels check
 *   tglbtest stress [s] ~10k textured, vertex-coloured triangles per frame for s seconds (default 10)
 * Log goes to stdout. */
#include <proto/exec.h>
#include <proto/intuition.h>
#include <proto/dos.h>
#include <proto/tinygl.h>
#include <GL/gl.h>
#include <intuition/intuition.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

unsigned long __stack = 262144;
struct Library *TinyGLBase;
GLContext *__tglContext;

#define W 320
#define H 240

struct vtx { float x, y, z; float u, v; unsigned char rgba[4]; };

static unsigned short tex565[64 * 64];
static unsigned short tex4444[64 * 64];

static void make_textures(void)
{
	int x, y;
	for (y = 0; y < 64; y++)
		for (x = 0; x < 64; x++) {
			int on = ((x >> 3) ^ (y >> 3)) & 1;
			/* RGB565: red / yellow checker (host order = big-endian, as on disk) */
			tex565[y * 64 + x] = on ? 0xF800 : 0xFFE0;
			/* RGBA4444 (R in the top nibble): white ring with alpha, transparent outside */
			{
				int dx = x - 32, dy = y - 32, d2 = dx * dx + dy * dy;
				unsigned short a = (d2 < 900 && d2 > 300) ? 0xF : (d2 <= 300 ? 0x8 : 0x0);
				tex4444[y * 64 + x] = (unsigned short)(0x0 << 12 | 0xF << 8 | 0xF << 4 | a); /* cyan */
			}
		}
}

static double now_s(void)
{
	struct DateStamp ds;
	DateStamp(&ds);
	return ds.ds_Days * 86400.0 + ds.ds_Minute * 60.0 + ds.ds_Tick / 50.0;
}

static int run_basic(struct Window *win)
{
	GLuint tex[2];
	struct vtx q[4] = {
		{ 0.1f, 0.1f, 0, 0, 0, {255, 255, 255, 255} }, { 0.9f, 0.1f, 0, 2, 0, {255, 255, 255, 255} },
		{ 0.9f, 0.9f, 0, 2, 2, {255, 255, 255, 255} }, { 0.1f, 0.9f, 0, 0, 2, {255, 255, 255, 255} } };
	struct vtx q2[4] = {
		{ 0.2f, -0.9f, 0, 0, 0, {255, 255, 255, 255} }, { 0.8f, -0.9f, 0, 1, 0, {255, 255, 255, 255} },
		{ 0.8f, -0.3f, 0, 1, 1, {255, 255, 255, 255} }, { 0.2f, -0.3f, 0, 0, 1, {255, 255, 255, 255} } };
	unsigned short idx[6] = { 0, 1, 2, 0, 2, 3 };
	unsigned char px[4 * 4];
	int f, err;
	float fogc[4] = { 0.1f, 0.1f, 0.3f, 1.0f };
	double t0;

	printf("GL_VENDOR   %s\nGL_RENDERER %s\nGL_VERSION  %s\n", (char *)glGetString(GL_VENDOR),
	       (char *)glGetString(GL_RENDERER), (char *)glGetString(GL_VERSION));
	{
		GLint mt = 0, mu = 0, vp[4] = {0, 0, 0, 0};
		glGetIntegerv(GL_MAX_TEXTURE_SIZE, &mt);
		glGetIntegerv(GL_MAX_TEXTURE_UNITS_ARB, &mu);
		glGetIntegerv(GL_VIEWPORT, vp);
		printf("MAX_TEXTURE_SIZE %d  MAX_TEXTURE_UNITS %d  VIEWPORT %d %d %d %d\n", (int)mt, (int)mu,
		       (int)vp[0], (int)vp[1], (int)vp[2], (int)vp[3]);
	}
	make_textures();
	glGenTextures(2, tex);
	printf("textures %u %u\n", (unsigned)tex[0], (unsigned)tex[1]);
	glBindTexture(GL_TEXTURE_2D, tex[0]);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 2);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 64, 64, 0, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, tex565);
	glBindTexture(GL_TEXTURE_2D, tex[1]);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 64, 64, 0, GL_RGBA, GL_UNSIGNED_SHORT_4_4_4_4, tex4444);
	printf("glGetError after uploads: 0x%x\n", (unsigned)glGetError());

	glViewport(0, 0, W, H);
	t0 = now_s();
	for (f = 0; f < 150; f++) {
		glClearColor(0.1f, 0.1f, 0.3f, 1.0f);
		glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
		glMatrixMode(GL_PROJECTION);
		glLoadIdentity();
		glOrtho(-1, 1, -1, 1, -1, 1);
		glMatrixMode(GL_MODELVIEW);
		glLoadIdentity();
		glEnable(GL_DEPTH_TEST);
		glDepthFunc(GL_LEQUAL);

		/* 1: Gouraud triangle, immediate mode, rotating */
		glDisable(GL_TEXTURE_2D);
		glPushMatrix();
		glTranslatef(-0.5f, 0.0f, 0.0f);
		glRotatef((float)f * 2.0f, 0, 0, 1);
		glBegin(GL_TRIANGLES);
		glColor3f(1, 0, 0); glVertex3f(-0.4f, -0.35f, 0);
		glColor3f(0, 1, 0); glVertex3f(0.4f, -0.35f, 0);
		glColor3f(0, 0, 1); glVertex3f(0.0f, 0.45f, 0);
		glEnd();
		glPopMatrix();

		/* 2: RGB565 textured quad, client arrays (interleaved, stride 24) + lock + DrawElements */
		glEnable(GL_TEXTURE_2D);
		glBindTexture(GL_TEXTURE_2D, tex[0]);
		glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
		glEnableClientState(GL_VERTEX_ARRAY);
		glEnableClientState(GL_TEXTURE_COORD_ARRAY);
		glEnableClientState(GL_COLOR_ARRAY);
		glVertexPointer(3, GL_FLOAT, sizeof(struct vtx), &q[0].x);
		glTexCoordPointer(2, GL_FLOAT, sizeof(struct vtx), &q[0].u);
		glColorPointer(4, GL_UNSIGNED_BYTE, sizeof(struct vtx), q[0].rgba);
		glLockArraysEXT(0, 4);
		glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, idx);
		glUnlockArraysEXT();

		/* 3: RGBA4444 quad, alpha blend + alpha test + linear fog */
		glBindTexture(GL_TEXTURE_2D, tex[1]);
		glEnable(GL_BLEND);
		glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
		glEnable(GL_ALPHA_TEST);
		glAlphaFunc(GL_GREATER, 0.1f);
		glEnable(GL_FOG);
		glFogi(GL_FOG_MODE, GL_LINEAR);
		glFogf(GL_FOG_START, 0.0f);
		glFogf(GL_FOG_END, 4.0f);
		glFogfv(GL_FOG_COLOR, fogc);
		glVertexPointer(3, GL_FLOAT, sizeof(struct vtx), &q2[0].x);
		glTexCoordPointer(2, GL_FLOAT, sizeof(struct vtx), &q2[0].u);
		glColorPointer(4, GL_UNSIGNED_BYTE, sizeof(struct vtx), q2[0].rgba);
		glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, idx);
		glDisable(GL_FOG);
		glDisable(GL_ALPHA_TEST);
		glDisable(GL_BLEND);
		glDisableClientState(GL_COLOR_ARRAY);
		glDisableClientState(GL_TEXTURE_COORD_ARRAY);
		glDisableClientState(GL_VERTEX_ARRAY);

		if (f == 60) {
			/* readback: triangle centre (x=80,y=120 is the left-half centre), quad, ring */
			glReadPixels(80, 120, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
			glReadPixels(200, 160, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px + 4);
			glReadPixels(5, 5, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px + 8);
			printf("readpixels tri(80,120)=%d,%d,%d,%d quad(200,160)=%d,%d,%d,%d bg(5,5)=%d,%d,%d,%d\n",
			       px[0], px[1], px[2], px[3], px[4], px[5], px[6], px[7], px[8], px[9], px[10], px[11]);
		}
		glASwapBuffers();
	}
	printf("basic: 150 frames in %.2f s\n", now_s() - t0);
	err = glGetError();
	printf("glGetError at end: 0x%x\n", (unsigned)err);
	glDeleteTextures(2, tex);
	return 0;
}

static int run_stress(struct Window *win, double secs)
{
	const int GX = 100, GY = 50;           /* 100x50 quads = 10000 triangles */
	int nv = (GX + 1) * (GY + 1), ni = GX * GY * 6, x, y, k, frames = 0;
	struct vtx *v = malloc(sizeof(struct vtx) * nv);
	unsigned short *idx = malloc(2 * ni);
	GLuint tex;
	double t0, t1, tlast;
	int flast = 0;
	if (!v || !idx) return 1;
	for (y = 0; y <= GY; y++)
		for (x = 0; x <= GX; x++) {
			struct vtx *p = &v[y * (GX + 1) + x];
			p->x = -0.95f + 1.9f * x / GX; p->y = -0.95f + 1.9f * y / GY; p->z = 0;
			p->u = x / 8.0f; p->v = y / 8.0f;
			p->rgba[0] = (unsigned char)(x * 255 / GX); p->rgba[1] = (unsigned char)(y * 255 / GY);
			p->rgba[2] = 200; p->rgba[3] = 255;
		}
	k = 0;
	for (y = 0; y < GY; y++)
		for (x = 0; x < GX; x++) {
			int a = y * (GX + 1) + x;
			idx[k++] = a; idx[k++] = a + 1; idx[k++] = a + GX + 2;
			idx[k++] = a; idx[k++] = a + GX + 2; idx[k++] = a + GX + 1;
		}
	make_textures();
	glGenTextures(1, &tex);
	glBindTexture(GL_TEXTURE_2D, tex);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 64, 64, 0, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, tex565);
	glEnable(GL_TEXTURE_2D);
	glEnable(GL_DEPTH_TEST);
	glViewport(0, 0, W, H);
	glEnableClientState(GL_VERTEX_ARRAY);
	glEnableClientState(GL_TEXTURE_COORD_ARRAY);
	glEnableClientState(GL_COLOR_ARRAY);
	glVertexPointer(3, GL_FLOAT, sizeof(struct vtx), &v[0].x);
	glTexCoordPointer(2, GL_FLOAT, sizeof(struct vtx), &v[0].u);
	glColorPointer(4, GL_UNSIGNED_BYTE, sizeof(struct vtx), v[0].rgba);
	printf("stress: %d vertices, %d triangles per frame, %.0f s\n", nv, ni / 3, secs);
	t0 = tlast = now_s();
	do {
		glClearColor(0, 0, 0, 1);
		glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
		glMatrixMode(GL_MODELVIEW);
		glLoadIdentity();
		glRotatef(frames * 1.0f, 0, 0, 1);
		glLockArraysEXT(0, nv);
		glDrawElements(GL_TRIANGLES, ni, GL_UNSIGNED_SHORT, idx);
		glUnlockArraysEXT();
		glASwapBuffers();
		frames++;
		t1 = now_s();
		if (t1 - tlast >= 2.0) {
			printf("stress: %.1f fps (%d frames in %.2f s)\n", (frames - flast) / (t1 - tlast), frames - flast, t1 - tlast);
			tlast = t1; flast = frames;
		}
	} while (t1 - t0 < secs);
	printf("stress: TOTAL %d frames in %.2f s = %.2f fps, %.0f triangles/s\n", frames, t1 - t0,
	       frames / (t1 - t0), frames * (ni / 3) / (t1 - t0));
	printf("glGetError at end: 0x%x\n", (unsigned)glGetError());
	glDisableClientState(GL_COLOR_ARRAY);
	glDisableClientState(GL_TEXTURE_COORD_ARRAY);
	glDisableClientState(GL_VERTEX_ARRAY);
	glDeleteTextures(1, &tex);
	free(v); free(idx);
	return 0;
}

/* T73: the bug-B pattern - one CPU-side vertex buffer reused for every draw of the frame. A copy-at-call bridge
 * shows 8x8 differently coloured quads; a driver that reads client arrays late (TGLBRIDGE_STRICT 1) shows only the
 * last quad's data, and the canary (32) counts the changed buffers. argv: reuse <secs> [tight] */
static int run_reuse(struct Window *win, double secs, int tight)
{
	static struct vtx v[4];
	static float txyz[12];
	static unsigned char trgba[16];
	static const unsigned short idx[6] = { 0, 1, 2, 0, 2, 3 };
	int frames = 0, gx, gy, k;
	double t0 = now_s(), t1;
	(void)win;
	glDisable(GL_TEXTURE_2D);
	glDisable(GL_DEPTH_TEST);
	glViewport(0, 0, W, H);
	glEnableClientState(GL_VERTEX_ARRAY);
	glEnableClientState(GL_COLOR_ARRAY);
	if (tight) {
		glVertexPointer(3, GL_FLOAT, 0, txyz);
		glColorPointer(4, GL_UNSIGNED_BYTE, 0, trgba);
	} else {
		glVertexPointer(3, GL_FLOAT, sizeof(struct vtx), &v[0].x);
		glColorPointer(4, GL_UNSIGNED_BYTE, sizeof(struct vtx), v[0].rgba);
	}
	printf("reuse: 64 quads per frame from one %s scratch buffer, %.0f s\n", tight ? "tight" : "interleaved", secs);
	do {
		glClearColor(0, 0, 0.3f, 1);
		glClear(GL_COLOR_BUFFER_BIT);
		glMatrixMode(GL_MODELVIEW);
		glLoadIdentity();
		for (gy = 0; gy < 8; gy++)
			for (gx = 0; gx < 8; gx++) {
				float x0 = -0.95f + gx * 0.24f, y0 = -0.95f + gy * 0.24f, x1 = x0 + 0.2f, y1 = y0 + 0.2f;
				float xs[4] = { x0, x1, x1, x0 }, ys[4] = { y0, y0, y1, y1 };
				for (k = 0; k < 4; k++) {
					unsigned char r = (unsigned char)(gx * 32 + 31), g = (unsigned char)(gy * 32 + 31), b = (unsigned char)(k * 60);
					v[k].x = xs[k]; v[k].y = ys[k]; v[k].z = 0; v[k].u = 0; v[k].v = 0;
					v[k].rgba[0] = r; v[k].rgba[1] = g; v[k].rgba[2] = b; v[k].rgba[3] = 255;
					txyz[k * 3] = xs[k]; txyz[k * 3 + 1] = ys[k]; txyz[k * 3 + 2] = 0;
					trgba[k * 4] = r; trgba[k * 4 + 1] = g; trgba[k * 4 + 2] = b; trgba[k * 4 + 3] = 255;
				}
				glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, idx);
			}
		glASwapBuffers();
		frames++;
		t1 = now_s();
	} while (t1 - t0 < secs);
	printf("reuse: %d frames, glGetError 0x%x\n", frames, (unsigned)glGetError());
	glDisableClientState(GL_COLOR_ARRAY);
	glDisableClientState(GL_VERTEX_ARRAY);
	return 0;
}

int main(int argc, char **argv)
{
	struct Window *win;
	int rc = 0, ok;
	TinyGLBase = OpenLibrary((CONST_STRPTR)"tinygl.library", 50);
	printf("tinygl.library %p %s\n", TinyGLBase, TinyGLBase ? (char *)TinyGLBase->lib_IdString : "");
	if (!TinyGLBase) return 20;
	__tglContext = GLInit();
	printf("GLInit -> %p\n", __tglContext);
	if (!__tglContext) { CloseLibrary(TinyGLBase); return 20; }
	win = OpenWindowTags(NULL, WA_InnerWidth, W, WA_InnerHeight, H, WA_Title, (ULONG)"tglbtest",
	                     WA_DragBar, TRUE, WA_DepthGadget, TRUE, WA_Activate, TRUE, WA_Left, 40, WA_Top, 40,
	                     TAG_DONE);
	ok = win ? GLAInitializeContextWindowed(__tglContext, win) : 0;
	printf("window %p GLAInitializeContextWindowed -> %d\n", win, ok);
	if (ok) {
		GLASetSync(__tglContext, 0);
		if (argc > 1 && !strcmp(argv[1], "reuse"))
			rc = run_reuse(win, argc > 2 ? atof(argv[2]) : 6.0, argc > 3 && !strcmp(argv[3], "tight"));
		else if (argc > 1 && !strcmp(argv[1], "stress"))
			rc = run_stress(win, argc > 2 ? atof(argv[2]) : 10.0);
		else
			rc = run_basic(win);
		GLADestroyContextWindowed(__tglContext);
	} else rc = 10;
	if (win) CloseWindow(win);
	GLClose(__tglContext);
	CloseLibrary(TinyGLBase);
	printf("done rc=%d\n", rc);
	return rc;
}
