#pragma once

#include "Modules/ModuleInterface.h"
#include "OptiProbe.h"

class OPTICOMPANION_API FOptiCompanionModule : public IModuleInterface
{
public:
	static FOptiCompanionModule& Get();

	virtual void ShutdownModule() override;

	/** Starts a probe unless one is already running. Only one probe can measure at a time. */
	bool StartProbe(const FOptiProbeSettings& Settings, FString& OutError, FOnOptiProbeFinished OnFinished = FOnOptiProbeFinished());
	void CancelProbe();
	bool IsProbeRunning() const { return ActiveProbe.IsValid() && ActiveProbe->IsRunning(); }

private:
	TSharedPtr<FOptiProbe> ActiveProbe;
};
