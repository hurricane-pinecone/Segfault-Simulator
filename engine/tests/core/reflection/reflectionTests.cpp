#include "../../testHarness.h"

#include <engine/core/components/meshRendererComponent.h>
#include <engine/core/components/transform3DComponent.h>
#include <engine/core/ecs/ecs.h>
#include <engine/core/reflection/reflection.h>

#include <algorithm>
#include <string>

using namespace sfs;

namespace
{
const reflect::FieldInfo* findField(const reflect::TypeInfo& type,
                                    std::string_view name)
{
  const auto it =
      std::find_if(type.fields.begin(),
                   type.fields.end(),
                   [&](const reflect::FieldInfo& f) { return f.name == name; });
  return it != type.fields.end() ? &*it : nullptr;
}
} // namespace

int main()
{
  TEST("the engine's components should be registered with their fields")
  {
    const reflect::TypeInfo* transform =
        reflect::types().find("Transform3DComponent");
    CHECK(transform != nullptr);
    CHECK(transform->kind == reflect::TypeKind::Component);
    CHECK(transform->displayName == std::string("Transform 3D"));
    CHECK(transform->module == std::string("engine"));
    CHECK(transform->fields.size() == 3u);
    CHECK(findField(*transform, "rotation")->kind == reflect::FieldKind::Quat);

    CHECK(reflect::types().find("NameComponent") != nullptr);
    CHECK(reflect::types().find("PointLight3DComponent") != nullptr);
  }

  TEST("attributes should carry through from the headers")
  {
    const reflect::TypeInfo* renderer =
        reflect::types().find("MeshRendererComponent");
    CHECK(findField(*renderer, "tint")->attributes.color);
    CHECK(findField(*renderer, "mesh")->kind == reflect::FieldKind::Mesh);

    const reflect::TypeInfo* light =
        reflect::types().find("PointLight3DComponent");
    const reflect::FieldInfo* intensity = findField(*light, "intensity");
    CHECK(intensity->attributes.hasRange);
    CHECK(testing::approx(intensity->attributes.max, 10.0f));
  }

  TEST("component ops should add, find, edit and remove on an entity")
  {
    Registry registry;
    Entity entity = registry.createEntity();
    const reflect::TypeInfo* transform =
        reflect::types().find("Transform3DComponent");

    CHECK(!transform->component.has(entity));
    CHECK(transform->component.get(entity) == nullptr);

    transform->component.add(entity);
    CHECK(transform->component.has(entity));

    void* object = transform->component.get(entity);
    findField(*transform, "position")->in<glm::vec3>(object) = {
        1.0f, 2.0f, 3.0f};
    CHECK(testing::approx(
        entity.getComponent<Transform3DComponent>().position.y, 2.0f));

    transform->component.remove(entity);
    CHECK(!entity.hasComponent<Transform3DComponent>());
  }

  TEST("a module's types should be removable as a unit")
  {
    reflect::TypeRegistry registry;
    reflect::TypeInfo a;
    a.name = "A";
    a.module = "game";
    reflect::TypeInfo b;
    b.name = "B";
    b.module = "other";
    registry.add(a);
    registry.add(b);

    registry.removeModule("game");
    CHECK(registry.find("A") == nullptr);
    CHECK(registry.find("B") != nullptr);
  }

  TEST("registering a type again should replace it")
  {
    reflect::TypeRegistry registry;
    reflect::TypeInfo first;
    first.name = "A";
    first.displayName = "Old";
    registry.add(first);
    reflect::TypeInfo second;
    second.name = "A";
    second.displayName = "New";
    registry.add(second);

    CHECK(registry.types().size() == 1u);
    CHECK(registry.find("A")->displayName == std::string("New"));
  }

  return testing::report("reflectionTests");
}
