#include "engine/core/rendering/3d/mesh.h"

namespace sfs
{

Aabb MeshData::bounds() const
{
  Aabb box;
  for (const MeshVertex& vertex : vertices)
    box.expand(vertex.position);
  return box;
}

} // namespace sfs
