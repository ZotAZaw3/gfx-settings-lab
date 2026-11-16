#!/usr/bin/env python3
"""Headless image-quality test for the AA / upscaling / frame-generation shaders.

Renders a procedural scene (thin lines, a moving rotated square, HDR highlights) with moderngl on
any GL 4.5 driver (Mesa llvmpipe works: `xvfb-run python3 tests/aa/quality_test.py`) and compares
each technique against a 16x16 supersampled reference. Needs: pip install moderngl numpy pillow.
Run from the repo root. PNGs of each result are written to the current directory.
"""
import moderngl, numpy as np, re, struct, sys, os
from PIL import Image
import os
SH=os.path.join(os.path.dirname(os.path.abspath(__file__)),'..','..','shaders')+'/'
ctx=moderngl.create_standalone_context(require=450)
import ctypes
_gl=ctypes.CDLL('libGL.so.1')
_gpa=_gl.glXGetProcAddressARB; _gpa.restype=ctypes.c_void_p; _gpa.argtypes=[ctypes.c_char_p]
glClipControl=ctypes.CFUNCTYPE(None,ctypes.c_uint,ctypes.c_uint)(_gpa(b'glClipControl'))
glClipControl(0x8CA2, 0x935F)   # GL_UPPER_LEFT, GL_ZERO_TO_ONE
def load(path):
    s=open(SH+path).read()
    return re.sub(r'#include "(.+?)"', lambda m: open(SH+m.group(1)).read(), s)
VS=open(SH+'common/fullscreen.vert').read()
def prog(fs_src): return ctx.program(vertex_shader=VS, fragment_shader=fs_src)
vao_cache={}
def run(p, targets, textures, ubo=None):
    fbo=ctx.framebuffer(color_attachments=targets)
    fbo.use(); fbo.clear(0,0,0,0)
    for unit,(tex,filt) in textures.items():
        tex.filter=(moderngl.LINEAR,moderngl.LINEAR) if filt=='l' else (moderngl.NEAREST,moderngl.NEAREST)
        tex.repeat_x=tex.repeat_y=False
        tex.use(location=unit)
    if ubo is not None:
        b=ctx.buffer(ubo); b.bind_to_uniform_block(3)
    vao=ctx.vertex_array(p, [])
    vao.render(moderngl.TRIANGLES, vertices=3)
    fbo.release()
def tex(w,h,comp=4,dtype='f2'): return ctx.texture((w,h),comp,dtype=dtype)
def read(t):
    comp=t.components; a=np.frombuffer(t.read(),dtype=np.float16 if t.dtype=='f2' else (np.uint8 if t.dtype=='f1' else np.float32)).reshape(t.height,t.width,comp).astype(np.float32)
    if t.dtype=='f1': a/=255
    return a
SCENE='''#version 450
layout(location=0) in vec2 vUV;
layout(location=0) out vec4 oColor; layout(location=1) out vec2 oVel; layout(location=2) out float oDepth;
layout(std140,binding=3) uniform P { vec4 size; vec4 jit; vec4 motion; int ss; int p0,p1,p2; };
// returns color, depth; shapes in pixel space of a 'size' frame
vec4 shade(vec2 px, float t, out float depth, out float moving){
  depth=0.0; moving=0.0;
  vec3 c = vec3(0.15,0.2,0.3) + 0.05*vec3(step(0.5,fract(px.x/37.0))); // bg stripes
  // thin diagonal lines
  float l = abs(fract((px.x*0.7+px.y*0.3)/23.0)-0.5)*23.0; if (l<0.6) { c=vec3(1); depth=0.1; }
  // rotated square, moving
  vec2 o = px - (vec2(0.5,0.5)*size.xy + vec2(t*motion.x,0));
  float a=0.33; vec2 r=vec2(cos(a)*o.x+sin(a)*o.y, -sin(a)*o.x+cos(a)*o.y);
  if (max(abs(r.x),abs(r.y))<size.y*0.2){ c=vec3(1.0,0.6,0.1)*(0.7+0.3*step(0.0,r.x*r.y)); depth=0.5; moving=1.0; }
  // circle
  if (length(px-vec2(0.25,0.3)*size.xy)<size.y*0.12){ c=vec3(0.2,0.9,0.4); depth=0.3; }
  // bright HDR spec dot grid
  vec2 g=fract(px/41.0)-0.5; if (length(g)*41.0<0.8) c=vec3(motion.z > 0.5 ? 4.0 : 1.0);
  return vec4(c,1);
}
void main(){
  vec2 base=gl_FragCoord.xy; float t=motion.y;
  vec3 acc=vec3(0); float d,m; float dd=0.0, mm=0.0;
  if (ss>1){ for(int y=0;y<ss;y++)for(int x=0;x<ss;x++){ vec2 off=(vec2(x,y)+0.5)/float(ss)-0.5; acc+=shade((base+off)*size.z, t, d,m).rgb;} acc/=float(ss*ss); shade(base*size.z,t,dd,mm);} 
  else { acc=shade((base - jit.xy)*size.z, t, dd, mm).rgb; }
  oColor=vec4(acc,1); oDepth=dd;
  // velocity NDC: object moves motion.x px/frame (in reference pixel space) to the right
  oVel = vec2(mm*motion.x*2.0/size.x, 0.0);
}
'''

W,H=512,288
scene=prog(SCENE)
def halton(i,b):
    f=1;r=0
    while i>0: f/=b; r+=f*(i%b); i//=b
    return r
def render(w,h,jit=(0,0),t=0.0,motion=0.0,ss=1,hdr=1.0):
    c=tex(w,h); v=tex(w,h,2); d=tex(w,h,1,'f4')
    ubo=struct.pack('4f4f4f4i',W,H,W/w,0, jit[0],jit[1],0,0, motion,t,hdr,0, ss,0,0,0)
    run(scene,[c,v,d],{},ubo); return c,v,d
def tm(a): return a[...,:3]/(1+a[...,:3])
def psnr(a,b):
    m=np.mean((tm(a)-tm(b))**2); return 10*np.log10(1/m)
def save(a,name): Image.fromarray((np.clip(tm(a)*2,0,1)*255).astype(np.uint8)).save(name)
res={}
# ---------- post AA (LDR scene) ----------
ref_ldr=read(render(W,H,ss=16,hdr=0)[0])
raw,_,_=render(W,H,hdr=0); rawa=read(raw)
res['noAA (LDR)']=psnr(rawa,ref_ldr)
out=tex(W,H)
fx=prog(load('aa/fxaa.frag'))
run(fx,[out],{0:(raw,'l')},struct.pack('4f3fi4f',1/W,1/H,0,0,0.125,0.0312,1.0,12,1,0,0,0))
res['FXAA high']=psnr(read(out),ref_ldr); save(read(out),'fxaa.png')
edges=tex(W,H,2,'f1'); weights=tex(W,H,4,'f1')
smaa_ubo=struct.pack('4ffiif',1/W,1/H,1,0,0.1,16,1,2.0)
run(prog(load('aa/smaa_edge.frag')),[edges],{0:(raw,'n')},smaa_ubo)
run(prog(load('aa/smaa_weights.frag')),[weights],{0:(edges,'n')},smaa_ubo)
out2=tex(W,H)
run(prog(load('aa/smaa_blend.frag')),[out2],{0:(raw,'n'),1:(weights,'n')},smaa_ubo)
res['SMAA high']=psnr(read(out2),ref_ldr); save(read(out2),'smaa.png'); save(rawa,'noaa.png'); save(ref_ldr,'ref.png')
print('edge px %:',float((read(edges)[...,:2].sum(-1)>0).mean()*100), 'weights mean', float(read(weights).mean()))
# ---------- TAA native, static, HDR ----------
ref=read(render(W,H,ss=16)[0])
taa=prog(load('aa/taa_resolve.frag'))
hist=[tex(W,H),tex(W,H)]
def taa_run(frames, motion=0.0, gamma=1.25, minA=0.08):
    for f in range(frames):
        i=f%8+1; j=(halton(i,2)-0.5, halton(i,3)-0.5)
        c,v,d=render(W,H,jit=j,t=float(f),motion=motion)
        cur,prev=hist[f&1],hist[(f&1)^1]
        run(taa,[cur],{0:(c,'n'),1:(prev,'l'),2:(v,'n'),3:(d,'n')},struct.pack('4f3fii3i',0,0,0,0,gamma,minA,0.3,1 if f==0 else 0,0,0,0,0))
    return read(cur)
res['noAA (HDR)']=psnr(read(render(W,H)[0]),ref)
a=taa_run(32); res['TAA 32f static']=psnr(a,ref); save(a,'taa.png')
refm=read(render(W,H,ss=16,t=31.0,motion=2.0)[0])
a=taa_run(32,motion=2.0); res['TAA 32f moving 2px/f']=psnr(a,refm); res['noAA moving']=psnr(read(render(W,H,t=31.0,motion=2.0)[0]),refm); save(a,'taa_moving.png')
# ---------- upscaling 50% ----------
w,h=W//2,H//2
lo=render(w,h)[0]
bil=tex(W,H)
# bilinear: sample via EASU-less simple pass
BIL='''#version 450
layout(location=0) in vec2 vUV; layout(location=0) out vec4 o; layout(binding=0) uniform sampler2D t;
void main(){ o=texture(t,vUV); }'''
run(prog(BIL),[bil],{0:(lo,'l')}); res['50% bilinear']=psnr(read(bil),ref)
ea=tex(W,H)
run(prog(load('aa/fsr1_easu.frag')),[ea],{0:(lo,'n')},struct.pack('8f',w,h,1/w,1/h,1,1,0,0))
res['50% EASU']=psnr(read(ea),ref); save(read(ea),'easu.png')
taau=prog(load('aa/taau_accumulate.frag'))
uh=[tex(W,H),tex(W,H)]
KS=0.75; G=2.0; MA=0.02   # same values temporal.cpp uses
def taau_run(frames,motion=0.0):
    n=32
    for f in range(frames):
        i=f%n+1; j=(halton(i,2)-0.5, halton(i,3)-0.5)
        c,v,d=render(w,h,jit=j,t=float(f),motion=motion)
        cur,prev=uh[f&1],uh[(f&1)^1]
        ubo=struct.pack('4f4f4f3fi2ifi',w,h,1/w,1/h,W,H,1/W,1/H,j[0],j[1],0,0,G,MA,0.25,1 if f==0 else 0,0,0,KS,0)
        run(taau,[cur],{0:(c,'n'),1:(prev,'l'),2:(v,'n'),3:(d,'n'),4:(d,'n')},ubo)
    return read(cur)
a=taau_run(64); res['50% TAAU 64f static']=psnr(a,ref); save(a,'taau.png')
a=taau_run(64,motion=2.0); res['50% TAAU 64f moving']=psnr(a,read(render(W,H,ss=16,t=63.0,motion=2.0)[0])); save(a,'taau_moving.png')
# ---------- frame gen ----------
fA=render(W,H,t=10.0,motion=6.0); fB=render(W,H,t=11.0,motion=6.0)
refmid=read(render(W,H,t=10.5,motion=6.0)[0])
flow=tex(W//4,H//4)
run(prog(load('aa/fg_optical_flow.frag')),[flow],{0:(fB[0],'l'),1:(fA[0],'l'),2:(fB[1],'l')},struct.pack('4fifii',W/4,H/4,4/W,4/H,2,0.0,0,0))
gen=tex(W,H)
run(prog(load('aa/fg_interpolate.frag')),[gen],{0:(fA[0],'l'),1:(fB[0],'l'),2:(flow,'n'),3:(fA[2],'n'),4:(fB[2],'n')},struct.pack('ffii',0.5,4.0,0,2))
bad=lambda a: float((np.abs(tm(a)-tm(refmid)).sum(-1)>0.1).mean()*100)
wrong={'FG interpolated (ours)':bad(read(gen)),'FG naive 50/50 blend':bad(0.5*read(fA[0])+0.5*read(fB[0])),'FG repeat previous frame':bad(read(fA[0]))}
save(read(gen),'fg.png')
# ---------- sharpen sanity ----------
os.makedirs('out',exist_ok=True)
for n in ['cas','rcas','unsharp','nis_lite']:
    o=tex(W,H); run(prog(load(f'aa/{n}.frag')),[o],{0:(out,'l')},struct.pack('fiff',0.5,0,0.5,1.0))
    a=read(o); assert np.isfinite(a).all(); res[f'{n} on FXAA (sanity)']=psnr(a,ref_ldr)
for k,v in res.items(): print(f'{k:32s} {v:6.2f} dB PSNR vs 16x16 supersampled reference')
for k,v in wrong.items(): print(f'{k:32s} {v:6.2f} % pixels visibly wrong vs true mid-frame')
