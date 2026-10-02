import struct, sys, collections

OPN = {}
def _o(n, names):
    for i, s in enumerate(names.split()):
        OPN[n+i] = s
_o(0,"nop aconst_null iconst_m1 iconst_0 iconst_1 iconst_2 iconst_3 iconst_4 iconst_5 lconst_0 lconst_1 fconst_0 fconst_1 fconst_2 dconst_0 dconst_1 bipush sipush ldc ldc_w ldc2_w iload lload fload dload aload iload_0 iload_1 iload_2 iload_3 lload_0 lload_1 lload_2 lload_3 fload_0 fload_1 fload_2 fload_3 dload_0 dload_1 dload_2 dload_3 aload_0 aload_1 aload_2 aload_3 iaload laload faload daload aaload baload caload saload istore lstore fstore dstore astore istore_0 istore_1 istore_2 istore_3 lstore_0 lstore_1 lstore_2 lstore_3 fstore_0 fstore_1 fstore_2 fstore_3 dstore_0 dstore_1 dstore_2 dstore_3 astore_0 astore_1 astore_2 astore_3 iastore lastore fastore dastore aastore bastore castore sastore pop pop2 dup dup_x1 dup_x2 dup2 dup2_x1 dup2_x2 swap iadd ladd fadd dadd isub lsub fsub dsub imul lmul fmul dmul idiv ldiv fdiv ddiv irem lrem frem drem ineg lneg fneg dneg ishl lshl ishr lshr iushr lushr iand land ior lor ixor lxor iinc i2l i2f i2d l2i l2f l2d f2i f2l f2d d2i d2l d2f i2b i2c i2s lcmp fcmpl fcmpg dcmpl dcmpg ifeq ifne iflt ifge ifgt ifle if_icmpeq if_icmpne if_icmplt if_icmpge if_icmpgt if_icmple if_acmpeq if_acmpne goto jsr ret tableswitch lookupswitch ireturn lreturn freturn dreturn areturn return getstatic putstatic getfield putfield invokevirtual invokespecial invokestatic invokeinterface xxx new newarray anewarray arraylength athrow checkcast instanceof monitorenter monitorexit wide multianewarray ifnull ifnonnull goto_w jsr_w")

class R:
    def __init__(s,b): s.b=b; s.p=0
    def u1(s): v=s.b[s.p]; s.p+=1; return v
    def u2(s): v=struct.unpack_from('>H',s.b,s.p)[0]; s.p+=2; return v
    def u4(s): v=struct.unpack_from('>I',s.b,s.p)[0]; s.p+=4; return v
    def raw(s,n): v=s.b[s.p:s.p+n]; s.p+=n; return v

def parse(path):
    r=R(open(path,'rb').read())
    assert r.u4()==0xCAFEBABE
    minor=r.u2(); major=r.u2()
    n=r.u2(); cp=[None]*n; i=1
    while i<n:
        t=r.u1()
        if t==1: l=r.u2(); raw=r.raw(l); cp[i]=('utf8',raw.decode('utf8','replace'),raw)
        elif t==3: cp[i]=('int',struct.unpack('>i',r.raw(4))[0])
        elif t==4: cp[i]=('float',struct.unpack('>f',r.raw(4))[0])
        elif t==5: cp[i]=('long',struct.unpack('>q',r.raw(8))[0]); i+=1
        elif t==6: cp[i]=('double',struct.unpack('>d',r.raw(8))[0]); i+=1
        elif t==7: cp[i]=('class',r.u2())
        elif t==8: cp[i]=('string',r.u2())
        elif t in (9,10,11): cp[i]=({9:'field',10:'method',11:'imethod'}[t],r.u2(),r.u2())
        elif t==12: cp[i]=('nat',r.u2(),r.u2())
        else: raise Exception('cp tag %d'%t)
        i+=1
    c=dict(major=major,minor=minor,cp=cp)
    c['access']=r.u2(); c['this']=r.u2(); c['super']=r.u2()
    c['ifaces']=[r.u2() for _ in range(r.u2())]
    def attrs():
        out=[]
        for _ in range(r.u2()):
            nm=r.u2(); ln=r.u4(); out.append((cp[nm][1],r.raw(ln)))
        return out
    c['fields']=[]
    for _ in range(r.u2()):
        a=r.u2(); nm=r.u2(); d=r.u2(); c['fields'].append(dict(acc=a,name=cp[nm][1],desc=cp[d][1],attrs=attrs()))
    c['methods']=[]
    for _ in range(r.u2()):
        a=r.u2(); nm=r.u2(); d=r.u2(); at=attrs()
        m=dict(acc=a,name=cp[nm][1],desc=cp[d][1],attrs=at)
        for an,ab in at:
            if an=='Code':
                rr=R(ab); ms=rr.u2(); ml=rr.u2(); cl=rr.u4(); code=rr.raw(cl)
                ex=[(rr.u2(),rr.u2(),rr.u2(),rr.u2()) for _ in range(rr.u2())]
                m['code']=code; m['maxstack']=ms; m['maxlocals']=ml; m['exc']=ex
        c['methods'].append(m)
    c['attrs']=attrs()
    return c

def cname(c,i): return c['cp'][c['cp'][i][1]][1]
def nat(c,i):
    n=c['cp'][i]; return c['cp'][n[1]][1], c['cp'][n[2]][1]
def ref(c,i):
    e=c['cp'][i]; return cname(c,e[1]), *nat(c,e[2])

def ops(code):
    """yield (pc, opcode, operands-len) iterating bytecode"""
    p=0; L=len(code)
    while p<L:
        op=code[p]; start=p; p+=1
        if op in (16,18,21,22,23,24,25,54,55,56,57,58,169,188): p+=1
        elif op in (17,19,20,132,153,154,155,156,157,158,159,160,161,162,163,164,165,166,167,168,178,179,180,181,182,183,184,187,189,192,193,198,199): p+=2
        elif op==197: p+=3
        elif op in (185,200,201): p+=4
        elif op==196:
            p+= 5 if code[p]==132 else 3
        elif op==170:
            p=(p+3)&~3; lo=struct.unpack_from('>i',code,p+4)[0]; hi=struct.unpack_from('>i',code,p+8)[0]; p+=12+4*(hi-lo+1)
        elif op==171:
            p=(p+3)&~3; n=struct.unpack_from('>i',code,p+4)[0]; p+=8+8*n
        yield start,op,p


def mutf8_units(raw):
    """Decode Java modified UTF-8 into a list of UTF-16 code units."""
    out=[]; i=0; n=len(raw)
    while i<n:
        b=raw[i]
        if b<0x80: out.append(b); i+=1
        elif (b&0xE0)==0xC0: out.append(((b&0x1F)<<6)|(raw[i+1]&0x3F)); i+=2
        else: out.append(((b&0x0F)<<12)|((raw[i+1]&0x3F)<<6)|(raw[i+2]&0x3F)); i+=3
    return out
