#pragma once

#include "CoreMinimal.h"
#include "OptiProbe.h"
#include "UObject/WeakObjectPtrTemplates.h"

struct FOptiProfile;
class IConsoleVariable;
class UActorComponent;
class UWorld;

/** Sectors of the central-complex ring. Neighbours on the ring are related, so focus drifts to similar work. */
enum class EOptiSector : uint8
{
	Shadows,
	Lighting,
	Reflections,
	Effects,
	PostProcess,
	Atmosphere,
	Geometry,
	CPU,
	Count
};

OPTICOMPANION_API const TCHAR* LexToString(EOptiSector Sector);

struct OPTICOMPANION_API FOptiCVarChange
{
	enum class EOp : uint8 { Set, Multiply, Add };

	FString Name;
	EOp Op = EOp::Set;
	double Value = 0.0;
	double Min = -DBL_MAX;
	double Max = DBL_MAX;
	/** For Set: only applies when the current value is above (or below, if negative) this threshold. */
	TOptional<double> OnlyIfAbove;
	/** Many CVars use 0 or -1 for "engine default"; scaling that is meaningless, so this value is proposed instead. */
	TOptional<double> WhenDefault;
};

/**
 * A change to specific components of the level, set through reflection. During a nap it is applied in
 * memory only and undone afterwards; accepted findings are applied as an undoable editor transaction.
 */
struct OPTICOMPANION_API FOptiSceneRule
{
	UClass* ComponentClass = nullptr;
	FName Property;
	/** Which components the rule touches (e.g. lights that cast shadows and are dim). */
	TFunction<bool(const UActorComponent&)> Filter;
	/** The proposed value, as property text. */
	TFunction<FString(const UActorComponent&)> NewValue;
};

/**
 * One experiment the fly can run: console-variable changes and/or per-component scene changes, plus the
 * innate knowledge the lateral horn has about it (how much it usually saves and how risky it looks).
 */
struct OPTICOMPANION_API FOptiAction
{
	FName Id;
	EOptiSector Sector = EOptiSector::Shadows;
	TArray<FOptiCVarChange> Changes;
	TArray<FOptiSceneRule> SceneRules;
	/** Pass glomeruli (EOptiGlom) whose GPU time this action targets. Empty means CPU thread time. */
	TArray<int32> Passes;
	/** Innate expectation: fraction of the targeted time usually saved. */
	float TypicalSaving = 0.2f;
	/** Innate expectation: how likely the change is to be visible, 0..1. */
	float VisualRisk = 0.3f;
	/** CVars that must hold a given value for the action to make sense (e.g. VSM on). */
	TArray<TPair<FString, FString>> Requires;
	/** False for settings the editor viewport ignores, so naps don't learn from a change that did nothing. */
	bool bMeasurableInEditor = true;
	/** Only testable while the game runs (Blueprint logic): never picked for an editor experiment. */
	bool bPlayOnly = false;

	bool IsSceneAction() const { return !SceneRules.IsEmpty(); }

	/**
	 * Builds the A (current) and B (changed) console-variable variants. Returns false when the action cannot
	 * apply right now. Scene actions need the world and at least one component the rules would change.
	 */
	bool BuildVariants(FOptiCVarSet& OutA, FOptiCVarSet& OutB, UWorld* World = nullptr) const;

	/** Milliseconds of the profile this action can act on. */
	double TargetMs(const FOptiProfile& Profile) const;
};

/** Finds a console variable, following renamed ("shadow") CVars and skipping deprecated ones. */
OPTICOMPANION_API IConsoleVariable* OptiFindCVar(const FString& Name, FString* OutRealName = nullptr);

namespace OptiActions
{
	OPTICOMPANION_API const TArray<FOptiAction>& Catalog();
	OPTICOMPANION_API const FOptiAction* Find(FName Id);
	OPTICOMPANION_API int32 IndexOf(FName Id);

	/** Logs every action and whether it can run on this engine and project (Opti.Catalog). */
	OPTICOMPANION_API void LogCatalog(UWorld* World);
}

/** One component property changed by a scene action. */
struct OPTICOMPANION_API FOptiSceneEdit
{
	TWeakObjectPtr<UActorComponent> Component;
	FString ComponentPath;
	FName Property;
	FString Before;
	FString After;
};

namespace OptiScene
{
	/** Components the action's rules would change right now, with their current and proposed values. */
	OPTICOMPANION_API TArray<FOptiSceneEdit> Collect(const FOptiAction& Action, UWorld* World, int32 MaxEdits = 20000);

	/** Transient switch between the current (A) and proposed (B) values. Does not dirty any package. */
	OPTICOMPANION_API void SetVariant(const TArray<FOptiSceneEdit>& Edits, bool bVariantB);

	/** Sets one value and refreshes the component's render state; shared by the transient and the transacted paths. */
	OPTICOMPANION_API bool SetValue(UActorComponent& Component, FName Property, const FString& Value);
	OPTICOMPANION_API FString GetValue(const UActorComponent& Component, FName Property);

	OPTICOMPANION_API FString Serialize(const TArray<FOptiSceneEdit>& Edits);
	OPTICOMPANION_API TArray<FOptiSceneEdit> Deserialize(const FString& Text);
}
