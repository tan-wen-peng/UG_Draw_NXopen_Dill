// NX12_NXOpenCPP_打标图
// step1 : 打标图 - 创建坐标（BV3 F 前盖）
// 录制来源: d:\A_UG\03_打标图录制\BV3打标图，创建坐标F.vb
//
// 步骤（已忽略录制中的视图缩放/平移等纯交互操作）:
//   1. 图层设置: 49 -> Selectable
//   2. 直线1: 圆弧A中心 -> 圆弧B中心（图层49上的两个圆）
//   3. 直线4: (356.876, -0.0704159, 0) -> 原点（录制中 builder2 的线，
//      方向近似 -X，用于约束 WCS 的 X 轴，X 轴与其向量方向相反）
//   4. 图层设置: 205 -> Hidden -> Selectable; 221 -> Hidden
//   5. 直线2: 直线1中点(50%参数) -> 圆弧C中心（图层205上的圆）
//   6. 直线3: 绝对坐标 (-50,0,0) -> (50,0,0)
//   7. 方向: dir1 = 直线2正向; dir2 = 直线3反向
//   8. WCS: 原点(0,0,0) + 绕X轴旋转153°; 保存坐标系
//
// 创建的对象已命名，供后续步骤(step2...)查找:
//   DBT_LINE1 / DBT_LINE2 / DBT_LINE3 / DBT_LINE4 / DBT_DIR1 / DBT_DIR2 / DBT_CSYS_F
//
// 说明:
//   - 圆弧优先按图层自动查找（图层49需2个圆，图层205需1个圆），
//     数量不符时自动弹出类选择对话框交互选择（兜底）。
//   - 圆弧圆心点与录制一致（PointCollection::CreatePoint(IBaseCurve*, ...)
//     即"曲线/边中心点"，对应录制中 CreatePoint(arc, UpdateOption)）。

// Mandatory UF Includes
#include <uf.h>
#include <uf_curve.h>
#include <uf_layer.h>
#include <uf_obj.h>
#include <uf_object_types.h>
#include <uf_ui.h>
#include <uf_ui_types.h>

// Internal Includes
#include <NXOpen/ListingWindow.hxx>
#include <NXOpen/NXMessageBox.hxx>
#include <NXOpen/UI.hxx>

// Internal+External Includes
#include <NXOpen/Annotations.hxx>
#include <NXOpen/Arc.hxx>
#include <NXOpen/Assemblies_Component.hxx>
#include <NXOpen/Assemblies_ComponentAssembly.hxx>
#include <NXOpen/Body.hxx>
#include <NXOpen/BodyCollection.hxx>
#include <NXOpen/CartesianCoordinateSystem.hxx>
#include <NXOpen/Conic.hxx>
#include <NXOpen/Direction.hxx>
#include <NXOpen/DirectionCollection.hxx>
#include <NXOpen/Edge.hxx>
#include <NXOpen/Expression.hxx>
#include <NXOpen/ExpressionCollection.hxx>
#include <NXOpen/Face.hxx>
#include <NXOpen/Features_AssociativeLine.hxx>
#include <NXOpen/Features_AssociativeLineBuilder.hxx>
#include <NXOpen/Features_BaseFeatureCollection.hxx>
#include <NXOpen/IBaseCurve.hxx>
#include <NXOpen/INXObject.hxx>
#include <NXOpen/Layer.hxx>
#include <NXOpen/Layer_LayerManager.hxx>
#include <NXOpen/Line.hxx>
#include <NXOpen/NXException.hxx>
#include <NXOpen/NXObject.hxx>
#include <NXOpen/NXObjectManager.hxx>
#include <NXOpen/Part.hxx>
#include <NXOpen/PartCollection.hxx>
#include <NXOpen/Point.hxx>
#include <NXOpen/PointCollection.hxx>
#include <NXOpen/Scalar.hxx>
#include <NXOpen/ScalarCollection.hxx>
#include <NXOpen/SelectPoint.hxx>
#include <NXOpen/Session.hxx>
#include <NXOpen/SmartObject.hxx>
#include <NXOpen/ugmath.hxx>
#include <NXOpen/WCS.hxx>

// Std C++ Includes
#include <cstdio>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <vector>

using namespace NXOpen;
using std::string;
using std::exception;
using std::stringstream;
using std::endl;
using std::cout;
using std::cerr;


//------------------------------------------------------------------------------
// NXOpen c++ test class 
//------------------------------------------------------------------------------
class MyClass
{
    // class members
public:
    static Session *theSession;
    static UI *theUI;

    MyClass();
    ~MyClass();

	void do_it();
	void print(const NXString &);
	void print(const string &);
	void print(const char*);

	// step1: 打标图 - 创建坐标
	void step1();

private:
	BasePart *workPart, *displayPart;
	NXMessageBox *mb;
	ListingWindow *lw;
	LogFile *lf;

	// step1 工具函数
	void setLayerState(int layer, NXOpen::Layer::State state);
	bool arcFromTag(tag_t tag, NXOpen::IBaseCurve **arc);
	std::vector<NXOpen::IBaseCurve*> arcsOnLayer(int layer);
	std::vector<NXOpen::IBaseCurve*> selectArcs(int wanted, const char *cue);
	NXOpen::Features::AssociativeLine* createLine(NXOpen::Point *startPt, NXOpen::Point *endPt);
	NXOpen::Line* lineFromFeature(NXOpen::Features::AssociativeLine *feat);
	void setObjName(NXOpen::NXObject *obj, const char *name);
	void printArc(NXOpen::IBaseCurve *arc, const char *tag);
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

	// Initialize the NX Open C++ API environment
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
// Print string to listing window or stdout
//------------------------------------------------------------------------------
void MyClass::print(const NXString &msg)
{
	if(! lw->IsOpen() ) lw->Open();
	lw->WriteLine(msg);
}
void MyClass::print(const string &msg)
{
	if(! lw->IsOpen() ) lw->Open();
	lw->WriteLine(msg);
}
void MyClass::print(const char * msg)
{
	if(! lw->IsOpen() ) lw->Open();
	lw->WriteLine(msg);
}




//------------------------------------------------------------------------------
// Do something
//------------------------------------------------------------------------------
void MyClass::do_it()
{

	step1();

}

//------------------------------------------------------------------------------
// step1: 打标图 - 创建坐标（对应 BV3打标图，创建坐标F.vb）
//------------------------------------------------------------------------------
void MyClass::step1()
{
	print("=== step1: marking lines + save WCS (BV3 F) ===");

	// ---- 1. 图层设置: 49 -> Selectable ----
	setLayerState(49, NXOpen::Layer::StateSelectable);
	print("layer 49 -> Selectable");

	// ---- 2. 直线1: 图层49上圆弧A中心 -> 圆弧B中心 ----
	std::vector<NXOpen::IBaseCurve*> arcsA = arcsOnLayer(49);
	if ((int)arcsA.size() != 2)
	{
		stringstream ss;
		ss << "auto-detect: expect 2 circles on layer 49, found " << arcsA.size()
		   << " -> select manually";
		print(ss.str());
		arcsA = selectArcs(2, "select 2 circles for line 1 (start / end)");
	}
	if ((int)arcsA.size() < 2)
	{
		print("step1 aborted: line 1 needs 2 circles");
		return;
	}
	printArc(arcsA[0], "line1 start arc");
	printArc(arcsA[1], "line1 end arc");

	NXOpen::Point *ptStart = workPart->Points()->CreatePoint(arcsA[0], NXOpen::SmartObject::UpdateOptionWithinModeling);
	NXOpen::Point *ptEnd   = workPart->Points()->CreatePoint(arcsA[1], NXOpen::SmartObject::UpdateOptionWithinModeling);
	NXOpen::Features::AssociativeLine *feat1 = createLine(ptStart, ptEnd);
	NXOpen::Line *line1 = lineFromFeature(feat1);
	if (!line1)
		throw std::runtime_error("step1 failed: line 1 commit failed");
	setObjName(line1, "DBT_LINE1");
	print("line 1 created: DBT_LINE1");

	// ---- 3. 直线4: (356.876, -0.0704159, 0) -> 原点 ----
	// 录制中 builder2 的线（StartPoint = point3），方向近似 -X，
	// 用于约束 WCS 的 X 轴（X 轴与其向量方向相反）
	NXOpen::Point *ptC = workPart->Points()->CreatePoint(NXOpen::Point3d(356.876, -0.0704159, 0.0));
	NXOpen::Point *ptOrigin = workPart->Points()->CreatePoint(NXOpen::Point3d(0.0, 0.0, 0.0));
	NXOpen::Features::AssociativeLine *feat4 = createLine(ptC, ptOrigin);
	NXOpen::Line *line4 = lineFromFeature(feat4);
	if (!line4)
		throw std::runtime_error("step1 failed: line 4 commit failed");
	setObjName(line4, "DBT_LINE4");
	print("line 4 created: DBT_LINE4 (WCS X-axis reference)");

	// ---- 4. 图层设置: 205 -> Hidden -> Selectable; 221 -> Hidden ----
	setLayerState(205, NXOpen::Layer::StateHidden);
	setLayerState(205, NXOpen::Layer::StateSelectable);
	setLayerState(221, NXOpen::Layer::StateHidden);
	print("layer 205 -> Selectable, layer 221 -> Hidden");

	// ---- 5. 直线2: 直线1中点(50%) -> 图层205上圆弧C中心 ----
	std::vector<NXOpen::IBaseCurve*> arcsB = arcsOnLayer(205);
	if ((int)arcsB.size() != 1)
	{
		stringstream ss;
		ss << "auto-detect: expect 1 circle on layer 205, found " << arcsB.size()
		   << " -> select manually";
		print(ss.str());
		arcsB = selectArcs(1, "select 1 circle for line 2 (end point)");
	}
	if (arcsB.empty())
	{
		print("step1 aborted: line 2 needs 1 circle");
		return;
	}
	printArc(arcsB[0], "line2 end arc");

	// 直线1的50%参数点（关联点）
	NXOpen::Expression *exp50 = workPart->Expressions()->CreateSystemExpression("50");
	NXOpen::Scalar *scalar50 = workPart->Scalars()->CreateScalarExpression(
		exp50, NXOpen::Scalar::DimensionalityTypeNone,
		NXOpen::SmartObject::UpdateOptionWithinModeling);
	NXOpen::Point *ptMid = workPart->Points()->CreatePoint(
		line1, scalar50,
		NXOpen::PointCollection::PointOnCurveLocationOptionPercentParameter,
		NXOpen::SmartObject::UpdateOptionWithinModeling);

	NXOpen::Point *ptArcC = workPart->Points()->CreatePoint(arcsB[0], NXOpen::SmartObject::UpdateOptionWithinModeling);
	NXOpen::Features::AssociativeLine *feat2 = createLine(ptMid, ptArcC);
	NXOpen::Line *line2 = lineFromFeature(feat2);
	if (!line2)
		throw std::runtime_error("step1 failed: line 2 commit failed");
	setObjName(line2, "DBT_LINE2");
	print("line 2 created: DBT_LINE2");

	// ---- 6. 直线3: 绝对坐标 (-50,0,0) -> (50,0,0) ----
	NXOpen::Point *ptA = workPart->Points()->CreatePoint(NXOpen::Point3d(-50.0, 0.0, 0.0));
	NXOpen::Point *ptB = workPart->Points()->CreatePoint(NXOpen::Point3d(50.0, 0.0, 0.0));
	NXOpen::Features::AssociativeLine *feat3 = createLine(ptA, ptB);
	NXOpen::Line *line3 = lineFromFeature(feat3);
	if (!line3)
		throw std::runtime_error("step1 failed: line 3 commit failed");
	setObjName(line3, "DBT_LINE3");
	print("line 3 created: DBT_LINE3");

	// ---- 7. 方向: dir1 = 直线2正向; dir2 = 直线3反向 ----
	NXOpen::Direction *dir1 = workPart->Directions()->CreateDirection(
		line2, NXOpen::Sense::SenseForward, NXOpen::SmartObject::UpdateOptionAfterModeling);
	NXOpen::Direction *dir2 = workPart->Directions()->CreateDirection(
		line3, NXOpen::Sense::SenseReverse, NXOpen::SmartObject::UpdateOptionAfterModeling);
	setObjName(dir1, "DBT_DIR1");
	setObjName(dir2, "DBT_DIR2");
	print("directions created: DBT_DIR1 / DBT_DIR2");

	// ---- 8. WCS: 原点(0,0,0) + 绕X轴旋转153°, 保存 ----
	// 录制矩阵 (rotX(153°)):
	//   Yy=-0.89100652418836968  Yz=-0.45399049973954336
	//   Zy= 0.45399049973954336  Zz=-0.89100652418836968
	NXOpen::Matrix3x3 m;
	m.Xx = 1.0; m.Xy = 0.0;                    m.Xz = 0.0;
	m.Yx = 0.0; m.Yy = -0.8910065241883679;    m.Yz = -0.4539904997395468;
	m.Zx = 0.0; m.Zy =  0.4539904997395468;    m.Zz = -0.8910065241883679;
	workPart->WCS()->SetOriginAndMatrix(NXOpen::Point3d(0.0, 0.0, 0.0), m);
	NXOpen::CartesianCoordinateSystem *savedCsys = workPart->WCS()->Save();
	if (savedCsys)
		setObjName(savedCsys, "DBT_CSYS_F");
	print("WCS saved: DBT_CSYS_F");

	print("=== step1 done ===");
}

//------------------------------------------------------------------------------
// step1 helper: 设置图层状态
//------------------------------------------------------------------------------
void MyClass::setLayerState(int layer, NXOpen::Layer::State state)
{
	std::vector<NXOpen::Layer::StateInfo> infos;
	infos.push_back(NXOpen::Layer::StateInfo(layer, state));
	workPart->Layers()->ChangeStates(infos, false);
}

//------------------------------------------------------------------------------
// step1 helper: tag -> 圆对象（独立圆/圆弧 Arc，或圆形实体边 Edge）
//------------------------------------------------------------------------------
bool MyClass::arcFromTag(tag_t tag, NXOpen::IBaseCurve **arc)
{
	*arc = NULL;
	int type = 0, subtype = 0;
	UF_OBJ_ask_type_and_subtype(tag, &type, &subtype);

	NXOpen::TaggedObject *obj = NXOpen::NXObjectManager::Get(tag);
	if (!obj)
		return false;

	// 独立圆/圆弧
	if (type == UF_circle_type)
	{
		NXOpen::Arc *a = dynamic_cast<NXOpen::Arc*>(obj);
		if (a) { *arc = a; return true; }
		return false;
	}
	// 实体圆边（NX12 中 Edge 直接实现 IBaseCurve）
	if (type == UF_solid_type && subtype == UF_solid_edge_subtype)
	{
		NXOpen::Edge *edge = dynamic_cast<NXOpen::Edge*>(obj);
		if (edge && edge->SolidEdgeType() == NXOpen::Edge::EdgeTypeCircular)
		{
			*arc = edge;
			return true;
		}
	}
	return false;
}

//------------------------------------------------------------------------------
// step1 helper: 收集图层上的圆/圆弧
//------------------------------------------------------------------------------
std::vector<NXOpen::IBaseCurve*> MyClass::arcsOnLayer(int layer)
{
	std::vector<NXOpen::IBaseCurve*> arcs;
	tag_t objTag = NULL_TAG;
	while (UF_LAYER_cycle_by_layer(layer, &objTag) == 0 && objTag != NULL_TAG)
	{
		NXOpen::IBaseCurve *arc = NULL;
		if (arcFromTag(objTag, &arc) && arc)
			arcs.push_back(arc);
	}
	return arcs;
}

//------------------------------------------------------------------------------
// step1 helper: 逐个交互选择圆弧（选一个圆按一次确定，实时反馈）
//------------------------------------------------------------------------------
std::vector<NXOpen::IBaseCurve*> MyClass::selectArcs(int wanted, const char *cue)
{
	std::vector<NXOpen::IBaseCurve*> arcs;

	for (int i = 0; i < wanted; ++i)
	{
		char cueLine[256];
		sprintf_s(cueLine, sizeof(cueLine), "%s (%d/%d)", cue, i + 1, wanted);

		int response = 0, count = 0;
		tag_p_t objects = NULL;
		int err = UF_UI_select_with_class_dialog(cueLine,
			const_cast<char*>("DBT - select circle"),
			UF_UI_SEL_SCOPE_WORK_PART, NULL, NULL, &response, &count, &objects);
		if (err != 0)
		{
			print("selection dialog error");
			return arcs;
		}
		// 类选择器点确定返回 UF_UI_OK(2)；兼容单选框的选中响应码(7/8)
		bool ok = (response == UF_UI_OK
				|| response == UF_UI_OBJECT_SELECTED
				|| response == UF_UI_OBJECT_SELECTED_BY_NAME);
		if (!ok)
		{
			print("selection canceled");
			return arcs;
		}
		NXOpen::IBaseCurve *arc = NULL;
		if (count > 0 && objects && arcFromTag(objects[0], &arc) && arc)
		{
			arcs.push_back(arc);
			char idx[16];
			sprintf_s(idx, sizeof(idx), "sel %d", (int)arcs.size() - 1);
			printArc(arc, idx);
		}
		else
		{
			print("selected object is not a circle, please re-select");
			--i;  // 重选当前
		}
		if (objects)
			UF_free(objects);
	}
	return arcs;
}

//------------------------------------------------------------------------------
// step1 helper: 创建关联直线（起点/终点均为 Point）
//------------------------------------------------------------------------------
NXOpen::Features::AssociativeLine* MyClass::createLine(NXOpen::Point *startPt, NXOpen::Point *endPt)
{
	NXOpen::Features::AssociativeLineBuilder *builder =
		workPart->BaseFeatures()->CreateAssociativeLineBuilder(
			static_cast<NXOpen::Features::AssociativeLine*>(NULL));

	builder->SetStartPointOptions(NXOpen::Features::AssociativeLineBuilder::StartOptionPoint);
	builder->SetEndPointOptions(NXOpen::Features::AssociativeLineBuilder::EndOptionPoint);
	builder->StartPoint()->SetValue(startPt);
	builder->EndPoint()->SetValue(endPt);

	NXOpen::NXObject *committed = builder->Commit();
	builder->Destroy();

	return dynamic_cast<NXOpen::Features::AssociativeLine*>(committed);
}

//------------------------------------------------------------------------------
// step1 helper: 取关联直线的曲线对象 (CURVE 1)
//------------------------------------------------------------------------------
NXOpen::Line* MyClass::lineFromFeature(NXOpen::Features::AssociativeLine *feat)
{
	if (!feat)
		return NULL;
	NXOpen::INXObject *obj = feat->FindObject("CURVE 1");
	return dynamic_cast<NXOpen::Line*>(obj);
}

//------------------------------------------------------------------------------
// step1 helper: 对象命名（供后续步骤查找）
//------------------------------------------------------------------------------
void MyClass::setObjName(NXOpen::NXObject *obj, const char *name)
{
	if (!obj)
		return;
	UF_OBJ_set_name(obj->Tag(), name);
}

//------------------------------------------------------------------------------
// step1 helper: 打印圆弧中心信息
//------------------------------------------------------------------------------
void MyClass::printArc(NXOpen::IBaseCurve *arc, const char *tag)
{
	double cx = 0.0, cy = 0.0, cz = 0.0;
	NXOpen::Conic *conic = dynamic_cast<NXOpen::Conic*>(arc);
	if (conic)
	{
		NXOpen::Point3d c = conic->CenterPoint();
		cx = c.X; cy = c.Y; cz = c.Z;
	}
	else
	{
		NXOpen::Edge *edge = dynamic_cast<NXOpen::Edge*>(arc);
		if (edge)
		{
			UF_CURVE_arc_t arcData;
			if (UF_CURVE_ask_arc_data(edge->Tag(), &arcData) == 0)
			{
				cx = arcData.arc_center[0];
				cy = arcData.arc_center[1];
				cz = arcData.arc_center[2];
			}
		}
	}
	stringstream ss;
	ss << "  [" << tag << "] center = (" << cx << ", " << cy << ", " << cz << ")";
	print(ss.str());
}


//------------------------------------------------------------------------------
// Entry point(s) for unmanaged internal NXOpen C/C++ programs
//------------------------------------------------------------------------------
//  Explicit Execution
extern "C" DllExport void ufusr( char *parm, int *returnCode, int rlen )
{
    try
    {
		// Create NXOpen C++ class instance
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


