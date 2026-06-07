layout (location = 0) in vec2 aPosition;
layout (location = 1) in vec2 aUv;
layout (location = 2) in vec2 aWorldPosition;
layout (location = 3) in float aZ;
layout (location = 4) in vec4 aTint;

out vec2 vUv;
out vec2 vWorldPosition;
out vec4 vTint;

void main()
{
  vUv = aUv;
  vWorldPosition = aWorldPosition;
  vTint = aTint;

  gl_Position = vec4(aPosition, aZ, 1.0);
}
