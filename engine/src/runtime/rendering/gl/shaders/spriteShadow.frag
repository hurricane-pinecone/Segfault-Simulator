in vec2 vUv;
in vec4 vColor;

out vec4 FragColor;

uniform sampler2D uTexture;

void main()
{
  // Silhouette alpha comes from the sprite texture; the tint (typically black)
  // comes from the per-vertex color.
  float silhouette = texture(uTexture, vUv).a;

  if (silhouette <= 0.0)
    discard;

  FragColor = vec4(vColor.rgb, vColor.a * silhouette);
}
