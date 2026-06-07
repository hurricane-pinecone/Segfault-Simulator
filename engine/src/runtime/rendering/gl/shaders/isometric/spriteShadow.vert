layout(location = 0) in vec2 aPosition;
layout(location = 1) in vec2 aUv;
layout(location = 2) in vec4 aColor;
layout(location = 3) in float aZ;

out vec2 vUv;
out vec4 vColor;

void main()
{
  vUv = aUv;
  vColor = aColor;
  gl_Position = vec4(aPosition, aZ, 1.0);
}
