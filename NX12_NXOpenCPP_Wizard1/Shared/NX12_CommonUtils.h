#pragma once
//------------------------------------------------------------------------------
// NX12 壳体出图插件 —— 共享工具函数声明
// 从 MyClass 成员函数重构为自由函数，供所有 DLL 模块调用
//------------------------------------------------------------------------------

#include "NX12_CommonConfig.h"
#include <vector>

namespace CommonUtils {

    //--------------------------------------------------------------------------
    // 曲线信息结构体（视图内曲线枚举结果）
    //--------------------------------------------------------------------------
    struct CurveInfo {
        tag_t tag;              // 曲线 tag
        int   type;             // UF line_type / circle_type / conic_type / spline_type
        int   subtype;          // UF solid_edge_subtype 等
        double start_pt[3];     // parm=0.0 处坐标
        double end_pt[3];       // parm=1.0 处坐标
        double start_tg[3];     // parm=0.0 处切矢
        double end_tg[3];       // parm=1.0 处切矢
        bool  is_closed;        // 是否封闭（360° 圆弧）
        bool  has_props;        // UF_MODL_ask_curve_props 至少一端成功（端点有效）
        double arc_center[3];   // 圆弧中心（仅 circle_type 有效）
        double arc_radius;      // 圆弧半径（仅 circle_type 有效）
    };

    // 初始化并返回 NX 核心单例
    NXOpen::Session* get_session();
    NXOpen::UI* get_ui();

    // 日志输出（3 个重载），同时写 ListingWindow 和 LogFile
    void print_msg(const NXOpen::NXString& msg);
    void print_msg(const std::string& msg);
    void print_msg(const char* msg);

    // 静默更新（从 phase_center_view 内的 lambda 提取）
    void silent_update(NXOpen::Session* session);

    // 视图两步法精确定位（从 MyClass::phase_center_view 提取，L496-625）
    // 完整保留：silent_update → view->Update → UF_DRAW_ask_view_borders → MoveView → 复核收敛
    void center_view(NXOpen::Part* part, NXOpen::Drawings::DraftingView* view,
                     const SheetDesc& sd, const NXOpen::Point3d* cTargetOverride = NULL);

    // 回读视图边界中心（silent_update → view->Update → UF_DRAW_ask_view_borders）；
    // 用于 center_view 后读回最终中心（如剖视图 X 对齐基准视图最终 X）。
    // 成功返回 true 并写入 cx/cy，失败返回 false
    bool ask_view_border_center(NXOpen::Drawings::DraftingView* view, double& cx, double& cy);

    // 多视图整体居中：求各视图边界的并集包围盒中心，所有视图同移一增量
    // （保持相对对齐不变），使并集中心落在 targetCenter；两步法 + 复核收敛（最多两轮）
    void center_views_combined(NXOpen::Part* part,
                               const std::vector<NXOpen::Drawings::DraftingView*>& views,
                               const SheetDesc& sd,
                               const NXOpen::Point3d& targetCenter);

    // 运行时实测图纸真实尺寸（NXOpen DrawingSheet::Length/Height 为主，
    // UF_DRAW_ask_drawing_info 交叉核对，并回显图纸视图绝对原点），
    // 不再依赖配置假定的 297x210；成功返回 true 并写 len(长)/hgt(高)
    bool ask_sheet_actual_size(NXOpen::Drawings::DraftingDrawingSheet* sheet,
                               double& len, double& hgt);

    // 实测模板图框实际中心（图纸坐标）：按图层遍历图纸上的模板曲线，
    // 对"图框级大框曲线"（包围盒两方向均 >= 0.6 倍图幅）求并集包围盒中心；
    // 同时回显图纸视图边界与绝对原点，用于诊断原点偏移/图框留边。
    // 成功返回 true 并写 frameCenter 与 frameLayers（含图框曲线的图层号集合）；
    // 未找到图框曲线时以图纸几何中心兜底并返回 false
    bool ask_sheet_frame_center(NXOpen::Part* part,
                                NXOpen::Drawings::DraftingDrawingSheet* sheet,
                                NXOpen::Point3d& frameCenter,
                                std::vector<int>& frameLayers);

    // 运行时查找已有剖视图（Step3/4/5 共用）
    NXOpen::Drawings::SectionView* find_section_view(NXOpen::Part* part);

    // 运行时查找已有中心线（Step3 用）
    NXOpen::Annotations::Centerline2d* find_centerline(NXOpen::Part* part);

    // 枚举视图内所有曲线/边，返回 CurveInfo 列表（含 0.01mm 容差去重）
    std::vector<CurveInfo> enumerate_view_curves(NXOpen::View* view);

    // 将曲线列表按类型分类为直线段和圆弧
    void classify_edges(const std::vector<CurveInfo>& curves,
                        std::vector<CurveInfo>& lines,
                        std::vector<CurveInfo>& arcs);

    // 将模型/绝对坐标点映射到图纸坐标（封装 UF_VIEW_map_model_to_drawing，
    // 自动包含视图比例/旋转/朝向/视图原点变换）；memberView 必须是图纸成员视图。
    // 成功返回 true 并写 drawPt[2]（图纸 2D 坐标，与 UF_DRAW_ask_view_borders 同一坐标系）
    bool map_model_to_drawing(tag_t memberView, const double modelPt[3], double drawPt[2]);

    // 计算线性标注放置点（全部输入均为图纸坐标）
    // s1/e1、s2/e2: 两条边映射后的起止点；方向由图纸坐标判断水平/垂直；
    // borders: [Xmin, Ymin, Xmax, Ymax]（图纸坐标），offset: 标注距视图边界的偏移
    NXOpen::Point3d calc_linear_placement(const double s1[2], const double e1[2],
                                          const double s2[2], const double e2[2],
                                          const double borders[4], double offset);

    // 计算径向/直径标注放置点（全部输入均为图纸坐标）
    // centerDraw: 圆弧映射后圆心；midDraw: 圆弧中点映射（parm=0.5）；
    // drawRadius: 图纸坐标下半径（模型半径 × 视图比例）；
    // 放置点 = 圆心 + 径方向 × (半径 + offset)，并钳制在视图边界附近
    NXOpen::Point3d calc_radial_placement(const double centerDraw[2], const double midDraw[2],
                                          double drawRadius,
                                          const double borders[4], double offset);

} // namespace CommonUtils
