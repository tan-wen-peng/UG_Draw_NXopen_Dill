//------------------------------------------------------------------------------
// NX12 Step7：尺寸后缀标号（DLL 7/7）
// 功能：只处理"当前工作/活动图纸"（NX 界面中当前打开显示的那张），
//       为该图纸剖视图上已存在的线性/直径尺寸标注自动追加后缀标号
//       (D1)/(L1)...；其余图纸一律不处理、不改动。
// 依据：录制宏 D:\A_UG\02_study\尺寸后面加标号.vb（15 条尺寸逐个编辑：
//       CreateLinearDimensionBuilder(已有尺寸) → AppendedText.SetAfter → Commit）
//------------------------------------------------------------------------------

// Win32 防护 + 共享头文件
#include "../Shared/NX12_CommonConfig.h"
#include "../Shared/NX12_CommonUtils.h"

// Step7 专有 includes
#include <NXOpen/Annotations_LinearDimensionBuilder.hxx>  // VB CreateLinearDimensionBuilder(dim) 编辑已有线性尺寸
#include <NXOpen/Annotations_RadialDimensionBuilder.hxx>  // 直径/半径标注编辑（线性 Builder 不适用）
#include <NXOpen/Annotations_AppendedTextBuilder.hxx>     // AppendedText().SetAfter() 后缀写入
#include <NXOpen/Annotations_Annotation.hxx>              // GetAssociativeOrigin / AssociativeOriginData（视图过滤）
#include <NXOpen/Annotations_Dimension.hxx>
#include <NXOpen/Annotations_GeneralHorizontalDimension.hxx>    // 类型名诊断
#include <NXOpen/Annotations_GeneralVerticalDimension.hxx>
#include <NXOpen/Annotations_GeneralParallelDimension.hxx>
#include <NXOpen/Annotations_GeneralPerpendicularDimension.hxx>
#include <NXOpen/Annotations_GeneralDiameterDimension.hxx>
#include <NXOpen/NXMessageBox.hxx>
#include <NXOpen/DraftingManager.hxx>
#include <uf_object_types.h>    // UF_dimension_type / UF_dim_*_subtype（类型白名单与排除）
#include <uf_draw.h>            // UF_DRAW_ask_view_borders（位置兜底过滤）
#include <uf_drf.h>             // UF_DRF_ask_associative_origin / UF_DRF_ask_associativity_data（大判断图纸归属通道）
#include <algorithm>            // std::sort（编号空间排序）
#include <map>                  // 视图归属诊断统计
#include <set>                  // 目标图纸视图 tag 集合（图纸维度过滤）

//==============================================================================
// 目标标注条目（剖视图过滤后进入编号队列的尺寸）
//==============================================================================
struct TargetDim
{
	NXOpen::Annotations::Dimension* dim;  // 尺寸对象
	int   subtype;                        // UF_dim_*_subtype（决定编辑 Builder 与日志类型名）
	double sortKey1;                      // 主排序键（组内编号顺序）
	double sortKey2;                      // 次排序键
	tag_t tag;                            // 最终稳定键（前两键全同时按 tag 排，保证确定性）
};

//==============================================================================
// 辅助：UF 子类型 → 可读类型名（日志用）
//==============================================================================
static const char* dim_type_name(int subtype)
{
	switch (subtype)
	{
	case UF_dim_horizontal_subtype:    return "水平(测径向)";
	case UF_dim_vertical_subtype:      return "垂直(测轴向)";
	case UF_dim_parallel_subtype:      return "平行";
	case UF_dim_perpendicular_subtype: return "正交";
	case UF_dim_diameter_subtype:      return "直径";
	case UF_dim_radius_subtype:        return "半径";
	default:                           return "其它";
	}
}

//==============================================================================
// 为单条已有尺寸设置后缀（修改已有标注的附加文本属性，不新建尺寸）
//
// VB 映射（000R 宏 L69-209 单条完整序列）：
//   FindObject("HANDLE R-x") → 已有尺寸对象
//   CreateLinearDimensionBuilder(dim)      → part->Dimensions()->CreateLinearDimensionBuilder(dim)
//   AppendedText.SetAfter(lines(0)="(D1)") → builder->AppendedText()->SetAfter(vector<NXString>)
//   Commit() / Destroy()
// 差异说明：
//   1) 宏中 Driving.DrivingMethod=Reference 属用户改参考尺寸的独立操作，
//      与"追加标号"目标无关，本模块不复现（避免尺寸被意外改为参考尺寸加括号）。
//   2) 直径/半径标注（subtype 9/10）必须用 CreateRadialDimensionBuilder 编辑，
//      CreateLinearDimensionBuilder 仅接受线性尺寸（宏中无直径标注故未体现）。
//   3) SetAfter 为全量替换语义：重复运行同规则产生相同后缀，天然幂等。
//==============================================================================
static bool append_suffix_to_dim(NXOpen::Part* part,
	NXOpen::Annotations::Dimension* dim,
	int subtype, const char* suffix)
{
	char fmt[384];
	try
	{
		std::vector<NXOpen::NXString> lines;
		lines.push_back(NXOpen::NXString(suffix));   // "(D1)" 等 ASCII 后缀

		if (subtype == UF_dim_diameter_subtype || subtype == UF_dim_radius_subtype)
		{
			NXOpen::Annotations::RadialDimensionBuilder* rb =
				part->Dimensions()->CreateRadialDimensionBuilder(dim);
			rb->AppendedText()->SetAfter(lines);
			rb->Commit();
			rb->Destroy();
			rb = NULL;
		}
		else
		{
			NXOpen::Annotations::LinearDimensionBuilder* lb =
				part->Dimensions()->CreateLinearDimensionBuilder(dim);
			lb->AppendedText()->SetAfter(lines);
			lb->Commit();
			lb->Destroy();
			lb = NULL;
		}
		return true;
	}
	catch (const NXOpen::NXException& e)
	{
		sprintf_s(fmt, sizeof(fmt), "  警告: 尺寸 tag=%llu 后缀 \"%s\" 设置失败: %s",
			(unsigned long long)dim->Tag(), suffix, e.Message());
		CommonUtils::print_msg(fmt);
		return false;
	}
	catch (...)
	{
		sprintf_s(fmt, sizeof(fmt), "  警告: 尺寸 tag=%llu 后缀 \"%s\" 设置失败（未知异常）",
			(unsigned long long)dim->Tag(), suffix);
		CommonUtils::print_msg(fmt);
		return false;
	}
}

//==============================================================================
// 大判断辅助：标注归属信息（classify_dim_sheet 的输出，含诊断字段）
//==============================================================================
struct SheetClsInfo
{
	int   result;        // 0=未知 1=明确当前图纸 2=明确其它图纸
	int   channel;       // 命中通道：1=关联原点 2=关联数据 3=GetViews 0=全部失败
	int   originType;    // UF_DRF_associative_origin_type_t（通道1诊断）
	int   numAssoc;      // 关联数据条数（通道2诊断）
	tag_t firstObjView;  // 首条关联数据的 object_view（通道2诊断）
	int   blankStatus;   // UF_OBJ_ask_display_properties 显示状态（诊断）
	int   layer;         // 图层（诊断）
};

//==============================================================================
// map_view_to_sheet —— 把标注关联的视图/图纸 tag 映射为图纸归属
// 返回：0=无法判定，1=当前图纸，2=其它图纸
// v 可能是：图纸 tag（图纸级标注直接挂在图纸上，UF_drawing_type=62）、
// 制图成员视图 tag（经 UF_DRAW_ask_drawing_of_view 求所属图纸），
// 或模型视图 tag（无图纸归属，返回 0）。
//==============================================================================
static int map_view_to_sheet(tag_t v, tag_t curSheetTag)
{
	if (v == NULL_TAG || curSheetTag == NULL_TAG) return 0;
	int t = 0, s = 0;
	UF_OBJ_ask_type_and_subtype(v, &t, &s);
	if (t == UF_drawing_type)
		return (v == curSheetTag) ? 1 : 2;   // 图纸级标注直接挂在图纸对象上
	tag_t drawing = NULL_TAG;
	if (UF_DRAW_ask_drawing_of_view(v, &drawing) == 0 && drawing != NULL_TAG)
		return (drawing == curSheetTag) ? 1 : 2;
	return 0;
}

//==============================================================================
// classify_dim_sheet —— 判定尺寸标注所属图纸（大判断核心）
// 背景：NXOpen Annotation::GetViews() 在 NX12 中对制图标注会抛内部错误
//       3655004（"Object is not a displayable PMI"，该接口实际只对 PMI 显示
//       实例可用），上一版大判断因此全部判"未知"而失效（日志实测
//       "明确当前图纸 0 / 明确其它图纸 0 / 未知 229"）。本版改为 UF 层
//       原始 tag 通道，绕过 NXOpen 的 View 指针包装：
//   通道1：UF_DRF_ask_associative_origin → view_eid / view_of_geometry /
//          associated_view 原始 tag（NXOpen GetAssociativeOrigin 包装 View
//          指针时对非显示图纸的视图返回 NULL，原始 tag 不受此影响）
//   通道2：UF_DRF_ask_associativity_data → 各条关联数据的 object_view
//          （"Drawing or drafting member view of the associated objects"，
//          即标注所测几何对象所在的图纸/制图视图——标注挂靠视图的
//          权威依据，与放置位置无关）
//   通道3：NXOpen GetViews()（保留兜底，个别可用则用）
// 全部通道失败返回 0=未知（走位置兜底）。
//==============================================================================
static void classify_dim_sheet(NXOpen::Annotations::Dimension* d, tag_t curSheetTag, SheetClsInfo& info)
{
	info.result = 0;
	info.channel = 0;
	info.originType = -1;
	info.numAssoc = -1;
	info.firstObjView = 0;
	info.blankStatus = -1;
	info.layer = -1;
	if (!d || curSheetTag == NULL_TAG) return;
	tag_t dtag = d->Tag();

	// 诊断：显示状态与图层（仅记录，不参与判定）
	{
		UF_OBJ_disp_props_t props;
		if (UF_OBJ_ask_display_properties(dtag, &props) == 0)
		{
			info.blankStatus = props.blank_status;
			info.layer = props.layer;
		}
	}

	// 通道1：关联原点原始视图 tag
	{
		UF_DRF_associative_origin_p_t od = NULL;
		double origin[3] = { 0.0, 0.0, 0.0 };
		if (UF_DRF_ask_associative_origin(dtag, &od, origin) == 0 && od)
		{
			info.originType = (int)od->origin_type;
			tag_t v = NULL_TAG;
			if (od->origin_type == UF_DRF_ORIGIN_RELATIVE_TO_VIEW)        v = od->view_eid;
			else if (od->origin_type == UF_DRF_ORIGIN_RELATIVE_TO_GEOMETRY) v = od->view_of_geometry;
			else if (od->origin_type == UF_DRF_ORIGIN_AT_A_POINT)          v = od->associated_view;
			UF_free(od);
			int r = map_view_to_sheet(v, curSheetTag);
			if (r != 0) { info.result = r; info.channel = 1; return; }
		}
	}

	// 通道2：关联数据 object_view（标注测量的几何所在视图）
	{
		int nAssoc = 0;
		UF_DRF_object_assoc_data_p_t assoc = NULL;
		if (UF_DRF_ask_associativity_data(dtag, &nAssoc, &assoc) == 0 && assoc && nAssoc > 0)
		{
			info.numAssoc = nAssoc;
			info.firstObjView = assoc[0].object_view;
			for (int i = 0; i < nAssoc; ++i)
			{
				int r = map_view_to_sheet(assoc[i].object_view, curSheetTag);
				if (r != 0)
				{
					UF_free(assoc);
					info.result = r;
					info.channel = 2;
					return;
				}
			}
			UF_free(assoc);
		}
	}

	// 通道3：NXOpen GetViews（对制图标注可能抛 3655004，catch 后作罢）
	try
	{
		std::vector<NXOpen::View*> views = d->GetViews();
		for (size_t i = 0; i < views.size(); ++i)
		{
			int r = map_view_to_sheet(views[i] ? views[i]->Tag() : NULL_TAG, curSheetTag);
			if (r != 0) { info.result = r; info.channel = 3; return; }
		}
	}
	catch (...) {}
}

//==============================================================================
// process_sheet —— 单张图纸处理：定位剖视图、过滤目标尺寸、按图纸独立编号
// 编号空间按图纸独立：D/L 组各自从 (D1)/(L1) 起；SetAfter 全量替换，
// 重跑自动覆盖本图纸旧后缀（幂等），不会沿用其它图纸的旧序号。
//==============================================================================
static void process_sheet(NXOpen::Part* part, NXOpen::Drawings::DraftingDrawingSheet* sheet)
{
	try
	{
		char fmt[512];

		// 图纸名：Name() 的 NXString 可能是 Locale 模式，经 GetUTF8Text()
		// 统一转 UTF-8 字节，与源文件 UTF-8 字面量拼接后按 char* 路径
		// 打印（ListingWindow 按 UTF-8 解读）；避免 NXString 拼接的模式
		// 混用导致名字与引号丢失。
		std::string nameLine = "[Step7] 当前工作图纸 \"";
		nameLine += sheet->Name().GetUTF8Text();
		nameLine += "\"";
		CommonUtils::print_msg(nameLine);
		{
			int shtType = 0, shtSub = 0;
			UF_OBJ_ask_type_and_subtype(sheet->Tag(), &shtType, &shtSub);
			sprintf_s(fmt, sizeof(fmt),
				"[Step7] 当前图纸 tag=%llu UF类型=%d/%d（62=UF_drawing_type 图纸对象）",
				(unsigned long long)sheet->Tag(), shtType, shtSub);
			CommonUtils::print_msg(fmt);
		}

		// ---- 目标图纸视图列表 + 视图诊断（图纸维度过滤的基础） ----
		std::vector<NXOpen::Drawings::DraftingView*> sheetViews = sheet->GetDraftingViews();
		std::set<tag_t> sheetViewTags;   // 目标图纸视图 tag 集合
		sprintf_s(fmt, sizeof(fmt), "[Step7 视图诊断] 图纸上共有 %d 个视图:",
			(int)sheetViews.size());
		CommonUtils::print_msg(fmt);
		for (size_t idx = 0; idx < sheetViews.size(); ++idx)
		{
			NXOpen::Drawings::DraftingView* v = sheetViews[idx];
			if (!v) continue;
			sheetViewTags.insert(v->Tag());
			const char* vtype = "DraftingView";
			if (dynamic_cast<NXOpen::Drawings::SectionView*>(v))
				vtype = "SectionView(剖视)";
			else if (dynamic_cast<NXOpen::Drawings::BaseView*>(v))
				vtype = "BaseView(基础/载体)";
			sprintf_s(fmt, sizeof(fmt), "  [%d] tag=%llu 类型=%s",
				(int)idx, (unsigned long long)v->Tag(), vtype);
			CommonUtils::print_msg(fmt);
		}

		// ===== 定位剖视图（限定在目标图纸视图内查找，避免多图纸时全局
		//       find_section_view 命中其它图纸的剖视图） =====
		NXOpen::Drawings::SectionView* sectionView = NULL;
		for (size_t idx = 0; idx < sheetViews.size(); ++idx)
		{
			NXOpen::Drawings::SectionView* sv =
				dynamic_cast<NXOpen::Drawings::SectionView*>(sheetViews[idx]);
			if (sv) { sectionView = sv; break; }
		}
		if (!sectionView) { CommonUtils::print_msg("[Step7] 当前图纸上没有剖视图，未做任何改动"); return; }
		sprintf_s(fmt, sizeof(fmt), "[Step7] 目标剖视图 tag=%llu（后缀只作用于该视图上的标注）",
			(unsigned long long)sectionView->Tag());
		CommonUtils::print_msg(fmt);

		// 剖视图边界（位置兜底判据，兼容无视图归属的手工标注）
		double sectBorder[4] = { 0.0, 0.0, 0.0, 0.0 };
		bool haveSectBorder = (UF_DRAW_ask_view_borders(sectionView->Tag(), sectBorder) == 0);
		double fallbackMargin = 5.0;
		if (haveSectBorder)
		{
			double bw = sectBorder[2] - sectBorder[0];
			double bh = sectBorder[3] - sectBorder[1];
			double fivePct = 0.05 * (bw > bh ? bw : bh);
			if (fivePct > fallbackMargin) fallbackMargin = fivePct;
		}

		// ===== 枚举尺寸标注并过滤（大判断 图纸归属 + 多层防线，防止误改其它图纸/视图/类型） =====
		// 大判断（图纸归属，循环入口提前执行）：每条标注先经 UF 层原始
		//         tag 通道（关联原点 view_eid / 关联数据 object_view，详见
		//         classify_dim_sheet）判定所属图纸，明确属于其它图纸的直接
		//         排除——即使其它图纸标注的放置位置与当前图纸完全相同，
		//         也不会进入后续任何判断、不会被改动；属于当前图纸或无法
		//         判定的继续走以下防线。
		// 防线1（剖视图维度 + 位置兜底）：
		//   主判据：View tag == 剖视图 tag（View 为 NULL 时 ViewOfGeometry == 剖视图）；
		//   兜底：View/ViewOfGeometry 均 NULL、挂在目标图纸其它视图（如基础视图——
		//         剖视图标注的常见挂载形态）或 GetAssociativeOrigin 异常时，
		//         改判放置点是否落在剖视图边界矩形（UF_DRAW_ask_view_borders）
		//         内（含 5% 余量）；异常时放置点改用 AnnotationOrigin()。
		// 防线2（类型白名单）：subtype ∈ {水平1, 平行3, 正交5, 直径10, 半径9} → D 组
		//                     subtype ∈ {垂直2} → L 组
		// 防线3（排除）：坐标标注 subtype 13/14/18 与角度/弧长等其它类型一律不动
		//              （录制宏中仅对线性/直径标注追加后缀，坐标标注无此操作）
		//
		// 分组语义（映射自录制宏）：
		//   宏中 12 条水平/正交标注编 (D1)~(D11)、3 条垂直标注编 (L1)~(L3)。
		//   车床件剖视图中水平标注测径向距离（直径方向 → D），垂直标注测
		//   轴向距离（长度方向 → L），直径标注本身即 D 语义，故归 D 组。
		// 宏的手工瑕疵：D7 重复出现、D6/D7 错序——自动编号按空间排序
		// 连续递增，已消除重复与错序。
		std::vector<TargetDim> dGroup, lGroup;   // D 组(径向) / L 组(轴向)
		int otherSheetCount = 0;                 // 其它图纸上的尺寸（大判断/防线0排除）
		int otherViewCount = 0;                  // 本图纸其它视图上的尺寸（防线1排除）
		int sheetClsCur = 0;                     // 图纸归属判定：明确当前图纸
		int sheetClsOther = 0;                   // 明确其它图纸（大判断排除）
		int sheetClsUnknown = 0;                 // 未知（无视图可查，走位置兜底）
		int ch1Hit = 0, ch2Hit = 0, ch3Hit = 0;  // 大判断各通道命中统计
		int sheetDiagCount = 0;                  // 未知归属诊断打印条数（限 10 条）
		int ordinateCount  = 0;                  // 坐标标注（防线3排除）
		int otherTypeCount = 0;                  // 角度/弧长等其它类型（防线2/3排除）
		int lDiagCount     = 0;                  // 垂直(L 组)未命中诊断条数（限 10 条）
		// 视图归属诊断统计（防线1）
		std::map<tag_t, int> viewTagDist;        // View tag 分布
		std::map<tag_t, int> geomTagDist;        // ViewOfGeometry tag 分布
		int noViewCount      = 0;                // 位置兜底尝试条数（无视图归属或查询异常）
		int gaExceptionCount = 0;                // GetAssociativeOrigin 异常
		int posFallbackHit   = 0;                // 位置兜底命中

		for (NXOpen::Annotations::DimensionCollection::iterator it = part->Dimensions()->begin();
			it != part->Dimensions()->end(); ++it)
		{
			NXOpen::Annotations::Dimension* d = *it;
			if (!d) continue;

			// UF 类型/子类型
			int t = 0, s = 0;
			UF_OBJ_ask_type_and_subtype(d->Tag(), &t, &s);
			if (t != UF_dimension_type) { ++otherTypeCount; continue; }

			// ★ 大判断（图纸归属，提前执行）：先判定标注所属图纸。多张图纸
			// 的剖视图边界在各自坐标空间里可能重合，其它图纸标注的放置位置
			// 可能与当前图纸完全相同，必须在任何后续处理之前截住——明确属于
			// 其它图纸的直接排除，不再参与剖视图/位置等任何判断。
			SheetClsInfo clsInfo;
			classify_dim_sheet(d, sheet->Tag(), clsInfo);
			int sheetCls = clsInfo.result;
			if (sheetCls == 1) ++sheetClsCur;
			else if (sheetCls == 2) ++sheetClsOther;
			else ++sheetClsUnknown;
			if (clsInfo.channel == 1) ++ch1Hit;
			else if (clsInfo.channel == 2) ++ch2Hit;
			else if (clsInfo.channel == 3) ++ch3Hit;
			if (sheetCls == 0 && sheetDiagCount < 10)
			{
				++sheetDiagCount;
				sprintf_s(fmt, sizeof(fmt),
					"[Step7 大判断诊断] 未知归属 tag=%llu originType=%d 关联数=%d object_view=%llu 显示状态=%d 图层=%d",
					(unsigned long long)d->Tag(), clsInfo.originType, clsInfo.numAssoc,
					(unsigned long long)clsInfo.firstObjView, clsInfo.blankStatus, clsInfo.layer);
				CommonUtils::print_msg(fmt);
			}
			if (sheetCls == 2)
			{
				++otherSheetCount;
				if (s == UF_dim_vertical_subtype && lDiagCount < 10)
				{
					++lDiagCount;
					sprintf_s(fmt, sizeof(fmt),
						"[Step7 L组诊断] 垂直标注 tag=%llu -> 排除: 其它图纸（大判断）",
						(unsigned long long)d->Tag());
					CommonUtils::print_msg(fmt);
				}
				continue;
			}

			// 防线3：排除坐标标注（OrdinateDimension 族，Step3 产物）
			if (s == UF_dim_ordinate_horiz_subtype || s == UF_dim_ordinate_vert_subtype ||
				s == UF_dim_ordinate_origin_subtype)
			{
				++ordinateCount;
				continue;
			}

			// 防线2：类型白名单分组
			bool isD = (s == UF_dim_horizontal_subtype || s == UF_dim_parallel_subtype ||
				s == UF_dim_perpendicular_subtype || s == UF_dim_diameter_subtype ||
				s == UF_dim_radius_subtype);
			bool isL = (s == UF_dim_vertical_subtype);
			if (!isD && !isL) { ++otherTypeCount; continue; }

			// 防线1：图纸维度 + 剖视图维度 + 位置兜底
			NXOpen::Point3d originPt(0.0, 0.0, 0.0);
			tag_t viewTag = 0, geomTag = 0;
			int viewState = -1;   // -1=查询异常 0=无目标图纸视图归属 1=本图纸其它视图 2=剖视图
			try
			{
				NXOpen::Annotations::Annotation::AssociativeOriginData od =
					d->GetAssociativeOrigin(&originPt);
				viewTag = od.View ? od.View->Tag() : 0;
				geomTag = od.ViewOfGeometry ? od.ViewOfGeometry->Tag() : 0;
				if (viewTag == sectionView->Tag() ||
					(viewTag == 0 && geomTag == sectionView->Tag()))
				{
					viewState = 2;
				}
				else if (sheetViewTags.find(viewTag) != sheetViewTags.end() ||
					sheetViewTags.find(geomTag) != sheetViewTags.end())
				{
					viewState = 1;   // 目标图纸内其它视图（如基础视图）
				}
				else
				{
					viewState = 0;   // 全 NULL 或明确挂在其它图纸视图上
				}
				++viewTagDist[viewTag];
				++geomTagDist[geomTag];
			}
			catch (...)
			{
				++gaExceptionCount;
				viewState = -1;
			}

			const char* skipReason = NULL;
			bool onSection = (viewState == 2);
			if (!onSection)
			{
				// 防线0：图纸维度——明确挂在目标图纸之外视图上的标注直接排除
				// （大判断已截住明确其它图纸的标注，此处为视图维度的第二道保险）
				if (viewState == 0 && (viewTag != 0 || geomTag != 0))
				{
					++otherSheetCount;
					skipReason = "其它图纸";
				}
				else
				{
					// 位置兜底（图纸归属已在大判断处过滤；此处 viewState==1
					// 本图纸其它视图、viewState==0 且全 NULL、或 -1 查询异常）：
					// 改判放置点是否落在剖视图边界矩形内；异常时改用
					// AnnotationOrigin()。
					// 注：剖视图上的标注常挂在基础视图（父视图）上，此类标注
					//     viewState==1，须靠位置兜底收入目标组。
					if (viewState == -1)
					{
						try { originPt = d->AnnotationOrigin(); } catch (...) {}
					}
					++noViewCount;
					if (haveSectBorder &&
						originPt.X >= sectBorder[0] - fallbackMargin &&
						originPt.X <= sectBorder[2] + fallbackMargin &&
						originPt.Y >= sectBorder[1] - fallbackMargin &&
						originPt.Y <= sectBorder[3] + fallbackMargin)
					{
						onSection = true;
						++posFallbackHit;
					}
					else
					{
						++otherViewCount;
						skipReason = (viewState == 1) ? "本图纸其它视图" : "位置兜底未命中";
					}
				}
			}
			if (!onSection)
			{
				// L 组（垂直）未命中诊断：定位垂直标注被排除的原因
				if (s == UF_dim_vertical_subtype && lDiagCount < 10)
				{
					++lDiagCount;
					sprintf_s(fmt, sizeof(fmt),
						"[Step7 L组诊断] 垂直标注 tag=%llu viewTag=%llu geomTag=%llu viewState=%d origin=(%.2f, %.2f) -> 排除: %s",
						(unsigned long long)d->Tag(), (unsigned long long)viewTag,
						(unsigned long long)geomTag, viewState,
						originPt.X, originPt.Y, skipReason ? skipReason : "未知");
					CommonUtils::print_msg(fmt);
				}
				continue;
			}

			// 排序键（空间顺序）：
			//   D 组（测径向，标注沿垂直方向分布）→ 主键 origin.Y 降序（自上而下），
			//     次键 origin.X 升序
			//   L 组（测轴向，标注沿水平方向分布）→ 主键 origin.X 升序（自左而右），
			//     次键 origin.Y 降序
			TargetDim td;
			td.dim = d;
			td.subtype = s;
			td.tag = d->Tag();
			if (isD)
			{
				td.sortKey1 = -originPt.Y;
				td.sortKey2 = originPt.X;
				dGroup.push_back(td);
			}
			else
			{
				td.sortKey1 = originPt.X;
				td.sortKey2 = -originPt.Y;
				lGroup.push_back(td);
			}
		}

		sprintf_s(fmt, sizeof(fmt),
			"[Step7] 枚举结果: D 组 %d 条 / L 组 %d 条 | 排除: 其它图纸 %d 条、其它视图 %d 条、坐标标注 %d 条、其它类型 %d 条",
			(int)dGroup.size(), (int)lGroup.size(), otherSheetCount, otherViewCount, ordinateCount, otherTypeCount);
		CommonUtils::print_msg(fmt);
		sprintf_s(fmt, sizeof(fmt),
			"[Step7] 图纸归属判定: 明确当前图纸 %d 条 / 明确其它图纸 %d 条 / 未知 %d 条 | 通道命中: 关联原点 %d、关联数据 %d、GetViews %d（未知者走位置兜底）",
			sheetClsCur, sheetClsOther, sheetClsUnknown, ch1Hit, ch2Hit, ch3Hit);
		CommonUtils::print_msg(fmt);
		sprintf_s(fmt, sizeof(fmt),
			"[Step7] 视图归属诊断: 位置兜底尝试 %d 条（命中 %d 条）/ GetAssociativeOrigin 异常 %d 条 / 剖视图边界%s",
			noViewCount, posFallbackHit, gaExceptionCount, haveSectBorder ? "有效" : "获取失败");
		CommonUtils::print_msg(fmt);
		// View / ViewOfGeometry tag 分布（前 5 高频，排查标注实际挂在哪些视图上）
		{
			std::vector<std::pair<tag_t, int>> vd(viewTagDist.begin(), viewTagDist.end());
			std::sort(vd.begin(), vd.end(),
				[](const std::pair<tag_t, int>& a, const std::pair<tag_t, int>& b) { return a.second > b.second; });
			for (size_t i = 0; i < vd.size() && i < 5; ++i)
			{
				sprintf_s(fmt, sizeof(fmt), "  View tag=%llu: %d 条",
					(unsigned long long)vd[i].first, vd[i].second);
				CommonUtils::print_msg(fmt);
			}
		}
		{
			std::vector<std::pair<tag_t, int>> gd(geomTagDist.begin(), geomTagDist.end());
			std::sort(gd.begin(), gd.end(),
				[](const std::pair<tag_t, int>& a, const std::pair<tag_t, int>& b) { return a.second > b.second; });
			for (size_t i = 0; i < gd.size() && i < 5; ++i)
			{
				sprintf_s(fmt, sizeof(fmt), "  ViewOfGeometry tag=%llu: %d 条",
					(unsigned long long)gd[i].first, gd[i].second);
				CommonUtils::print_msg(fmt);
			}
		}
		if (dGroup.empty() && lGroup.empty())
		{
			CommonUtils::print_msg("[Step7] 剖视图上没有符合规则的尺寸标注（水平/垂直/平行/正交/直径/半径），无需处理");
			return;
		}

		// ===== 组内空间排序（确定性编号顺序） =====
		auto cmpTarget = [](const TargetDim& a, const TargetDim& b) -> bool
		{
			if (a.sortKey1 != b.sortKey1) return a.sortKey1 < b.sortKey1;
			if (a.sortKey2 != b.sortKey2) return a.sortKey2 < b.sortKey2;
			return a.tag < b.tag;
		};
		std::sort(dGroup.begin(), dGroup.end(), cmpTarget);
		std::sort(lGroup.begin(), lGroup.end(), cmpTarget);

		// ===== 编号并编辑后缀（(D1)...(Dn) / (L1)...(Ln)） =====
		// 编号空间按当前图纸独立：D/L 组各自从 1 起连续递增；
		// SetAfter 为全量替换，重跑自动覆盖本图纸旧后缀（幂等），
		// 不会沿用其它图纸的旧序号。
		int done = 0, failed = 0;
		char suffix[32];
		for (size_t i = 0; i < dGroup.size(); ++i)
		{
			sprintf_s(suffix, sizeof(suffix), "(D%d)", (int)i + 1);
			sprintf_s(fmt, sizeof(fmt),
				"[Step7] %s标注 tag=%llu origin=(%.2f, %.2f) -> 后缀 \"%s\"",
				dim_type_name(dGroup[i].subtype), (unsigned long long)dGroup[i].tag,
				-dGroup[i].sortKey1, dGroup[i].sortKey2, suffix);
			CommonUtils::print_msg(fmt);
			if (append_suffix_to_dim(part, dGroup[i].dim, dGroup[i].subtype, suffix)) ++done; else ++failed;
		}
		for (size_t i = 0; i < lGroup.size(); ++i)
		{
			sprintf_s(suffix, sizeof(suffix), "(L%d)", (int)i + 1);
			sprintf_s(fmt, sizeof(fmt),
				"[Step7] %s标注 tag=%llu origin=(%.2f, %.2f) -> 后缀 \"%s\"",
				dim_type_name(lGroup[i].subtype), (unsigned long long)lGroup[i].tag,
				lGroup[i].sortKey1, -lGroup[i].sortKey2, suffix);
			CommonUtils::print_msg(fmt);
			if (append_suffix_to_dim(part, lGroup[i].dim, lGroup[i].subtype, suffix)) ++done; else ++failed;
		}

		// ===== 静默更新（后缀改动进入显示） =====
		CommonUtils::silent_update(CommonUtils::get_session());

		sprintf_s(fmt, sizeof(fmt),
			"========== Step7 尺寸后缀标号 完成: 成功 %d / 失败 %d（D 组 %d 条、L 组 %d 条，编号按本图纸独立从 1 起） ==========",
			done, failed, (int)dGroup.size(), (int)lGroup.size());
		CommonUtils::print_msg(fmt);
	}
	catch (const NXOpen::NXException& e) { CommonUtils::print_msg(std::string("NXException: ") + e.Message()); }
	catch (const std::exception& e) { CommonUtils::print_msg(std::string("Exception: ") + e.what()); }
	catch (...) { CommonUtils::print_msg("Unknown Exception"); }
}

//==============================================================================
// do_it —— Step7 入口逻辑：只处理"当前工作/活动图纸"（NX 界面中当前
//          打开显示的那张），其余图纸一律不处理、不改动。
//==============================================================================
static void do_it()
{
	try
	{
		// ===== 进入制图环境 =====
		NXOpen::Part* part = dynamic_cast<NXOpen::Part*>(CommonUtils::get_session()->Parts()->BaseWork());
		if (!part) { CommonUtils::print_msg("错误：无工作部件"); return; }

		CommonUtils::get_session()->ApplicationSwitchImmediate("UG_APP_DRAFTING");
		part->Drafting()->EnterDraftingApplication();
		CommonUtils::print_msg("[Step7] 已进入制图模块");

		// ===== 获取当前工作/活动图纸（只处理该图纸） =====
		// 主接口：DraftingDrawingSheetCollection::CurrentDrawingSheet()
		//         （NX12.0.2 起可用，返回当前打开的图纸；无图纸打开时返回 NULL）
		// 备选：  UF_DRAW_ask_current_drawing 取当前图纸 tag（V16 起可用，
		//         兼容更早的 NX 小版本），经 NXObjectManager::Get 还原为
		//         DraftingDrawingSheet，避免旧版 NXOpen 库缺该导出。
		NXOpen::Drawings::DraftingDrawingSheet* sheet =
			part->DraftingDrawingSheets()->CurrentDrawingSheet();
		if (!sheet)
		{
			tag_t curTag = NULL_TAG;
			if (UF_DRAW_ask_current_drawing(&curTag) == 0 && curTag != NULL_TAG)
				sheet = dynamic_cast<NXOpen::Drawings::DraftingDrawingSheet*>(
					NXOpen::NXObjectManager::Get(curTag));
		}
		if (!sheet)
		{
			CommonUtils::print_msg("[Step7] 警告：当前未激活任何图纸，请先在制图环境中打开目标图纸再运行");
			return;
		}
		sheet->Open();          // 确保该图纸处于打开/显示状态（对已打开图纸幂等）
		process_sheet(part, sheet);
	}
	catch (const NXOpen::NXException& e) { CommonUtils::print_msg(std::string("NXException: ") + e.Message()); }
	catch (const std::exception& e) { CommonUtils::print_msg(std::string("Exception: ") + e.what()); }
	catch (...) { CommonUtils::print_msg("Unknown Exception"); }
}

//==============================================================================
// ufusr —— DLL 入口点（三层异常保护，防止 NX 崩溃）
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
