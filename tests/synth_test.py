#!/usr/bin/env python3
"""Offline check of the MIDI synth: builds a tiny SMF (C4-E4-G4, then a tempo change), renders it,
and verifies note pitch (FFT peak) and timing against the expected values."""
import struct, subprocess, sys, numpy as np, os
def vlq(n):
    b=[n&0x7f]; n>>=7
    while n: b.append(0x80|(n&0x7f)); n>>=7
    return bytes(reversed(b))
def smf(events, div=480):
    trk=b''.join(vlq(dt)+ev for dt,ev in events)+vlq(0)+b'\xff\x2f\x00'
    return b'MThd'+struct.pack('>IHHH',6,0,1,div)+b'MTrk'+struct.pack('>I',len(trk))+trk
tempo=lambda us: b'\xff\x51\x03'+us.to_bytes(3,'big')
on=lambda n:bytes([0x90,n,100]); off=lambda n:bytes([0x80,n,0])
ev=[(0,tempo(500000)),(0,on(60)),(480,off(60)),(0,on(64)),(480,off(64)),(0,tempo(250000)),(0,on(67)),(480,off(67))]
here=os.path.dirname(os.path.abspath(__file__)); tmp='/tmp/synth_test'; os.makedirs(tmp,exist_ok=True)
open(f'{tmp}/t.mid','wb').write(smf(ev))
subprocess.check_call(['gcc','-O2','-o',f'{tmp}/sr',f'{here}/synth_render.c',f'{here}/../runtime/synth.c','-lm'])
print(subprocess.check_output([f'{tmp}/sr',f'{tmp}/t.mid',f'{tmp}/t.raw','1.6']).decode().strip())
x=np.fromfile(f'{tmp}/t.raw',dtype=np.float32).reshape(-1,2).mean(axis=1); sr=44100
def peak(t0,t1):
    seg=x[int(t0*sr):int(t1*sr)]; sp=np.abs(np.fft.rfft(seg*np.hanning(len(seg)),1<<17)); f=np.fft.rfftfreq(1<<17,1/sr)
    m=(f>100)&(f<2000); return f[m][np.argmax(sp[m])]
exp=[(0.05,0.45,261.63),(0.55,0.95,329.63),(1.05,1.20,392.00)]   # 3rd note is 0.25 s long (tempo doubled at tick 960)
ok=True
for t0,t1,fe in exp:
    fm=peak(t0,t1); good=abs(1200*np.log2(fm/fe))<30; ok&=good
    print(f"{t0:.2f}-{t1:.2f}s expected {fe:7.2f} Hz  measured {fm:7.2f} Hz  {'OK' if good else 'FAIL'}")
env=np.abs(x); rms=lambda a,b: float(np.sqrt(np.mean(x[int(a*sr):int(b*sr)]**2)))
tail=rms(1.45,1.6); body=rms(1.05,1.2); g=tail<body*0.2; ok&=g
print(f"third note ends at 1.25s (tempo change honoured): body rms {body:.3f}, tail rms {tail:.3f}  {'OK' if g else 'FAIL'}")
sys.exit(0 if ok else 1)
