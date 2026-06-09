// Tiny-voxel 3D pass: real GPU MVP transform (the iso path pre-projects on the
// CPU; this one does not). #version is prepended at runtime.
layout(location = 0) in vec3 aPos;    // world space
layout(location = 1) in vec3 aNormal; // flat face normal
layout(location = 2) in vec4 aColor;  // per-voxel colour + alpha

uniform mat4 uViewProj;
uniform mat4 uModel; // identity for world-space geometry; per-object for blocks

out vec3 vWorldPos;
out vec3 vNormal;
out vec4 vColor;

void main()
{
  vec4 worldPos = uModel * vec4(aPos, 1.0);
  vWorldPos = worldPos.xyz;
  vNormal = mat3(uModel) * aNormal; // uModel is translate*rotate (no scale)
  vColor = aColor;
  gl_Position = uViewProj * worldPos;
}
