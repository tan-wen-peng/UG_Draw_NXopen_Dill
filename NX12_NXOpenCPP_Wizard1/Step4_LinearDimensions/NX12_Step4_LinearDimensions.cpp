//------------------------------------------------------------------------------
// NX12 Step4：线性/直径尺寸标注（DLL 4/6）
// 功能：自动枚举剖视图轮廓边，按平行边配对生成线性标注，自动识别圆弧生成直径标注
//------------------------------------------------------------------------------

// Win32 防护 + 共享头文件
#include "../Shared/NX12_CommonConfig.h"
#include "../Shared/NX12_CommonUtils.h"

// Step4 专有 includes
#include <NXOpen/Annotations_LinearDimensionBuilder.hxx>
#include <NXOpen/Annotations_RapidDimensionBuilder.hxx>
#include <NXOpen/Annotations_RadialDimensionBuilder.hxx>
#include <NXOpen/Annotations_Dimension.hxx>
#include <NXOpen/Annotations_DimensionCollection.hxx>
#include <NXOpen/Annotations_GeneralHorizontalDimension.hxx>
#include <NXOpen/Annotations_GeneralVerticalDimension.hxx>
#include <NXOpen/Annotations_GeneralParallelDimension.hxx>
#include <NXOpen/Annotations_GeneralDiameterDimension.hxx>
#include <NXOpen/InferSnapType.hxx>
#include <NXOpen/Annotations_OriginBuilder.hxx>
#include <NXOpen/DisplayableObject.hxx>
#include <NXOpen/NXObjectManager.hxx>
#include <NXOpen/SelectNXObject.hxx>
#include <NXOpen/SelectDisplayableObject.hxx>
#include <NXOpen/Drawings_SelectDraftingView.hxx>
#include <NXOpen/NXMessageBox.hxx>
#include <NXOpen/Drawings_DraftingDrawingSheetCollection.hxx>
#include <NXOpen/Drawings_DraftingDrawingSheet.hxx>
#include <NXOpen/Drawings_DraftingViewCollection.hxx>
#include <NXOpen/Drawings_BaseView.hxx>
#include <NXOpen/DraftingManager.hxx>

#include <algorithm>

//==============================================================================
// 辅助：计算曲线中点（直线取端点平均，圆弧取 parm=0.5）
//==============================================================================
static void curve_midpoint(const CommonUtils::CurveInfo& ci, double mid[3])
{
	if (ci.type == UF_circle_type)
	{
		double pt[3], tg[3], pn[3], bn[3], torsion = 0.0, roc = 0.0;
		if (UF_MODL_ask_curve_props(ci.tag, 0.5, pt, tg, pn, bn, &torsion, &roc) == 0)
		{
			mid[0] = pt[0]; mid[1] = pt[1]; mid[2] = pt[2];
		}
		else
		{
			for (int i = 0; i < 3; ++i) mid[i] = (ci.start_pt[i] + ci.end_pt[i]) / 2.0;
		}
	}
	else
	{
		for (int i = 0; i < 3; ++i) mid[i] = (ci.start_pt[i] + ci.end_pt[i]) / 2.0;
	}
}

//==============================================================================
// 辅助：计算曲线长度（直线段长度，圆弧用弧长）
//==============================================================================
static double curve_length(const CommonUtils::CurveInfo& ci)
{
	double dx = ci.end_pt[0] - ci.start_pt[0];
	double dy = ci.end_pt[1] - ci.start_pt[1];
	double dz = ci.end_pt[2] - ci.start_pt[2];
	return sqrt(dx * dx + dy * dy + dz * dz);
}

//==============================================================================
// 辅助：归一化切矢
//==============================================================================
static void normalize_vec(const double v[3], double out[3])
{
	double len = sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
	if (len < 1e-12) { out[0] = out[1] = out[2] = 0.0; return; }
	out[0] = v[0] / len; out[1] = v[1] / len; out[2] = v[2] / len;
}

//==============================================================================
// 辅助：叉积的模（用于判断平行度）
//==============================================================================
static double cross_magnitude(const double a[3], const double b[3])
{
	double cx = a[1] * b[2] - a[2] * b[1];
	double cy = a[2] * b[0] - a[0] * b[2];
	double cz = a[0] * b[1] - a[1] * b[0];
	return sqrt(cx * cx + cy * cy + cz * cz);
}

//==============================================================================
// 辅助结构：曲线映射到图纸坐标后的几何（与 UF_DRAW_ask_view_borders 同一坐标系）
// 背景：UF_MODL_ask_curve_props / UF_CURVE_ask_arc_data 返回模型/绝对坐标，
// 而视图边界是图纸坐标，两者相差视图比例/旋转/视图原点，必须用
// UF_VIEW_map_model_to_drawing 转换后才能参与放置点计算
//==============================================================================
struct DrawGeom {
	bool ok;
	double s[2];       // 起点映射
	double e[2];       // 终点映射
	double mid[2];     // 中点映射（直线端点平均/圆弧 parm=0.5）
	double center[2];  // 圆心映射（仅圆弧有效）
};

static DrawGeom map_curve_geom(tag_t viewTag, const CommonUtils::CurveInfo& ci)
{
	DrawGeom g;
	g.ok = true;
	g.s[0] = g.s[1] = g.e[0] = g.e[1] = 0.0;
	g.mid[0] = g.mid[1] = g.center[0] = g.center[1] = 0.0;

	double m[3];
	curve_midpoint(ci, m);
	if (!CommonUtils::map_model_to_drawing(viewTag, ci.start_pt, g.s)) g.ok = false;
	if (!CommonUtils::map_model_to_drawing(viewTag, ci.end_pt, g.e)) g.ok = false;
	if (!CommonUtils::map_model_to_drawing(viewTag, m, g.mid)) g.ok = false;
	if (ci.type == UF_circle_type)
	{
		if (!CommonUtils::map_model_to_drawing(viewTag, ci.arc_center, g.center)) g.ok = false;
	}
	return g;
}

//==============================================================================
// 辅助结构：线性标注对
//==============================================================================
struct DimPair {
	CommonUtils::CurveInfo e1;
	CommonUtils::CurveInfo e2;
	bool is_horizontal;  // true=水平标注（切矢主方向Y），false=垂直标注（切矢主方向X）
	double sort_coord;   // 排序坐标（水平组按X，垂直组按Y，图纸坐标）
	double s1[2], e1d[2];  // 边1映射后起止点（图纸坐标）
	double s2[2], e2d[2];  // 边2映射后起止点（图纸坐标）
	double dmid1[2], dmid2[2];  // 两边映射后中点（图纸坐标）
};

//==============================================================================
// 辅助：将放置点钳制在图纸内（内缩 kSheetMargin），返回是否发生钳制
// 背景：NX 不渲染图纸边界外的标注，超出 [0,sheetW]×[0,sheetH] 的放置点
// 会导致标注“创建成功但不可见”
//==============================================================================
static bool clamp_to_sheet(NXOpen::Point3d& pt, double sheetW, double sheetH,
	const double kSheetMargin = 2.0)
{
	bool clamped = false;
	if (pt.X < kSheetMargin) { pt.X = kSheetMargin; clamped = true; }
	if (pt.X > sheetW - kSheetMargin) { pt.X = sheetW - kSheetMargin; clamped = true; }
	if (pt.Y < kSheetMargin) { pt.Y = kSheetMargin; clamped = true; }
	if (pt.Y > sheetH - kSheetMargin) { pt.Y = sheetH - kSheetMargin; clamped = true; }
	return clamped;
}

//==============================================================================
// phase_auto_linear_dims —— 自动线性尺寸标注
//==============================================================================
static void phase_auto_linear_dims(NXOpen::Part* part,
	NXOpen::Drawings::DraftingView* sectionView,
	const double borders[4],
	double sheetW, double sheetH)
{
	char fmt[512];

	// ---- 幂等保护：检测剖视图上是否已存在本模块创建的尺寸标注 ----
	// 启发式：遍历 part->Dimensions()，取每条标注的关联原点数据
	// (Annotation::GetAssociativeOrigin)，若其 View / ViewOfGeometry 为本剖视图
	// 则计入；因创建时 Origin()->AnnotationView() 均设为剖视图，此检测可靠。
	// 背景：UF_DRAW_ask_view_borders 会把已有标注计入视图边界，重复运行会使
	// 边界膨胀、放置点逐次外漂，故检测到已有标注即跳过创建。
	int existingLinear = 0, existingRadial = 0;
	{
		for (NXOpen::Annotations::DimensionCollection::iterator it = part->Dimensions()->begin();
			it != part->Dimensions()->end(); ++it)
		{
			NXOpen::Annotations::Dimension* d = *it;
			if (!d) continue;
			bool linked = false;
			try
			{
				NXOpen::Point3d originPt(0.0, 0.0, 0.0);
				NXOpen::Annotations::Annotation::AssociativeOriginData od = d->GetAssociativeOrigin(&originPt);
				if ((od.View && od.View->Tag() == sectionView->Tag()) ||
					(od.ViewOfGeometry && od.ViewOfGeometry->Tag() == sectionView->Tag()))
					linked = true;
			}
			catch (...) { linked = false; }
			if (!linked) continue;
			// NX12 无 LinearDimension/RadialDimension 类：线性标注具体类型为
			// GeneralHorizontal/GeneralVertical/GeneralParallelDimension，
			// 直径标注为 GeneralDiameterDimension
			if (dynamic_cast<NXOpen::Annotations::GeneralHorizontalDimension*>(d) ||
				dynamic_cast<NXOpen::Annotations::GeneralVerticalDimension*>(d) ||
				dynamic_cast<NXOpen::Annotations::GeneralParallelDimension*>(d))
				++existingLinear;
			else if (dynamic_cast<NXOpen::Annotations::GeneralDiameterDimension*>(d))
				++existingRadial;
		}
		const int existingTotal = existingLinear + existingRadial;
		// 线性与直径分开判重：上一次只成功创建了其中一类时，另一类仍应继续创建
		// （原先 total>0 即整体跳过，会让首次只出直径的部件永远补不上线性标注）。
		if (existingTotal > 0)
		{
			sprintf_s(fmt, sizeof(fmt),
				"[Step4] 幂等检查: 剖视图已有尺寸标注 线性 %d / 直径 %d（检测启发式：标注关联原点 View/ViewOfGeometry == 剖视图）",
				existingLinear, existingRadial);
			CommonUtils::print_msg(fmt);
		}
		else
		{
			CommonUtils::print_msg("[Step4] 幂等检查: 剖视图上无已有尺寸标注，继续创建");
		}
	}

	// ---- 确保视图已更新（再生投影几何） ----
	NXOpen::Session* sess = CommonUtils::get_session();
	CommonUtils::silent_update(sess);
	sectionView->Update();

	// ---- 枚举视图曲线 ----
	std::vector<CommonUtils::CurveInfo> curves = CommonUtils::enumerate_view_curves(sectionView);
	sprintf_s(fmt, sizeof(fmt), "[Step4] 枚举视图曲线 %d 条", (int)curves.size());
	CommonUtils::print_msg(fmt);
	if (curves.empty()) return;

	// ---- 分类为直线和圆弧 ----
	std::vector<CommonUtils::CurveInfo> lines, arcs;
	CommonUtils::classify_edges(curves, lines, arcs);
	sprintf_s(fmt, sizeof(fmt), "[Step4] 分类结果: 直线 %d 条, 圆弧 %d 条",
		(int)lines.size(), (int)arcs.size());
	CommonUtils::print_msg(fmt);

	// ---- 过滤端点求取失败的曲线（无效坐标会污染配对与放置计算） ----
	{
		std::vector<CommonUtils::CurveInfo> fl, fa;
		for (size_t i = 0; i < lines.size(); ++i) if (lines[i].has_props) fl.push_back(lines[i]);
		for (size_t i = 0; i < arcs.size(); ++i) if (arcs[i].has_props) fa.push_back(arcs[i]);
		if (fl.size() != lines.size() || fa.size() != arcs.size())
		{
			sprintf_s(fmt, sizeof(fmt),
				"[Step4] 已过滤端点无效曲线: 直线 %d -> %d, 圆弧 %d -> %d",
				(int)lines.size(), (int)fl.size(), (int)arcs.size(), (int)fa.size());
			CommonUtils::print_msg(fmt);
		}
		lines.swap(fl);
		arcs.swap(fa);
	}

	// ---- 将直线几何映射到图纸坐标（与 UF_DRAW_ask_view_borders 同一坐标系） ----
	std::vector<DrawGeom> lineGeom(lines.size());
	int mapFail = 0;
	for (size_t i = 0; i < lines.size(); ++i)
	{
		lineGeom[i] = map_curve_geom(sectionView->Tag(), lines[i]);
		if (!lineGeom[i].ok) ++mapFail;
	}
	sprintf_s(fmt, sizeof(fmt), "[Step4] 直线坐标映射到图纸坐标: 成功 %d 条, 失败 %d 条",
		(int)lines.size() - mapFail, mapFail);
	CommonUtils::print_msg(fmt);

	// =========================================================================
	// Phase 1: 线性标注 —— 平行边配对
	// =========================================================================
	if (existingLinear > 0)
	{
		CommonUtils::print_msg("[Step4] 线性标注已存在，跳过线性创建（直径标注仍执行）");
	}
	else if (lines.empty())
	{
		CommonUtils::print_msg("[Step4] 无直线段，跳过线性标注");
	}
	else
	{
		// ---- 按切矢方向分组平行边 ----
		const double kParallelTol = 0.01;   // 叉积容差
		const double kMinEdgeLen = 0.5;     // 最短边长
		const double kMinDist = 1.0;        // 最小间距

		// 归一化切矢
		size_t n = lines.size();
		std::vector<double> normTg(n * 3);
		for (size_t i = 0; i < n; ++i)
		{
			normalize_vec(lines[i].start_tg, &normTg[i * 3]);
		}

		// 分组：简单遍历，对每条未分组的线找所有平行线
		std::vector<bool> grouped(n, false);
		std::vector<std::vector<size_t> > groups;

		for (size_t i = 0; i < n; ++i)
		{
			if (grouped[i]) continue;
			if (!lineGeom[i].ok) { grouped[i] = true; continue; }
			if (curve_length(lines[i]) < kMinEdgeLen) { grouped[i] = true; continue; }

			std::vector<size_t> grp;
			grp.push_back(i);
			grouped[i] = true;

			for (size_t j = i + 1; j < n; ++j)
			{
				if (grouped[j]) continue;
				if (!lineGeom[j].ok) { grouped[j] = true; continue; }
				if (curve_length(lines[j]) < kMinEdgeLen) { grouped[j] = true; continue; }

				double crossMag = cross_magnitude(&normTg[i * 3], &normTg[j * 3]);
				if (crossMag < kParallelTol)
				{
					grp.push_back(j);
					grouped[j] = true;
				}
			}
			if (grp.size() >= 2)
			{
				groups.push_back(grp);
			}
		}

		sprintf_s(fmt, sizeof(fmt), "[Step4] 平行边分组 %d 组", (int)groups.size());
		CommonUtils::print_msg(fmt);

		// ---- 对每组平行边排序，生成相邻配对 ----
		std::vector<DimPair> hDims;   // 水平标注（切矢主方向X -> 边是水平的 -> 标注Y方向间距）
		std::vector<DimPair> vDims;   // 垂直标注（切矢主方向Y -> 边是垂直的 -> 标注X方向间距）

		for (size_t g = 0; g < groups.size(); ++g)
		{
			const std::vector<size_t>& grp = groups[g];

			// 判断该组平行边的主方向
			double avgTgX = 0.0, avgTgY = 0.0;
			for (size_t k = 0; k < grp.size(); ++k)
			{
				avgTgX += fabs(normTg[grp[k] * 3 + 0]);
				avgTgY += fabs(normTg[grp[k] * 3 + 1]);
			}
			avgTgX /= (double)grp.size();
			avgTgY /= (double)grp.size();

			bool isHorizEdge = (avgTgX > avgTgY);  // 边是水平方向

			// 按位置排序：水平边按Y坐标排序，垂直边按X坐标排序（均为图纸坐标）
			// 排序指标 = 映射后中点在垂直于边方向上的坐标
			std::vector<std::pair<double, size_t> > sorted;
			for (size_t k = 0; k < grp.size(); ++k)
			{
				double coord = isHorizEdge ? lineGeom[grp[k]].mid[1] : lineGeom[grp[k]].mid[0];
				sorted.push_back(std::make_pair(coord, grp[k]));
			}
			std::sort(sorted.begin(), sorted.end());

			// 相邻配对（间距用图纸坐标中点距离，与视图边界同尺度）
			for (size_t k = 0; k + 1 < sorted.size(); ++k)
			{
				size_t idx1 = sorted[k].second;
				size_t idx2 = sorted[k + 1].second;

				double ddx = lineGeom[idx1].mid[0] - lineGeom[idx2].mid[0];
				double ddy = lineGeom[idx1].mid[1] - lineGeom[idx2].mid[1];
				double dist = sqrt(ddx * ddx + ddy * ddy);
				if (dist < kMinDist) continue;

				DimPair dp;
				dp.e1 = lines[idx1];
				dp.e2 = lines[idx2];
				dp.is_horizontal = isHorizEdge;
				memcpy(dp.s1, lineGeom[idx1].s, sizeof(dp.s1));
				memcpy(dp.e1d, lineGeom[idx1].e, sizeof(dp.e1d));
				memcpy(dp.s2, lineGeom[idx2].s, sizeof(dp.s2));
				memcpy(dp.e2d, lineGeom[idx2].e, sizeof(dp.e2d));
				memcpy(dp.dmid1, lineGeom[idx1].mid, sizeof(dp.dmid1));
				memcpy(dp.dmid2, lineGeom[idx2].mid, sizeof(dp.dmid2));

				// 排序坐标：两映射中点的平均（沿标注排列方向，图纸坐标）
				dp.sort_coord = isHorizEdge ? ((dp.dmid1[0] + dp.dmid2[0]) / 2.0)
				                            : ((dp.dmid1[1] + dp.dmid2[1]) / 2.0);

				if (isHorizEdge)
					hDims.push_back(dp);
				else
					vDims.push_back(dp);
			}
		}

		// ---- 排序：避免标注链交叉 ----
		// 水平标注（标注Y方向间距）：按X坐标排序，放置在不同Y层
		std::sort(hDims.begin(), hDims.end(),
			[](const DimPair& a, const DimPair& b) { return a.sort_coord < b.sort_coord; });
		// 垂直标注（标注X方向间距）：按Y坐标排序，放置在不同X层
		std::sort(vDims.begin(), vDims.end(),
			[](const DimPair& a, const DimPair& b) { return a.sort_coord < b.sort_coord; });

		// ---- 创建线性标注 ----
		const double kBaseOffset = 12.0;   // 基础偏移
		const double kChainSpacing = 8.0;  // 标注链间距
		int linearCount = 0;

		auto createLinearDim = [&](const DimPair& dp, int chainIndex, int totalH, int totalV)
		{
			NXOpen::Annotations::LinearDimensionBuilder* dimBuilder = NULL;
			NXOpen::NXObject* obj1 = static_cast<NXOpen::NXObject*>(NXOpen::NXObjectManager::Get(dp.e1.tag));
			NXOpen::NXObject* obj2 = static_cast<NXOpen::NXObject*>(NXOpen::NXObjectManager::Get(dp.e2.tag));
			NXOpen::DisplayableObject* disp1 = dynamic_cast<NXOpen::DisplayableObject*>(obj1);
			NXOpen::DisplayableObject* disp2 = dynamic_cast<NXOpen::DisplayableObject*>(obj2);
			if (!disp1 || !disp2) return;

			// 中点（SetValue 的拾取点，均为边上真实中点）
			double mid1[3], mid2[3];
			curve_midpoint(dp.e1, mid1);
			curve_midpoint(dp.e2, mid2);
			NXOpen::Point3d pick1(mid1[0], mid1[1], mid1[2]);
			NXOpen::Point3d pick2(mid2[0], mid2[1], mid2[2]);

			// ---- 确定放置点（全自动：图纸坐标） ----
			// 基础点：calc_linear_placement（沿边方向已取本对被测边中点，跟随自身几何）
			double offset = kBaseOffset + chainIndex * kChainSpacing;
			NXOpen::Point3d placePt = CommonUtils::calc_linear_placement(
				dp.s1, dp.e1d, dp.s2, dp.e2d, borders, offset);

			// ---- 错层放置：按被测跨距长度与序号奇偶附加层偏移，
			// 避免长短不一的标注挤在同一链线导致文字/尺寸界线重叠交叉 ----
			const double spanDx = (fabs(dp.e1d[0] - dp.s1[0]) + fabs(dp.e2d[0] - dp.s2[0])) / 2.0;
			const double spanDy = (fabs(dp.e1d[1] - dp.s1[1]) + fabs(dp.e2d[1] - dp.s2[1])) / 2.0;
			const bool horizEdges = (spanDx > spanDy);   // 与 calc_linear_placement 的方向判定一致
			const double edgeLen = (horizEdges ? spanDx : spanDy);   // 沿边方向跨距（图纸坐标）
			const double kLenLevelStep = 20.0;   // 跨距每 20mm 增加一层
			const double kStaggerSpacing = 4.0;  // 附加层间距
			const int extraLevel = (chainIndex % 2) + (int)(edgeLen / kLenLevelStep);
			const double extra = extraLevel * kStaggerSpacing;
			if (horizEdges) placePt.Y += extra;   // 垂直标注：附加层向图纸上方错开
			else            placePt.X += extra;   // 水平标注：附加层向图纸右方错开

			// ---- 图纸边界防护：越界时优先放入视图内侧（靠近被测边、逐条错开），
			// 再钳制到图纸内 —— NX 不渲染图纸外的标注 ----
			const double kSheetMargin = 2.0;
			const double kInsideStep = 6.0;
			bool outSheet = (placePt.X < kSheetMargin || placePt.X > sheetW - kSheetMargin ||
				placePt.Y < kSheetMargin || placePt.Y > sheetH - kSheetMargin);
			if (outSheet)
			{
				const double inOff = kInsideStep * (chainIndex + 1);
				if (placePt.Y > sheetH - kSheetMargin) placePt.Y = borders[3] - inOff;
				if (placePt.Y < kSheetMargin)          placePt.Y = borders[1] + inOff;
				if (placePt.X > sheetW - kSheetMargin) placePt.X = borders[2] - inOff;
				if (placePt.X < kSheetMargin)          placePt.X = borders[0] + inOff;
			}
			bool clamped = clamp_to_sheet(placePt, sheetW, sheetH, kSheetMargin);

			sprintf_s(fmt, sizeof(fmt),
				"[Step4] 线性标注放置: 边1中点(图纸)(%.2f, %.2f), 边2中点(图纸)(%.2f, %.2f) 跨距=%.1f 错层+%d(%.1fmm) -> 放置点(%.2f, %.2f) [%s]（图纸 %.0fx%.0f）",
				dp.dmid1[0], dp.dmid1[1], dp.dmid2[0], dp.dmid2[1], edgeLen,
				extraLevel, extra, placePt.X, placePt.Y,
				clamped ? "已钳制入图纸" : (outSheet ? "越界→移入视图内侧" : "界内"),
				sheetW, sheetH);
			CommonUtils::print_msg(fmt);

			const NXOpen::Point3d zeroPt(0.0, 0.0, 0.0);
			bool created = false;

			// ---- 主路径：LinearDimensionBuilder（7 参 SetValue，带 SnapType） ----
			// NX12 Commit 校验需要 snap 信息才能推断线性标注类型（水平/垂直/平行）；
			// 3 参 SetValue（NX10 已弃用）无 snap 信息会导致
			// "Unable to infer a dimension type from the selected associativities"
			try
			{
				dimBuilder = part->Dimensions()->CreateLinearDimensionBuilder(NULL);
				dimBuilder->FirstAssociativity()->SetValue(
					NXOpen::InferSnapType::SnapTypeMid, disp1, sectionView, pick1, NULL, NULL, zeroPt);
				dimBuilder->SecondAssociativity()->SetValue(
					NXOpen::InferSnapType::SnapTypeMid, disp2, sectionView, pick2, NULL, NULL, zeroPt);
				dimBuilder->Origin()->SetAnchor(NXOpen::Annotations::OriginBuilder::AlignmentPositionMidCenter);
				dimBuilder->Origin()->SetInferRelativeToGeometry(false);
				dimBuilder->Origin()->AnnotationView()->SetValue(sectionView);
				// 录制 VB 同款：显式放置前先 SetAssociativeOrigin(Drag)（000R_VB.vb 每个标注都有）
				{
					NXOpen::Annotations::Annotation::AssociativeOriginData ao;
					ao.OriginType = NXOpen::Annotations::AssociativeOriginTypeDrag;
					dimBuilder->Origin()->SetAssociativeOrigin(ao);
				}
				dimBuilder->Origin()->SetOriginPoint(placePt);
				dimBuilder->Commit();
				dimBuilder->Destroy();
				dimBuilder = NULL;
				created = true;
			}
			catch (const NXOpen::NXException& e)
			{
				if (dimBuilder) { dimBuilder->Destroy(); dimBuilder = NULL; }
				CommonUtils::print_msg(string("  警告: LinearDimensionBuilder 失败，回退 RapidDimensionBuilder: ") + e.Message());
			}
			catch (...)
			{
				if (dimBuilder) { dimBuilder->Destroy(); dimBuilder = NULL; }
				CommonUtils::print_msg("  警告: LinearDimensionBuilder 失败（未知异常），回退 RapidDimensionBuilder");
			}

			// ---- 回退路径：RapidDimensionBuilder（录制 VB 宏验证过的模式） ----
			if (!created)
			{
				NXOpen::Annotations::RapidDimensionBuilder* rapidBuilder = NULL;
				try
				{
					rapidBuilder = part->Dimensions()->CreateRapidDimensionBuilder(NULL);
					rapidBuilder->FirstAssociativity()->SetValue(
						NXOpen::InferSnapType::SnapTypeMid, disp1, sectionView, pick1, NULL, NULL, zeroPt);
					rapidBuilder->SecondAssociativity()->SetValue(
						NXOpen::InferSnapType::SnapTypeMid, disp2, sectionView, pick2, NULL, NULL, zeroPt);
					rapidBuilder->Origin()->SetAnchor(NXOpen::Annotations::OriginBuilder::AlignmentPositionMidCenter);
					rapidBuilder->Origin()->SetInferRelativeToGeometry(false);
					rapidBuilder->Origin()->AnnotationView()->SetValue(sectionView);
					{
						NXOpen::Annotations::Annotation::AssociativeOriginData ao;
						ao.OriginType = NXOpen::Annotations::AssociativeOriginTypeDrag;
						rapidBuilder->Origin()->SetAssociativeOrigin(ao);
					}
					rapidBuilder->Origin()->SetOriginPoint(placePt);
					rapidBuilder->Commit();
					rapidBuilder->Destroy();
					rapidBuilder = NULL;
					created = true;
				}
				catch (const NXOpen::NXException& e)
				{
					if (rapidBuilder) { rapidBuilder->Destroy(); rapidBuilder = NULL; }
					CommonUtils::print_msg(string("  警告: 线性标注创建失败（含回退）: ") + e.Message());
				}
				catch (...)
				{
					if (rapidBuilder) { rapidBuilder->Destroy(); rapidBuilder = NULL; }
					CommonUtils::print_msg("  警告: 线性标注创建失败（含回退，未知异常）");
				}
			}

			if (created)
			{
				++linearCount;
				sprintf_s(fmt, sizeof(fmt), "[Step4] 已创建线性标注 第 %d 条（水平组 %d / 垂直组 %d）",
					linearCount, totalH, totalV);
				CommonUtils::print_msg(fmt);
			}
		};

		// 创建水平标注
		for (size_t i = 0; i < hDims.size(); ++i)
		{
			createLinearDim(hDims[i], (int)i, (int)hDims.size(), (int)vDims.size());
		}
		// 创建垂直标注
		for (size_t i = 0; i < vDims.size(); ++i)
		{
			createLinearDim(vDims[i], (int)i, (int)hDims.size(), (int)vDims.size());
		}

		sprintf_s(fmt, sizeof(fmt), "[Step4] 共创建线性标注 %d 条（水平 %d / 垂直 %d）",
			linearCount, (int)hDims.size(), (int)vDims.size());
		CommonUtils::print_msg(fmt);
	}

	// =========================================================================
	// Phase 2: 直径/半径标注
	// =========================================================================
	if (existingRadial > 0)
	{
		CommonUtils::print_msg("[Step4] 直径标注已存在，跳过直径创建（线性标注仍执行）");
	}
	else if (arcs.empty())
	{
		CommonUtils::print_msg("[Step4] 无圆弧，跳过直径标注");
	}
	else
	{
		// ---- 视图比例：模型半径 × 比例 = 图纸坐标半径 ----
		const double viewScale = sectionView->Scale();
		sprintf_s(fmt, sizeof(fmt), "[Step4] 剖视图比例: %.3f（图纸半径 = 模型半径 × 比例）", viewScale);
		CommonUtils::print_msg(fmt);

		// ---- 过滤与去重 ----
		const double kMinRadius = 1.0;
		const double kDupTol = 0.01;
		double viewSize = sqrt(
			(borders[2] - borders[0]) * (borders[2] - borders[0]) +
			(borders[3] - borders[1]) * (borders[3] - borders[1]));

		std::vector<CommonUtils::CurveInfo> uniqueArcs;
		for (size_t i = 0; i < arcs.size(); ++i)
		{
			const double drawR = arcs[i].arc_radius * viewScale;  // 图纸坐标半径
			if (drawR < kMinRadius) continue;
			if (drawR > viewSize) continue;

			// 去重：与已有圆弧比较中心和半径
			bool dup = false;
			for (size_t j = 0; j < uniqueArcs.size(); ++j)
			{
				double dx = arcs[i].arc_center[0] - uniqueArcs[j].arc_center[0];
				double dy = arcs[i].arc_center[1] - uniqueArcs[j].arc_center[1];
				double dz = arcs[i].arc_center[2] - uniqueArcs[j].arc_center[2];
				double dr = fabs(arcs[i].arc_radius - uniqueArcs[j].arc_radius);
				if (sqrt(dx * dx + dy * dy + dz * dz) < kDupTol && dr < kDupTol)
				{
					dup = true;
					break;
				}
			}
			if (!dup) uniqueArcs.push_back(arcs[i]);
		}

		sprintf_s(fmt, sizeof(fmt), "[Step4] 去重后圆弧 %d 个（原始 %d 个）",
			(int)uniqueArcs.size(), (int)arcs.size());
		CommonUtils::print_msg(fmt);

		// ---- 创建直径标注 ----
		const double kRadialOffset = 12.0;
		int radialCount = 0;

		for (size_t i = 0; i < uniqueArcs.size(); ++i)
		{
			NXOpen::Annotations::RadialDimensionBuilder* dimBuilder = NULL;
			try
			{
				NXOpen::NXObject* obj = static_cast<NXOpen::NXObject*>(NXOpen::NXObjectManager::Get(uniqueArcs[i].tag));
				NXOpen::DisplayableObject* disp = dynamic_cast<NXOpen::DisplayableObject*>(obj);
				if (!disp) continue;

				// 圆弧中点（parm=0.5）
				double mid[3];
				curve_midpoint(uniqueArcs[i], mid);
				NXOpen::Point3d arcMid(mid[0], mid[1], mid[2]);

				// 映射圆弧几何到图纸坐标（圆心与 parm=0.5 中点）
				DrawGeom ag = map_curve_geom(sectionView->Tag(), uniqueArcs[i]);
				if (!ag.ok)
				{
					CommonUtils::print_msg("  警告: 圆弧坐标映射到图纸失败，跳过该直径标注");
					continue;
				}
				const double drawRadius = uniqueArcs[i].arc_radius * viewScale;

				// ---- 确定放置点（全自动：图纸坐标 圆心 + 径方向 × (图纸半径 + offset)） ----
				NXOpen::Point3d placePt = CommonUtils::calc_radial_placement(
					ag.center, ag.mid, drawRadius, borders, kRadialOffset);

				// ---- 图纸边界防护：越界时移入视图内侧，再钳制到图纸内 ----
				const double kSheetMargin = 2.0;
				bool outSheet = (placePt.X < kSheetMargin || placePt.X > sheetW - kSheetMargin ||
					placePt.Y < kSheetMargin || placePt.Y > sheetH - kSheetMargin);
				if (outSheet)
				{
					if (placePt.Y > sheetH - kSheetMargin) placePt.Y = borders[3] - 6.0;
					if (placePt.Y < kSheetMargin)          placePt.Y = borders[1] + 6.0;
					if (placePt.X > sheetW - kSheetMargin) placePt.X = borders[2] - 6.0;
					if (placePt.X < kSheetMargin)          placePt.X = borders[0] + 6.0;
				}
				bool clamped = clamp_to_sheet(placePt, sheetW, sheetH, kSheetMargin);

				sprintf_s(fmt, sizeof(fmt),
					"[Step4] 直径标注放置: 模型中心(%.2f, %.2f, %.2f) 半径=%.2f -> 图纸中心(%.2f, %.2f) 图纸半径=%.2f, 放置点(%.2f, %.2f) [%s]",
					uniqueArcs[i].arc_center[0], uniqueArcs[i].arc_center[1], uniqueArcs[i].arc_center[2],
					uniqueArcs[i].arc_radius, ag.center[0], ag.center[1], drawRadius, placePt.X, placePt.Y,
					clamped ? "已钳制入图纸" : (outSheet ? "越界→移入视图内侧" : "界内"));
				CommonUtils::print_msg(fmt);

				dimBuilder = part->Dimensions()->CreateRadialDimensionBuilder(NULL);
				dimBuilder->FirstAssociativity()->SetValue(disp, sectionView, arcMid);
				dimBuilder->SetHoleStyle(true);
				dimBuilder->Origin()->SetAnchor(NXOpen::Annotations::OriginBuilder::AlignmentPositionMidCenter);
				dimBuilder->Origin()->SetInferRelativeToGeometry(false);
				dimBuilder->Origin()->AnnotationView()->SetValue(sectionView);
				{
					NXOpen::Annotations::Annotation::AssociativeOriginData ao;
					ao.OriginType = NXOpen::Annotations::AssociativeOriginTypeDrag;
					dimBuilder->Origin()->SetAssociativeOrigin(ao);
				}
				dimBuilder->Origin()->SetOriginPoint(placePt);
				dimBuilder->Commit();
				dimBuilder->Destroy();
				dimBuilder = NULL;
				++radialCount;

				sprintf_s(fmt, sizeof(fmt), "[Step4] 已创建直径标注 第 %d 条", radialCount);
				CommonUtils::print_msg(fmt);
			}
			catch (const NXOpen::NXException& e)
			{
				if (dimBuilder) { dimBuilder->Destroy(); dimBuilder = NULL; }
				CommonUtils::print_msg(string("  警告: 直径标注创建失败: ") + e.Message());
			}
			catch (...)
			{
				if (dimBuilder) { dimBuilder->Destroy(); dimBuilder = NULL; }
				CommonUtils::print_msg("  警告: 直径标注创建失败（未知异常）");
			}
		}

		sprintf_s(fmt, sizeof(fmt), "[Step4] 共创建直径标注 %d 条", radialCount);
		CommonUtils::print_msg(fmt);
	}
}

//==============================================================================
// do_it —— Step4 入口逻辑
//==============================================================================
static void do_it()
{
	try
	{
		NXOpen::Session* session = CommonUtils::get_session();
		NXOpen::Part* part = dynamic_cast<NXOpen::Part*>(session->Parts()->BaseWork());
		if (!part) { CommonUtils::print_msg("错误：无工作部件"); return; }

		// ===== 进入制图环境 =====
		session->ApplicationSwitchImmediate("UG_APP_DRAFTING");
		part->Drafting()->EnterDraftingApplication();
		CommonUtils::print_msg("[Step4] 已进入制图模块");

		// ===== 激活图纸 =====
		NXOpen::Drawings::DraftingDrawingSheet* sheet = NULL;
		for (NXOpen::Drawings::DraftingDrawingSheetCollection::iterator it =
			part->DraftingDrawingSheets()->begin();
			it != part->DraftingDrawingSheets()->end(); ++it)
		{
			NXOpen::Drawings::DraftingDrawingSheet* s = *it;
			if (s) { s->Open(); sheet = s; break; }
		}
		if (!sheet) { CommonUtils::print_msg("[Step4] 警告：未找到任何图纸，请先运行 Step1"); return; }
		CommonUtils::print_msg("[Step4] 已激活图纸");
		
		// ---- 诊断：列举图纸上所有视图（排查多余视图致标注挂载错误） ----
		{
			std::vector<NXOpen::Drawings::DraftingView*> sheetViews = sheet->GetDraftingViews();
			const int nViews = (int)sheetViews.size();
			char fmt[256];
			sprintf_s(fmt, sizeof(fmt),
				"[Step4 视图诊断] 图纸 \"%s\" 上共有 %d 个视图:",
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
		
		// ===== 查找剖视图 =====
		NXOpen::Drawings::SectionView* sectionView = CommonUtils::find_section_view(part);
		if (!sectionView) { CommonUtils::print_msg("[Step4] 未找到剖视图，请先运行 Step1"); return; }

		// ===== 图纸尺寸（放置点图纸内钳制依赖） =====
		double sheetLen = 297.0, sheetHgt = 210.0;
		if (!CommonUtils::ask_sheet_actual_size(sheet, sheetLen, sheetHgt))
		{
			CommonUtils::print_msg("[Step4] 警告: 图纸尺寸获取失败，使用默认 297x210");
		}
		{
			char sf[192];
			sprintf_s(sf, sizeof(sf),
				"[Step4] 图纸尺寸: %.2f x %.2f（图纸坐标范围 [0..%.0f] x [0..%.0f]）",
				sheetLen, sheetHgt, sheetLen, sheetHgt);
			CommonUtils::print_msg(sf);
		}

		// ===== 获取视图边界 =====
		double borders[4] = { 0.0, 0.0, 0.0, 0.0 };
		int rc = UF_DRAW_ask_view_borders(sectionView->Tag(), borders);
		if (rc != 0)
		{
			char fmt[512];
			sprintf_s(fmt, sizeof(fmt), "[Step4] 警告: UF_DRAW_ask_view_borders 返回 %d", rc);
			CommonUtils::print_msg(fmt);
			return;
		}

		char fmt[512];
		sprintf_s(fmt, sizeof(fmt), "[Step4] 视图边界: (%.2f, %.2f) - (%.2f, %.2f)",
			borders[0], borders[1], borders[2], borders[3]);
		CommonUtils::print_msg(fmt);
		if (borders[0] < 0.0 || borders[1] < 0.0 ||
			borders[2] > sheetLen || borders[3] > sheetHgt)
		{
			CommonUtils::print_msg(
				"[Step4] 警告: 视图边界超出图纸范围（Step1 放置问题），标注放置点将钳制到图纸内");
		}

		// ===== 执行自动标注 =====
		phase_auto_linear_dims(part, sectionView, borders, sheetLen, sheetHgt);

		// ===== 静默更新 =====
		CommonUtils::silent_update(session);

		sprintf_s(fmt, sizeof(fmt), "[Step4] 全自动尺寸标注 完成");
		CommonUtils::print_msg(fmt);
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
