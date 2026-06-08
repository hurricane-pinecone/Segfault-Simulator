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

uniform float uTime; // shared with the fragment stage

void main()
{
  vWorldPosition = aWorldPosition;
  vColor = aColor;
  vUv = aUv;
  vParams = aParams;

  // Gentle crossing swells so the surface reads as moving water with real
  // undulation, not a flat plane. Long wavelengths look smooth even at one quad
  // per tile. The offset is a clip-space vertical shift (up = +y in iso); kept
  // small so it laps the shore without tearing through terrain.
  float swell =
      sin(aWorldPosition.x * 0.6 + uTime * 1.3) +
      sin(aWorldPosition.y * 0.5 - uTime * 0.9) +
      sin((aWorldPosition.x + aWorldPosition.y) * 0.3 + uTime * 0.7) * 0.6;

  gl_Position = vec4(aPosition.x, aPosition.y + swell * 0.006, aZ, 1.0);
}
