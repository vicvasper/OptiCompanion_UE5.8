#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "OptiCompanionSettings.generated.h"

UENUM()
enum class EOptiTone : uint8
{
	Sober,
	Friendly,
};

UENUM()
enum class EOptiLanguage : uint8
{
	Auto UMETA(DisplayName = "Editor language"),
	English,
	Spanish UMETA(DisplayName = "Español"),
};

UENUM()
enum class EOptiPresence : uint8
{
	/** The fly lives on top of the editor, can be dragged anywhere and talks in bubbles. */
	Mascot,
	/** No mascot: a status-bar button and regular editor notifications, same features. */
	Professional,
};

UENUM()
enum class EOptiSelectorSetting : uint8
{
	Fly UMETA(DisplayName = "Fly brain"),
	ThompsonSampling UMETA(DisplayName = "Thompson sampling (baseline)"),
	Random UMETA(DisplayName = "Random (baseline)"),
};

/** Per-user settings for the performance copilot. Editor Preferences > Plugins > OptiCompanion. */
UCLASS(config = EditorPerProjectUserSettings, meta = (DisplayName = "OptiCompanion"))
class OPTICOMPANIONEDITOR_API UOptiCompanionSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	UOptiCompanionSettings();

	virtual FName GetContainerName() const override { return TEXT("Editor"); }
	virtual FName GetCategoryName() const override { return TEXT("Plugins"); }

	UPROPERTY(config, EditAnywhere, Category = "General")
	bool bEnabled = true;

	UPROPERTY(config, EditAnywhere, Category = "General")
	EOptiPresence Presence = EOptiPresence::Mascot;

	/** Who lives on your editor. Themes are folders in the plugin's Resources/Mascots; add your own there. */
	UPROPERTY(config, EditAnywhere, Category = "General", meta = (GetOptions = "GetMascotOptions"))
	FString Mascot = TEXT("oc");

	UFUNCTION()
	static TArray<FString> GetMascotOptions();

	UPROPERTY(config, EditAnywhere, Category = "General")
	EOptiTone Tone = EOptiTone::Sober;

	UPROPERTY(config, EditAnywhere, Category = "General")
	EOptiLanguage Language = EOptiLanguage::Auto;

	/** The fly moves as little as possible; peripheral motion is distracting. */
	UPROPERTY(config, EditAnywhere, Category = "General")
	bool bQuietMode = false;

	/** Keeps measuring and learning, but never shows a notice. Findings still go to the notebook. */
	UPROPERTY(config, EditAnywhere, Category = "General")
	bool bDoNotDisturb = false;

	/** Upper limit; the fly lowers it by itself if you keep ignoring its notices. */
	UPROPERTY(config, EditAnywhere, Category = "General", meta = (ClampMin = 1, ClampMax = 20))
	int32 MaxNoticesPerHour = 3;

	/** Short experiments while you are away from the keyboard. They stop the moment you touch anything. */
	UPROPERTY(config, EditAnywhere, Category = "Naps")
	bool bEnableNaps = true;

	/**
	 * Experiment while you work, as long as the viewport camera stays still (editing details, Blueprints,
	 * browsing assets...). Moving the camera, compiling or playing pauses the experiment. While you work the
	 * fly only tries changes it expects to be invisible and never touches components of your level.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Naps")
	bool bExperimentWhileWorking = true;

	/** How long the viewport camera must stay still before an experiment starts or resumes. */
	UPROPERTY(config, EditAnywhere, Category = "Naps", meta = (ClampMin = 0.2, ClampMax = 30, Units = "s"))
	float StillCameraSeconds = 0.5f;

	/** Away from the keyboard this long, the fly also tries riskier changes and changes to level components. */
	UPROPERTY(config, EditAnywhere, Category = "Naps", meta = (ClampMin = 5, ClampMax = 600, Units = "s"))
	float IdleSecondsBeforeNap = 20.f;

	/**
	 * Longer experiments: more and longer blocks per variant, so each finding rests on more evidence. Off by
	 * default, because short experiments already have to pass the same significance test, they just find fewer
	 * of the small savings.
	 */
	UPROPERTY(config, EditAnywhere, Category = "General", AdvancedDisplay)
	bool bThoroughExperiments = false;

	UPROPERTY(config, EditAnywhere, Category = "Naps", meta = (ClampMin = 0.5, ClampMax = 3600, Units = "s"))
	float SecondsBetweenNaps = 1.5f;

	/** Unreal slows itself down when it is not the active window, which distorts measurements. Turn off only for automated runs. */
	UPROPERTY(config, EditAnywhere, Category = "Naps", AdvancedDisplay)
	bool bNapOnlyWhenForeground = true;

	/** Laptops on battery throttle the GPU, so measurements there are unreliable. */
	UPROPERTY(config, EditAnywhere, Category = "Naps")
	bool bNapOnBattery = false;

	/** Minimum saving worth telling you about. */
	UPROPERTY(config, EditAnywhere, Category = "Findings", meta = (ClampMin = 0.01, Units = "ms"))
	float MinGainMs = 0.1f;

	UPROPERTY(config, EditAnywhere, Category = "Findings", meta = (ClampMin = 0.1, ClampMax = 50))
	float MinGainPercent = 1.f;

	/** Average perceptual difference (0..1) above which a change counts as visible. */
	UPROPERTY(config, EditAnywhere, Category = "Findings", meta = (ClampMin = 0.001, ClampMax = 0.5))
	float VisualThresholdMean = 0.012f;

	/** 95th-percentile perceptual difference above which a change counts as visible somewhere on screen. */
	UPROPERTY(config, EditAnywhere, Category = "Findings", meta = (ClampMin = 0.01, ClampMax = 1))
	float VisualThresholdP95 = 0.08f;

	/** Store the notebook in Config/ so it goes through version control and the team sees the same findings. */
	UPROPERTY(config, EditAnywhere, Category = "Findings")
	bool bShareNotebookWithTeam = false;

	/** Warn right after a save that made the frame slower (camera must not move). */
	UPROPERTY(config, EditAnywhere, Category = "Save reflex")
	bool bEnableSaveReflex = true;

	/** In Play and Simulate, record a few seconds of CPU trace now and then to find the Blueprints whose Tick costs the most,
	 *  and try a longer Tick Interval on the worst one. Skipped when you are already tracing (Unreal Insights). */
	UPROPERTY(config, EditAnywhere, Category = "Play")
	bool bMeasureBlueprintsInPlay = true;

	/** Which policy picks experiments. The baselines exist to check that the fly brain actually wins. */
	UPROPERTY(config, EditAnywhere, Category = "Brain")
	EOptiSelectorSetting Selector = EOptiSelectorSetting::Fly;

	/** Where the fly sits, as a fraction of the editor window. */
	UPROPERTY(config)
	FVector2D FlyPosition = FVector2D(0.78, 0.7);

	UPROPERTY(config)
	bool bIntroShown = false;
};
