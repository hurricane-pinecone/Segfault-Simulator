layout(location = 0) in vec2 aWorldPos;
layout(location = 1) in float aElevation;
layout(location = 2) in vec2 aUv;
layout(location = 3) in vec4 aColor;
layout(location = 4) in float aSortKey;

out vec2 vUv;
out vec4 vColor;
out vec2 vWorldPos;
out float vGround;

uniform vec2 uTileSize;       // (tileWidth, tileHeight)
uniform float uWorldScale;
uniform float uZoom;
uniform vec2 uCameraIso;
uniform vec2 uScreenCenter;
uniform float uElevationStep;
uniform vec2 uNdcScale;       // (2/width, 2/height)
uniform float uDepthMin;
uniform float uDepthInvRange;

void main()
{
  vUv = aUv;
  vColor = aColor;
  vWorldPos = aWorldPos;
  vGround = aElevation;

  vec2 iso = vec2(aWorldPos.x - aWorldPos.y, aWorldPos.x + aWorldPos.y)
             * uTileSize * uWorldScale * 0.5;
  vec2 screen = (iso - uCameraIso) * uZoom + uScreenCenter;
  screen.y -= aElevation * uElevationStep * uWorldScale * uZoom;

  vec2 ndc = vec2(screen.x * uNdcScale.x - 1.0, 1.0 - screen.y * uNdcScale.y);

  float t = clamp((aSortKey - uDepthMin) * uDepthInvRange, 0.0, 1.0);
  float clipZ = 0.9 - 1.8 * t;

  gl_Position = vec4(ndc, clipZ, 1.0);
}
