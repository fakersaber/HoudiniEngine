#include "STiledLandscapeBakeDlg.h"
#include "Widgets/Layout/SUniformGridPanel.h"
#include "SlateOptMacros.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "Widgets/Input/SVectorInputBox.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SNumericEntryBox.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"



#define LOCTEXT_NAMESPACE "YHoudini_Engine"

const int32 STiledLandscapeBakeDlg::TiledSize[] = { 2,4,8 };



void STiledLandscapeBakeDlg::Construct(const FArguments& InArgs, TSharedPtr<SWindow> InParentWindow, FIntPoint LandscapeComponentCount) {
	ParentWindow = InParentWindow;

	bShouldBakeTiled = false;

	//generate size of tile, don't non-2 powers
	for (auto tileSize : TiledSize) {
		if (tileSize > LandscapeComponentCount.X)
			break;
		TileOptions.Emplace(MakeShared<TiledStruct>(tileSize));
	}

	CurSelected = TileOptions[0];
	
	ChildSlot[
		SNew(SVerticalBox)
			+ SVerticalBox::Slot()
			.FillHeight(1)
			.Padding(0,10,0,10)[
				SNew(SUniformGridPanel)
				.SlotPadding(2)

				//Tile configuration
				+SUniformGridPanel::Slot(0, 0)
				.VAlign(VAlign_Center)[
					SNew(STextBlock)
					.Text(LOCTEXT("TiledLandscapeBake_ConfigurationText", "Tile Size"))
				]

				+ SUniformGridPanel::Slot(1, 0)
				.VAlign(VAlign_Center)[
					SAssignNew(TileComboBox, SComboBox<TSharedPtr<TiledStruct>>)
					.OptionsSource(&TileOptions)
					.OnSelectionChanged(this, &STiledLandscapeBakeDlg::OnTiledSelectionChanged)
					.OnGenerateWidget(this, &STiledLandscapeBakeDlg::HandleTileConfigurationComboBoxGenarateWidget)
					.Content()
					[
						SNew(STextBlock)
						.Text(this, &STiledLandscapeBakeDlg::GetTileConfigurationText)
					]
				]
			]

			// Import, Cancel
			+SVerticalBox::Slot()
			.AutoHeight()
			.HAlign(HAlign_Right)
			.VAlign(VAlign_Bottom)
			.Padding(0, 10, 0, 10)
			[
				SNew(SUniformGridPanel)
				.SlotPadding(FEditorStyle::GetMargin("StandardDialog.SlotPadding"))
				.MinDesiredSlotWidth(FEditorStyle::GetFloat("StandardDialog.MinDesiredSlotWidth"))
				.MinDesiredSlotHeight(FEditorStyle::GetFloat("StandardDialog.MinDesiredSlotHeight"))
				+ SUniformGridPanel::Slot(0, 0)
				[
					SNew(SButton)
					.HAlign(HAlign_Center)
					.ContentPadding(FEditorStyle::GetMargin("StandardDialog.ContentPadding"))
					//.IsEnabled(this, &STiledLandscapeBakeDlg::IsImportEnabled)
					.OnClicked(this, &STiledLandscapeBakeDlg::OnClickedOK)
					.Text(LOCTEXT("TiledLandscapeImport_ImportButtonText", "OK"))
				]
				+ SUniformGridPanel::Slot(1, 0)
				[
					SNew(SButton)
					.HAlign(HAlign_Center)
					.ContentPadding(FEditorStyle::GetMargin("StandardDialog.ContentPadding"))
					.OnClicked(this, &STiledLandscapeBakeDlg::OnClickedCancel)
					.Text(LOCTEXT("TiledLandscapeImport_CancelButtonText", "Cancel"))
				]
			]
	];
}


TSharedRef<SWidget> STiledLandscapeBakeDlg::HandleTileConfigurationComboBoxGenarateWidget(TSharedPtr<TiledStruct> InItem) const
{
	return SNew(SBox)
		.Padding(4)
		[
			SNew(STextBlock).Text(
				FText::FromString(
					FString::Printf(TEXT("%d x %d"),InItem->TileSize, InItem->TileSize)
				)
			)
		];
}


void STiledLandscapeBakeDlg::OnTiledSelectionChanged(TSharedPtr<TiledStruct> InTileConfig, ESelectInfo::Type SelectInfo) {
	CurSelected = InTileConfig;
}


FText STiledLandscapeBakeDlg::GetTileConfigurationText() const{
	if (CurSelected.IsValid()){
		return FText::FromString(FString::Printf(TEXT("%d x %d"), CurSelected->TileSize, CurSelected->TileSize));
	}
	return FText();
}

FReply STiledLandscapeBakeDlg::OnClickedOK(){
	ParentWindow->RequestDestroyWindow();
	bShouldBakeTiled = true;
	return FReply::Handled();
}

FReply STiledLandscapeBakeDlg::OnClickedCancel(){
	ParentWindow->RequestDestroyWindow();
	bShouldBakeTiled = false;
	return FReply::Handled();
}


#undef LOCTEXT_NAMESPACE