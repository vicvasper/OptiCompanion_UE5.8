#pragma once

#include "CoreMinimal.h"

struct FOptiStatResult;

namespace OptiAnalysis
{
	/** Per-block column means extracted from a probe CSV. Index 0 of each row is the EVENTS column and is unused. */
	struct FBlockMeans
	{
		TArray<FString> Columns;
		TArray<TArray<double>> A;
		TArray<TArray<double>> B;
	};

	/** Event markers written by FOptiProbe into the CSV EVENTS column. */
	inline const TCHAR* BeginEventPrefix = TEXT("OPTI_BEGIN_");
	inline const TCHAR* EndEventPrefix = TEXT("OPTI_END_");
	/** A block interrupted halfway (camera moved, compile started): its frames are discarded. */
	inline const TCHAR* AbortEventPrefix = TEXT("OPTI_ABORT_");

	/** Reads a CSV profiler file and averages every column inside each OPTI_BEGIN/OPTI_END block. */
	OPTICOMPANION_API bool ParseProbeCsv(const FString& Path, FBlockMeans& Out, FString& OutError);

	/** Compares the A and B block means of one column. Bootstrap resamples blocks, never frames, because frames are autocorrelated. */
	OPTICOMPANION_API FOptiStatResult CompareColumn(const FString& Name, const TArray<double>& A, const TArray<double>& B, int32 Seed, int32 Resamples = 2000);

	/** Columns worth reporting: frame, thread and GPU totals, every GPU pass and RHI counters. */
	OPTICOMPANION_API bool IsReportedColumn(const FString& Name);

	/** RHI columns are counts (draw calls, primitives), everything else reported is milliseconds. */
	inline bool IsCounterColumn(const FString& Name) { return Name.StartsWith(TEXT("RHI/")); }
}
