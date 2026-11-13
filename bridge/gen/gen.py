#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""TGLBridge code generator.

Reads the MorphOS SDK's ppcinline/tinygl.h (every tinygl.library entry point with its exact
SysV prototype and LVO) and writes:
  guest/gen_slots.c   - one guest implementation per library slot + the function array
  guest/gen_manual.h  - prototypes the hand-written guest functions must match
  host/gen_dispatch.c - host decoder: slot opcode -> real OpenGL call
  host/gen_names.c    - slot -> name table (for logging)

Slot n sits at LVO 6*n (n=1..4 are Open/Close/Expunge/Reserved, GL functions start at n=5,
LVO -30). The inline header reads the function pointer at base-(LVO-2).

Classes:
  manual - written by hand in guest/tglb_manual.c (context, arrays, textures, readback, gets)
  auto   - all arguments scalar: serialised by value; scalar return => synchronous call
  fixv   - last argument is a const pointer to a fixed number of scalars (glColor4fv, glLoadMatrixf...)
  pnamev - (..., GLenum pname, const T *params): count depends on pname (glFogfv, glLightfv...)
  unimpl - anything else: the guest sends OP_UNIMPL(slot) once, the host logs the name once
"""
import re, sys, os

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
def find_header():
    """The SDK header is never part of this repository (MorphOS SDK licence): it is read from the
    SDK installed on the build machine. Order: argv[1], $MORPHOS_SDK_INCLUDE, $MORPHOS_CROSS,
    ~/.morphos-cross/current (the default install of metaneutrons/morphos-cross)."""
    if len(sys.argv) > 1:
        return sys.argv[1]
    cands = []
    if os.environ.get('MORPHOS_SDK_INCLUDE'):
        cands.append(os.path.join(os.environ['MORPHOS_SDK_INCLUDE'], 'ppcinline', 'tinygl.h'))
    if os.environ.get('MORPHOS_CROSS'):
        cands.append(os.path.join(os.environ['MORPHOS_CROSS'], 'os-include', 'ppcinline', 'tinygl.h'))
    cands.append(os.path.join(os.path.expanduser('~'), '.morphos-cross', 'current', 'os-include', 'ppcinline', 'tinygl.h'))
    for c in cands:
        if os.path.isfile(c):
            return c
    sys.exit('gen.py: MorphOS SDK header ppcinline/tinygl.h not found; pass its path as the first argument '
             'or set MORPHOS_SDK_INCLUDE (tried: %s)' % ', '.join(cands))

SRC = find_header()

text = open(SRC).read()
pat = re.compile(r'#define\s+(\w+)\(([^)]*)\)\s*\\\s*\(\(\((.*?)\(\*\)\((.*?)\)\)\*\(void\*\*\)\(\(long\)\(TINYGL_BASE_NAME\)\s*-\s*(\d+)\)\)', re.S)

funcs = {}
for m in pat.finditer(text):
    name, _, ret, args, off = m.group(1), m.group(2), m.group(3).strip(), m.group(4).strip(), int(m.group(5))
    lvo = off + 2
    assert lvo % 6 == 0, name
    slot = lvo // 6
    al = [a.strip() for a in args.split(',')] if args and args != 'void' else []
    funcs[slot] = dict(name=name, ret=ret, args=al, slot=slot)

NSLOTS = max(funcs) + 1

SCALAR = {
    'GLenum': ('u32', 'unsigned int'), 'GLint': ('s32', 'int'), 'GLuint': ('u32', 'unsigned int'),
    'GLsizei': ('s32', 'int'), 'GLfloat': ('f32', 'float'), 'GLclampf': ('f32', 'float'),
    'GLdouble': ('f64', 'double'), 'GLclampd': ('f64', 'double'), 'GLboolean': ('u32', 'unsigned char'),
    'GLbyte': ('s32', 'signed char'), 'GLubyte': ('u32', 'unsigned char'), 'GLshort': ('s32', 'short'),
    'GLushort': ('u32', 'unsigned short'), 'GLbitfield': ('u32', 'unsigned int'),
    'int': ('s32', 'int'), 'unsigned int': ('u32', 'unsigned int'), 'GLhandleARB': ('u32', 'unsigned int'),
    'GLintptr': ('s32', 'intptr_t'), 'GLsizeiptr': ('s32', 'intptr_t'), 'GLintptrARB': ('s32', 'intptr_t'),
    'GLsizeiptrARB': ('s32', 'intptr_t'), 'GLhalfNV': ('u32', 'unsigned short'), 'GLhalfARB': ('u32', 'unsigned short'),
    'GLuint64': ('u64', 'unsigned long long'), 'GLuint64EXT': ('u64', 'unsigned long long'),
    'GLint64': ('u64', 'long long'), 'GLchar': ('s32', 'char'),
}
PTRBASE = {  # const T * -> (wire kind, host type, guest element size)
    'GLfloat': ('f32', 'float', 4), 'GLdouble': ('f64', 'double', 8), 'GLint': ('s32', 'int', 4),
    'GLuint': ('u32', 'unsigned int', 4), 'GLshort': ('s16', 'short', 2), 'GLushort': ('u16', 'unsigned short', 2),
    'GLbyte': ('s8', 'signed char', 1), 'GLubyte': ('u8', 'unsigned char', 1), 'GLboolean': ('u8', 'unsigned char', 1),
    'GLclampf': ('f32', 'float', 4),
}

# Written by hand (guest/tglb_manual.c, host/tglhost.c). Keep in sync with both.
MANUAL = set('''
GLInit GLClose TGLGetContexts TGLGetProcAddress TGLEnableNewExtensions TGLSetContextVersion TGLGetContextVersion
GLAInitializeContext GLAInitializeContextWindowed GLAInitializeContextScreen GLAInitializeContextBitMap
GLAReinitializeContextWindowed GLADestroyContext GLADestroyContextWindowed GLADestroyContextScreen
GLADestroyContextBitMap GLASwapBuffers GLASetSync GLASetAttr
GLFlush GLFinish GLGetString GLGetIntegerv GLGetFloatv GLGetBooleanv GLGetDoublev
GLPixelStorei GLPixelStoref
GLTexImage2D GLTexSubImage2D GLTexImage1D GLTexSubImage1D GLDrawPixels GLReadPixels
GLGenTextures GLDeleteTextures
GLVertexPointer GLColorPointer GLTexCoordPointer GLNormalPointer
GLEnableClientState GLDisableClientState GLClientActiveTexture
GLDrawElements GLDrawArrays GLDrawRangeElements GLArrayElement
GLLockArraysEXT GLUnlockArraysEXT
'''.split())

FIXV = {'GLLoadMatrixf': 16, 'GLLoadMatrixd': 16, 'GLMultMatrixf': 16, 'GLMultMatrixd': 16,
        'GLLoadTransposeMatrixf': 16, 'GLLoadTransposeMatrixd': 16, 'GLMultTransposeMatrixf': 16,
        'GLMultTransposeMatrixd': 16, 'GLClipPlane': 4, 'GLEdgeFlagv': 1}
PNAMEV = set('''GLFogfv GLFogiv GLLightfv GLLightiv GLMaterialfv GLMaterialiv GLLightModelfv GLLightModeliv
GLTexEnvfv GLTexEnviv GLTexParameterfv GLTexParameteriv GLTexGenfv GLTexGeniv GLTexGendv GLPointParameterfv
GLPointParameteriv'''.split())

def strip_const(t):
    return t.replace('const ', '').strip()

def classify(f):
    n, a, r = f['name'], f['args'], f['ret']
    if n in MANUAL:
        return 'manual'
    if not a or a[0] != 'GLContext *':
        return 'unimpl_noctx'
    rest = a[1:]
    rs = r.strip()
    retok = rs == 'void' or rs in SCALAR
    if not retok:
        return 'unimpl'
    if all(t in SCALAR for t in rest):
        return 'auto'
    # pointer as the last argument
    if rest and rest[-1].endswith('*') and all(t in SCALAR for t in rest[:-1]) and rs == 'void':
        base = strip_const(rest[-1][:-1].strip())
        if base in PTRBASE and rest[-1].startswith('const'):
            if n in FIXV:
                return 'fixv'
            if n in PNAMEV:
                return 'pnamev'
            m = re.match(r'^GL\w*?([1-4])(b|s|i|f|d|ub|us|ui)v(ARB|EXT)?$', n)
            if m:
                return 'fixv'
    return 'unimpl'

def fixcount(n):
    if n in FIXV:
        return FIXV[n]
    return int(re.match(r'^GL\w*?([1-4])(b|s|i|f|d|ub|us|ui)v(ARB|EXT)?$', n).group(1))

def gl_name(n):
    if n.startswith('GL'):
        return 'gl' + n[2:]
    return n

cls = {s: classify(f) for s, f in funcs.items()}

# ---------------------------------------------------------------- guest
g = ['/* GENERATED by gen/gen.py from ppcinline/tinygl.h - do not edit */',
     '#include "tglb_internal.h"', '']
gm = ['/* GENERATED by gen/gen.py: prototypes of hand-written slots (must match the SDK ABI) */',
      '#ifndef GEN_MANUAL_H', '#define GEN_MANUAL_H', '#include "tglb_types.h"']
WIRESZ = {'u32': 4, 's32': 4, 'f32': 4, 'f64': 8, 'u64': 8}

def proto(f, prefix):
    args = ', '.join('%s a%d' % (t, i) for i, t in enumerate(f['args'])) or 'void'
    return '%s %s%s(%s)' % (f['ret'], prefix, f['name'], args)

for s in sorted(funcs):
    f = funcs[s]; c = cls[s]
    if c == 'manual':
        gm.append(proto(f, 'm_') + ';')
        continue
    if c.startswith('unimpl'):
        # stubs ignore their arguments: a SysV callee may take fewer parameters than passed
        g.append('static %s s_%s(%s)' % (f['ret'].strip(), f['name'],
                                         'void' if c == 'unimpl_noctx' else 'GLContext *a0'))
    else:
        g.append('static ' + proto(f, 's_'))
    g.append('{')
    rest = f['args'][1:]
    if c in ('auto',):
        size = sum(WIRESZ[SCALAR[t][0]] for t in rest)
        g.append('\tunsigned char *p = tglb_cmd(a0, %d, %d);' % (s, size))
        for i, t in enumerate(rest, 1):
            w = SCALAR[t][0]
            if w == 'f32':
                g.append('\tp = tglb_putf(p, a%d);' % i)
            elif w == 'f64':
                g.append('\tp = tglb_putd(p, a%d);' % i)
            elif w == 'u64':
                g.append('\tp = tglb_put64(p, (unsigned long long)a%d);' % i)
            elif w == 's32':
                g.append('\tp = tglb_put32(p, (unsigned int)(int)a%d);' % i)
            else:
                g.append('\tp = tglb_put32(p, (unsigned int)a%d);' % i)
        g.append('\t(void)p;')
        if f['ret'].strip() != 'void':
            g.append('\treturn (%s)tglb_sync_u32(a0);' % f['ret'])
    elif c in ('fixv', 'pnamev'):
        base = strip_const(rest[-1][:-1].strip())
        wk, _, esz = PTRBASE[base]
        scal = rest[:-1]
        ssize = sum(WIRESZ[SCALAR[t][0]] for t in scal)
        last = len(rest)
        if c == 'fixv':
            g.append('\tint n = %d;' % fixcount(f['name']))
        else:
            g.append('\tint n = tglb_pname_count(a%d);' % (last - 1))
        g.append('\tunsigned char *p = tglb_cmd(a0, %d, %d + 4 + ((n * %d + 3) & ~3));' % (s, ssize, esz))
        for i, t in enumerate(scal, 1):
            w = SCALAR[t][0]
            g.append('\tp = tglb_put32(p, (unsigned int)a%d);' % i if w in ('u32', 's32') else '\tp = tglb_putf(p, a%d);' % i)
        g.append('\tp = tglb_put32(p, (unsigned int)n);')
        g.append('\ttglb_memcpy(p, a%d, n * %d);' % (last, esz))
    else:
        if c == 'unimpl_noctx':
            g.append('\ttglb_unimpl(0, %d);' % s)
        else:
            g.append('\ttglb_unimpl(a0, %d);' % s)
        if f['ret'].strip() != 'void':
            g.append('\treturn (%s)0;' % f['ret'])
    g.append('}')
    g.append('')

g.append('static unsigned int s_private(void) { return 0; }')
g.append('')
g.append('const unsigned int tglb_nslots = %d;' % NSLOTS)
g.append('const APTR tglb_vectors[] = {')
g.append('\t(APTR)FUNCARRAY_BEGIN,')
g.append('\t(APTR)FUNCARRAY_32BIT_NATIVE,')
g.append('\t(APTR)tglb_LibOpen, (APTR)tglb_LibClose, (APTR)tglb_LibExpunge, (APTR)tglb_LibNull,')
g.append('\t(APTR)0xffffffff,')
g.append('\t(APTR)FUNCARRAY_32BIT_SYSTEMV,')
for s in range(5, NSLOTS):
    if s in funcs:
        f = funcs[s]
        g.append('\t(APTR)%s%s,' % ('m_' if cls[s] == 'manual' else 's_', f['name']))
    else:
        g.append('\t(APTR)s_private,')
g.append('\t(APTR)0xffffffff,')
g.append('\t(APTR)FUNCARRAY_END')
g.append('};')
g.append('')
# expected pointer per slot for the self-check in LibInit
g.append('const APTR tglb_expect[] = {')
for s in range(5, NSLOTS):
    if s in funcs:
        f = funcs[s]
        g.append('\t(APTR)%s%s,' % ('m_' if cls[s] == 'manual' else 's_', f['name']))
    else:
        g.append('\t(APTR)s_private,')
g.append('};')
gm.append('#endif')

open(os.path.join(ROOT, 'guest', 'gen_slots.c'), 'w', newline='\n').write('\n'.join(g) + '\n')
open(os.path.join(ROOT, 'guest', 'gen_manual.h'), 'w', newline='\n').write('\n'.join(gm) + '\n')

# ---------------------------------------------------------------- host
h = ['/* GENERATED by gen/gen.py - do not edit */', '#include "tglhost.h"', '']
h.append('const int g_nslots = %d;' % NSLOTS)
h.append('')
# names
names = ['/* GENERATED by gen/gen.py - do not edit */', '#include <stddef.h>',
         'const char *const g_slot_name[%d] = {' % NSLOTS]
for s in range(NSLOTS):
    names.append('\t"%s",' % (funcs[s]['name'] if s in funcs else ('private%d' % s)))
names.append('};')
names.append('const char g_slot_class[%d] = {' % NSLOTS)
for s in range(NSLOTS):
    names.append("\t'%s'," % ({'manual': 'M', 'auto': 'A', 'fixv': 'V', 'pnamev': 'P'}.get(cls.get(s), 'U')))
names.append('};')
open(os.path.join(ROOT, 'host', 'gen_names.c'), 'w', newline='\n').write('\n'.join(names) + '\n')

h.append('/* returns 1 if handled, 0 if the slot is not a generated one */')
h.append('int gen_dispatch(Client *cl, int slot, const unsigned char *p, unsigned int len)')
h.append('{')
h.append('\tswitch (slot) {')
for s in sorted(funcs):
    f = funcs[s]; c = cls[s]
    if c not in ('auto', 'fixv', 'pnamev'):
        continue
    rest = f['args'][1:]
    ret = f['ret'].strip()
    hret = 'void' if ret == 'void' else SCALAR[ret][1]
    h.append('\tcase %d: { /* %s */' % (s, f['name']))
    decl = []
    call = []
    if c == 'auto':
        types = [SCALAR[t][1] for t in rest]
        for i, t in enumerate(rest):
            w = SCALAR[t][0]
            ht = SCALAR[t][1]
            rd = {'u32': 'rd_u32', 's32': 'rd_s32', 'f32': 'rd_f32', 'f64': 'rd_f64', 'u64': 'rd_u64'}[w]
            h.append('\t\t%s a%d = (%s)%s(&p);' % (ht, i, ht, rd))
            call.append('a%d' % i)
    else:
        base = strip_const(rest[-1][:-1].strip())
        wk, ht_el, esz = PTRBASE[base]
        types = [SCALAR[t][1] for t in rest[:-1]] + ['const %s *' % ht_el]
        for i, t in enumerate(rest[:-1]):
            w = SCALAR[t][0]
            ht = SCALAR[t][1]
            rd = {'u32': 'rd_u32', 's32': 'rd_s32', 'f32': 'rd_f32'}[w]
            h.append('\t\t%s a%d = (%s)%s(&p);' % (ht, i, ht, rd))
            call.append('a%d' % i)
        h.append('\t\tint n = rd_s32(&p); %s v[16];' % ht_el)
        h.append('\t\tif (n > 16) n = 16;')
        h.append('\t\tunpack_array(v, p, n, %d);' % esz)
        call.append('v')
    tdef = '%s (APIENTRY *)(%s)' % (hret, ', '.join(types) or 'void')
    h.append('\t\tvoid *fp = resolve(cl, %d);' % s)
    h.append('\t\tif (!fp) { %s break; }' % ('reply_u32(cl, 0);' if ret != 'void' else ''))
    if ret == 'void':
        h.append('\t\t((%s)fp)(%s);' % (tdef, ', '.join(call)))
    else:
        h.append('\t\treply_u32(cl, (unsigned int)((%s)fp)(%s));' % (tdef, ', '.join(call)))
    h.append('\t\tbreak; }')
h.append('\tdefault: return 0;')
h.append('\t}')
h.append('\t(void)len;')
h.append('\treturn 1;')
h.append('}')
h.append('')
h.append('int gen_is_sync(int slot)')
h.append('{')
h.append('\tswitch (slot) {')
for s in sorted(funcs):
    f = funcs[s]
    if cls[s] == 'auto' and f['ret'].strip() != 'void':
        h.append('\tcase %d:' % s)
h.append('\t\treturn 1;')
h.append('\tdefault: return 0;')
h.append('\t}')
h.append('}')
open(os.path.join(ROOT, 'host', 'gen_dispatch.c'), 'w', newline='\n').write('\n'.join(h) + '\n')

# slot numbers of the manual functions, for both sides
sl = ['/* GENERATED by gen/gen.py - slot numbers (= wire opcodes) */', '#ifndef GEN_SLOTS_H', '#define GEN_SLOTS_H']
for s in sorted(funcs):
    sl.append('#define SLOT_%s %d' % (funcs[s]['name'], s))
sl.append('#define TGLB_NSLOTS %d' % NSLOTS)
sl.append('#endif')
txt = '\n'.join(sl) + '\n'
open(os.path.join(ROOT, 'guest', 'gen_slotnums.h'), 'w', newline='\n').write(txt)
open(os.path.join(ROOT, 'host', 'gen_slotnums.h'), 'w', newline='\n').write(txt)

from collections import Counter
cnt = Counter(cls.values())
print('slots', NSLOTS, dict(cnt))
missing = [n for n in MANUAL if n not in {f['name'] for f in funcs.values()}]
if missing:
    print('MANUAL names not in header:', missing)
