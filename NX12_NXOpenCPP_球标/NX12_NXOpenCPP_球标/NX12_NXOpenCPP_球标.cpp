// NX12_NXOpenCPP_球标
// step1 : 球标标注 - 手动逐点选择，顺序编号
//
// 功能：
//   循环让用户在图形窗口直接点选部件上的位置（无选择对话框），
//   每点选一个位置就创建一个圆形球标（ID 符号），点选位置即
//   指引线终止位置；编号从 START_NUMBER 开始依次 +1，
//   直到按取消结束。适合给装配图、打标图上的特征逐个编号。
//
// 使用：
//   1. NX12 打开图纸，进入制图环境
//   2. 文件 -> 执行 -> NX Open（Ctrl+U），选择编译出的 balloon_step1.dll
//   3. 按提示在部件上逐个点选引线位置，取消结束
//   4. 不满意 Ctrl+Z 一次撤销全部球标
//
// 说明：
//   - 指引线终点固定在用户点选处（屏幕位置附着），球标中心按
//     OFFSET_X / OFFSET_Y 偏移（图纸坐标）
//   - 点在视图内部时，选点坐标会自动从模型坐标映射到图纸坐标
//   - 球标规格在创建后统一设置：类型=圆、大小=BALLOON_SIZE、
//     文字字体=TEXT_FONT、文字大小=TEXT_HEIGHT（不再依赖制图首选项）

// Mandatory UF Includes
#include <uf.h>
#include <uf_draw.h>
#include <uf_drf.h>
#include <uf_drf_types.h>
#include <uf_ui.h>
#include <uf_ui_types.h>
#include <uf_view.h>

// Internal Includes
#include <NXOpen/Annotations.hxx>
#include <NXOpen/Annotations_AnnotationManager.hxx>
#include <NXOpen/Annotations_IdSymbol.hxx>
#include <NXOpen/Annotations_IdSymbolBuilder.hxx>
#include <NXOpen/Annotations_IdSymbolCollection.hxx>
#include <NXOpen/Annotations_LetteringStyleBuilder.hxx>
#include <NXOpen/Annotations_LineArrowStyleBuilder.hxx>
#include <NXOpen/Annotations_StyleBuilder.hxx>
#include <NXOpen/BasePart.hxx>
#include <NXOpen/FontCollection.hxx>
#include <NXOpen/ListingWindow.hxx>
#include <NXOpen/NXException.hxx>
#include <NXOpen/NXMessageBox.hxx>
#include <NXOpen/NXObjectManager.hxx>
#include <NXOpen/Part.hxx>
#include <NXOpen/PartCollection.hxx>
#include <NXOpen/Session.hxx>
#include <NXOpen/TaggedObject.hxx>
#include <NXOpen/UI.hxx>

// Std C++ Includes
#include <cstdio>
#include <stdexcept>
#include <string>

using namespace NXOpen;
using std::string;
using std::exception;

//------------------------------------------------------------------------------
// 配置区（按需修改）
//------------------------------------------------------------------------------
const int    START_NUMBER = 1;     // 起始编号（之后依次 +1）
const bool   WITH_LEADER  = true;  // true=球标带指引线; false=只有球标
const double BALLOON_SIZE = 40.0;  // 球标圆的直径
const double TEXT_HEIGHT  = 20.0;  // 球标内数字的字高
const double ARROW_LENGTH = 8.0;   // 引线箭头长度
const char   TEXT_FONT[]  = "FANGSONG";  // 文字字体（NX 字体或系统字体名，找不到时自动尝试仿宋候选名）
const double OFFSET_X     = 25.0;  // 球标中心相对选择点的 X 偏移（图纸坐标，正=右）
const double OFFSET_Y     = 25.0;  // 球标中心相对选择点的 Y 偏移（图纸坐标，正=上）

//------------------------------------------------------------------------------
// NXOpen c++ class
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
    void print(const char *);

    // step1: 球标标注 - 手动逐点选择，顺序编号
    void step1();

private:
    BasePart *workPart, *displayPart;
    NXMessageBox *mb;
    ListingWindow *lw;

    // step1 工具函数：选择对话框返回的光标位置 -> 图纸坐标
    void cursorToDrawing(double cursor[3], tag_t view, double target[3]);

    // step1 工具函数：设置球标的大小与文字样式（圆/40/仿宋/字高20）
    void styleBalloon(tag_t idTag);
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
    if (!lw->IsOpen()) lw->Open();
    lw->WriteLine(msg);
}
void MyClass::print(const string &msg)
{
    if (!lw->IsOpen()) lw->Open();
    lw->WriteLine(msg);
}
void MyClass::print(const char *msg)
{
    if (!lw->IsOpen()) lw->Open();
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
// step1: 球标标注 - 手动逐点选择，顺序编号
//------------------------------------------------------------------------------
void MyClass::step1()
{
    print("=== step1: 球标标注（手动逐点选择，顺序编号）===");

    // ---- 1. 环境检查：必须存在图纸 ----
    int numDrawings = 0;
    if (UF_DRAW_ask_num_drawings(&numDrawings) != 0 || numDrawings <= 0)
    {
        print("当前部件没有图纸，请先进入制图环境再运行。");
        return;
    }
    char info[128];
    sprintf_s(info, sizeof(info), "图纸数量：%d", numDrawings);
    print(info);

    // ---- 2. 一个撤销标记：Ctrl+Z 一次撤销全部球标 ----
    theSession->SetUndoMark(Session::MarkVisibilityVisible, "球标标注");

    // ---- 3. 循环点选 -> 创建球标 ----
    int num = START_NUMBER;
    int created = 0;

    while (true)
    {
        char cue[128];
        sprintf_s(cue, sizeof(cue),
                  "点选第 %d 个球标的引线终止位置（取消结束）", num);

        // 不弹选择对话框：直接在图形窗口点选部件上的位置
        int    response = 0;
        double cursor[3] = { 0.0, 0.0, 0.0 };
        tag_t  view = NULL_TAG;

        int err = UF_UI_specify_screen_position(
            cue, NULL, NULL, cursor, &view, &response);

        // 点中返回 UF_UI_PICK_RESPONSE(1)；取消/返回/出错结束循环
        if (err != 0 || response != UF_UI_PICK_RESPONSE)
            break;

        // 光标位置 -> 图纸坐标（点在成员视图内时是模型坐标，需要映射）
        double target[3] = { 0.0, 0.0, 0.0 };
        cursorToDrawing(cursor, view, target);

        char text[MAX_ID_SYM_TEXT_LENGTH + 1];
        sprintf_s(text, sizeof(text), "%d", num);

        double origin[3] = { target[0] + OFFSET_X,
                             target[1] + OFFSET_Y,
                             target[2] };

        UF_DRF_leader_mode_t leaderMode =
            WITH_LEADER ? UF_DRF_with_leader : UF_DRF_without_leader;

        tag_t idTag = NULL_TAG;
        err = UF_DRF_create_id_symbol(UF_DRF_sym_circle, text, "",
            origin, leaderMode, UF_DRF_leader_attach_screen,
            NULL, target, &idTag);
        if (err != 0)
        {
            char msg[256] = "";
            UF_get_fail_message(err, msg);
            print("球标创建失败，已停止：");
            print(msg);
            break;
        }

        // 创建后统一设置规格：圆、大小 40、字体 FANGSONG、字高 20
        styleBalloon(idTag);

        char done[128];
        sprintf_s(done, sizeof(done), "  球标 %d 已创建", num);
        print(done);

        ++num;
        ++created;
    }

    // ---- 4. 统计 ----
    char summary[256];
    if (created > 0)
        sprintf_s(summary, sizeof(summary),
                  "完成：共创建 %d 个球标（编号 %d ~ %d）。Ctrl+Z 可一次全部撤销。",
                  created, START_NUMBER, num - 1);
    else
        sprintf_s(summary, sizeof(summary), "未创建任何球标。");
    print(summary);
    print("=== step1 done ===");
}

//------------------------------------------------------------------------------
// step1 helper: 选择对话框返回的光标位置 -> 图纸坐标
//------------------------------------------------------------------------------
void MyClass::cursorToDrawing(double cursor[3], tag_t view, double target[3])
{
    target[0] = cursor[0];
    target[1] = cursor[1];
    target[2] = cursor[2];

    if (view == NULL_TAG)
        return;

    UF_VIEW_type_t type = UF_VIEW_MODEL_TYPE;
    UF_VIEW_subtype_t subtype = UF_VIEW_INVALID_SUBTYPE;

    // 光标在制图成员视图（图纸上的投影视图）内 -> 模型绝对坐标 -> 图纸坐标
    if (UF_VIEW_ask_type(view, &type, &subtype) == 0
        && type == UF_VIEW_DRAWING_MEMBER_TYPE)
    {
        double xy[2] = { 0.0, 0.0 };
        if (UF_VIEW_map_model_to_drawing(view, cursor, xy) == 0)
        {
            target[0] = xy[0];
            target[1] = xy[1];
            target[2] = 0.0;
        }
    }
}

//------------------------------------------------------------------------------
// step1 helper: 设置球标的大小、文字样式与箭头
//   圆直径 = BALLOON_SIZE，文字字体 = TEXT_FONT（仿宋），字高 = TEXT_HEIGHT，
//   文字角度 = 0（正立），箭头长度 = ARROW_LENGTH
//------------------------------------------------------------------------------
void MyClass::styleBalloon(tag_t idTag)
{
    using namespace NXOpen::Annotations;

    IdSymbol *idSymbol = dynamic_cast<IdSymbol *>(
        NXOpen::NXObjectManager::Get(idTag));
    if (idSymbol == NULL)
        return;

    // 字体名 -> 部件字体编号：先按 NX 字体，再依次按系统字体尝试仿宋候选名
    const char *candidates[] = { TEXT_FONT, "FangSong", "仿宋", "仿宋_GB2312" };
    int fontId = -1;
    try { fontId = workPart->Fonts()->AddFont(TEXT_FONT); }
    catch (...) {}
    for (int i = 0; fontId < 0 && i < (int)(sizeof(candidates) / sizeof(candidates[0])); ++i)
    {
        try { fontId = workPart->Fonts()->AddFont(candidates[i], FontCollection::TypeStandard); }
        catch (...) {}
    }
    if (fontId < 0)
        return;

    IdSymbolBuilder *builder = workPart->Annotations()->IdSymbols()
        ->CreateIdSymbolBuilder(idSymbol);
    if (builder == NULL)
        return;

    builder->SetBalloonType(BalloonTypesCircle);   // 类型：圆
    builder->SetSize(BALLOON_SIZE);                // 大小（圆的直径）

    // 文字样式：四个文字类别都设一遍（球标文字继承哪个类别因环境而异），
    // 统一为仿宋/字高 TEXT_HEIGHT/不斜体，文字角度归零保证正立
    LetteringStyleBuilder *lettering = builder->Style()->LetteringStyle();
    lettering->SetAngle(0.0);
    lettering->SetDimensionTextFont(fontId);
    lettering->SetDimensionTextSize(TEXT_HEIGHT);
    lettering->SetDimensionTextItalicized(false);
    lettering->SetAppendedTextFont(fontId);
    lettering->SetAppendedTextSize(TEXT_HEIGHT);
    lettering->SetAppendedTextItalicized(false);
    lettering->SetToleranceTextFont(fontId);
    lettering->SetToleranceTextSize(TEXT_HEIGHT);
    lettering->SetToleranceTextItalicized(false);
    lettering->SetGeneralTextFont(fontId);
    lettering->SetGeneralTextSize(TEXT_HEIGHT);
    lettering->SetGeneralTextItalicized(false);

    // 引线箭头：加大箭头长度
    LineArrowStyleBuilder *lineArrow = builder->Style()->LineArrowStyle();
    lineArrow->SetArrowheadLength(ARROW_LENGTH);

    builder->Commit();
    builder->Destroy();
}

//------------------------------------------------------------------------------
// Entry point(s) for unmanaged internal NXOpen C/C++ programs
//------------------------------------------------------------------------------
//  Explicit Execution
extern "C" DllExport void ufusr(char *parm, int *returnCode, int rlen)
{
    try
    {
        // Create NXOpen C++ class instance
        MyClass *theMyClass;
        theMyClass = new MyClass();
        theMyClass->do_it();
        delete theMyClass;
    }
    catch (const NXException &e1)
    {
        UI::GetUI()->NXMessageBox()->Show("NXException", NXOpen::NXMessageBox::DialogTypeError, e1.Message());
    }
    catch (const exception &e2)
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
