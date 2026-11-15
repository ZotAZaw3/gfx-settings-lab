#version 450
// Camera-only motion reconstructed from depth, for pixels that wrote no object velocity (sky, far plane).
// Writes into scene.velocity with depth test GREATER-EQUAL disabled; run as a full-screen pass that
// only touches pixels at depth == 0 (reversed-Z far plane), or everywhere when uAll = 1.
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec2 oVelocity;
layout(binding = 3) uniform sampler2D uDepth;
layout(std140, binding = 3) uniform CamVelParams {
    mat4 uInvViewProj;     // inverse of current UNJITTERED view-proj
    mat4 uPrevViewProj;    // previous UNJITTERED view-proj
    int  uAll; int _p0, _p1, _p2;
};
void main() {
    float d = texelFetch(uDepth, ivec2(gl_FragCoord.xy), 0).r;
    if (uAll == 0 && d > 0.0) discard;
    // sky: use a tiny depth instead of 0 so the point is at a finite (far) distance
    float dz = max(d, 1e-7);
    vec2 ndc = vec2(vUV.x * 2.0 - 1.0, 1.0 - vUV.y * 2.0);
    vec4 world = uInvViewProj * vec4(ndc, dz, 1.0);
    vec4 prev = uPrevViewProj * world;
    oVelocity = ndc - prev.xy / prev.w;
}
