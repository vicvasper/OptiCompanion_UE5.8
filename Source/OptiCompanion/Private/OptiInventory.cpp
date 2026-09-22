#include "OptiInventory.h"
#include "OptiProbe.h"

#include "Components/AudioComponent.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/LightComponent.h"
#include "Components/LocalLightComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/SpotLightComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Dom/JsonObject.h"
#include "Engine/PostProcessVolume.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Materials/Material.h"
#include "Materials/MaterialInterface.h"
#include "RHI.h"
#include "Misc/FileHelper.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Sound/SoundWave.h"
#include "StaticMeshResources.h"

namespace
{
	// Memory model from OptiLogger's ResourceAnalyzer: per-element sizes of a typical desktop cook.
	// Approximations by design; the real footprint depends on platform, streaming and cook settings.
	constexpr float BytesPerMegabyte = 1024.f * 1024.f;
	constexpr float TextureBytesPerPixelUncompressed = 4.f;
	constexpr float TextureCompressionFactorBlockCompressed = 0.25f;
	constexpr float TextureCompressionFactorNormalMap = 0.5f;
	constexpr float TextureCompressionFactorHDR = 2.f;
	constexpr float TextureCompressionFactorDefault = 0.5f;
	constexpr float TextureMipChainFactor = 1.33f;
	constexpr float StaticMeshBytesPerVertex = 32.f;
	constexpr float MeshBytesPerTriangle = 3.f * 4.f;
	constexpr float SkeletalMeshBytesPerVertex = 48.f;
	constexpr float SkeletalMeshBytesPerBone = 64.f;
	constexpr float AudioBytesPerSample = 2.f; // 16-bit, as OptiLogger assumes

	// Thresholds for the warnings OptiCompanion adds on top.
	constexpr int32 DenseMeshTriangles = 50000;
	constexpr int32 RepeatedMeshUses = 20;
	constexpr int32 HeavyMaterialInstructions = 300;
	constexpr int32 NoticeableMaterialInstructions = 200;
	constexpr int32 HeavyTranslucentInstructions = 120;
	constexpr int32 LargeTextureSize = 4096;
	constexpr int32 HugeTextureSize = 8192;
	constexpr float LargeInMemoryAudioMB = 5.f;
	constexpr float HugeLightRadius = 2500.f;

	TFunction<int32(UMaterialInterface*)> GMaterialCounter;

	FString CompressionName(TextureCompressionSettings Settings)
	{
		const UEnum* Enum = StaticEnum<TextureCompressionSettings>();
		return Enum ? Enum->GetDisplayNameTextByValue(Settings).ToString() : FString::FromInt(Settings);
	}

	FString LightTypeName(const ULightComponent& Light)
	{
		// Spot lights derive from point lights, so the most derived type is tested first.
		if (Light.IsA<UDirectionalLightComponent>()) { return TEXT("Directional"); }
		if (Light.IsA<USpotLightComponent>()) { return TEXT("Spot"); }
		if (Light.IsA<ULocalLightComponent>()) { return TEXT("Point/Rect"); }
		return Light.GetClass()->GetName();
	}

	FString ActorName(const AActor& Actor)
	{
#if WITH_EDITOR
		return Actor.GetActorLabel();
#else
		return Actor.GetName();
#endif
	}

	void Warn(FOptiInventoryItem& Item, int32 Severity, const FString& Text)
	{
		if (Severity > Item.Severity)
		{
			Item.Severity = Severity;
		}
		Item.Warning = Item.Warning.IsEmpty() ? Text : Item.Warning + TEXT("; ") + Text;
	}
}

// ---------------------------------------------------------------------------------------------- view

void FOptiView::BuildFrustum() const
{
	// Inward-facing normals of the side planes and the near plane, through the camera position.
	const FRotationMatrix Axes(Rotation);
	const FVector Forward = Axes.GetScaledAxis(EAxis::X);
	const FVector Right = Axes.GetScaledAxis(EAxis::Y);
	const FVector Up = Axes.GetScaledAxis(EAxis::Z);
	const float TanH = FMath::Tan(FMath::DegreesToRadians(FMath::Clamp(FOVDegrees, 5.f, 170.f) * 0.5f));
	const float TanV = TanH / FMath::Max(AspectRatio, 0.1f);

	Planes.Reset();
	for (const FVector& Normal : { Forward, (Forward * TanH - Right), (Forward * TanH + Right), (Forward * TanV - Up), (Forward * TanV + Up) })
	{
		Planes.Add(FPlane(Normal.GetSafeNormal(), 0.f));
	}
	bFrustumBuilt = true;
}

bool FOptiView::SeesSphere(const FVector& Center, float Radius) const
{
	if (!bValid)
	{
		return true;
	}
	if (!bFrustumBuilt)
	{
		BuildFrustum();
	}
	const FVector Local = Center - Location;
	for (const FPlane& Plane : Planes)
	{
		if (FVector::DotProduct(FVector(Plane), Local) < -Radius)
		{
			return false;
		}
	}
	return true;
}

// ---------------------------------------------------------------------------------------------- inventory

float FOptiInventory::TotalMemoryMB(FOptiInventoryItem::EType Type) const
{
	float Total = 0.f;
	for (const FOptiInventoryItem& Item : Items)
	{
		Total += Item.Type == Type ? Item.MemoryMB : 0.f;
	}
	return Total;
}

int32 FOptiInventory::Count(FOptiInventoryItem::EType Type) const
{
	return Items.FilterByPredicate([Type](const FOptiInventoryItem& Item) { return Item.Type == Type; }).Num();
}

bool FOptiInventory::ExportJson(const FString& Path) const
{
	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("ExportDate"), Time.ToIso8601());
	Root->SetBoolField(TEXT("VisibleOnly"), bVisibleOnly);
	TArray<TSharedPtr<FJsonValue>> Values;
	for (const FOptiInventoryItem& Item : Items)
	{
		TSharedRef<FJsonObject> J = MakeShared<FJsonObject>();
		J->SetStringField(TEXT("Type"), OptiInventory::LexToString(Item.Type));
		J->SetStringField(TEXT("Name"), Item.Name);
		J->SetStringField(TEXT("Path"), Item.Path);
		J->SetNumberField(TEXT("EstimatedMemoryUsageMB"), Item.MemoryMB);
		J->SetStringField(TEXT("Details"), Item.Details);
		J->SetStringField(TEXT("Warning"), Item.Warning);
		J->SetNumberField(TEXT("Severity"), Item.Severity);
		J->SetNumberField(TEXT("Uses"), Item.Uses);
		Values.Add(MakeShared<FJsonValueObject>(J));
	}
	Root->SetArrayField(TEXT("Items"), Values);
	FString Text;
	FJsonSerializer::Serialize(Root, TJsonWriterFactory<>::Create(&Text));
	return FFileHelper::SaveStringToFile(Text, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
}

namespace OptiInventory
{
	const TCHAR* LexToString(FOptiInventoryItem::EType Type)
	{
		switch (Type)
		{
		case FOptiInventoryItem::EType::StaticMesh: return TEXT("StaticMesh");
		case FOptiInventoryItem::EType::SkeletalMesh: return TEXT("SkeletalMesh");
		case FOptiInventoryItem::EType::Texture: return TEXT("Texture");
		case FOptiInventoryItem::EType::Material: return TEXT("Material");
		case FOptiInventoryItem::EType::Light: return TEXT("Light");
		case FOptiInventoryItem::EType::Audio: return TEXT("Audio");
		case FOptiInventoryItem::EType::PostProcess: return TEXT("PostProcess");
		case FOptiInventoryItem::EType::Blueprint: return TEXT("Blueprint");
		default: return TEXT("?");
		}
	}

	void SetMaterialInstructionCounter(TFunction<int32(UMaterialInterface*)> Counter)
	{
		GMaterialCounter = MoveTemp(Counter);
	}

	int32 PixelInstructions(UMaterialInterface* Material)
	{
		return GMaterialCounter && Material ? GMaterialCounter(Material) : 0;
	}

	float EstimateTextureMB(int32 Width, int32 Height, uint8 CompressionSettings, int32 Mips)
	{
		float Factor = TextureCompressionFactorDefault;
		switch (static_cast<TextureCompressionSettings>(CompressionSettings))
		{
		case TC_Default:
		case TC_BC7:
		case TC_Grayscale:
		case TC_Alpha:
			Factor = TextureCompressionFactorBlockCompressed;
			break;
		case TC_Normalmap:
			Factor = TextureCompressionFactorNormalMap;
			break;
		case TC_HDR:
		case TC_HDR_Compressed:
			Factor = TextureCompressionFactorHDR;
			break;
		default:
			break;
		}
		const float Base = static_cast<float>(Width) * Height * TextureBytesPerPixelUncompressed;
		return Base * Factor * (Mips > 1 ? TextureMipChainFactor : 1.f) / BytesPerMegabyte;
	}

	FOptiInventory Collect(UWorld* World, const FOptiView& View)
	{
		FOptiInventory Inventory;
		Inventory.bVisibleOnly = View.bValid;
		Inventory.Time = FDateTime::Now();
		if (!World)
		{
			return Inventory;
		}

		struct FMeshUse { int32 Components = 0; int32 NotInstanced = 0; int32 Instances = 0; };
		TMap<UStaticMesh*, FMeshUse> StaticMeshes;
		TMap<USkeletalMesh*, int32> SkeletalMeshes;
		TMap<UMaterialInterface*, int32> Materials;
		TMap<USoundWave*, int32> Sounds;

		for (TActorIterator<AActor> It(World); It; ++It)
		{
			AActor* Actor = *It;
			if (Actor->IsTemplate() || Actor->HasAnyFlags(RF_Transient))
			{
				continue;
			}

			Actor->ForEachComponent<UPrimitiveComponent>(false, [&](UPrimitiveComponent* Primitive)
			{
				if (!Primitive->IsRegistered() || Primitive->IsEditorOnly() || !View.SeesSphere(Primitive->Bounds.Origin, Primitive->Bounds.SphereRadius))
				{
					return;
				}
				if (UStaticMeshComponent* Mesh = Cast<UStaticMeshComponent>(Primitive))
				{
					if (UStaticMesh* Asset = Mesh->GetStaticMesh())
					{
						FMeshUse& Use = StaticMeshes.FindOrAdd(Asset);
						++Use.Components;
						if (const UInstancedStaticMeshComponent* Instanced = Cast<UInstancedStaticMeshComponent>(Mesh))
						{
							Use.Instances += Instanced->GetInstanceCount();
						}
						else
						{
							++Use.NotInstanced;
						}
					}
				}
				else if (USkeletalMeshComponent* Skinned = Cast<USkeletalMeshComponent>(Primitive))
				{
					if (USkeletalMesh* Asset = Skinned->GetSkeletalMeshAsset())
					{
						++SkeletalMeshes.FindOrAdd(Asset);
					}
				}
				TArray<UMaterialInterface*> Used;
				Primitive->GetUsedMaterials(Used);
				for (UMaterialInterface* Material : Used)
				{
					if (Material)
					{
						++Materials.FindOrAdd(Material);
					}
				}
			});

			Actor->ForEachComponent<ULightComponent>(false, [&](ULightComponent* Light)
			{
				const ULocalLightComponent* Local = Cast<ULocalLightComponent>(Light);
				const float Radius = Local ? Local->AttenuationRadius : 0.f;
				if (Local && !View.SeesSphere(Light->GetComponentLocation(), Radius))
				{
					return;
				}
				FOptiInventoryItem Item;
				Item.Type = FOptiInventoryItem::EType::Light;
				Item.Name = ActorName(*Actor);
				Item.Path = Actor->GetPathName();
				Item.Uses = 1;
				const bool bMovable = Light->Mobility != EComponentMobility::Static;
				Item.Details = FString::Printf(TEXT("%s, %s, shadows %s%s%s"), *LightTypeName(*Light),
					Light->Mobility == EComponentMobility::Movable ? TEXT("movable") : (bMovable ? TEXT("stationary") : TEXT("static")),
					Light->CastShadows ? TEXT("on") : TEXT("off"),
					Local ? *FString::Printf(TEXT(", radius %.0f"), Radius) : TEXT(""),
					Light->LightFunctionMaterial ? TEXT(", light function") : TEXT(""));
				if (Light->LightFunctionMaterial && bMovable)
				{
					Warn(Item, 1, TEXT("Dynamic light function"));
				}
				if (Local && bMovable && Light->CastShadows && Radius > HugeLightRadius)
				{
					Warn(Item, 1, TEXT("Large radius with dynamic shadows"));
				}
				if (Local && bMovable && Light->CastShadows && Light->bCastVolumetricShadow && Light->VolumetricScatteringIntensity > 0.f)
				{
					Warn(Item, 1, TEXT("Casts volumetric fog shadows"));
				}
				Inventory.Items.Add(MoveTemp(Item));
			});

			Actor->ForEachComponent<UAudioComponent>(false, [&](UAudioComponent* Audio)
			{
				if (USoundWave* Wave = Cast<USoundWave>(Audio->Sound))
				{
					++Sounds.FindOrAdd(Wave);
				}
			});

			if (APostProcessVolume* Volume = Cast<APostProcessVolume>(Actor))
			{
				const FPostProcessSettings& S = Volume->Settings;
				TArray<FString> Effects;
				if (S.bOverride_BloomIntensity && S.BloomIntensity > 0.f) { Effects.Add(TEXT("bloom")); }
				if (S.bOverride_MotionBlurAmount && S.MotionBlurAmount > 0.f) { Effects.Add(TEXT("motion blur")); }
				if (S.bOverride_DepthOfFieldFstop || S.bOverride_DepthOfFieldFocalDistance) { Effects.Add(TEXT("depth of field")); }
				if (S.bOverride_AutoExposureMinBrightness || S.bOverride_AutoExposureMaxBrightness) { Effects.Add(TEXT("auto exposure")); }
				if (S.bOverride_ColorSaturation) { Effects.Add(TEXT("color grading")); }
				if (S.bOverride_VignetteIntensity && S.VignetteIntensity > 0.f) { Effects.Add(TEXT("vignette")); }
				FOptiInventoryItem Item;
				Item.Type = FOptiInventoryItem::EType::PostProcess;
				Item.Name = ActorName(*Actor);
				Item.Path = Actor->GetPathName();
				Item.Uses = 1;
				Item.Details = FString::Printf(TEXT("%s, priority %.1f, %s"), Volume->bUnbound ? TEXT("unbound") : TEXT("bounded"), Volume->Priority,
					Effects.IsEmpty() ? TEXT("no overrides") : *FString::Join(Effects, TEXT(", ")));
				Inventory.Items.Add(MoveTemp(Item));
			}
		}

		for (const TPair<UStaticMesh*, FMeshUse>& Entry : StaticMeshes)
		{
			UStaticMesh* Mesh = Entry.Key;
			FOptiInventoryItem Item;
			Item.Type = FOptiInventoryItem::EType::StaticMesh;
			Item.Name = Mesh->GetName();
			Item.Path = Mesh->GetPathName();
			Item.Uses = Entry.Value.Components;
			int32 Vertices = 0, Triangles = 0, LODs = 0;
			if (const FStaticMeshRenderData* RenderData = Mesh->GetRenderData())
			{
				LODs = RenderData->LODResources.Num();
				if (LODs > 0)
				{
					Vertices = RenderData->LODResources[0].GetNumVertices();
					Triangles = RenderData->LODResources[0].GetNumTriangles();
				}
			}
			const bool bNanite = Mesh->IsNaniteEnabled();
			Item.MemoryMB = (Vertices * StaticMeshBytesPerVertex + Triangles * MeshBytesPerTriangle) / BytesPerMegabyte;
			Item.Details = FString::Printf(TEXT("%d tris, %d LODs%s, %d in view%s"), Triangles, LODs, bNanite ? TEXT(", Nanite") : TEXT(""),
				Entry.Value.Components, Entry.Value.Instances > 0 ? *FString::Printf(TEXT(" (+%d instances)"), Entry.Value.Instances) : TEXT(""));
			if (!bNanite && Triangles > DenseMeshTriangles && LODs < 2)
			{
				Warn(Item, 2, FString::Printf(TEXT("%dk triangles and no LODs"), Triangles / 1000));
			}
			if (!bNanite && Entry.Value.NotInstanced >= RepeatedMeshUses)
			{
				Warn(Item, 1, FString::Printf(TEXT("Placed %d times without instancing"), Entry.Value.NotInstanced));
			}
			Inventory.Items.Add(MoveTemp(Item));
		}

		for (const TPair<USkeletalMesh*, int32>& Entry : SkeletalMeshes)
		{
			USkeletalMesh* Mesh = Entry.Key;
			FOptiInventoryItem Item;
			Item.Type = FOptiInventoryItem::EType::SkeletalMesh;
			Item.Name = Mesh->GetName();
			Item.Path = Mesh->GetPathName();
			Item.Uses = Entry.Value;
			const int32 Bones = Mesh->GetRefSkeleton().GetNum();
			int32 Vertices = 0, LODs = 0;
			if (const FSkeletalMeshRenderData* RenderData = Mesh->GetResourceForRendering())
			{
				LODs = RenderData->LODRenderData.Num();
				Vertices = LODs > 0 ? RenderData->LODRenderData[0].GetNumVertices() : 0;
			}
			Item.MemoryMB = (Vertices * SkeletalMeshBytesPerVertex + Bones * SkeletalMeshBytesPerBone) / BytesPerMegabyte;
			Item.Details = FString::Printf(TEXT("%d verts, %d bones, %d LODs, %d in view"), Vertices, Bones, LODs, Entry.Value);
			if (Vertices > DenseMeshTriangles && LODs < 2)
			{
				Warn(Item, 2, FString::Printf(TEXT("%dk vertices and no LODs"), Vertices / 1000));
			}
			Inventory.Items.Add(MoveTemp(Item));
		}

		TMap<UTexture2D*, int32> Textures;
		for (const TPair<UMaterialInterface*, int32>& Entry : Materials)
		{
			UMaterialInterface* Material = Entry.Key;
			FOptiInventoryItem Item;
			Item.Type = FOptiInventoryItem::EType::Material;
			Item.Name = Material->GetName();
			Item.Path = Material->GetPathName();
			Item.Uses = Entry.Value;
			TArray<UTexture*> Used;
			Material->GetUsedTextures(Used); // all quality levels and platforms (the 5-argument overload is an empty stub since 5.7)
			for (UTexture* Texture : Used)
			{
				if (UTexture2D* Texture2D = Cast<UTexture2D>(Texture))
				{
					Textures.FindOrAdd(Texture2D) += Entry.Value;
				}
			}
			const EBlendMode Blend = Material->GetBlendMode();
			const bool bTranslucent = IsTranslucentBlendMode(Blend);
			const int32 Instructions = PixelInstructions(Material);
			Item.Details = FString::Printf(TEXT("%s, %d textures%s, %d uses"),
				bTranslucent ? TEXT("translucent") : (Blend == BLEND_Masked ? TEXT("masked") : TEXT("opaque")), Used.Num(),
				Instructions > 0 ? *FString::Printf(TEXT(", %d pixel instructions"), Instructions) : TEXT(""), Entry.Value);
			if (Instructions > HeavyMaterialInstructions)
			{
				Warn(Item, 2, FString::Printf(TEXT("Heavy shader (%d instructions)"), Instructions));
			}
			else if (Instructions > NoticeableMaterialInstructions)
			{
				Warn(Item, 1, FString::Printf(TEXT("Costly shader (%d instructions)"), Instructions));
			}
			if (bTranslucent && Instructions > HeavyTranslucentInstructions)
			{
				Warn(Item, 1, TEXT("Heavy translucent shader: pays per overlapping layer"));
			}
			Inventory.Items.Add(MoveTemp(Item));
		}

		for (const TPair<UTexture2D*, int32>& Entry : Textures)
		{
			UTexture2D* Texture = Entry.Key;
			FOptiInventoryItem Item;
			Item.Type = FOptiInventoryItem::EType::Texture;
			Item.Name = Texture->GetName();
			Item.Path = Texture->GetPathName();
			Item.Uses = Entry.Value;
			const int32 Width = Texture->GetSizeX(), Height = Texture->GetSizeY(), Mips = Texture->GetNumMips();
			const bool bVirtual = Texture->VirtualTextureStreaming;
			Item.MemoryMB = EstimateTextureMB(Width, Height, Texture->CompressionSettings, Mips);
			Item.Details = FString::Printf(TEXT("%dx%d, %s, %d mips%s%s"), Width, Height, *CompressionName(Texture->CompressionSettings), Mips,
				bVirtual ? TEXT(", virtual") : TEXT(""), Texture->NeverStream ? TEXT(", never streams") : TEXT(""));
			const int32 Largest = FMath::Max(Width, Height);
			if (Largest >= HugeTextureSize && !bVirtual)
			{
				Warn(Item, 2, TEXT("8K texture without virtual texturing"));
			}
			else if (Largest >= LargeTextureSize && !bVirtual)
			{
				Warn(Item, 1, TEXT("4K texture without virtual texturing"));
			}
			if (Texture->CompressionSettings == TC_HDR && Largest >= 1024)
			{
				Warn(Item, 1, TEXT("Uncompressed HDR"));
			}
			if (Texture->NeverStream && Largest >= 2048)
			{
				Warn(Item, 1, TEXT("Large texture that never streams"));
			}
			Inventory.Items.Add(MoveTemp(Item));
		}

		for (const TPair<USoundWave*, int32>& Entry : Sounds)
		{
			USoundWave* Wave = Entry.Key;
			FOptiInventoryItem Item;
			Item.Type = FOptiInventoryItem::EType::Audio;
			Item.Name = Wave->GetName();
			Item.Path = Wave->GetPathName();
			Item.Uses = Entry.Value;
			const int32 SampleRate = Wave->GetSampleRateForCurrentPlatform();
			Item.MemoryMB = Wave->Duration * SampleRate * Wave->NumChannels * AudioBytesPerSample / BytesPerMegabyte;
			Item.Details = FString::Printf(TEXT("%.1f s, %d Hz, %d ch, %s"), Wave->Duration, SampleRate, Wave->NumChannels,
				Wave->IsStreaming() ? TEXT("streaming") : TEXT("in memory"));
			if (!Wave->IsStreaming() && Item.MemoryMB > LargeInMemoryAudioMB)
			{
				Warn(Item, 1, TEXT("Long sound kept in memory"));
			}
			Inventory.Items.Add(MoveTemp(Item));
		}

		Inventory.Items.Sort([](const FOptiInventoryItem& L, const FOptiInventoryItem& R)
		{
			return L.Severity != R.Severity ? L.Severity > R.Severity : L.MemoryMB > R.MemoryMB;
		});
		return Inventory;
	}
}
