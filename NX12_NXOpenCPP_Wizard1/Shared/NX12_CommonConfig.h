#pragma once
//------------------------------------------------------------------------------
// NX12 壳体出图插件 —— 共享配置与类型定义
// 所有 DLL 模块共同 include 的头文件（纯头文件，无编译单元）
//------------------------------------------------------------------------------

//==============================================================================
// Std C++ Includes
//==============================================================================
#include <string>
#include <sstream>
#include <iostream>
#include <vector>
#include <iterator>   // std::distance（图纸集合迭代器距离，用于动态图号）
#include <math.h>

//==============================================================================
// UF Includes（被 2 个以上 DLL 使用的）
//==============================================================================
#include <uf.h>
#include <uf_ui.h>
#include <uf_draw.h>
#include <uf_view.h>            // UF_VIEW_cycle_objects（阶段6a 枚举视图内可见对象）
#include <uf_obj.h>             // UF_OBJ_ask_type_and_subtype（对象类型过滤）
#include <uf_modl_curves.h>     // UF_MODL_ask_curve_props（曲线端点求取）
#include <uf_curve.h>           // UF_CURVE_ask_arc_data（圆弧中心/半径求取）

//==============================================================================
// NXOpen Includes（被 2 个以上 DLL 使用的）
//==============================================================================
// Core
#include <NXOpen/Session.hxx>
#include <NXOpen/UI.hxx>
#include <NXOpen/Part.hxx>
#include <NXOpen/PartCollection.hxx>
#include <NXOpen/BasePart.hxx>
#include <NXOpen/NXObject.hxx>
#include <NXOpen/NXException.hxx>
#include <NXOpen/NXString.hxx>
#include <NXOpen/NXObjectManager.hxx>
#include <NXOpen/ugmath.hxx>    // NXOpen::Point3d

// Listing / Logging
#include <NXOpen/ListingWindow.hxx>
#include <NXOpen/LogFile.hxx>

// Views
#include <NXOpen/View.hxx>
#include <NXOpen/ViewCollection.hxx>
#include <NXOpen/ModelingViewCollection.hxx>
#include <NXOpen/ModelingView.hxx>

// Drawings
#include <NXOpen/Drawings_DraftingView.hxx>
#include <NXOpen/Drawings_SectionView.hxx>
#include <NXOpen/Drawings_BaseView.hxx>
#include <NXOpen/Drawings_DraftingDrawingSheet.hxx>
#include <NXOpen/Drawings_DraftingViewCollection.hxx>
#include <NXOpen/Drawings_DraftingDrawingSheetCollection.hxx>

// Annotations
#include <NXOpen/Annotations_Centerline2d.hxx>
#include <NXOpen/Annotations_DimensionCollection.hxx>

// Layer
#include <NXOpen/Layer.hxx>          // NXOpen::Layer::State 枚举与 StateInfo 结构
#include <NXOpen/Layer_LayerManager.hxx>

//==============================================================================
// using 声明（各 DLL include 后可直接使用 NXOpen 类型）
//==============================================================================
using namespace NXOpen;
using std::string;
using std::exception;
using std::stringstream;
using std::endl;
using std::cout;
using std::cerr;

//==============================================================================
// 配置区（通配修改点：换零件/换模板只改这里）
//==============================================================================
#define MAX_PROCESS_COUNT 8   // 最大工序数（图层对话框行数）

struct ShellDrawingConfig
{
	// 模板与零件路径（按实际环境修改）
	const char* templatePath;
	const char* partPath;

	// 图纸参数（A4 横向：297 x 210）
	double sheetHeight;      // A4 横向短边 210（图纸高度）
	double sheetLength;      // A4 横向长边 297（图纸长度）
	double sheetScaleNum;    // 图纸比例分子
	double sheetScaleDen;    // 图纸比例分母

	// 基础视图（兜底载体：NX12 剖视图必须挂在父成员视图下，
	// 故保留一个基础视图作为剖视父视图）
	// 载体视图=剖视父视图，按零件轴向选择：本零件回转轴沿模型 X 轴，
	// 录制脚本使用 Right（用 Top 会出斜视/内容错误）
	const char* baseModelViewName;   // 建模视图名（Right/Top/Front...），默认 "Right"
	Point3d baseViewPlace;           // 载体端面视图目标中心（A4 图纸坐标，左下角原点，
	                                 // 直接使用，不再走 A0×kSheetScale 派生）；
	                                 // 兼作铅垂剖切线的参考中心

	// A-A 全剖视图（主体视图）
	// 布局规则：视图位置对齐目标图——载体端面视图与 A-A 剖视图各自对齐
	// 独立目标点（均为 A4 图纸坐标）；剖切线仍相对载体实际位置计算。
	double aaSecHalfLen;             // 剖切线半长 d（录制 A0 坐标量级）：剖切线两点
	                                 // 运行时取载体放置点 ±(0,d)，X 严格相等（铅垂过轴），
	                                 // 初值待实机微调
	Point3d aaViewTarget;            // A-A 剖视图目标中心（A4 图纸坐标，独立目标点，
	                                 // 直接使用不再由"载体中心+偏移"派生；取代已移除的
	                                 // aaViewOffsetY）
	// 注：aaViewOffsetY（剖视图中心相对载体中心的 Y 偏移）已移除——由独立目标点
	// aaViewTarget 取代（视图位置对齐目标图，不再依赖载体位置派生）
	// 注：aaViewPlace（剖视图初始放置点）已移除——初始放置点不影响最终布局，
	// 剖视图位置完全由两步法对齐独立目标点决定，保留徒增混淆
	// 注：aaViewOrigin 已弃用——录制中 vieworigin1/ENTITY 2 1 对齐点属于
	// 投影视图 Builder 参数，误用于剖视图导致内容错误，已删除
	double aaViewScale;              // 剖视图比例
	const char* aaViewLabel;         // 视图标签（"A-A"；NX12 NXOpen 无视图标签
	                                 // 设置接口，图纸内首个剖视图自动编号 A-A）

	// 中心线（交互拾取视图内投影边生成，与投影边关联）
	bool createCenterline;

	// 轮廓端点自动坐标标注（成组水平/垂直坐标标注，以中心线为基准；
	// 中心线不可用时降级警告跳过，不中断主流程）
	bool autoContourDims;

	// 尺寸标注（交互拾取边生成线性/直径半径标注，运行时取消选择即跳过）
	bool createDimensions;

	// 工序图层映射（运行时对话框可修改，此处仅为默认值）
	int processCount;                    // 工序数量（1..MAX_PROCESS_COUNT）
	int defaultProcessLayers[MAX_PROCESS_COUNT]; // 工序 i 的默认图层
};

// 单张图纸描述（出图配置驱动：名称/图号/图幅/比例）
// 图纸级隐藏 = 全局状态为隐藏的图层；用户在 NX 图层设置(Ctrl+L)中
// 控制哪些图层出图，SheetDesc 不再维护可见图层列表。
struct SheetDesc
{
	const char* name;      // 图纸名（SetName，部件内唯一，幂等匹配依据），
	                       // 中文图纸名称由此承载
	// 标题栏图号（SetNumber）不在此配置：运行时动态分配 = 现有图纸数量 + 1。
	// 依据与局限：NX12 要求图号为从 1 开始的连续整数序列，跳号 Commit 会报
	// "The drawing sheet number is not in sequence."；且 NX12 NXOpen 未提供
	// 已有图纸图号的只读接口（Number() 仅存在于 DrawingSheetBuilder），
	// 故用"现有图纸张数"近似最大图号。若用户手动把图号改乱（缺号/重号），
	// 该近似可能失配，届时 Commit 仍会抛序列错误，按日志提示人工整理图号即可。
	double height, length; // 图幅（A4 横向: 210 x 297）
	double scaleNum, scaleDen;
};
