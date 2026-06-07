#include "../../testHarness.h"

#include <engine/core/scripting/iLuaConfigurable.h>
#include <engine/core/scripting/luaSchema.h>
#include <engine/core/scripting/luaScripting.h>

#include <stdexcept>
#include <string>

namespace
{
bool contains(const std::string& haystack, const std::string& needle)
{
  return haystack.find(needle) != std::string::npos;
}

// A configurable used to exercise the ILuaConfigurable + schema path end to end.
struct Range2
{
  float min = 0.0f;
  float max = 0.0f;
};

struct TestConfig : sfs::ILuaConfigurable
{
  int count = 1;
  float speed = 2.0f;
  bool flag = false;
  Range2 span;
  int changedCount = 0;

  std::string luaConfigName() const override { return "cfg"; }

  sfs::LuaSchema luaConfigSchema() const override
  {
    return {
        sfs::field("count", &TestConfig::count, "int"),
        sfs::field("speed", &TestConfig::speed, "float"),
        sfs::field("flag", &TestConfig::flag, "bool"),
        sfs::rangeField("span", &TestConfig::span, "range"),
    };
  }

  void* luaConfigData() override { return this; }
  void onLuaConfigChanged() override { ++changedCount; }
};
} // namespace

int main()
{
  sfs::LuaScripting vm;

  TEST("the VM should initialise")
  {
    CHECK(vm.init());
  }

  TEST("eval and evalRepl should handle expressions and report errors")
  {
    CHECK(vm.evalRepl("return 1 + 1") == "2");
    CHECK(contains(vm.evalRepl("return 1 +"), "error:"));  // compile error
    CHECK(contains(vm.evalRepl("error('boom')"), "boom")); // runtime error caught
    CHECK(vm.eval("x = 5").empty());                       // statements -> ""
    CHECK(vm.evalRepl("return x") == "5");                 // state persists
  }

  TEST("the execution guard should abort an infinite loop")
  {
    // reaching past this at all proves it returned rather than hung
    CHECK(contains(vm.evalRepl("while true do end"), "error:"));
    // a finite loop under budget still runs fine
    CHECK(
        vm.evalRepl("local s = 0; for i = 1, 1000 do s = s + i end; return s") ==
        "500500");
  }

  TEST("the sandbox should hide unsafe globals")
  {
    CHECK(vm.evalRepl("return load") == "nil");
    CHECK(vm.evalRepl("return loadfile") == "nil");
    CHECK(vm.evalRepl("return dofile") == "nil");
    CHECK(vm.evalRepl("return io") == "nil");
    CHECK(vm.evalRepl("return os") == "nil");
  }

  TEST("a bound function should run and contain a thrown exception")
  {
    int counter = 0;
    vm.bind("inc", [&counter] { ++counter; });
    vm.eval("inc(); inc()");
    CHECK(counter == 2);

    vm.bind("boom", [] { throw std::runtime_error("kaboom"); });
    CHECK(contains(vm.evalRepl("boom()"), "kaboom")); // becomes a Lua error
    CHECK(true); // reached here == no UB/crash crossing the C boundary
  }

  TestConfig cfg;

  TEST("a configurable should round-trip through its generated table")
  {
    vm.registerConfig(cfg);
    CHECK(vm.eval("cfg.set{ count = 5, speed = 3.5, flag = true, "
                  "spanMin = 1, spanMax = 2 }")
              .empty());
    CHECK(cfg.count == 5);
    CHECK(cfg.flag == true);
    CHECK(cfg.span.min == 1.0f && cfg.span.max == 2.0f);
    CHECK(cfg.changedCount == 1);                        // onLuaConfigChanged fired
    CHECK(vm.evalRepl("return cfg.get().count") == "5"); // values round-trip
    CHECK(contains(vm.evalRepl("return cfg.options.count"), "int")); // schema doc
  }

  TEST("unregister should neutralise a lingering reference")
  {
    vm.eval("saved = cfg"); // Lua still holds the table after unregister
    vm.unregisterConfig(cfg);
    CHECK(vm.evalRepl("return cfg") == "nil");               // global cleared
    CHECK(vm.evalRepl("return saved.get().count") == "nil"); // slot neutered
  }

  TEST("the memory cap should turn a huge allocation into an error")
  {
    sfs::LuaScripting memVm;
    CHECK(memVm.init());
    memVm.setMemoryLimit(4 * 1024 * 1024); // 4 MiB
    const std::string r =
        memVm.evalRepl("return string.rep('x', 50 * 1024 * 1024)");
    CHECK(contains(r, "error:"));                 // out-of-memory, caught
    CHECK(memVm.memoryUsed() <= 4 * 1024 * 1024); // cap never blown
  }

  TEST("the output cap should truncate a giant print")
  {
    sfs::LuaScripting outVm;
    CHECK(outVm.init());
    outVm.setOutputLimit(128);
    const std::string r = outVm.evalRepl("print(string.rep('x', 5000))");
    CHECK(contains(r, "truncated"));
    CHECK(r.size() < 5000); // nowhere near the raw output size
  }

  return testing::report("luaTests");
}
