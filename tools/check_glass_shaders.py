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

def header_literal(path, name):
    text = (ROOT / path).read_text()
    m = re.search(name + r' = R"\((.*?)\)";', text, re.S)
    return m.group(1).replace(')" R"(', "")

def compose(defines, parts):
    """Mirror of GlassSurface.cpp Compose(): splice the SDF and reflection trace HLSL at their markers."""
    source = defines + "".join(parts)
    marker = "//SDF_TRACE_INCLUDE"
    if marker in source:
        source = source.replace(marker, header_literal("engine/lighting/include/orbit/lighting/SdfTraceShader.hpp", "kSdfTraceHlsl"), 1)
        exact = "//REFLECTION_TRACE_INCLUDE"
        if exact in source:
            trace = header_literal("engine/lighting/include/orbit/lighting/ReflectionTraceShader.hpp", "kReflectionTraceHlsl")
            types_end = trace.index("bool ReflectionBox")
            bindings = """
[[vk::binding(5, 0)]] StructuredBuffer<ReflectionTriangle> g_reflectionTriangles;
[[vk::binding(6, 0)]] StructuredBuffer<ReflectionNode> g_reflectionNodes;
[[vk::binding(7, 0)]] StructuredBuffer<float4> g_reflectionLighting;
float4 ExactParams() { return g_records[(uint)g.c5.w * 6u + 3u]; }
#define REFLECTION_ORIGIN_COUNT ExactParams()
"""
            source = source.replace(exact, trace[:types_end] + bindings + trace[types_end:], 1)
    return source
D0,D1=define('kDefaultDefines'),define('kPhotonPassDefines')
shared=['kCommon','kTrace','kScene','kCausticTextures','kCausticTypes','kCausticHelpers']
jobs={'legacy_vs':(D0,shared+['kCausticVertex'],'vert'),'legacy_ps':(D0,['kCommon','kScene','kCausticPixel'],'frag'),
 'buffered_vs':(D1,shared+['kCausticVertexBuffered'],'vert'),'buffered_ps':(D1,['kCommon','kScene','kCausticPixel'],'frag'),
 'compute':(D1,shared+['kCausticCompute'],'comp')}
jobs['glass_ps']=None
bad=0
TMP=pathlib.Path(tempfile.mkdtemp())
for n,job in jobs.items():
    if job is None:
        d,keys,st='#define DEPTH_BINDING 8\n#define BACKDROP_BINDING 9\n',['kCommon','kTrace','kScene','kGlassPixel'],'frag'
        text=compose(d,[lits[k] for k in keys])
    else:
        d,keys,st=job
        text=d+''.join(lits[k] for k in keys)
    f=str(TMP/n)
    open(f+'.hlsl','w').write(text)
    r=subprocess.run(['glslangValidator','-D','-V','-S',st,'-e','main','--target-env','vulkan1.3','-o',f+'.spv',f+'.hlsl'],capture_output=True,text=True)
    err=[l for l in r.stdout.splitlines() if l.startswith('ERROR')]
    print(f'{n:12s}','ok' if r.returncode==0 else 'FAIL',err[:4]); bad+=r.returncode!=0
sys.exit(1 if bad else 0)
