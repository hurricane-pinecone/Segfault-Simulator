# Fire spread

The GPU voxel world includes a fire simulation that runs automatically as a cellular automaton on the GPU each frame. No API call is needed to sustain a fire once it is burning; the simulation spreads and burns out on its own.

## How fire propagates

Fire spreads by sampling neighbours each frame. A voxel catches fire when a burning neighbour is present and a per-frame probability roll succeeds. The roll is weighted by the material's flammability and the simulation step time, so fire spreads at a consistent real-time rate regardless of frame rate.

When a flammable solid finishes burning it either crumbles to falling powder or chars in place. Leaves and similar light materials are consumed entirely. These burn rates and outcomes are baked into the material palette; you do not configure them per call.

## Tree felling

When fire burns through structural voxels such as tree trunks it may sever the canopy from the ground. The engine detects this automatically:

- Each time a load-bearing voxel is removed by fire, there is a small chance the engine runs a connectivity pass centred on that cell.
- The pass uses a 96-voxel-cube window to classify which voxels above the cut are still connected to the ground. Any disconnected mass is promoted to one or more rigid bodies that fall under gravity.
- Multiple fell events can be triggered in the same frame, so a fire spreading through a dense area can drop several structures simultaneously.
- Fire-triggered felling runs on its own work-list, entirely separate from the carve tool's fell detection. Carving terrain while a fire is burning nearby does not suppress or delay tree falls triggered by the fire.

## Relationship to rigid bodies and carving

Pieces detached by fire join the same rigid body pool as chunks severed by the carve tool. See [Rigid bodies](../index.md#rigid-bodies) on the GPU voxel world page for how those bodies fall, collide, and come to rest.
