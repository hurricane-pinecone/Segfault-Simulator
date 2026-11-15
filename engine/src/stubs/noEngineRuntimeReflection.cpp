#include "engine/runtime/scripting/scriptRegistry.h"

// Builds without sfs-codegen have no generated runtime reflection.
void sfs::reflect::registerEngineRuntimeTypes(TypeRegistry&) {}
