// A game module with nothing in it, for moduleHostTests: built once as
// sfs_add_game_module builds modules (SFS_GAME_MODULE: it reports the engine
// ABI it was built against) and once without, as modules were before they
// did.
#include "engine/runtime/scripting/gameModule.h"

void sfsRegisterModule(sfs::reflect::TypeRegistry&) {}

#ifdef SFS_GAME_MODULE
SFS_DEFINE_MODULE_ENGINE_ABI()
#endif
