//------------------------------------------------------------------------------
// NX12 Step1：图纸创建与视图放置（DLL 1/6）
// 功能：创建图纸(A4横向)、设置图号/图名、创建载体基础视图、创建A-A全剖视图、
//       两步法定位视图到目标坐标、设置剖视图比例
//------------------------------------------------------------------------------

// Win32 防护
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <stdlib.h>
#ifdef CreateDialog
#undef CreateDialog
#endif

// 共享头文件
#include "../Shared/NX12_CommonConfig.h"
#include "../Shared/NX12_CommonUtils.h"

// Step1 专有 includes
#include <NXOpen/Drawings_DraftingDrawingSheetBuilder.hxx>
#include <NXOpen/Drawings_BaseViewBuilder.hxx>
#include <NXOpen/Drawings_SectionViewBuilder.hxx>
#include <NXOpen/Drawings_SectionLineSegmentsBuilder.hxx>
#include <NXOpen/Drawings_SectionLineSegmentPointListBuilder.hxx>
#include <NXOpen/Drawings_SectionLineSegmentPointBuilder.hxx>
#include <NXOpen/Drawings_EditViewSettingsBuilder.hxx>
#include <NXOpen/Drawings_ViewPlacementBuilder.hxx>
#include <NXOpen/Drawings_SelectModelViewBuilder.hxx>
#include <NXOpen/Drawings_ParentViewBuilder.hxx>
#include <NXOpen/Drawings_ViewStyleBuilder.hxx>
#include <NXOpen/Drawings_DraftingComponentSelectionBuilder.hxx>
#include <NXOpen/Drawings_DraftingDrawingSheetCollection.hxx>
#include <NXOpen/DraftingManager.hxx>
#include <NXOpen/Point.hxx>
#include <NXOpen/PointCollection.hxx>
#include <NXOpen/Unit.hxx>
#include <NXOpen/UnitCollection.hxx>
#include <NXOpen/Expression.hxx>
#include <NXOpen/ExpressionCollection.hxx>
#include <NXOpen/Update.hxx>
#include <NXOpen/SelectDisplayableObject.hxx>
#include <NXOpen/DisplayableObject.hxx>
#include <NXOpen/SelectNXObject.hxx>
#include <NXOpen/Drawings_SelectDraftingView.hxx>
#include <NXOpen/Drafting_SettingsManager.hxx>
#include <NXOpen/NXMessageBox.hxx>
#include <NXOpen/UI.hxx>

//------------------------------------------------------------------------------
// 阶段2：创建图纸（按 SheetDesc 配置，幂等匹配图纸名 Name）
// 图号规则：NX12 要求图号为从 1 开始的连续整数序列，故新建图纸时
// 动态取"现有图纸数量 + 1"作为新图号；
// 复用分支（按 Name 匹配到已有图纸）不触碰其原图号。
//------------------------------------------------------------------------------
NXOpen::Drawings::DraftingDrawingSheet* phase_create_sheet(NXOpen::Part* part, const ShellDrawingConfig& cfg, const SheetDesc& sheetDesc)
{
	// 幂等保护：若已存在同名图纸，直接打开复用，不重复创建
	for (NXOpen::Drawings::DraftingDrawingSheetCollection::iterator it =
		part->DraftingDrawingSheets()->begin();
		it != part->DraftingDrawingSheets()->end(); ++it)
	{
		NXOpen::Drawings::DraftingDrawingSheet* s = *it;
		if (s && std::string(s->Name().GetText()) == sheetDesc.name)
		{
			s->Open();
			CommonUtils::print_msg(string("[2/8] 已存在图纸 \"") + sheetDesc.name + "\"，直接打开复用，跳过创建");
			return s;
		}
	}

	NXOpen::Drawings::DraftingDrawingSheetBuilder* dsBuilder =
		part->DraftingDrawingSheets()->CreateDraftingDrawingSheetBuilder(NULL);

	dsBuilder->SetAutoStartViewCreation(true);
	dsBuilder->SetStandardMetricScale(NXOpen::Drawings::DrawingSheetBuilder::SheetStandardMetricScaleS11);
	dsBuilder->SetStandardEnglishScale(NXOpen::Drawings::DrawingSheetBuilder::SheetStandardEnglishScaleS11);
	dsBuilder->SetMetricSheetTemplateLocation(cfg.templatePath);
	dsBuilder->SetEnglishSheetTemplateLocation("");
	dsBuilder->SetName(sheetDesc.name);
	dsBuilder->SetHeight(sheetDesc.height);
	dsBuilder->SetLength(sheetDesc.length);
	dsBuilder->SetScaleNumerator(sheetDesc.scaleNum);
	dsBuilder->SetScaleDenominator(sheetDesc.scaleDen);
	dsBuilder->SetUnits(NXOpen::Drawings::DrawingSheetBuilder::SheetUnitsMetric);
	dsBuilder->SetProjectionAngle(NXOpen::Drawings::DrawingSheetBuilder::SheetProjectionAngleThird);

	// ---- 图号动态分配：新图号 = 现有图纸数量 + 1 ----
	const int existingSheetCount = (int)std::distance(
		part->DraftingDrawingSheets()->begin(),
		part->DraftingDrawingSheets()->end());
	const int assignedNumber = existingSheetCount + 1;
	const std::string assignedNumberStr = std::to_string(assignedNumber);
	dsBuilder->SetNumber(assignedNumberStr.c_str());
	dsBuilder->SetSecondaryNumber("");
	dsBuilder->SetRevision("A");
	CommonUtils::print_msg(string("[") + sheetDesc.name + "] 图号自动分配: " + assignedNumberStr);

	NXOpen::NXObject* sheetObj = NULL;
	try
	{
		sheetObj = dsBuilder->Commit();
	}
	catch (const NXOpen::NXException& e)
	{
		CommonUtils::print_msg(string("  警告: 图纸 Commit 失败（本次分配图号: ") + assignedNumberStr +
			"，现有图纸总数: " + std::to_string(existingSheetCount) +
			"）: " + e.Message());
		dsBuilder->Destroy();
		throw;
	}
	dsBuilder->Destroy();
	part->Drafting()->SetTemplateInstantiationIsComplete(true);

	NXOpen::Drawings::DraftingDrawingSheet* sheet =
		dynamic_cast<NXOpen::Drawings::DraftingDrawingSheet*>(sheetObj);
	if (sheet) sheet->Open();

	CommonUtils::print_msg(string("[2/8] 已创建图纸 \"") + sheetDesc.name +
		"\"（图号 " + assignedNumberStr + "）");
	return sheet;
}

//------------------------------------------------------------------------------
// 阶段2b：设置本图纸（成员视图）的图层可见性——按图层显隐状态过滤
//------------------------------------------------------------------------------
void phase_apply_sheet_layer_visibility(NXOpen::Part* part,
	NXOpen::Drawings::DraftingDrawingSheet* sheet)
{
	if (!sheet) return;
	try
	{
		std::vector<NXOpen::Layer::StateInfo> states;
		for (int ly = 1; ly <= 256; ++ly)
		{
			if (part->Layers()->GetState(ly) == NXOpen::Layer::StateHidden)
			{
				states.push_back(NXOpen::Layer::StateInfo(ly, NXOpen::Layer::StateHidden));
			}
		}

		if (states.empty())
		{
			CommonUtils::print_msg("  - 所有图层均为显示状态，无需图纸级隐藏");
			return;
		}

		part->Layers()->SetObjectsVisibilityOnLayer(sheet->View(), states, true);

		// 回显被隐藏的图层号（数量多时截断显示）
		std::string layers;
		const size_t kMaxShown = 16;
		for (size_t k = 0; k < states.size() && k < kMaxShown; ++k)
		{
			if (k) layers += ",";
			layers += std::to_string(states[k].Layer);
		}
		if (states.size() > kMaxShown) layers += ",...";
		CommonUtils::print_msg(string("  - 已按图层显隐状态隐藏 ") + std::to_string(states.size()) +
			" 个图层: " + layers);
	}
	catch (const NXOpen::NXException& e)
	{
		CommonUtils::print_msg(string("  警告: 设置图纸图层可见性失败: ") + e.Message());
	}
}

//------------------------------------------------------------------------------
// 阶段3：创建基础视图（载体视图=剖视父视图，按零件轴向选择，默认 Right）
//------------------------------------------------------------------------------
NXOpen::Drawings::BaseView* phase_create_base_view(NXOpen::Part* part, const ShellDrawingConfig& cfg)
{
	NXOpen::ModelingView* modelView = dynamic_cast<NXOpen::ModelingView*>(
		part->ModelingViews()->FindObject(cfg.baseModelViewName));
	if (!modelView)
	{
		CommonUtils::print_msg(string("错误：找不到建模视图 ") + cfg.baseModelViewName);
		return NULL;
	}

	NXOpen::Drawings::BaseViewBuilder* bvBuilder =
		part->DraftingViews()->CreateBaseViewBuilder(NULL);

	bvBuilder->SelectModelView()->SetSelectedView(modelView);
	bvBuilder->Placement()->SetAssociative(true);
	bvBuilder->SecondaryComponents()->SetObjectType(
		NXOpen::Drawings::DraftingComponentSelectionBuilder::GeometryPrimaryGeometry);
	bvBuilder->Style()->ViewStyleBase()->SetPart(part);
	bvBuilder->Style()->ViewStyleBase()->SetPartName(cfg.partPath);

	// 放置点（图纸坐标，直接使用 cfg.baseViewPlace，不再乘 kSheetScale）
	bvBuilder->Placement()->Placement()->SetValue(NULL, part->Views()->WorkView(), cfg.baseViewPlace);

	NXOpen::NXObject* bvObj = bvBuilder->Commit();
	bvBuilder->Destroy();

	NXOpen::Drawings::BaseView* baseView = dynamic_cast<NXOpen::Drawings::BaseView*>(bvObj);
	if (!baseView)
	{
		CommonUtils::print_msg("错误：基础视图创建失败");
		return NULL;
	}
	CommonUtils::print_msg(string("[3/8] 已创建基础视图 (载体视图 ") + cfg.baseModelViewName + "，放置于目标位置)");
	return baseView;
}

//------------------------------------------------------------------------------
// 阶段4：创建 A-A 全剖视图（主体视图，两点直线剖切）
//------------------------------------------------------------------------------
NXOpen::Drawings::SectionView* phase_create_aa_section(NXOpen::Part* part, const ShellDrawingConfig& cfg, NXOpen::Drawings::BaseView* baseView)
{
	if (!baseView) return NULL;

	NXOpen::Drawings::SectionViewBuilder* svBuilder =
		part->DraftingViews()->CreateSectionViewBuilder(NULL);

	// 直线（简单阶梯）剖切线类型
	svBuilder->SetSectionViewType(
		NXOpen::Drawings::SectionViewBuilder::SectionLineTypeSimpleStepped);

	svBuilder->ViewPlacement()->SetAlignmentMethod(
		NXOpen::Drawings::ViewPlacementBuilder::MethodPerpendicularToHingeLine);
	svBuilder->ViewPlacement()->SetAlignmentOption(
		NXOpen::Drawings::ViewPlacementBuilder::OptionModelPoint);
	svBuilder->ViewStyle()->ViewStyleOrientation()->HingeLine()->SetAssociative(true);
	svBuilder->SecondaryComponents()->SetObjectType(
		NXOpen::Drawings::DraftingComponentSelectionBuilder::GeometryPrimaryGeometry);
	svBuilder->ParentView()->View()->SetValue(baseView);
	svBuilder->ViewStyle()->ViewStyleBase()->SetPartName(cfg.partPath);
	svBuilder->ViewPlacement()->AlignmentView()->SetValue(baseView);

	// 两点直线剖切
	NXOpen::Point3d secPt1(cfg.baseViewPlace.X, cfg.baseViewPlace.Y + cfg.aaSecHalfLen, 0.0);
	NXOpen::Point3d secPt2(cfg.baseViewPlace.X, cfg.baseViewPlace.Y - cfg.aaSecHalfLen, 0.0);
	char secFmt[192];
	sprintf_s(secFmt, sizeof(secFmt),
		"  剖切线(铅垂): 起点(%.3f, %.3f) -> 终点(%.3f, %.3f), 半长d=%.3f（待实机微调）",
		secPt1.X, secPt1.Y, secPt2.X, secPt2.Y, cfg.aaSecHalfLen);
	CommonUtils::print_msg(secFmt);
	NXOpen::Point* pt1 = part->Points()->CreatePoint(secPt1);
	svBuilder->SectionLineSegments()->SegmentLocation()->AddCutSegment(pt1);
	svBuilder->SectionLineSegments()->SetSectionLineOnlyPlacementOrigin(secPt2);

	// 初始放置点：图幅中心附近
	NXOpen::Point3d aaViewPlaceInit(cfg.sheetLength / 2.0, cfg.sheetHeight / 2.0, 0.0);
	svBuilder->ViewPlacement()->Placement()->SetValue(NULL, part->Views()->WorkView(), aaViewPlaceInit);

	NXOpen::NXObject* svObj = svBuilder->Commit();
	svBuilder->Destroy();

	NXOpen::Drawings::SectionView* sectionView = dynamic_cast<NXOpen::Drawings::SectionView*>(svObj);
	if (!sectionView)
	{
		CommonUtils::print_msg("错误：A-A 剖视图创建失败");
		return NULL;
	}
	CommonUtils::print_msg(string("[4/8] 已创建 ") + cfg.aaViewLabel + " 全剖视图（主体视图）");
	return sectionView;
}

//------------------------------------------------------------------------------
// 阶段8.1：剖视比例设置（单图流程中执行一次；DoUpdate 由主流程统一收尾）
//------------------------------------------------------------------------------
void phase_post_process_view_scale(NXOpen::Part* part, const ShellDrawingConfig& cfg,
	NXOpen::Drawings::SectionView* sectionView)
{
	if (sectionView)
	{
		try
		{
			std::vector<NXOpen::View*> views;
			views.push_back(sectionView);
			NXOpen::Drawings::EditViewSettingsBuilder* evBuilder =
				part->SettingsManager()->CreateDrawingEditViewSettingsBuilder(views);
			evBuilder->ViewStyle()->ViewStyleGeneral()->Scale()->SetNumerator(cfg.aaViewScale);
			evBuilder->Commit();
			evBuilder->Destroy();
			CommonUtils::print_msg("  - 已设置剖视图比例");
		}
		catch (NXOpen::NXException& e)
		{
			CommonUtils::print_msg(string("  警告: 比例设置失败: ") + e.Message());
		}
	}
}

//------------------------------------------------------------------------------
// 主流程
//------------------------------------------------------------------------------
void do_it()
{
	try
	{
		// ==================== 配置区（按零件修改） ====================
		static const double kSheetScale = 297.0 / 1189.0;

		ShellDrawingConfig cfg;
		cfg.templatePath =
			"D:\\Program Files\\Siemens\\NX 12.0\\localization\\prc\\simpl_chinese\\startup\\A4-noviews-asm-template.prt";

		// partPath 自动取当前工作部件完整路径
		static const char* kFallbackPartPath = "Y:\\Brisk\\BV3\\BV3R\\2D\\02KBV3RCL20A.prt";
		std::string workPartPath;
		try
		{
			workPartPath = CommonUtils::get_session()->Parts()->BaseWork()->FullPath().GetText();
		}
		catch (...) { workPartPath.clear(); }
		if (workPartPath.empty()) workPartPath = kFallbackPartPath;
		cfg.partPath = workPartPath.c_str();

		cfg.sheetHeight = 210.0;
		cfg.sheetLength = 297.0;
		cfg.sheetScaleNum = 1.0;
		cfg.sheetScaleDen = 1.0;

		cfg.baseModelViewName = "Right";
		cfg.baseViewPlace = Point3d(82.0, 62.0, 0.0);

		cfg.aaSecHalfLen = 50.0;
		cfg.aaViewTarget = Point3d(185.0, 112.0, 0.0);
		cfg.aaViewScale = 1.5;
		cfg.aaViewLabel = "A-A";

		cfg.createCenterline = true;
		cfg.autoContourDims = true;
		cfg.createDimensions = true;

		cfg.processCount = 4;
		cfg.defaultProcessLayers[0] = 21;
		cfg.defaultProcessLayers[1] = 22;
		cfg.defaultProcessLayers[2] = 23;
		cfg.defaultProcessLayers[3] = 24;
		for (int i = 4; i < MAX_PROCESS_COUNT; ++i)
			cfg.defaultProcessLayers[i] = 21 + i;

		SheetDesc sheets[1] = {
			{ "车床图", 210.0, 297.0, 1.0, 1.0 },
		};

		cfg.aaSecHalfLen *= kSheetScale;
		// =================================================================

		NXOpen::Part* part = dynamic_cast<NXOpen::Part*>(CommonUtils::get_session()->Parts()->BaseWork());
		if (!part) { CommonUtils::print_msg("错误：当前工作部件不是 Part 类型"); return; }

		// 阶段1：进入制图模块
		CommonUtils::get_session()->ApplicationSwitchImmediate("UG_APP_DRAFTING");
		part->Drafting()->EnterDraftingApplication();
		CommonUtils::print_msg("[1] 已进入制图模块");

		// 阶段2：创建图纸
		NXOpen::Drawings::DraftingDrawingSheet* sheet = phase_create_sheet(part, cfg, sheets[0]);
		if (!sheet) { CommonUtils::print_msg("图纸创建失败"); return; }

		// 阶段2b：图层可见性
		phase_apply_sheet_layer_visibility(part, sheet);

		// 二级幂等
		if (!sheet->GetDraftingViews().empty()) {
			CommonUtils::print_msg("已存在视图，跳过视图创建");
		} else {
			// 阶段3：基础视图
			NXOpen::Drawings::BaseView* baseView = phase_create_base_view(part, cfg);
			if (!baseView) { CommonUtils::print_msg("基础视图创建失败"); return; }

			// 阶段3b：载体定位
			CommonUtils::center_view(part, baseView, sheets[0], &cfg.baseViewPlace);

			// 阶段4：剖视图
			NXOpen::Drawings::SectionView* sectionView = phase_create_aa_section(part, cfg, baseView);

			// 阶段4b：剖视定位
			NXOpen::Point3d aaTarget(cfg.aaViewTarget);
			CommonUtils::center_view(part, sectionView, sheets[0], &aaTarget);

			// 阶段8.1：比例
			phase_post_process_view_scale(part, cfg, sectionView);
		}

		// 最终更新
		NXOpen::Session::UndoMarkId finalMark = CommonUtils::get_session()->SetUndoMark(
			NXOpen::Session::MarkVisibilityVisible, "Final Update");
		CommonUtils::get_session()->UpdateManager()->DoUpdate(finalMark);
		CommonUtils::print_msg("========== Step1 图纸创建与视图放置 完成 ==========");
	}
	catch (const NXOpen::NXException& e) { CommonUtils::print_msg(std::string("NXException: ") + e.Message()); }
	catch (const std::exception& e) { CommonUtils::print_msg(std::string("Exception: ") + e.what()); }
	catch (...) { CommonUtils::print_msg("Unknown Exception"); }
}

//------------------------------------------------------------------------------
// Entry point(s) for unmanaged internal NXOpen C/C++ programs
//------------------------------------------------------------------------------
extern "C" DllExport void ufusr(char *parm, int *returnCode, int rlen)
{
	try
	{
		do_it();
	}
	catch (const NXException& e)
	{
		UI::GetUI()->NXMessageBox()->Show("NXException", NXOpen::NXMessageBox::DialogTypeError, e.Message());
	}
	catch (const exception& e)
	{
		UI::GetUI()->NXMessageBox()->Show("Exception", NXOpen::NXMessageBox::DialogTypeError, e.what());
	}
	catch (...)
	{
		UI::GetUI()->NXMessageBox()->Show("Exception", NXOpen::NXMessageBox::DialogTypeError, "Unknown Exception.");
	}
}

extern "C" DllExport int ufusr_ask_unload()
{
	return (int)NXOpen::Session::LibraryUnloadOptionImmediately;
}
