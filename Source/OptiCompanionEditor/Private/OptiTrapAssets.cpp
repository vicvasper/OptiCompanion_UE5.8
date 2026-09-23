#include "OptiTrapAssets.h"
#include "OptiProbe.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Components/PointLightComponent.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "FileHelpers.h"
#include "K2Node_CallFunction.h"
#include "K2Node_Event.h"
#include "Kismet/GameplayStatics.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "MaterialEditingLibrary.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionAdd.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionMultiply.h"
#include "Materials/MaterialExpressionSine.h"
#include "Materials/MaterialExpressionTextureCoordinate.h"
#include "Materials/MaterialExpressionTextureSample.h"
#include "Materials/MaterialExpressionTime.h"
#include "MeshDescription.h"
#include "StaticMeshAttributes.h"
#include "TextureCompiler.h"
#include "UObject/Package.h"

namespace OptiTrap
{
	namespace
	{
		const TCHAR* Folder = TEXT("/Game/OptiTrap/");
		constexpr EObjectFlags AssetFlags = RF_Public | RF_Standalone | RF_Transactional;

		template <typename T>
		T* Existing(const TCHAR* Name)
		{
			const FString Path = FString(Folder) + Name + TEXT(".") + Name;
			return LoadObject<T>(nullptr, *Path, nullptr, LOAD_NoWarn | LOAD_Quiet);
		}

		UPackage* NewPackage(const TCHAR* Name)
		{
			UPackage* Package = CreatePackage(*(FString(Folder) + Name));
			Package->FullyLoad();
			return Package;
		}

		void Created(UObject* Asset, TArray<UPackage*>& ToSave)
		{
			FAssetRegistryModule::AssetCreated(Asset);
			Asset->MarkPackageDirty();
			ToSave.Add(Asset->GetOutermost());
		}

		// ------------------------------------------------------------------ textures

		UTexture2D* NoiseTexture(TArray<UPackage*>& ToSave)
		{
			const TCHAR* Name = TEXT("T_Trap_Noise4K");
			if (UTexture2D* Found = Existing<UTexture2D>(Name))
			{
				return Found;
			}
			constexpr int32 Size = 4096;
			TArray<uint8> Pixels;
			Pixels.SetNumUninitialized(Size * Size * 4);
			for (int32 Y = 0; Y < Size; ++Y)
			{
				for (int32 X = 0; X < Size; ++X)
				{
					// Cheap hashed noise over a few colour bands: detail everywhere, so compression cannot hide it.
					const uint32 H = HashCombineFast(static_cast<uint32>(X * 73856093), static_cast<uint32>(Y * 19349663));
					uint8* P = &Pixels[(Y * Size + X) * 4];
					P[0] = static_cast<uint8>((H & 0xFF) / 2 + (X >> 5));
					P[1] = static_cast<uint8>(((H >> 8) & 0xFF) / 2 + (Y >> 5));
					P[2] = static_cast<uint8>((H >> 16) & 0xFF);
					P[3] = 255;
				}
			}
			UTexture2D* Texture = NewObject<UTexture2D>(NewPackage(Name), Name, AssetFlags);
			Texture->PreEditChange(nullptr);
			Texture->Source.Init(Size, Size, 1, 1, TSF_BGRA8, Pixels.GetData());
			Texture->SRGB = true;
			Texture->NeverStream = true; // trap: the whole 4K chain stays resident
			Texture->PostEditChange();
			Created(Texture, ToSave);
			return Texture;
		}

		UTexture2D* HdrTexture(TArray<UPackage*>& ToSave)
		{
			const TCHAR* Name = TEXT("T_Trap_HDR_Uncompressed");
			if (UTexture2D* Found = Existing<UTexture2D>(Name))
			{
				return Found;
			}
			constexpr int32 Size = 2048;
			TArray<FFloat16Color> Pixels;
			Pixels.SetNumUninitialized(Size * Size);
			for (int32 Y = 0; Y < Size; ++Y)
			{
				for (int32 X = 0; X < Size; ++X)
				{
					const float U = X / float(Size), V = Y / float(Size);
					Pixels[Y * Size + X] = FFloat16Color(FLinearColor(4.f * U * U, 2.f * V, 0.5f + 0.5f * FMath::Sin(U * 40.f) * FMath::Cos(V * 40.f), 1.f));
				}
			}
			UTexture2D* Texture = NewObject<UTexture2D>(NewPackage(Name), Name, AssetFlags);
			Texture->PreEditChange(nullptr);
			Texture->Source.Init(Size, Size, 1, 1, TSF_RGBA16F, reinterpret_cast<const uint8*>(Pixels.GetData()));
			Texture->SRGB = false;
			Texture->CompressionSettings = TC_HDR; // trap: 16-bit float, uncompressed
			Texture->NeverStream = true;
			Texture->PostEditChange();
			Created(Texture, ToSave);
			return Texture;
		}

		// ------------------------------------------------------------------ materials

		UMaterialExpressionTextureSample* Sample(UMaterial* Material, UTexture2D* Texture, UMaterialExpression* UVs, int32 X, int32 Y)
		{
			auto* Node = Cast<UMaterialExpressionTextureSample>(UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionTextureSample::StaticClass(), X, Y));
			Node->Texture = Texture;
			Node->AutoSetSampleType();
			if (UVs)
			{
				UMaterialEditingLibrary::ConnectMaterialExpressions(UVs, TEXT(""), Node, TEXT("UVs"));
			}
			return Node;
		}

		UMaterialExpressionMultiply* MultiplyBy(UMaterial* Material, UMaterialExpression* A, const TCHAR* AOutput, float B, int32 X, int32 Y)
		{
			auto* Node = Cast<UMaterialExpressionMultiply>(UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionMultiply::StaticClass(), X, Y));
			Node->ConstB = B;
			UMaterialEditingLibrary::ConnectMaterialExpressions(A, AOutput, Node, TEXT("A"));
			return Node;
		}

		/** A long chain of sines and dependent texture reads: what an artist's "just one more layer" looks like after a year. */
		UMaterialExpression* HeavyChain(UMaterial* Material, UTexture2D* Noise, UTexture2D* Hdr, int32 Steps)
		{
			auto* Coords = UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionTextureCoordinate::StaticClass(), -1600, 0);
			auto* Time = UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionTime::StaticClass(), -1600, 200);

			// Dependent reads: every sample's UVs come from the previous sample.
			UMaterialExpression* UVs = Coords;
			UMaterialExpression* Last = nullptr;
			for (int32 Read = 0; Read < 8; ++Read)
			{
				Last = Sample(Material, Read % 3 == 2 ? Hdr : Noise, UVs, -1400 + Read * 120, -300);
				UVs = MultiplyBy(Material, Last, TEXT("RG"), 1.7f + Read * 0.31f, -1340 + Read * 120, -200);
			}

			UMaterialExpression* Value = Last;
			const TCHAR* ValueOutput = TEXT("R");
			for (int32 Step = 0; Step < Steps; ++Step)
			{
				const int32 X = -1000 + (Step % 20) * 50, Y = 100 + (Step / 20) * 120;
				UMaterialExpression* Scaled = MultiplyBy(Material, Value, ValueOutput, 1.3f + 0.07f * Step, X, Y);
				auto* Add = UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionAdd::StaticClass(), X, Y + 40);
				UMaterialEditingLibrary::ConnectMaterialExpressions(Scaled, TEXT(""), Add, TEXT("A"));
				UMaterialEditingLibrary::ConnectMaterialExpressions(Time, TEXT(""), Add, TEXT("B"));
				auto* Sine = UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionSine::StaticClass(), X, Y + 80);
				UMaterialEditingLibrary::ConnectMaterialExpressions(Add, TEXT(""), Sine, TEXT(""));
				Value = Sine;
				ValueOutput = TEXT("");
			}

			auto* Tint = UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionMultiply::StaticClass(), 200, 0);
			UMaterialEditingLibrary::ConnectMaterialExpressions(Value, TEXT(""), Tint, TEXT("A"));
			UMaterialEditingLibrary::ConnectMaterialExpressions(Last, TEXT("RGB"), Tint, TEXT("B"));
			return Tint;
		}

		UMaterial* HeavyMaterial(const TCHAR* Name, bool bGlass, UTexture2D* Noise, UTexture2D* Hdr, TArray<UPackage*>& ToSave)
		{
			if (UMaterial* Found = Existing<UMaterial>(Name))
			{
				return Found;
			}
			UMaterial* Material = NewObject<UMaterial>(NewPackage(Name), Name, AssetFlags);
			UMaterialExpression* Color = HeavyChain(Material, Noise, Hdr, bGlass ? 70 : 120);
			UMaterialEditingLibrary::ConnectMaterialProperty(Color, TEXT(""), MP_BaseColor);
			UMaterialExpression* Glow = MultiplyBy(Material, Color, TEXT(""), 0.15f, 400, 100);
			UMaterialEditingLibrary::ConnectMaterialProperty(Glow, TEXT(""), MP_EmissiveColor);
			if (bGlass)
			{
				// Trap: translucent, lit per pixel, and the level stacks a dozen layers of it.
				Material->BlendMode = BLEND_Translucent;
				Material->TranslucencyLightingMode = TLM_SurfacePerPixelLighting;
				Material->TwoSided = true;
				auto* Opacity = Cast<UMaterialExpressionConstant>(UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionConstant::StaticClass(), 400, 200));
				Opacity->R = 0.25f;
				UMaterialEditingLibrary::ConnectMaterialProperty(Opacity, TEXT(""), MP_Opacity);
			}
			UMaterialEditingLibrary::RecompileMaterial(Material);
			Created(Material, ToSave);
			return Material;
		}

		// ------------------------------------------------------------------ mesh

		UStaticMesh* DenseMesh(UMaterial* Material, TArray<UPackage*>& ToSave)
		{
			const TCHAR* Name = TEXT("SM_Trap_Dense_NoLOD");
			if (UStaticMesh* Found = Existing<UStaticMesh>(Name))
			{
				return Found;
			}
			// A UV sphere with far more triangles than it will ever cover pixels: ~260k triangles, one LOD.
			constexpr int32 Rings = 256, Segments = 512;
			constexpr float Radius = 50.f;
			FMeshDescription Description;
			FStaticMeshAttributes Attributes(Description);
			Attributes.Register();
			TVertexAttributesRef<FVector3f> Positions = Attributes.GetVertexPositions();
			TVertexInstanceAttributesRef<FVector3f> Normals = Attributes.GetVertexInstanceNormals();
			TVertexInstanceAttributesRef<FVector2f> UVs = Attributes.GetVertexInstanceUVs();
			const FPolygonGroupID Group = Description.CreatePolygonGroup();
			Attributes.GetPolygonGroupMaterialSlotNames()[Group] = TEXT("Heavy");

			TArray<FVertexInstanceID> Instances;
			Instances.Reserve((Rings + 1) * (Segments + 1));
			Description.ReserveNewVertices((Rings + 1) * (Segments + 1));
			Description.ReserveNewVertexInstances((Rings + 1) * (Segments + 1));
			for (int32 Ring = 0; Ring <= Rings; ++Ring)
			{
				const float Theta = PI * Ring / Rings;
				for (int32 Segment = 0; Segment <= Segments; ++Segment)
				{
					const float Phi = 2.f * PI * Segment / Segments;
					const FVector3f Normal(FMath::Sin(Theta) * FMath::Cos(Phi), FMath::Sin(Theta) * FMath::Sin(Phi), FMath::Cos(Theta));
					const FVertexID Vertex = Description.CreateVertex();
					Positions[Vertex] = Normal * Radius;
					const FVertexInstanceID Instance = Description.CreateVertexInstance(Vertex);
					Normals[Instance] = Normal;
					UVs.Set(Instance, 0, FVector2f(float(Segment) / Segments, float(Ring) / Rings));
					Instances.Add(Instance);
				}
			}
			Description.ReserveNewTriangles(Rings * Segments * 2);
			for (int32 Ring = 0; Ring < Rings; ++Ring)
			{
				for (int32 Segment = 0; Segment < Segments; ++Segment)
				{
					const FVertexInstanceID A = Instances[Ring * (Segments + 1) + Segment];
					const FVertexInstanceID B = Instances[Ring * (Segments + 1) + Segment + 1];
					const FVertexInstanceID C = Instances[(Ring + 1) * (Segments + 1) + Segment];
					const FVertexInstanceID D = Instances[(Ring + 1) * (Segments + 1) + Segment + 1];
					if (Ring > 0) { Description.CreateTriangle(Group, { A, C, B }); }
					if (Ring < Rings - 1) { Description.CreateTriangle(Group, { B, C, D }); }
				}
			}

			UStaticMesh* Mesh = NewObject<UStaticMesh>(NewPackage(Name), Name, AssetFlags);
			Mesh->GetStaticMaterials().Add(FStaticMaterial(Material, TEXT("Heavy"), TEXT("Heavy")));
			FStaticMeshSourceModel& Source = Mesh->AddSourceModel();
			Source.BuildSettings.bRecomputeNormals = false;
			Source.BuildSettings.bRecomputeTangents = true;
			Source.BuildSettings.bGenerateLightmapUVs = false;
			Mesh->CreateMeshDescription(0, MoveTemp(Description));
			Mesh->CommitMeshDescription(0);
			FMeshNaniteSettings Nanite = Mesh->GetNaniteSettings();
			Nanite.bEnabled = false; // trap: no Nanite and no LODs, so every copy draws all of it
			Mesh->SetNaniteSettings(Nanite);
			Mesh->Build(true);
			Mesh->PostEditChange();
			Created(Mesh, ToSave);
			return Mesh;
		}

		// ------------------------------------------------------------------ blueprint

		void AddTickTrap(UBlueprint* Blueprint)
		{
			// Event Tick -> GetAllActorsOfClass(Actor): a classic that walks every actor in the world, every frame.
			UEdGraph* Graph = FBlueprintEditorUtils::FindEventGraph(Blueprint);
			if (!Graph)
			{
				return;
			}
			UK2Node_Event* Tick = nullptr;
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				UK2Node_Event* Event = Cast<UK2Node_Event>(Node);
				if (Event && Event->EventReference.GetMemberName() == GET_FUNCTION_NAME_CHECKED(AActor, ReceiveTick))
				{
					Tick = Event;
				}
			}
			if (!Tick)
			{
				int32 PosY = 0;
				Tick = FKismetEditorUtilities::AddDefaultEventNode(Blueprint, Graph, GET_FUNCTION_NAME_CHECKED(AActor, ReceiveTick), AActor::StaticClass(), PosY);
			}
			if (!Tick)
			{
				return;
			}

			UK2Node_CallFunction* Call = NewObject<UK2Node_CallFunction>(Graph);
			Call->CreateNewGuid();
			Call->SetFromFunction(UGameplayStatics::StaticClass()->FindFunctionByName(GET_FUNCTION_NAME_CHECKED(UGameplayStatics, GetAllActorsOfClass)));
			Call->NodePosX = Tick->NodePosX + 320;
			Call->NodePosY = Tick->NodePosY;
			Graph->AddNode(Call, false, false);
			Call->PostPlacedNewNode();
			Call->AllocateDefaultPins();

			const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();
			Schema->TryCreateConnection(Tick->FindPinChecked(UEdGraphSchema_K2::PN_Then), Call->GetExecPin());
			if (UEdGraphPin* ClassPin = Call->FindPin(TEXT("ActorClass")))
			{
				Schema->TrySetDefaultObject(*ClassPin, AActor::StaticClass());
			}
			Tick->SetEnabledState(ENodeEnabledState::Enabled, false);
		}

		UBlueprint* HeavyBlueprint(UStaticMesh* Dense, UMaterial* Material, TArray<UPackage*>& ToSave)
		{
			const TCHAR* Name = TEXT("BP_Trap_HeavyProp");
			if (UBlueprint* Found = Existing<UBlueprint>(Name))
			{
				return Found;
			}
			UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(AActor::StaticClass(), NewPackage(Name), Name, BPTYPE_Normal,
				UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
			USimpleConstructionScript* Scs = Blueprint->SimpleConstructionScript;

			USCS_Node* Root = Scs->CreateNode(USceneComponent::StaticClass(), TEXT("Root"));
			Scs->AddNode(Root);

			// Trap: six dense meshes that could have been one instanced component, all casting shadows.
			for (int32 Index = 0; Index < 8; ++Index)
			{
				USCS_Node* Node = Scs->CreateNode(UStaticMeshComponent::StaticClass(), *FString::Printf(TEXT("DenseMesh_%d"), Index));
				UStaticMeshComponent* Component = CastChecked<UStaticMeshComponent>(Node->ComponentTemplate);
				Component->SetStaticMesh(Dense);
				Component->SetMaterial(0, Material);
				const float Angle = Index * PI / 3.f;
				Component->SetRelativeLocation(FVector(FMath::Cos(Angle) * 160.f, FMath::Sin(Angle) * 160.f, 60.f + Index * 25.f));
				Root->AddChildNode(Node);
			}
			// Trap: three shadowed lights in every copy, overlapping each other.
			for (int32 Index = 0; Index < 3; ++Index)
			{
				USCS_Node* Node = Scs->CreateNode(UPointLightComponent::StaticClass(), *FString::Printf(TEXT("ShadowLight_%d"), Index));
				UPointLightComponent* Light = CastChecked<UPointLightComponent>(Node->ComponentTemplate);
				Light->SetMobility(EComponentMobility::Movable);
				Light->SetRelativeLocation(FVector(0.f, 0.f, 150.f + Index * 60.f));
				Light->SetAttenuationRadius(900.f);
				Light->SetIntensityUnits(ELightUnits::Candelas);
				Light->SetIntensity(20.f);
				Light->SetCastShadows(true);
				Light->SetCastVolumetricShadow(true);
				Root->AddChildNode(Node);
			}

			AddTickTrap(Blueprint);
			FKismetEditorUtilities::CompileBlueprint(Blueprint);
			Created(Blueprint, ToSave);
			return Blueprint;
		}
	}

	FHeavyAssets CreateHeavyAssets()
	{
		TArray<UPackage*> ToSave;
		FHeavyAssets Assets;
		Assets.Noise4K = NoiseTexture(ToSave);
		Assets.HdrUncompressed = HdrTexture(ToSave);
		FTextureCompilingManager::Get().FinishCompilation({ Assets.Noise4K, Assets.HdrUncompressed });
		Assets.HeavyOpaque = HeavyMaterial(TEXT("M_Trap_HeavyOpaque"), false, Assets.Noise4K, Assets.HdrUncompressed, ToSave);
		Assets.HeavyGlass = HeavyMaterial(TEXT("M_Trap_HeavyGlass"), true, Assets.Noise4K, Assets.HdrUncompressed, ToSave);
		Assets.DenseMesh = DenseMesh(Assets.HeavyOpaque, ToSave);
		Assets.HeavyProp = HeavyBlueprint(Assets.DenseMesh, Assets.HeavyOpaque, ToSave);
		if (!ToSave.IsEmpty())
		{
			UEditorLoadingAndSavingUtils::SavePackages(ToSave, false);
		}
		UE_LOG(LogOptiCompanion, Display, TEXT("Trap assets ready: %d created, the rest reused."), ToSave.Num());
		return Assets;
	}
}
