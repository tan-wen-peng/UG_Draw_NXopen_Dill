//------------------------------------------------------------------------------
// NX12 Step3：坐标标注（DLL 3/6）
// 功能：自动枚举剖视图轮廓边端点，生成成组水平/垂直坐标标注
//------------------------------------------------------------------------------

// Win32 防护 + 共享头文件
#include "../Shared/NX12_CommonConfig.h"
#include "../Shared/NX12_CommonUtils.h"

// Step3 专有 includes
#include <NXOpen/SelectDisplayableObject.hxx>
#include <NXOpen/SelectDisplayableObjectList.hxx>
#include <NXOpen/Annotations_OrdinateDimensionBuilder.hxx>
#include <NXOpen/Annotations_OriginBuilder.hxx>
#include <NXOpen/DisplayableObject.hxx>
#include <NXOpen/NXMessageBox.hxx>
#include <NXOpen/Drawings_SelectDraftingView.hxx>
#include <NXOpen/Drawings_DraftingDrawingSheetCollection.hxx>
#include <NXOpen/Drawings_DraftingDrawingSheet.hxx>
#include <NXOpen/Drawings_DraftingViewCollection.hxx>
#include <NXOpen/Drawings_BaseView.hxx>
#include <NXOpen/DraftingManager.hxx>
#include <algorithm>   // std::min/std::max（候选基准曲线极值筛选）
#include <NXOpen/InferSnapType.hxx>                    // 录制 VB 同款 SetValue 需显式 snap 类型
#include <NXOpen/Annotations_BaseOrdinateDimensionBuilder.hxx>
#include <NXOpen/Annotations_OrdinateOriginDimension.hxx>  // 首条 Commit 产物，后续标注基准
#include <NXOpen/Annotations_StyleBuilder.hxx>            // Style()->LineArrowStyle()（VB L1534 LeaderOrientation）
#include <NXOpen/Annotations_LineArrowStyleBuilder.hxx>
#include <NXOpen/Annotations.hxx>                         // LeaderSide 枚举
#include <NXOpen/Annotations_Dimension.hxx>               // 首条产物类型诊断/链式容错
#include <NXOpen/Annotations_HorizontalDimension.hxx>     // 首条产物类型诊断
#include <NXOpen/Annotations_VerticalDimension.hxx>       // 首条产物类型诊断
#include <NXOpen/Annotations_DimensionCollection.hxx>     // part->Dimensions() 扫描降级
#include <NXOpen/Annotations_OrdinateMarginCollection.hxx>// VB L1564 CreateInferredMargin
#include <NXOpen/Annotations_OrdinateMargin.hxx>          // SetActiveVertical/HorizontalMargin 参数
#include <NXOpen/Annotations_AnnotationManager.hxx>       // part->Annotations()->OrdinateMargins() / NewAssociativity()
#include <NXOpen/Annotations_Associativity.hxx>           // VB L1566-1596 margin 关联链
#include <NXOpen/Annotations_Annotation.hxx>              // GetAssociativeOrigin / SetAssociativity / AssociativeOriginData
#include <uf_object_types.h>        // UF_dimension_type / UF_dim_ordinate_*_subtype（幂等扫描）

//==============================================================================
// phase_auto_contour_dims —— 自动轮廓坐标标注（自由函数）
// 从 MyClass 成员函数提取（原 L801-972）
// 完整保留：UF_VIEW_cycle_objects + UF_VIEW_DEPENDENT_OBJECTS 视图成员遍历、UF_MODL_ask_curve_props 端点提取、
// 0.01 容差去重、成组 OrdinateDimensionBuilder（水平+垂直）、失败降级单条模式
//==============================================================================
void phase_auto_contour_dims(NXOpen::Part* part,
	NXOpen::Drawings::DraftingView* sectionView,
	NXOpen::Annotations::Centerline2d* centerlineObj)
{
	if (!sectionView)
	{
		CommonUtils::print_msg("[Step3] 轮廓坐标标注已跳过（无剖视图）");
		return;
	}
	if (!centerlineObj)
	{
		CommonUtils::print_msg("[Step3] 轮廓坐标标注已跳过（中心线不可用，无法作为坐标基准）");
		return;
	}

	char fmt[512];

	// ---- 幂等保护（任务#24）：重复运行会使 UF_DRAW_ask_view_borders 把已有
	// 坐标标注计入视图边界，导致边界/放置位置逐级膨胀。检测到当前剖视图上
	// 已存在坐标标注（type=26 且 subtype 为 13/14/18，见 uf_object_types.h）则
	// 跳过创建；其它图纸/视图上的坐标标注不计入（避免跨图纸误拦）。
	// 关联信息不可得（GetAssociativeOrigin 抛异常）的标注不据此拦截。
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
			catch (...) { ++unlinked; }   // 关联信息不可得：不据此拦截
			if (onView) ++existOrd;
		}
		if (existOrd > 0)
		{
			sprintf_s(fmt, sizeof(fmt),
				"[Step3] 当前剖视图已存在坐标标注 %d 条，跳过创建（幂等保护；无法确认归属 %d 条不拦截）",
				existOrd, unlinked);
			CommonUtils::print_msg(fmt);
			return;
		}
	}

	// ---- 视图边界（图纸坐标 [Xmin,Ymin,Xmax,Ymax]）与 margin 位置 ----
	double b[4] = { 0.0, 0.0, 0.0, 0.0 };
	int rcB = UF_DRAW_ask_view_borders(sectionView->Tag(), b);
	bool borderOk = (rcB == 0);
	NXOpen::Point3d hMargin(0.0, 0.0, 0.0);   // 水平组 margin：Y = 视图下缘-12
	NXOpen::Point3d vMargin(0.0, 0.0, 0.0);   // 垂直组 margin：X = 视图左缘-12
	if (rcB == 0)
	{
		hMargin = NXOpen::Point3d((b[0] + b[2]) / 2.0, b[1] - 12.0, 0.0);
		vMargin = NXOpen::Point3d(b[0] - 12.0, (b[1] + b[3]) / 2.0, 0.0);
	}
	else
	{
		// 边界读取失败：降级用 0 坐标作 margin 位置（Commit 时 NX 自行推断）
		sprintf_s(fmt, sizeof(fmt),
			"  警告: UF_DRAW_ask_view_borders 返回 %d，坐标标注 margin 位置降级为默认", rcB);
		CommonUtils::print_msg(fmt);
	}

	// ---- 多策略枚举视图内曲线/边（含诊断日志） ----
	// CommonUtils::enumerate_view_curves 内部已含 silent_update + view->Update，
	// 依次尝试：A=UF_VIEW_cycle_objects / B=DraftingBody DraftingCurves /
	// C=UF_DRAW 截面边 API，按 tag 去重合并（截面边是视图 DraftingBody
	// 所属 DraftingCurve，策略A 遍历不到，主要依赖 B/C）
	const std::vector<CommonUtils::CurveInfo> curves =
		CommonUtils::enumerate_view_curves(sectionView);

	// ---- 诊断：中心线与首几条曲线的对象形态 ----
	// 背景：上一轮实测 Builder 报 "The first object associativity type is
	// invalid."（单条降级模式），需确认 OrdinateOrigin（第一关联对象）
	// 与 AutoAssociativities 传入对象的 UF 类型是否合法。
	// 录制 VB 参考：OrdinateOrigin 用的是截面边 DraftingCurve（而非中心线）。
	{
		int clType = 0, clSub = 0;
		UF_OBJ_ask_type_and_subtype(centerlineObj->Tag(), &clType, &clSub);
		std::string clJid;
		try { clJid = centerlineObj->JournalIdentifier().GetText(); }
		catch (...) { clJid = "<无JournalIdentifier>"; }
		sprintf_s(fmt, sizeof(fmt),
			"[坐标标注诊断] 中心线 tag=%llu, type=%d, subtype=%d, JournalId=%s",
			(unsigned long long)centerlineObj->Tag(), clType, clSub, clJid.c_str());
		CommonUtils::print_msg(fmt);
		const int nShow = (int)curves.size() < 3 ? (int)curves.size() : 3;
		for (int i = 0; i < nShow; ++i)
		{
			sprintf_s(fmt, sizeof(fmt),
				"  [坐标标注诊断] 曲线 #%d tag=%llu type=%d subtype=%d has_props=%d "
				"start=(%.3f, %.3f) end=(%.3f, %.3f)",
				i, (unsigned long long)curves[i].tag, curves[i].type, curves[i].subtype,
				(int)curves[i].has_props,
				curves[i].start_pt[0], curves[i].start_pt[1],
				curves[i].end_pt[0], curves[i].end_pt[1]);
			CommonUtils::print_msg(fmt);
		}
	}

	// ---- 候选坐标原点（任务#17 修正）：VB 的 OrdinateOrigin 是端面截面边——
	// 位于测量方向极值位置（最外侧端面）的近铅垂/近水平长边，基准点取其
	// 中点（SnapTypeMid）。旧版只取"最左/最下"未考虑长度，可能选到过短的
	// 碎边导致 NX 无法推断出 OrdinateOriginDimension。现改为：
	// 水平组(测X) → 极值 X（交替尝试最大/最小）处最长的近铅垂曲线；
	// 垂直组(测Y) → 极值 Y 处最长的近水平曲线；找不到则退化为最长同向曲线。
	auto pickOriginCurve = [&](bool horizontal) -> std::pair<NXOpen::DisplayableObject*, NXOpen::Point3d>
	{
		// 第一轮只挑"制图曲线"（line/circle/conic/spline，即 DraftingCurve 形态，
		// 录制 VB 的 OrdinateOrigin 就是截面边 DraftingCurve）；实体边(solid edge)
		// 的关联类型对坐标标注不合法（实测报 "first object associativity type
		// is invalid"），仅作第二轮兜底。
		for (int allowSolid = 0; allowSolid < 2; ++allowSolid)
		{
			for (int ext = 0; ext < 2; ++ext)   // ext=0 取最大极值端，ext=1 取最小极值端
			{
				NXOpen::DisplayableObject* obj = NULL;
				NXOpen::Point3d pt(0.0, 0.0, 0.0);
				double bestKey = (ext == 0) ? -1e300 : 1e300;   // 极值坐标
				double bestLen = -1.0;                          // 该极值曲线长度
				for (size_t i = 0; i < curves.size(); ++i)
				{
					const CommonUtils::CurveInfo& ci = curves[i];
					if (!ci.has_props) continue;
					const bool isSolid = (ci.type == UF_solid_type &&
						ci.subtype == UF_solid_edge_subtype);
					if (allowSolid == 0 && isSolid) continue;   // 第一轮排除实体边
					const double tx = fabs(ci.start_tg[0]) + fabs(ci.end_tg[0]);
					const double ty = fabs(ci.start_tg[1]) + fabs(ci.end_tg[1]);
					// 水平组测量 X -> 基准线近铅垂(ty>tx)；垂直组测量 Y -> 基准线近水平
					if (horizontal ? (ty <= tx) : (tx <= ty)) continue;
					const double key = horizontal
						? (ci.start_pt[0] + ci.end_pt[0]) / 2.0
						: (ci.start_pt[1] + ci.end_pt[1]) / 2.0;
					const double dx = ci.end_pt[0] - ci.start_pt[0];
					const double dy = ci.end_pt[1] - ci.start_pt[1];
					const double len = sqrt(dx * dx + dy * dy);
					if (len < 2.0) continue;   // 过短的碎边不作为基准（任务#17）
					const bool betterKey = (ext == 0) ? (key > bestKey + 1.0)
						                              : (key < bestKey - 1.0);
					if (betterKey || (fabs(key - bestKey) <= 1.0 && len > bestLen))
					{
						if (betterKey)
						{
							bestKey = key;
							bestLen = len;
						}
						else
						{
							bestLen = len;
						}
						obj = dynamic_cast<NXOpen::DisplayableObject*>(
							NXOpen::NXObjectManager::Get(ci.tag));
						pt = NXOpen::Point3d((ci.start_pt[0] + ci.end_pt[0]) / 2.0,
							(ci.start_pt[1] + ci.end_pt[1]) / 2.0, 0.0);
					}
				}
				if (obj) return std::pair<NXOpen::DisplayableObject*, NXOpen::Point3d>(obj, pt);
			}
		}
		// 兜底1：无方向相符曲线时取最长制图曲线；兜底2：再取最长任意曲线
		for (int allowSolid = 0; allowSolid < 2; ++allowSolid)
		{
			NXOpen::DisplayableObject* obj = NULL;
			NXOpen::Point3d pt(0.0, 0.0, 0.0);
			double bestLen = -1.0;
			for (size_t i = 0; i < curves.size(); ++i)
			{
				const CommonUtils::CurveInfo& ci = curves[i];
				if (!ci.has_props) continue;
				const bool isSolid = (ci.type == UF_solid_type &&
					ci.subtype == UF_solid_edge_subtype);
				if (allowSolid == 0 && isSolid) continue;
				const double dx = ci.end_pt[0] - ci.start_pt[0];
				const double dy = ci.end_pt[1] - ci.start_pt[1];
				const double len = sqrt(dx * dx + dy * dy);
				if (len > bestLen)
				{
					bestLen = len;
					obj = dynamic_cast<NXOpen::DisplayableObject*>(
						NXOpen::NXObjectManager::Get(ci.tag));
					pt = NXOpen::Point3d((ci.start_pt[0] + ci.end_pt[0]) / 2.0,
						(ci.start_pt[1] + ci.end_pt[1]) / 2.0, 0.0);
				}
			}
			if (obj) return std::pair<NXOpen::DisplayableObject*, NXOpen::Point3d>(obj, pt);
		}
		return std::pair<NXOpen::DisplayableObject*, NXOpen::Point3d>(NULL, NXOpen::Point3d(0.0, 0.0, 0.0));
	};

	// ---- 取端点（枚举时已求 parm=0.0/1.0），0.01 容差去重 ----
	// EndPt 记录去重后每个端点的承载对象/坐标/snap 类型（parm=0→Start，
	// parm=1→End，录制 VB 的 SecondAssociativities.SetValue 需显式 snap）
	struct EndPt
	{
		NXOpen::DisplayableObject* disp;
		NXOpen::Point3d pt;
		NXOpen::InferSnapType::SnapType snap;
	};
	std::vector<EndPt> endPts;
	{
		const double kDupTol = 0.01;
		for (size_t i = 0; i < curves.size(); ++i)
		{
			if (!curves[i].has_props) continue;   // 端点无效(0,0,0)者不参与
			NXOpen::TaggedObject* tobj = NXOpen::NXObjectManager::Get(curves[i].tag);
			NXOpen::DisplayableObject* disp =
				dynamic_cast<NXOpen::DisplayableObject*>(tobj);
			if (!disp) continue;
			for (int e = 0; e < 2; ++e)
			{
				const double* p = (e == 0) ? curves[i].start_pt : curves[i].end_pt;
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
					// （坐标标注的被测关联对象在 VB 录制中全部是 DraftingCurve）
					int oldT = 0, oldS = 0;
					UF_OBJ_ask_type_and_subtype(endPts[dupIdx].disp->Tag(), &oldT, &oldS);
					const bool oldSolid = (oldT == UF_solid_type && oldS == UF_solid_edge_subtype);
					const bool newSolid = (curves[i].type == UF_solid_type &&
						curves[i].subtype == UF_solid_edge_subtype);
					if (oldSolid && !newSolid)
					{
						endPts[dupIdx].disp = disp;
						endPts[dupIdx].snap = (e == 0) ? NXOpen::InferSnapType::SnapTypeStart
							                       : NXOpen::InferSnapType::SnapTypeEnd;
					}
					continue;
				}
				EndPt ep;
				ep.disp = disp;
				ep.pt = NXOpen::Point3d(p[0], p[1], p[2]);
				ep.snap = (e == 0) ? NXOpen::InferSnapType::SnapTypeStart
					               : NXOpen::InferSnapType::SnapTypeEnd;
				endPts.push_back(ep);
			}
		}
	}
	sprintf_s(fmt, sizeof(fmt),
		"  视图成员曲线/边 %d 条，轮廓端点（去重后）%d 个",
		(int)curves.size(), (int)endPts.size());
	CommonUtils::print_msg(fmt);
	if (endPts.empty())
	{
		CommonUtils::print_msg("[Step3] 轮廓坐标标注已跳过（未取到轮廓端点）");
		return;
	}

	// ---- 单个 Builder（水平组或垂直组）：基准按候选顺序尝试，端点集=AutoAssociativities ----
	// 基准候选：[0]=中心线（原方案） [1]=截面边 DraftingCurve（录制 VB 方案）；
	// 每个基准内再按成组模式（TypesMultipleDimension）→ 单条降级模式
	// （TypesSingleDimension）两级重试；任一组合成功即返回，全部失败记日志。
	// 注：头文件实测 BaseOrdinateDimensionBuilder 的 OrdinateOrigin 为
	// SelectDisplayableObject、AutoAssociativities 为 SelectDisplayableObjectList，
	// 均支持 (DisplayableObject*, View*, Point3d) 三元重载（DraftingCurve/
	// Edge 均继承自 DisplayableObject，可直接传入）。
	NXOpen::DisplayableObject* clDisp = centerlineObj;
	auto runBuilder = [&](bool horizontal) -> int
	{
		const char* dirName = horizontal ? "水平" : "垂直";
		struct OriginCand
		{
			NXOpen::DisplayableObject* obj;
			NXOpen::Point3d pt;
			const char* desc;
		};
		const std::pair<NXOpen::DisplayableObject*, NXOpen::Point3d> fc = pickOriginCurve(horizontal);
		OriginCand cands[2] = {
			{ clDisp, NXOpen::Point3d(0.0, 0.0, 0.0), "中心线" },
			{ fc.first, fc.second, "截面边曲线" }
		};
		int created = -1;   // -1 = 全部失败
		for (int oi = 0; oi < 2 && created < 0; ++oi)
		{
			if (!cands[oi].obj) continue;
			for (int pass = 0; pass < 2 && created < 0; ++pass)
			{
				NXOpen::Annotations::OrdinateDimensionBuilder* obBuilder = NULL;
				try
				{
					obBuilder = part->Dimensions()->CreateOrdinateDimensionBuilder(NULL);
					obBuilder->SetType(pass == 0
						? NXOpen::Annotations::BaseOrdinateDimensionBuilder::TypesMultipleDimension
						: NXOpen::Annotations::BaseOrdinateDimensionBuilder::TypesSingleDimension);
					// 基准：SetValue(对象, 视图, 基准点)；VB 参考传曲线中点
					obBuilder->OrdinateOrigin()->SetValue(cands[oi].obj, sectionView,
						cands[oi].pt);
					// 端点集：曲线/边 NXObject + 所在视图 + 端点坐标
					for (size_t i = 0; i < curves.size(); ++i)
					{
						NXOpen::TaggedObject* tobj = NXOpen::NXObjectManager::Get(curves[i].tag);
						NXOpen::DisplayableObject* disp =
							dynamic_cast<NXOpen::DisplayableObject*>(tobj);
						if (!disp) continue;
						double pt[3], tg[3], pn[3], bn[3], torsion = 0.0, roc = 0.0;
						for (double parm = 0.0; parm <= 1.0 + 1e-9; parm += 1.0)
						{
							if (UF_MODL_ask_curve_props(curves[i].tag, parm, pt, tg, pn, bn, &torsion, &roc) != 0)
								continue;
							NXOpen::Point3d q(pt[0], pt[1], pt[2]);
							bool dup = false;
							for (size_t k = 0; k < endPts.size(); ++k)
							{
								const double dx = endPts[k].pt.X - q.X, dy = endPts[k].pt.Y - q.Y;
								if (dx * dx + dy * dy < 0.01 * 0.01) { dup = true; break; }
							}
							if (!dup) continue;   // 该端点已被去重丢弃，不重复 Add
							obBuilder->AutoAssociativities()->Add(disp, sectionView, q);
						}
					}
					// Commit 前诊断：基准对象与关联对象数量
					sprintf_s(fmt, sizeof(fmt),
						"[坐标标注诊断] %s组 基准=%s 模式=%s 关联对象 %d 个，准备 Commit",
						dirName, cands[oi].desc, (pass == 0 ? "成组" : "单条"),
						obBuilder->AutoAssociativities()->Size());
					CommonUtils::print_msg(fmt);
					// 放置方位（margin）决定标注方向
					if (horizontal) obBuilder->SetHorizontalInferredMarginLocation(hMargin);
					else            obBuilder->SetVerticalInferredMarginLocation(vMargin);
					obBuilder->Origin()->AnnotationView()->SetValue(sectionView);
					obBuilder->Commit();
					obBuilder->Destroy();
					obBuilder = NULL;
					created = (pass == 0) ? (int)endPts.size() : 1;
				}
				catch (const NXOpen::NXException& e)
				{
					if (obBuilder) { obBuilder->Destroy(); obBuilder = NULL; }
					sprintf_s(fmt, sizeof(fmt), "  警告: %s坐标标注 基准=%s %s模式失败: %s",
						dirName, cands[oi].desc, (pass == 0 ? "成组" : "单条降级"),
						e.Message());
					CommonUtils::print_msg(fmt);
				}
				catch (...)
				{
					if (obBuilder) { obBuilder->Destroy(); obBuilder = NULL; }
					CommonUtils::print_msg(string("  警告: ") + dirName + "坐标标注失败（未知异常）");
				}
			}
		}
		return created;
	};

	// ---- 录制 VB 忠实逐条模式（终极降级，任务#15 根因修复 / #24 放置控制） ----
	// 录制宏 000R_VB.vb 成功模式（L1364/L1434/L1527/L1564/L1614/L1722/L1817）：
	//   首条：OrdinateOrigin.SetValue(SnapTypeMid, 截面边曲线, 视图, 曲线上点,
	//         NULL, NULL, (0,0,0)) + SecondAssociativities.SetValue(snap, 端点曲线,
	//         视图, 端点, ...) + 显式放置原点 Origin.Origin.SetValue(Nothing,
	//         nullView, 图纸坐标点)（L1527） → Commit 产出 OrdinateOriginDimension
	//         （ENTITY 26 6）；Commit 后 OrdinateMargins.CreateInferredMargin
	//         （L1564）建链式 margin。
	//   后续：OrdinateOrigin.SetValue(基准标注) + ActiveVerticalMargin = 已建
	//         margin（L1722）+ 显式原点逐条步进 9.0（L1817 vs L1527：X 不变 Y+9），
	//         每条标注独立 Commit。
	// C++ 映射（头文件证据）：
	//   Origin.Origin.SetValue          → ob->Origin()->Origin()->SetValue(NULL, NULL, pt)
	//                                     （Annotations_OriginBuilder.hxx L111: Origin() → SelectDisplayableObject*）
	//   OrdinateMargins.CreateInferredMargin → part->Annotations()->OrdinateMargins()->CreateInferredMargin(...)
	//                                     （Annotations_AnnotationManager.hxx L899、Annotations_OrdinateMarginCollection.hxx L191）
	//   ActiveVerticalMargin = margin   → ob->SetActiveVerticalMargin(margin)
	//                                     （Annotations_BaseOrdinateDimensionBuilder.hxx L228/L211）
	auto runVbPattern = [&](bool horizontal) -> int
	{
		const char* dirName = horizontal ? "水平" : "垂直";
		const std::pair<NXOpen::DisplayableObject*, NXOpen::Point3d> oc = pickOriginCurve(horizontal);
		if (!oc.first)
		{
			CommonUtils::print_msg(string("  [Step3] ") + dirName +
				"VB 模式降级跳过：无可用截面边基准曲线");
			return -1;
		}
		// VB 中首条的被测点（SecondAssociativities）是另一条曲线的端点，
		// 绝不在基准曲线自身上；若被测点落在基准曲线上 NX 会推断出
		// 零长度/普通尺寸而非 OrdinateOriginDimension（任务#17 根因候选）。
		// 故过滤掉承载对象即基准曲线的端点。
		std::vector<const EndPt*> measPts;
		for (size_t i = 0; i < endPts.size(); ++i)
		{
			if (endPts[i].disp == oc.first) continue;
			measPts.push_back(&endPts[i]);
		}
		sprintf_s(fmt, sizeof(fmt),
			"  [%sVB 模式] 基准曲线 tag=%llu 中点=(%.3f, %.3f)，可用被测端点 %d/%d 个",
			dirName, (unsigned long long)oc.first->Tag(),
			oc.second.X, oc.second.Y, (int)measPts.size(), (int)endPts.size());
		CommonUtils::print_msg(fmt);
		if (measPts.empty())
		{
			CommonUtils::print_msg(string("  [Step3] ") + dirName +
				"VB 模式降级跳过：除基准曲线外无可用被测端点");
			return -1;
		}

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
		NXOpen::Annotations::OrdinateMargin* activeMargin = NULL;   // VB L1722 链式 margin

		// ---- 显式放置原点（任务#24，图纸坐标，仿 VB L1527/L1817）----
		// VB 实测：首条原点(49.45, 77.66)，后续每条同 X、Y 步进 +9.0。
		// 本实现从视图边界推导：垂直组(测X) 首条放在视图内近左缘（margin
		// 在左缘外侧），沿 Y 步进；水平组(测Y) 首条放在视图内近下缘，沿 X
		// 步进。边界读取失败则不显式放置（维持 NX 推断，记日志）。
		const double kOrdStep = 9.0;   // VB 实测步进 9.0（L1817 减 L1527）
		NXOpen::Point3d firstOrigin(0.0, 0.0, 0.0);
		NXOpen::Point3d originStep(0.0, 0.0, 0.0);
		bool explicitOrigin = false;
		if (borderOk)
		{
			if (horizontal)   // 测 Y：margin 在视图下缘外侧，文字行沿 X 排布
			{
				firstOrigin = NXOpen::Point3d((b[0] + b[2]) / 2.0, b[1] + 6.0, 0.0);
				originStep = NXOpen::Point3d(kOrdStep, 0.0, 0.0);
			}
			else              // 测 X：margin 在视图左缘外侧，文字行沿 Y 排布
			{
				firstOrigin = NXOpen::Point3d(b[0] + 6.0, (b[1] + b[3]) / 2.0, 0.0);
				originStep = NXOpen::Point3d(0.0, kOrdStep, 0.0);
			}
			explicitOrigin = true;
			sprintf_s(fmt, sizeof(fmt),
				"  [%sVB 模式] 显式放置: 首条原点=(%.3f, %.3f) 步进=(%.1f, %.1f)（图纸坐标）",
				dirName, firstOrigin.X, firstOrigin.Y, originStep.X, originStep.Y);
			CommonUtils::print_msg(fmt);
		}
		else
		{
			CommonUtils::print_msg(string("  [") + dirName +
				"VB 模式] 视图边界不可用，放置降级为 NX 推断");
		}
		for (size_t i = 0; i < measPts.size(); ++i)
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
					// 基准 = 端面截面边 DraftingCurve（VB L1364 同款 7 参 SetValue）
					ob->OrdinateOrigin()->SetValue(NXOpen::InferSnapType::SnapTypeMid,
						oc.first, sectionView, oc.second,
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
						if (horizontal) ob->SetActiveHorizontalMargin(activeMargin);
						else            ob->SetActiveVerticalMargin(activeMargin);
					}
				}
				// 被测点：SecondAssociativities（每 Builder 仅 1 个，VB L1434/L1764）
				ob->SecondAssociativities()->SetValue(measPts[i]->snap,
					measPts[i]->disp, sectionView, measPts[i]->pt,
					NULL, NULL, NXOpen::Point3d(0.0, 0.0, 0.0));
				if (horizontal) ob->SetHorizontalInferredMarginLocation(hMargin);
				else            ob->SetVerticalInferredMarginLocation(vMargin);
				// VB L1534：LeaderOrientation = Left
				ob->Style()->LineArrowStyle()->SetLeaderOrientation(
					NXOpen::Annotations::LeaderSideLeft);
				ob->Origin()->AnnotationView()->SetValue(sectionView);
				// ---- 显式放置原点（VB L1506-1529 / L1796-1817）----
				// 关键：先 SetAssociativeOrigin(Drag)（OriginType=Drag、其余字段全空），
				// 再 Origin.Origin.SetValue(NULL, NULL, pt)，否则显式原点不生效；
				// 该调用序列在录制宏中每个标注（含线性/直径）都有。
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
				// VB L1552-1553：Commit 返回值即首条坐标标注（Vertical/HorizontalOrdinateDimension）
				NXOpen::NXObject* commitObj = NULL;
				if (i == 0) commitObj = ob->Commit();
				else        ob->Commit();
				if (i == 0)
				{
					// 诊断：打印首条 Commit 全部产物的 UF 类型与类名，
					// 定位 NX 实际推断出的标注类型（任务#17）
					std::vector<NXOpen::NXObject*> objs = ob->GetCommittedObjects();
					sprintf_s(fmt, sizeof(fmt),
						"  [%sVB 诊断] 首条 Commit 产物 %d 个:",
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
						else if (dynamic_cast<NXOpen::Annotations::HorizontalDimension*>(o))
							cls = "HorizontalDimension";
						else if (dynamic_cast<NXOpen::Annotations::VerticalDimension*>(o))
							cls = "VerticalDimension";
						else if (dynamic_cast<NXOpen::Annotations::Dimension*>(o))
							cls = "Dimension(其它子类)";
						sprintf_s(fmt, sizeof(fmt),
							"    #%d tag=%llu type=%d subtype=%d class=%s",
							(int)k, (unsigned long long)o->Tag(), t, s, cls);
						CommonUtils::print_msg(fmt);
						if (!originDim) originDim = ood;
						if (!fallbackDim) fallbackDim = dynamic_cast<NXOpen::Annotations::Dimension*>(o);
					}
					// VB L1553：Commit 返回值本身就是首条坐标标注（nXObject6）
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
									"  [%sVB 诊断] 经 Dimensions() 扫描定位新基准标注 tag=%llu",
									dirName, (unsigned long long)d->Tag());
								CommonUtils::print_msg(fmt);
							}
						}
						if (!originDim && fallbackDim)
						{
							sprintf_s(fmt, sizeof(fmt),
								"  警告: %sVB 模式：首条产物非 OrdinateOriginDimension，"
								"降级用普通 Dimension(tag=%llu) 作链式基准",
								dirName, (unsigned long long)fallbackDim->Tag());
							CommonUtils::print_msg(fmt);
						}
						else if (!originDim)
						{
							CommonUtils::print_msg(string("  警告: ") + dirName +
								"VB 模式：首条 Commit 后未获取任何可作基准的 Dimension");
						}
						// VB L1562-1564：首条产出 OrdinateOriginDimension 后，用
						// OrdinateMargins.CreateInferredMargin 创建链式 margin，
						// 供后续 builder 的 ActiveVertical/HorizontalMargin 继承。
						// subtype 取 13(水平)/14(垂直)，与 uf_object_types.h 中
						// UF_dim_ordinate_horiz/vert_subtype 一致（VB 实测传 14）。
						if (originDim && !activeMargin)
						{
							try
							{
								const NXOpen::Point3d& mp = horizontal ? hMargin : vMargin;
								activeMargin = part->Annotations()->OrdinateMargins()->
									CreateInferredMargin(originDim, mp,
										horizontal ? UF_dim_ordinate_horiz_subtype
										           : UF_dim_ordinate_vert_subtype);
								if (activeMargin)
								{
									sprintf_s(fmt, sizeof(fmt),
										"  [%sVB 诊断] 已创建链式 margin tag=%llu 位置=(%.3f, %.3f)",
										dirName, (unsigned long long)activeMargin->Tag(),
										mp.X, mp.Y);
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
								CommonUtils::print_msg(string("  警告: ") + dirName +
									"VB 模式：链式 margin 创建失败（未知异常）");
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
										"  [%sVB 诊断] margin tag=%llu 已关联到首条标注 tag=%llu",
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
								CommonUtils::print_msg(string("  警告: ") + dirName +
									"VB 模式：margin 关联到标注失败（未知异常）");
							}
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
			"[Step3] %sVB 逐条模式: 成功 %d / 失败 %d (链式基准=%s)",
				dirName, created, failed,
				originDim ? "OrdinateOriginDimension" : (fallbackDim ? "普通Dimension" : "无"));
		CommonUtils::print_msg(fmt);
		return created > 0 ? created : -1;
	};

	// 头文件语义为"放置方位决定方向"，单个 Builder 无法同时产出水平+垂直两组，
	// 故分别创建水平组与垂直组两个 Builder；原有两级降级全部失败时
	// 追加录制 VB 同款逐条模式作为终极降级
	int hCount = runBuilder(true);
	if (hCount < 0) hCount = runVbPattern(true);
	int vCount = runBuilder(false);
	if (vCount < 0) vCount = runVbPattern(false);

	const std::string hStr = (hCount >= 0) ? std::to_string(hCount) + " 条" : "失败";
	const std::string vStr = (vCount >= 0) ? std::to_string(vCount) + " 条" : "失败";
	sprintf_s(fmt, sizeof(fmt),
		"[Step3] 水平坐标标注 %s / 垂直坐标标注 %s / 轮廓边 %d 条",
		hStr.c_str(), vStr.c_str(), (int)curves.size());
	CommonUtils::print_msg(fmt);
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

		// ===== 激活图纸 =====
		// 遍历图纸集合，打开第一张图纸（单图纸场景与原始流程一致）
		NXOpen::Drawings::DraftingDrawingSheet* sheet = NULL;
		for (NXOpen::Drawings::DraftingDrawingSheetCollection::iterator it =
			part->DraftingDrawingSheets()->begin();
			it != part->DraftingDrawingSheets()->end(); ++it)
		{
			NXOpen::Drawings::DraftingDrawingSheet* s = *it;
			if (s) { s->Open(); sheet = s; break; }
		}
		if (!sheet) { CommonUtils::print_msg("[Step3] 警告：未找到任何图纸，请先运行 Step1"); return; }
		CommonUtils::print_msg("[Step3] 已激活图纸");
		
		// ---- 诊断：列举图纸上所有视图（排查多余视图致标注挂载错误） ----
		{
			std::vector<NXOpen::Drawings::DraftingView*> sheetViews = sheet->GetDraftingViews();
			const int nViews = (int)sheetViews.size();
			char fmt[256];
			sprintf_s(fmt, sizeof(fmt),
				"[Step3 视图诊断] 图纸 \"%s\" 上共有 %d 个视图:",
				sheet->Name().GetText(), nViews);
			CommonUtils::print_msg(fmt);
			for (size_t idx = 0; idx < sheetViews.size(); ++idx)
			{
				NXOpen::Drawings::DraftingView* v = sheetViews[idx];
				if (!v) continue;
				const char* vtype = "DraftingView";
				if (dynamic_cast<NXOpen::Drawings::SectionView*>(v))
					vtype = "SectionView(剖视)";
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
		
		NXOpen::Drawings::SectionView* sectionView = CommonUtils::find_section_view(part);
		if (!sectionView) { CommonUtils::print_msg("[Step3] 未找到剖视图，请先运行 Step1"); return; }

		NXOpen::Annotations::Centerline2d* centerlineObj = CommonUtils::find_centerline(part);
		if (!centerlineObj) { CommonUtils::print_msg("[Step3] 未找到中心线，请先运行 Step5"); return; }

		phase_auto_contour_dims(part, sectionView, centerlineObj);

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
