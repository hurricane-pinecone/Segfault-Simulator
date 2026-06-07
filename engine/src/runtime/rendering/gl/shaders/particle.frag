in vec2 vUv;
in vec4 vColor;

out vec4 FragColor;

uniform sampler2D uTexture;

void main()
{
  // Unlit: the particle's colour fully modulates the sprite texture (RGB and
  // alpha). Premultiply-free; the blend mode decides how it combines.
  vec4 tex = texture(uTexture, vUv);
  FragColor = tex * vColor;

  if (FragColor.a <= 0.0)
    discard;
}
