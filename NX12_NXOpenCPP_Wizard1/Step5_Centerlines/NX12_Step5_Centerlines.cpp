//------------------------------------------------------------------------------
// NX12 Step5：中心线标注（DLL 5/6）
// 功能：交互拾取视图内投影边，生成中心线注释
//------------------------------------------------------------------------------
#include "../Shared/NX12_CommonConfig.h"
#include "../Shared/NX12_CommonUtils.h"
// Step5 专有 includes
#include <NXOpen/Annotations_Centerline2d.hxx>
#include <NXOpen/Annotations_Centerline2dBuilder.hxx>
#include <NXOpen/Annotations_CenterlineCollection.hxx>
#include <NXOpen/Annotations_AnnotationManager.hxx>
#include <NXOpen/Selection.hxx>
#include <NXOpen/SelectNXObject.hxx>
#include <NXOpen/NXMessageBox.hxx>

//------------------------------------------------------------------------------
// phase_create_centerline
// 交互拾取剖视图内两条投影边，通过 Centerline2dBuilder 创建中心线注释。
// 关联点修正要点：不再在模型空间/绝对坐标直接创建点对象，
// 而是把光标视图切到"任意视图"（UF_UI_set_cursor_view(0)），
// 在制图成员视图内拾取投影边（视图相关几何），
// 再用 Centerline2dBuilder 的 Side1/Side2 把拾取到的投影边
// （连同所在视图与拾取坐标）写入，生成的中心线与视图投影边保持关联。
// 参数为 A-A 剖视图（DraftingView 基类，SetValue 接受任意成员视图）。
// 返回值：创建成功返回中心线对象（供阶段6a 坐标标注作基准），
// 跳过/失败返回 NULL（阶段6a 降级警告跳过）。
//------------------------------------------------------------------------------
NXOpen::Annotations::Centerline2d* phase_create_centerline(NXOpen::Part* part, NXOpen::Drawings::SectionView* sectionView)
{
	if (!sectionView)
	{
		CommonUtils::print_msg("[6/8] 中心线已跳过（无剖视图）");
		return NULL;
	}

	try
	{
		// 保存当前光标视图，制图默认为工作视图；
		// 设为 0（任意视图）后才能拾取成员视图内的投影边
		int oldCursorView = 1;
		UF_UI_ask_cursor_view(&oldCursorView);
		UF_UI_set_cursor_view(0);

		NXOpen::NXObject* edge1 = NULL;
		NXOpen::NXObject* edge2 = NULL;
		NXOpen::Point3d cur1, cur2;

		NXOpen::Selection::Response r1 = CommonUtils::get_ui()->SelectionManager()->SelectObject(
			"拾取视图内第一条投影边", "中心线-边1",
			NXOpen::Selection::SelectionScopeWorkPart, false, true, &edge1, &cur1);
		if (r1 != NXOpen::Selection::ResponseObjectSelected || !edge1)
		{
			UF_UI_set_cursor_view(oldCursorView);
			CommonUtils::print_msg("[6/8] 中心线已跳过（未选择投影边）");
			return NULL;
		}

		NXOpen::Selection::Response r2 = CommonUtils::get_ui()->SelectionManager()->SelectObject(
			"拾取视图内第二条投影边", "中心线-边2",
			NXOpen::Selection::SelectionScopeWorkPart, false, true, &edge2, &cur2);

		// 恢复光标视图
		UF_UI_set_cursor_view(oldCursorView);

		if (r2 != NXOpen::Selection::ResponseObjectSelected || !edge2)
		{
			CommonUtils::print_msg("[6/8] 中心线已跳过（第二条边未选择）");
			return NULL;
		}

		NXOpen::Annotations::Centerline2dBuilder* clBuilder =
			part->Annotations()->Centerlines()->CreateCenterline2dBuilder(NULL);
		// SetValue(对象, 所在视图, 拾取点) —— 保证关联建立在视图投影边上
		clBuilder->Side1()->SetValue(edge1, sectionView, cur1);
		clBuilder->Side2()->SetValue(edge2, sectionView, cur2);
		NXOpen::NXObject* clObj = clBuilder->Commit();
		clBuilder->Destroy();

		NXOpen::Annotations::Centerline2d* centerline =
			dynamic_cast<NXOpen::Annotations::Centerline2d*>(clObj);
		CommonUtils::print_msg("[6/8] 已创建中心线（关联于视图投影边）");
		return centerline;
	}
	catch (const NXOpen::NXException& e)
	{
		CommonUtils::print_msg(string("  警告: 中心线创建失败: ") + e.Message());
	}
	return NULL;
}

//------------------------------------------------------------------------------
// do_it —— Step5 入口
//------------------------------------------------------------------------------
void do_it()
{
	try
	{
		NXOpen::Part* part = dynamic_cast<NXOpen::Part*>(CommonUtils::get_session()->Parts()->BaseWork());
		if (!part) { CommonUtils::print_msg("错误：无工作部件"); return; }

		NXOpen::Drawings::SectionView* sectionView = CommonUtils::find_section_view(part);
		if (!sectionView) { CommonUtils::print_msg("[Step5] 未找到剖视图，请先运行 Step1"); return; }

		NXOpen::Annotations::Centerline2d* cl = phase_create_centerline(part, sectionView);
		if (cl) CommonUtils::print_msg("========== Step5 中心线 完成 ==========");
		else CommonUtils::print_msg("[Step5] 中心线创建被跳过或失败");
	}
	catch (const NXOpen::NXException& e) { CommonUtils::print_msg(std::string("NXException: ") + e.Message()); }
	catch (...) { CommonUtils::print_msg("Unknown Exception"); }
}

//------------------------------------------------------------------------------
// 入口点
//------------------------------------------------------------------------------
extern "C" DllExport void ufusr(char* param, int* retcode, int param_len)
{
	try
	{
		do_it();
	}
	catch (const NXOpen::NXException& e)
	{
		NXOpen::UI::GetUI()->NXMessageBox()->Show("NXException", NXOpen::NXMessageBox::DialogTypeError, e.Message());
	}
	catch (const std::exception& e)
	{
		NXOpen::UI::GetUI()->NXMessageBox()->Show("Exception", NXOpen::NXMessageBox::DialogTypeError, e.what());
	}
	catch (...)
	{
		NXOpen::UI::GetUI()->NXMessageBox()->Show("Exception", NXOpen::NXMessageBox::DialogTypeError, "Unknown Exception.");
	}
}

extern "C" DllExport int ufusr_ask_unload()
{
	return UF_UNLOAD_IMMEDIATELY;
}
