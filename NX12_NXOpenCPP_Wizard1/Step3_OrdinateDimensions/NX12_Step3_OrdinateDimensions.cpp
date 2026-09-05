//------------------------------------------------------------------------------
// NX12 Step3：坐标标注（DLL 3/6）
// 功能：交互拾取剖视图投影线段，用户点选带捕捉的坐标原点，对话框指定
//       测量方向与排列间隔，按坐标值排序后链式生成坐标标注（Ordinate Dimension）
//------------------------------------------------------------------------------
// 版本历史：
//   v2 (2026-09-05) 交互式重构（本文件最新）：
//     · 废弃"自动枚举剖视图轮廓端点 + 中心线/截面边启发式基准"模式（v1），
//       改为用户完全控制：多选线段（SelectTaggedObjects）→ 带捕捉点选原点
//       （UF_UI_point_construct 推断点模式，可捕捉端点/中点/圆心）→ 对话框
//       指定测量方向（垂直=测X / 水平=测Y）与排列间隔（mm）→ 每条线段两端点
//       按坐标值升序排序后，沿录制 VB 同款链式逐条 Commit。
//     · 原点关联策略：NXOpen::Point 继承 SmartObject 而非 DisplayableObject，
//       不能直接作 OrdinateOrigin 关联对象；改为在 enumerate_view_curves 结果中
//       搜索距拾取点最近的曲线特征点（端点/中点/圆心，容差 0.05mm），以
//       特征曲线 + SnapType(Start/End/Mid/Center) + 精确特征点作基准；
//       拾取产生的临时关联点随即 UF_OBJ_delete_object 删除（失败仅告警，
//       点不可见不影响标注）。
//     · 任务#17 教训保留：被测端点过滤掉承载对象即基准曲线的点，以及与原点
//       重合（0.05mm 内）的点；任务#24 放置控制保留：显式放置原点按间隔步进。
//     · 幂等保护降级为警示：检测到本视图已有坐标标注时仅打印数量告警，
//       不再拦截（交互模式用户有意追加，且 v1 的全视图拦截误伤跨批次补标）。
//     · 选择过滤器坑（实测）：不带掩码的 SelectTaggedObjects 会继承 NX 会话
//       Class Selection 残留过滤器（停在"视图/尺寸"上选不中曲线），改用
//       MaskTriple 重载 + SelectionActionClearAndEnableSpecific（曲线/边掩码）。
//     · do_it 不切换图纸（用户要求）：只读当前活动图纸（CurrentDrawingSheet()，
//       空则 UF_DRAW_ask_current_drawing 兜底），剖视图仅在本图纸视图上查找；
//       无剖视图时提示手动激活，绝不 Open。
//     · Step3 不再依赖 Step5 中心线（原点由用户点选）。
//   v1 (原) 自动枚举剖视图轮廓端点，成组水平/垂直坐标标注（中心线/截面边基准）。
//------------------------------------------------------------------------------

// Win32 防护（本模块需要 windows.h 用于坐标标注参数对话框）
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
// 兼容旧工具链/IntelliSense：SDK 10.0.19041 winnt.h 的 DECLSPEC_NOINITALL
// 使用 VS2019 16.5+ 才支持的 no_init_all 属性，旧版下置空（仅损失零初始化提示）
#ifndef DECLSPEC_NOINITALL
#define DECLSPEC_NOINITALL
#endif
#include <windows.h>
#include <stdlib.h>                            // _wtof（对话框数值解析）
#include <wchar.h>                             // swprintf_s（对话框默认值回显）
#ifdef CreateDialog
#undef CreateDialog
#endif

// 共享头文件（含 UF/NXOpen 基础 includes）
#include "../Shared/NX12_CommonConfig.h"
#include "../Shared/NX12_CommonUtils.h"

// Step3 专有 includes
#include <NXOpen/SelectDisplayableObject.hxx>
#include <NXOpen/Annotations_OrdinateDimensionBuilder.hxx>
#include <NXOpen/Annotations_BaseOrdinateDimensionBuilder.hxx>
#include <NXOpen/Annotations_OriginBuilder.hxx>
#include <NXOpen/Annotations_OrdinateOriginDimension.hxx>  // 首条 Commit 产物，后续标注基准
#include <NXOpen/Annotations_Dimension.hxx>               // 首条产物类型诊断/链式容错
#include <NXOpen/Annotations_DimensionCollection.hxx>     // part->Dimensions() 扫描降级
#include <NXOpen/Annotations_OrdinateMarginCollection.hxx>// CreateInferredMargin
#include <NXOpen/Annotations_OrdinateMargin.hxx>          // SetActiveVertical/HorizontalMargin 参数
#include <NXOpen/Annotations_AnnotationManager.hxx>       // part->Annotations()->OrdinateMargins() / NewAssociativity()
#include <NXOpen/Annotations_Associativity.hxx>           // margin 关联链
#include <NXOpen/Annotations_Annotation.hxx>              // SetAssociativeOrigin / AssociativeOriginData
#include <NXOpen/Annotations_StyleBuilder.hxx>            // Style()->LineArrowStyle()
#include <NXOpen/Annotations_LineArrowStyleBuilder.hxx>
#include <NXOpen/Annotations.hxx>                         // LeaderSide 枚举
#include <NXOpen/InferSnapType.hxx>                       // SnapType 枚举（端点/中点/圆心）
#include <NXOpen/DisplayableObject.hxx>
#include <NXOpen/Selection.hxx>                           // SelectTaggedObjects（多选）
#include <NXOpen/NXMessageBox.hxx>
#include <NXOpen/DraftingManager.hxx>                     // part->Drafting()->EnterDraftingApplication()
#include <NXOpen/Drawings_SelectDraftingView.hxx>          // Origin()->AnnotationView() 完整类型（缺则 C2027）
#include <uf_object_types.h>        // UF_dimension_type / UF_dim_ordinate_*_subtype / UF_*_type
#include <uf_ui_types.h>             // UF_UI_SEL_FEATURE_ANY_EDGE（选择掩码）
#include <algorithm>                // std::stable_sort
#include <stdio.h>                  // sprintf_s
#include <vector>

//==============================================================================
// 配置区（通配修改点）
//==============================================================================
static const double kDefaultSpacing = 9.0;    // 排列间隔默认值（mm，对话框可调）
static const double kSpacingMin      = 0.5;   // 排列间隔下限（mm）
static const double kSpacingMax      = 200.0; // 排列间隔上限（mm）
static const double kOriginTol       = 0.05;  // 原点捕捉点匹配曲线特征点的容差（模型单位）
static const double kDupTol          = 0.01;  // 端点去重容差（模型单位）

//==============================================================================
// 坐标标注参数对话框（Win32 内存模板，无需 .dlx 模板文件，Step6 同款范式）
// 界面：测量方向单选（垂直坐标标注=测X / 水平坐标标注=测Y）+ 排列间隔编辑框
//==============================================================================
namespace OrdDlg
{
	const int IDC_DIR_VERT  = 1000;   // 垂直坐标标注（测 X 坐标）
	const int IDC_DIR_HORIZ = 1001;   // 水平坐标标注（测 Y 坐标）
	const int IDC_SPACING   = 1002;   // 排列间隔（mm）

	struct Result
	{
		bool measureX;      // true=测 X（垂直坐标标注）；false=测 Y（水平坐标标注）
		double spacing;     // 排列间隔（mm）
	};
	static Result g_result;
	static double g_defSpacing;

	struct Buf
	{
		BYTE d[8192];
		size_t n;
		Buf() : n(0) { memset(d, 0, sizeof(d)); }
		void W(WORD v)      { memcpy(d + n, &v, 2); n += 2; }
		void DW(DWORD v)    { memcpy(d + n, &v, 4); n += 4; }
		void Str(const WCHAR* s) { size_t L = wcslen(s) + 1; memcpy(d + n, s, L * 2); n += L * 2; }
		void Align()        { n = (n + 3) & ~(size_t)3; }
	};

	// 写入一个控件（类名用原子序号）
	void AddItem(Buf& b, DWORD style, short x, short y, short cx, short cy,
		WORD id, WORD clsOrd, const WCHAR* title)
	{
		b.Align();
		b.DW(style);                    // style
		b.DW(0);                        // dwExtendedStyle
		b.W((WORD)x); b.W((WORD)y); b.W((WORD)cx); b.W((WORD)cy);
		b.W(id);
		b.W(0xFFFF); b.W(clsOrd);       // class: atom
		if (title && title[0]) b.Str(title);
		else b.W(0);
		b.W(0);                         // creation data 长度
	}

	INT_PTR CALLBACK DlgProc(HWND h, UINT m, WPARAM w, LPARAM l)
	{
		switch (m)
		{
		case WM_INITDIALOG:
		{
			WCHAR buf[32];
			swprintf_s(buf, 32, L"%.1f", g_defSpacing);
			SetWindowTextW(GetDlgItem(h, IDC_SPACING), buf);
			// 默认选中垂直坐标标注（测 X）
			CheckDlgButton(h, IDC_DIR_VERT, BST_CHECKED);
			CheckDlgButton(h, IDC_DIR_HORIZ, BST_UNCHECKED);
			return TRUE;
		}
		case WM_COMMAND:
			switch (LOWORD(w))
			{
			case IDOK:
			{
				// 读方向单选：勾了"水平"则测 Y，否则测 X
				g_result.measureX =
					(IsDlgButtonChecked(h, IDC_DIR_HORIZ) != BST_CHECKED);
				WCHAR t[32];
				GetWindowTextW(GetDlgItem(h, IDC_SPACING), t, 32);
				const double v = _wtof(t);
				if (v < kSpacingMin || v > kSpacingMax)
				{
					MessageBoxW(h, L"排列间隔必须是 0.5~200 的数值（mm）",
						L"坐标标注参数", MB_ICONWARNING | MB_OK);
					return TRUE;
				}
				g_result.spacing = v;
				EndDialog(h, 1);
				return TRUE;
			}
			case IDCANCEL:
				EndDialog(h, 0);
				return TRUE;
			}
			break;
		case WM_CLOSE:
			EndDialog(h, 0);
			return TRUE;
		}
		return FALSE;
	}

	// 返回 true=用户确定，结果写入 out
	bool Show(double defSpacing, Result& out)
	{
		g_defSpacing = defSpacing;

		Buf b;
		// DLGTEMPLATE
		b.DW(WS_POPUP | WS_VISIBLE | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME);
		b.DW(0);
		b.W(6);   // 控件总数：方向单选x2 + 标签 + 编辑框 + 确定/取消
		b.W(0); b.W(0); b.W(260); b.W(82);
		b.W(0);                         // 无菜单
		b.W(0);                         // 默认类
		b.Str(L"坐标标注参数设置");

		// 方向单选按钮组（BS_AUTORADIOBUTTON 自动互斥；首个带 WS_GROUP 划定分组）
		AddItem(b, WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_GROUP | BS_AUTORADIOBUTTON,
			8, 8, 200, 10, IDC_DIR_VERT, 0x0080, L"垂直坐标标注（测 X 坐标，左侧竖排）");
		AddItem(b, WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON,
			8, 22, 200, 10, IDC_DIR_HORIZ, 0x0080, L"水平坐标标注（测 Y 坐标，下方横排）");

		// 排列间隔
		AddItem(b, WS_CHILD | WS_VISIBLE | SS_LEFT,
			8, 40, 92, 10, 9000, 0x0082, L"排列间隔 (mm):");
		AddItem(b, WS_CHILD | WS_VISIBLE | WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL,
			104, 38, 60, 12, IDC_SPACING, 0x0081, L"");

		// 确定/取消
		AddItem(b, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
			56, 58, 40, 14, IDOK, 0x0080, L"确定");
		AddItem(b, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
			106, 58, 40, 14, IDCANCEL, 0x0080, L"取消");

		HWND parent = GetActiveWindow();
		INT_PTR r = DialogBoxIndirectW(NULL, (LPCDLGTEMPLATEW)(void*)b.d, parent, DlgProc);
		if (r == 1) { out = g_result; return true; }
		return false;
	}
} // namespace OrdDlg

//==============================================================================
// phase_pick_ordinate_dims —— 交互式坐标标注（v2 主流程）
// 流程：幂等警示 → 光标视图切 0 → 多选投影线段 → 取每条线段两端点并去重
//       → UF_UI_point_construct（推断点，带捕捉）拾取原点 → 视图曲线中吸附
//       最近特征点作基准 → 对话框（方向/间隔）→ 端点按坐标值升序排序 →
//       录制 VB 同款链式逐条 Commit（首条产基准标注 + 链式 margin）。
//==============================================================================
void phase_pick_ordinate_dims(NXOpen::Part* part,
	NXOpen::Drawings::DraftingView* sectionView)
{
	if (!sectionView)
	{
		CommonUtils::print_msg("[Step3] 坐标标注已跳过（无剖视图）");
		return;
	}

	char fmt[512];

	// ---- 幂等警示（v2 不再拦截）：本视图已有坐标标注时打印数量告警 ----
	// 背景（任务#24）：重复运行会使 UF_DRAW_ask_view_borders 把已有坐标标注
	// 计入视图边界导致边界膨胀；交互模式用户有意追加标注（分批次补标），
	// 因此只告警不跳过。关联信息不可得（GetAssociativeOrigin 抛异常）的
	// 标注不计数。
	{
		int existOrd = 0, unlinked = 0;
		for (NXOpen::Annotations::DimensionCollection::iterator dit = part->Dimensions()->begin();
			dit != part->Dimensions()->end(); ++dit)
		{
			NXOpen::Annotations::Dimension* d = *dit;
			if (!d) continue;
			int t = 0, s = 0;
			UF_OBJ_ask_type_and_subtype(d->Tag(), &t, &s);
			if (!(t == UF_dimension_type &&
				(s == UF_dim_ordinate_horiz_subtype || s == UF_dim_ordinate_vert_subtype ||
				 s == UF_dim_ordinate_origin_subtype)))
				continue;
			bool onView = false;
			try
			{
				NXOpen::Point3d originPt(0.0, 0.0, 0.0);
				NXOpen::Annotations::Annotation::AssociativeOriginData od =
					d->GetAssociativeOrigin(&originPt);
				onView = (od.View && od.View->Tag() == sectionView->Tag()) ||
				         (od.ViewOfGeometry && od.ViewOfGeometry->Tag() == sectionView->Tag());
			}
			catch (...) { ++unlinked; }
			if (onView) ++existOrd;
		}
		if (existOrd > 0)
		{
			sprintf_s(fmt, sizeof(fmt),
				"[Step3] 提示: 当前剖视图已有坐标标注 %d 条，本次将追加（重复标注请自查；%d 条归属不明）",
				existOrd, unlinked);
			CommonUtils::print_msg(fmt);
		}
	}

	// ---- 1. 多选投影线段（视图相关几何，须切"任意视图"） ----
	// 坑（实测）：不带掩码的 SelectTaggedObjects 会继承 NX 会话 Class Selection
	// 上一次残留的过滤器（停在"视图/尺寸"上，选不中曲线）；改用 MaskTriple
	// 重载 + ClearAndEnableSpecific 强制只允许曲线/边：截面边为
	// line/circle/conic/spline（UF_*_type），模型边为 UF_solid_type + ANY_EDGE。
	int oldCursorView = 1;
	UF_UI_ask_cursor_view(&oldCursorView);
	UF_UI_set_cursor_view(0);

	std::vector<NXOpen::Selection::MaskTriple> masks;
	masks.push_back(NXOpen::Selection::MaskTriple(UF_line_type, UF_all_subtype, 0));
	masks.push_back(NXOpen::Selection::MaskTriple(UF_circle_type, UF_all_subtype, 0));
	masks.push_back(NXOpen::Selection::MaskTriple(UF_conic_type, UF_all_subtype, 0));
	masks.push_back(NXOpen::Selection::MaskTriple(UF_spline_type, UF_all_subtype, 0));
	masks.push_back(NXOpen::Selection::MaskTriple(UF_solid_type, UF_all_subtype, UF_UI_SEL_FEATURE_ANY_EDGE));

	std::vector<NXOpen::TaggedObject*> picked;
	NXOpen::Selection::Response rsp =
		CommonUtils::get_ui()->SelectionManager()->SelectTaggedObjects(
			"选择剖视图内的投影轮廓线段（逐条单击，MB2/确定结束）", "Step3 坐标标注",
			NXOpen::Selection::SelectionScopeWorkPart,
			NXOpen::Selection::SelectionActionClearAndEnableSpecific,
			false, true, masks, picked);

	UF_UI_set_cursor_view(oldCursorView);

	if (rsp != NXOpen::Selection::ResponseOk &&
		rsp != NXOpen::Selection::ResponseObjectSelected &&
		rsp != NXOpen::Selection::ResponseBack)
	{
		CommonUtils::print_msg("[Step3] 已跳过坐标标注（未选择线段）");
		return;
	}

	// ---- 2. 取每条选中线段两端点（parm=0/1），0.01 容差去重 ----
	struct EndPt
	{
		NXOpen::DisplayableObject* disp;
		NXOpen::Point3d pt;
		NXOpen::InferSnapType::SnapType snap;
		int hostType;      // UF type（去重时优先保留制图曲线）
		int hostSubtype;
	};
	std::vector<EndPt> endPts;
	int segCount = 0, propFail = 0, dupMerged = 0;
	for (size_t i = 0; i < picked.size(); ++i)
	{
		NXOpen::DisplayableObject* disp =
			dynamic_cast<NXOpen::DisplayableObject*>(picked[i]);
		if (!disp)
		{
			sprintf_s(fmt, sizeof(fmt),
				"  警告: 所选对象 #%d 非显示对象，已跳过", (int)i);
			CommonUtils::print_msg(fmt);
			continue;
		}
		++segCount;
		int t = 0, s = 0;
		UF_OBJ_ask_type_and_subtype(disp->Tag(), &t, &s);

		double pt0[3] = { 0,0,0 }, pt1[3] = { 0,0,0 };
		bool ok0 = false, ok1 = false;
		for (int e = 0; e < 2; ++e)
		{
			double pt[3], tg[3], pn[3], bn[3], torsion = 0.0, roc = 0.0;
			const double parm = (e == 0) ? 0.0 : 1.0;
			if (UF_MODL_ask_curve_props(disp->Tag(), parm, pt, tg, pn, bn, &torsion, &roc) != 0)
			{
				++propFail;
				continue;
			}
			if (e == 0) { memcpy(pt0, pt, sizeof(pt0)); ok0 = true; }
			else        { memcpy(pt1, pt, sizeof(pt1)); ok1 = true; }
		}
		// 封闭曲线（整圆等）：parm=0 与 parm=1 是同一点，只标注一次
		if (ok0 && ok1)
		{
			const double dx = pt0[0] - pt1[0], dy = pt0[1] - pt1[1];
			if (dx * dx + dy * dy < kDupTol * kDupTol) ok1 = false;
		}
		for (int e = 0; e < 2; ++e)
		{
			if (e == 0 && !ok0) continue;
			if (e == 1 && !ok1) continue;
			const double* p = (e == 0) ? pt0 : pt1;
			int dupIdx = -1;
			for (size_t k = 0; k < endPts.size(); ++k)
			{
				const double dx = endPts[k].pt.X - p[0];
				const double dy = endPts[k].pt.Y - p[1];
				if (dx * dx + dy * dy < kDupTol * kDupTol) { dupIdx = (int)k; break; }
			}
			if (dupIdx >= 0)
			{
				// 同一几何位置若先收录了实体边、后遇到制图曲线，则换成制图曲线
				//（坐标标注的被测关联对象在录制 VB 中全部是 DraftingCurve）
				const bool oldSolid = (endPts[dupIdx].hostType == UF_solid_type &&
					endPts[dupIdx].hostSubtype == UF_solid_edge_subtype);
				const bool newSolid = (t == UF_solid_type && s == UF_solid_edge_subtype);
				if (oldSolid && !newSolid)
				{
					endPts[dupIdx].disp = disp;
					endPts[dupIdx].hostType = t;
					endPts[dupIdx].hostSubtype = s;
					endPts[dupIdx].snap = (e == 0) ? NXOpen::InferSnapType::SnapTypeStart
						: NXOpen::InferSnapType::SnapTypeEnd;
				}
				++dupMerged;
				continue;
			}
			EndPt ep;
			ep.disp = disp;
			ep.pt = NXOpen::Point3d(p[0], p[1], p[2]);
			ep.snap = (e == 0) ? NXOpen::InferSnapType::SnapTypeStart
				: NXOpen::InferSnapType::SnapTypeEnd;
			ep.hostType = t;
			ep.hostSubtype = s;
			endPts.push_back(ep);
		}
	}
	sprintf_s(fmt, sizeof(fmt),
		"[Step3] 已选择线段 %d 条，端点去重后 %d 个（合并 %d 次，属性读取失败 %d 次）",
		segCount, (int)endPts.size(), dupMerged, propFail);
	CommonUtils::print_msg(fmt);
	if (endPts.empty())
	{
		CommonUtils::print_msg("[Step3] 已跳过坐标标注（所选线段未取到有效端点）");
		return;
	}

	// ---- 3. 带捕捉点选坐标原点（UF_UI_point_construct 推断点模式） ----
	// 推断点模式可捕捉曲线端点/中点/圆心等；生成的临时关联点随后删除。
	// 注意：NXOpen::Point 继承 SmartObject 而非 DisplayableObject，不能直接
	// 作 OrdinateOrigin 的关联对象，故只用其坐标到视图曲线中吸附特征点。
	char cue[128] = "拾取坐标原点（捕捉端点/中点/圆心，MB1 确定）";
	UF_UI_POINT_base_method_t method = UF_UI_POINT_INFERRED;
	tag_t ptTag = NULL_TAG;
	double originPt[3] = { 0.0, 0.0, 0.0 };
	int resp = UF_UI_CANCEL;
	int rc = UF_UI_point_construct(cue, &method, &ptTag, originPt, &resp);
	if (rc != 0 || resp != UF_UI_OK || ptTag == NULL_TAG)
	{
		CommonUtils::print_msg("[Step3] 已跳过坐标标注（未拾取坐标原点）");
		return;
	}
	sprintf_s(fmt, sizeof(fmt),
		"[Step3] 原点拾取: 坐标=(%.3f, %.3f, %.3f)，捕捉方法=%d",
		originPt[0], originPt[1], originPt[2], (int)method);
	CommonUtils::print_msg(fmt);
	// 删除临时关联点（不可见；删除失败仅告警，不影响标注）
	if (UF_OBJ_delete_object(ptTag) != 0)
	{
		sprintf_s(fmt, sizeof(fmt),
			"  警告: 原点临时关联点 tag=%llu 删除失败（点不可见，无影响）",
			(unsigned long long)ptTag);
		CommonUtils::print_msg(fmt);
	}

	// ---- 4. 在视图曲线中吸附原点特征点（最近端点/中点/圆心，容差 kOriginTol） ----
	const std::vector<CommonUtils::CurveInfo> curves =
		CommonUtils::enumerate_view_curves(sectionView);

	NXOpen::DisplayableObject* originCurve = NULL;
	NXOpen::Point3d originFeaturePt(0.0, 0.0, 0.0);
	NXOpen::InferSnapType::SnapType originSnap = NXOpen::InferSnapType::SnapTypeMid;
	double bestDist = kOriginTol;
	for (size_t i = 0; i < curves.size(); ++i)
	{
		const CommonUtils::CurveInfo& ci = curves[i];
		if (!ci.has_props) continue;
		struct Feat { const double* p; NXOpen::InferSnapType::SnapType snap; };
		const double midPt[3] = {
			(ci.start_pt[0] + ci.end_pt[0]) / 2.0,
			(ci.start_pt[1] + ci.end_pt[1]) / 2.0,
			(ci.start_pt[2] + ci.end_pt[2]) / 2.0 };
		Feat feats[4] = {
			{ ci.start_pt,   NXOpen::InferSnapType::SnapTypeStart  },
			{ ci.end_pt,     NXOpen::InferSnapType::SnapTypeEnd    },
			{ midPt,         NXOpen::InferSnapType::SnapTypeMid    },
			{ ci.arc_center, NXOpen::InferSnapType::SnapTypeCenter }   // 仅圆/圆弧有效
		};
		const int nFeat = (ci.type == UF_circle_type) ? 4 : 3;
		for (int f = 0; f < nFeat; ++f)
		{
			const double dx = feats[f].p[0] - originPt[0];
			const double dy = feats[f].p[1] - originPt[1];
			const double d = sqrt(dx * dx + dy * dy);
			if (d < bestDist)
			{
				NXOpen::DisplayableObject* disp = dynamic_cast<NXOpen::DisplayableObject*>(
					NXOpen::NXObjectManager::Get(ci.tag));
				if (!disp) continue;   // 非显示对象不作基准宿主
				bestDist = d;
				originCurve = disp;
				originFeaturePt = NXOpen::Point3d(feats[f].p[0], feats[f].p[1], feats[f].p[2]);
				originSnap = feats[f].snap;
			}
		}
	}
	if (!originCurve)
	{
		sprintf_s(fmt, sizeof(fmt),
			"[Step3] 已跳过坐标标注（原点未捕捉到剖视图内几何: 拾取点距最近曲线特征点 %.3f mm，容差 %.2f mm）",
			bestDist, kOriginTol);
		CommonUtils::print_msg(fmt);
		return;
	}
	{
		int ot = 0, os = 0;
		UF_OBJ_ask_type_and_subtype(originCurve->Tag(), &ot, &os);
		const char* snapName = "Mid";
		if (originSnap == NXOpen::InferSnapType::SnapTypeStart) snapName = "Start";
		else if (originSnap == NXOpen::InferSnapType::SnapTypeEnd) snapName = "End";
		else if (originSnap == NXOpen::InferSnapType::SnapTypeCenter) snapName = "Center";
		sprintf_s(fmt, sizeof(fmt),
			"[Step3] 原点已吸附: 曲线 tag=%llu type=%d subtype=%d snap=%s 特征点=(%.3f, %.3f) 距离=%.4f mm",
			(unsigned long long)originCurve->Tag(), ot, os, snapName,
			originFeaturePt.X, originFeaturePt.Y, bestDist);
		CommonUtils::print_msg(fmt);
	}

	// ---- 5. 对话框：测量方向 + 排列间隔 ----
	OrdDlg::Result res;
	if (!OrdDlg::Show(kDefaultSpacing, res))
	{
		CommonUtils::print_msg("[Step3] 已跳过坐标标注（用户取消对话框）");
		return;
	}
	const bool measureX = res.measureX;   // true=测 X（垂直坐标标注）；false=测 Y（水平坐标标注）
	const double spacing = res.spacing;

	// ---- 6. 过滤被测端点并按坐标值升序排序 ----
	// 任务#17 教训：被测点不能落在基准曲线自身上（NX 会推断出零长度尺寸）；
	// 与原点重合的点也不标注。
	std::vector<EndPt> meas;
	for (size_t i = 0; i < endPts.size(); ++i)
	{
		if (endPts[i].disp == originCurve) continue;
		const double dx = endPts[i].pt.X - originFeaturePt.X;
		const double dy = endPts[i].pt.Y - originFeaturePt.Y;
		if (dx * dx + dy * dy < kOriginTol * kOriginTol) continue;
		meas.push_back(endPts[i]);
	}
	if (meas.empty())
	{
		CommonUtils::print_msg("[Step3] 已跳过坐标标注（除基准曲线外无可用被测端点）");
		return;
	}
	std::stable_sort(meas.begin(), meas.end(), [&](const EndPt& a, const EndPt& b) {
		if (measureX) return a.pt.X < b.pt.X;
		return a.pt.Y < b.pt.Y;
	});
	{
		const char* dirName = measureX ? "X" : "Y";
		sprintf_s(fmt, sizeof(fmt),
			"[Step3] 测量方向=%s ，被测端点 %d 个（按坐标值升序）:",
			dirName, (int)meas.size());
		CommonUtils::print_msg(fmt);
		for (size_t i = 0; i < meas.size(); ++i)
		{
			sprintf_s(fmt, sizeof(fmt),
				"    #%d (X=%.3f, Y=%.3f) snap=%s 承载tag=%llu",
				(int)i, meas[i].pt.X, meas[i].pt.Y,
				(meas[i].snap == NXOpen::InferSnapType::SnapTypeEnd) ? "End" : "Start",
				(unsigned long long)meas[i].disp->Tag());
			CommonUtils::print_msg(fmt);
		}
	}

	// ---- 7. 视图边界与 margin / 放置位置（图纸坐标） ----
	double b[4] = { 0.0, 0.0, 0.0, 0.0 };
	int rcB = UF_DRAW_ask_view_borders(sectionView->Tag(), b);
	bool borderOk = (rcB == 0);
	NXOpen::Point3d hMargin(0.0, 0.0, 0.0);   // 水平组 margin：Y = 视图下缘-12
	NXOpen::Point3d vMargin(0.0, 0.0, 0.0);   // 垂直组 margin：X = 视图左缘-12
	if (borderOk)
	{
		hMargin = NXOpen::Point3d((b[0] + b[2]) / 2.0, b[1] - 12.0, 0.0);
		vMargin = NXOpen::Point3d(b[0] - 12.0, (b[1] + b[3]) / 2.0, 0.0);
	}
	else
	{
		sprintf_s(fmt, sizeof(fmt),
			"  警告: UF_DRAW_ask_view_borders 返回 %d ，margin 位置降级为默认", rcB);
		CommonUtils::print_msg(fmt);
	}
	// 显式放置原点（任务#24，图纸坐标，仿 VB L1527/L1817）：
	// 测X（垂直坐标标注）：首条在视图内近左缘，标签沿 Y 步进（间隔=用户输入）；
	// 测Y（水平坐标标注）：首条在视图内近下缘，标签沿 X 步进。
	NXOpen::Point3d firstOrigin(0.0, 0.0, 0.0);
	NXOpen::Point3d originStep(0.0, 0.0, 0.0);
	bool explicitOrigin = false;
	if (borderOk)
	{
		if (measureX)
		{
			firstOrigin = NXOpen::Point3d(b[0] + 6.0, (b[1] + b[3]) / 2.0, 0.0);
			originStep = NXOpen::Point3d(0.0, spacing, 0.0);
		}
		else
		{
			firstOrigin = NXOpen::Point3d((b[0] + b[2]) / 2.0, b[1] + 6.0, 0.0);
			originStep = NXOpen::Point3d(spacing, 0.0, 0.0);
		}
		explicitOrigin = true;
		sprintf_s(fmt, sizeof(fmt),
			"[Step3] 显式放置: 首条原点=(%.3f, %.3f) 步进=(%.1f, %.1f)（图纸坐标）",
			firstOrigin.X, firstOrigin.Y, originStep.X, originStep.Y);
		CommonUtils::print_msg(fmt);
	}

	// ---- 8. 录制 VB 忠实逐条链式生成（单方向） ----
	// 录制宏 000R_VB.vb 成功模式（L1364/L1434/L1527/L1564/L1614/L1722/L1817）：
	//   首条：OrdinateOrigin.SetValue(SnapType, 基准曲线, 视图, 特征点, NULL, NULL,
	//         (0,0,0)) + SecondAssociativities.SetValue(snap, 端点曲线, 视图, 端点,
	//         ...) + 显式放置原点 → Commit 产出 OrdinateOriginDimension（ENTITY 26 6）；
	//   Commit 后 OrdinateMargins.CreateInferredMargin 建链式 margin，再
	//   SetAssociativity(3) 关联回首条标注；
	//   后续：OrdinateOrigin.SetValue(基准标注) + Active*Margin = 已建 margin +
	//   显式原点按间隔步进，每条标注独立 Commit。
	const char* dirName = measureX ? "测X(垂直)" : "测Y(水平)";
	const int marginSubtype = measureX
		? UF_dim_ordinate_vert_subtype   // 14：垂直坐标标注（测 X）
		: UF_dim_ordinate_horiz_subtype; // 13：水平坐标标注（测 Y）
	const NXOpen::Point3d& marginPt = measureX ? vMargin : hMargin;

	// 降级扫描基准：记录 Commit 前已有 OrdinateOriginDimension 的最大 tag，
	// 首条 Commit 后若 GetCommittedObjects 中找不到，可在 part->Dimensions()
	// 中按 tag 增量定位新创建的基准标注。
	tag_t maxOodTag = 0;
	for (NXOpen::Annotations::DimensionCollection::iterator dit = part->Dimensions()->begin();
		dit != part->Dimensions()->end(); ++dit)
	{
		NXOpen::Annotations::OrdinateOriginDimension* d =
			dynamic_cast<NXOpen::Annotations::OrdinateOriginDimension*>(*dit);
		if (d && d->Tag() > maxOodTag) maxOodTag = d->Tag();
	}

	int created = 0;
	int failed = 0;
	NXOpen::Annotations::OrdinateOriginDimension* originDim = NULL;
	NXOpen::Annotations::Dimension* fallbackDim = NULL;   // 链式容错：任意 Dimension 作基准
	NXOpen::Annotations::OrdinateMargin* activeMargin = NULL;

	for (size_t i = 0; i < meas.size(); ++i)
	{
		NXOpen::Annotations::OrdinateDimensionBuilder* ob = NULL;
		try
		{
			ob = part->Dimensions()->CreateOrdinateDimensionBuilder(NULL);
			// ---- VB 忠实初始化（000R_VB.vb L1210/L1214/L1230/L1632/L1636/L1650）----
			ob->Baseline()->SetActivateBaseline(true);
			ob->Origin()->SetAnchor(NXOpen::Annotations::OriginBuilder::AlignmentPositionMidCenter);
			ob->Origin()->Plane()->SetPlaneMethod(
				NXOpen::Annotations::PlaneBuilder::PlaneMethodTypeXyPlane);
			ob->Origin()->SetInferRelativeToGeometry(false);
			ob->Style()->DimensionStyle()->SetTextCentered(false);
			if (i == 0)
			{
				// 基准 = 原点吸附的曲线特征点（VB L1364 同款 7 参 SetValue）
				ob->OrdinateOrigin()->SetValue(originSnap,
					originCurve, sectionView, originFeaturePt,
					NULL, NULL, NXOpen::Point3d(0.0, 0.0, 0.0));
			}
			else
			{
				if (originDim)         ob->OrdinateOrigin()->SetValue(originDim);     // VB L1614
				else if (fallbackDim)  ob->OrdinateOrigin()->SetValue(fallbackDim);   // 容错：普通 Dimension
				else break;   // 基准标注缺失则无法继续继承
				// VB L1722：后续标注继承首条 Commit 后创建的链式 margin
				if (activeMargin)
				{
					if (measureX) ob->SetActiveVerticalMargin(activeMargin);
					else          ob->SetActiveHorizontalMargin(activeMargin);
				}
			}
			// 被测点：SecondAssociativities（每 Builder 仅 1 个，VB L1434/L1764）
			ob->SecondAssociativities()->SetValue(meas[i].snap,
				meas[i].disp, sectionView, meas[i].pt,
				NULL, NULL, NXOpen::Point3d(0.0, 0.0, 0.0));
			if (measureX) ob->SetVerticalInferredMarginLocation(vMargin);
			else          ob->SetHorizontalInferredMarginLocation(hMargin);
			// VB L1534：LeaderOrientation = Left
			ob->Style()->LineArrowStyle()->SetLeaderOrientation(
				NXOpen::Annotations::LeaderSideLeft);
			ob->Origin()->AnnotationView()->SetValue(sectionView);
			// ---- 显式放置原点（VB L1506-1529 / L1796-1817）----
			// 关键：先 SetAssociativeOrigin(Drag)（OriginType=Drag、其余字段全空），
			// 再 Origin.Origin.SetValue(NULL, NULL, pt)，否则显式原点不生效。
			if (explicitOrigin)
			{
				const NXOpen::Point3d placePt(
					firstOrigin.X + originStep.X * (double)i,
					firstOrigin.Y + originStep.Y * (double)i,
					0.0);
				NXOpen::Annotations::Annotation::AssociativeOriginData ao;   // 默认构造全空
				ao.OriginType = NXOpen::Annotations::AssociativeOriginTypeDrag;
				ob->Origin()->SetAssociativeOrigin(ao);
				ob->Origin()->Origin()->SetValue(NULL, NULL, placePt);
				ob->Origin()->SetInferRelativeToGeometry(false);   // VB L1212/L1529
			}
			// VB L1552-1553：Commit 返回值即首条坐标标注
			NXOpen::NXObject* commitObj = NULL;
			if (i == 0) commitObj = ob->Commit();
			else        ob->Commit();
			if (i == 0)
			{
				// 诊断：打印首条 Commit 全部产物的 UF 类型与类名，
				// 定位 NX 实际推断出的标注类型（任务#17）
				std::vector<NXOpen::NXObject*> objs = ob->GetCommittedObjects();
				sprintf_s(fmt, sizeof(fmt),
					"[%sVB 诊断] 首条 Commit 产物 %d 个:",
					dirName, (int)objs.size());
				CommonUtils::print_msg(fmt);
				for (size_t k = 0; k < objs.size(); ++k)
				{
					NXOpen::NXObject* o = objs[k];
					if (!o) continue;
					int t = 0, s = 0;
					UF_OBJ_ask_type_and_subtype(o->Tag(), &t, &s);
					const char* cls = "未知类型";
					NXOpen::Annotations::OrdinateOriginDimension* ood =
						dynamic_cast<NXOpen::Annotations::OrdinateOriginDimension*>(o);
					if (ood) cls = "OrdinateOriginDimension";
					else if (dynamic_cast<NXOpen::Annotations::Dimension*>(o))
						cls = "Dimension(其它子类)";
					sprintf_s(fmt, sizeof(fmt),
						"    #%d tag=%llu type=%d subtype=%d class=%s",
						(int)k, (unsigned long long)o->Tag(), t, s, cls);
					CommonUtils::print_msg(fmt);
					if (!originDim) originDim = ood;
					if (!fallbackDim) fallbackDim = dynamic_cast<NXOpen::Annotations::Dimension*>(o);
				}
				// VB L1553：Commit 返回值本身就是首条坐标标注
				if (!fallbackDim && commitObj)
					fallbackDim = dynamic_cast<NXOpen::Annotations::Dimension*>(commitObj);
				if (!originDim)
				{
					// 降级 1：在 part->Dimensions() 中按 tag 增量定位新创建的基准标注
					for (NXOpen::Annotations::DimensionCollection::iterator dit = part->Dimensions()->begin();
						dit != part->Dimensions()->end() && !originDim; ++dit)
					{
						NXOpen::Annotations::OrdinateOriginDimension* d =
							dynamic_cast<NXOpen::Annotations::OrdinateOriginDimension*>(*dit);
						if (d && d->Tag() > maxOodTag)
						{
							originDim = d;
							sprintf_s(fmt, sizeof(fmt),
								"[%sVB 诊断] 经 Dimensions() 扫描定位新基准标注 tag=%llu",
								dirName, (unsigned long long)d->Tag());
							CommonUtils::print_msg(fmt);
						}
					}
					if (!originDim && fallbackDim)
					{
						sprintf_s(fmt, sizeof(fmt),
							"  警告: %sVB 模式：首条产物非 OrdinateOriginDimension，降级用普通 Dimension(tag=%llu) 作链式基准",
							dirName, (unsigned long long)fallbackDim->Tag());
						CommonUtils::print_msg(fmt);
					}
					else if (!originDim)
					{
						sprintf_s(fmt, sizeof(fmt),
							"  警告: %sVB 模式：首条 Commit 后未获取任何可作基准的 Dimension", dirName);
						CommonUtils::print_msg(fmt);
					}
				}
				// VB L1562-1564：首条产出 OrdinateOriginDimension 后，用
				// OrdinateMargins.CreateInferredMargin 创建链式 margin，
				// 供后续 builder 的 ActiveVertical/HorizontalMargin 继承。
				if (originDim && !activeMargin)
				{
					try
					{
						activeMargin = part->Annotations()->OrdinateMargins()->
							CreateInferredMargin(originDim, marginPt, marginSubtype);
						if (activeMargin)
						{
							sprintf_s(fmt, sizeof(fmt),
								"[%sVB 诊断] 已创建链式 margin tag=%llu 位置=(%.3f, %.3f)",
								dirName, (unsigned long long)activeMargin->Tag(),
								marginPt.X, marginPt.Y);
							CommonUtils::print_msg(fmt);
						}
					}
					catch (const NXOpen::NXException& em)
					{
						sprintf_s(fmt, sizeof(fmt),
							"  警告: %sVB 模式：链式 margin 创建失败（后续降级为推断放置）: %s",
							dirName, em.Message());
						CommonUtils::print_msg(fmt);
					}
					catch (...)
					{
						sprintf_s(fmt, sizeof(fmt),
							"  警告: %sVB 模式：链式 margin 创建失败（未知异常）", dirName);
						CommonUtils::print_msg(fmt);
					}
				}
				// VB L1566-1597：把新创建的 margin 关联到首条坐标标注
				// （SetAssociativity(3, assoc) + LogForUpdate + DoUpdate），
				// 否则链式标注无法正确挂接，后续 Active*Margin 可能无效。
				if (activeMargin && commitObj)
				{
					try
					{
						NXOpen::Annotations::Associativity* assoc =
							part->Annotations()->NewAssociativity();
						assoc->SetFirstObject(activeMargin);
						assoc->SetSecondObject(NULL);
						assoc->SetObjectView(NULL);
						assoc->SetPointOption(NXOpen::Annotations::AssociativityPointOptionNone);
						assoc->SetLineOption(NXOpen::Annotations::AssociativityLineOptionNone);
						assoc->SetFirstDefinitionPoint(NXOpen::Point3d(0.0, 0.0, 0.0));
						assoc->SetSecondDefinitionPoint(NXOpen::Point3d(0.0, 0.0, 0.0));
						assoc->SetAngle(0.0);
						assoc->SetPickPoint(NXOpen::Point3d(0.0, 0.0, 0.0));
						NXOpen::Annotations::Dimension* dim =
							dynamic_cast<NXOpen::Annotations::Dimension*>(commitObj);
						if (dim)
						{
							dim->SetAssociativity(3, assoc);
							CommonUtils::get_session()->UpdateManager()->LogForUpdate(dim);
							// VB L1599-1600：提交后立即 DoUpdate 一次，让 margin 关联生效
							NXOpen::Session::UndoMarkId um = CommonUtils::get_session()->SetUndoMark(
								NXOpen::Session::MarkVisibilityInvisible, "坐标尺寸 margin 关联");
							CommonUtils::get_session()->UpdateManager()->DoUpdate(um);
							sprintf_s(fmt, sizeof(fmt),
								"[%sVB 诊断] margin tag=%llu 已关联到首条标注 tag=%llu",
								dirName, (unsigned long long)activeMargin->Tag(),
								(unsigned long long)dim->Tag());
							CommonUtils::print_msg(fmt);
						}
						// 注：C++ 的 Associativity 无 Dispose（VB 才有），对象由 NX 会话管理
					}
					catch (const NXOpen::NXException& em2)
					{
						sprintf_s(fmt, sizeof(fmt),
							"  警告: %sVB 模式：margin 关联到标注失败: %s",
							dirName, em2.Message());
						CommonUtils::print_msg(fmt);
					}
					catch (...)
					{
						sprintf_s(fmt, sizeof(fmt),
							"  警告: %sVB 模式：margin 关联到标注失败（未知异常）", dirName);
						CommonUtils::print_msg(fmt);
					}
				}
			}
			ob->Destroy();
			ob = NULL;
			++created;
		}
		catch (const NXOpen::NXException& e)
		{
			if (ob) { ob->Destroy(); ob = NULL; }
			++failed;
			if (failed <= 3)
			{
				sprintf_s(fmt, sizeof(fmt),
					"  警告: %sVB 逐条 #%d 失败: %s",
					dirName, (int)i, e.Message());
				CommonUtils::print_msg(fmt);
			}
			if (i == 0) break;   // 基准标注都建不出，后续无意义
		}
		catch (...)
		{
			if (ob) { ob->Destroy(); ob = NULL; }
			++failed;
			if (i == 0) break;
		}
	}
	sprintf_s(fmt, sizeof(fmt),
		"[Step3] %s VB 逐条模式: 成功 %d / 失败 %d (链式基准=%s ，间隔=%.1f mm)",
		dirName, created, failed,
		originDim ? "OrdinateOriginDimension" : (fallbackDim ? "普通Dimension" : "无"),
		spacing);
	CommonUtils::print_msg(fmt);

	// 刷新显示
	CommonUtils::silent_update(CommonUtils::get_session());
}

//==============================================================================
// do_it —— Step3 入口逻辑
//==============================================================================
void do_it()
{
	try
	{
		NXOpen::Part* part = dynamic_cast<NXOpen::Part*>(CommonUtils::get_session()->Parts()->BaseWork());
		if (!part) { CommonUtils::print_msg("错误：无工作部件"); return; }

		// ===== 进入制图环境 =====
		CommonUtils::get_session()->ApplicationSwitchImmediate("UG_APP_DRAFTING");
		part->Drafting()->EnterDraftingApplication();
		CommonUtils::print_msg("[Step3] 已进入制图模块");

		// ===== 当前活动图纸（v2：只读，绝不 Open/切换图纸）=====
		// 用户要求 Step3 不得切换工作活动图纸：主路径 CurrentDrawingSheet()；
		// 为空再走 UF_DRAW_ask_current_drawing 兜底（同样只读）。找不到就提示
		// 用户手动激活，不做任何切换操作。
		NXOpen::Drawings::DraftingDrawingSheet* sheet =
			part->DraftingDrawingSheets()->CurrentDrawingSheet();
		if (!sheet)
		{
			tag_t drawTag = NULL_TAG;
			if (UF_DRAW_ask_current_drawing(&drawTag) == 0 && drawTag != NULL_TAG)
			{
				NXOpen::TaggedObject* tobj = NXOpen::NXObjectManager::Get(drawTag);
				sheet = dynamic_cast<NXOpen::Drawings::DraftingDrawingSheet*>(tobj);
			}
		}
		if (!sheet)
		{
			CommonUtils::print_msg("[Step3] 未找到活动图纸：Step3 不切换图纸，请先在 NX 中激活目标图纸再运行");
			return;
		}
		CommonUtils::print_msg("[Step3] 使用当前活动图纸（不切换）");

		// ---- 诊断：列举当前图纸上所有视图，并在其上找剖视图 ----
		NXOpen::Drawings::SectionView* sectionView = NULL;
		{
			std::vector<NXOpen::Drawings::DraftingView*> sheetViews = sheet->GetDraftingViews();
			const int nViews = (int)sheetViews.size();
			char fmt[256];
			sprintf_s(fmt, sizeof(fmt),
				"[Step3 视图诊断] 当前图纸 \"%s\" 上共有 %d 个视图:",
				sheet->Name().GetText(), nViews);
			CommonUtils::print_msg(fmt);
			for (size_t idx = 0; idx < sheetViews.size(); ++idx)
			{
				NXOpen::Drawings::DraftingView* v = sheetViews[idx];
				if (!v) continue;
				const char* vtype = "DraftingView";
				if (dynamic_cast<NXOpen::Drawings::SectionView*>(v))
				{
					vtype = "SectionView(剖视)";
					if (!sectionView) sectionView = dynamic_cast<NXOpen::Drawings::SectionView*>(v);
				}
				else if (dynamic_cast<NXOpen::Drawings::BaseView*>(v))
					vtype = "BaseView(基础/载体)";
				double vb[4] = { 0,0,0,0 };
				UF_DRAW_ask_view_borders(v->Tag(), vb);
				sprintf_s(fmt, sizeof(fmt),
					"  [%d] tag=%llu 类型=%s 边界(%.1f,%.1f)-(%.1f,%.1f)",
					(int)idx, (unsigned long long)v->Tag(), vtype, vb[0], vb[1], vb[2], vb[3]);
				CommonUtils::print_msg(fmt);
			}
		}
		if (!sectionView)
		{
			CommonUtils::print_msg("[Step3] 当前图纸没有剖视图：请激活包含剖视图的图纸后再运行（Step3 不切换图纸）");
			return;
		}
		// v2 起不再依赖 Step5 中心线：坐标原点由用户带捕捉点选
		phase_pick_ordinate_dims(part, sectionView);

		CommonUtils::print_msg("========== Step3 坐标标注 完成 ==========");
	}
	catch (const NXOpen::NXException& e) { CommonUtils::print_msg(std::string("NXException: ") + e.Message()); }
	catch (...) { CommonUtils::print_msg("Unknown Exception"); }
}

//==============================================================================
// ufusr —— DLL 入口点
//==============================================================================
extern "C" DllExport void ufusr(char *parm, int *returnCode, int rlen)
{
	try
	{
		do_it();
	}
	catch (const NXOpen::NXException& e1)
	{
		NXOpen::UI::GetUI()->NXMessageBox()->Show("NXException", NXOpen::NXMessageBox::DialogTypeError, e1.Message());
	}
	catch (const std::exception& e2)
	{
		NXOpen::UI::GetUI()->NXMessageBox()->Show("Exception", NXOpen::NXMessageBox::DialogTypeError, e2.what());
	}
	catch (...)
	{
		NXOpen::UI::GetUI()->NXMessageBox()->Show("Exception", NXOpen::NXMessageBox::DialogTypeError, "Unknown Exception.");
	}
}

//------------------------------------------------------------------------------
// Unload Handler
//------------------------------------------------------------------------------
extern "C" DllExport int ufusr_ask_unload()
{
	return (int)NXOpen::Session::LibraryUnloadOptionImmediately;
}
