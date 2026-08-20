//------------------------------------------------------------------------------
// NX12 NXOpen C++ 壳体类零件通配出图插件
//
// 依据 02_study 录制日志（000R_VB.vb）去噪整理后的关键流程：
//   阶段1 进入制图模块
//   阶段2 创建车床图（单张 A4 图纸、第三角投影、1:1、A4-noviews-asm-template）
//   阶段3 创建载体基础视图（载体视图=剖视父视图，按零件轴向选择，默认 Right；
//         按录制放置于图幅内——NX12 SectionViewBuilder
//         无 SelectModelView 类接口，剖视图必须挂在成员视图下）
//   阶段4 创建 A-A 全剖视图（主体视图，两点直线剖切，图幅居中）
//   阶段6 创建中心线（交互拾取视图内投影边，与投影边保持关联）
//   阶段6a 轮廓端点自动坐标标注（成组水平/垂直坐标标注，可开关）
//   阶段7 工序图层切换（运行时对话框指定 工序->图层 映射）
//   阶段8 后处理（剖视图比例、视图位置、最终更新）
//
// 说明：插件只新建图纸/视图/注释，不再删除任何旧对象。
// 通配要点：所有路径/点/开关集中在"配置区"，换零件只改配置区。
//------------------------------------------------------------------------------

// Mandatory UF Includes
#include <uf.h>
#include <uf_ui.h>
#include <uf_object_types.h>
#include <uf_draw.h>
#include <uf_view.h>            // UF_VIEW_cycle_objects（阶段6a 枚举视图内可见对象）
#include <uf_obj.h>             // UF_OBJ_ask_type_and_subtype（对象类型过滤）
#include <uf_modl_curves.h>     // UF_MODL_ask_curve_props（曲线端点求取）
#include <vector>

// Win32（工序图层映射对话框，不依赖 Block Styler .dlx 模板）
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <stdlib.h>
#include <math.h>
// windows.h 的 CreateDialog 宏会破坏 NXOpen/UI.hxx 中的同名方法，
// NX 官方头文件注释明确要求：C++ 程序中必须 #undef CreateDialog
#ifdef CreateDialog
#undef CreateDialog
#endif

// Internal Includes
#include <NXOpen/ListingWindow.hxx>
#include <NXOpen/NXMessageBox.hxx>
#include <NXOpen/UI.hxx>
#include <NXOpen/LogFile.hxx>
#include <NXOpen/Update.hxx>
#include <NXOpen/Drawings_DraftingDrawingSheetBuilder.hxx>
#include <NXOpen/Drawings_BaseViewBuilder.hxx>
#include <NXOpen/Drawings_SectionViewBuilder.hxx>
#include <NXOpen/Drawings_SectionLineSegmentsBuilder.hxx>
#include <NXOpen/Drawings_SectionLineSegmentPointListBuilder.hxx>
#include <NXOpen/Drawings_SectionLineSegmentPointBuilder.hxx>
#include <NXOpen/Drawings_EditViewSettingsBuilder.hxx>
#include <NXOpen/Drafting_SettingsManager.hxx>
#include <NXOpen/Drawings_DraftingDrawingSheet.hxx>
#include <NXOpen/Drawings_BaseView.hxx>
#include <NXOpen/Drawings_DraftingView.hxx>
#include <NXOpen/Drawings_SectionView.hxx>
#include <NXOpen/Drawings_ViewPlacementBuilder.hxx>
#include <NXOpen/Drawings_SelectModelViewBuilder.hxx>
#include <NXOpen/Drawings_ParentViewBuilder.hxx>
#include <NXOpen/Drawings_ViewStyleBuilder.hxx>
#include <NXOpen/Drawings_DraftingComponentSelectionBuilder.hxx>
#include <NXOpen/Drawings_DraftingViewCollection.hxx>
#include <NXOpen/Drawings_DraftingDrawingSheetCollection.hxx>
#include <NXOpen/Drawings_SelectDraftingView.hxx>
#include <NXOpen/SelectNXObject.hxx>
#include <NXOpen/SelectTaggedObject.hxx>
#include <NXOpen/SelectDisplayableObject.hxx>      // OrdinateOrigin（坐标标注基准）
#include <NXOpen/SelectDisplayableObjectList.hxx>  // AutoAssociativities（坐标标注端点集）
#include <NXOpen/Selection.hxx>
#include <NXOpen/ViewCollection.hxx>
#include <NXOpen/ModelingViewCollection.hxx>
#include <NXOpen/DraftingManager.hxx>

// Modeling / Geometry
#include <NXOpen/ModelingView.hxx>
#include <NXOpen/View.hxx>
#include <NXOpen/Point.hxx>
#include <NXOpen/PointCollection.hxx>
#include <NXOpen/Unit.hxx>
#include <NXOpen/UnitCollection.hxx>
#include <NXOpen/Expression.hxx>
#include <NXOpen/ExpressionCollection.hxx>

// Annotations
#include <NXOpen/Annotations_Note.hxx>
#include <NXOpen/Annotations_Centerline3d.hxx>
#include <NXOpen/Annotations_CenterlineCollection.hxx>
#include <NXOpen/Annotations_Centerline2d.hxx>
#include <NXOpen/Annotations_Centerline2dBuilder.hxx>
#include <NXOpen/Annotations_AnnotationManager.hxx>
#include <NXOpen/Annotations_DimensionCollection.hxx>
#include <NXOpen/Annotations_LinearDimensionBuilder.hxx>
#include <NXOpen/Annotations_RadialDimensionBuilder.hxx>
#include <NXOpen/Annotations_OrdinateDimensionBuilder.hxx>
#include <NXOpen/Annotations_OriginBuilder.hxx>

// Layer（工序换图层 / 图层显隐状态切换）
#include <NXOpen/Layer.hxx>   // NXOpen::Layer::State 枚举与 StateInfo 结构
#include <NXOpen/Layer_LayerManager.hxx>
#include <NXOpen/DisplayableObject.hxx>

// Internal+External Includes
#include <NXOpen/Annotations.hxx>
#include <NXOpen/Assemblies_Component.hxx>
#include <NXOpen/Assemblies_ComponentAssembly.hxx>
#include <NXOpen/Body.hxx>
#include <NXOpen/BodyCollection.hxx>
#include <NXOpen/Face.hxx>
#include <NXOpen/Line.hxx>
#include <NXOpen/NXException.hxx>
#include <NXOpen/NXObject.hxx>
#include <NXOpen/Part.hxx>
#include <NXOpen/PartCollection.hxx>
#include <NXOpen/Session.hxx>
#include <NXOpen/NXObjectManager.hxx>

// Std C++ Includes
#include <iostream>
#include <sstream>
#include <string>
#include <iterator>   // std::distance（图纸集合迭代器距离，用于动态图号）

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

//------------------------------------------------------------------------------
// NXOpen c++ 壳体出图类
//------------------------------------------------------------------------------
class MyClass
{
public:
	static Session *theSession;
	static UI *theUI;

	MyClass();
	~MyClass();

	void do_it();
	void print(const NXString &);
	void print(const string &);
	void print(const char*);

private:
	// 各阶段模块
	NXOpen::Drawings::DraftingDrawingSheet* phase_create_sheet(NXOpen::Part* part, const ShellDrawingConfig& cfg, const SheetDesc& sheetDesc);
	void phase_apply_sheet_layer_visibility(NXOpen::Part* part, NXOpen::Drawings::DraftingDrawingSheet* sheet);
	NXOpen::Drawings::BaseView* phase_create_base_view(NXOpen::Part* part, const ShellDrawingConfig& cfg);
	void phase_center_view(NXOpen::Part* part, NXOpen::Drawings::DraftingView* view, const SheetDesc& sd,
		const NXOpen::Point3d* cTargetOverride = NULL);
	NXOpen::Drawings::SectionView* phase_create_aa_section(NXOpen::Part* part, const ShellDrawingConfig& cfg, NXOpen::Drawings::BaseView* baseView);
	NXOpen::Annotations::Centerline2d* phase_create_centerline(NXOpen::Part* part, NXOpen::Drawings::DraftingView* sectionView);
	void phase_auto_contour_dims(NXOpen::Part* part, NXOpen::Drawings::DraftingView* sectionView,
		NXOpen::Annotations::Centerline2d* centerlineObj);
	void phase_create_dimensions(NXOpen::Part* part, NXOpen::Drawings::DraftingView* sectionView);
	void phase_apply_layers(NXOpen::Part* part, const ShellDrawingConfig& cfg);
	void phase_post_process_view_scale(NXOpen::Part* part, const ShellDrawingConfig& cfg,
		NXOpen::Drawings::SectionView* sectionView);

	BasePart *workPart, *displayPart;
	NXMessageBox *mb;
	ListingWindow *lw;
	LogFile *lf;
};

//------------------------------------------------------------------------------
// Initialize static variables
//------------------------------------------------------------------------------
Session *(MyClass::theSession) = NULL;
UI *(MyClass::theUI) = NULL;

//------------------------------------------------------------------------------
// Constructor
//------------------------------------------------------------------------------
MyClass::MyClass()
{
	MyClass::theSession = NXOpen::Session::GetSession();
	MyClass::theUI = UI::GetUI();
	mb = theUI->NXMessageBox();
	lw = theSession->ListingWindow();
	lf = theSession->LogFile();

	workPart = theSession->Parts()->BaseWork();
	displayPart = theSession->Parts()->BaseDisplay();
}

//------------------------------------------------------------------------------
// Destructor
//------------------------------------------------------------------------------
MyClass::~MyClass()
{
}

//------------------------------------------------------------------------------
// Print string to listing window
// 同时写入 NX 系统日志（LogFile::WriteLine，即 syslog，路径可用
// lf->FileName() 查询）；日志写失败静默降级，不影响主流程。
//------------------------------------------------------------------------------
void MyClass::print(const NXString &msg)
{
	if (!lw->IsOpen()) lw->Open();
	lw->WriteLine(msg);
	try { if (lf) lf->WriteLine(msg); } catch (...) {}
}
void MyClass::print(const string &msg)
{
	if (!lw->IsOpen()) lw->Open();
	lw->WriteLine(msg);
	try { if (lf) lf->WriteLine(msg.c_str()); } catch (...) {}
}
void MyClass::print(const char * msg)
{
	if (!lw->IsOpen()) lw->Open();
	lw->WriteLine(msg);
	try { if (lf) lf->WriteLine(msg); } catch (...) {}
}

//------------------------------------------------------------------------------
// 阶段2：创建图纸（按 SheetDesc 配置，幂等匹配图纸名 Name）
// 图号规则：NX12 要求图号为从 1 开始的连续整数序列，故新建图纸时
// 动态取"现有图纸数量 + 1"作为新图号（详见 SheetDesc 注释）；
// 复用分支（按 Name 匹配到已有图纸）不触碰其原图号。
//------------------------------------------------------------------------------
NXOpen::Drawings::DraftingDrawingSheet* MyClass::phase_create_sheet(NXOpen::Part* part, const ShellDrawingConfig& cfg, const SheetDesc& sheetDesc)
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
			print(string("[2/8] 已存在图纸 \"") + sheetDesc.name + "\"，直接打开复用，跳过创建");
			return s;
		}
	}

	NXOpen::Drawings::DraftingDrawingSheetBuilder* dsBuilder =
		part->DraftingDrawingSheets()->CreateDraftingDrawingSheetBuilder(NULL);

	// 注意：C++ 属性赋值使用方法传参（或 Set 前缀方法），不能像 VB 用 "=" 赋值
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
	// NX12 要求图号为从 1 起的连续整数序列，跳号（如部件内无 1~6 号图却
	// 直接建 7 号）Commit 会报 "The drawing sheet number is not in sequence."。
	// NX12 NXOpen 未提供已有图纸图号的只读接口（Number() 仅存在于
	// DrawingSheetBuilder），而 NX 强制图号从 1 起连续，故以"现有图纸张数"
	// 作为最大图号。遍历发生在 Commit 之前，新建图纸尚未入集合，天然排除了
	// 自身。局限：若用户手动把图号改乱（缺号/重号），该近似可能失配。
	const int existingSheetCount = (int)std::distance(
		part->DraftingDrawingSheets()->begin(),
		part->DraftingDrawingSheets()->end());
	const int assignedNumber = existingSheetCount + 1;   // 无图纸时即 1
	const std::string assignedNumberStr = std::to_string(assignedNumber);
	// number 必须是合法图号格式（数字），中文名称只通过上方 SetName 设置
	dsBuilder->SetNumber(assignedNumberStr.c_str());
	dsBuilder->SetSecondaryNumber("");   // 空串表示不填副图号，合法（非数字字符才会触发格式校验）
	dsBuilder->SetRevision("A");
	print(string("[") + sheetDesc.name + "] 图号自动分配: " + assignedNumberStr);

	NXOpen::NXObject* sheetObj = NULL;
	try
	{
		sheetObj = dsBuilder->Commit();
	}
	catch (const NXOpen::NXException& e)
	{
		// 保持异常向上抛出的原有降级路径（do_it 统一弹框/打印），
		// 此处补充诊断：回显本次分配的图号与当前图纸总数，便于实机定位
		// 图号序列类错误（如 "The drawing sheet number is not in sequence."）
		print(string("  警告: 图纸 Commit 失败（本次分配图号: ") + assignedNumberStr +
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

	print(string("[2/8] 已创建图纸 \"") + sheetDesc.name +
		"\"（图号 " + assignedNumberStr + "）");
	return sheet;
}

//------------------------------------------------------------------------------
// 阶段2b：设置本图纸（成员视图）的图层可见性——按图层显隐状态过滤：
// 图纸上只出"显示的图层"的实体。遍历图层 1..256，用 GetState 查询全局
// 状态，仅把状态为 StateHidden 的图层收集为图纸级隐藏条目；
// 工作/可见/可选状态的图层一律不干预（图纸按全局显示）。
// 即：图纸级隐藏 = 全局状态为隐藏的图层；用户在 NX 图层设置(Ctrl+L)
// 中控制哪些图层出图。
//------------------------------------------------------------------------------
void MyClass::phase_apply_sheet_layer_visibility(NXOpen::Part* part,
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
			print("  - 所有图层均为显示状态，无需图纸级隐藏");
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
		print(string("  - 已按图层显隐状态隐藏 ") + std::to_string(states.size()) +
			" 个图层: " + layers);
	}
	catch (const NXOpen::NXException& e)
	{
		print(string("  警告: 设置图纸图层可见性失败: ") + e.Message());
	}
}

//------------------------------------------------------------------------------
// 阶段3：创建基础视图（载体视图=剖视父视图，按零件轴向选择，默认 Right）
// 定位：A-A 剖视图必需的父视图载体（NX12 SectionViewBuilder
// 无 SelectModelView 类接口，剖视图不能独立创建）。
// 放置点 = cfg.baseViewPlace（A4 图纸坐标，直接使用，不再乘 kSheetScale），
// 创建后由主流程再走一次两步法定位收敛到该目标；同时作为铅垂剖切线的参考中心。
//------------------------------------------------------------------------------
NXOpen::Drawings::BaseView* MyClass::phase_create_base_view(NXOpen::Part* part, const ShellDrawingConfig& cfg)
{
	NXOpen::ModelingView* modelView = dynamic_cast<NXOpen::ModelingView*>(
		part->ModelingViews()->FindObject(cfg.baseModelViewName));
	if (!modelView)
	{
		print(string("错误：找不到建模视图 ") + cfg.baseModelViewName);
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
		print("错误：基础视图创建失败");
		return NULL;
	}
	print(string("[3/8] 已创建基础视图 (载体视图 ") + cfg.baseModelViewName + "，放置于目标位置)");
	return baseView;
}

//------------------------------------------------------------------------------
// 阶段3b：视图两步法精确定位（目标中心可配置，默认图幅中心）
// 泛化实现：接受任意成员视图（BaseView/SectionView 均继承 DraftingView）。
// 本流程分别作用于：载体视图（目标 = cfg.baseViewPlace）与
// A-A 剖视图（目标 = 独立目标点 cfg.aaViewTarget，视图位置对齐目标图）。
// 第一步：静默更新 + 视图显式更新，确保视图边界已生成；
//        UF_DRAW_ask_view_borders 读取当前视图边界（图纸坐标
//        [Xmin, Ymin, Xmax, Ymax]），算出当前中心 C0；
// 第二步：新锚点 = 当前锚点 + (目标中心 - C0)，MoveView（参数为
//        "新视图原点"而非位移量）后再静默更新 + 复核边界；
//        偏差超过 0.01mm 则再补偿一轮（最多两轮），仍不收敛则
//        print 诊断信息。任何异常均降级为警告，不中断主流程。
//------------------------------------------------------------------------------
void MyClass::phase_center_view(NXOpen::Part* part, NXOpen::Drawings::DraftingView* view, const SheetDesc& sd,
	const NXOpen::Point3d* cTargetOverride)
{
	if (!view) return;

	// 静默更新（最小代价）：让刚 Commit 的视图生成/刷新边界。
	// 注：DoUpdate(mark) 的 mark 仅作更新失败时的回滚点（见 Update.hxx
	// 注释），DoUpdate 本身会更新会话内所有过期对象，mark 之后
	// 新建的视图同样会被刷新。
	auto silent_update = [this]()
	{
		NXOpen::Session::UndoMarkId mark = theSession->SetUndoMark(
			NXOpen::Session::MarkVisibilityInvisible, "Center View");
		theSession->UpdateManager()->DoUpdate(mark);
	};

	char fmt[512];
	try
	{
		// 第一步：更新后读取视图边界（图纸坐标 [Xmin, Ymin, Xmax, Ymax]）
		silent_update();
		// 双保险：显式更新该视图（DraftingView::Update 文档明确包含
		// view bounds 更新），确保 ask_view_borders 读到的是最新边界
		view->Update();

		double b[4] = { 0.0, 0.0, 0.0, 0.0 };
		int rc = UF_DRAW_ask_view_borders(view->Tag(), b);
		if (rc != 0)
		{
			sprintf_s(fmt, sizeof(fmt),
				"  警告: UF_DRAW_ask_view_borders 返回 %d，跳过视图居中", rc);
			print(fmt);
			return;
		}

		// 当前视图中心 C0 与目标中心 C。
		// 目标中心可配置：cTargetOverride 非空时以其为准（载体 -> baseViewPlace；
		// 剖视图 -> 独立目标点 aaViewTarget），否则取图幅中心。
		// 边界数组下标对应：[0]=Xmin [1]=Ymin [2]=Xmax [3]=Ymax（图纸坐标），
		// X 轴沿图纸 length 方向、Y 轴沿 height 方向；
		// uf_draw_types.h 中 size[0]=height/size[1]=length 仅是
		// UF_DRAW_info_t 尺寸数组约定，与边界框 X/Y 无关。
		NXOpen::Point3d c0((b[0] + b[2]) / 2.0, (b[1] + b[3]) / 2.0, 0.0);
		NXOpen::Point3d cTarget = cTargetOverride
			? *cTargetOverride
			: NXOpen::Point3d(sd.length / 2.0, sd.height / 2.0, 0.0);

		// 诊断日志：返回码、边界四值、中心、图幅
		sprintf_s(fmt, sizeof(fmt),
			"  居中诊断: UF_DRAW rc=%d, 边界(%.3f, %.3f, %.3f, %.3f), "
			"当前中心(%.3f, %.3f), 目标中心(%.3f, %.3f), 图幅 %.0f x %.0f",
			rc, b[0], b[1], b[2], b[3], c0.X, c0.Y, cTarget.X, cTarget.Y,
			sd.length, sd.height);
		print(fmt);

		// 第二步：新锚点 = 当前锚点 + (C - C0)，z 分量保持；
		// MoveView 参数即"新视图原点"（见 Drawings_DraftingView.hxx 注释）
		NXOpen::Point3d refPt = view->GetDrawingReferencePoint();
		NXOpen::Point3d newRefPt(
			refPt.X + (cTarget.X - c0.X),
			refPt.Y + (cTarget.Y - c0.Y),
			refPt.Z);
		sprintf_s(fmt, sizeof(fmt),
			"  居中诊断: 旧锚点(%.3f, %.3f, %.3f) -> 新锚点(%.3f, %.3f, %.3f)",
			refPt.X, refPt.Y, refPt.Z, newRefPt.X, newRefPt.Y, newRefPt.Z);
		print(fmt);
		view->MoveView(newRefPt);

		// 再静默更新一次，让移动后的视图边界生效
		silent_update();
		view->Update();

		// 收敛验证：复核移动后的边界，偏差 > 0.01mm 则再补偿一轮（最多两轮）
		const double tol = 0.01;
		for (int round = 0; round < 2; ++round)
		{
			double b2[4] = { 0.0, 0.0, 0.0, 0.0 };
			int rc2 = UF_DRAW_ask_view_borders(view->Tag(), b2);
			if (rc2 != 0)
			{
				sprintf_s(fmt, sizeof(fmt),
					"  警告: 复核 UF_DRAW_ask_view_borders 返回 %d，无法验证居中结果", rc2);
				print(fmt);
				return;
			}
			NXOpen::Point3d c1((b2[0] + b2[2]) / 2.0, (b2[1] + b2[3]) / 2.0, 0.0);
			double errX = cTarget.X - c1.X;
			double errY = cTarget.Y - c1.Y;
			sprintf_s(fmt, sizeof(fmt),
				"  居中诊断(第%d轮复核): 边界(%.3f, %.3f, %.3f, %.3f), "
				"实际中心(%.3f, %.3f), 偏差(%.4f, %.4f)",
				round + 1, b2[0], b2[1], b2[2], b2[3], c1.X, c1.Y, errX, errY);
			print(fmt);

			if (fabs(errX) <= tol && fabs(errY) <= tol)
			{
				sprintf_s(fmt, sizeof(fmt),
					"[图纸 %s] 视图已定位 (中心 -> (%.2f, %.2f))",
					sd.name, cTarget.X, cTarget.Y);
				print(fmt);
				return;
			}

			if (round == 1)
			{
				print("  警告: 两轮补偿后仍未收敛（偏差 > 0.01mm），请回传以上诊断信息");
				return;
			}

			// 补偿：按残余偏差再移动一次
			NXOpen::Point3d rp = view->GetDrawingReferencePoint();
			NXOpen::Point3d nrp(rp.X + errX, rp.Y + errY, rp.Z);
			sprintf_s(fmt, sizeof(fmt),
				"  居中诊断: 补偿移动 旧锚点(%.3f, %.3f, %.3f) -> 新锚点(%.3f, %.3f, %.3f)",
				rp.X, rp.Y, rp.Z, nrp.X, nrp.Y, nrp.Z);
			print(fmt);
			view->MoveView(nrp);
			silent_update();
			view->Update();
		}
	}
	catch (const NXOpen::NXException& e)
	{
		print(string("  警告: 视图居中失败: ") + e.Message());
	}
	catch (...)
	{
		print("  警告: 视图居中失败（未知异常）");
	}
}

//------------------------------------------------------------------------------
// 阶段4：创建 A-A 全剖视图（主体视图，两点直线剖切）
// API 核验结论（NX12 Drawings_SectionViewBuilder.hxx）：
//   1. 无 SelectModelView 类接口，剖视图不能独立创建，必须通过
//      ParentView() 挂在成员视图（基础视图）下；
//      SectionViewModeTypeStandAlone 仅是剖切线模式，不免除父视图。
//   2. 两点直线剖切写法（沿用现有实机验证过的阶梯剖经验）：
//      只对起点调用一次 SegmentLocation()->AddCutSegment(pt)，
//      终点用 SetSectionLineOnlyPlacementOrigin 拖动生成；
//      若两点都 AddCutSegment 会报"截面线中添加箭头段错误"。
//   3. 剖切线两点运行时相对载体（基础视图）放置点计算：
//      pt1 = 载体中心 + (0,+d)、pt2 = 载体中心 + (0,-d)，两点 X
//      严格相等（铅垂过轴），避免硬编码坐标连线歪斜；
//      d（aaSecHalfLen）为可配置项，待实机微调。
//   4. 视图标签：DraftingView/SectionViewBuilder 均无 SetName/SetViewLabel
//      接口，图纸内首个剖视图由 NX 自动编号为 A-A，故此处不显式设置。
//   5. 不设置 ViewOrigin/对齐点：录制中 vieworigin1/ENTITY 2 1 属于
//      投影视图 Builder 参数，不可用于剖视图；对齐用 Builder 默认。
//------------------------------------------------------------------------------
NXOpen::Drawings::SectionView* MyClass::phase_create_aa_section(NXOpen::Part* part, const ShellDrawingConfig& cfg, NXOpen::Drawings::BaseView* baseView)
{
	if (!baseView) return NULL;

	NXOpen::Drawings::SectionViewBuilder* svBuilder =
		part->DraftingViews()->CreateSectionViewBuilder(NULL);

	// 直线（简单阶梯）剖切线类型：两点即一条直剖切线
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
	// 注：不设置 ViewOrigin，也不设置 ENTITY 对齐点——录制中这些参数
	// 属于投影视图 Builder，误用于剖视图会导致出斜视/内容错误；
	// 对齐采用 Builder 默认（上方 MethodPerpendicularToHingeLine + OptionModelPoint）
	svBuilder->ViewPlacement()->AlignmentView()->SetValue(baseView);

	// 两点直线剖切（沿用实机验证过的写法）：
	// 起点点对象只 AddCutSegment 一次，终点用
	// SetSectionLineOnlyPlacementOrigin 拖动生成；
	// 若两点都调用 AddCutSegment 会与箭头段结构冲突，
	// 报“截面线中添加箭头段错误”。
	// 两点运行时相对载体放置点计算：X 严格相等（铅垂过零件回转轴），
	// 避免旧硬编码两点连线歪斜（约 2.83°）
	NXOpen::Point3d secPt1(cfg.baseViewPlace.X, cfg.baseViewPlace.Y + cfg.aaSecHalfLen, 0.0);
	NXOpen::Point3d secPt2(cfg.baseViewPlace.X, cfg.baseViewPlace.Y - cfg.aaSecHalfLen, 0.0);
	char secFmt[192];
	sprintf_s(secFmt, sizeof(secFmt),
		"  剖切线(铅垂): 起点(%.3f, %.3f) -> 终点(%.3f, %.3f), 半长d=%.3f（待实机微调）",
		secPt1.X, secPt1.Y, secPt2.X, secPt2.Y, cfg.aaSecHalfLen);
	print(secFmt);
	NXOpen::Point* pt1 = part->Points()->CreatePoint(secPt1);
	svBuilder->SectionLineSegments()->SegmentLocation()->AddCutSegment(pt1);
	svBuilder->SectionLineSegments()->SetSectionLineOnlyPlacementOrigin(secPt2);

	// 初始放置点：图幅中心附近（仅为 Commit 提供落点，最终位置由
	// phase_center_view 两步法精确对齐到载体正上方）
	NXOpen::Point3d aaViewPlaceInit(cfg.sheetLength / 2.0, cfg.sheetHeight / 2.0, 0.0);
	svBuilder->ViewPlacement()->Placement()->SetValue(NULL, part->Views()->WorkView(), aaViewPlaceInit);

	NXOpen::NXObject* svObj = svBuilder->Commit();
	svBuilder->Destroy();

	NXOpen::Drawings::SectionView* sectionView = dynamic_cast<NXOpen::Drawings::SectionView*>(svObj);
	if (!sectionView)
	{
		print("错误：A-A 剖视图创建失败");
		return NULL;
	}
	print(string("[4/8] 已创建 ") + cfg.aaViewLabel + " 全剖视图（主体视图）");
	return sectionView;
}

//------------------------------------------------------------------------------
// 阶段6：创建中心线（交互拾取视图内投影边）
//
// 关联点修正要点：不再在模型空间/绝对坐标直接创建点对象，
// 而是把光标视图切到"任意视图"（UF_UI_set_cursor_view(0)），
// 在制图成员视图内拾取投影边（视图相关几何），
// 再用 Centerline2dBuilder 的 Side1/Side2 把拾取到的投影边
// （连同所在视图与拾取坐标）写入，生成的中心线与视图投影边保持关联。
// 参数为 A-A 剖视图（DraftingView 基类，SetValue 接受任意成员视图）。
// 返回值：创建成功返回中心线对象（供阶段6a 坐标标注作基准），
// 跳过/失败返回 NULL（阶段6a 降级警告跳过）。
//------------------------------------------------------------------------------
NXOpen::Annotations::Centerline2d* MyClass::phase_create_centerline(NXOpen::Part* part, NXOpen::Drawings::DraftingView* sectionView)
{
	if (!sectionView)
	{
		print("[6/8] 中心线已跳过（无剖视图）");
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

		NXOpen::Selection::Response r1 = theUI->SelectionManager()->SelectObject(
			"拾取视图内第一条投影边", "中心线-边1",
			NXOpen::Selection::SelectionScopeWorkPart, false, true, &edge1, &cur1);
		if (r1 != NXOpen::Selection::ResponseObjectSelected || !edge1)
		{
			UF_UI_set_cursor_view(oldCursorView);
			print("[6/8] 中心线已跳过（未选择投影边）");
			return NULL;
		}

		NXOpen::Selection::Response r2 = theUI->SelectionManager()->SelectObject(
			"拾取视图内第二条投影边", "中心线-边2",
			NXOpen::Selection::SelectionScopeWorkPart, false, true, &edge2, &cur2);

		// 恢复光标视图
		UF_UI_set_cursor_view(oldCursorView);

		if (r2 != NXOpen::Selection::ResponseObjectSelected || !edge2)
		{
			print("[6/8] 中心线已跳过（第二条边未选择）");
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
		print("[6/8] 已创建中心线（关联于视图投影边）");
		return centerline;
	}
	catch (const NXOpen::NXException& e)
	{
		print(string("  警告: 中心线创建失败: ") + e.Message());
	}
	return NULL;
}

//------------------------------------------------------------------------------
// 阶段6a：轮廓端点自动坐标标注（成组水平/垂直坐标标注，在中心线创建之后、
// 交互标注之前执行）
//   1. UF_VIEW_cycle_objects(UF_VIEW_VISIBLE_OBJECTS) 枚举剖视图内可见对象，
//      UF_OBJ_ask_type_and_subtype 只留曲线/边类（直线/圆弧/圆锥/样条/实体边），
//      丢弃 face/body/注释类对象；
//   2. UF_MODL_ask_curve_props(parm=0.0/1.0) 取端点（视图内制图曲线返回
//      即图纸坐标），端点按 0.01 容差去重；
//   3. 成组坐标标注：OrdinateDimensionBuilder SetType(TypesMultipleDimension)，
//      OrdinateOrigin 基准 = 阶段6 创建的中心线，AutoAssociativities 逐端点
//      Add(曲线/边, 视图, 端点坐标)；头文件语义为"放置方位（margin）决定方向"，
//      故水平组与垂直组各建一个 Builder：
//        水平组 SetHorizontalInferredMarginLocation（Y = 视图边界下缘-12）
//        垂直组 SetVerticalInferredMarginLocation（X = 视图边界左缘-12）
//      边界用 UF_DRAW_ask_view_borders 实算；任一 Builder 失败（如成组模式
//      实机不可行）自动降级单条模式（TypesSingleDimension）重试；
//      无论成败 Builder 均 Destroy。
//   中心线不可用（NULL）时降级警告跳过本阶段，不中断主流程。
//------------------------------------------------------------------------------
void MyClass::phase_auto_contour_dims(NXOpen::Part* part, NXOpen::Drawings::DraftingView* sectionView,
	NXOpen::Annotations::Centerline2d* centerlineObj)
{
	if (!sectionView)
	{
		print("[6a/8] 轮廓坐标标注已跳过（无剖视图）");
		return;
	}
	if (!centerlineObj)
	{
		print("[6a/8] 轮廓坐标标注已跳过（中心线不可用，无法作为坐标基准）");
		return;
	}

	char fmt[512];

	// ---- 视图边界（图纸坐标 [Xmin,Ymin,Xmax,Ymax]）与 margin 位置 ----
	double b[4] = { 0.0, 0.0, 0.0, 0.0 };
	int rcB = UF_DRAW_ask_view_borders(sectionView->Tag(), b);
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
		print(fmt);
	}

	// ---- 枚举剖视图内可见对象，过滤只留曲线/边类 ----
	std::vector<tag_t> curves;
	tag_t obj = NULL_TAG;
	for (;;)
	{
		int rc = UF_VIEW_cycle_objects(sectionView->Tag(), UF_VIEW_VISIBLE_OBJECTS, &obj);
		if (rc != 0 || obj == NULL_TAG) break;
		int type = 0, subtype = 0;
		if (UF_OBJ_ask_type_and_subtype(obj, &type, &subtype) != 0) continue;
		bool keep = false;
		if (type == UF_line_type || type == UF_circle_type ||
			type == UF_conic_type || type == UF_spline_type)
		{
			keep = true;   // 制图曲线类
		}
		else if (type == UF_solid_type && subtype == UF_solid_edge_subtype)
		{
			keep = true;   // 实体边（剖视图投影边的常见形态）；face/body 一律丢弃
		}
		if (keep) curves.push_back(obj);
	}

	// ---- 取端点（UF_MODL_ask_curve_props parm=0.0/1.0），0.01 容差去重 ----
	std::vector<NXOpen::Point3d> ends;
	{
		const double kDupTol = 0.01;
		auto addEnd = [&ends, kDupTol](const double p[3])
		{
			NXOpen::Point3d q(p[0], p[1], p[2]);
			for (size_t k = 0; k < ends.size(); ++k)
			{
				const double dx = ends[k].X - q.X, dy = ends[k].Y - q.Y;
				if (dx * dx + dy * dy < kDupTol * kDupTol) return;
			}
			ends.push_back(q);
		};
		for (size_t i = 0; i < curves.size(); ++i)
		{
			double pt[3], tg[3], pn[3], bn[3], torsion = 0.0, roc = 0.0;
			if (UF_MODL_ask_curve_props(curves[i], 0.0, pt, tg, pn, bn, &torsion, &roc) == 0)
				addEnd(pt);
			if (UF_MODL_ask_curve_props(curves[i], 1.0, pt, tg, pn, bn, &torsion, &roc) == 0)
				addEnd(pt);
		}
	}
	sprintf_s(fmt, sizeof(fmt),
		"  视图内可见曲线/边 %d 条，轮廓端点（去重后）%d 个",
		(int)curves.size(), (int)ends.size());
	print(fmt);
	if (ends.empty())
	{
		print("[6a/8] 轮廓坐标标注已跳过（未取到轮廓端点）");
		return;
	}

	// ---- 单个 Builder（水平组或垂直组）：基准=中心线，端点集=AutoAssociativities ----
	// 每个 Builder 独立 try/catch：成组模式（TypesMultipleDimension）Commit 失败
	// 时降级单条模式（TypesSingleDimension）重试；无论成败最终 Destroy。
	// 注：头文件实测 BaseOrdinateDimensionBuilder 的 OrdinateOrigin 为
	// SelectDisplayableObject、AutoAssociativities 为 SelectDisplayableObjectList，
	// 均支持 (DisplayableObject*, View*, Point3d) 三元重载（Edge 继承自
	// DisplayableObject，实体边可直接添加）。
	NXOpen::DisplayableObject* clDisp = centerlineObj;
	auto runBuilder = [&](bool horizontal) -> int
	{
		const char* dirName = horizontal ? "水平" : "垂直";
		int created = -1;   // -1 = 整组失败
		for (int pass = 0; pass < 2 && created < 0; ++pass)
		{
			NXOpen::Annotations::OrdinateDimensionBuilder* obBuilder = NULL;
			try
			{
				obBuilder = part->Dimensions()->CreateOrdinateDimensionBuilder(NULL);
				obBuilder->SetType(pass == 0
					? NXOpen::Annotations::BaseOrdinateDimensionBuilder::TypesMultipleDimension
					: NXOpen::Annotations::BaseOrdinateDimensionBuilder::TypesSingleDimension);
				// 基准：阶段6 创建的中心线（SetValue(对象, 视图, 基准点)）
				obBuilder->OrdinateOrigin()->SetValue(clDisp, sectionView,
					NXOpen::Point3d(0.0, 0.0, 0.0));
				// 端点集：曲线/边 NXObject + 所在视图 + 端点坐标
				for (size_t i = 0; i < curves.size(); ++i)
				{
					NXOpen::TaggedObject* tobj = NXOpen::NXObjectManager::Get(curves[i]);
					NXOpen::DisplayableObject* disp =
						dynamic_cast<NXOpen::DisplayableObject*>(tobj);
					if (!disp) continue;
					double pt[3], tg[3], pn[3], bn[3], torsion = 0.0, roc = 0.0;
					for (double parm = 0.0; parm <= 1.0 + 1e-9; parm += 1.0)
					{
						if (UF_MODL_ask_curve_props(curves[i], parm, pt, tg, pn, bn, &torsion, &roc) != 0)
							continue;
						NXOpen::Point3d q(pt[0], pt[1], pt[2]);
						bool dup = false;
						for (size_t k = 0; k < ends.size(); ++k)
						{
							const double dx = ends[k].X - q.X, dy = ends[k].Y - q.Y;
							if (dx * dx + dy * dy < 0.01 * 0.01) { dup = true; break; }
						}
						if (!dup) continue;   // 该端点已被去重丢弃，不重复 Add
						obBuilder->AutoAssociativities()->Add(disp, sectionView, q);
					}
				}
				// 放置方位（margin）决定标注方向
				if (horizontal) obBuilder->SetHorizontalInferredMarginLocation(hMargin);
				else            obBuilder->SetVerticalInferredMarginLocation(vMargin);
				obBuilder->Origin()->AnnotationView()->SetValue(sectionView);
				obBuilder->Commit();
				obBuilder->Destroy();
				obBuilder = NULL;
				created = (pass == 0) ? (int)ends.size() : 1;
			}
			catch (const NXOpen::NXException& e)
			{
				if (obBuilder) { obBuilder->Destroy(); obBuilder = NULL; }
				print(string("  警告: ") + dirName + "坐标标注" +
					(pass == 0 ? "成组" : "单条降级") + "模式失败: " + e.Message());
			}
			catch (...)
			{
				if (obBuilder) { obBuilder->Destroy(); obBuilder = NULL; }
				print(string("  警告: ") + dirName + "坐标标注失败（未知异常）");
			}
		}
		return created;
	};

	// 头文件语义为"放置方位决定方向"，单个 Builder 无法同时产出水平+垂直两组，
	// 故分别创建水平组与垂直组两个 Builder
	const int hCount = runBuilder(true);
	const int vCount = runBuilder(false);

	const std::string hStr = (hCount >= 0) ? std::to_string(hCount) + " 条" : "失败";
	const std::string vStr = (vCount >= 0) ? std::to_string(vCount) + " 条" : "失败";
	sprintf_s(fmt, sizeof(fmt),
		"[6a/8] 水平坐标标注 %s / 垂直坐标标注 %s / 轮廓边 %d 条",
		hStr.c_str(), vStr.c_str(), (int)curves.size());
	print(fmt);
}

//------------------------------------------------------------------------------
// 阶段6b：交互选边尺寸标注（线性 + 直径/半径）
//
// 与阶段6中心线同款交互框架：UF_UI_set_cursor_view(0) 切任意视图拾取，
// 结束恢复。放置点用 Selection::SelectScreenPosition 拾取屏幕位置
// （不要求选中对象）。每条标注单独 try/catch，失败降级警告不中断循环；
// Builder 在"全部拾取成功后才创建"，Commit 后立即 Destroy，
// catch 路径同样 Destroy，保证任何提前退出都不泄漏。
//------------------------------------------------------------------------------
void MyClass::phase_create_dimensions(NXOpen::Part* part, NXOpen::Drawings::DraftingView* sectionView)
{
	if (!sectionView)
	{
		print("[6b/8] 尺寸标注已跳过（无剖视图）");
		return;
	}

	// 保存当前光标视图，设为 0（任意视图）后才能拾取成员视图内的投影边
	int oldCursorView = 1;
	UF_UI_ask_cursor_view(&oldCursorView);
	UF_UI_set_cursor_view(0);

	int linearCount = 0;
	int radialCount = 0;

	try
	{
		// ---------- 线性标注循环 ----------
		for (;;)
		{
			NXOpen::NXObject* edge1 = NULL;
			NXOpen::NXObject* edge2 = NULL;
			NXOpen::Point3d cur1, cur2, placePt;
			NXOpen::View* placeView = NULL;

			NXOpen::Selection::Response r1 = theUI->SelectionManager()->SelectObject(
				"拾取第一条边（取消结束线性标注）", "线性标注-边1",
				NXOpen::Selection::SelectionScopeWorkPart, false, true, &edge1, &cur1);
			if (r1 != NXOpen::Selection::ResponseObjectSelected || !edge1)
			{
				break;   // 取消 -> 结束线性标注循环
			}

			NXOpen::Selection::Response r2 = theUI->SelectionManager()->SelectObject(
				"拾取第二条边（取消跳过本条标注）", "线性标注-边2",
				NXOpen::Selection::SelectionScopeWorkPart, false, true, &edge2, &cur2);
			if (r2 != NXOpen::Selection::ResponseObjectSelected || !edge2)
			{
				continue;   // 取消 -> 跳过本条标注，回到循环头
			}

			NXOpen::Selection::DialogResponse rp = theUI->SelectionManager()->SelectScreenPosition(
				"指定尺寸放置位置（取消跳过本条标注）", &placeView, &placePt);
			if (rp != NXOpen::Selection::DialogResponsePick)
			{
				continue;   // 取消 -> 跳过本条标注，回到循环头
			}

			NXOpen::Annotations::LinearDimensionBuilder* dimBuilder = NULL;
			try
			{
				dimBuilder = part->Dimensions()->CreateLinearDimensionBuilder(NULL);
				// SetValue(对象, 所在视图, 拾取点) —— 与中心线同款写法，关联建立在视图投影边上
				dimBuilder->FirstAssociativity()->SetValue(edge1, sectionView, cur1);
				dimBuilder->SecondAssociativity()->SetValue(edge2, sectionView, cur2);
				dimBuilder->Origin()->AnnotationView()->SetValue(sectionView);
				dimBuilder->Origin()->SetOriginPoint(placePt);
				dimBuilder->Commit();
				dimBuilder->Destroy();
				dimBuilder = NULL;
				++linearCount;
				print(string("[6b/8] 已创建线性标注 第 ") + std::to_string(linearCount) + " 条");
			}
			catch (const NXOpen::NXException& e)
			{
				if (dimBuilder) { dimBuilder->Destroy(); dimBuilder = NULL; }
				print(string("  警告: 线性标注创建失败: ") + e.Message());
			}
		}

		// ---------- 直径/半径标注循环 ----------
		for (;;)
		{
			NXOpen::NXObject* edge = NULL;
			NXOpen::Point3d cur, placePt;
			NXOpen::View* placeView = NULL;

			NXOpen::Selection::Response r = theUI->SelectionManager()->SelectObject(
				"拾取圆弧/圆边（取消结束直径标注）", "直径/半径标注",
				NXOpen::Selection::SelectionScopeWorkPart, false, true, &edge, &cur);
			if (r != NXOpen::Selection::ResponseObjectSelected || !edge)
			{
				break;   // 取消 -> 结束直径/半径标注循环
			}

			NXOpen::Selection::DialogResponse rp = theUI->SelectionManager()->SelectScreenPosition(
				"指定直径/半径标注放置位置（取消跳过本条标注）", &placeView, &placePt);
			if (rp != NXOpen::Selection::DialogResponsePick)
			{
				continue;   // 取消 -> 跳过本条标注，回到循环头
			}

			NXOpen::Annotations::RadialDimensionBuilder* dimBuilder = NULL;
			try
			{
				dimBuilder = part->Dimensions()->CreateRadialDimensionBuilder(NULL);
				dimBuilder->FirstAssociativity()->SetValue(edge, sectionView, cur);
				dimBuilder->SetHoleStyle(true);   // 孔直径样式
				dimBuilder->Origin()->AnnotationView()->SetValue(sectionView);
				dimBuilder->Origin()->SetOriginPoint(placePt);
				dimBuilder->Commit();
				dimBuilder->Destroy();
				dimBuilder = NULL;
				++radialCount;
				print(string("[6b/8] 已创建直径/半径标注 第 ") + std::to_string(radialCount) + " 条");
			}
			catch (const NXOpen::NXException& e)
			{
				if (dimBuilder) { dimBuilder->Destroy(); dimBuilder = NULL; }
				print(string("  警告: 直径/半径标注创建失败: ") + e.Message());
			}
		}
	}
	catch (const NXOpen::NXException& e)
	{
		print(string("  警告: 尺寸标注阶段异常: ") + e.Message());
	}
	catch (...)
	{
		print("  警告: 尺寸标注阶段异常（未知异常）");
	}

	// 恢复光标视图
	UF_UI_set_cursor_view(oldCursorView);

	print(string("[6b/8] 尺寸标注阶段结束（线性 ") + std::to_string(linearCount) +
		" 条，直径/半径 " + std::to_string(radialCount) + " 条）");
}

//==============================================================================
// 工序图层映射对话框（Win32 内存模板，无需 .dlx 模板文件）
// 界面：顶部下拉框选择"本次运行的工序"，下方逐行编辑 工序 i -> 图层。
// 确定后插件将用户在视图内选中的对象移动到所选工序对应的图层。
//（NX12 中视图继承自 NXObject 而非 DisplayableObject，视图本身无法换图层，
//  因此换图层对象由用户在阶段7交互选择）
//==============================================================================
namespace LayerDlg
{
	const int IDC_PROCESS_COMBO = 1000;
	const int IDC_LAYER_BASE    = 1100;   // 工序 i 的编辑框 ID = IDC_LAYER_BASE + i
	const int IDC_MODE_MOVE     = 1200;   // 模式A 单选：移动对象到工序图层
	const int IDC_MODE_STATE    = 1201;   // 模式B 单选：切换工序图层显隐状态

	struct Result
	{
		int currentProcess;               // 0-based
		int layers[MAX_PROCESS_COUNT];
		int mode;                         // 0=模式A(移动对象) 1=模式B(切换图层显隐)
	};
	static Result g_result;
	static int g_processCount;
	static int g_defaultLayers[MAX_PROCESS_COUNT];

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
			HWND cb = GetDlgItem(h, IDC_PROCESS_COMBO);
			WCHAR buf[16];
			for (int i = 0; i < g_processCount; ++i)
			{
				wsprintfW(buf, L"工序 %d", i + 1);
				SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)buf);
			}
			SendMessageW(cb, CB_SETCURSEL, 0, 0);
			for (int i = 0; i < g_processCount; ++i)
			{
				wsprintfW(buf, L"%d", g_defaultLayers[i]);
				SetWindowTextW(GetDlgItem(h, IDC_LAYER_BASE + i), buf);
			}
			// 默认选中模式A（移动对象到工序图层）
			CheckDlgButton(h, IDC_MODE_MOVE, BST_CHECKED);
			CheckDlgButton(h, IDC_MODE_STATE, BST_UNCHECKED);
			return TRUE;
		}
		case WM_COMMAND:
			switch (LOWORD(w))
			{
			case IDOK:
			{
				HWND cb = GetDlgItem(h, IDC_PROCESS_COMBO);
				int sel = (int)SendMessageW(cb, CB_GETCURSEL, 0, 0);
				if (sel < 0) sel = 0;
				g_result.currentProcess = sel;
				// 读取模式单选状态：0=模式A（移动对象） 1=模式B（切换显隐）
				g_result.mode =
					(IsDlgButtonChecked(h, IDC_MODE_STATE) == BST_CHECKED) ? 1 : 0;
				bool ok = true;
				WCHAR t[32];
				for (int i = 0; i < g_processCount; ++i)
				{
					GetWindowTextW(GetDlgItem(h, IDC_LAYER_BASE + i), t, 32);
					int v = _wtoi(t);
					if (v < 1 || v > 256) ok = false;
					g_result.layers[i] = v;
				}
				if (!ok)
				{
					MessageBoxW(h, L"图层号必须是 1~256 的整数",
						L"工序图层设置", MB_ICONWARNING | MB_OK);
					return TRUE;
				}
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
	bool Show(int count, const int* defLayers, Result& out)
	{
		g_processCount = count;
		for (int i = 0; i < count; ++i) g_defaultLayers[i] = defLayers[i];

		Buf b;
		// DLGTEMPLATE
		b.DW(WS_POPUP | WS_VISIBLE | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME);
		b.DW(0);
		// cdit 控件总数：标签+下拉框 + 模式单选x2 + 每工序(静态文本+编辑框) + 确定/取消
		b.W((WORD)(6 + count * 2));
		b.W(0); b.W(0); b.W(250); b.W((short)(58 + count * 15));
		b.W(0);                         // 无菜单
		b.W(0);                         // 默认类
		b.Str(L"工序图层设置");

		// 标签：本次运行工序
		AddItem(b, WS_CHILD | WS_VISIBLE | SS_LEFT,
			8, 8, 60, 10, 9000, 0x0082, L"本次工序:");
		// 下拉框：选择本次运行对应的工序
		AddItem(b, WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
			74, 6, 80, 80, IDC_PROCESS_COMBO, 0x0085, L"");

		// 模式选择单选按钮组（BS_AUTORADIOBUTTON 自动互斥勾选；
		// 首个按钮带 WS_GROUP 划定分组，第二/三个控件起 Tab 键切换范围正确）
		AddItem(b, WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_GROUP | BS_AUTORADIOBUTTON,
			8, 24, 112, 10, IDC_MODE_MOVE, 0x0080, L"移动对象到工序图层");
		AddItem(b, WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON,
			128, 24, 118, 10, IDC_MODE_STATE, 0x0080, L"切换工序图层显隐状态");

		short y = 40;
		WCHAR lbl[32];
		for (int i = 0; i < count; ++i)
		{
			wsprintfW(lbl, L"工序 %d  ->  图层", i + 1);
			AddItem(b, WS_CHILD | WS_VISIBLE | SS_LEFT,
				8, y, 90, 10, (WORD)(9100 + i), 0x0082, lbl);
			AddItem(b, WS_CHILD | WS_VISIBLE | WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL | ES_NUMBER,
				102, y - 2, 40, 12, (WORD)(IDC_LAYER_BASE + i), 0x0081, L"");
			y += 15;
		}

		AddItem(b, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
			30, y, 40, 14, IDOK, 0x0080, L"确定");
		AddItem(b, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
			80, y, 40, 14, IDCANCEL, 0x0080, L"取消");

		HWND parent = GetActiveWindow();
		INT_PTR r = DialogBoxIndirectW(NULL, (LPCDLGTEMPLATEW)(void*)b.d, parent, DlgProc);
		if (r == 1) { out = g_result; return true; }
		return false;
	}
}

//------------------------------------------------------------------------------
// 阶段7：工序换图层（双模式）
// 弹出对话框让用户指定"第几道工序对应哪个图层"（不硬编码），并选择模式：
// 模式A（默认）：确定后交互选择要换图层的对象（可在视图内多选，MB2/确定结束），
//   再用 LayerManager::MoveDisplayableObjects 移到所选工序对应的图层。
// 模式B：用 LayerManager::ChangeStates 切换图层显隐状态——当前工序图层
//   设为 StateSelectable，其余工序图层设为 StateHidden（不交互选对象）。
// 说明：NX12 中视图不是 DisplayableObject，无法直接换图层，
// 所以换图层对象由用户选择（新建的中心线/注释/曲线等均可选）。
//------------------------------------------------------------------------------
void MyClass::phase_apply_layers(NXOpen::Part* part, const ShellDrawingConfig& cfg)
{
	LayerDlg::Result res;
	if (!LayerDlg::Show(cfg.processCount, cfg.defaultProcessLayers, res))
	{
		print("[7/8] 已跳过换图层（用户取消对话框）");
		return;
	}

	// 回显全部映射，便于核对
	for (int i = 0; i < cfg.processCount; ++i)
	{
		print(string("  - 工序 ") + std::to_string(i + 1) +
			" -> 图层 " + std::to_string(res.layers[i]));
	}

	int proc = res.currentProcess;
	int layer = res.layers[proc];
	if (layer < 1 || layer > 256)
	{
		print("[7/8] 已跳过换图层（所选工序图层号无效）");
		return;
	}

	// ---------- 模式B：切换工序图层显隐状态（不选对象） ----------
	// 当前所选工序的图层 -> StateSelectable（可见可选），
	// 其余工序的图层   -> StateHidden（隐藏）；
	// 与录制 111r_vb.vb 的工序切换语义一致。
	if (res.mode == 1)
	{
		try
		{
			int workLayer = part->Layers()->WorkLayer();

			std::vector<NXOpen::Layer::StateInfo> states;
			// 当前工序图层先入队（若多个工序映射到同一图层，以当前工序为准）；
			// 工作图层不能设为 Selectable/Hidden，保持 StateWorkLayer
			states.push_back(NXOpen::Layer::StateInfo(layer,
				(layer == workLayer) ? NXOpen::Layer::StateWorkLayer
									   : NXOpen::Layer::StateSelectable));

			for (int i = 0; i < cfg.processCount; ++i)
			{
				if (i == proc) continue;
				int ly = res.layers[i];
				if (ly < 1 || ly > 256) continue;   // 沿用图层号有效性校验
				bool dup = false;
				for (size_t k = 0; k < states.size(); ++k)
				{
					if (states[k].Layer == ly) { dup = true; break; }
				}
				if (dup) continue;
				states.push_back(NXOpen::Layer::StateInfo(ly,
					(ly == workLayer) ? NXOpen::Layer::StateWorkLayer
										: NXOpen::Layer::StateHidden));
			}

			part->Layers()->ChangeStates(states, true);

			// 回显每个图层切换后的实际状态
			for (size_t k = 0; k < states.size(); ++k)
			{
				NXOpen::Layer::State st = part->Layers()->GetState(states[k].Layer);
				const char* stName =
					(st == NXOpen::Layer::StateWorkLayer)  ? "工作(Work)"     :
					(st == NXOpen::Layer::StateSelectable) ? "可选(Selectable)" :
					(st == NXOpen::Layer::StateVisible)    ? "可见(Visible)"   :
																"隐藏(Hidden)";
				print(string("  - 图层 ") + std::to_string(states[k].Layer) +
					" 新状态: " + stName);
			}
			print(string("[7/8] 已切换工序图层显隐状态（工序 ") +
				std::to_string(proc + 1) + " 的图层 " + std::to_string(layer) + " 可见可选）");
		}
		catch (const NXOpen::NXException& e)
		{
			print(string("  警告: 切换图层状态失败: ") + e.Message());
		}
		return;
	}

	// ---------- 模式A：交互选择对象移动到所选工序对应图层 ----------
	try
	{
		// 允许在任意视图内选择（含成员视图内的对象）
		int oldCursorView = 1;
		UF_UI_ask_cursor_view(&oldCursorView);
		UF_UI_set_cursor_view(0);

		std::vector<NXOpen::NXObject*> picked;
		NXOpen::Selection::Response rsp = theUI->SelectionManager()->SelectObjects(
			"选择要换图层的对象（可多选，确定/MB2 结束）", "工序换图层",
			NXOpen::Selection::SelectionScopeWorkPart, false, true, picked);

		UF_UI_set_cursor_view(oldCursorView);

		if (rsp != NXOpen::Selection::ResponseOk &&
			rsp != NXOpen::Selection::ResponseObjectSelected &&
			rsp != NXOpen::Selection::ResponseBack)
		{
			print("[7/8] 已跳过换图层（未选择对象）");
			return;
		}

		// 只保留可显示对象（MoveDisplayableObjects 的参数类型要求）
		std::vector<NXOpen::DisplayableObject*> objs;
		for (size_t i = 0; i < picked.size(); ++i)
		{
			NXOpen::DisplayableObject* d =
				dynamic_cast<NXOpen::DisplayableObject*>(picked[i]);
			if (d) objs.push_back(d);
		}
		if (objs.empty())
		{
			print("[7/8] 已跳过换图层（所选对象中没有可换图层的对象）");
			return;
		}

		part->Layers()->MoveDisplayableObjects(layer, objs);
		print(string("[7/8] 已将 ") + std::to_string((int)objs.size()) +
			" 个对象移动到图层 " + std::to_string(layer) +
			"（工序 " + std::to_string(proc + 1) + "）");
	}
	catch (const NXOpen::NXException& e)
	{
		print(string("  警告: 换图层失败: ") + e.Message());
	}
}

//------------------------------------------------------------------------------
// 阶段8.1：剖视比例设置（单图流程中执行一次；DoUpdate 由主流程统一收尾）
//------------------------------------------------------------------------------
void MyClass::phase_post_process_view_scale(NXOpen::Part* part, const ShellDrawingConfig& cfg,
	NXOpen::Drawings::SectionView* sectionView)
{
	// 8.1 设置剖视图比例
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
			print("  - 已设置剖视图比例");
		}
		catch (NXOpen::NXException& e)
		{
			print(string("  警告: 比例设置失败: ") + e.Message());
		}
	}

	// 8.2 剖视图位置由阶段4b 两步法居中一次性完成（对齐独立目标点），
	//     不再 MoveView；载体视图同样由阶段3b 对齐其目标点
}

//------------------------------------------------------------------------------
// 主流程
//------------------------------------------------------------------------------
void MyClass::do_it()
{
	try
	{
		// ==================== 配置区（按零件修改） ====================
		// 图纸坐标缩放常量：录制坐标基于 A0（1189 长边），
		// 实际出 A4（297 长边）图纸，图纸坐标统一乘以该比例；
		// 模型坐标不缩放。
		static const double kSheetScale = 297.0 / 1189.0;

		ShellDrawingConfig cfg;
		cfg.templatePath =
			"D:\\Program Files\\Siemens\\NX 12.0\\localization\\prc\\simpl_chinese\\startup\\A4-noviews-asm-template.prt";

		// partPath 自动取当前工作部件完整路径（BasePart::FullPath，返回 NXString，
		// 用局部 std::string 保存以保证 c_str() 存活到流程结束）；
		// 获取失败或为空（部件未保存过）时回退到硬编码路径
		static const char* kFallbackPartPath = "Y:\\Brisk\\BV3\\BV3R\\2D\\02KBV3RCL20A.prt";
		std::string workPartPath;
		try
		{
			workPartPath = workPart->FullPath().GetText();
		}
		catch (...) { workPartPath.clear(); }
		if (workPartPath.empty()) workPartPath = kFallbackPartPath;
		cfg.partPath = workPartPath.c_str();

		cfg.sheetHeight = 210.0;
		cfg.sheetLength = 297.0;
		cfg.sheetScaleNum = 1.0;
		cfg.sheetScaleDen = 1.0;

		// 基础视图（兜底载体）：NX12 剖视图必须挂在父成员视图下，
		// 故保留一个基础视图作为剖视父视图。
		// 载体视图=剖视父视图，按零件轴向选择：本零件回转轴沿模型 X 轴，
		// 录制脚本使用 Right（用 Top 会出斜视/内容错误）
		cfg.baseModelViewName = "Right";
		// 载体端面视图目标中心（A4 图纸坐标，左下角原点）：对照目标图的估算初值
		//（±8mm），实机可微调；直接使用，不再走 A0×kSheetScale 派生；
		// 同时作为铅垂剖切线的参考中心
		cfg.baseViewPlace = Point3d(82.0, 62.0, 0.0);

		// ---------- A-A 全剖视图（主体视图） ----------
		// 剖切线半长 d：两点运行时取载体放置点 ±(0,d)（铅垂过轴）；
		// 初值参考录制剖切线长度约 108 A0 单位（半长约 54），取 50，待实机微调；
		// 录制坐标系（A0 图面），下方乘 kSheetScale 缩放
		cfg.aaSecHalfLen = 50.0;
		// A-A 剖视图目标中心（A4 图纸坐标，独立目标点）：对照目标图的估算初值
		//（±8mm），实机可微调；直接使用，不再走 A0×kSheetScale 派生，
		// 也不再由"载体中心 + aaViewOffsetY"派生（视图位置对齐目标图）
		cfg.aaViewTarget = Point3d(185.0, 112.0, 0.0);
		// 注：aaViewOffsetY 已移除——由独立目标点 aaViewTarget 取代
		// 注：aaViewOrigin 已弃用删除——录制中 vieworigin1/ENTITY 2 1 对齐点
		// 属于投影视图 Builder 参数，误用于剖视图导致内容错误
		cfg.aaViewScale = 1.5;
		cfg.aaViewLabel = "A-A";

		// 中心线：交互拾取视图内投影边生成（与投影边关联），取消选择即跳过
		cfg.createCenterline = true;

		// 轮廓端点自动坐标标注：成组水平/垂直坐标标注（基准=中心线），
		// 在中心线创建之后、交互标注之前执行；中心线不可用时降级跳过
		cfg.autoContourDims = true;

		// 尺寸标注：交互拾取边生成线性/直径半径标注，取消选择即跳过
		cfg.createDimensions = true;

		// 工序图层映射默认值（运行时可通过对话框修改，不硬编码）
		cfg.processCount = 4;
		cfg.defaultProcessLayers[0] = 21;   // 工序1 -> 图层21
		cfg.defaultProcessLayers[1] = 22;   // 工序2 -> 图层22
		cfg.defaultProcessLayers[2] = 23;   // 工序3 -> 图层23
		cfg.defaultProcessLayers[3] = 24;   // 工序4 -> 图层24
		for (int i = 4; i < MAX_PROCESS_COUNT; ++i)
			cfg.defaultProcessLayers[i] = 21 + i;

		// 图纸配置：当前仅出"车床图"一张，其余图纸（CNC品检图/品检图/工序图1-4）
		// 已按需求移除；保留 SheetDesc 数组结构便于日后恢复多图。
		// A4 横向 297x210、1:1；name 为中文图纸名（SetName）；
		// 图号（SetNumber）不在此配置——新建图纸时运行时动态分配 =
		// 现有图纸数量 + 1（NX12 要求图号为从 1 起的连续整数序列，
		// 跳号会报 "The drawing sheet number is not in sequence."，
		// 详见 SheetDesc 注释）；
		// 图纸级隐藏 = 全局状态为隐藏的图层，用户在 NX 图层设置(Ctrl+L)
		// 中控制哪些图层出图，无需在此配置可见图层列表
		SheetDesc sheets[1] = {
			{ "车床图", 210.0, 297.0, 1.0, 1.0 },
		};
		const int sheetCount = 1;

		// 图纸坐标缩放：仅剖切线半长 d（录制 A0 坐标量级）参与 kSheetScale 缩放；
		// 载体目标中心与剖视图目标中心均为 A4 图纸坐标初值，直接使用不再缩放
		cfg.aaSecHalfLen *= kSheetScale;
		// =================================================================

		// 将 BasePart 转为 Part（制图 API 需要）
		NXOpen::Part* part = dynamic_cast<NXOpen::Part*>(workPart);
		if (!part)
		{
			print("错误：当前工作部件不是 Part 类型，无法进入制图模块");
			return;
		}

		// ---------- 阶段1：进入制图模块 ----------
		theSession->ApplicationSwitchImmediate("UG_APP_DRAFTING");
		part->Drafting()->EnterDraftingApplication();
		print("[1/8] 已进入制图模块");

		// ---------- 阶段2~6+8.1：车床图单张顺序执行（建图→图层可见性→视图→剖视比例） ----------
		const SheetDesc& sd = sheets[0];
		(void)sheetCount;
		const std::string tag = "[车床图]";
		print(tag + " 开始处理");

		// 阶段2：创建/复用图纸（一级幂等：按图纸名匹配）
		NXOpen::Drawings::DraftingDrawingSheet* sheet = phase_create_sheet(part, cfg, sd);
		if (!sheet)
		{
			print(tag + " 图纸创建失败，流程终止");
			return;
		}

		// 阶段2b：图纸级图层可见性（按图层显隐状态过滤，sheet->Open() 之后）
		phase_apply_sheet_layer_visibility(part, sheet);

		// 二级幂等：图纸已有视图则跳过视图创建
		if (!sheet->GetDraftingViews().empty())
		{
			print(tag + " 已存在视图，跳过视图创建");
		}
		else
		{
			// 阶段3：基础视图（载体视图，放置于目标中心）
			NXOpen::Drawings::BaseView* baseView = phase_create_base_view(part, cfg);
			if (!baseView)
			{
				print(tag + " 基础视图创建失败，流程终止");
				return;
			}

			// 阶段3b：载体视图两步法定位收敛到目标中心（视图位置对齐目标图）；
			// 收敛后的实际位置即后续剖切线两点的参考中心
			phase_center_view(part, baseView, sd, &cfg.baseViewPlace);

			// 阶段4：A-A 全剖视图（主体视图，挂在载体基础视图下；
			// 剖切线两点仍相对载体实际位置计算）
			NXOpen::Drawings::SectionView* sectionView = phase_create_aa_section(part, cfg, baseView);

			// 阶段4b：A-A 剖视图两步法精确定位（仅新建视图时执行）：
			// 目标中心 = 独立目标点 cfg.aaViewTarget（视图位置对齐目标图）
			NXOpen::Point3d aaTarget(cfg.aaViewTarget);
			phase_center_view(part, sectionView, sd, &aaTarget);

			// 阶段6：中心线（交互式，用户取消即跳过；关联到剖视图投影边）
			NXOpen::Annotations::Centerline2d* centerlineObj = NULL;
			if (cfg.createCenterline)
			{
				centerlineObj = phase_create_centerline(part, sectionView);
			}
			else
			{
				print("[6/8] 中心线阶段已跳过（配置关闭）");
			}

			// 阶段6a：轮廓端点自动坐标标注（成组水平/垂直，基准=中心线）
			if (cfg.autoContourDims)
			{
				phase_auto_contour_dims(part, sectionView, centerlineObj);
			}
			else
			{
				print("[6a/8] 轮廓坐标标注阶段已跳过（配置关闭）");
			}

			// 阶段6b：尺寸标注（交互式，用户取消即结束对应循环）
			if (cfg.createDimensions && sectionView)
			{
				phase_create_dimensions(part, sectionView);
			}

			// 阶段8.1：剖视图比例
			phase_post_process_view_scale(part, cfg, sectionView);
		}
		print(tag + " 处理完成");

		// ---------- 阶段7：工序换图层（对话框指定映射，视图流程之后执行一次） ----------
		phase_apply_layers(part, cfg);

		// ---------- 阶段8：后处理（最终更新只执行一次；剖视比例已在上方设置） ----------
		NXOpen::Session::UndoMarkId finalMark = theSession->SetUndoMark(
			NXOpen::Session::MarkVisibilityVisible, "Final Update");
		theSession->UpdateManager()->DoUpdate(finalMark);
		print("[8/8] 后处理完成（最终更新）");

		print("========== 壳体出图流程执行完毕 ==========");
	}
	catch (const NXOpen::NXException& e1)
	{
		print(std::string("!!! NXException: ") + e1.Message());
		mb->Show("NXException", NXOpen::NXMessageBox::DialogTypeError, e1.Message());
	}
	catch (const std::exception& e2)
	{
		print(std::string("!!! std::exception: ") + e2.what());
		mb->Show("Exception", NXOpen::NXMessageBox::DialogTypeError, e2.what());
	}
	catch (...)
	{
		print("!!! Unknown Exception");
		mb->Show("Exception", NXOpen::NXMessageBox::DialogTypeError, "Unknown Exception.");
	}
}

//------------------------------------------------------------------------------
// Entry point(s) for unmanaged internal NXOpen C/C++ programs
//------------------------------------------------------------------------------
extern "C" DllExport void ufusr(char *parm, int *returnCode, int rlen)
{
	try
	{
		MyClass *theMyClass;
		theMyClass = new MyClass();
		theMyClass->do_it();
		delete theMyClass;
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
