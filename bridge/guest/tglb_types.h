/* SPDX-License-Identifier: MIT */
/* TGLBridge guest: system + TinyGL type/constant headers in the right order.
 * The socket/dos/exec inline macros must be included BEFORE _NO_PPCINLINE is defined;
 * TinyGL's own inline macros must NOT be seen (we implement those functions). */
#ifndef TGLB_TYPES_H
#define TGLB_TYPES_H

#include <exec/types.h>
#include <exec/libraries.h>
#include <exec/resident.h>
#include <exec/memory.h>
#include <exec/execbase.h>
#include <dos/dos.h>
#include <dos/var.h>
#include <intuition/intuition.h>
#include <intuition/screens.h>
#include <graphics/rastport.h>
#include <utility/tagitem.h>
#include <cybergraphx/cybergraphics.h>
#include <emul/emulinterface.h>
#include <emul/emulregs.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/socket.h>
#include <proto/cybergraphics.h>
#include <sys/socket.h>
#include <netinet/in.h>

#define _NO_PPCINLINE
#include <tgl/gl.h>
#include <tgl/gla.h>

#endif
