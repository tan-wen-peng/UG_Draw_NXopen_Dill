//------------------------------------------------------------------------------
// NX12 Step2：图纸首选项与字体调整（DLL 2/6）
// 功能：修改当前图纸的首选项设置，包括尺寸文本样式、字体大小、
//       标注样式、视图标签样式等
//------------------------------------------------------------------------------

// Win32 防护（windows.h 的 CreateDialog 宏会破坏 NXOpen/UI.hxx）
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#ifdef CreateDialog
#undef CreateDialog
#endif

// 共享头文件
#include "../Shared/NX12_CommonConfig.h"
#include "../Shared/NX12_CommonUtils.h"

// Step2 专有 includes
#include <NXOpen/Drafting_SettingsManager.hxx>
#include <NXOpen/Drafting_PreferencesBuilder.hxx>
#include <NXOpen/Annotations_StyleBuilder.hxx>
#include <NXOpen/Annotations_LetteringStyleBuilder.hxx>
#include <NXOpen/Annotations_LineArrowStyleBuilder.hxx>
#include <NXOpen/Drawings_EditViewSettingsBuilder.hxx>
#include <NXOpen/Drawings_ViewStyleBuilder.hxx>
#include <NXOpen/Drawings_ViewLabelBuilder.hxx>
#include <NXOpen/Drawings_ViewCommonViewLabelBuilder.hxx>
#include <NXOpen/Drawings_DraftingDrawingSheet.hxx>
#include <NXOpen/Drawings_DraftingDrawingSheetCollection.hxx>
#include <NXOpen/FontCollection.hxx>
#include <NXOpen/DraftingManager.hxx>
#include <NXOpen/Drawings_DraftingView.hxx>
#include <NXOpen/Drawings_DraftingViewCollection.hxx>
#include <NXOpen/NXMessageBox.hxx>

using namespace NXOpen;
using std::string;
using std::exception;

// ==================== Step2 配置区 ====================
// 字体/样式参数（按实际需求修改）
// 注：NX12 字体使用整数 ID，通过 FontCollection::AddFont 按名称注册后获取
static const char* kDimensionFontName = "FangSong";    // 尺寸文本字体（仿宋）
static const double kDimensionFontSize = 3.0;           // 尺寸文本字高(mm)
static const double kDimensionTextLineSpacing = 1.0;    // 尺寸文本行间距因子
static const char* kViewLabelFontName = "FangSong";    // 视图标签（附加文本）字体（仿宋）
static const char* kToleranceFontName = "FangSong";    // 公差文本字体（仿宋）
static const double kToleranceTextSize = 2.0;          // 公差文本字高(mm)
static const double kAppendedTextSize = 3.0;             // 附加文本大小(mm)
static const double kAppendedTextAspectRatio = 1.0;      // 附加文本宽高比
static const double kDimensionTextAspectRatio = 1.0;     // 尺寸文本宽高比
static const double kToleranceTextAspectRatio = 1.0;     // 公差文本宽高比
// 注：NX11.0.0 起符号宽高比按文本类别独立提供，无全局 SetSymbolAspectRatio
static const double kAppendedTextSymbolAspectRatio = 1.0;   // 附加文本符号宽高比
static const double kDimensionTextSymbolAspectRatio = 1.0;  // 尺寸文本符号宽高比
static const double kToleranceTextSymbolAspectRatio = 1.0;  // 公差文本符号宽高比
// =====================================================

//------------------------------------------------------------------------------
// 辅助：通过字体名称获取 NX 字体 ID
// 使用 FontCollection::AddFont(name, TypeStandard) 注册/查找字体
// 失败时返回 -1（降级：不修改字体，仅修改字高）
//------------------------------------------------------------------------------
static int get_or_add_font_id(NXOpen::Part* part, const char* fontName)
{
	if (!part || !fontName || !fontName[0]) return -1;
	try
	{
		int fontId = part->Fonts()->AddFont(
			fontName, NXOpen::FontCollection::TypeStandard);
		return fontId;
	}
	catch (const NXOpen::NXException& e)
	{
		CommonUtils::print_msg(string("  警告: 注册字体 \"") + fontName + "\" 失败: " + e.Message());
		return -1;
	}
	catch (...)
	{
		CommonUtils::print_msg(string("  警告: 注册字体 \"") + fontName + "\" 失败（未知异常）");
		return -1;
	}
}

//------------------------------------------------------------------------------
// 设置尺寸/公差文本首选项（通过 PreferencesBuilder -> AnnotationStyle -> LetteringStyle）
// API 路径：
//   part->SettingsManager()->CreatePreferencesBuilder()
//     ->AnnotationStyle()->LetteringStyle()
//       ->SetDimensionTextFont(int)   尺寸文本字体 ID
//       ->SetDimensionTextSize(double) 尺寸文本字高(mm)
//       ->SetDimensionTextLineSpaceFactor(double) 行间距因子
//       ->SetToleranceTextFont(int)   公差文本字体 ID（NX6.0.0）
//       ->SetToleranceTextSize(double) 公差文本字高(mm)（NX6.0.0）
//         （见 NXOpen/Annotations_LetteringStyleBuilder.hxx）
//------------------------------------------------------------------------------
static void apply_dimension_text_preferences(NXOpen::Part* part, int fontId, double fontSize, int toleranceFontId)
{
	NXOpen::Drafting::PreferencesBuilder* prefBuilder =
		part->SettingsManager()->CreatePreferencesBuilder();

	NXOpen::Annotations::LetteringStyleBuilder* lettering =
		prefBuilder->AnnotationStyle()->LetteringStyle();

	// 设置尺寸文本字高
	lettering->SetDimensionTextSize(fontSize);
	CommonUtils::print_msg(string("  - 尺寸文本字高已设置为 ") + std::to_string(fontSize) + " mm");

	// 设置尺寸文本行间距因子
	lettering->SetDimensionTextLineSpaceFactor(kDimensionTextLineSpacing);
	CommonUtils::print_msg(string("  - 尺寸文本行间距因子已设置为 ") +
		std::to_string(kDimensionTextLineSpacing));

	// 设置尺寸文本宽高比（长度/高度比，1.0 为标准）
	lettering->SetDimensionTextAspectRatio(kDimensionTextAspectRatio);
	CommonUtils::print_msg(string("  - 尺寸文本宽高比已设置为 ") +
		std::to_string(kDimensionTextAspectRatio));

	// 设置公差文本宽高比
	lettering->SetToleranceTextAspectRatio(kToleranceTextAspectRatio);
	CommonUtils::print_msg(string("  - 公差文本宽高比已设置为 ") +
		std::to_string(kToleranceTextAspectRatio));

	// 设置尺寸文本符号宽高比（NX12 无全局 SetSymbolAspectRatio，按类别拆分设置）
	lettering->SetDimensionTextSymbolAspectRatio(kDimensionTextSymbolAspectRatio);
	CommonUtils::print_msg(string("  - 尺寸文本符号宽高比已设置为 ") +
		std::to_string(kDimensionTextSymbolAspectRatio));

	// 设置公差文本符号宽高比
	lettering->SetToleranceTextSymbolAspectRatio(kToleranceTextSymbolAspectRatio);
	CommonUtils::print_msg(string("  - 公差文本符号宽高比已设置为 ") +
		std::to_string(kToleranceTextSymbolAspectRatio));

	// 设置公差文本字高（mm）
	lettering->SetToleranceTextSize(kToleranceTextSize);
	CommonUtils::print_msg(string("  - 公差文本字高已设置为 ") +
		std::to_string(kToleranceTextSize) + " mm");

	// 设置公差文本字体（toleranceFontId 有效时才设置）
	if (toleranceFontId >= 0)
	{
		lettering->SetToleranceTextFont(toleranceFontId);
		CommonUtils::print_msg(string("  - 公差文本字体 ID 已设置为 ") + std::to_string(toleranceFontId));
	}
	else
	{
		CommonUtils::print_msg("  - 公差文本字体未修改（字体 ID 无效）");
	}

	// 设置尺寸文本字体（fontId 有效时才设置）
	if (fontId >= 0)
	{
		lettering->SetDimensionTextFont(fontId);
		CommonUtils::print_msg(string("  - 尺寸文本字体 ID 已设置为 ") + std::to_string(fontId));
	}
	else
	{
		CommonUtils::print_msg("  - 尺寸文本字体未修改（字体 ID 无效）");
	}

	prefBuilder->Commit();
	prefBuilder->Destroy();
}

//------------------------------------------------------------------------------
// 设置视图标签（附加文本）首选项（通过 PreferencesBuilder -> AnnotationStyle -> LetteringStyle）
// 附加文本大小统一为 3.0 mm（含视图标签等附加文字）；
// 附加文本宽高比与符号宽高比均设为 1.0。
// 注：ViewLabelBuilder 无直接字体名/字高 API，字体通过 SetAppendedTextFont 控制
//------------------------------------------------------------------------------
static void apply_view_label_preferences(NXOpen::Part* part, int fontId)
{
	NXOpen::Drafting::PreferencesBuilder* prefBuilder =
		part->SettingsManager()->CreatePreferencesBuilder();

	// 视图标签的 Lettering 通过 AnnotationStyle 的 LetteringStyle 设置
	NXOpen::Annotations::LetteringStyleBuilder* lettering =
		prefBuilder->AnnotationStyle()->LetteringStyle();

	// 设置附加文本（视图标签等）的大小为 3.0 mm
	lettering->SetAppendedTextSize(kAppendedTextSize);
	CommonUtils::print_msg(string("  - 附加文本大小已设置为 ") + std::to_string(kAppendedTextSize) + " mm");

	// 设置附加文本宽高比
	lettering->SetAppendedTextAspectRatio(kAppendedTextAspectRatio);
	CommonUtils::print_msg(string("  - 附加文本宽高比已设置为 ") +
		std::to_string(kAppendedTextAspectRatio));

	// 设置附加文本符号宽高比
	lettering->SetAppendedTextSymbolAspectRatio(kAppendedTextSymbolAspectRatio);
	CommonUtils::print_msg(string("  - 附加文本符号宽高比已设置为 ") +
		std::to_string(kAppendedTextSymbolAspectRatio));

	if (fontId >= 0)
	{
		lettering->SetAppendedTextFont(fontId);
		CommonUtils::print_msg(string("  - 视图标签字体 ID 已设置为 ") + std::to_string(fontId));
	}
	else
	{
		CommonUtils::print_msg("  - 视图标签字体未修改（字体 ID 无效）");
	}

	prefBuilder->Commit();
	prefBuilder->Destroy();
}

//------------------------------------------------------------------------------
// 设置标注箭头样式（通过 PreferencesBuilder -> AnnotationStyle -> LineArrowStyle）
// 尝试设置箭头相关首选项，如果 API 不可用则降级打印
//------------------------------------------------------------------------------
static void apply_arrow_style_preferences(NXOpen::Part* part)
{
	try
	{
		NXOpen::Drafting::PreferencesBuilder* prefBuilder =
			part->SettingsManager()->CreatePreferencesBuilder();

		// LineArrowStyle 提供箭头样式设置
		NXOpen::Annotations::LineArrowStyleBuilder* arrowStyle =
			prefBuilder->AnnotationStyle()->LineArrowStyle();

		// 此处可设置箭头样式参数（如箭头大小等）
		// 具体参数按实际需求调整，当前保持默认
		(void)arrowStyle;
		CommonUtils::print_msg("  - 标注箭头样式已检查（保持默认）");

		prefBuilder->Commit();
		prefBuilder->Destroy();
	}
	catch (const NXOpen::NXException& e)
	{
		CommonUtils::print_msg(string("  警告: 标注箭头样式设置失败: ") + e.Message());
	}
	catch (...)
	{
		CommonUtils::print_msg("  警告: 标注箭头样式设置失败（未知异常）");
	}
}

//------------------------------------------------------------------------------
// 主功能
//------------------------------------------------------------------------------
static void do_it()
{
	try
	{
		CommonUtils::print_msg("========== Step2: 图纸首选项与字体调整 ==========");

		NXOpen::Session* theSession = CommonUtils::get_session();
		NXOpen::Part* part = dynamic_cast<NXOpen::Part*>(theSession->Parts()->BaseWork());
		if (!part)
		{
			CommonUtils::print_msg("错误：当前工作部件不是 Part 类型，无法设置图纸首选项");
			return;
		}

		// 1. 确保进入制图模块
		try
		{
			part->Drafting()->EnterDraftingApplication();
			CommonUtils::print_msg("  - 已进入制图模块");
		}
		catch (const NXOpen::NXException& e)
		{
			CommonUtils::print_msg(string("  提示: 进入制图模块: ") + e.Message());
		}

		// 2. 打开当前图纸（如果有图纸但未打开）
		NXOpen::Drawings::DraftingDrawingSheet* currentSheet = NULL;
		try
		{
			// 查找并打开第一张图纸
			for (NXOpen::Drawings::DraftingDrawingSheetCollection::iterator it =
				part->DraftingDrawingSheets()->begin();
				it != part->DraftingDrawingSheets()->end(); ++it)
			{
				NXOpen::Drawings::DraftingDrawingSheet* s = *it;
				if (s)
				{
					s->Open();
					currentSheet = s;
					CommonUtils::print_msg(string("  - 已打开图纸: ") + s->Name().GetText());
					break;
				}
			}
		}
		catch (const NXOpen::NXException& e)
		{
			CommonUtils::print_msg(string("  警告: 打开图纸失败: ") + e.Message());
		}

		if (!currentSheet)
		{
			CommonUtils::print_msg("  警告: 当前部件没有图纸，将尝试设置全局首选项");
		}

		// 3. 注册字体获取字体 ID
		CommonUtils::print_msg("  --- 注册字体 ---");
		int dimFontId = get_or_add_font_id(part, kDimensionFontName);
		if (dimFontId >= 0)
		{
			CommonUtils::print_msg(string("  - 尺寸文本字体 \"") + kDimensionFontName +
				"\" -> ID " + std::to_string(dimFontId));
		}

		int labelFontId = get_or_add_font_id(part, kViewLabelFontName);
		if (labelFontId >= 0)
		{
			CommonUtils::print_msg(string("  - 视图标签字体 \"") + kViewLabelFontName +
				"\" -> ID " + std::to_string(labelFontId));
		}

		int toleranceFontId = get_or_add_font_id(part, kToleranceFontName);
		if (toleranceFontId >= 0)
		{
			CommonUtils::print_msg(string("  - 公差文本字体 \"") + kToleranceFontName +
				"\" -> ID " + std::to_string(toleranceFontId));
		}

		// 4. 设置尺寸文本首选项
		CommonUtils::print_msg("  --- 设置尺寸/公差文本首选项 ---");
		try
		{
			apply_dimension_text_preferences(part, dimFontId, kDimensionFontSize, toleranceFontId);
		}
		catch (const NXOpen::NXException& e)
		{
			CommonUtils::print_msg(string("  警告: 尺寸文本首选项设置失败: ") + e.Message());
		}
		catch (...)
		{
			CommonUtils::print_msg("  警告: 尺寸文本首选项设置失败（未知异常）");
		}

		// 5. 设置视图标签首选项
		CommonUtils::print_msg("  --- 设置视图标签首选项 ---");
		try
		{
			apply_view_label_preferences(part, labelFontId);
		}
		catch (const NXOpen::NXException& e)
		{
			CommonUtils::print_msg(string("  警告: 视图标签首选项设置失败: ") + e.Message());
		}
		catch (...)
		{
			CommonUtils::print_msg("  警告: 视图标签首选项设置失败（未知异常）");
		}

		// 6. 设置标注箭头样式
		CommonUtils::print_msg("  --- 设置标注箭头样式 ---");
		apply_arrow_style_preferences(part);

		// 7. 最终更新
		NXOpen::Session::UndoMarkId mark = theSession->SetUndoMark(
			NXOpen::Session::MarkVisibilityVisible, "Step2 SheetPreferences");
		theSession->UpdateManager()->DoUpdate(mark);

		CommonUtils::print_msg("========== Step2 图纸首选项设置完成 ==========");
	}
	catch (const NXOpen::NXException& e)
	{
		CommonUtils::print_msg(string("!!! NXException: ") + e.Message());
		CommonUtils::get_ui()->NXMessageBox()->Show(
			"Step2 SheetPreferences", NXOpen::NXMessageBox::DialogTypeError, e.Message());
	}
	catch (const std::exception& e)
	{
		CommonUtils::print_msg(string("!!! Exception: ") + e.what());
		CommonUtils::get_ui()->NXMessageBox()->Show(
			"Step2 SheetPreferences", NXOpen::NXMessageBox::DialogTypeError, e.what());
	}
	catch (...)
	{
		CommonUtils::print_msg("!!! Unknown Exception");
		CommonUtils::get_ui()->NXMessageBox()->Show(
			"Step2 SheetPreferences", NXOpen::NXMessageBox::DialogTypeError, "Unknown Exception.");
	}
}

//------------------------------------------------------------------------------
// Entry point
//------------------------------------------------------------------------------
extern "C" DllExport void ufusr(char *parm, int *returnCode, int rlen)
{
	try
	{
		do_it();
	}
	catch (const NXException& e1)
	{
		UI::GetUI()->NXMessageBox()->Show("NXException", NXOpen::NXMessageBox::DialogTypeError, e1.Message());
	}
	catch (const exception& e2)
	{
		UI::GetUI()->NXMessageBox()->Show("Exception", NXOpen::NXMessageBox::DialogTypeError, e2.what());
	}
	catch (...)
	{
		UI::GetUI()->NXMessageBox()->Show("Exception", NXOpen::NXMessageBox::DialogTypeError, "Unknown Exception.");
	}
}

//------------------------------------------------------------------------------
// Unload Handler
//------------------------------------------------------------------------------
extern "C" DllExport int ufusr_ask_unload()
{
	return (int)NXOpen::Session::LibraryUnloadOptionImmediately;
}
