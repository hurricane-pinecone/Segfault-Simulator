layout(location = 0) in vec2 aPosition;   // NDC
layout(location = 1) in vec2 aWorldPos;
layout(location = 2) in float aGround;
layout(location = 3) in vec2 aUv;
layout(location = 4) in vec3 aNormal;
layout(location = 5) in float aZ;         // clip-space depth

out vec2 vUv;
out vec2 vWorldPos;
out float vGround;
out vec3 vNormal;

void main()
{
  vUv = aUv;
  vWorldPos = aWorldPos;
  vGround = aGround;
  vNormal = aNormal;
  gl_Position = vec4(aPosition, aZ, 1.0);
}
