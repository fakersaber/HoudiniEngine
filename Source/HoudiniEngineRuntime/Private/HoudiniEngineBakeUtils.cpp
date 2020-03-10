/*
* Copyright (c) <2017> Side Effects Software Inc.
*
* Permission is hereby granted, free of charge, to any person obtaining a copy
* of this software and associated documentation files (the "Software"), to deal
* in the Software without restriction, including without limitation the rights
* to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
* copies of the Software, and to permit persons to whom the Software is
* furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included in all
* copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
* AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
* LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
* OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
* SOFTWARE.
*
*/

#include "HoudiniEngineBakeUtils.h"

#include "HoudiniApi.h"
#include "HoudiniEngineRuntimePrivatePCH.h"
#include "HoudiniEngineUtils.h"
#include "HoudiniRuntimeSettings.h"
#include "HoudiniAssetActor.h"
#include "HoudiniAssetComponent.h"
#include "HoudiniEngine.h"
#include "HoudiniAsset.h"
#include "HoudiniEngineString.h"
#include "HoudiniInstancedActorComponent.h"
#include "HoudiniMeshSplitInstancerComponent.h"
#include "HoudiniAssetInstanceInputField.h"

#include "CoreMinimal.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Materials/Material.h"

#if WITH_EDITOR
#include "ActorFactories/ActorFactory.h"
#include "Editor.h"
#include "Factories/MaterialFactoryNew.h"
#include "ActorFactories/ActorFactoryStaticMesh.h"
#include "Interfaces/ITargetPlatform.h"
#include "Interfaces/ITargetPlatformManagerModule.h"
#include "FileHelpers.h"
#include "Editor/UnrealEd/Public/Layers/ILayers.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstance.h"
#include "Materials/MaterialExpressionTextureSample.h"
#include "Materials/MaterialExpressionTextureCoordinate.h"
#include "StaticMeshResources.h"
#include "InstancedFoliage.h"
#include "InstancedFoliageActor.h"
#include "Layers/LayersSubsystem.h"

#include "LandscapeInfo.h"
#include "Engine/WorldComposition.h"
#include "Templates/SharedPointer.h"
#include "LevelUtils.h"
#include "LandscapeHeightfieldCollisionComponent.h"
#include "LandscapeStreamingProxy.h"
#include "LandscapeEdit.h"

#include "WorldBrowserModule.h"
#include "LevelCollectionModel.h"
#include "Tiles/WorldTileCollectionModel.h"
#include "Tiles/WorldTileModel.h"
#include "Tiles/WorldTileDetails.h"


#endif
#include "EngineUtils.h"
#include "UObject/MetaData.h"
#include "PhysicsEngine/BodySetup.h"
#include "Components/InstancedStaticMeshComponent.h"

#if PLATFORM_WINDOWS
#include "Windows/WindowsHWrapper.h"

// Of course, Windows defines its own GetGeoInfo,
// So we need to undefine that before including HoudiniApi.h to avoid collision...
#ifdef GetGeoInfo
#undef GetGeoInfo
#endif
#endif












#define LOCTEXT_NAMESPACE HOUDINI_LOCTEXT_NAMESPACE 

UPackage *
FHoudiniEngineBakeUtils::BakeCreateBlueprintPackageForComponent(
	UHoudiniAssetComponent * HoudiniAssetComponent,
	FString & BlueprintName)
{
	UPackage * Package = nullptr;

#if WITH_EDITOR

	if (!HoudiniAssetComponent || HoudiniAssetComponent->IsPendingKill())
		return nullptr;

	FString HoudiniAssetName;
	if (HoudiniAssetComponent->HoudiniAsset)
		HoudiniAssetName = HoudiniAssetComponent->HoudiniAsset->GetName();
	else if (HoudiniAssetComponent->GetOuter())
		HoudiniAssetName = HoudiniAssetComponent->GetOuter()->GetName();
	else
		HoudiniAssetName = HoudiniAssetComponent->GetName();

	FGuid BakeGUID = FGuid::NewGuid();

	if (!BakeGUID.IsValid())
		BakeGUID = FGuid::NewGuid();

	// We only want half of generated guid string.
	FString BakeGUIDString = BakeGUID.ToString().Left(FHoudiniEngineUtils::PackageGUIDItemNameLength);

	// Generate Blueprint name.
	BlueprintName = HoudiniAssetName + TEXT("_") + BakeGUIDString;

	// Generate unique package name.
	FString PackageName = HoudiniAssetComponent->GetBakeFolder().ToString() + TEXT("/") + BlueprintName;

	PackageName = UPackageTools::SanitizePackageName(PackageName);

	// See if package exists, if it does, we need to regenerate the name.
	Package = FindPackage(nullptr, *PackageName);

	if (Package && !Package->IsPendingKill())
	{
		// Package does exist, there's a collision, we need to generate a new name.
		BakeGUID.Invalidate();
	}
	else
	{
		// Create actual package.
		Package = CreatePackage(nullptr, *PackageName);
	}
#endif

	return Package;
}

UStaticMesh *
FHoudiniEngineBakeUtils::BakeStaticMesh(
	UHoudiniAssetComponent * HoudiniAssetComponent,
	const FHoudiniGeoPartObject & HoudiniGeoPartObject,
	UStaticMesh * InStaticMesh)
{
	UStaticMesh * StaticMesh = nullptr;

#if WITH_EDITOR

	if (!HoudiniAssetComponent || HoudiniAssetComponent->IsPendingKill())
		return nullptr;

	UHoudiniAsset * HoudiniAsset = HoudiniAssetComponent->HoudiniAsset;
	if (!HoudiniAsset || HoudiniAsset->IsPendingKill())
		return nullptr;

	// We cannot bake curves.
	if (HoudiniGeoPartObject.IsCurve())
		return nullptr;

	if (HoudiniGeoPartObject.IsInstancer())
	{
		HOUDINI_LOG_MESSAGE(TEXT("Baking of instanced static meshes is not supported at the moment."));
		return nullptr;
	}

	// Get platform manager LOD specific information.
	ITargetPlatform * CurrentPlatform = GetTargetPlatformManagerRef().GetRunningTargetPlatform();
	check(CurrentPlatform);
	const FStaticMeshLODGroup & LODGroup = CurrentPlatform->GetStaticMeshLODSettings().GetLODGroup(NAME_None);
	int32 NumLODs = LODGroup.GetDefaultNumLODs();

	// Get runtime settings.
	const UHoudiniRuntimeSettings * HoudiniRuntimeSettings = GetDefault< UHoudiniRuntimeSettings >();
	check(HoudiniRuntimeSettings);

	FHoudiniCookParams HoudiniCookParams(HoudiniAssetComponent);
	HoudiniCookParams.StaticMeshBakeMode = EBakeMode::CreateNewAssets;
	FString MeshName;
	FGuid BakeGUID;
	UPackage * Package = FHoudiniEngineBakeUtils::BakeCreateStaticMeshPackageForComponent(
		HoudiniCookParams, HoudiniGeoPartObject, MeshName, BakeGUID);

	if (!Package || Package->IsPendingKill())
		return nullptr;

	// Create static mesh.
	StaticMesh = NewObject< UStaticMesh >(Package, FName(*MeshName), RF_Public | RF_Transactional);
	if (!StaticMesh || StaticMesh->IsPendingKill())
		return nullptr;

	// Add meta information to this package.
	FHoudiniEngineBakeUtils::AddHoudiniMetaInformationToPackage(
		Package, StaticMesh, HAPI_UNREAL_PACKAGE_META_GENERATED_OBJECT, TEXT("true"));
	FHoudiniEngineBakeUtils::AddHoudiniMetaInformationToPackage(
		Package, StaticMesh, HAPI_UNREAL_PACKAGE_META_GENERATED_NAME, *MeshName);

	// Notify registry that we created a new asset.
	FAssetRegistryModule::AssetCreated(StaticMesh);

	// Copy materials.
	StaticMesh->StaticMaterials = InStaticMesh->StaticMaterials;

	// Create new source model for current static mesh.
	if (!StaticMesh->GetNumSourceModels())
		StaticMesh->AddSourceModel();

	FStaticMeshSourceModel * SrcModel = &StaticMesh->GetSourceModel(0);

	// Load raw data bytes.
	FRawMesh RawMesh;
	FStaticMeshSourceModel * InSrcModel = &InStaticMesh->GetSourceModel(0);
	InSrcModel->LoadRawMesh(RawMesh);

	// Some mesh generation settings.
	HoudiniRuntimeSettings->SetMeshBuildSettings(SrcModel->BuildSettings, RawMesh);

	// Setting the DistanceField resolution
	SrcModel->BuildSettings.DistanceFieldResolutionScale = HoudiniAssetComponent->GeneratedDistanceFieldResolutionScale;

	// We need to check light map uv set for correctness. Unreal seems to have occasional issues with
	// zero UV sets when building lightmaps.
	if (SrcModel->BuildSettings.bGenerateLightmapUVs)
	{
		// See if we need to disable lightmap generation because of bad UVs.
		if (FHoudiniEngineUtils::ContainsInvalidLightmapFaces(RawMesh, StaticMesh->LightMapCoordinateIndex))
		{
			SrcModel->BuildSettings.bGenerateLightmapUVs = false;

			HOUDINI_LOG_MESSAGE(
				TEXT("Skipping Lightmap Generation: Object %s ")
				TEXT("- skipping."),
				*MeshName);
		}
	}

	// Store the new raw mesh.
	SrcModel->StaticMeshOwner = StaticMesh;
	SrcModel->SaveRawMesh(RawMesh);

	while (StaticMesh->GetNumSourceModels() < NumLODs)
		StaticMesh->AddSourceModel();

	for (int32 ModelLODIndex = 0; ModelLODIndex < NumLODs; ++ModelLODIndex)
	{
		StaticMesh->GetSourceModel(ModelLODIndex).ReductionSettings = LODGroup.GetDefaultSettings(ModelLODIndex);

		for (int32 MaterialIndex = 0; MaterialIndex < StaticMesh->StaticMaterials.Num(); ++MaterialIndex)
		{
			FMeshSectionInfo Info = StaticMesh->GetSectionInfoMap().Get(ModelLODIndex, MaterialIndex);
			Info.MaterialIndex = MaterialIndex;
			Info.bEnableCollision = true;
			Info.bCastShadow = true;
			StaticMesh->GetSectionInfoMap().Set(ModelLODIndex, MaterialIndex, Info);
		}
	}

	// Assign generation parameters for this static mesh.
	HoudiniAssetComponent->SetStaticMeshGenerationParameters(StaticMesh);

	// Copy custom lightmap resolution if it is set.
	if (InStaticMesh->LightMapResolution != StaticMesh->LightMapResolution)
		StaticMesh->LightMapResolution = InStaticMesh->LightMapResolution;

	if (HoudiniGeoPartObject.IsCollidable() || HoudiniGeoPartObject.IsRenderCollidable())
	{
		UBodySetup * BodySetup = StaticMesh->BodySetup;
		if (BodySetup && !BodySetup->IsPendingKill())
		{
			// Enable collisions for this static mesh.
			BodySetup->CollisionTraceFlag = ECollisionTraceFlag::CTF_UseComplexAsSimple;
		}
	}

	FHoudiniScopedGlobalSilence ScopedGlobalSilence;
	StaticMesh->Build(true);
	StaticMesh->MarkPackageDirty();

#endif

	return StaticMesh;
}

UBlueprint *
FHoudiniEngineBakeUtils::BakeBlueprint(UHoudiniAssetComponent * HoudiniAssetComponent)
{
	UBlueprint * Blueprint = nullptr;

	if (!HoudiniAssetComponent || HoudiniAssetComponent->IsPendingKill())
		return nullptr;

#if WITH_EDITOR

	// Create package for our Blueprint.
	FString BlueprintName = TEXT("");
	UPackage * Package = FHoudiniEngineBakeUtils::BakeCreateBlueprintPackageForComponent(
		HoudiniAssetComponent, BlueprintName);

	//Bake the asset's landscape
	BakeLandscape(HoudiniAssetComponent);

	if (Package && !Package->IsPendingKill())
	{
		AActor * Actor = HoudiniAssetComponent->CloneComponentsAndCreateActor();
		if (Actor && !Actor->IsPendingKill())
		{
			Blueprint = FKismetEditorUtilities::CreateBlueprintFromActor(*BlueprintName, Package, Actor, false);

			// If actor is rooted, unroot it. We can also delete intermediate actor.
			Actor->RemoveFromRoot();
			Actor->ConditionalBeginDestroy();

			if (Blueprint && !Blueprint->IsPendingKill())
				FAssetRegistryModule::AssetCreated(Blueprint);
		}
	}

#endif

	return Blueprint;
}

AActor *
FHoudiniEngineBakeUtils::ReplaceHoudiniActorWithBlueprint(UHoudiniAssetComponent * HoudiniAssetComponent)
{
	AActor * Actor = nullptr;

	if (!HoudiniAssetComponent || HoudiniAssetComponent->IsPendingKill())
		return nullptr;

#if WITH_EDITOR

	// Create package for our Blueprint.
	FString BlueprintName = TEXT("");
	UPackage * Package = FHoudiniEngineBakeUtils::BakeCreateBlueprintPackageForComponent(HoudiniAssetComponent, BlueprintName);
	if (!Package || Package->IsPendingKill())
		return nullptr;

	//Bake the asset's landscape
	BakeLandscape(HoudiniAssetComponent);

	AActor * ClonedActor = HoudiniAssetComponent->CloneComponentsAndCreateActor();
	if (!ClonedActor || ClonedActor->IsPendingKill())
		return nullptr;

	UBlueprint * Blueprint = FKismetEditorUtilities::CreateBlueprint(
		ClonedActor->GetClass(), Package, *BlueprintName,
		EBlueprintType::BPTYPE_Normal, UBlueprint::StaticClass(),
		UBlueprintGeneratedClass::StaticClass(), FName("CreateFromActor"));

	if (Blueprint && !Blueprint->IsPendingKill())
	{
		Package->MarkPackageDirty();

		if (ClonedActor->GetInstanceComponents().Num() > 0)
			FKismetEditorUtilities::AddComponentsToBlueprint(Blueprint, ClonedActor->GetInstanceComponents());

		if (Blueprint->GeneratedClass)
		{
			AActor * CDO = Cast< AActor >(Blueprint->GeneratedClass->GetDefaultObject());
			if (!CDO || CDO->IsPendingKill())
				return nullptr;

			const auto CopyOptions = (EditorUtilities::ECopyOptions::Type)
				(EditorUtilities::ECopyOptions::OnlyCopyEditOrInterpProperties |
					EditorUtilities::ECopyOptions::PropagateChangesToArchetypeInstances);

			EditorUtilities::CopyActorProperties(ClonedActor, CDO, CopyOptions);

			USceneComponent * Scene = CDO->GetRootComponent();
			if (Scene && !Scene->IsPendingKill())
			{
				Scene->SetRelativeLocation(FVector::ZeroVector);
				Scene->SetRelativeRotation(FRotator::ZeroRotator);

				// Clear out the attachment info after having copied the properties from the source actor
				Scene->DetachFromComponent(FDetachmentTransformRules::KeepRelativeTransform);
				while (true)
				{
					const int32 ChildCount = Scene->GetAttachChildren().Num();
					if (ChildCount < 1)
						break;

					USceneComponent * Component = Scene->GetAttachChildren()[ChildCount - 1];
					if (Component && !Component->IsPendingKill())
						Component->DetachFromComponent(FDetachmentTransformRules::KeepRelativeTransform);
				}
				check(Scene->GetAttachChildren().Num() == 0);

				// Ensure the light mass information is cleaned up
				Scene->InvalidateLightingCache();
			}
		}

		// Compile our blueprint and notify asset system about blueprint.
		FKismetEditorUtilities::CompileBlueprint(Blueprint);
		FAssetRegistryModule::AssetCreated(Blueprint);

		// Retrieve actor transform.
		FVector Location = ClonedActor->GetActorLocation();
		FRotator Rotator = ClonedActor->GetActorRotation();

		// Replace cloned actor with Blueprint instance.
		{
			TArray< AActor * > Actors;
			Actors.Add(ClonedActor);

			ClonedActor->RemoveFromRoot();
			Actor = FKismetEditorUtilities::CreateBlueprintInstanceFromSelection(Blueprint, Actors, Location, Rotator);
		}

		// We can initiate Houdini actor deletion.
		DeleteBakedHoudiniAssetActor(HoudiniAssetComponent);
	}
	else
	{
		ClonedActor->RemoveFromRoot();
		ClonedActor->ConditionalBeginDestroy();
	}

#endif

	return Actor;
}

UStaticMesh *
FHoudiniEngineBakeUtils::DuplicateStaticMeshAndCreatePackage(
	const UStaticMesh * StaticMesh, UHoudiniAssetComponent * Component,
	const FHoudiniGeoPartObject & HoudiniGeoPartObject, EBakeMode BakeMode)
{
	UStaticMesh * DuplicatedStaticMesh = nullptr;
#if WITH_EDITOR
	if (!HoudiniGeoPartObject.IsCurve() && !HoudiniGeoPartObject.IsInstancer() && !HoudiniGeoPartObject.IsPackedPrimitiveInstancer() && !HoudiniGeoPartObject.IsVolume())
	{
		// Create package for this duplicated mesh.
		FHoudiniCookParams HoudiniCookParams(Component);
		// Transferring the CookMode to the materials
		// We're either using the default one, or a custom one
		HoudiniCookParams.StaticMeshBakeMode = BakeMode;
		if (BakeMode == FHoudiniCookParams::GetDefaultStaticMeshesCookMode())
			HoudiniCookParams.MaterialAndTextureBakeMode = FHoudiniCookParams::GetDefaultMaterialAndTextureCookMode();
		else
			HoudiniCookParams.MaterialAndTextureBakeMode = BakeMode;

		FString MeshName;
		FGuid MeshGuid;

		UPackage * MeshPackage = FHoudiniEngineBakeUtils::BakeCreateStaticMeshPackageForComponent(
			HoudiniCookParams, HoudiniGeoPartObject, MeshName, MeshGuid);
		if (!MeshPackage || MeshPackage->IsPendingKill())
			return nullptr;

		// We need to be sure the package has been fully loaded before calling DuplicateObject
		if (!MeshPackage->IsFullyLoaded())
		{
			FlushAsyncLoading();
			if (!MeshPackage->GetOuter())
			{
				MeshPackage->FullyLoad();
			}
			else
			{
				MeshPackage->GetOutermost()->FullyLoad();
			}
		}

		// Duplicate mesh for this new copied component.
		DuplicatedStaticMesh = DuplicateObject< UStaticMesh >(StaticMesh, MeshPackage, *MeshName);
		if (!DuplicatedStaticMesh || DuplicatedStaticMesh->IsPendingKill())
			return nullptr;

		if (BakeMode != EBakeMode::Intermediate)
			DuplicatedStaticMesh->SetFlags(RF_Public | RF_Standalone);

		// Add meta information.
		FHoudiniEngineBakeUtils::AddHoudiniMetaInformationToPackage(
			MeshPackage, DuplicatedStaticMesh,
			HAPI_UNREAL_PACKAGE_META_GENERATED_OBJECT, TEXT("true"));
		FHoudiniEngineBakeUtils::AddHoudiniMetaInformationToPackage(
			MeshPackage, DuplicatedStaticMesh,
			HAPI_UNREAL_PACKAGE_META_GENERATED_NAME, *MeshName);

		// See if we need to duplicate materials and textures.
		TArray< FStaticMaterial > DuplicatedMaterials;
		TArray< FStaticMaterial > & Materials = DuplicatedStaticMesh->StaticMaterials;

		for (int32 MaterialIdx = 0; MaterialIdx < Materials.Num(); ++MaterialIdx)
		{
			UMaterialInterface* MaterialInterface = Materials[MaterialIdx].MaterialInterface;
			if (MaterialInterface)
			{
				UPackage * MaterialPackage = Cast< UPackage >(MaterialInterface->GetOuter());
				if (MaterialPackage && !MaterialPackage->IsPendingKill())
				{
					FString MaterialName;
					if (FHoudiniEngineBakeUtils::GetHoudiniGeneratedNameFromMetaInformation(
						MaterialPackage, MaterialInterface, MaterialName))
					{
						// We only deal with materials.
						UMaterial * Material = Cast< UMaterial >(MaterialInterface);
						if (Material && !Material->IsPendingKill())
						{
							// Duplicate material resource.
							UMaterial * DuplicatedMaterial = FHoudiniEngineBakeUtils::DuplicateMaterialAndCreatePackage(
								Material, HoudiniCookParams, MaterialName);

							if (!DuplicatedMaterial || DuplicatedMaterial->IsPendingKill())
								continue;

							// Store duplicated material.
							FStaticMaterial DupeStaticMaterial = Materials[MaterialIdx];
							DupeStaticMaterial.MaterialInterface = DuplicatedMaterial;
							DuplicatedMaterials.Add(DupeStaticMaterial);
							continue;
						}
					}
				}
			}

			DuplicatedMaterials.Add(Materials[MaterialIdx]);
		}

		/*
		// We need to be sure the package has been fully loaded before calling DuplicateObject
		if (!MeshPackage->IsFullyLoaded())
		{
			FlushAsyncLoading();
			if (!MeshPackage->GetOuter())
			{
				MeshPackage->FullyLoad();
			}
			else
			{
				MeshPackage->GetOutermost()->FullyLoad();
			}
		}
		*/

		// Assign duplicated materials.
		DuplicatedStaticMesh->StaticMaterials = DuplicatedMaterials;

		// Notify registry that we have created a new duplicate mesh.
		FAssetRegistryModule::AssetCreated(DuplicatedStaticMesh);

		// Dirty the static mesh package.
		DuplicatedStaticMesh->MarkPackageDirty();
	}
#endif
	return DuplicatedStaticMesh;
}

bool
FHoudiniEngineBakeUtils::ReplaceHoudiniActorWithActors(UHoudiniAssetComponent * HoudiniAssetComponent, bool SelectNewActors)
{
	bool bSuccess = false;
#if WITH_EDITOR
	if (FHoudiniEngineBakeUtils::BakeHoudiniActorToActors(HoudiniAssetComponent, SelectNewActors))
	{
		bSuccess = FHoudiniEngineBakeUtils::DeleteBakedHoudiniAssetActor(HoudiniAssetComponent);
	}
#endif
	return bSuccess;
}


bool
FHoudiniEngineBakeUtils::BakeHoudiniActorToActors(UHoudiniAssetComponent * HoudiniAssetComponent, bool SelectNewActors)
{
	bool bSuccess = false;
#if WITH_EDITOR
	if (!HoudiniAssetComponent || HoudiniAssetComponent->IsPendingKill())
		return bSuccess;

	const FScopedTransaction Transaction(LOCTEXT("BakeToActors", "Bake To Actors"));

	auto SMComponentToPart = HoudiniAssetComponent->CollectAllStaticMeshComponents();
	TArray< AActor* > NewActors = BakeHoudiniActorToActors_StaticMeshes(HoudiniAssetComponent, SMComponentToPart);

	auto IAComponentToPart = HoudiniAssetComponent->CollectAllInstancedActorComponents();
	NewActors.Append(BakeHoudiniActorToActors_InstancedActors(HoudiniAssetComponent, IAComponentToPart));

	auto SplitMeshInstancerComponentToPart = HoudiniAssetComponent->CollectAllMeshSplitInstancerComponents();
	NewActors.Append(BakeHoudiniActorToActors_SplitMeshInstancers(HoudiniAssetComponent, SplitMeshInstancerComponentToPart));

	bSuccess = NewActors.Num() > 0;

	if (GEditor && SelectNewActors && bSuccess)
	{
		GEditor->SelectNone(false, true);
		for (AActor* NewActor : NewActors)
		{
			if (NewActor && !NewActor->IsPendingKill())
				GEditor->SelectActor(NewActor, true, false);
		}
		GEditor->NoteSelectionChange();
	}
#endif

	return bSuccess;
}

TArray< AActor* >
FHoudiniEngineBakeUtils::BakeHoudiniActorToActors_InstancedActors(
	UHoudiniAssetComponent * HoudiniAssetComponent,
	TMap< const UHoudiniInstancedActorComponent*, FHoudiniGeoPartObject >& ComponentToPart)
{
	TArray< AActor* > NewActors;
	if (!HoudiniAssetComponent || HoudiniAssetComponent->IsPendingKill())
		return NewActors;

#if WITH_EDITOR
	ULevel* DesiredLevel = GWorld->GetCurrentLevel();
	FName BaseName(*(HoudiniAssetComponent->GetOwner()->GetName() + TEXT("_Baked")));

	for (const auto& Iter : ComponentToPart)
	{
		const UHoudiniInstancedActorComponent * OtherSMC = Iter.Key;
		if (!OtherSMC || OtherSMC->IsPendingKill())
			continue;

		for (AActor* InstActor : OtherSMC->Instances)
		{
			if (!InstActor || InstActor->IsPendingKill())
				continue;

			FName NewName = MakeUniqueObjectName(DesiredLevel, OtherSMC->InstancedAsset->StaticClass(), BaseName);
			FString NewNameStr = NewName.ToString();

			FTransform CurrentTransform = InstActor->GetTransform();
			AActor* NewActor = OtherSMC->SpawnInstancedActor(CurrentTransform);
			if (NewActor && !NewActor->IsPendingKill())
			{
				NewActor->SetActorLabel(NewNameStr);
				NewActor->SetFolderPath(BaseName);
				NewActor->SetActorTransform(CurrentTransform);
			}
		}
	}
#endif
	return NewActors;
}

void
FHoudiniEngineBakeUtils::CheckedBakeStaticMesh(
	UHoudiniAssetComponent* HoudiniAssetComponent,
	TMap< const UStaticMesh*, UStaticMesh* >& OriginalToBakedMesh,
	const FHoudiniGeoPartObject & HoudiniGeoPartObject, UStaticMesh* OriginalSM)
{
	UStaticMesh* BakedSM = nullptr;
	if (UStaticMesh ** FoundMeshPtr = OriginalToBakedMesh.Find(OriginalSM))
	{
		// We've already baked this mesh, use it
		BakedSM = *FoundMeshPtr;
	}
	else
	{
		if (FHoudiniEngineBakeUtils::StaticMeshRequiresBake(OriginalSM))
		{
			// Bake the found mesh into the project
			BakedSM = FHoudiniEngineBakeUtils::DuplicateStaticMeshAndCreatePackage(
				OriginalSM, HoudiniAssetComponent, HoudiniGeoPartObject, EBakeMode::CreateNewAssets);

			if (ensure(BakedSM))
			{
				FAssetRegistryModule::AssetCreated(BakedSM);
			}
		}
		else
		{
			// We didn't bake this mesh, but it's already baked so we will just use it as is
			BakedSM = OriginalSM;
		}
		OriginalToBakedMesh.Add(OriginalSM, BakedSM);
	}
}

TArray< AActor* >
FHoudiniEngineBakeUtils::BakeHoudiniActorToActors_StaticMeshes(
	UHoudiniAssetComponent * HoudiniAssetComponent,
	TMap< const UStaticMeshComponent*, FHoudiniGeoPartObject >& SMComponentToPart)
{
	TMap< const UStaticMesh*, UStaticMesh* > OriginalToBakedMesh;

	TArray< AActor* > NewActors;
	if (!HoudiniAssetComponent || HoudiniAssetComponent->IsPendingKill())
		return NewActors;

#if WITH_EDITOR
	// Loop over all comps, bake static mesh if not already baked, and create an actor for every one of them    
	for (const auto& Iter : SMComponentToPart)
	{
		const FHoudiniGeoPartObject & HoudiniGeoPartObject = Iter.Value;
		const UStaticMeshComponent * OtherSMC = Iter.Key;
		if (!OtherSMC || OtherSMC->IsPendingKill())
			continue;

		UStaticMesh * OtherSM = OtherSMC->GetStaticMesh();
		if (!OtherSM || OtherSM->IsPendingKill())
			continue;

		CheckedBakeStaticMesh(HoudiniAssetComponent, OriginalToBakedMesh, HoudiniGeoPartObject, OtherSM);
	}

	// Finished baking, now spawn the actors

	for (const auto& Iter : SMComponentToPart)
	{
		const UStaticMeshComponent * OtherSMC = Iter.Key;
		if (!OtherSMC || OtherSMC->IsPendingKill())
			continue;

		const FHoudiniGeoPartObject & HoudiniGeoPartObject = Iter.Value;
		UStaticMesh* BakedSM = OriginalToBakedMesh[OtherSMC->GetStaticMesh()];

		if (!BakedSM || BakedSM->IsPendingKill())
			continue;

		ULevel* DesiredLevel = GWorld->GetCurrentLevel();
		FName BaseName(*(HoudiniAssetComponent->GetOwner()->GetName() + TEXT("_Baked")));
		UActorFactory* Factory = GEditor ? GEditor->FindActorFactoryByClass(UActorFactoryStaticMesh::StaticClass()) : nullptr;
		if (!Factory)
			continue;

		auto PrepNewStaticMeshActor = [&](AActor* NewActor)
		{
			if (!NewActor || NewActor->IsPendingKill())
				return;

			// The default name will be based on the static mesh package, we would prefer it to be based on the Houdini asset
			FName NewName = MakeUniqueObjectName(DesiredLevel, Factory->NewActorClass, BaseName);
			FString NewNameStr = NewName.ToString();
			NewActor->Rename(*NewNameStr);
			NewActor->SetActorLabel(NewNameStr);
			NewActor->SetFolderPath(BaseName);

			// Copy properties to new actor
			AStaticMeshActor* SMActor = Cast< AStaticMeshActor>(NewActor);
			if (!SMActor || SMActor->IsPendingKill())
				return;
			UStaticMeshComponent* SMC = SMActor->GetStaticMeshComponent();
			if (!SMC || SMC->IsPendingKill())
				return;

			UStaticMeshComponent* OtherSMC_NonConst = const_cast<UStaticMeshComponent*>(OtherSMC);

			SMC->SetCollisionProfileName(OtherSMC_NonConst->GetCollisionProfileName());
			SMC->SetCollisionEnabled(OtherSMC->GetCollisionEnabled());
			SMC->LightmassSettings = OtherSMC->LightmassSettings;
			SMC->CastShadow = OtherSMC->CastShadow;
			SMC->SetMobility(OtherSMC->Mobility);

			if (OtherSMC_NonConst->GetBodySetup() && SMC->GetBodySetup())
			{
				// Copy the BodySetup
				SMC->GetBodySetup()->CopyBodyPropertiesFrom(OtherSMC_NonConst->GetBodySetup());

				// We need to recreate the physics mesh for the new body setup
				SMC->GetBodySetup()->InvalidatePhysicsData();
				SMC->GetBodySetup()->CreatePhysicsMeshes();

				// Only copy the physical material if it's different from the default one,
				// As this was causing crashes on BakeToActors in some cases
				if (GEngine != NULL && OtherSMC_NonConst->GetBodySetup()->GetPhysMaterial() != GEngine->DefaultPhysMaterial)
					SMC->SetPhysMaterialOverride(OtherSMC_NonConst->GetBodySetup()->GetPhysMaterial());
			}
			SMActor->SetActorHiddenInGame(OtherSMC->bHiddenInGame);
			SMC->SetVisibility(OtherSMC->IsVisible());

			// Reapply the uproperties modified by attributes on the new component
			FHoudiniEngineUtils::UpdateUPropertyAttributesOnObject(SMC, HoudiniGeoPartObject);

			SMC->PostEditChange();
		};

		const UInstancedStaticMeshComponent* OtherISMC = Cast< const UInstancedStaticMeshComponent>(OtherSMC);
		if (OtherISMC && !OtherISMC->IsPendingKill())
		{
#ifdef BAKE_TO_INSTANCEDSTATICMESHCOMPONENT_ACTORS
			// This is an instanced static mesh component - we will create a generic AActor with a UInstancedStaticMeshComponent root
			FActorSpawnParameters SpawnInfo;
			SpawnInfo.OverrideLevel = DesiredLevel;
			SpawnInfo.ObjectFlags = RF_Transactional;
			SpawnInfo.Name = MakeUniqueObjectName(DesiredLevel, AActor::StaticClass(), BaseName);
			SpawnInfo.bDeferConstruction = true;

			if (AActor* NewActor = DesiredLevel->OwningWorld->SpawnActor<AActor>(SpawnInfo))
			{
				NewActor->SetActorLabel(NewActor->GetName());
				NewActor->SetActorHiddenInGame(OtherISMC->bHiddenInGame);

				// Do we need to create a HISMC?
				const UHierarchicalInstancedStaticMeshComponent* OtherHISMC = Cast< const UHierarchicalInstancedStaticMeshComponent>(OtherSMC);
				UInstancedStaticMeshComponent* NewISMC = nullptr;
				if (OtherHISMC)
					NewISMC = DuplicateObject< UHierarchicalInstancedStaticMeshComponent >(OtherHISMC, NewActor, *OtherHISMC->GetName()) );
				else
					NewISMC = DuplicateObject< UInstancedStaticMeshComponent >(OtherISMC, NewActor, *OtherISMC->GetName()) );

					if (NewISMC)
					{
						NewISMC->SetupAttachment(nullptr);
						NewISMC->SetStaticMesh(BakedSM);
						NewActor->AddInstanceComponent(NewISMC);
						NewActor->SetRootComponent(NewISMC);
						NewISMC->SetWorldTransform(OtherISMC->GetComponentTransform());
						NewISMC->RegisterComponent();

						NewActor->SetFolderPath(BaseName);
						NewActor->FinishSpawning(OtherISMC->GetComponentTransform());

						NewActors.Add(NewActor);
						NewActor->InvalidateLightingCache();
						NewActor->PostEditMove(true);
						NewActor->MarkPackageDirty();
					}
			}
#else
			// This is an instanced static mesh component - we will split it up into StaticMeshActors
			for (int32 InstanceIx = 0; InstanceIx < OtherISMC->GetInstanceCount(); ++InstanceIx)
			{
				FTransform InstanceTransform;
				OtherISMC->GetInstanceTransform(InstanceIx, InstanceTransform, true);
				AActor* NewActor = Factory->CreateActor(BakedSM, DesiredLevel, InstanceTransform, RF_Transactional);
				if (!NewActor || NewActor->IsPendingKill())
					continue;

				PrepNewStaticMeshActor(NewActor);

				// We need to set the modified uproperty on the created actor
				AStaticMeshActor* SMActor = Cast< AStaticMeshActor>(NewActor);
				if (!SMActor || SMActor->IsPendingKill())
					continue;

				UStaticMeshComponent* SMC = SMActor->GetStaticMeshComponent();
				if (!SMC || SMC->IsPendingKill())
					continue;
				FHoudiniGeoPartObject GeoPartObject = HoudiniAssetComponent->LocateGeoPartObject(OtherSMC->GetStaticMesh());

				// Set the part id to 0 so we can access the instancer
				GeoPartObject.PartId = 0;
				FHoudiniEngineUtils::UpdateUPropertyAttributesOnObject(SMC, GeoPartObject);
			}
#endif
		}
		else
		{
			AActor* NewActor = Factory->CreateActor(BakedSM, DesiredLevel, OtherSMC->GetComponentTransform(), RF_Transactional);
			if (NewActor && !NewActor->IsPendingKill())
				PrepNewStaticMeshActor(NewActor);
		}
	}
#endif
	return NewActors;
}

TArray<AActor*>
FHoudiniEngineBakeUtils::BakeHoudiniActorToActors_SplitMeshInstancers(UHoudiniAssetComponent * HoudiniAssetComponent,
	TMap<const UHoudiniMeshSplitInstancerComponent *, FHoudiniGeoPartObject> SplitMeshInstancerComponentToPart)
{
	TArray< AActor* > NewActors;
	if (!HoudiniAssetComponent || HoudiniAssetComponent->IsPendingKill())
		return NewActors;

#if WITH_EDITOR
	TMap< const UStaticMesh*, UStaticMesh* > OriginalToBakedMesh;

	for (auto&& Iter : SplitMeshInstancerComponentToPart)
	{
		auto&& HoudiniGeoPartObject = Iter.Value;
		auto&& OtherMSIC = Iter.Key;

		if (!OtherMSIC || OtherMSIC->IsPendingKill())
			continue;

		UStaticMesh * OtherSM = OtherMSIC->GetStaticMesh();

		if (!OtherSM || OtherSM->IsPendingKill())
			continue;

		CheckedBakeStaticMesh(HoudiniAssetComponent, OriginalToBakedMesh, HoudiniGeoPartObject, OtherSM);
	}
	// Done baking, now spawn the actors
	for (auto&& Iter : SplitMeshInstancerComponentToPart)
	{
		auto&& OtherMSIC = Iter.Key;

		UStaticMesh* BakedSM = OriginalToBakedMesh[OtherMSIC->GetStaticMesh()];
		if (!BakedSM || BakedSM->IsPendingKill())
			continue;

		ULevel* DesiredLevel = GWorld->GetCurrentLevel();
		FName BaseName(*(HoudiniAssetComponent->GetOwner()->GetName() + TEXT("_Baked")));

		// This is a split mesh instancer component - we will create a generic AActor with a bunch of SMC
		FActorSpawnParameters SpawnInfo;
		SpawnInfo.OverrideLevel = DesiredLevel;
		SpawnInfo.ObjectFlags = RF_Transactional;
		SpawnInfo.Name = MakeUniqueObjectName(DesiredLevel, AActor::StaticClass(), BaseName);
		SpawnInfo.bDeferConstruction = true;

		AActor* NewActor = DesiredLevel->OwningWorld->SpawnActor<AActor>(SpawnInfo);
		if (!NewActor || NewActor->IsPendingKill())
			continue;

		NewActor->SetActorLabel(NewActor->GetName());
		NewActor->SetActorHiddenInGame(OtherMSIC->bHiddenInGame);

		for (UStaticMeshComponent* OtherSMC : OtherMSIC->GetInstances())
		{
			if (!OtherSMC || OtherSMC->IsPendingKill())
				continue;

			UStaticMeshComponent* NewSMC = DuplicateObject< UStaticMeshComponent >(OtherSMC, NewActor, *OtherSMC->GetName());
			if (!NewSMC || NewSMC->IsPendingKill())
				continue;

			NewSMC->SetupAttachment(nullptr);
			NewSMC->SetStaticMesh(BakedSM);
			NewActor->AddInstanceComponent(NewSMC);
			NewSMC->SetWorldTransform(OtherSMC->GetComponentTransform());
			NewSMC->RegisterComponent();
		}

		NewActor->SetFolderPath(BaseName);
		NewActor->FinishSpawning(OtherMSIC->GetComponentTransform());

		NewActors.Add(NewActor);
		NewActor->InvalidateLightingCache();
		NewActor->PostEditMove(true);
		NewActor->MarkPackageDirty();
	}
#endif
	return NewActors;
}

UHoudiniAssetInput*
FHoudiniEngineBakeUtils::GetInputForBakeHoudiniActorToOutlinerInput(const UHoudiniAssetComponent * HoudiniAssetComponent)
{
	if (!HoudiniAssetComponent || HoudiniAssetComponent->IsPendingKill())
		return nullptr;

	UHoudiniAssetInput *FirstInput = nullptr, *Result = nullptr;
#if WITH_EDITOR
	if (HoudiniAssetComponent->GetInputs().Num() && HoudiniAssetComponent->GetInputs()[0])
	{
		FirstInput = HoudiniAssetComponent->GetInputs()[0];
	}
	else
	{
		TMap< FString, UHoudiniAssetParameter* > InputParams;
		HoudiniAssetComponent->CollectAllParametersOfType(UHoudiniAssetInput::StaticClass(), InputParams);
		TArray< UHoudiniAssetParameter* > InputParamsArray;
		InputParams.GenerateValueArray(InputParamsArray);
		if (InputParamsArray.Num() > 0)
		{
			FirstInput = Cast<UHoudiniAssetInput>(InputParamsArray[0]);
		}
	}

	if (FirstInput && !FirstInput->IsPendingKill())
	{
		if (FirstInput->GetChoiceIndex() == EHoudiniAssetInputType::WorldInput && FirstInput->GetWorldOutlinerInputs().Num() == 1)
		{
			const FHoudiniAssetInputOutlinerMesh& InputOutlinerMesh = FirstInput->GetWorldOutlinerInputs()[0];
			if (InputOutlinerMesh.ActorPtr.IsValid() && InputOutlinerMesh.StaticMeshComponent)
			{
				Result = FirstInput;
			}
		}
	}
#endif
	return Result;
}

bool
FHoudiniEngineBakeUtils::CanComponentBakeToOutlinerInput(const UHoudiniAssetComponent * HoudiniAssetComponent)
{
	if (!HoudiniAssetComponent || HoudiniAssetComponent->IsPendingKill())
		return false;

#if WITH_EDITOR
	// TODO: Cache this
	auto SMComponentToPart = HoudiniAssetComponent->CollectAllStaticMeshComponents();
	if (SMComponentToPart.Num() == 1)
	{
		return nullptr != GetInputForBakeHoudiniActorToOutlinerInput(HoudiniAssetComponent);
	}
#endif
	return false;
}

bool
FHoudiniEngineBakeUtils::CanComponentBakeToFoliage(const UHoudiniAssetComponent * HoudiniAssetComponent)
{
	if (!HoudiniAssetComponent || HoudiniAssetComponent->IsPendingKill())
		return false;

	const TArray< UHoudiniAssetInstanceInputField * > InstanceInputFields = HoudiniAssetComponent->GetAllInstanceInputFields();

	return InstanceInputFields.Num() > 0;
}

void
FHoudiniEngineBakeUtils::BakeHoudiniActorToOutlinerInput(UHoudiniAssetComponent * HoudiniAssetComponent)
{
	if (!HoudiniAssetComponent || HoudiniAssetComponent->IsPendingKill())
		return;

#if WITH_EDITOR
	TMap< const UStaticMesh*, UStaticMesh* > OriginalToBakedMesh;
	TMap< const UStaticMeshComponent*, FHoudiniGeoPartObject > SMComponentToPart = HoudiniAssetComponent->CollectAllStaticMeshComponents();

	for (const auto& Iter : SMComponentToPart)
	{
		const FHoudiniGeoPartObject & HoudiniGeoPartObject = Iter.Value;
		const UStaticMeshComponent * OtherSMC = Iter.Key;
		if (!OtherSMC || OtherSMC->IsPendingKill())
			continue;

		UStaticMesh* OtherSM = OtherSMC->GetStaticMesh();
		if (!OtherSM || OtherSM->IsPendingKill())
			continue;

		UStaticMesh* BakedSM = nullptr;
		if (UStaticMesh ** FoundMeshPtr = OriginalToBakedMesh.Find(OtherSM))
		{
			// We've already baked this mesh, use it
			BakedSM = *FoundMeshPtr;
		}
		else
		{
			// Bake the found mesh into the project
			BakedSM = FHoudiniEngineBakeUtils::DuplicateStaticMeshAndCreatePackage(
				OtherSM, HoudiniAssetComponent, HoudiniGeoPartObject, EBakeMode::CreateNewAssets);

			if (BakedSM)
			{
				OriginalToBakedMesh.Add(OtherSM, BakedSM);
				FAssetRegistryModule::AssetCreated(BakedSM);
			}
		}
	}

	{
		const FScopedTransaction Transaction(LOCTEXT("BakeToInput", "Bake To Input"));

		for (auto Iter : OriginalToBakedMesh)
		{
			// Get the first outliner input
			UHoudiniAssetInput* FirstInput = GetInputForBakeHoudiniActorToOutlinerInput(HoudiniAssetComponent);
			if (!FirstInput || FirstInput->IsPendingKill())
				continue;

			const FHoudiniAssetInputOutlinerMesh& InputOutlinerMesh = FirstInput->GetWorldOutlinerInputs()[0];
			if (InputOutlinerMesh.ActorPtr.IsValid() && InputOutlinerMesh.StaticMeshComponent)
			{
				UStaticMeshComponent* InOutSMC = InputOutlinerMesh.StaticMeshComponent;
				if (InOutSMC && !InOutSMC->IsPendingKill())
				{
					InputOutlinerMesh.ActorPtr->Modify();
					InOutSMC->SetStaticMesh(Iter.Value);
					InOutSMC->InvalidateLightingCache();
					InOutSMC->MarkPackageDirty();
				}

				// Disconnect the input from the asset - InputOutlinerMesh now garbage
				FirstInput->RemoveWorldOutlinerInput(0);
			}

			// Only handle the first Baked Mesh
			break;
		}
	}
#endif
}

// Bakes output instanced meshes to the level's foliage actor and removes the Houdini actor
bool
FHoudiniEngineBakeUtils::ReplaceHoudiniActorWithFoliage(UHoudiniAssetComponent * HoudiniAssetComponent)
{
	bool bSuccess = false;

#if WITH_EDITOR
	if (FHoudiniEngineBakeUtils::BakeHoudiniActorToFoliage(HoudiniAssetComponent))
	{
		bSuccess = FHoudiniEngineBakeUtils::DeleteBakedHoudiniAssetActor(HoudiniAssetComponent);
	}
#endif

	return bSuccess;
}

bool
FHoudiniEngineBakeUtils::BakeHoudiniActorToFoliage(UHoudiniAssetComponent * HoudiniAssetComponent)
{
#if WITH_EDITOR
	if (!HoudiniAssetComponent || HoudiniAssetComponent->IsPendingKill())
		return false;

	const FScopedTransaction Transaction(LOCTEXT("BakeToFoliage", "Bake To Foliage"));

	ULevel* DesiredLevel = GWorld->GetCurrentLevel();
	AInstancedFoliageActor* InstancedFoliageActor = AInstancedFoliageActor::GetInstancedFoliageActorForLevel(DesiredLevel, true);
	if (!InstancedFoliageActor || InstancedFoliageActor->IsPendingKill())
		return false;

	// Map storing original and baked Static Meshes
	TMap< const UStaticMesh*, UStaticMesh* > OriginalToBakedMesh;

	int32 BakedCount = 0;
	const TArray< UHoudiniAssetInstanceInputField * > InstanceInputFields = HoudiniAssetComponent->GetAllInstanceInputFields();
	for (int32 Idx = 0; Idx < InstanceInputFields.Num(); ++Idx)
	{
		const UHoudiniAssetInstanceInputField * HoudiniAssetInstanceInputField = InstanceInputFields[Idx];
		if (!HoudiniAssetInstanceInputField || HoudiniAssetInstanceInputField->IsPendingKill())
			continue;

		for (int32 VariationIdx = 0; VariationIdx < HoudiniAssetInstanceInputField->InstanceVariationCount(); VariationIdx++)
		{
			UInstancedStaticMeshComponent * ISMC =
				Cast<UInstancedStaticMeshComponent>(HoudiniAssetInstanceInputField->GetInstancedComponent(VariationIdx));

			if (!ISMC || ISMC->IsPendingKill())
				continue;

			// If the original static mesh is used (the cooked mesh for HAPI), then we need to bake it to a file
			// If not, we don't need to bake a new mesh as we're using a SM override, so can use the existing unreal asset
			UStaticMesh* OutStaticMesh = Cast<UStaticMesh>(HoudiniAssetInstanceInputField->GetInstanceVariation(VariationIdx));
			if (HoudiniAssetInstanceInputField->IsOriginalObjectUsed(VariationIdx))
			{
				UStaticMesh* OriginalSM = Cast<UStaticMesh>(HoudiniAssetInstanceInputField->GetOriginalObject());
				if (!OriginalSM || OriginalSM->IsPendingKill())
					continue;

				// We're instancing a mesh created by the plugin, we need to bake it first
				auto&& ItemGeoPartObject = HoudiniAssetComponent->LocateGeoPartObject(OutStaticMesh);
				FHoudiniEngineBakeUtils::CheckedBakeStaticMesh(HoudiniAssetComponent, OriginalToBakedMesh, ItemGeoPartObject, OriginalSM);
				OutStaticMesh = OriginalToBakedMesh[OutStaticMesh];
			}

			// See if we already have a FoliageType for that mesh
			UFoliageType* FoliageType = InstancedFoliageActor->GetLocalFoliageTypeForSource(OutStaticMesh);
			if (!FoliageType || FoliageType->IsPendingKill())
			{
				// We need to create a new FoliageType for this Static Mesh
				InstancedFoliageActor->AddMesh(OutStaticMesh, &FoliageType, HoudiniAssetComponent->GeneratedFoliageDefaultSettings);
			}

			// Get the FoliageMeshInfo for this Foliage type so we can add the instance to it
			FFoliageInfo* FoliageInfo = InstancedFoliageActor->FindOrAddMesh(FoliageType);
			if (!FoliageInfo)
				continue;

			// Get the transforms for this instance
			TArray< FTransform > ProcessedTransforms;
			HoudiniAssetInstanceInputField->GetProcessedTransforms(ProcessedTransforms, VariationIdx);
			FTransform HoudiniAssetTransform = HoudiniAssetComponent->GetComponentTransform();

			FFoliageInstance FoliageInstance;
			for (auto CurrentTransform : ProcessedTransforms)
			{
				FoliageInstance.Location = HoudiniAssetTransform.TransformPosition(CurrentTransform.GetLocation());
				FoliageInstance.Rotation = HoudiniAssetTransform.TransformRotation(CurrentTransform.GetRotation()).Rotator();
				FoliageInstance.DrawScale3D = CurrentTransform.GetScale3D() * HoudiniAssetTransform.GetScale3D();

				FoliageInfo->AddInstance(InstancedFoliageActor, FoliageType, FoliageInstance);
			}

			// TODO: This was due to a bug in UE4.22-20, check if still needed! 
			if (FoliageInfo->GetComponent())
				FoliageInfo->GetComponent()->BuildTreeIfOutdated(true, true);

			// Notify the user that we succesfully bake the instances to foliage
			FString Notification = TEXT("Successfully baked ") + FString::FromInt(ProcessedTransforms.Num()) + TEXT(" instances of ") + OutStaticMesh->GetName() + TEXT(" to Foliage");
			FHoudiniEngineUtils::CreateSlateNotification(Notification);

			BakedCount += ProcessedTransforms.Num();
		}
	}

	if (BakedCount > 0)
		return true;
#endif

	return false;
}

bool
FHoudiniEngineBakeUtils::StaticMeshRequiresBake(const UStaticMesh * StaticMesh)
{
#if WITH_EDITOR
	if (!StaticMesh || StaticMesh->IsPendingKill())
		return false;

	FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");

	FAssetData BackingAssetData = AssetRegistryModule.Get().GetAssetByObjectPath(*StaticMesh->GetPathName());
	if (!BackingAssetData.IsUAsset())
		return true;

	for (const auto& StaticMaterial : StaticMesh->StaticMaterials)
	{
		if (!StaticMaterial.MaterialInterface || StaticMaterial.MaterialInterface->IsPendingKill())
			continue;

		BackingAssetData = AssetRegistryModule.Get().GetAssetByObjectPath(*StaticMaterial.MaterialInterface->GetPathName());
		if (!BackingAssetData.IsUAsset())
			return true;
	}
#endif
	return false;
}

UMaterial *
FHoudiniEngineBakeUtils::DuplicateMaterialAndCreatePackage(
	UMaterial * Material, FHoudiniCookParams& HoudiniCookParams, const FString & SubMaterialName)
{
	UMaterial * DuplicatedMaterial = nullptr;
#if WITH_EDITOR
	// Create material package.
	FString MaterialName;
	UPackage * MaterialPackage = FHoudiniEngineBakeUtils::BakeCreateTextureOrMaterialPackageForComponent(
		HoudiniCookParams, SubMaterialName, MaterialName);

	if (!MaterialPackage || MaterialPackage->IsPendingKill())
		return nullptr;

	// Clone material.
	DuplicatedMaterial = DuplicateObject< UMaterial >(Material, MaterialPackage, *MaterialName);
	if (!DuplicatedMaterial || DuplicatedMaterial->IsPendingKill())
		return nullptr;

	if (HoudiniCookParams.MaterialAndTextureBakeMode != EBakeMode::Intermediate)
		DuplicatedMaterial->SetFlags(RF_Public | RF_Standalone);

	// Add meta information.
	FHoudiniEngineBakeUtils::AddHoudiniMetaInformationToPackage(
		MaterialPackage, DuplicatedMaterial,
		HAPI_UNREAL_PACKAGE_META_GENERATED_OBJECT, TEXT("true"));
	FHoudiniEngineBakeUtils::AddHoudiniMetaInformationToPackage(
		MaterialPackage, DuplicatedMaterial,
		HAPI_UNREAL_PACKAGE_META_GENERATED_NAME, *MaterialName);

	// Retrieve and check various sampling expressions. If they contain textures, duplicate (and bake) them.

	for (auto& Expression : DuplicatedMaterial->Expressions)
	{
		FHoudiniEngineBakeUtils::ReplaceDuplicatedMaterialTextureSample(
			Expression, HoudiniCookParams);
	}

	// Notify registry that we have created a new duplicate material.
	FAssetRegistryModule::AssetCreated(DuplicatedMaterial);

	// Dirty the material package.
	DuplicatedMaterial->MarkPackageDirty();

	// Reset any derived state
	DuplicatedMaterial->ForceRecompileForRendering();
#endif
	return DuplicatedMaterial;
}

void
FHoudiniEngineBakeUtils::ReplaceDuplicatedMaterialTextureSample(
	UMaterialExpression * MaterialExpression, FHoudiniCookParams& HoudiniCookParams)
{
#if WITH_EDITOR
	UMaterialExpressionTextureSample * TextureSample = Cast< UMaterialExpressionTextureSample >(MaterialExpression);
	if (!TextureSample || TextureSample->IsPendingKill())
		return;

	UTexture2D * Texture = Cast< UTexture2D >(TextureSample->Texture);
	if (!Texture || Texture->IsPendingKill())
		return;

	UPackage * TexturePackage = Cast< UPackage >(Texture->GetOuter());
	if (!TexturePackage || TexturePackage->IsPendingKill())
		return;

	FString GeneratedTextureName;
	if (FHoudiniEngineBakeUtils::GetHoudiniGeneratedNameFromMetaInformation(
		TexturePackage, Texture, GeneratedTextureName))
	{
		// Duplicate texture.
		UTexture2D * DuplicatedTexture = FHoudiniEngineBakeUtils::DuplicateTextureAndCreatePackage(
			Texture, HoudiniCookParams, GeneratedTextureName);

		// Re-assign generated texture.
		TextureSample->Texture = DuplicatedTexture;
	}
#endif
}

UTexture2D *
FHoudiniEngineBakeUtils::DuplicateTextureAndCreatePackage(
	UTexture2D * Texture, FHoudiniCookParams& HoudiniCookParams, const FString & SubTextureName)
{
	UTexture2D* DuplicatedTexture = nullptr;
#if WITH_EDITOR
	// Retrieve original package of this texture.
	UPackage * TexturePackage = Cast< UPackage >(Texture->GetOuter());
	if (!TexturePackage || TexturePackage->IsPendingKill())
		return nullptr;

	FString GeneratedTextureName;
	if (FHoudiniEngineBakeUtils::GetHoudiniGeneratedNameFromMetaInformation(TexturePackage, Texture, GeneratedTextureName))
	{
		UMetaData * MetaData = TexturePackage->GetMetaData();
		if (!MetaData || MetaData->IsPendingKill())
			return nullptr;

		// Retrieve texture type.
		const FString & TextureType =
			MetaData->GetValue(Texture, HAPI_UNREAL_PACKAGE_META_GENERATED_TEXTURE_TYPE);

		// Create texture package.
		FString TextureName;
		UPackage * NewTexturePackage = FHoudiniEngineBakeUtils::BakeCreateTextureOrMaterialPackageForComponent(
			HoudiniCookParams, SubTextureName, TextureName);

		if (!NewTexturePackage || NewTexturePackage->IsPendingKill())
			return nullptr;

		// Clone texture.
		DuplicatedTexture = DuplicateObject< UTexture2D >(Texture, NewTexturePackage, *TextureName);
		if (!DuplicatedTexture || DuplicatedTexture->IsPendingKill())
			return nullptr;

		if (HoudiniCookParams.MaterialAndTextureBakeMode != EBakeMode::Intermediate)
			DuplicatedTexture->SetFlags(RF_Public | RF_Standalone);

		// Add meta information.
		FHoudiniEngineBakeUtils::AddHoudiniMetaInformationToPackage(
			NewTexturePackage, DuplicatedTexture,
			HAPI_UNREAL_PACKAGE_META_GENERATED_OBJECT, TEXT("true"));
		FHoudiniEngineBakeUtils::AddHoudiniMetaInformationToPackage(
			NewTexturePackage, DuplicatedTexture,
			HAPI_UNREAL_PACKAGE_META_GENERATED_NAME, *TextureName);
		FHoudiniEngineBakeUtils::AddHoudiniMetaInformationToPackage(
			NewTexturePackage, DuplicatedTexture,
			HAPI_UNREAL_PACKAGE_META_GENERATED_TEXTURE_TYPE, *TextureType);

		// Notify registry that we have created a new duplicate texture.
		FAssetRegistryModule::AssetCreated(DuplicatedTexture);

		// Dirty the texture package.
		DuplicatedTexture->MarkPackageDirty();
	}
#endif
	return DuplicatedTexture;
}

FHoudiniCookParams::FHoudiniCookParams(UHoudiniAssetComponent* HoudiniAssetComponent)
{
#if WITH_EDITOR
	HoudiniAsset = HoudiniAssetComponent->GetHoudiniAsset();
	HoudiniCookManager = HoudiniAssetComponent;
	PackageGUID = HoudiniAssetComponent->GetComponentGuid();
	BakedStaticMeshPackagesForParts = &HoudiniAssetComponent->BakedStaticMeshPackagesForParts;
	BakedMaterialPackagesForIds = &HoudiniAssetComponent->BakedMaterialPackagesForIds;
	CookedTemporaryStaticMeshPackages = &HoudiniAssetComponent->CookedTemporaryStaticMeshPackages;
	CookedTemporaryPackages = &HoudiniAssetComponent->CookedTemporaryPackages;
	CookedTemporaryLandscapeLayers = &HoudiniAssetComponent->CookedTemporaryLandscapeLayers;
	TempCookFolder = HoudiniAssetComponent->GetTempCookFolder();
	BakeFolder = HoudiniAssetComponent->GetBakeFolder();
	BakeNameOverrides = &HoudiniAssetComponent->BakeNameOverrides;
	IntermediateOuter = HoudiniAssetComponent->GetComponentLevel();
	GeneratedDistanceFieldResolutionScale = HoudiniAssetComponent->GeneratedDistanceFieldResolutionScale;
#endif
}

bool
FHoudiniEngineBakeUtils::BakeLandscape(UHoudiniAssetComponent* HoudiniAssetComponent, ALandscapeProxy * OnlyBakeThisLandscape)
{
#if WITH_EDITOR
	if (!HoudiniAssetComponent || HoudiniAssetComponent->IsPendingKill())
		return false;

	if (!HoudiniAssetComponent->HasLandscape())
		return false;

	TMap< FHoudiniGeoPartObject, TWeakObjectPtr<ALandscapeProxy>> * LandscapeComponentsPtr = HoudiniAssetComponent->GetLandscapeComponents();
	if (!LandscapeComponentsPtr)
		return false;

	TArray<UPackage *> LayerPackages;
	bool bNeedToUpdateProperties = false;
	for (TMap< FHoudiniGeoPartObject, TWeakObjectPtr<ALandscapeProxy> >::TIterator Iter(*LandscapeComponentsPtr); Iter; ++Iter)
	{
		ALandscapeProxy * CurrentLandscape = Iter.Value().Get();
		if (!CurrentLandscape || CurrentLandscape->IsPendingKill() || !CurrentLandscape->IsValidLowLevel())
			continue;

		// If we only want to bake a single landscape
		if (OnlyBakeThisLandscape && CurrentLandscape != OnlyBakeThisLandscape)
			continue;

		// Simply remove the landscape from the map
		FHoudiniGeoPartObject & HoudiniGeoPartObject = Iter.Key();
		LandscapeComponentsPtr->Remove(HoudiniGeoPartObject);

		CurrentLandscape->DetachFromActor(FDetachmentTransformRules::KeepWorldTransform);

		// And save its layers to prevent them from being removed
		for (TMap< TWeakObjectPtr< UPackage >, FHoudiniGeoPartObject > ::TIterator IterPackage(HoudiniAssetComponent->CookedTemporaryLandscapeLayers); IterPackage; ++IterPackage)
		{
			if (!(HoudiniGeoPartObject == IterPackage.Value()))
				continue;

			UPackage * Package = IterPackage.Key().Get();
			if (Package && !Package->IsPendingKill())
				LayerPackages.Add(Package);
		}

		bNeedToUpdateProperties = true;

		// If we only wanted to bake a single landscape, we're done
		if (OnlyBakeThisLandscape)
			break;
	}

	if (LayerPackages.Num() > 0)
	{
		// Save the layer info's package
		FEditorFileUtils::PromptForCheckoutAndSave(LayerPackages, true, false);

		// Remove the packages from the asset component, or the asset component might 
		// destroy them when it is being destroyed
		for (int32 n = 0; n < LayerPackages.Num(); n++)
			HoudiniAssetComponent->CookedTemporaryLandscapeLayers.Remove(LayerPackages[n]);
	}

	return bNeedToUpdateProperties;
#else
	return false;
#endif
}

bool FHoudiniEngineBakeUtils::TiledBakeLandScape(UHoudiniAssetComponent* HoudiniAssetComponent, class ALandscapeProxy* OnlyBakeThisLandscape) {
#if WITH_EDITOR


	//ULandscapeInfo* LandscapeInfo = OnlyBakeThisLandscape->GetLandscapeInfo();
	//check(LandscapeInfo);
	//ALandscape* Landscape = LandscapeInfo->LandscapeActor.Get();
	//check(Landscape);

	//Landscape->Modify();
	//LandscapeInfo->Modify();


	//LandscapeInfo->GetComponentsInRegion(X1 + 1, Y1 + 1, X2 - 1, Y2 - 1, SelectedComponents);


	//UWorld* World = getviewport->GetScene()->GetWorld();

	//TSet<ALandscapeProxy*> SelectProxies;
	//TSet<ULandscapeComponent*> TargetSelectedComponents;
	//TArray<ULandscapeHeightfieldCollisionComponent*> TargetSelectedCollisionComponents;
	//for (ULandscapeComponent* Component : SelectedComponents)
	//{
	//	SelectProxies.Add(Component->GetLandscapeProxy());
	//	if (Component->GetLandscapeProxy()->GetOuter() != World->GetCurrentLevel())
	//	{
	//		TargetSelectedComponents.Add(Component);
	//	}

	//	ULandscapeHeightfieldCollisionComponent* CollisionComp = Component->CollisionComponent.Get();
	//	SelectProxies.Add(CollisionComp->GetLandscapeProxy());
	//	if (CollisionComp->GetLandscapeProxy()->GetOuter() != World->GetCurrentLevel())
	//	{
	//		TargetSelectedCollisionComponents.Add(CollisionComp);
	//	}
	//}

	return true;
#else
	return false;
#endif
}

FName FHoudiniEngineBakeUtils::CreateTiledLevel(const FString& SavePath)
{
	UWorld* NewGWorld = UWorld::CreateWorld(EWorldType::None, false);
	ULevel* CurLevel = nullptr;
	check(NewGWorld);

	// Save new empty level
	bool bSaved = FEditorFileUtils::SaveLevel(NewGWorld->PersistentLevel, SavePath);

	//手动加载模块
	FWorldBrowserModule& WorldBrowserModule = FModuleManager::LoadModuleChecked<FWorldBrowserModule>(FName("WorldBrowser"));
	UWorld* EditorWorld = GEditor->GetEditorWorldContext().World();
	TSharedPtr<FWorldTileCollectionModel> WorlTiledModel = StaticCastSharedPtr<FWorldTileCollectionModel>(WorldBrowserModule.SharedWorldModel(EditorWorld));

	FName LevelPackageName = NewGWorld->GetOutermost()->GetFName();


	// Update levels list
	if (bSaved) {
		WorlTiledModel->PopulateLevelsList();
	}

	// Destroy the new world we created and collect the garbage
	NewGWorld->DestroyWorld(false);
	CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);

	return LevelPackageName;
}


ALandscapeProxy* FHoudiniEngineBakeUtils::MoveLandscapeToNewLevel(const FName& PackageName,class ALandscapeProxy* LandscapeProx, uint32 StartXIndex, uint32 EndXIndex, uint32 StartYIndex, uint32 EndYIndex) {

	//重复加载模块的原因是因为不想让HoudiniEditor模块加载WorldBrowser模块
	FWorldBrowserModule& WorldBrowserModule = FModuleManager::LoadModuleChecked<FWorldBrowserModule>(FName("WorldBrowser"));
	UWorld* EditorWorld = GEditor->GetEditorWorldContext().World();
	TSharedPtr<FWorldTileCollectionModel> WorlTiledModel = StaticCastSharedPtr<FWorldTileCollectionModel>(WorldBrowserModule.SharedWorldModel(EditorWorld));
	TSharedPtr<FWorldTileModel> NewTileModel = StaticCastSharedPtr<FWorldTileModel>(WorlTiledModel->FindLevelModel(PackageName));
	NewTileModel->MakeLevelCurrent();

	NewTileModel->SetVisible(false);

	UWorld* OriginWorld = LandscapeProx->GetWorld();

	ULandscapeInfo* LandScapeInfo = LandscapeProx->GetLandscapeInfo();
	check(LandScapeInfo);

	ALandscape* Landscape = LandScapeInfo ? LandScapeInfo->LandscapeActor.Get() : nullptr;
	check(Landscape);

	Landscape->Modify();
	LandScapeInfo->Modify();


	//收集
	TSet<ULandscapeComponent*>  ToBeMoveComponents;
	TSet<ALandscapeProxy*> ToBeMoveLandProxies;
	TArray<ULandscapeHeightfieldCollisionComponent*> TobeMoveCollisionComponents;
	for (uint32 ComponentIndexY = StartYIndex; ComponentIndexY <= EndYIndex; ComponentIndexY++)
	{
		for (uint32 ComponentIndexX = StartXIndex; ComponentIndexX <= EndXIndex; ComponentIndexX++)
		{
			ULandscapeComponent* Component = LandScapeInfo->XYtoComponentMap.FindRef(FIntPoint(ComponentIndexX, ComponentIndexY));
			ULandscapeHeightfieldCollisionComponent* CollisionComponent = Component->CollisionComponent.Get();
			if (Component && !FLevelUtils::IsLevelLocked(Component->GetLandscapeProxy()->GetLevel()) && FLevelUtils::IsLevelVisible(Component->GetLandscapeProxy()->GetLevel()))
			{
				ToBeMoveLandProxies.Emplace(Component->GetLandscapeProxy());
				ToBeMoveComponents.Emplace(Component);
				TobeMoveCollisionComponents.Emplace(CollisionComponent);
			}
		}
	}

	const int32 ComponentSizeVerts = Landscape->NumSubsections * (Landscape->SubsectionSizeQuads + 1);
	//直接用ComponentSizeVerts就行了，因为Landscape->NumSubsections * Landscape->SubsectionSizeQuads + 1必定是2的幂
	const int32 NeedHeightMapSize = 1 << FMath::CeilLogTwo(ComponentSizeVerts);

	TSet<UTexture2D*> OldHeightmapTextures;
	for (auto Component : ToBeMoveComponents) {
		Component->Modify();
		OldHeightmapTextures.Emplace(Component->GetHeightmap());
	}

	//更新被裁掉的高度图
	//存在情况，Neighbor是同一张HeightiMap但是不在ToBeMoveComponents容器中？ 存在
	TMap<ULandscapeComponent*, bool> HeightmapUpdateComponents;
	HeightmapUpdateComponents.Reserve(ToBeMoveComponents.Num() * 4); //对于每一个需要移动的Component，最多更新所有相邻的HeightMap,即上下左右4个
	for (auto Component : ToBeMoveComponents) {
		int HeightMapX = Component->GetHeightmap()->Source.GetSizeX();
		const int32 SearchX = HeightMapX / NeedHeightMapSize - 1;
		int HeightMapY = Component->GetHeightmap()->Source.GetSizeY();
		const int32 SearchY = HeightMapY / NeedHeightMapSize - 1;
		const FIntPoint ComponentBase = Component->GetSectionBase() / Component->ComponentSizeQuads;
		for (int32 Y = -SearchY; Y <= SearchY; ++Y) {
			for (int32 X = -SearchX; X <= SearchX; ++X) {
				ULandscapeComponent* Neighbor = LandScapeInfo->XYtoComponentMap.FindRef(ComponentBase + FIntPoint(X, Y));
				if (Neighbor && Neighbor->GetHeightmap() == Component->GetHeightmap() && !HeightmapUpdateComponents.Contains(Neighbor)) {
					Neighbor->Modify();
					//当Neighbor不存在于ToBeMoveComponents中时，SplitHeightmap第二个参数为nullptr，代表不会复制到指定的NewLandscapeProx中
					bool bNeedsMoveToCurrentLevel = ToBeMoveComponents.Contains(Neighbor);
					HeightmapUpdateComponents.Emplace(Neighbor, bNeedsMoveToCurrentLevel);
				}
			}
		}
	}

	//在新关卡创建ALandscapeStreamingProxy，注意不同于原始代码World是本身，新world需要设置transform？
	//要使用原world必须保证主关卡已经识别新建关卡，并且CurrentLevel已经是要移动的关卡
	ALandscapeProxy* NewLandscapeProx = OriginWorld->SpawnActor<ALandscapeStreamingProxy>();
	//拷贝共享属性
	NewLandscapeProx->GetSharedProperties(Landscape);
	NewLandscapeProx->CreateLandscapeInfo();

	//默认使用第一个Component的transform
	ULandscapeComponent* FirstComponent = *ToBeMoveComponents.CreateConstIterator();
	auto Position = FirstComponent->GetComponentLocation();
	auto Rotation = FirstComponent->GetComponentRotation();
	NewLandscapeProx->GetRootComponent()->SetWorldLocationAndRotation(Position, Rotation);
	NewLandscapeProx->LandscapeSectionOffset = FirstComponent->GetSectionBase();

	if (!NewLandscapeProx->GetLevel()->bIsVisible) {
		NewLandscapeProx->UnregisterAllComponents();
	}

	for (const auto& HeightmapUpdateComponentPair : HeightmapUpdateComponents) {
		SplitHeightmap(HeightmapUpdateComponentPair.Key, HeightmapUpdateComponentPair.Value ? NewLandscapeProx : nullptr);
	}

	//重新创建Texture后，更新原来无效引用的贴图
	for (UTexture2D* Texture : OldHeightmapTextures) {
		Texture->SetFlags(RF_Transactional);
		Texture->Modify();
		Texture->MarkPackageDirty();
		Texture->ClearFlags(RF_Standalone); // Keep object around for editing even if unreferenced.
	}

	for (ALandscapeProxy* Proxy : ToBeMoveLandProxies) {
		Proxy->Modify();
	}

	NewLandscapeProx->Modify();
	NewLandscapeProx->MarkPackageDirty();

	// Handle XY-offset textures (these don't need splitting, as they aren't currently shared between components like heightmaps/weightmaps can be)
	for (ULandscapeComponent* Component : ToBeMoveComponents)
	{
		if (Component->XYOffsetmapTexture) {
			Component->XYOffsetmapTexture->Modify();
			Component->XYOffsetmapTexture->Rename(nullptr, NewLandscapeProx);
		}
	}

	// Change Weight maps...
	{
		FLandscapeEditDataInterface LandscapeEdit(LandScapeInfo);
		for (ULandscapeComponent* Component : ToBeMoveComponents) {
			Component->ReallocateWeightmaps(&LandscapeEdit, false, true, false, true, NewLandscapeProx);
			Component->ForEachLayer([&](const FGuid& LayerGuid, FLandscapeLayerComponentData& LayerData)
			{
				FScopedSetLandscapeEditingLayer Scope(Landscape, LayerGuid);
				Component->ReallocateWeightmaps(&LandscapeEdit, true, true, false, true, NewLandscapeProx);
			});
			Landscape->RequestLayersContentUpdateForceAll();
		}

		// Need to Repacking all the Weight map (to make it packed well...)
		for (ALandscapeProxy* Proxy : ToBeMoveLandProxies)
		{
			Proxy->RemoveInvalidWeightmaps();
		}
	}


	// Move the components to the Proxy actor
	// This does not use the MoveSelectedActorsToCurrentLevel path as there is no support to only move certain components.
	for (ULandscapeComponent* Component : ToBeMoveComponents)
	{
		// Need to move or recreate all related data (Height map, Weight map, maybe collision components, allocation info)
		Component->GetLandscapeProxy()->LandscapeComponents.Remove(Component);
		Component->UnregisterComponent();
		Component->DetachFromComponent(FDetachmentTransformRules::KeepWorldTransform);
		Component->InvalidateLightingCache();
		Component->Rename(nullptr, NewLandscapeProx);
		NewLandscapeProx->LandscapeComponents.Add(Component);
		Component->AttachToComponent(NewLandscapeProx->GetRootComponent(), FAttachmentTransformRules::KeepWorldTransform);

		// clear transient mobile data
		Component->MobileDataSourceHash.Invalidate();
		Component->MobileMaterialInterfaces.Reset();
		Component->MobileWeightmapTextures.Reset();

		Component->UpdateMaterialInstances();

		FFormatNamedArguments Args;
		Args.Add(TEXT("ComponentName"), FText::FromString(Component->GetName()));
	}
	NewLandscapeProx->UpdateCachedHasLayersContent();

	for (ULandscapeHeightfieldCollisionComponent* Component : TobeMoveCollisionComponents) {
		// Need to move or recreate all related data (Height map, Weight map, maybe collision components, allocation info)
		Component->GetLandscapeProxy()->CollisionComponents.Remove(Component);
		Component->UnregisterComponent();
		Component->DetachFromComponent(FDetachmentTransformRules::KeepWorldTransform);
		Component->Rename(nullptr, NewLandscapeProx);
		NewLandscapeProx->CollisionComponents.Add(Component);
		Component->AttachToComponent(NewLandscapeProx->GetRootComponent(), FAttachmentTransformRules::KeepWorldTransform);

		// Move any foliage associated
		//注意植被移动的关卡需要修改，已经改为一个World下的Level不用动
		AInstancedFoliageActor::MoveInstancesForComponentToCurrentLevel(Component);

		FFormatNamedArguments Args;
		Args.Add(TEXT("ComponentName"), FText::FromString(Component->GetName()));
	}
	GEditor->SelectNone(false, true);
	GEditor->SelectActor(NewLandscapeProx, true, false, true);

	GEditor->SelectNone(false, true);
	// Register our new components if destination landscape is registered in scene 
	if (NewLandscapeProx->GetRootComponent()->IsRegistered())
	{
		NewLandscapeProx->RegisterAllComponents();
	}

	for (ALandscapeProxy* Proxy : ToBeMoveLandProxies)
	{
		if (Proxy->GetRootComponent()->IsRegistered())
		{
			Proxy->RegisterAllComponents();
		}
	}


	FString SvaePath = FPackageName::LongPackageNameToFilename(PackageName.ToString()) + FPackageName::GetMapPackageExtension();

	FEditorFileUtils::SaveLevel(NewTileModel->GetLevelObject(), SvaePath);

	TSharedPtr<FWorldTileModel> PersistenLevelTileModel = StaticCastSharedPtr<FWorldTileModel>(WorlTiledModel->FindLevelModel(OriginWorld->PersistentLevel));

	PersistenLevelTileModel->MakeLevelCurrent();

	return NewLandscapeProx;
}



ENGINE_API extern bool GDisableAutomaticTextureMaterialUpdateDependencies;
void FHoudiniEngineBakeUtils::SplitHeightmap(
	class ULandscapeComponent* Comp,
	class ALandscapeProxy* TargetProxy,
	class FMaterialUpdateContext* InOutUpdateContext,
	TArray<class FComponentRecreateRenderStateContext>* InOutRecreateRenderStateContext,
	bool InReregisterComponent)
{
	ULandscapeInfo* Info = Comp->GetLandscapeInfo();

	// Make sure the heightmap UVs are powers of two.
	int32 ComponentSizeVerts = Comp->NumSubsections * (Comp->SubsectionSizeQuads + 1);
	int32 HeightmapSizeU = (1 << FMath::CeilLogTwo(ComponentSizeVerts));
	int32 HeightmapSizeV = (1 << FMath::CeilLogTwo(ComponentSizeVerts));

	ALandscapeProxy* SrcProxy = Comp->GetLandscapeProxy();
	ALandscapeProxy* DstProxy = TargetProxy ? TargetProxy : SrcProxy;
	SrcProxy->Modify();
	DstProxy->Modify();

	UTexture2D* OldHeightmapTexture = Comp->GetHeightmap(false);
	UTexture2D* NewHeightmapTexture = NULL;
	FVector4 OldHeightmapScaleBias = Comp->HeightmapScaleBias;
	FVector4 NewHeightmapScaleBias = FVector4(1.0f / (float)HeightmapSizeU, 1.0f / (float)HeightmapSizeV, 0.0f, 0.0f);

	{
		// Read old data and split
		FLandscapeEditDataInterface LandscapeEdit(Info);
		TArray<uint8> HeightData;
		HeightData.AddZeroed((1 + Comp->ComponentSizeQuads)*(1 + Comp->ComponentSizeQuads) * sizeof(uint16));
		// Because of edge problem, normal would be just copy from old component data
		TArray<uint8> NormalData;
		NormalData.AddZeroed((1 + Comp->ComponentSizeQuads)*(1 + Comp->ComponentSizeQuads) * sizeof(uint16));
		LandscapeEdit.GetHeightDataFast(Comp->GetSectionBase().X, Comp->GetSectionBase().Y, Comp->GetSectionBase().X + Comp->ComponentSizeQuads, Comp->GetSectionBase().Y + Comp->ComponentSizeQuads, (uint16*)HeightData.GetData(), 0, (uint16*)NormalData.GetData());

		// Create the new heightmap texture
		NewHeightmapTexture = DstProxy->CreateLandscapeTexture(HeightmapSizeU, HeightmapSizeV, TEXTUREGROUP_Terrain_Heightmap, TSF_BGRA8);
		ULandscapeComponent::CreateEmptyTextureMips(NewHeightmapTexture, true);
		Comp->HeightmapScaleBias = NewHeightmapScaleBias;
		Comp->SetHeightmap(NewHeightmapTexture);

		check(Comp->GetHeightmap(false) == Comp->GetHeightmap(true));
		LandscapeEdit.SetHeightData(Comp->GetSectionBase().X, Comp->GetSectionBase().Y, Comp->GetSectionBase().X + Comp->ComponentSizeQuads, Comp->GetSectionBase().Y + Comp->ComponentSizeQuads, (uint16*)HeightData.GetData(), 0, false, (uint16*)NormalData.GetData());
	}

	// End material update
	if (InOutUpdateContext != nullptr && InOutRecreateRenderStateContext != nullptr)
	{
		Comp->UpdateMaterialInstances(*InOutUpdateContext, *InOutRecreateRenderStateContext);
	}
	else
	{
		Comp->UpdateMaterialInstances();
	}

	// We disable automatic material update context, to manage it manually if we have a custom update context specified
	GDisableAutomaticTextureMaterialUpdateDependencies = (InOutUpdateContext != nullptr);

	NewHeightmapTexture->PostEditChange();

	if (InOutUpdateContext != nullptr)
	{
		// Build a list of all unique materials the landscape uses
		TArray<UMaterialInterface*> LandscapeMaterials;

		int8 MaxLOD = FMath::CeilLogTwo(Comp->SubsectionSizeQuads + 1) - 1;

		for (int8 LODIndex = 0; LODIndex < MaxLOD; ++LODIndex)
		{
			UMaterialInterface* Material = Comp->GetLandscapeMaterial(LODIndex);
			LandscapeMaterials.AddUnique(Material);
		}

		TSet<UMaterial*> BaseMaterialsThatUseThisTexture;

		for (UMaterialInterface* MaterialInterface : LandscapeMaterials)
		{
			if (DoesMaterialUseTexture(MaterialInterface, NewHeightmapTexture))
			{
				UMaterial* Material = MaterialInterface->GetMaterial();
				bool MaterialAlreadyCompute = false;
				BaseMaterialsThatUseThisTexture.Add(Material, &MaterialAlreadyCompute);

				if (!MaterialAlreadyCompute)
				{
					if (Material->IsTextureForceRecompileCacheRessource(NewHeightmapTexture))
					{
						InOutUpdateContext->AddMaterial(Material);
						Material->UpdateMaterialShaderCacheAndTextureReferences();
					}
				}
			}
		}
	}

	GDisableAutomaticTextureMaterialUpdateDependencies = false;

#if WITH_EDITORONLY_DATA
	check(Comp->GetLandscapeProxy()->HasLayersContent() == DstProxy->CanHaveLayersContent());
	if (Comp->GetLandscapeProxy()->HasLayersContent() && DstProxy->CanHaveLayersContent())
	{
		FLandscapeLayersTexture2DCPUReadBackResource* NewCPUReadBackResource = new FLandscapeLayersTexture2DCPUReadBackResource(NewHeightmapTexture->Source.GetSizeX(), NewHeightmapTexture->Source.GetSizeY(), NewHeightmapTexture->GetPixelFormat(), NewHeightmapTexture->Source.GetNumMips());
		BeginInitResource(NewCPUReadBackResource);
		DstProxy->HeightmapsCPUReadBack.Add(NewHeightmapTexture, NewCPUReadBackResource);

		// Free OldHeightmapTexture's CPUReadBackResource if not used by any component
		bool FreeCPUReadBack = true;
		for (ULandscapeComponent* Component : SrcProxy->LandscapeComponents)
		{
			if (Component != Comp && Component->GetHeightmap(false) == OldHeightmapTexture)
			{
				FreeCPUReadBack = false;
				break;
			}
		}
		if (FreeCPUReadBack)
		{
			FLandscapeLayersTexture2DCPUReadBackResource** OldCPUReadBackResource = SrcProxy->HeightmapsCPUReadBack.Find(OldHeightmapTexture);
			if (OldCPUReadBackResource)
			{
				if (FLandscapeLayersTexture2DCPUReadBackResource* ResourceToDelete = *OldCPUReadBackResource)
				{
					ReleaseResourceAndFlush(ResourceToDelete);
					delete ResourceToDelete;
					SrcProxy->HeightmapsCPUReadBack.Remove(OldHeightmapTexture);
				}
			}
		}

		// Move layer content to new layer heightmap
		FLandscapeEditDataInterface LandscapeEdit(Info);
		ALandscape* Landscape = Info->LandscapeActor.Get();
		Comp->ForEachLayer([&](const FGuid& LayerGuid, FLandscapeLayerComponentData& LayerData)
		{
			UTexture2D* OldLayerHeightmap = LayerData.HeightmapData.Texture;
			if (OldLayerHeightmap != nullptr)
			{
				FScopedSetLandscapeEditingLayer Scope(Landscape, LayerGuid);
				// Read old data and split
				TArray<uint8> LayerHeightData;
				LayerHeightData.AddZeroed((1 + Comp->ComponentSizeQuads)*(1 + Comp->ComponentSizeQuads) * sizeof(uint16));
				// Because of edge problem, normal would be just copy from old component data
				TArray<uint8> LayerNormalData;
				LayerNormalData.AddZeroed((1 + Comp->ComponentSizeQuads)*(1 + Comp->ComponentSizeQuads) * sizeof(uint16));

				// Read using old heightmap scale/bias
				Comp->HeightmapScaleBias = OldHeightmapScaleBias;
				LandscapeEdit.GetHeightDataFast(Comp->GetSectionBase().X, Comp->GetSectionBase().Y, Comp->GetSectionBase().X + Comp->ComponentSizeQuads, Comp->GetSectionBase().Y + Comp->ComponentSizeQuads, (uint16*)LayerHeightData.GetData(), 0, (uint16*)LayerNormalData.GetData());
				// Restore new heightmap scale/bias
				Comp->HeightmapScaleBias = NewHeightmapScaleBias;
				{
					UTexture2D* LayerHeightmapTexture = DstProxy->CreateLandscapeTexture(HeightmapSizeU, HeightmapSizeV, TEXTUREGROUP_Terrain_Heightmap, TSF_BGRA8);
					ULandscapeComponent::CreateEmptyTextureMips(LayerHeightmapTexture, true);
					LayerHeightmapTexture->PostEditChange();
					// Set Layer heightmap texture
					LayerData.HeightmapData.Texture = LayerHeightmapTexture;
					LandscapeEdit.SetHeightData(Comp->GetSectionBase().X, Comp->GetSectionBase().Y, Comp->GetSectionBase().X + Comp->ComponentSizeQuads, Comp->GetSectionBase().Y + Comp->ComponentSizeQuads, (uint16*)LayerHeightData.GetData(), 0, false, (uint16*)LayerNormalData.GetData());
				}
			}
		});

		Landscape->RequestLayersContentUpdateForceAll();
	}
#endif

	// Reregister
	if (InReregisterComponent)
	{
		FComponentReregisterContext ReregisterContext(Comp);
	}
}

UPackage *
FHoudiniEngineBakeUtils::BakeCreateMaterialPackageForComponent(
	FHoudiniCookParams& HoudiniCookParams,
	const HAPI_MaterialInfo & MaterialInfo, FString & MaterialName)
{
	UHoudiniAsset * HoudiniAsset = HoudiniCookParams.HoudiniAsset;
	if (!HoudiniAsset || HoudiniAsset->IsPendingKill())
		return nullptr;

	FString MaterialDescriptor;
	if (HoudiniCookParams.MaterialAndTextureBakeMode != EBakeMode::Intermediate)
		MaterialDescriptor = HoudiniAsset->GetName() + TEXT("_material_") + FString::FromInt(MaterialInfo.nodeId) + TEXT("_");
	else
		MaterialDescriptor = HoudiniAsset->GetName() + TEXT("_") + FString::FromInt(MaterialInfo.nodeId) + TEXT("_");

	return FHoudiniEngineBakeUtils::BakeCreateTextureOrMaterialPackageForComponent(
		HoudiniCookParams, MaterialDescriptor, MaterialName);
}

UPackage *
FHoudiniEngineBakeUtils::BakeCreateTexturePackageForComponent(
	FHoudiniCookParams& HoudiniCookParams,
	const HAPI_MaterialInfo & MaterialInfo, const FString & TextureType,
	FString & TextureName)
{
	UHoudiniAsset * HoudiniAsset = HoudiniCookParams.HoudiniAsset;
	if (!HoudiniAsset || HoudiniAsset->IsPendingKill())
		return nullptr;

	FString TextureInfoDescriptor;
	if (HoudiniCookParams.MaterialAndTextureBakeMode != EBakeMode::Intermediate)
	{
		TextureInfoDescriptor = HoudiniAsset->GetName() + TEXT("_texture_") + FString::FromInt(MaterialInfo.nodeId) +
			TEXT("_") + TextureType + TEXT("_");
	}
	else
	{
		TextureInfoDescriptor = HoudiniAsset->GetName() + TEXT("_") + FString::FromInt(MaterialInfo.nodeId) + TEXT("_") +
			TextureType + TEXT("_");
	}

	return FHoudiniEngineBakeUtils::BakeCreateTextureOrMaterialPackageForComponent(
		HoudiniCookParams, TextureInfoDescriptor, TextureName);
}

UPackage *
FHoudiniEngineBakeUtils::BakeCreateTextureOrMaterialPackageForComponent(
	FHoudiniCookParams& HoudiniCookParams,
	const FString & MaterialInfoDescriptor,
	FString & MaterialName)
{
	UPackage * PackageNew = nullptr;

#if WITH_EDITOR
	EBakeMode BakeMode = HoudiniCookParams.MaterialAndTextureBakeMode;
	UHoudiniAsset * HoudiniAsset = HoudiniCookParams.HoudiniAsset;
	FGuid BakeGUID;
	FString PackageName;

	const FGuid & ComponentGUID = HoudiniCookParams.PackageGUID;
	FString ComponentGUIDString = ComponentGUID.ToString().Left(FHoudiniEngineUtils::PackageGUIDComponentNameLength);

	if ((BakeMode == EBakeMode::ReplaceExisitingAssets) || (BakeMode == EBakeMode::CookToTemp))
	{
		bool bRemovePackageFromCache = false;

		UPackage* FoundPackage = nullptr;
		if (BakeMode == EBakeMode::ReplaceExisitingAssets)
		{
			TWeakObjectPtr< UPackage > * FoundPointer = HoudiniCookParams.BakedMaterialPackagesForIds->Find(MaterialInfoDescriptor);
			if (FoundPointer)
			{
				if ((*FoundPointer).IsValid())
					FoundPackage = (*FoundPointer).Get();
			}
			else
			{
				bRemovePackageFromCache = true;
			}
		}
		else
		{
			TWeakObjectPtr< UPackage > * FoundPointer = HoudiniCookParams.CookedTemporaryPackages->Find(MaterialInfoDescriptor);
			if (FoundPointer)
			{
				if ((*FoundPointer).IsValid())
					FoundPackage = (*FoundPointer).Get();
			}
			else
			{
				bRemovePackageFromCache = true;
			}
		}

		// Find a previously baked / cooked asset
		if (FoundPackage && !FoundPackage->IsPendingKill())
		{
			if (UPackage::IsEmptyPackage(FoundPackage))
			{
				// This happens when the prior baked output gets renamed, we can delete this 
				// orphaned package so that we can re-use the name
				FoundPackage->ClearFlags(RF_Standalone);
				FoundPackage->ConditionalBeginDestroy();

				bRemovePackageFromCache = true;
			}
			else
			{
				if (CheckPackageSafeForBake(FoundPackage, MaterialName) && !MaterialName.IsEmpty())
				{
					return FoundPackage;
				}
				else
				{
					// Found the package but we can't update it.  We already issued an error, but should popup the standard reference error dialog
					//::ErrorPopup( TEXT( "Baking Failed: Could not overwrite %s, because it is being referenced" ), *(*FoundPackage)->GetPathName() );

					// If we're cooking, we'll create a new package, if baking, fail
					if (BakeMode != EBakeMode::CookToTemp)
						return nullptr;
				}
			}

			bRemovePackageFromCache = true;
		}

		if (bRemovePackageFromCache)
		{
			// Package is either invalid / not found so we need to remove it from the cache
			if (BakeMode == EBakeMode::ReplaceExisitingAssets)
				HoudiniCookParams.BakedMaterialPackagesForIds->Remove(MaterialInfoDescriptor);
			else
				HoudiniCookParams.CookedTemporaryPackages->Remove(MaterialInfoDescriptor);
		}
	}

	while (true)
	{
		if (!BakeGUID.IsValid())
			BakeGUID = FGuid::NewGuid();

		// We only want half of generated guid string.
		FString BakeGUIDString = BakeGUID.ToString().Left(FHoudiniEngineUtils::PackageGUIDItemNameLength);

		// Generate material name.
		MaterialName = MaterialInfoDescriptor;
		MaterialName += BakeGUIDString;

		switch (BakeMode)
		{
		case EBakeMode::Intermediate:
		{
			// Generate unique package name.
			PackageName = FPackageName::GetLongPackagePath(HoudiniAsset->GetOuter()->GetName()) +
				TEXT("/") +
				HoudiniAsset->GetName() +
				TEXT("_") +
				ComponentGUIDString +
				TEXT("/") +
				MaterialName;
		}
		break;

		case EBakeMode::CookToTemp:
		{
			PackageName = HoudiniCookParams.TempCookFolder.ToString() + TEXT("/") + MaterialName;
		}
		break;

		default:
		{
			// Generate unique package name.
			PackageName = HoudiniCookParams.BakeFolder.ToString() + TEXT("/") + MaterialName;
		}
		break;
		}

		// Sanitize package name.
		PackageName = UPackageTools::SanitizePackageName(PackageName);

		UObject * OuterPackage = nullptr;
		if (BakeMode == EBakeMode::Intermediate)
		{
			// If we are not baking, then use outermost package, since objects within our package need to be visible
			// to external operations, such as copy paste.
			OuterPackage = HoudiniCookParams.IntermediateOuter;
		}

		// See if package exists, if it does, we need to regenerate the name.
		UPackage * Package = FindPackage(OuterPackage, *PackageName);
		if (Package && !Package->IsPendingKill())
		{
			// Package does exist, there's a collision, we need to generate a new name.
			BakeGUID.Invalidate();
		}
		else
		{
			// Create actual package.
			PackageNew = CreatePackage(OuterPackage, *PackageName);
			PackageNew->SetPackageFlags(RF_Standalone);
			break;
		}
	}

	if (PackageNew && !PackageNew->IsPendingKill()
		&& ((BakeMode == EBakeMode::ReplaceExisitingAssets) || (BakeMode == EBakeMode::CookToTemp)))
	{
		// Add the new package to the cache
		if (BakeMode == EBakeMode::ReplaceExisitingAssets)
			HoudiniCookParams.BakedMaterialPackagesForIds->Add(MaterialInfoDescriptor, PackageNew);
		else
			HoudiniCookParams.CookedTemporaryPackages->Add(MaterialInfoDescriptor, PackageNew);
	}
#endif
	return PackageNew;
}

bool
FHoudiniEngineBakeUtils::CheckPackageSafeForBake(UPackage* Package, FString& FoundAssetName)
{
	if (!Package || Package->IsPendingKill())
		return false;

	FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
	TArray<FAssetData> AssetsInPackage;
	AssetRegistryModule.Get().GetAssetsByPackageName(Package->GetFName(), AssetsInPackage);
	for (const auto& AssetInfo : AssetsInPackage)
	{
		UObject* AssetInPackage = AssetInfo.GetAsset();
		if (!AssetInPackage || AssetInPackage->IsPendingKill())
			continue;

		// Check and see whether we are referenced by any objects that won't be garbage collected (*including* the undo buffer)
		FReferencerInformationList ReferencesIncludingUndo;
		bool bReferencedInMemoryOrUndoStack = IsReferenced(AssetInPackage, GARBAGE_COLLECTION_KEEPFLAGS, EInternalObjectFlags::GarbageCollectionKeepFlags, true, &ReferencesIncludingUndo);
		if (bReferencedInMemoryOrUndoStack)
		{
			// warn
			HOUDINI_LOG_ERROR(TEXT("Could not bake to %s because it is being referenced"), *Package->GetPathName());
			return false;
		}
		FoundAssetName = AssetInfo.AssetName.ToString();
	}

	return true;
}

UPackage *
FHoudiniEngineBakeUtils::BakeCreateStaticMeshPackageForComponent(
	FHoudiniCookParams& HoudiniCookParams,
	const FHoudiniGeoPartObject & HoudiniGeoPartObject,
	FString & MeshName, FGuid & BakeGUID)
{
	UPackage * PackageNew = nullptr;

#if WITH_EDITOR
	EBakeMode BakeMode = HoudiniCookParams.StaticMeshBakeMode;
	FString PackageName;
	int32 BakeCount = 0;
	const FGuid & ComponentGUID = HoudiniCookParams.PackageGUID;
	FString ComponentGUIDString = ComponentGUID.ToString().Left(
		FHoudiniEngineUtils::PackageGUIDComponentNameLength);

	while (true)
	{
		if ((BakeMode == EBakeMode::ReplaceExisitingAssets) || (BakeMode == EBakeMode::CookToTemp))
		{
			bool bRemovePackageFromCache = false;

			UPackage* FoundPackage = nullptr;
			if (BakeMode == EBakeMode::ReplaceExisitingAssets)
			{
				TWeakObjectPtr< UPackage > * FoundPointer = HoudiniCookParams.BakedStaticMeshPackagesForParts->Find(HoudiniGeoPartObject);
				if (FoundPointer)
				{
					if ((*FoundPointer).IsValid())
						FoundPackage = (*FoundPointer).Get();
				}
				else
				{
					bRemovePackageFromCache = true;
				}
			}
			else
			{
				TWeakObjectPtr< UPackage > * FoundPointer = HoudiniCookParams.CookedTemporaryStaticMeshPackages->Find(HoudiniGeoPartObject);
				if (FoundPointer)
				{
					if ((*FoundPointer).IsValid())
						FoundPackage = (*FoundPointer).Get();
				}
				else
				{
					bRemovePackageFromCache = true;
				}
			}

			// Find a previously baked / cooked asset
			if (FoundPackage && !FoundPackage->IsPendingKill())
			{
				if (UPackage::IsEmptyPackage(FoundPackage))
				{
					// This happens when the prior baked output gets renamed, we can delete this 
					// orphaned package so that we can re-use the name
					FoundPackage->ClearFlags(RF_Standalone);
					FoundPackage->ConditionalBeginDestroy();

					bRemovePackageFromCache = true;
				}
				else
				{
					if (FHoudiniEngineBakeUtils::CheckPackageSafeForBake(FoundPackage, MeshName) && !MeshName.IsEmpty())
					{
						return FoundPackage;
					}
					else
					{
						// Found the package but we can't update it.  We already issued an error, but should popup the standard reference error dialog
						//::ErrorPopup( TEXT( "Baking Failed: Could not overwrite %s, because it is being referenced" ), *(*FoundPackage)->GetPathName() );

						// If we're cooking, we'll create a new package, if baking, fail
						if (BakeMode != EBakeMode::CookToTemp)
							return nullptr;
					}
				}

				bRemovePackageFromCache = true;
			}

			if (bRemovePackageFromCache)
			{
				// Package is either invalid / not found so we need to remove it from the cache
				if (BakeMode == EBakeMode::ReplaceExisitingAssets)
					HoudiniCookParams.BakedStaticMeshPackagesForParts->Remove(HoudiniGeoPartObject);
				else
					HoudiniCookParams.CookedTemporaryStaticMeshPackages->Remove(HoudiniGeoPartObject);
			}
		}

		if (!BakeGUID.IsValid())
			BakeGUID = FGuid::NewGuid();

		MeshName = HoudiniCookParams.HoudiniCookManager->GetBakingBaseName(HoudiniGeoPartObject);

		if (BakeCount > 0)
		{
			MeshName += FString::Printf(TEXT("_%02d"), BakeCount);
		}

		switch (BakeMode)
		{
		case EBakeMode::Intermediate:
		{
			// We only want half of generated guid string.
			FString BakeGUIDString = BakeGUID.ToString().Left(FHoudiniEngineUtils::PackageGUIDItemNameLength);

			MeshName += TEXT("_") +
				FString::FromInt(HoudiniGeoPartObject.ObjectId) + TEXT("_") +
				FString::FromInt(HoudiniGeoPartObject.GeoId) + TEXT("_") +
				FString::FromInt(HoudiniGeoPartObject.PartId) + TEXT("_") +
				FString::FromInt(HoudiniGeoPartObject.SplitId) + TEXT("_") +
				HoudiniGeoPartObject.SplitName + TEXT("_") +
				BakeGUIDString;

			PackageName = FPackageName::GetLongPackagePath(HoudiniCookParams.HoudiniAsset->GetOuter()->GetName()) +
				TEXT("/") +
				HoudiniCookParams.HoudiniAsset->GetName() +
				TEXT("_") +
				ComponentGUIDString +
				TEXT("/") +
				MeshName;
		}
		break;

		case EBakeMode::CookToTemp:
		{
			PackageName = HoudiniCookParams.TempCookFolder.ToString() + TEXT("/") + MeshName;
		}
		break;

		default:
		{
			PackageName = HoudiniCookParams.BakeFolder.ToString() + TEXT("/") + MeshName;
		}
		break;
		}

		// Santize package name.
		PackageName = UPackageTools::SanitizePackageName(PackageName);

		UObject * OuterPackage = nullptr;

		if (BakeMode == EBakeMode::Intermediate)
		{
			// If we are not baking, then use outermost package, since objects within our package need to be visible
			// to external operations, such as copy paste.
			OuterPackage = HoudiniCookParams.IntermediateOuter;
		}

		// See if package exists, if it does, we need to regenerate the name.
		UPackage * Package = FindPackage(OuterPackage, *PackageName);

		if (Package && !Package->IsPendingKill())
		{
			if (BakeMode != EBakeMode::Intermediate)
			{
				// Increment bake counter
				BakeCount++;
			}
			else
			{
				// Package does exist, there's a collision, we need to generate a new name.
				BakeGUID.Invalidate();
			}
		}
		else
		{
			// Create actual package.
			PackageNew = CreatePackage(OuterPackage, *PackageName);
			break;
		}
	}

	if (PackageNew && !PackageNew->IsPendingKill()
		&& ((BakeMode == EBakeMode::ReplaceExisitingAssets) || (BakeMode == EBakeMode::CookToTemp)))
	{
		// Add the new package to the cache
		if (BakeMode == EBakeMode::ReplaceExisitingAssets)
			HoudiniCookParams.BakedStaticMeshPackagesForParts->Add(HoudiniGeoPartObject, PackageNew);
		else
			HoudiniCookParams.CookedTemporaryStaticMeshPackages->Add(HoudiniGeoPartObject, PackageNew);
	}
#endif
	return PackageNew;
}


void
FHoudiniEngineBakeUtils::AddHoudiniMetaInformationToPackage(
	UPackage * Package, UObject * Object, const TCHAR * Key,
	const TCHAR * Value)
{
	if (!Package || Package->IsPendingKill())
		return;

	UMetaData * MetaData = Package->GetMetaData();
	if (MetaData && !MetaData->IsPendingKill())
		MetaData->SetValue(Object, Key, Value);
}

bool
FHoudiniEngineBakeUtils::GetHoudiniGeneratedNameFromMetaInformation(
	UPackage * Package, UObject * Object, FString & HoudiniName)
{
	if (!Package || Package->IsPendingKill())
		return false;

	UMetaData * MetaData = Package->GetMetaData();
	if (!MetaData || MetaData->IsPendingKill())
		return false;

	if (MetaData->HasValue(Object, HAPI_UNREAL_PACKAGE_META_GENERATED_OBJECT))
	{
		// Retrieve name used for package generation.
		const FString NameFull = MetaData->GetValue(Object, HAPI_UNREAL_PACKAGE_META_GENERATED_NAME);
		HoudiniName = NameFull.Left(NameFull.Len() - FHoudiniEngineUtils::PackageGUIDItemNameLength);

		return true;
	}

	return false;
}

bool
FHoudiniEngineBakeUtils::DeleteBakedHoudiniAssetActor(UHoudiniAssetComponent * HoudiniAssetComponent)
{
	// Helper function used by the replace function to delete the Houdini Asset Actor after it's been baked
	if (!HoudiniAssetComponent || HoudiniAssetComponent->IsPendingKill())
		return false;

	bool bSuccess = false;
#if WITH_EDITOR
	// We can initiate Houdini actor deletion.
	AActor * ActorOwner = HoudiniAssetComponent->GetOwner();
	if (!ActorOwner || ActorOwner->IsPendingKill())
		return bSuccess;

	// Remove Houdini actor from active selection in editor and delete it.
	if (GEditor)
	{
		GEditor->SelectActor(ActorOwner, false, false);
		ULayersSubsystem* LayerSubSystem = GEditor->GetEditorSubsystem<ULayersSubsystem>();
		if (LayerSubSystem)
			LayerSubSystem->DisassociateActorFromLayers(ActorOwner);
	}

	UWorld * World = ActorOwner->GetWorld();
	if (!World)
		World = GWorld;

	bSuccess = World->EditorDestroyActor(ActorOwner, false);
#endif

	return bSuccess;
}