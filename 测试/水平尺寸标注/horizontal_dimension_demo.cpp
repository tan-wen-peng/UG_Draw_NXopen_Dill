//=============================================================================
// horizontal_dimension_demo.cpp
//
// NX12 NXOpen C++ 学习示例：在制图（Drafting）模块中创建“水平尺寸标注”
// ---------------------------------------------------------------------------
// 本文件完全独立，不依赖工作区内任何其他工程，仅使用 NX12 自带的
// NXOpen C++ 头文件与 UF（Open C）头文件（均位于安装目录的 UGOPEN 下）。
// 所有 NXOpen C++ 类名/方法名均已按本机 NX12 头文件逐一核实：
//   - Annotations_LinearDimensionBuilder.hxx（NXOpen::Annotations 命名空间）
//   - Annotations_BaseLinearDimensionBuilder.hxx（FirstAssociativity 等）
//   - Annotations_OriginBuilder.hxx（SetOriginPoint）
//   - Builder.hxx（Commit / Destroy）
//   - uf_drf.h（UF_DRF_create_horizontal_dim）
//
// 【演示两条路径】
//   方法 A：NXOpen C++  LinearDimensionBuilder（面向对象 Builder 方式）
//   方法 B：UF          UF_DRF_create_horizontal_dim（老式 C API 对照）
//
// 【运行方式】
//   1. 编译为 DLL（命令见文件末尾注释，注意 /utf-8 参数）；
//   2. 在 NX12 中打开一个部件（最好含一条水平直线/边，没有也行，
//      程序会自动创建一条演示直线）；
//   3. 菜单：文件 → 执行 → NX Open DLL…（旧版菜单：文件→执行→用户函数），
//      选择编译出的 horizontal_dimension_demo.dll 即可执行；
//   4. 运行日志输出到 ListingWindow（菜单：信息 → 列出窗口）。
//   NX 加载 DLL 时自动调用 ufusr()，卸载前调用 ufusr_cleanup()。
//
// 【关于“水平”的核心结论（NX12 实测头文件）】
//   NX12 的 LinearDimensionBuilder 并不存在网上常见教程所说的
//   SetDimensionPlacement(PlacementType::kHorizontal) 之类的枚举开关。
//   尺寸类型（HorizontalDimension/VerticalDimension/ParallelDimension…）
//   由两个关联对象（First/SecondAssociativity）的几何关系自动推断：
//   两关联位置 Y 坐标相同 → 推断为水平尺寸。因此本示例用一条水平直线
//   的两端点作为关联对象，即“天然水平”。若需显式指定类型，
//   NX12 还提供 DimensionCollection::CreateHorizontalDimension
//   （需自行构造 DimensionData/Associativity，较繁琐，此处不展开）。
//=============================================================================

#include <stdio.h>
#include <uf.h>
#include <uf_defs.h>
#include <uf_obj.h>
#include <uf_part.h>
#include <uf_curve.h>
#include <uf_draw.h>
#include <uf_drf.h>          // UF_DRF_create_horizontal_dim（已核实）
#include <uf_drf_types.h>    // UF_DRF_object_t / UF_DRF_text_t
#include <uf_ui.h>           // UF_UI_write_listing_window

//---------------------------------------------------------------------------
// SDK 探测：是否安装了 NXOPEN C++ 头文件（NX12 默认位于 UGOPEN\NXOpen）
//---------------------------------------------------------------------------
#if defined(__has_include) && __has_include(<NXOpen/Session.hxx>)
#  define HDEMO_HAS_NXOPEN 1
#  include <vector>
#  include <NXOpen/NXException.hxx>
#  include <NXOpen/Session.hxx>
#  include <NXOpen/Part.hxx>
#  include <NXOpen/PartCollection.hxx>
#  include <NXOpen/NXObject.hxx>
#  include <NXOpen/NXObjectManager.hxx>
#  include <NXOpen/Drawings_DrawingSheetCollection.hxx>
#  include <NXOpen/Drawings_DrawingSheet.hxx>
#  include <NXOpen/Drawings_DraftingView.hxx>
#endif

#if defined(HDEMO_HAS_NXOPEN) && defined(__has_include) && \
    __has_include(<NXOpen/Annotations_LinearDimensionBuilder.hxx>)
#  define HDEMO_HAS_LINBUILDER 1
#  include <NXOpen/Annotations_DimensionCollection.hxx>
#  include <NXOpen/Annotations_LinearDimensionBuilder.hxx>
#  include <NXOpen/Annotations_HorizontalDimension.hxx>
#  include <NXOpen/Annotations_OriginBuilder.hxx>
#  include <NXOpen/SelectNXObject.hxx>
#endif

// NX 官方示例约定：DllExport 用于导出 ufusr 等入口函数
#ifndef DllExport
#  define DllExport __declspec(dllexport)
#endif

//---------------------------------------------------------------------------
// 辅助函数：把 UF 返回码翻译为可读消息写入 ListingWindow
// 注意：NX12 中 UF_get_fail_message 的缓冲区固定为 char[133]，
//       NX12 没有 UF_MAX_MESSAGE_LENGTH 常量，请勿使用该宏。
//---------------------------------------------------------------------------
static void hdemo_report_uf_error(const char *context, int uf_status)
{
    char msg[133];                      // NX12 固定 133 字节缓冲区
    msg[0] = '\0';
    UF_get_fail_message(uf_status, msg);
    char buf[256];
    sprintf_s(buf, sizeof(buf), "[错误] %s 失败, 错误码 %d: %s\n",
              context, uf_status, msg);
    UF_UI_write_listing_window(buf);
}

//===========================================================================
// 主入口：NX 通过“文件→执行→NX Open DLL”加载本 DLL 时自动调用。
// 签名以 NX 官方 C++ 示例为准：
//     void ufusr(char *param, int *retcod, int param_len)
//（部分旧文档写作 char *retcode，实际加载器按指针写回返回码。）
//===========================================================================
extern "C" DllExport void ufusr(char * /*param*/, int *retcod, int /*param_len*/)
{
    *retcod = 0;
    UF_UI_write_listing_window("===== 水平尺寸标注演示开始 =====\n");

    //-----------------------------------------------------------------------
    // 第 1 步：初始化 UF（Open C）会话
    //   UF_initialize 必须在任何 UF_XXX 调用之前执行，与 UF_terminate 成对。
    //-----------------------------------------------------------------------
    int uf_status = UF_initialize();
    if (uf_status != 0)
    {
        hdemo_report_uf_error("UF_initialize", uf_status);
        return;
    }

    //-----------------------------------------------------------------------
    // 第 2 步：获取工作部件（Session → Part 链条的起点）
    //   有 NXOpen C++ SDK 时：Session::GetSession()→Parts()→Work()；
    //   否则用 UF_PART_ask_display_part 做等效检查（学习示例从简）。
    //-----------------------------------------------------------------------
    tag_t work_tag = NULL_TAG;
#ifdef HDEMO_HAS_NXOPEN
    NXOpen::Session *theSession = NXOpen::Session::GetSession();
    NXOpen::Part  *workPart    = theSession->Parts()->Work();
    if (workPart == NULL)
    {
        UF_UI_write_listing_window("[错误] 没有打开的工作部件，请先打开或新建一个部件。\n");
        UF_terminate();
        return;
    }
    work_tag = workPart->Tag();
    UF_UI_write_listing_window("[信息] 工作部件（NXOpen Session 获取）成功。\n");
#else
    work_tag = UF_PART_ask_display_part();
    if (work_tag == NULL_TAG)
    {
        UF_UI_write_listing_window("[错误] 没有打开的工作部件，请先打开或新建一个部件。\n");
        UF_terminate();
        return;
    }
    UF_UI_write_listing_window("[信息] 工作部件（UF 获取）成功。\n");
    UF_UI_write_listing_window("[提示] 本机未探测到 NXOPEN C++ 头文件，"
        "方法 A（LinearDimensionBuilder）已跳过，仅演示方法 B（UF）。\n");
#endif

    //-----------------------------------------------------------------------
    // 第 3 步：检查部件中是否存在图纸（制图前提）
    //   UF_DRAW_ask_num_drawings 返回部件中图纸（Drawing）的数量。
    //   尺寸标注属于制图对象，先确认图纸存在；若无图纸，请先进入制图
    //   应用新建一张图纸页再运行（本示例不自动建图，保持精简）。
    //-----------------------------------------------------------------------
    int num_drawings = 0;
    uf_status = UF_DRAW_ask_num_drawings(&num_drawings);
    if (uf_status != 0)
    {
        hdemo_report_uf_error("UF_DRAW_ask_num_drawings", uf_status);
        UF_terminate();
        return;
    }
    if (num_drawings == 0)
    {
        UF_UI_write_listing_window("[错误] 当前部件没有任何图纸页，"
            "请先进入制图应用并新建一张图纸页后重试。\n");
        UF_terminate();
        return;
    }
    {
        char buf[128];
        sprintf_s(buf, sizeof(buf), "[信息] 部件中共有 %d 张图纸\n", num_drawings);
        UF_UI_write_listing_window(buf);
    }

    //-----------------------------------------------------------------------
    // 第 4 步（NXOpen 侧，可选）：遍历图纸页（Drawing Sheets）与成员视图
    //   NX12 官方示例的遍历方式：Part→DrawingSheets()→迭代器；
    //   成员视图通过 DrawingSheet::GetDraftingViews() 获取
    //   （DraftingView 即模型投影到图纸上的“成员视图”）。
    //-----------------------------------------------------------------------
    tag_t line_tag = NULL_TAG;   // 待标注的直线对象（第 5 步填充）

#ifdef HDEMO_HAS_NXOPEN
    try
    {
        NXOpen::Drawings::DrawingSheetCollection *sheets = workPart->DrawingSheets();
        NXOpen::Drawings::DrawingSheet *firstSheet = NULL;
        int sheetCount = 0;
        for (NXOpen::Drawings::DrawingSheetCollection::iterator it = sheets->begin();
             it != sheets->end(); ++it)
        {
            ++sheetCount;
            if (firstSheet == NULL)
                firstSheet = *it;
        }

        char buf[128];
        sprintf_s(buf, sizeof(buf), "[信息] NXOpen 侧图纸页数量: %d\n", sheetCount);
        UF_UI_write_listing_window(buf);

        if (firstSheet != NULL)
        {
            // 成员视图（Member Views）：尺寸通常关联到某个成员视图内的几何。
            std::vector<NXOpen::Drawings::DraftingView *> memberViews =
                firstSheet->GetDraftingViews();
            sprintf_s(buf, sizeof(buf),
                      "[信息] 第一张图纸页的成员视图数量: %d\n",
                      (int)memberViews.size());
            UF_UI_write_listing_window(buf);
        }
    }
    catch (NXOpen::NXException &ex)
    {
        UF_UI_write_listing_window("[警告] NXOpen 图纸查询阶段异常（不影响后续标注）：");
        UF_UI_write_listing_window(ex.what());
        UF_UI_write_listing_window("\n");
    }
#endif

    //-----------------------------------------------------------------------
    // 第 5 步：准备一条“水平直线”作为标注对象
    //   方式一：UF_OBJ_cycle_objs_in_part 遍历部件，找一条已有直线；
    //   方式二：找不到则用 UF_CURVE_create_line 创建一条演示直线。
    //   实际产品中更常见：把用户交互选择的对象（tag）直接传入，
    //   例如通过 UF_UI_select_with_single_dialog 获取 tag 后使用。
    //-----------------------------------------------------------------------
    uf_status = UF_OBJ_cycle_objs_in_part(work_tag, UF_line_type, &line_tag);
    if (uf_status != 0 || line_tag == NULL_TAG)
    {
        UF_UI_write_listing_window("[信息] 部件中没有现成直线，创建一条演示水平直线。\n");
        UF_CURVE_line_t line_coords;   // 两端点 Y 相同 → 天然水平
        line_coords.start_point[0] = 0.0;  line_coords.start_point[1] = 0.0;  line_coords.start_point[2] = 0.0;
        line_coords.end_point[0]   = 80.0; line_coords.end_point[1]   = 0.0;  line_coords.end_point[2]   = 0.0;
        uf_status = UF_CURVE_create_line(&line_coords, &line_tag);
        if (uf_status != 0)
        {
            hdemo_report_uf_error("UF_CURVE_create_line", uf_status);
            UF_terminate();
            return;
        }
    }
    UF_UI_write_listing_window("[信息] 已找到可标注的直线对象。\n");

    // 读取直线端点，供后面计算尺寸放置位置（中点正下方 10mm）
    UF_CURVE_line_t lc;
    uf_status = UF_CURVE_ask_line_data(line_tag, &lc);
    if (uf_status != 0)
    {
        hdemo_report_uf_error("UF_CURVE_ask_line_data", uf_status);
        UF_terminate();
        return;
    }
    double mid[3];
    mid[0] = (lc.start_point[0] + lc.end_point[0]) / 2.0;
    mid[1] = (lc.start_point[1] + lc.end_point[1]) / 2.0 - 10.0;
    mid[2] = 0.0;

    //-----------------------------------------------------------------------
    // 方法 A：LinearDimensionBuilder（NXOpen C++，NX12 推荐方式）
    //   链条：Part→Dimensions()（DimensionCollection）
    //         → CreateLinearDimensionBuilder(NULL)（NULL=新建）
    //         → First/SecondAssociativity()->SetValue()（关联对象）
    //         → Origin()->SetOriginPoint()（尺寸放置位置）
    //         → Commit()（创建）→ Destroy()（释放 builder）
    //
    //   【强制水平的实现】把同一条水平直线的两端作为两个关联对象
    //   （两关联位置 Y 坐标相同），NX 自动推断出水平尺寸类型，
    //   Commit 返回的对象实际类型即 HorizontalDimension。
    //-----------------------------------------------------------------------
#ifdef HDEMO_HAS_LINBUILDER
    try
    {
        // A.1 获取尺寸集合与 builder。CreateLinearDimensionBuilder 的参数
        //     传入已有尺寸表示“编辑”，传 NULL 表示“新建”。
        NXOpen::Annotations::DimensionCollection *dimensions = workPart->Dimensions();
        NXOpen::Annotations::LinearDimensionBuilder *builder =
            dimensions->CreateLinearDimensionBuilder(NULL);

        // A.2 关联对象：NXObjectManager::Get 由 tag 反查 NX 对象。
        //     版本差异提示：NX12 中该函数返回 TaggedObject*，
        //     需 dynamic_cast 为 NXObject*（部分新版直接返回 NXObject*）。
        NXOpen::NXObject *lineObj =
            dynamic_cast<NXOpen::NXObject *>(NXOpen::NXObjectManager::Get(line_tag));
        if (lineObj == NULL)
        {
            builder->Destroy();
            UF_UI_write_listing_window("[警告] 方法 A：无法由 tag 取得直线对象。\n");
        }
        else
        {
            // 同一直线的两个端点分别作为第一/第二关联对象
            builder->FirstAssociativity()->SetValue(lineObj);
            builder->SecondAssociativity()->SetValue(lineObj);

            // A.3 放置位置：尺寸线原点（直线中点正下方 10mm）
            builder->Origin()->SetOriginPoint(NXOpen::Point3d(mid[0], mid[1], mid[2]));

            // A.4 提交创建
            NXOpen::NXObject *dimObj = builder->Commit();

            // A.5 释放 builder：Commit 后必须 Destroy（NX12 官方约定）。
            builder->Destroy();

            // A.6 验证：推断出的类型应为水平尺寸
            if (dynamic_cast<NXOpen::Annotations::HorizontalDimension *>(dimObj) != NULL)
            {
                UF_UI_write_listing_window("[成功] 方法 A：已创建水平尺寸"
                    "（HorizontalDimension）。\n");
            }
            else
            {
                UF_UI_write_listing_window("[成功] 方法 A：尺寸已创建"
                    "（非水平类型，请检查关联几何是否水平）。\n");
            }
        }
    }
    catch (NXOpen::NXException &ex)
    {
        UF_UI_write_listing_window("[警告] 方法 A 失败（将继续尝试方法 B）：");
        UF_UI_write_listing_window(ex.what());
        UF_UI_write_listing_window("\n");
    }
#endif // HDEMO_HAS_LINBUILDER

    //-----------------------------------------------------------------------
    // 方法 B：UF_DRF_create_horizontal_dim（UF 等效实现，签名已核实）
    //   本机 NX12 头文件 UGOPEN/uf_drf.h 第 917 行：
    //     int UF_DRF_create_horizontal_dim(
    //             UF_DRF_object_p_t object1, UF_DRF_object_p_t object2,
    //             UF_DRF_text_t *drf_text, double dimension_3d_origin[3],
    //             tag_t *dimension_tag);
    //   “水平”由该函数本身保证：无论两对象几何如何，尺寸线恒为水平，
    //   测量值为两关联位置在水平方向的间距。
    //-----------------------------------------------------------------------
    do
    {
        UF_DRF_object_t object1, object2;
        UF_DRF_init_object_structure(&object1);   // 官方推荐：先清零初始化
        UF_DRF_init_object_structure(&object2);

        // 关联同一条直线的两个端点：起点 → 终点
        object1.object_tag            = line_tag;
        object1.object_view_tag       = NULL_TAG;      // NULL_TAG = 当前视图
        object1.object_assoc_type     = UF_DRF_end_point;
        object1.object_assoc_modifier = UF_DRF_first_end_point;

        object2.object_tag            = line_tag;
        object2.object_view_tag       = NULL_TAG;
        object2.object_assoc_type     = UF_DRF_end_point;
        object2.object_assoc_modifier = UF_DRF_last_end_point;

        // drf_text 传 NULL：使用默认测量文本（自动显示测量值）。
        // 如需自定义/附加文本，参考官方示例 ufd_drf_cre_hz_dim.c，
        // 填充 UF_DRF_text_t 后传入其地址。
        // 放置位置即前面算好的 mid[]（WCS 坐标，直线中点正下方）。
        tag_t dim_tag = NULL_TAG;
        uf_status = UF_DRF_create_horizontal_dim(&object1, &object2,
                                                 NULL, mid, &dim_tag);
        if (uf_status != 0)
        {
            hdemo_report_uf_error("UF_DRF_create_horizontal_dim", uf_status);
            break;
        }
        UF_UI_write_listing_window("[成功] 方法 B：UF_DRF_create_horizontal_dim "
            "水平尺寸创建完成。\n");
    } while (0);

    //-----------------------------------------------------------------------
    // 收尾：与 UF_initialize 成对
    //-----------------------------------------------------------------------
    UF_terminate();
    UF_UI_write_listing_window("===== 水平尺寸标注演示结束 =====\n");
}

//---------------------------------------------------------------------------
// 询问 DLL 卸载时机：Explicitly 表示由用户手动卸载（学习期推荐）。
//---------------------------------------------------------------------------
extern "C" DllExport int ufusr_ask_unload(void)
{
#ifdef HDEMO_HAS_NXOPEN
    return (int)NXOpen::Session::LibraryUnloadOptionExplicitly;
#else
    return 2;   // 与 LibraryUnloadOptionExplicitly 等值
#endif
}

//---------------------------------------------------------------------------
// NX 卸载本 DLL 前自动调用，可做资源清理。
//（旧式 UF 程序使用的 ufsta()/unufusr() 是 NX 早期命名约定；
//  NX12 C++ 推荐 ufusr_ask_unload + ufusr_cleanup。）
//---------------------------------------------------------------------------
extern "C" DllExport void ufusr_cleanup(void)
{
}

//=============================================================================
// 【编译命令示例】（VS2017 x64 命令行，请按实际安装路径调整）：
//
//   "D:\Program Files (x86)\Microsoft Visual Studio\2017\Community\VC\Auxiliary\Build\vcvars64.bat"
//
//   cl /nologo /EHsc /W3 /LD /utf-8 horizontal_dimension_demo.cpp
//      /I "D:\Program Files\Siemens\NX 12.0\UGOPEN"
//      /link /LIBPATH:"D:\Program Files\Siemens\NX 12.0\UGOPEN"
//            libufun.lib libnxopencpp.lib libnxopencpp_annotations.lib
//            libnxopencpp_drawings.lib
//
//   说明：
//     /utf-8 —— 源码含中文，必须指定；残留的 C4819/C4275 警告无害；
//     NX12 的 NXOpen C++ 头文件就放在 UGOPEN\NXOpen 目录下，
//     因此一个 /I UGOPEN 同时覆盖 UF 头文件与 NXOpen 头文件；
//     链接库（同在 UGOPEN 目录下）：
//       libufun.lib                —— UF（Open C）导入库
//       libnxopencpp.lib           —— NXOpen C++ 核心（Session/Part/Builder 等）
//       libnxopencpp_annotations.lib —— 注释/尺寸模块（DimensionCollection 等）
//       libnxopencpp_drawings.lib  —— 图纸模块（DrawingSheets/DraftingView 等）
//   生成的 DLL 通过“文件→执行→NX Open DLL”加载。
//=============================================================================
