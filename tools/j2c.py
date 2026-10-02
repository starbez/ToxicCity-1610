#!/usr/bin/env python3
"""
j2c.py - static recompiler: J2ME/CLDC class files (JAR) -> C.

    python3 tools/j2c.py --jar game.jar --out gen --midlet GloftSPDN

Generated files (all derived from the user's own JAR; never commit them):
    gen/jgame.h        class structs, selectors, prototypes
    gen/jrt_api.h      prototypes of every runtime (J2ME API) function the game calls
    gen/jclass_<X>.c   one file per Java class
    gen/jstrings.c     string literal table
    gen/jentry.c       MIDlet entry points

Model
  * Operand stack slots become C locals named s<Kind><depth> (kinds I J F D A).
  * Java locals become l<Kind><index>, one C variable per (kind,index) pair.
  * Control flow is gotos to L<pc> labels. Exceptions use setjmp frames, but
    only in methods that actually contain handlers.
  * Virtual calls use a global selector table; calls that cannot be
    overridden anywhere in the (closed-world) jar are devirtualised.
  * Calls into the J2ME/CLDC API become calls to rt_<class>_<method>__<params>
    which are implemented by hand in runtime/.
"""
import argparse, os, struct, sys, zipfile, collections

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from classfile import parse, cname, nat, ops, OPN, mutf8_units  # noqa: E402

CT = {'I': 'int32_t', 'J': 'int64_t', 'F': 'float', 'D': 'double', 'A': 'JObj *'}

# Runtime classes that game classes may extend, and their C layouts.
EXT_STRUCT = {
    'java/lang/Object': 'JObj',
    'javax/microedition/lcdui/Canvas': 'rt_Canvas',
    'javax/microedition/midlet/MIDlet': 'rt_MIDlet',
}
# Where inherited members of runtime classes are declared (for resolving
# calls such as k.repaint() whose constant-pool class is the app subclass).
EXT_SUPER = {
    'javax/microedition/lcdui/Canvas': 'javax/microedition/lcdui/Displayable',
    'javax/microedition/lcdui/Displayable': 'java/lang/Object',
    'javax/microedition/midlet/MIDlet': 'java/lang/Object',
}
EXT_MEMBERS = {
    'javax/microedition/lcdui/Canvas': {'<init>', 'repaint', 'serviceRepaints',
                                       'setFullScreenMode', 'getWidth', 'getHeight',
                                       'isShown'},
    'javax/microedition/lcdui/Displayable': {'addCommand', 'removeCommand',
                                            'setCommandListener', 'getWidth', 'getHeight'},
    'javax/microedition/midlet/MIDlet': {'<init>', 'getAppProperty', 'notifyDestroyed',
                                        'platformRequest', 'notifyPaused'},
    'java/lang/Object': {'<init>', 'getClass', 'hashCode', 'equals', 'toString'},
}


def kind_of(fd):
    c = fd[0]
    if c in 'ZBCSI': return 'I'
    if c == 'J': return 'J'
    if c == 'F': return 'F'
    if c == 'D': return 'D'
    if c in 'L[': return 'A'
    if c == 'V': return None
    raise ValueError(fd)


def storage_ctype(fd):
    return {'Z': 'int8_t', 'B': 'int8_t', 'C': 'uint16_t', 'S': 'int16_t', 'I': 'int32_t',
            'J': 'int64_t', 'F': 'float', 'D': 'double'}.get(fd[0], 'JObj *')


def split_desc(desc):
    """'(I[BLjava/lang/String;)V' -> (['I','[B','Ljava/lang/String;'], 'V')"""
    assert desc[0] == '('
    i = 1; params = []
    while desc[i] != ')':
        j = i
        while desc[j] == '[': j += 1
        if desc[j] == 'L': j = desc.index(';', j)
        params.append(desc[i:j + 1]); i = j + 1
    return params, desc[i + 1:]


def mangle_params(desc):
    ps, _ = split_desc(desc)
    out = []
    for p in ps:
        n = p.count('[', 0, len(p))
        base = p[n:]
        if base[0] == 'L': base = 'L' + base[1:-1].replace('/', '_')
        out.append('A' * n + base)
    return '_'.join(out)


def rt_fn(cls, name, desc):
    return 'rt_%s_%s__%s' % (cls.replace('/', '_'), 'init' if name == '<init>' else name,
                             mangle_params(desc))


def i32(n):
    return '(-2147483647-1)' if n == -2 ** 31 else ('(%d)' % n)


def i64(n):
    return '(-INT64_C(9223372036854775807)-1)' if n == -2 ** 63 else 'INT64_C(%d)' % n


def flit(v, single):
    import math
    if v != v: return 'NAN'
    if math.isinf(v): return 'INFINITY' if v > 0 else '(-INFINITY)'
    s = float(v).hex()
    return '(%s%s)' % (s, 'f' if single else '')


# ---------------------------------------------------------------------------
class Method:
    pass


class Cls:
    pass


class Translator:
    def __init__(self, jar, midlet):
        self.midlet = midlet
        self.app = collections.OrderedDict()
        z = zipfile.ZipFile(jar)
        for n in sorted(z.namelist()):
            if n.endswith('.class'):
                self.load(z.read(n), n[:-6])
        self.str_ids = collections.OrderedDict()
        self.ext_funcs = collections.OrderedDict()   # name -> prototype
        self.ext_statics = collections.OrderedDict()
        self.ext_new = set()
        self.warnings = []
        self.stats = collections.Counter()
        self.build_model()

    # ----------------------------------------------------------- model
    def load(self, data, expect):
        tmp = '/tmp/_j2c_cls.bin'
        open(tmp, 'wb').write(data)
        c = parse(tmp)
        k = Cls()
        k.c = c
        k.name = cname(c, c['this'])
        k.super_name = cname(c, c['super'])
        k.iface_names = [cname(c, i) for i in c['ifaces']]
        k.is_iface = bool(c['access'] & 0x200)
        k.is_abstract = bool(c['access'] & 0x400)
        k.fields = []
        for i, f in enumerate(c['fields']):
            f['idx'] = i
            f['static'] = bool(f['acc'] & 8)
            k.fields.append(f)
        k.methods = []
        for i, m in enumerate(c['methods']):
            mm = Method()
            mm.__dict__.update(m)
            mm.idx = i; mm.cls = k
            mm.static = bool(m['acc'] & 8)
            mm.private = bool(m['acc'] & 2)
            mm.params, mm.ret = split_desc(m['desc'])
            mm.has_code = 'code' in m
            k.methods.append(mm)
        self.app[k.name] = k

    def build_model(self):
        for k in self.app.values():
            k.clinit = next((m for m in k.methods if m.name == '<clinit>'), None)
        # selectors
        sels = set()
        for k in self.app.values():
            for m in k.methods:
                if not m.static and m.name not in ('<init>', '<clinit>') and not m.private:
                    sels.add((m.name, m.desc))
        self.sel = {s: i for i, s in enumerate(sorted(sels))}
        # vtables
        self.vt = {}
        done = set()

        def build(k):
            if k.name in done: return
            done.add(k.name)
            vt = {}
            sup = self.app.get(k.super_name)
            if sup is not None:
                build(sup); vt.update(self.vt[sup.name])
            if not k.is_iface:
                for m in k.methods:
                    if m.static or m.name in ('<init>', '<clinit>') or m.private: continue
                    if m.has_code: vt[self.sel[(m.name, m.desc)]] = m
            self.vt[k.name] = vt
        for k in self.app.values(): build(k)

    def subclasses_of(self, name):
        out = []
        for k in self.app.values():
            cur = k
            while cur is not None:
                if cur.name == name and k.name != name:
                    out.append(k); break
                cur = self.app.get(cur.super_name)
        return out

    def needs_init(self, k):
        while k is not None:
            if k.clinit is not None: return True
            k = self.app.get(k.super_name)
        return False

    # ------------------------------------------------------ resolution
    def ext_decl(self, e, name):
        cur = e
        while cur is not None:
            if cur in EXT_MEMBERS and name in EXT_MEMBERS[cur]: return cur
            cur = EXT_SUPER.get(cur)
        return e

    def resolve_method(self, cname_, name, desc):
        """-> ('app', Cls, Method) | ('ext', classname)"""
        seen = set()

        def in_ifaces(k):
            for i in k.iface_names:
                ik = self.app.get(i)
                if ik is None or ik.name in seen: continue
                seen.add(ik.name)
                for m in ik.methods:
                    if m.name == name and m.desc == desc: return m
                r = in_ifaces(ik)
                if r: return r
            return None
        cur = cname_
        while cur in self.app:
            k = self.app[cur]
            for m in k.methods:
                if m.name == name and m.desc == desc: return ('app', k, m)
            r = in_ifaces(k)
            if r: return ('app', r.cls, r)
            cur = k.super_name
        return ('ext', self.ext_decl(cur, name))

    def resolve_field(self, cname_, name, desc):
        def search(cn):
            k = self.app.get(cn)
            if k is None: return None
            for f in k.fields:
                if f['name'] == name and f['desc'] == desc: return (k, f)
            for i in k.iface_names:
                r = search(i)
                if r: return r
            return search(k.super_name)
        return search(cname_)

    # --------------------------------------------------- string table
    def str_id(self, units):
        t = tuple(units)
        if t not in self.str_ids: self.str_ids[t] = len(self.str_ids)
        return self.str_ids[t]

    # ------------------------------------------------ extern bookkeeping
    def want_ext(self, cls, name, desc, static, ctor=False):
        fn = rt_fn(cls, name, desc)
        if fn not in self.ext_funcs:
            ps, r = split_desc(desc)
            args = [] if static else ['JObj *self']
            args += [CT[kind_of(p)] for p in ps]
            ret = 'void' if r == 'V' else CT[kind_of(r)]
            self.ext_funcs[fn] = '%s %s(%s);' % (ret, fn, ', '.join(args) or 'void')
        return fn

    # ------------------------------------------------------ analysis
    def gen_method(self, k, m):
        return MethodGen(self, k, m).run()

    # ---------------------------------------------------- file output
    def emit_all(self, out, midlet):
        os.makedirs(out, exist_ok=True)
        bodies = {}
        for k in self.app.values():
            parts = []
            for m in k.methods:
                if m.has_code:
                    parts.append(self.gen_method(k, m))
            bodies[k.name] = parts
        self.write_header(out)
        for k in self.app.values():
            self.write_class(out, k, bodies[k.name])
        self.write_strings(out)
        self.write_entry(out, midlet)
        self.write_rt_api(out)

    def cdecl_method(self, m):
        ret = 'void' if m.ret == 'V' else CT[kind_of(m.ret)]
        args = [] if m.static else ['JObj *']
        args += [CT[kind_of(p)] for p in m.params]
        return '%s J_%s_m%d(%s)' % (ret, m.cls.name, m.idx, ', '.join(args) or 'void')

    def write_header(self, out):
        L = ['/* GENERATED by j2c.py - do not edit, do not commit */',
             '#ifndef JGAME_H', '#define JGAME_H', '#include "jvm.h"', '']
        L.append('#define NSEL %d' % max(1, len(self.sel)))
        for (n, d), i in sorted(self.sel.items(), key=lambda t: t[1]):
            L.append('#define SEL_%d %d /* %s%s */' % (i, i, n, d))
        L.append('')
        L.append('#define JVM_CLINIT(c) do { if (JVM_UNLIKELY((c).init_state != 2)) jvm_class_init(&(c)); } while (0)')
        L.append('')
        order = []; seen = set()

        def visit(k):
            if k.name in seen: return
            seen.add(k.name)
            s = self.app.get(k.super_name)
            if s: visit(s)
            order.append(k)
        for k in self.app.values(): visit(k)
        for k in order:
            L.append('typedef struct J_%s J_%s;' % (k.name, k.name))
        for k in order:
            sup = self.app.get(k.super_name)
            if sup: base = 'J_%s' % sup.name
            else:
                base = EXT_STRUCT.get(k.super_name)
                if base is None:
                    raise SystemExit('class %s extends unsupported runtime class %s' % (k.name, k.super_name))
            L.append('struct J_%s {' % k.name)
            L.append('    %s base;' % base)
            for f in k.fields:
                if not f['static']:
                    L.append('    %s f%d; /* %s %s */' % (storage_ctype(f['desc']), f['idx'], f['name'], f['desc']))
            L.append('};')
        L.append('')
        for k in order:
            L.append('extern JClass J_%s_class;' % k.name)
            for f in k.fields:
                if f['static']:
                    L.append('extern %s J_%s_s%d; /* %s %s */' % (storage_ctype(f['desc']), k.name, f['idx'], f['name'], f['desc']))
            for m in k.methods:
                if m.has_code and m.name != '<clinit>':
                    L.append('%s; /* %s%s */' % (self.cdecl_method(m), m.name, m.desc))
        L.append('')
        L.append('#include "jrt_api.h"')
        L.append('#endif')
        open(os.path.join(out, 'jgame.h'), 'w').write('\n'.join(L) + '\n')

    def write_class(self, out, k, bodies):
        L = ['/* GENERATED by j2c.py - class %s (%s) */' % (k.name, 'interface' if k.is_iface else 'class'),
             '#include "jgame.h"', '']
        for f in k.fields:
            if f['static']:
                L.append('%s J_%s_s%d;' % (storage_ctype(f['desc']), k.name, f['idx']))
        L.append('')
        for b in bodies: L.append(b)
        if k.clinit is not None:
            L.append('static void J_%s_clinit_tramp(void) { J_%s_m%d(); }' % (k.name, k.name, k.clinit.idx))
        # reflective method table (own virtual methods with code)
        own = [m for m in k.methods if m.has_code and not m.static and m.name not in ('<init>', '<clinit>')]
        if own:
            L.append('static const JMethodInfo J_%s_methods[] = {' % k.name)
            for m in own:
                L.append('    { "%s", "%s", (void *)J_%s_m%d },' % (m.name, m.desc, k.name, m.idx))
            L.append('};')
        ifs = [i for i in k.iface_names if i in self.app]
        if ifs:
            L.append('static JClass *const J_%s_ifaces[] = { %s };' % (k.name, ', '.join('&J_%s_class' % i for i in ifs)))
        vt = self.vt[k.name]
        if vt:
            L.append('static void *const J_%s_vt[NSEL] = {' % k.name)
            for s, m in sorted(vt.items()):
                L.append('    [SEL_%d] = (void *)J_%s_m%d,' % (s, m.cls.name, m.idx))
            L.append('};')
        eifs = [i for i in k.iface_names if i not in self.app]
        if eifs:
            L.append('static const char *const J_%s_eifaces[] = { %s };' % (k.name, ', '.join('"%s"' % i for i in eifs)))
        sup = self.app.get(k.super_name)
        flags = (1 if k.is_iface else 0) | (2 if k.is_abstract else 0)
        L.append('JClass J_%s_class = {' % k.name)
        L.append('    .name = "%s",' % k.name)
        if sup: L.append('    .super = &J_%s_class,' % sup.name)
        else: L.append('    .ext_super = "%s",' % k.super_name)
        if ifs: L.append('    .ifaces = J_%s_ifaces, .nifaces = %d,' % (k.name, len(ifs)))
        if eifs: L.append('    .ext_ifaces = J_%s_eifaces, .next_ifaces = %d,' % (k.name, len(eifs)))
        L.append('    .size = sizeof(J_%s), .flags = %d,' % (k.name, flags))
        if vt: L.append('    .vtable = J_%s_vt, .nvt = NSEL,' % k.name)
        if own: L.append('    .methods = J_%s_methods, .nmethods = %d,' % (k.name, len(own)))
        if k.clinit is not None: L.append('    .clinit = J_%s_clinit_tramp,' % k.name)
        L.append('};')
        open(os.path.join(out, 'jclass_%s.c' % k.name), 'w').write('\n'.join(L) + '\n')

    def write_strings(self, out):
        L = ['/* GENERATED by j2c.py - string literals (UTF-16) */', '#include "jvm.h"', '']
        for t, i in self.str_ids.items():
            L.append('static const uint16_t S%d[] = { %s };' % (i, ','.join(str(u) for u in t) if t else '0'))
        L.append('const JStrLit jvm_str_table[] = {')
        for t, i in self.str_ids.items():
            L.append('    { %d, S%d },' % (len(t), i))
        if not self.str_ids: L.append('    { 0, 0 },')
        L.append('};')
        L.append('const int jvm_str_count = %d;' % len(self.str_ids))
        open(os.path.join(out, 'jstrings.c'), 'w').write('\n'.join(L) + '\n')

    def write_entry(self, out, midlet):
        k = self.app[midlet]
        ctor = next(m for m in k.methods if m.name == '<init>' and m.desc == '()V')
        L = ['/* GENERATED by j2c.py */', '#include "jgame.h"',
             'JClass *const jgame_midlet_class = &J_%s_class;' % midlet,
             'JObj *jgame_new_midlet(void) {',
             '    JObj *o = jvm_new(&J_%s_class);' % midlet,
             '    J_%s_m%d(o);' % (midlet, ctor.idx),
             '    return o;', '}']
        open(os.path.join(out, 'jentry.c'), 'w').write('\n'.join(L) + '\n')

    def write_rt_api(self, out):
        L = ['/* GENERATED by j2c.py - runtime API required by the game */',
             '#ifndef JRT_API_H', '#define JRT_API_H', '#include "jvm.h"', '']
        for n in sorted(self.ext_new):
            L.append('JObj *rt_new_%s(void);' % n)
        for n, p in self.ext_statics.items(): L.append(p)
        for n, p in self.ext_funcs.items(): L.append(p)
        L.append('#endif')
        open(os.path.join(out, 'jrt_api.h'), 'w').write('\n'.join(L) + '\n')


# ---------------------------------------------------------------------------
IBIN = {'iadd': '(int32_t)((uint32_t)({a})+(uint32_t)({b}))',
        'isub': '(int32_t)((uint32_t)({a})-(uint32_t)({b}))',
        'imul': '(int32_t)((uint32_t)({a})*(uint32_t)({b}))',
        'idiv': 'jvm_idiv({a},{b})', 'irem': 'jvm_irem({a},{b})',
        'ishl': '(int32_t)((uint32_t)({a})<<(({b})&31))',
        'ishr': '(({a})>>(({b})&31))',
        'iushr': '(int32_t)((uint32_t)({a})>>(({b})&31))',
        'iand': '(({a})&({b}))', 'ior': '(({a})|({b}))', 'ixor': '(({a})^({b}))'}
LBIN = {'ladd': '(int64_t)((uint64_t)({a})+(uint64_t)({b}))',
        'lsub': '(int64_t)((uint64_t)({a})-(uint64_t)({b}))',
        'lmul': '(int64_t)((uint64_t)({a})*(uint64_t)({b}))',
        'ldiv': 'jvm_ldiv({a},{b})', 'lrem': 'jvm_lrem({a},{b})',
        'land': '(({a})&({b}))', 'lor': '(({a})|({b}))', 'lxor': '(({a})^({b}))'}
LSHIFT = {'lshl': '(int64_t)((uint64_t)({a})<<(({b})&63))',
          'lshr': '(({a})>>(({b})&63))',
          'lushr': '(int64_t)((uint64_t)({a})>>(({b})&63))'}
FBIN = {'fadd': '(({a})+({b}))', 'fsub': '(({a})-({b}))', 'fmul': '(({a})*({b}))',
        'fdiv': '(({a})/({b}))', 'frem': 'fmodf({a},{b})'}
DBIN = {'dadd': '(({a})+({b}))', 'dsub': '(({a})-({b}))', 'dmul': '(({a})*({b}))',
        'ddiv': '(({a})/({b}))', 'drem': 'fmod({a},{b})'}
CONV = {'i2l': ('I', 'J', '(int64_t)({a})'), 'i2f': ('I', 'F', '(float)({a})'),
        'i2d': ('I', 'D', '(double)({a})'), 'l2i': ('J', 'I', '(int32_t)({a})'),
        'l2f': ('J', 'F', '(float)({a})'), 'l2d': ('J', 'D', '(double)({a})'),
        'f2i': ('F', 'I', 'jvm_f2i({a})'), 'f2l': ('F', 'J', 'jvm_f2l({a})'),
        'f2d': ('F', 'D', '(double)({a})'), 'd2i': ('D', 'I', 'jvm_d2i({a})'),
        'd2l': ('D', 'J', 'jvm_d2l({a})'), 'd2f': ('D', 'F', '(float)({a})'),
        'i2b': ('I', 'I', '(int32_t)(int8_t)({a})'), 'i2c': ('I', 'I', '(int32_t)(uint16_t)({a})'),
        'i2s': ('I', 'I', '(int32_t)(int16_t)({a})')}
IFCOND = {'ifeq': '==', 'ifne': '!=', 'iflt': '<', 'ifge': '>=', 'ifgt': '>', 'ifle': '<='}
ICMP = {'if_icmpeq': '==', 'if_icmpne': '!=', 'if_icmplt': '<', 'if_icmpge': '>=',
        'if_icmpgt': '>', 'if_icmple': '<='}
ARR_LOAD = {'iaload': ('I', 'I'), 'laload': ('J', 'J'), 'faload': ('F', 'F'), 'daload': ('D', 'D'),
            'aaload': ('A', 'A'), 'baload': ('B', 'I'), 'caload': ('C', 'I'), 'saload': ('S', 'I')}
ARR_STORE = {'iastore': ('I', 'I'), 'lastore': ('J', 'J'), 'fastore': ('F', 'F'), 'dastore': ('D', 'D'),
             'aastore': ('A', 'A'), 'bastore': ('B', 'I'), 'castore': ('C', 'I'), 'sastore': ('S', 'I')}
LOADOPS = {'iload': 'I', 'lload': 'J', 'fload': 'F', 'dload': 'D', 'aload': 'A'}
STOREOPS = {'istore': 'I', 'lstore': 'J', 'fstore': 'F', 'dstore': 'D', 'astore': 'A'}


class MethodGen:
    def __init__(self, T, k, m):
        self.T, self.k, self.m = T, k, m
        self.code = m.code
        self.c = k.c
        self.used_s = set(); self.used_l = set()
        self.has_try = bool(m.exc)

    # -------- naming
    def S(self, kind, d):
        self.used_s.add((kind, d)); return 's%s%d' % (kind, d)

    def Lv(self, kind, n):
        self.used_l.add((kind, n)); return 'l%s%d' % (kind, n)

    def ret_stmt(self, v=None):
        pre = 'jvm_try_top = _tf.prev; ' if self.has_try else ''
        if v is None: return '{ %sreturn; }' % pre
        return '{ %sreturn %s; }' % (pre, v)

    # -------- stepping one instruction
    def step(self, pc, stack):
        code = self.code; c = self.c
        st = list(stack)
        L = []
        targets = []; falls = True
        op = code[pc]; name = OPN[op]
        rd2 = lambda p: struct.unpack_from('>H', code, p)[0]
        rs2 = lambda p: struct.unpack_from('>h', code, p)[0]
        rs4 = lambda p: struct.unpack_from('>i', code, p)[0]

        def pop():
            k = st.pop(); return k, self.S(k, len(st))

        def push(k):
            n = self.S(k, len(st)); st.append(k); return n

        def width(k): return 2 if k in 'JD' else 1

        if name == 'nop': pass
        elif name == 'aconst_null': L.append('%s = NULL;' % push('A'))
        elif name.startswith('iconst_'):
            v = -1 if name == 'iconst_m1' else int(name[-1]); L.append('%s = %d;' % (push('I'), v))
        elif name.startswith('lconst_'): L.append('%s = %s;' % (push('J'), name[-1]))
        elif name.startswith('fconst_'): L.append('%s = %s.0f;' % (push('F'), name[-1]))
        elif name.startswith('dconst_'): L.append('%s = %s.0;' % (push('D'), name[-1]))
        elif name == 'bipush': L.append('%s = %d;' % (push('I'), struct.unpack_from('>b', code, pc + 1)[0]))
        elif name == 'sipush': L.append('%s = %d;' % (push('I'), rs2(pc + 1)))
        elif name in ('ldc', 'ldc_w', 'ldc2_w'):
            idx = code[pc + 1] if name == 'ldc' else rd2(pc + 1)
            e = c['cp'][idx]
            if e[0] == 'int': L.append('%s = %s;' % (push('I'), i32(e[1])))
            elif e[0] == 'float': L.append('%s = %s;' % (push('F'), flit(e[1], True)))
            elif e[0] == 'long': L.append('%s = %s;' % (push('J'), i64(e[1])))
            elif e[0] == 'double': L.append('%s = %s;' % (push('D'), flit(e[1], False)))
            elif e[0] == 'string':
                u = c['cp'][e[1]]
                L.append('%s = jvm_str_lit(%d);' % (push('A'), self.T.str_id(mutf8_units(u[2]))))
            else: raise SystemExit('ldc of %s unsupported' % e[0])
        elif name in LOADOPS or name[:-2] in LOADOPS and name[-2] == '_':
            if name in LOADOPS:
                n = code[pc + 1]; k = LOADOPS[name]
            else:
                k = LOADOPS[name[:-2]]; n = int(name[-1])
            L.append('%s = %s;' % (push(k), self.Lv(k, n)))
        elif name in STOREOPS or name[:-2] in STOREOPS and name[-2] == '_':
            if name in STOREOPS:
                n = code[pc + 1]; k = STOREOPS[name]
            else:
                k = STOREOPS[name[:-2]]; n = int(name[-1])
            _, v = pop(); L.append('%s = %s;' % (self.Lv(k, n), v))
        elif name in ARR_LOAD:
            suf, k = ARR_LOAD[name]
            _, i = pop(); _, a = pop()
            L.append('%s = jaload_%s(%s, %s);' % (push(k), suf, a, i))
        elif name in ARR_STORE:
            suf, k = ARR_STORE[name]
            _, v = pop(); _, i = pop(); _, a = pop()
            L.append('jastore_%s(%s, %s, %s);' % (suf, a, i, v))
        elif name == 'pop': pop()
        elif name == 'pop2':
            k, _ = pop()
            if width(k) == 1: pop()
        elif name in ('dup', 'dup_x1', 'dup_x2', 'dup2', 'dup2_x1', 'dup2_x2'):
            n = 2 if name.startswith('dup2') else 1
            x = {'dup': 0, 'dup_x1': 1, 'dup_x2': 2, 'dup2': 0, 'dup2_x1': 1, 'dup2_x2': 2}[name]
            A = []; w = 0
            while w < n:
                e = st[-1]; nm = self.S(e, len(st) - 1); st.pop(); A.insert(0, (e, nm)); w += width(e)
            B = []; w = 0
            while w < x:
                e = st[-1]; nm = self.S(e, len(st) - 1); st.pop(); B.insert(0, (e, nm)); w += width(e)
            # snapshot into temps so overlapping moves are safe
            temps = []
            for j, (e, nm) in enumerate(A + B):
                t = '_t%d%s' % (j, e); self.used_l.add(('T' + e, j))
                L.append('%s = %s;' % (t, nm)); temps.append((e, t))
            Ta = temps[:len(A)]; Tb = temps[len(A):]
            for e, t in Ta + Tb + Ta:
                L.append('%s = %s;' % (push(e), t))
        elif name == 'swap':
            ka, a = pop(); kb, b = pop()
            L.append('{ %s _sw = %s; %s = %s; %s = _sw; }' % (CT[kb], b, b, a, a))
            # after swap: positions hold swapped values
            st.append(ka); st.append(kb)
            # names: b slot (lower) gets a's value of kind ka; fix kinds
            st[-2], st[-1] = ka, kb
            # (swap on mixed kinds never occurs in cat-1 code; both are cat-1)
            if ka != kb:
                L.pop()
                lo = self.S(ka, len(st) - 2); hi = self.S(kb, len(st) - 1)
                L.append('%s = %s; %s = %s;' % (lo, a, hi, b))
        elif name in IBIN:
            _, b = pop(); _, a = pop(); L.append('%s = %s;' % (push('I'), IBIN[name].format(a=a, b=b)))
        elif name in LBIN:
            _, b = pop(); _, a = pop(); L.append('%s = %s;' % (push('J'), LBIN[name].format(a=a, b=b)))
        elif name in LSHIFT:
            _, b = pop(); _, a = pop(); L.append('%s = %s;' % (push('J'), LSHIFT[name].format(a=a, b=b)))
        elif name in FBIN:
            _, b = pop(); _, a = pop(); L.append('%s = %s;' % (push('F'), FBIN[name].format(a=a, b=b)))
        elif name in DBIN:
            _, b = pop(); _, a = pop(); L.append('%s = %s;' % (push('D'), DBIN[name].format(a=a, b=b)))
        elif name in ('ineg', 'lneg', 'fneg', 'dneg'):
            k, a = pop()
            ex = {'I': '(int32_t)(0u-(uint32_t)(%s))', 'J': '(int64_t)(0ull-(uint64_t)(%s))',
                  'F': '(-(%s))', 'D': '(-(%s))'}[k] % a
            L.append('%s = %s;' % (push(k), ex))
        elif name == 'iinc':
            n = code[pc + 1]; d = struct.unpack_from('>b', code, pc + 2)[0]
            v = self.Lv('I', n); L.append('%s = (int32_t)((uint32_t)%s + (uint32_t)(%d));' % (v, v, d))
        elif name in CONV:
            kf, kt, ex = CONV[name]
            _, a = pop(); L.append('%s = %s;' % (push(kt), ex.format(a=a)))
        elif name == 'lcmp':
            _, b = pop(); _, a = pop(); L.append('%s = jvm_lcmp(%s, %s);' % (push('I'), a, b))
        elif name in ('fcmpl', 'fcmpg', 'dcmpl', 'dcmpg'):
            _, b = pop(); _, a = pop()
            fn = 'jvm_fcmpl' if name.endswith('l') else 'jvm_fcmpg'
            L.append('%s = %s(%s, %s);' % (push('I'), fn, a, b))
        elif name in IFCOND:
            _, a = pop(); t = pc + rs2(pc + 1); targets.append(t)
            L.append('if (%s %s 0) goto L%d;' % (a, IFCOND[name], t))
        elif name in ICMP:
            _, b = pop(); _, a = pop(); t = pc + rs2(pc + 1); targets.append(t)
            L.append('if (%s %s %s) goto L%d;' % (a, ICMP[name], b, t))
        elif name in ('if_acmpeq', 'if_acmpne'):
            _, b = pop(); _, a = pop(); t = pc + rs2(pc + 1); targets.append(t)
            L.append('if (%s %s %s) goto L%d;' % (a, '==' if name == 'if_acmpeq' else '!=', b, t))
        elif name in ('ifnull', 'ifnonnull'):
            _, a = pop(); t = pc + rs2(pc + 1); targets.append(t)
            L.append('if (%s%s) goto L%d;' % ('!' if name == 'ifnull' else '', a, t))
        elif name == 'goto':
            t = pc + rs2(pc + 1); targets.append(t); falls = False; L.append('goto L%d;' % t)
        elif name == 'tableswitch':
            _, a = pop(); p = (pc + 4) & ~3
            dflt = pc + rs4(p); lo = rs4(p + 4); hi = rs4(p + 8)
            L.append('switch (%s) {' % a)
            for i in range(hi - lo + 1):
                t = pc + rs4(p + 12 + 4 * i); targets.append(t); L.append('  case %d: goto L%d;' % (lo + i, t))
            targets.append(dflt); L.append('  default: goto L%d;' % dflt); L.append('}'); falls = False
        elif name == 'lookupswitch':
            _, a = pop(); p = (pc + 4) & ~3
            dflt = pc + rs4(p); n = rs4(p + 4)
            L.append('switch (%s) {' % a)
            for i in range(n):
                mv = rs4(p + 8 + 8 * i); t = pc + rs4(p + 12 + 8 * i); targets.append(t)
                L.append('  case %d: goto L%d;' % (mv, t))
            targets.append(dflt); L.append('  default: goto L%d;' % dflt); L.append('}'); falls = False
        elif name in ('ireturn', 'lreturn', 'freturn', 'dreturn', 'areturn'):
            _, v = pop(); L.append(self.ret_stmt(v)); falls = False
        elif name == 'return': L.append(self.ret_stmt()); falls = False
        elif name in ('getstatic', 'putstatic', 'getfield', 'putfield'):
            self.field_op(name, rd2(pc + 1), st, L, pop, push)
        elif name in ('invokevirtual', 'invokespecial', 'invokestatic', 'invokeinterface'):
            self.invoke(name, rd2(pc + 1), st, L, pop, push)
        elif name == 'new':
            cn = cname(c, rd2(pc + 1)); k = self.T.app.get(cn)
            if k:
                if self.T.needs_init(k) and not self.clinit_ok(k): L.append('JVM_CLINIT(J_%s_class);' % cn)
                L.append('%s = jvm_new(&J_%s_class);' % (push('A'), cn))
            else:
                self.T.ext_new.add(cn.replace('/', '_'))
                L.append('%s = rt_new_%s();' % (push('A'), cn.replace('/', '_')))
        elif name == 'newarray':
            _, n = pop(); L.append('%s = jvm_newarray(%d, %s);' % (push('A'), code[pc + 1], n))
        elif name == 'anewarray':
            cn = cname(c, rd2(pc + 1)); ed = cn if cn.startswith('[') else 'L%s;' % cn
            _, n = pop(); L.append('%s = jvm_newarray_ref(%s, "%s");' % (push('A'), n, ed))
        elif name == 'multianewarray':
            cn = cname(c, rd2(pc + 1)); nd = code[pc + 3]
            ds = []
            for _ in range(nd): ds.insert(0, pop()[1])
            L.append('%s = jvm_multianewarray("%s", %d, (int32_t[]){%s});' % (push('A'), cn, nd, ', '.join(ds)))
        elif name == 'arraylength':
            _, a = pop(); L.append('%s = jvm_arraylength(%s);' % (push('I'), a))
        elif name == 'athrow':
            _, a = pop(); L.append('jvm_throw(%s);' % a); falls = False
        elif name in ('checkcast', 'instanceof'):
            cn = cname(c, rd2(pc + 1)); _, a = pop()
            if cn.startswith('['): fn, arg = 'array', '"%s"' % cn
            elif cn in self.T.app: fn, arg = '', '&J_%s_class' % cn
            else: fn, arg = 'ext', '"%s"' % cn
            if name == 'checkcast':
                f = 'jvm_checkcast' + ('_' + fn if fn else '')
                L.append('%s = %s(%s, %s);' % (push('A'), f, a, arg))
            else:
                f = 'jvm_instanceof' + ('_' + fn if fn else '')
                L.append('%s = %s(%s, %s);' % (push('I'), f, a, arg))
        elif name in ('monitorenter', 'monitorexit'): pop()
        elif name == 'wide':
            o2 = OPN[code[pc + 1]]; n = rd2(pc + 2)
            if o2 == 'iinc':
                d = rs2(pc + 4); v = self.Lv('I', n)
                L.append('%s = (int32_t)((uint32_t)%s + (uint32_t)(%d));' % (v, v, d))
            elif o2 in LOADOPS:
                k = LOADOPS[o2]; L.append('%s = %s;' % (push(k), self.Lv(k, n)))
            elif o2 in STOREOPS:
                k = STOREOPS[o2]; _, v = pop(); L.append('%s = %s;' % (self.Lv(k, n), v))
            else: raise SystemExit('wide %s unsupported' % o2)
        else:
            raise SystemExit('unsupported opcode %s in %s.%s' % (name, self.k.name, self.m.name))
        return L, st, targets, falls

    # -------- fields
    def field_op(self, name, idx, st, L, pop, push):
        cn, fn, fd = self.ref(idx)
        r = self.T.resolve_field(cn, fn, fd)
        static = name in ('getstatic', 'putstatic')
        if r is None:
            if not static: raise SystemExit('field %s.%s on runtime class' % (cn, fn))
            sym = 'rt_%s_%s' % (cn.replace('/', '_'), fn)
            self.T.ext_statics[sym] = 'extern %s %s;' % (storage_ctype(fd), sym)
            ref = sym
        else:
            kk, f = r
            if static:
                if self.T.needs_init(kk) and not self.clinit_ok(kk): L.append('JVM_CLINIT(J_%s_class);' % kk.name)
                ref = 'J_%s_s%d' % (kk.name, f['idx'])
        kind = kind_of(fd)
        if name == 'getstatic': L.append('%s = %s;' % (push(kind), ref))
        elif name == 'putstatic':
            _, v = pop(); L.append('%s = %s;' % (ref, v))
        elif name == 'getfield':
            _, o = pop()
            L.append('%s = ((J_%s *)jvm_nn(%s))->f%d;' % (push(kind), kk.name, o, f['idx']))
        else:
            _, v = pop(); _, o = pop()
            L.append('((J_%s *)jvm_nn(%s))->f%d = %s;' % (kk.name, o, f['idx'], v))

    def clinit_ok(self, kk):
        cur = self.k
        while cur is not None:
            if cur is kk: return True
            cur = self.T.app.get(cur.super_name)
        return False

    def ref(self, idx):
        e = self.c['cp'][idx]
        return cname(self.c, e[1]), nat(self.c, e[2])[0], nat(self.c, e[2])[1]

    # -------- calls
    def invoke(self, op, idx, st, L, pop, push):
        T = self.T
        cn, name, desc = self.ref(idx)
        params, ret = split_desc(desc)
        args = []
        for _ in params: args.insert(0, pop()[1])
        recv = None
        if op != 'invokestatic': recv = pop()[1]
        ctypes = [CT[kind_of(p)] for p in params]
        res = T.resolve_method(cn, name, desc)
        pre = []
        if res[0] == 'app':
            _, dk, dm = res
            fnptr_sig = '%s (*)(%s)' % ('void' if ret == 'V' else CT[kind_of(ret)],
                                        ', '.join((([] if dm.static else ['JObj *'])) + ctypes) or 'void')
            direct = 'J_%s_m%d' % (dk.name, dm.idx)
            if dm.static:
                if T.needs_init(dk) and not self.clinit_ok(dk): pre.append('JVM_CLINIT(J_%s_class);' % dk.name)
                call = '%s(%s)' % (direct, ', '.join(args))
            elif op == 'invokespecial' or (op == 'invokevirtual' and self.can_devirt(dk, dm)):
                if not dm.has_code: raise SystemExit('direct call to abstract %s.%s' % (dk.name, name))
                call = '%s(%s)' % (direct, ', '.join([recv] + args))
                T.stats['direct'] += 1
            else:
                sel = T.sel[(name, desc)]
                call = '((%s)jvm_vlookup(%s, SEL_%d))(%s)' % (fnptr_sig, recv, sel, ', '.join([recv] + args))
                T.stats['virtual'] += 1
        else:
            ext = res[1]
            if op == 'invokespecial' and name == '<init>' and ext == 'java/lang/Object':
                for p in pre: L.append(p)
                return   # Object.<init> is a no-op
            fn = T.want_ext(ext, name, desc, op == 'invokestatic')
            call = '%s(%s)' % (fn, ', '.join(([] if op == 'invokestatic' else [recv]) + args))
            T.stats['runtime'] += 1
        L.extend(pre)
        if ret == 'V': L.append(call + ';')
        else: L.append('%s = %s;' % (push(kind_of(ret)), call))

    def can_devirt(self, dk, dm):
        if not dm.has_code: return False
        for sk in self.T.subclasses_of(dk.name):
            for m in sk.methods:
                if m.name == dm.name and m.desc == dm.desc and m.has_code: return False
        return True

    # -------- driver
    def run(self):
        T, m, k = self.T, self.m, self.k
        code = m.code
        # pass 1: stack shapes
        entry = {0: ()}
        work = [0]
        handlers = m.exc
        for (s, e, h, t) in handlers:
            entry[h] = ('A',); work.append(h)
        labels = set(h for (_, _, h, _) in handlers)
        while work:
            pc = work.pop()
            while True:
                L, st, targets, falls = self.step(pc, entry[pc])
                for t in targets:
                    labels.add(t)
                    if t not in entry: entry[t] = tuple(st); work.append(t)
                    elif entry[t] != tuple(st): raise SystemExit('stack mismatch at %d in %s.%s' % (t, k.name, m.name))
                if not falls: break
                nxt = self.next_pc(pc)
                if nxt in entry:
                    if entry[nxt] != tuple(st): raise SystemExit('stack mismatch fallthrough %d' % nxt)
                    break
                entry[nxt] = tuple(st); pc = nxt
        # regions for try frames
        region_of = {}; region_ids = {(): 0}
        if self.has_try:
            for pc in entry:
                cov = tuple(i for i, (s, e, h, t) in enumerate(handlers) if s <= pc < e)
                if cov not in region_ids: region_ids[cov] = len(region_ids)
                region_of[pc] = region_ids[cov]
        # pass 2: emit
        self.used_s.clear(); self.used_l.clear()
        body = []
        prev_region = None
        for pc in sorted(entry):
            L, st, targets, falls = self.step(pc, entry[pc])
            if pc in labels: body.append('L%d:;' % pc)
            if self.has_try and (pc in labels or region_of[pc] != prev_region or pc == 0):
                body.append('_rg = %d;' % region_of[pc]); prev_region = region_of[pc]
            elif self.has_try:
                prev_region = region_of[pc]
            if self.has_try and not falls: prev_region = None
            body.extend('    ' + l for l in L)
        # header
        vol = 'volatile ' if self.has_try else ''
        ptypes = []
        slot = 0; pcopy = []
        if not m.static:
            ptypes.append('JObj *p0'); pcopy.append('%s = p0;' % self.Lv('A', 0)); slot = 1
        for i, p in enumerate(m.params):
            kd = kind_of(p); nm = 'p%d' % (i + 1)
            ptypes.append('%s %s' % (CT[kd], nm)); pcopy.append('%s = %s;' % (self.Lv(kd, slot), nm))
            slot += 2 if kd in 'JD' else 1
        retc = 'void' if m.ret == 'V' else CT[kind_of(m.ret)]
        if m.name == '<clinit>': fname = 'J_%s_m%d' % (k.name, m.idx); sig = 'void %s(void)' % fname
        else:
            fname = 'J_%s_m%d' % (k.name, m.idx); sig = '%s %s(%s)' % (retc, fname, ', '.join(ptypes) or 'void')
        out = ['/* %s.%s%s */' % (k.name, m.name, m.desc), sig + ' {']
        for (kd, d) in sorted(self.used_s):
            out.append('    %s s%s%d = %s;' % (CT[kd], kd, d, 'NULL' if kd == 'A' else '0'))
        for (kd, n) in sorted(self.used_l, key=lambda t: (str(t[0]), t[1])):
            if kd.startswith('T'):
                out.append('    %s _t%d%s = %s;' % (CT[kd[1]], n, kd[1], 'NULL' if kd[1] == 'A' else '0'))
            else:
                ty = ('JObj * volatile' if kd == 'A' else 'volatile ' + CT[kd]) if vol else CT[kd]
                out.append('    %s l%s%d = %s;' % (ty, kd, n, 'NULL' if kd == 'A' else '0'))
        if self.has_try:
            out.append('    volatile int _rg = 0; JvmTry _tf;')
        out.extend('    ' + l for l in pcopy)
        if self.has_try:
            out.append('    _tf.prev = jvm_try_top; jvm_try_top = &_tf;')
            out.append('    if (setjmp(_tf.jb)) {')
            out.append('        JObj *_ex = jvm_exc; jvm_try_top = &_tf;')
            out.append('        switch (_rg) {')
            for cov, rid in sorted(region_ids.items(), key=lambda t: t[1]):
                if not cov: continue
                out.append('        case %d:' % rid)
                for hi in cov:
                    s, e, h, t = handlers[hi]
                    if t == 0: cond = '1'
                    else:
                        tn = cname(self.c, t)
                        if tn in T.app: cond = 'jvm_instanceof(_ex, &J_%s_class)' % tn
                        else: cond = 'jvm_instanceof_ext(_ex, "%s")' % tn
                    out.append('            if (%s) { sA0 = _ex; goto L%d; }' % (cond, h))
                out.append('            break;')
            out.append('        default: break;')
            out.append('        }')
            out.append('        jvm_try_top = _tf.prev; jvm_throw(_ex);')
            out.append('    }')
        out.extend(body)
        out.append('}')
        out.append('')
        T.stats['methods'] += 1
        return '\n'.join(out)

    def next_pc(self, pc):
        for p, op, nx in ops(self.code[pc:] if False else self.code):
            if p == pc: return nx
        raise SystemExit('bad pc')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--jar', required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('--midlet', required=True)
    a = ap.parse_args()
    T = Translator(a.jar, a.midlet)
    T.emit_all(a.out, a.midlet)
    print('classes=%d methods=%d selectors=%d strings=%d runtime-api=%d' % (
        len(T.app), T.stats['methods'], len(T.sel), len(T.str_ids), len(T.ext_funcs)))
    print('calls: direct=%d virtual=%d runtime=%d' % (T.stats['direct'], T.stats['virtual'], T.stats['runtime']))


if __name__ == '__main__':
    main()
