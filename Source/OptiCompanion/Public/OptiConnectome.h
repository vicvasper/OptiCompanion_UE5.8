#pragma once

#include "CoreMinimal.h"

/**
 * Wiring from projection neurons (glomeruli) to Kenyon cells.
 *
 * If Resources/Connectome/pn_kc.csv exists (extracted from FlyWire or MaleCNS with the script in Tools/),
 * each Kenyon cell gets exactly the glomeruli and synapse counts of a real one. Otherwise the wiring is
 * generated with the statistics reported for the adult fly (Caron et al. 2013; Li et al. 2020): each KC
 * samples about six claws from random glomeruli. The generator uses a fixed seed so every install gets
 * the same brain and learned memories stay portable between machines.
 */
struct OPTICOMPANION_API FOptiConnectome
{
	struct FClaw
	{
		int32 Glomerulus = 0;
		float Weight = 1.f;
	};

	TArray<TArray<FClaw>> KenyonCells;
	FString Source;
	uint32 Hash = 0;

	int32 NumKenyonCells() const { return KenyonCells.Num(); }

	/** Loads the real wiring when available, otherwise builds the statistical one. */
	static FOptiConnectome Load();

	static FOptiConnectome Statistical(int32 NumKenyonCells = 2000, int32 Seed = 20130408);
	static bool FromCsv(const FString& Path, FOptiConnectome& Out, FString& OutError);
};
