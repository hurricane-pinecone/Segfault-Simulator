// Tiny-voxel 3D pass: real GPU MVP transform (the iso path pre-projects on the
// CPU; this one does not). #version is prepended at runtime.
layout(location = 0) in vec3 aPos;    // world space
layout(location = 1) in vec3 aNormal; // flat face normal
layout(location = 2) in vec3 aColor;  // per-voxel colour

uniform mat4 uViewProj;

out vec3 vNormal;
out vec3 vColor;

void main()
{
  vNormal = aNormal;
  vColor = aColor;
  gl_Position = uViewProj * vec4(aPos, 1.0);
}
