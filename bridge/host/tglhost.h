/* SPDX-License-Identifier: MIT */
/* TGLBridge host: shared declarations */
#ifndef TGLHOST_H
#define TGLHOST_H

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <GL/gl.h>
#include <stdint.h>
#include <stdio.h>
#include "../common/tglb_proto.h"
#include "gen_slotnums.h"

#ifndef APIENTRY
#define APIENTRY __stdcall
#endif

typedef struct Client {
	SOCKET s;
	int id;
	char name[40];
	HWND hwnd;
	HDC hdc;
	HGLRC rc;
	unsigned int fbo, rb_color, rb_depth;
	int w, h, kind, shown;
	unsigned char *in;
	size_t in_len, in_cap;
	void *abuf[ARR_COUNT];
	size_t acap[ARR_COUNT];
	int client_unit;
	unsigned int swaps;
	double t_last;
	unsigned int swaps_last;
	unsigned long long bytes_in, bytes_last, cmds, cmds_last;
	int dead;
	int vsync;
} Client;

extern const char *const g_slot_name[];
extern const char g_slot_class[];
extern const int g_nslots;

void *resolve(Client *cl, int slot);
void reply_u32(Client *cl, unsigned int v);
void hlog(const char *fmt, ...);
int gen_dispatch(Client *cl, int slot, const unsigned char *p, unsigned int len);
int gen_is_sync(int slot);

static inline uint32_t be32(const unsigned char *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static inline uint32_t rd_u32(const unsigned char **pp) { uint32_t v = be32(*pp); *pp += 4; return v; }
static inline int32_t rd_s32(const unsigned char **pp) { return (int32_t)rd_u32(pp); }
static inline float rd_f32(const unsigned char **pp)
{
	union { uint32_t u; float f; } x; x.u = rd_u32(pp); return x.f;
}
static inline uint64_t rd_u64(const unsigned char **pp)
{
	uint64_t hi = rd_u32(pp); uint64_t lo = rd_u32(pp); return (hi << 32) | lo;
}
static inline double rd_f64(const unsigned char **pp)
{
	union { uint64_t u; double d; } x; x.u = rd_u64(pp); return x.d;
}

/* big-endian elements of size esz -> host order */
static inline void unpack_array(void *dst, const unsigned char *src, int n, int esz)
{
	unsigned char *d = (unsigned char *)dst;
	int i, k;
	for (i = 0; i < n; i++)
		for (k = 0; k < esz; k++) d[i * esz + k] = src[i * esz + esz - 1 - k];
}

/* in-place byte swap of n bytes in elements of esz */
static inline void swap_inplace(unsigned char *p, size_t n, int esz)
{
	size_t i;
	if (esz == 2) {
		for (i = 0; i + 1 < n; i += 2) { unsigned char t = p[i]; p[i] = p[i + 1]; p[i + 1] = t; }
	} else if (esz == 4) {
		for (i = 0; i + 3 < n; i += 4) {
			uint32_t v; memcpy(&v, p + i, 4);
			v = _byteswap_ulong(v); memcpy(p + i, &v, 4);
		}
	} else if (esz == 8) {
		for (i = 0; i + 7 < n; i += 8) {
			uint64_t v; memcpy(&v, p + i, 8);
			v = _byteswap_uint64(v); memcpy(p + i, &v, 8);
		}
	}
}

#endif
