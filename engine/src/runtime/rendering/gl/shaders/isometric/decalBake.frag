// Stamps one decal sprite into the paint texture. Output is premultiplied
// alpha: the paint layer accumulates with glBlendFunc(GL_ONE,
// GL_ONE_MINUS_SRC_ALPHA), so repeated splats of the same colour saturate (and
// a different colour paints over) without ever growing memory.
in vec2 vUv;
in vec4 vColor;

out vec4 FragColor;

uniform sampler2D uSprite;

void main()
{
  vec4 c = texture(uSprite, vUv) * vColor;
  FragColor = vec4(c.rgb * c.a, c.a);
}
