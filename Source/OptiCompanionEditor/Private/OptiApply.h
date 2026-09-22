#pragma once

#include "CoreMinimal.h"
#include "OptiProbe.h"

struct FOptiFinding;

/**
 * Applies accepted findings. Console-variable findings go to Config/DefaultEngine.ini [SystemSettings]
 * (checked out first if source control is on) and take effect immediately in the editor. The ini is
 * edited as text so comments and ordering in the file are preserved.
 */
namespace OptiApply
{
	bool Apply(FOptiFinding& Finding, FText& OutError);
	bool Undo(FOptiFinding& Finding, FText& OutError);

	/** True when every console variable currently has the value in Set. */
	bool CurrentMatches(const FOptiCVarSet& Set);

	/** True when every component of a scene finding still exists and holds its After (or Before) value. */
	bool SceneMatches(const FString& SceneEdits, bool bAfter);

	FString ConfigPath();

	/** True when the Blueprint's class defaults hold the To (or From) value of a Blueprint finding. */
	bool BlueprintMatches(const FOptiFinding& Finding, bool bAfter);
}
