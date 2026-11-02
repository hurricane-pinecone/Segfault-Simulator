#include "../../testHarness.h"

#include <engine/runtime/scripting/gameModule.h>
#include <engine/runtime/scripting/moduleHost.h>

#include <cstring>
#include <string>

using namespace sfs;

int main()
{
  TEST("the engine ABI should name its version and compiler")
  {
    const std::string abi = kEngineAbi;
    CHECK(abi.starts_with("sfs-"));
    CHECK(abi.size() > std::strlen("sfs-1-"));
  }

  TEST("a module built against this engine should load")
  {
    ModuleHost host;
    host.setQuiet(true);
    CHECK(host.load(SFS_CURRENT_MODULE, "current"));
    CHECK(host.loaded());
    host.unload();
    CHECK(!host.loaded());
  }

  TEST("a module that doesn't report this engine's ABI should be refused")
  {
    ModuleHost host;
    CHECK(!host.load(SFS_STALE_MODULE, "stale"));
    CHECK(!host.loaded());
  }

  return testing::report("moduleHostTests");
}
