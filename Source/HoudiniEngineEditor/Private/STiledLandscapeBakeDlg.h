// YJH Created by 2020-3-20
#pragma once
#include "CoreMinimal.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Input/Reply.h"
#include "Widgets/SWidget.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Input/SComboBox.h"
#include "PropertyHandle.h"

class STiledLandscapeBakeDlg : public SCompoundWidget {
public:

	SLATE_BEGIN_ARGS(STiledLandscapeBakeDlg)
	{}
	SLATE_END_ARGS()


	struct TiledStruct {
		TiledStruct(int32 TileSize) :
			TileSize(TileSize)
		{}

		int32 TileSize;
	};


	void Construct(const FArguments& InArgs, TSharedPtr<SWindow> InParentWindow, FIntPoint LandscapeComponentCount);

	void OnTiledSelectionChanged(TSharedPtr<TiledStruct> InTileConfig, ESelectInfo::Type SelectInfo);

	TSharedRef<SWidget> HandleTileConfigurationComboBoxGenarateWidget(TSharedPtr<TiledStruct> InItem) const;

	FText GetTileConfigurationText() const;

	FReply OnClickedOK();

	FReply OnClickedCancel();

	inline TSharedPtr<TiledStruct> GetCurSelected() const { return CurSelected; }

	inline bool ShouldBakeTiled() const{ return bShouldBakeTiled; }



private:
	bool bShouldBakeTiled;

	TSharedPtr<SWindow> ParentWindow;

	TSharedPtr<SComboBox<TSharedPtr<TiledStruct>>> TileComboBox;

	TArray<TSharedPtr<TiledStruct>> TileOptions;

	TSharedPtr<TiledStruct> CurSelected;

	static const int32 TiledSize[];
};