#include "../../testHarness.h"

#include <engine/runtime/rendering/backend/renderBackends.h>

using namespace sfs;

namespace
{
// A backend that only knows its name.
class Named : public IRenderBackend
{
public:
  explicit Named(std::string name) : m_name(std::move(name)) {}
  std::string_view name() const override { return m_name; }
  bool init(const char*, int, int) override { return false; }
  SDL_Window* window() const override { return nullptr; }
  void onResize(int, int) override {}
  SceneServices* sceneServices() override { return nullptr; }
  void beginFrame(int, int) override {}
  void endFrame() override {}
  void shutdown() override {}

private:
  std::string m_name;
};
} // namespace

int main()
{
  TEST("backends should be offered by name, in the order added")
  {
    RenderBackends::add("first", [] { return std::make_unique<Named>("1"); });
    RenderBackends::add("second", [] { return std::make_unique<Named>("2"); });
    const std::vector<std::string> names = RenderBackends::names();
    CHECK(names.size() == 2u);
    CHECK(names[0] == "first" && names[1] == "second");
    CHECK(RenderBackends::make("second")->name() == "2");
    CHECK(RenderBackends::make("missing") == nullptr);
  }

  TEST("adding a name again should replace its backend, keeping its place")
  {
    RenderBackends::add("first", [] { return std::make_unique<Named>("1b"); });
    CHECK(RenderBackends::names().front() == "first");
    CHECK(RenderBackends::make("first")->name() == "1b");
  }

  return testing::report("renderBackendsTests");
}
