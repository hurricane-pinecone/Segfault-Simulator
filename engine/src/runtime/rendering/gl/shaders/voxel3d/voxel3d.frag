// Flat-lit coloured cubes (the Teardown look): a single directional term over an
// ambient floor, shaded by the flat face normal so each face reads as a facet.
// #version (+ ES precision) is prepended at runtime.
in vec3 vNormal;
in vec4 vColor;

out vec4 FragColor;

uniform vec3 uLightDir; // direction TOWARD the light, world space

void main()
{
  vec3 n = normalize(vNormal);
  float d = max(dot(n, normalize(uLightDir)), 0.0);
  float shade = 0.4 + 0.6 * d;
  FragColor = vec4(vColor.rgb * shade, vColor.a); // alpha is per-voxel
}
