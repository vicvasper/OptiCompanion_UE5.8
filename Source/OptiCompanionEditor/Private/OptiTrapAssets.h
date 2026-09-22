#pragma once

#include "CoreMinimal.h"

class UBlueprint;
class UMaterial;
class UStaticMesh;
class UTexture2D;

namespace OptiTrap
{
	/** Assets that are heavy on purpose, created under /Game/OptiTrap for the trap level. */
	struct FHeavyAssets
	{
		UTexture2D* Noise4K = nullptr;       // 4096x4096, never streams
		UTexture2D* HdrUncompressed = nullptr; // 2048x2048 RGBA16F, no compression
		UMaterial* HeavyOpaque = nullptr;    // hundreds of instructions and dependent texture reads
		UMaterial* HeavyGlass = nullptr;     // translucent with per-pixel lighting, meant to be stacked
		UStaticMesh* DenseMesh = nullptr;    // ~260k triangles, no LODs, no Nanite
		UBlueprint* HeavyProp = nullptr;     // dense meshes, shadowed lights and GetAllActorsOfClass on Tick

		bool IsValid() const { return Noise4K && HdrUncompressed && HeavyOpaque && HeavyGlass && DenseMesh && HeavyProp; }
	};

	/** Creates (or reuses, when they already exist) the heavy assets and saves them. */
	FHeavyAssets CreateHeavyAssets();
}
