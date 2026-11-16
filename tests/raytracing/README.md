# Ray tracing tests

Run from the repo root.

| Test | What it checks | Command |
|---|---|---|
| `bvh_selftest.cpp` | CPU BVH traversal == brute force on 20k random triangles | `g++ -std=c++17 -O2 -Isrc tests/raytracing/bvh_selftest.cpp src/modules/raytracing/bvh.cpp -o /tmp/bvh_selftest && /tmp/bvh_selftest` |
| `gpu_trace_test.cpp` | `rt_common.glsl` closest-hit and any-hit on the GPU (GL 4.5 compute, headless EGL; Mesa llvmpipe works) == CPU BVH | `g++ -std=c++17 -O2 -Isrc tests/raytracing/gpu_trace_test.cpp src/modules/raytracing/bvh.cpp -lEGL -lOpenGL -o /tmp/gpu_trace_test && EGL_PLATFORM=surfaceless /tmp/gpu_trace_test shaders` |
| `validate_shaders.sh` | every `shaders/raytracing` shader (and composite permutation) compiles with glslang, GLSL 450 | `tests/raytracing/validate_shaders.sh` |

Last results (cloud, Mesa llvmpipe 25.2): BVH self-test 2000 rays / 0 mismatches; GPU test 4096 rays,
0 closest-hit and 0 any-hit mismatches; all 13 shader variants compile.
