// Bakes a decal into a paint texture. `aLocal` is the target surface's [0,1]
// coordinate, mapped straight to NDC so the splat lands at the right texel; no
// camera projection or depth -- this renders flat into an offscreen texture.
layout(location = 0) in vec2 aLocal;
layout(location = 1) in vec2 aUv;
layout(location = 2) in vec4 aColor;

out vec2 vUv;
out vec4 vColor;

void main()
{
  vUv = aUv;
  vColor = aColor;
  gl_Position = vec4(aLocal * 2.0 - 1.0, 0.0, 1.0);
}
