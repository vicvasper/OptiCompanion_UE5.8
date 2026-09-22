#pragma once

#include "CoreMinimal.h"

class UWorld;
class UMaterialInterface;
class UPrimitiveComponent;

/** A camera to filter by, so the fly smells and lists only what is on screen. */
struct OPTICOMPANION_API FOptiView
{
	FVector Location = FVector::ZeroVector;
	FRotator Rotation = FRotator::ZeroRotator;
	float FOVDegrees = 90.f;
	float AspectRatio = 16.f / 9.f;
	bool bValid = false;

	/** True if the sphere touches the view frustum (occlusion is ignored). */
	bool SeesSphere(const FVector& Center, float Radius) const;

private:
	mutable bool bFrustumBuilt = false;
	mutable TArray<FPlane> Planes;
	void BuildFrustum() const;
};

/** One asset or actor in the inventory, with why it may be expensive. */
struct OPTICOMPANION_API FOptiInventoryItem
{
	enum class EType : uint8 { StaticMesh, SkeletalMesh, Texture, Material, Light, Audio, PostProcess, Blueprint };

	EType Type = EType::StaticMesh;
	FString Name;
	FString Path;       // asset or actor path, to select it in the editor
	float MemoryMB = 0.f;
	float CostMs = 0.f; // Blueprints: game-thread milliseconds per frame, measured in Play
	FString Details;
	FString Warning;    // empty when nothing stands out
	int32 Severity = 0; // 0 fine, 1 worth a look, 2 likely expensive
	int32 Uses = 0;     // how many components in view use it
};

struct OPTICOMPANION_API FOptiInventory
{
	TArray<FOptiInventoryItem> Items;
	bool bVisibleOnly = false;
	FDateTime Time;

	float TotalMemoryMB(FOptiInventoryItem::EType Type) const;
	int32 Count(FOptiInventoryItem::EType Type) const;
	bool ExportJson(const FString& Path) const;
};

/**
 * Static resource analysis of the level, adapted from OptiLogger (Victor Rivas,
 * github.com/vicvasper/Optilogger_UE5.x): per-asset inventory, memory estimates with the same
 * per-element assumptions, and a visible-only filter. Where OptiLogger reports, OptiCompanion also
 * flags what usually costs frame time, and feeds the totals to the fly's antennal lobe.
 */
namespace OptiInventory
{
	/**
	 * Shader instruction counter for materials. Material statistics only exist in editor builds, so the
	 * editor module installs it; without one, material cost is simply left out.
	 */
	OPTICOMPANION_API void SetMaterialInstructionCounter(TFunction<int32(UMaterialInterface*)> Counter);
	OPTICOMPANION_API int32 PixelInstructions(UMaterialInterface* Material);

	/** Blocking pass over the level. View filters to what is on screen when valid. */
	OPTICOMPANION_API FOptiInventory Collect(UWorld* World, const FOptiView& View);

	/** Same memory model as OptiLogger's EstimateTextureMemoryUsage. */
	OPTICOMPANION_API float EstimateTextureMB(int32 Width, int32 Height, uint8 CompressionSettings, int32 Mips);

	OPTICOMPANION_API const TCHAR* LexToString(FOptiInventoryItem::EType Type);
}
