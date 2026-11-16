#!/bin/bash
# Expands #include "path" (relative to shaders/, like ShaderLibrary) and validates every ray tracing
# shader with glslangValidator using OpenGL GLSL 450 semantics. Usage: tests/raytracing/validate_shaders.sh
ROOT="$(cd "$(dirname "$0")/../.." && pwd)/shaders"
[ -d "$ROOT/raytracing" ] || ROOT="$(cd "$(dirname "$0")/.." && pwd)/shaders"   # scratch layout
out=$(mktemp -d); fail=0
for f in "$ROOT"/raytracing/*.comp "$ROOT"/raytracing/*.frag; do
  b=$(basename "$f"); stage=${b##*.}
  for def in "" COMPOSITE_SCALAR COMPOSITE_GI COMPOSITE_REFL COMPOSITE_PT COMPOSITE_DEBUG; do
    [ "$stage" = comp ] && [ -n "$def" ] && continue
    [ "$stage" = frag ] && [ -z "$def" ] && continue
    python3 - "$ROOT" "$f" "$def" > "$out/$b" <<'PY'
import re,sys
root,path,define=sys.argv[1],sys.argv[2],sys.argv[3]
seen=set()
def expand(p):
    s=open(p).read()
    def rep(m):
        n=m.group(1)
        if n in seen: return ""
        seen.add(n); return expand(root+"/"+n)
    return re.sub(r'^#include "([^"]+)"',rep,s,flags=re.M)
src=expand(path)
if define: src=src.replace("#version 450","#version 450\n#define "+define,1)
print(src)
PY
    if glslangValidator -G --target-env opengl -S "$stage" "$out/$b" -o /dev/null > "$out/log" 2>&1; then echo "OK   $b $def"
    else echo "FAIL $b $def"; grep -E "ERROR" "$out/log" | head -10; fail=1; fi
  done
done
exit $fail
