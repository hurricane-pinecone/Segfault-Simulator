layout(location = 0) in vec2 aPosition;
layout(location = 1) in vec2 aWorldPosition;
layout(location = 2) in vec4 aColor;
layout(location = 3) in vec2 aUv;
layout(location = 4) in vec4 aParams;
layout(location = 5) in float aZ;

out vec2 vWorldPosition;
out vec4 vColor;
out vec2 vUv;
out vec4 vParams;

void main()
{
  vWorldPosition = aWorldPosition;
  vColor = aColor;
  vUv = aUv;
  vParams = aParams;

  gl_Position = vec4(aPosition, aZ, 1.0);
}
