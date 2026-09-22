#include "OptiAnalysis.h"
#include "OptiProbe.h"
#include "Misc/FileHelper.h"
#include "Math/RandomStream.h"

namespace OptiAnalysis
{
	namespace
	{
		struct FBlockAccumulator
		{
			TArray<double> Sum;
			TArray<int32> Count;

			void Reset(int32 NumColumns)
			{
				Sum.Init(0.0, NumColumns);
				Count.Init(0, NumColumns);
			}

			TArray<double> Means() const
			{
				TArray<double> Result;
				Result.Init(0.0, Sum.Num());
				for (int32 Index = 0; Index < Sum.Num(); ++Index)
				{
					Result[Index] = Count[Index] > 0 ? Sum[Index] / Count[Index] : 0.0;
				}
				return Result;
			}

			int32 Frames() const
			{
				int32 Max = 0;
				for (int32 C : Count)
				{
					Max = FMath::Max(Max, C);
				}
				return Max;
			}
		};

		double Mean(const TArray<double>& Values)
		{
			double Sum = 0.0;
			for (double V : Values)
			{
				Sum += V;
			}
			return Values.Num() > 0 ? Sum / Values.Num() : 0.0;
		}

		double StdDev(const TArray<double>& Values)
		{
			if (Values.Num() < 2)
			{
				return 0.0;
			}
			const double M = Mean(Values);
			double Sq = 0.0;
			for (double V : Values)
			{
				Sq += (V - M) * (V - M);
			}
			return FMath::Sqrt(Sq / (Values.Num() - 1));
		}

		double ResampledMean(const TArray<double>& Values, FRandomStream& Rng)
		{
			double Sum = 0.0;
			for (int32 Index = 0; Index < Values.Num(); ++Index)
			{
				Sum += Values[Rng.RandHelper(Values.Num())];
			}
			return Sum / Values.Num();
		}

		/** Removes the "##<seconds>" timestamp suffix and any "Category/" prefix from a CSV event. */
		FString CleanEventText(FString Event)
		{
			int32 HashIndex;
			if (Event.FindChar(TEXT('#'), HashIndex))
			{
				Event.LeftInline(HashIndex);
			}
			int32 SlashIndex;
			if (Event.FindLastChar(TEXT('/'), SlashIndex))
			{
				Event.RightChopInline(SlashIndex + 1);
			}
			Event.TrimStartAndEndInline();
			return Event;
		}
	}

	bool ParseProbeCsv(const FString& Path, FBlockMeans& Out, FString& OutError)
	{
		TArray<FString> Lines;
		if (!FFileHelper::LoadFileToStringArray(Lines, *Path))
		{
			OutError = FString::Printf(TEXT("Could not read CSV '%s'"), *Path);
			return false;
		}

		// Series that appear mid-capture are missing from the first header, so the
		// summary header written at the end of the file is the complete one.
		int32 FirstHeader = INDEX_NONE;
		int32 LastHeader = INDEX_NONE;
		for (int32 Index = 0; Index < Lines.Num(); ++Index)
		{
			if (Lines[Index].StartsWith(TEXT("EVENTS,")) || Lines[Index] == TEXT("EVENTS"))
			{
				if (FirstHeader == INDEX_NONE)
				{
					FirstHeader = Index;
				}
				LastHeader = Index;
			}
		}
		if (FirstHeader == INDEX_NONE)
		{
			OutError = TEXT("CSV has no EVENTS header row");
			return false;
		}

		Lines[LastHeader].ParseIntoArray(Out.Columns, TEXT(","), false);
		const int32 NumColumns = Out.Columns.Num();
		const int32 LastDataLine = LastHeader > FirstHeader ? LastHeader : Lines.Num();

		FBlockAccumulator Block;
		bool bInBlock = false;
		bool bBlockIsB = false;
		TArray<FString> Values;
		TArray<FString> Events;

		for (int32 LineIndex = FirstHeader + 1; LineIndex < LastDataLine; ++LineIndex)
		{
			const FString& Line = Lines[LineIndex];
			if (Line.IsEmpty() || Line.StartsWith(TEXT("[")))
			{
				continue;
			}

			Line.ParseIntoArray(Values, TEXT(","), false);
			if (Values.Num() == 0)
			{
				continue;
			}

			bool bMarkerRow = false;
			Values[0].ParseIntoArray(Events, TEXT(";"), true);
			for (const FString& RawEvent : Events)
			{
				const FString Event = CleanEventText(RawEvent);
				if (Event.StartsWith(BeginEventPrefix))
				{
					bInBlock = true;
					bBlockIsB = Event.EndsWith(TEXT("_B"));
					Block.Reset(NumColumns);
					bMarkerRow = true;
				}
				else if (Event.StartsWith(AbortEventPrefix))
				{
					bInBlock = false;
					bMarkerRow = true;
				}
				else if (Event.StartsWith(EndEventPrefix))
				{
					if (bInBlock && Block.Frames() > 0)
					{
						(bBlockIsB ? Out.B : Out.A).Add(Block.Means());
					}
					bInBlock = false;
					bMarkerRow = true;
				}
			}

			// Marker rows straddle the variant switch, so they are left out of the block.
			if (!bInBlock || bMarkerRow)
			{
				continue;
			}

			const int32 Num = FMath::Min(Values.Num(), NumColumns);
			for (int32 Column = 1; Column < Num; ++Column)
			{
				if (!Values[Column].IsEmpty())
				{
					Block.Sum[Column] += FCString::Atod(*Values[Column]);
					Block.Count[Column]++;
				}
			}
		}

		if (Out.A.Num() < 2 || Out.B.Num() < 2)
		{
			OutError = FString::Printf(TEXT("Not enough valid blocks in CSV (A=%d, B=%d). Was the viewport rendering the whole time?"), Out.A.Num(), Out.B.Num());
			return false;
		}
		return true;
	}

	FOptiStatResult CompareColumn(const FString& Name, const TArray<double>& A, const TArray<double>& B, int32 Seed, int32 Resamples)
	{
		FOptiStatResult Result;
		Result.Name = Name;
		Result.MeanA = Mean(A);
		Result.MeanB = Mean(B);
		Result.Delta = Result.MeanB - Result.MeanA;
		Result.NoiseStdDev = StdDev(A);

		if (A.Num() < 2 || B.Num() < 2)
		{
			return Result;
		}

		FRandomStream Rng(Seed ^ GetTypeHash(Name));
		TArray<double> Deltas;
		Deltas.Reserve(Resamples);
		for (int32 Iteration = 0; Iteration < Resamples; ++Iteration)
		{
			Deltas.Add(ResampledMean(B, Rng) - ResampledMean(A, Rng));
		}
		Deltas.Sort();

		Result.CILow = Deltas[FMath::FloorToInt32(0.025 * (Resamples - 1))];
		Result.CIHigh = Deltas[FMath::CeilToInt32(0.975 * (Resamples - 1))];
		Result.bSignificant = Result.CILow > 0.0 || Result.CIHigh < 0.0;
		return Result;
	}

	bool IsReportedColumn(const FString& Name)
	{
		return Name == TEXT("FrameTime")
			|| Name == TEXT("GameThreadTime")
			|| Name == TEXT("RenderThreadTime")
			|| Name == TEXT("RHIThreadTime")
			|| Name == TEXT("GPUTime")
			|| Name.StartsWith(TEXT("GPU/"))
			|| Name.StartsWith(TEXT("RHI/"));
	}
}
