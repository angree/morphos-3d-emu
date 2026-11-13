/* SPDX-License-Identifier: MIT */
/* tglbload - make the TGLBridge tinygl.library the one every program gets.
 *   tglbload [path]     (default RAM:tinygl.library)
 * Removes a not-in-use system tinygl.library from memory, loads ours by path (ramlib adds it
 * to the library list under the name "tinygl.library") and keeps one open count so it stays.
 * After this, OpenLibrary("tinygl.library", 50) in any program returns the bridge. */
#include <proto/exec.h>
#include <exec/execbase.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv)
{
	const char *path = argc > 1 ? argv[1] : "RAM:tinygl.library";
	struct Library *old, *ours, *byname;

	Forbid();
	old = (struct Library *)FindName(&SysBase->LibList, (CONST_STRPTR)"tinygl.library");
	if (old && old->lib_IdString && strstr((char *)old->lib_IdString, "TGLBridge")) {
		/* a previous tglbload left one open count: drop it and expunge, so a new build loads */
		if (old->lib_OpenCnt > 1) {
			Permit();
			printf("tglbload: bridge loaded and in use by %d programs - kept\n", old->lib_OpenCnt - 1);
			return 0;
		}
		if (old->lib_OpenCnt == 1) CloseLibrary(old);
		RemLibrary(old);
		Permit();
		printf("tglbload: previous bridge expunged (reload)\n");
		Forbid();
		old = (struct Library *)FindName(&SysBase->LibList, (CONST_STRPTR)"tinygl.library");
	}
	if (old && old->lib_OpenCnt == 0) {
		RemLibrary(old);
		Permit();
		printf("tglbload: removed system tinygl.library v%d.%d from memory\n", old->lib_Version, old->lib_Revision);
	} else {
		Permit();
		if (old) {
			printf("tglbload: system tinygl.library is IN USE (%d openers) - quit those programs first\n", old->lib_OpenCnt);
			return 10;
		}
		printf("tglbload: no tinygl.library in memory yet\n");
	}
	ours = OpenLibrary((CONST_STRPTR)path, 0);
	if (!ours) {
		printf("tglbload: cannot load %s\n", path);
		return 20;
	}
	printf("tglbload: loaded %s -> %p  %s\n", path, ours, ours->lib_IdString ? (char *)ours->lib_IdString : "");
	byname = OpenLibrary((CONST_STRPTR)"tinygl.library", 50);
	printf("tglbload: OpenLibrary(\"tinygl.library\",50) -> %p (%s)\n", byname,
	       byname == ours ? "the bridge - OK" : "NOT the bridge");
	if (byname) CloseLibrary(byname);
	/* the open count from the path open is kept on purpose: the bridge stays in memory */
	return byname == ours ? 0 : 5;
}
