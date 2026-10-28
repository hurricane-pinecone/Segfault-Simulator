#include "engine/core/reflection/reflection.h"

// Builds without sfs-codegen (core-only, web) have no generated engine
// reflection; the registry starts empty.
void sfs::reflect::registerEngineTypes(TypeRegistry&) {}
