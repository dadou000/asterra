#!/usr/bin/env python3
"""Compile-check the caustic shaders embedded in GlassSurface.cpp (legacy path and photon pre-pass) without a
Windows build, using glslangValidator (apt install glslang-tools). Syntax and types only: not DXC, not run.

    python tools/check_glass_shaders.py
"""
import pathlib, re, subprocess, sys, tempfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
src=open(ROOT/'engine/mesh_render/src/GlassSurface.cpp').read()
lits={m.group(1):m.group(2).replace(')" R"(','') for m in re.finditer(r'constexpr const char\* (k\w+) = R"\((.*?)\)";',src,re.S)}
def define(name):
    m=re.search(r'inline const std::string '+name+r' =\s*((?:"[^;]*?")(?:\s*"[^;]*?")*);',src,re.S)
    return ''.join(re.findall(r'"((?:[^"\\]|\\.)*)"',m.group(1))).encode().decode('unicode_escape')
D0,D1=define('kDefaultDefines'),define('kPhotonPassDefines')
shared=['kCommon','kTrace','kScene','kCausticTextures','kCausticTypes','kCausticHelpers']
jobs={'legacy_vs':(D0,shared+['kCausticVertex'],'vert'),'legacy_ps':(D0,['kCommon','kScene','kCausticPixel'],'frag'),
 'buffered_vs':(D1,shared+['kCausticVertexBuffered'],'vert'),'buffered_ps':(D1,['kCommon','kScene','kCausticPixel'],'frag'),
 'compute':(D1,shared+['kCausticCompute'],'comp')}
bad=0
TMP=pathlib.Path(tempfile.mkdtemp())
for n,(d,keys,st) in jobs.items():
    f=str(TMP/n)
    open(f+'.hlsl','w').write(d+''.join(lits[k] for k in keys))
    r=subprocess.run(['glslangValidator','-D','-V','-S',st,'-e','main','--target-env','vulkan1.3','-o',f+'.spv',f+'.hlsl'],capture_output=True,text=True)
    err=[l for l in r.stdout.splitlines() if l.startswith('ERROR')]
    print(f'{n:12s}','ok' if r.returncode==0 else 'FAIL',err[:4]); bad+=r.returncode!=0
sys.exit(1 if bad else 0)
