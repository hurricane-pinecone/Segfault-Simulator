layout(location = 0) in vec2 aPosition;
layout(location = 1) in vec4 aColor;
layout(location = 2) in float aZ;

out vec4 vColor;

void main()
{
  vColor = aColor;
  gl_Position = vec4(aPosition, aZ, 1.0);
}
