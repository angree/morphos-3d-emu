/* SPDX-License-Identifier: MIT */
/* tglbget - move files between the MorphOS guest and the host's tglbserve.py over TCP, so files can
 * be exchanged after boot without the vvfat disk (which never shows host files added after boot).
 *   tglbget get <ip> <port> <name> <dest>
 *   tglbget put <ip> <port> <name> <src>
 * Protocol: "GET name\n" -> u32 size (0xFFFFFFFF = missing) + data;  "PUT name size\n" + data -> "OK\n" */
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/socket.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct Library *SocketBase;

static unsigned long parse_ip(const char *s)
{
	unsigned long ip = 0; int k;
	for (k = 0; k < 4; k++) {
		int part = 0;
		while (*s >= '0' && *s <= '9') part = part * 10 + (*s++ - '0');
		ip = (ip << 8) | (part & 255);
		if (*s == '.') s++;
	}
	return ip;
}

static int sendall(long s, const char *b, long n)
{
	while (n > 0) { long r = send(s, (APTR)b, n, 0); if (r <= 0) return -1; b += r; n -= r; }
	return 0;
}

static int recvall(long s, char *b, long n)
{
	while (n > 0) { long r = recv(s, (APTR)b, n, 0); if (r <= 0) return -1; b += r; n -= r; }
	return 0;
}

int main(int argc, char **argv)
{
	struct sockaddr_in sa;
	long s;
	char line[300];
	int rc = 20;
	if (argc != 6 || (strcmp(argv[1], "get") && strcmp(argv[1], "put"))) {
		printf("usage: tglbget get|put <ip> <port> <name> <file>\n");
		return 10;
	}
	SocketBase = OpenLibrary((CONST_STRPTR)"bsdsocket.library", 4);
	if (!SocketBase) { printf("tglbget: no bsdsocket.library\n"); return 20; }
	s = socket(AF_INET, SOCK_STREAM, 0);
	memset(&sa, 0, sizeof(sa));
	sa.sin_len = sizeof(sa);
	sa.sin_family = AF_INET;
	sa.sin_port = atoi(argv[3]);
	sa.sin_addr.s_addr = parse_ip(argv[2]);
	if (s < 0 || connect(s, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
		printf("tglbget: connect %s:%s failed (errno %ld)\n", argv[2], argv[3], (long)Errno());
		goto out;
	}
	if (!strcmp(argv[1], "get")) {
		unsigned long size;
		char *buf;
		BPTR fh;
		snprintf(line, sizeof(line), "GET %s\n", argv[4]);
		sendall(s, line, strlen(line));
		if (recvall(s, (char *)&size, 4) < 0) { printf("tglbget: no reply\n"); goto out; }
		if (size == 0xFFFFFFFFul) { printf("tglbget: %s not on host\n", argv[4]); goto out; }
		buf = malloc(size + 1);
		if (!buf || recvall(s, buf, size) < 0) { printf("tglbget: short read\n"); goto out; }
		fh = Open((CONST_STRPTR)argv[5], MODE_NEWFILE);
		if (!fh) { printf("tglbget: cannot write %s\n", argv[5]); goto out; }
		Write(fh, buf, size);
		Close(fh);
		free(buf);
		printf("tglbget: %s -> %s (%lu bytes)\n", argv[4], argv[5], size);
		rc = 0;
	} else {
		BPTR fh = Open((CONST_STRPTR)argv[5], MODE_OLDFILE);
		long size;
		char *buf;
		if (!fh) { printf("tglbget: cannot read %s\n", argv[5]); goto out; }
		Seek(fh, 0, OFFSET_END);
		size = Seek(fh, 0, OFFSET_BEGINNING);
		buf = malloc(size + 1);
		if (!buf) { Close(fh); goto out; }
		Read(fh, buf, size);
		Close(fh);
		snprintf(line, sizeof(line), "PUT %s %ld\n", argv[4], size);
		sendall(s, line, strlen(line));
		sendall(s, buf, size);
		free(buf);
		if (recvall(s, line, 3) == 0 && !strncmp(line, "OK", 2)) { printf("tglbget: %s -> host %s (%ld bytes)\n", argv[5], argv[4], size); rc = 0; }
		else printf("tglbget: put not confirmed\n");
	}
out:
	if (s >= 0) CloseSocket(s);
	CloseLibrary(SocketBase);
	return rc;
}
