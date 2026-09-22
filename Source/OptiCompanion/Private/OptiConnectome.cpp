#include "OptiConnectome.h"
#include "OptiProbe.h"
#include "OptiSmell.h"
#include "Interfaces/IPluginManager.h"
#include "Math/RandomStream.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

namespace
{
	void ComputeHash(FOptiConnectome& Connectome)
	{
		uint32 Hash = GetTypeHash(Connectome.KenyonCells.Num());
		for (const TArray<FOptiConnectome::FClaw>& Cell : Connectome.KenyonCells)
		{
			for (const FOptiConnectome::FClaw& Claw : Cell)
			{
				Hash = HashCombine(Hash, HashCombine(GetTypeHash(Claw.Glomerulus), GetTypeHash(FMath::RoundToInt32(Claw.Weight * 100.f))));
			}
		}
		Connectome.Hash = Hash;
	}

	/** "DA1_lPN", "DA1 adPN" or "DA1" -> "DA1". */
	FString GlomerulusFromCellType(const FString& CellType)
	{
		FString Name = CellType;
		int32 Cut;
		if (Name.FindChar(TEXT('_'), Cut) || Name.FindChar(TEXT(' '), Cut))
		{
			Name.LeftInline(Cut);
		}
		return Name;
	}
}

FOptiConnectome FOptiConnectome::Statistical(int32 NumKenyonCells, int32 Seed)
{
	FOptiConnectome Connectome;
	Connectome.Source = TEXT("Statistical wiring (claw statistics of Caron et al. 2013)");
	FRandomStream Rng(Seed);

	// Three KC classes with slightly different claw counts: gamma, alpha/beta, alpha'/beta'.
	struct FClass { float Share; float MeanClaws; };
	const FClass Classes[] = { { 0.4f, 7.f }, { 0.4f, 6.f }, { 0.2f, 5.f } };

	Connectome.KenyonCells.SetNum(NumKenyonCells);
	for (TArray<FClaw>& Cell : Connectome.KenyonCells)
	{
		const float Pick = Rng.GetFraction();
		const FClass& Class = Pick < Classes[0].Share ? Classes[0] : (Pick < Classes[0].Share + Classes[1].Share ? Classes[1] : Classes[2]);

		// Box-Muller for a normal claw count, clamped to the observed 2..11 range.
		const float U1 = FMath::Max(Rng.GetFraction(), 1e-6f), U2 = Rng.GetFraction();
		const float Normal = FMath::Sqrt(-2.f * FMath::Loge(U1)) * FMath::Cos(2.f * PI * U2);
		const int32 Claws = FMath::Clamp(FMath::RoundToInt32(Class.MeanClaws + 1.5f * Normal), 2, 11);

		for (int32 Claw = 0; Claw < Claws; ++Claw)
		{
			Cell.Add({ Rng.RandHelper(EOptiGlom::Count), 1.f });
		}
	}
	ComputeHash(Connectome);
	return Connectome;
}

bool FOptiConnectome::FromCsv(const FString& Path, FOptiConnectome& Out, FString& OutError)
{
	TArray<FString> Lines;
	if (!FFileHelper::LoadFileToStringArray(Lines, *Path) || Lines.Num() < 2)
	{
		OutError = TEXT("empty or unreadable file");
		return false;
	}

	// Expected header: kc_id,glomerulus,synapses (extra columns are ignored).
	TArray<FString> Header;
	Lines[0].ParseIntoArray(Header, TEXT(","), false);
	const int32 KcColumn = Header.IndexOfByKey(TEXT("kc_id"));
	const int32 GlomColumn = Header.IndexOfByKey(TEXT("glomerulus"));
	const int32 SynColumn = Header.IndexOfByKey(TEXT("synapses"));
	if (KcColumn == INDEX_NONE || GlomColumn == INDEX_NONE || SynColumn == INDEX_NONE)
	{
		OutError = TEXT("header must contain kc_id, glomerulus and synapses");
		return false;
	}

	TMap<FString, int32> CellIndex;
	TArray<FString> Values;
	int32 Unmapped = 0;
	for (int32 Line = 1; Line < Lines.Num(); ++Line)
	{
		Lines[Line].ParseIntoArray(Values, TEXT(","), false);
		if (Values.Num() <= FMath::Max3(KcColumn, GlomColumn, SynColumn))
		{
			continue;
		}
		const int32 Glomerulus = EOptiGlom::FromRealName(GlomerulusFromCellType(Values[GlomColumn]));
		if (Glomerulus == INDEX_NONE)
		{
			++Unmapped;
			continue;
		}
		int32& Index = CellIndex.FindOrAdd(Values[KcColumn], INDEX_NONE);
		if (Index == INDEX_NONE)
		{
			Index = Out.KenyonCells.AddDefaulted();
		}
		// A claw usually carries 5-20 synapses; weights are scaled so a typical claw is about 1.
		const float Synapses = static_cast<float>(FCString::Atof(*Values[SynColumn]));
		Out.KenyonCells[Index].Add({ Glomerulus, FMath::Min(Synapses, 30.f) / 10.f });
	}

	Out.KenyonCells.RemoveAll([](const TArray<FClaw>& Cell) { return Cell.IsEmpty(); });
	if (Out.KenyonCells.Num() < 100)
	{
		OutError = FString::Printf(TEXT("only %d Kenyon cells with known glomeruli"), Out.KenyonCells.Num());
		return false;
	}
	Out.Source = FString::Printf(TEXT("Connectome file %s (%d KCs, %d synapse rows from unmapped glomeruli skipped)"),
		*FPaths::GetCleanFilename(Path), Out.KenyonCells.Num(), Unmapped);
	ComputeHash(Out);
	return true;
}

FOptiConnectome FOptiConnectome::Load()
{
	if (TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("OptiCompanion")))
	{
		const FString Path = Plugin->GetBaseDir() / TEXT("Resources") / TEXT("Connectome") / TEXT("pn_kc.csv");
		if (FPaths::FileExists(Path))
		{
			FOptiConnectome Real;
			FString Error;
			if (FromCsv(Path, Real, Error))
			{
				return Real;
			}
			UE_LOG(LogOptiCompanion, Warning, TEXT("Could not use %s (%s); falling back to statistical wiring."), *Path, *Error);
		}
	}
	return Statistical();
}
