#include "../../testHarness.h"

#include <engine/core/components/meshRendererComponent.h>
#include <engine/core/components/transformComponent.h>
#include <engine/core/ecs/ecs.h>
#include <engine/core/reflection/reflection.h>
#include <engine/core/reflection/registration.h>

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

struct EmptyMark
{
};

struct OnlyHidden
{
  [[sfs::hide]] int count = 0;
};
} // namespace

int main()
{
  TEST("the engine's components should be registered with their fields")
  {
    const reflect::TypeInfo* transform =
        reflect::types().find("TransformComponent");
    CHECK(transform != nullptr);
    CHECK(transform->kind == reflect::TypeKind::Component);
    CHECK(transform->displayName == std::string("Transform"));
    CHECK(transform->module == std::string("engine"));
    CHECK(transform->fields.size() == 3u);
    CHECK(findField(*transform, "rotation")->kind == reflect::FieldKind::Quat);

    CHECK(reflect::types().find("NameComponent") != nullptr);
    CHECK(reflect::types().find("PointLightComponent") != nullptr);
  }

  TEST("attributes should carry through from the headers")
  {
    const reflect::TypeInfo* renderer =
        reflect::types().find("MeshRendererComponent");
    CHECK(findField(*renderer, "tint")->attributes.color);
    CHECK(findField(*renderer, "mesh")->kind == reflect::FieldKind::Mesh);

    const reflect::TypeInfo* light =
        reflect::types().find("PointLightComponent");
    const reflect::FieldInfo* intensity = findField(*light, "intensity");
    CHECK(intensity->attributes.hasRange);
    CHECK(testing::approx(intensity->attributes.max, 10.0f));
  }

  TEST("component ops should add, find, edit and remove on an entity")
  {
    Registry registry;
    Entity entity = registry.createEntity();
    const reflect::TypeInfo* transform =
        reflect::types().find("TransformComponent");

    CHECK(!transform->component.has(entity));
    CHECK(transform->component.get(entity) == nullptr);

    transform->component.add(entity);
    CHECK(transform->component.has(entity));

    void* object = transform->component.get(entity);
    CHECK(object == entity.getComponent<TransformComponent>());
    findField(*transform, "position")->in<glm::vec3>(object) = {
        1.0f, 2.0f, 3.0f};
    CHECK(testing::approx(
        entity.getComponent<TransformComponent>()->position.y, 2.0f));

    transform->component.remove(entity);
    CHECK(!entity.hasComponent<TransformComponent>());
    CHECK(transform->component.get(entity) == nullptr);
    CHECK(transform->component.get(Entity()) == nullptr);
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

  TEST("an empty component should be a tag; one with hidden data should not")
  {
    CHECK(reflect::componentType<EmptyMark>("EmptyMark", "Empty Mark", "test")
              .tag);
    CHECK(
        !reflect::componentType<OnlyHidden>("OnlyHidden", "Only Hidden", "test")
             .tag);
    const reflect::TypeInfo* solid = reflect::types().find("SolidObject");
    CHECK(solid != nullptr && solid->tag);
    CHECK(!reflect::types().find("TransformComponent")->tag);
  }

  return testing::report("reflectionTests");
}
