#include "../../engine/tests/testHarness.h"

#include "editorContext.h"

#include <engine/core/ecs/registry.h>

using namespace sfs;

int main()
{
  TEST("several selected should count only while the group holds the one "
       "selected; selecting anything else selects just that")
  {
    Registry registry;
    const Entity a = registry.createEntity();
    const Entity b = registry.createEntity();
    const Entity c = registry.createEntity();
    registry.flushEntities();

    EditorContext context;
    CHECK(context.selected().empty());

    context.selection = b;
    CHECK(context.selected() == std::vector<Entity>{b});

    // Several: the one selected first, then the rest.
    context.selectionGroup = {a, b, c};
    CHECK((context.selected() == std::vector<Entity>{b, a, c}));
    CHECK(context.isSelected(a) && context.isSelected(c));

    // Selected elsewhere (the viewport): just it.
    context.selection = c;
    context.selectionGroup = {a, b};
    CHECK(context.selected() == std::vector<Entity>{c});
    CHECK(!context.isSelected(a));

    // Deleted ones drop out.
    context.selection = a;
    context.selectionGroup = {a, b, c};
    registry.destroyEntity(b);
    registry.flushEntities();
    CHECK((context.selected() == std::vector<Entity>{a, c}));

    context.selection = {};
    CHECK(context.selected().empty() && !context.isSelected(a));
  }

  return testing::report("selectionTests");
}
