//------------------------------------------------------------------------------
// NX12 壳体出图插件 —— 共享工具函数实现
// 从 MyClass 成员函数重构为自由函数
//------------------------------------------------------------------------------

#include "NX12_CommonUtils.h"
#include <NXOpen/Update.hxx>
#include <NXOpen/Annotations_CenterlineCollection.hxx>
#include <NXOpen/Annotations_AnnotationManager.hxx>
// 以下 include 原在 NX12_CommonConfig.h，因头文件精简移至本编译单元（按需包含）
#include <uf_layer.h>                               // UF_LAYER_MIN_LAYER / UF_LAYER_cycle_by_layer
#include <uf_part_types.h>                          // UF_PART_METRIC
#include <NXOpen/Drawings_DraftingBody.hxx>
#include <NXOpen/Drawings_DraftingBodyCollection.hxx>
#include <NXOpen/Drawings_DraftingCurve.hxx>
#include <NXOpen/Drawings_DraftingCurveCollection.hxx>
#include <string.h>
#include <set>

namespace CommonUtils {

//==============================================================================
// 单例访问
//==============================================================================
NXOpen::Session* get_session()
{
    static NXOpen::Session* s = NXOpen::Session::GetSession();
    return s;
}

NXOpen::UI* get_ui()
{
    static NXOpen::UI* u = NXOpen::UI::GetUI();
    return u;
}

//==============================================================================
// 日志输出（lazy init ListingWindow / LogFile，同时写入两处）
//==============================================================================
void print_msg(const NXOpen::NXString& msg)
{
    static NXOpen::ListingWindow* lw = get_session()->ListingWindow();
    static NXOpen::LogFile* lf = get_session()->LogFile();
    if (!lw->IsOpen()) lw->Open();
    lw->WriteLine(msg);
    try { if (lf) lf->WriteLine(msg); } catch (...) {}
}

void print_msg(const std::string& msg)
{
    static NXOpen::ListingWindow* lw = get_session()->ListingWindow();
    static NXOpen::LogFile* lf = get_session()->LogFile();
    if (!lw->IsOpen()) lw->Open();
    lw->WriteLine(msg);
    try { if (lf) lf->WriteLine(msg.c_str()); } catch (...) {}
}

void print_msg(const char* msg)
{
    static NXOpen::ListingWindow* lw = get_session()->ListingWindow();
    static NXOpen::LogFile* lf = get_session()->LogFile();
    if (!lw->IsOpen()) lw->Open();
    lw->WriteLine(msg);
    try { if (lf) lf->WriteLine(msg); } catch (...) {}
}

//==============================================================================
// 静默更新（最小代价：invisible undo mark + DoUpdate）
//==============================================================================
void silent_update(NXOpen::Session* session)
{
    NXOpen::Session::UndoMarkId mark = session->SetUndoMark(
        NXOpen::Session::MarkVisibilityInvisible, "Silent Update");
    session->UpdateManager()->DoUpdate(mark);
}

//==============================================================================
// 视图两步法精确定位
// 完整复制源码 L496-625 逻辑：
//   silent_update → view->Update → UF_DRAW_ask_view_borders → MoveView
//   → 复核收敛（最多两轮，容差 0.01mm）→ 诊断日志 → 异常降级为警告
//==============================================================================
void center_view(NXOpen::Part* part, NXOpen::Drawings::DraftingView* view,
                 const SheetDesc& sd, const NXOpen::Point3d* cTargetOverride)
{
    if (!view) return;

    NXOpen::Session* session = get_session();

    char fmt[512];
    try
    {
        // 第一步：更新后读取视图边界（图纸坐标 [Xmin, Ymin, Xmax, Ymax]）
        silent_update(session);
        // 双保险：显式更新该视图（DraftingView::Update 文档明确包含
        // view bounds 更新），确保 ask_view_borders 读到的是最新边界
        view->Update();

        double b[4] = { 0.0, 0.0, 0.0, 0.0 };
        int rc = UF_DRAW_ask_view_borders(view->Tag(), b);
        if (rc != 0)
        {
            sprintf_s(fmt, sizeof(fmt),
                "  警告: UF_DRAW_ask_view_borders 返回 %d，跳过视图居中", rc);
            print_msg(fmt);
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
        print_msg(fmt);

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
        print_msg(fmt);
        view->MoveView(newRefPt);

        // 再静默更新一次，让移动后的视图边界生效
        silent_update(session);
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
                print_msg(fmt);
                return;
            }
            NXOpen::Point3d c1((b2[0] + b2[2]) / 2.0, (b2[1] + b2[3]) / 2.0, 0.0);
            double errX = cTarget.X - c1.X;
            double errY = cTarget.Y - c1.Y;
            sprintf_s(fmt, sizeof(fmt),
                "  居中诊断(第%d轮复核): 边界(%.3f, %.3f, %.3f, %.3f), "
                "实际中心(%.3f, %.3f), 偏差(%.4f, %.4f)",
                round + 1, b2[0], b2[1], b2[2], b2[3], c1.X, c1.Y, errX, errY);
            print_msg(fmt);

            if (fabs(errX) <= tol && fabs(errY) <= tol)
            {
                sprintf_s(fmt, sizeof(fmt),
                    "[图纸 %s] 视图已定位 (中心 -> (%.2f, %.2f))",
                    sd.name, cTarget.X, cTarget.Y);
                print_msg(fmt);
                return;
            }

            if (round == 1)
            {
                print_msg("  警告: 两轮补偿后仍未收敛（偏差 > 0.01mm），请回传以上诊断信息");
                return;
            }

            // 补偿：按残余偏差再移动一次
            NXOpen::Point3d rp = view->GetDrawingReferencePoint();
            NXOpen::Point3d nrp(rp.X + errX, rp.Y + errY, rp.Z);
            sprintf_s(fmt, sizeof(fmt),
                "  居中诊断: 补偿移动 旧锚点(%.3f, %.3f, %.3f) -> 新锚点(%.3f, %.3f, %.3f)",
                rp.X, rp.Y, rp.Z, nrp.X, nrp.Y, nrp.Z);
            print_msg(fmt);
            view->MoveView(nrp);
            silent_update(session);
            view->Update();
        }
    }
    catch (const NXOpen::NXException& e)
    {
        print_msg(std::string("  警告: 视图居中失败: ") + e.Message());
    }
    catch (...)
    {
        print_msg("  警告: 视图居中失败（未知异常）");
    }
}

//==============================================================================
// 回读视图边界中心（更新后读取，与 center_view 同一两步法读取方式）
//==============================================================================
bool ask_view_border_center(NXOpen::Drawings::DraftingView* view, double& cx, double& cy)
{
    if (!view) return false;
    try
    {
        silent_update(get_session());
        view->Update();
        double b[4] = { 0.0, 0.0, 0.0, 0.0 };
        if (UF_DRAW_ask_view_borders(view->Tag(), b) != 0) return false;
        cx = (b[0] + b[2]) / 2.0;
        cy = (b[1] + b[3]) / 2.0;
        return true;
    }
    catch (...) { return false; }
}

//==============================================================================
// 多视图整体居中（并集包围盒中心 -> targetCenter）
// 读各视图边界 -> 求并集包围盒中心 -> 所有视图同移一增量（MoveView 锚点+增量）
// -> 复核收敛（最多两轮，容差 0.01mm）。同移保证视图间相对对齐不变。
//==============================================================================
void center_views_combined(NXOpen::Part* part,
                           const std::vector<NXOpen::Drawings::DraftingView*>& views,
                           const SheetDesc& sd,
                           const NXOpen::Point3d& targetCenter)
{
    if (!part || views.empty()) return;

    NXOpen::Session* session = get_session();
    char fmt[512];
    try
    {
        // 第一轮前：更新所有视图
        silent_update(session);
        for (size_t i = 0; i < views.size(); ++i)
            if (views[i]) views[i]->Update();

        const double tol = 0.01;
        for (int round = 0; round <= 2; ++round)
        {
            // ---- 读取各视图边界，累计并集包围盒 ----
            bool haveUnion = false;
            double u[4] = { 0.0, 0.0, 0.0, 0.0 };
            for (size_t i = 0; i < views.size(); ++i)
            {
                if (!views[i]) continue;
                double b[4] = { 0.0, 0.0, 0.0, 0.0 };
                int rc = UF_DRAW_ask_view_borders(views[i]->Tag(), b);
                if (rc != 0)
                {
                    sprintf_s(fmt, sizeof(fmt),
                        "  警告: 整体居中 UF_DRAW_ask_view_borders 返回 %d（视图 %d），跳过整体居中",
                        rc, (int)i);
                    print_msg(fmt);
                    return;
                }
                sprintf_s(fmt, sizeof(fmt),
                    "  整体居中诊断: 视图%d 边界(%.3f, %.3f, %.3f, %.3f)",
                    (int)i, b[0], b[1], b[2], b[3]);
                print_msg(fmt);
                if (!haveUnion)
                {
                    u[0] = b[0]; u[1] = b[1]; u[2] = b[2]; u[3] = b[3];
                    haveUnion = true;
                }
                else
                {
                    if (b[0] < u[0]) u[0] = b[0];
                    if (b[1] < u[1]) u[1] = b[1];
                    if (b[2] > u[2]) u[2] = b[2];
                    if (b[3] > u[3]) u[3] = b[3];
                }
            }
            if (!haveUnion)
            {
                print_msg("  警告: 整体居中无有效视图边界，跳过");
                return;
            }

            const double ucx = (u[0] + u[2]) / 2.0;
            const double ucy = (u[1] + u[3]) / 2.0;
            const double errX = targetCenter.X - ucx;
            const double errY = targetCenter.Y - ucy;
            sprintf_s(fmt, sizeof(fmt),
                "  整体居中诊断(第%d轮): 并集边界(%.3f, %.3f, %.3f, %.3f), "
                "并集中心(%.3f, %.3f), 目标(%.3f, %.3f), 偏差(%.4f, %.4f)",
                round, u[0], u[1], u[2], u[3], ucx, ucy,
                targetCenter.X, targetCenter.Y, errX, errY);
            print_msg(fmt);

            if (fabs(errX) <= tol && fabs(errY) <= tol)
            {
                sprintf_s(fmt, sizeof(fmt),
                    "[图纸 %s] 视图组整体居中完成 (并集中心 -> (%.2f, %.2f))",
                    sd.name, targetCenter.X, targetCenter.Y);
                print_msg(fmt);
                return;
            }

            if (round == 2)
            {
                print_msg("  警告: 整体居中两轮补偿后仍未收敛（偏差 > 0.01mm），请回传以上诊断信息");
                return;
            }

            // ---- 所有视图同移一增量（锚点 + 增量，保持相对对齐） ----
            for (size_t i = 0; i < views.size(); ++i)
            {
                if (!views[i]) continue;
                NXOpen::Point3d rp = views[i]->GetDrawingReferencePoint();
                NXOpen::Point3d nrp(rp.X + errX, rp.Y + errY, rp.Z);
                sprintf_s(fmt, sizeof(fmt),
                    "  整体居中: 视图%d 锚点(%.3f, %.3f) -> (%.3f, %.3f)",
                    (int)i, rp.X, rp.Y, nrp.X, nrp.Y);
                print_msg(fmt);
                views[i]->MoveView(nrp);
            }
            silent_update(session);
            for (size_t i = 0; i < views.size(); ++i)
                if (views[i]) views[i]->Update();
        }
    }
    catch (const NXOpen::NXException& e)
    {
        print_msg(std::string("  警告: 视图组整体居中失败: ") + e.Message());
    }
    catch (...)
    {
        print_msg("  警告: 视图组整体居中失败（未知异常）");
    }
}

//==============================================================================
// 运行时实测图纸真实尺寸（不再依赖配置假定）
// 主值：NXOpen DrawingSheet::Length()/Height()（模板实例化后的真实图纸）；
// 交叉核对：UF_DRAW_ask_drawing_info；同时回显图纸视图绝对原点供诊断。
//==============================================================================
bool ask_sheet_actual_size(NXOpen::Drawings::DraftingDrawingSheet* sheet,
                           double& len, double& hgt)
{
    if (!sheet) return false;
    char fmt[512];
    try
    {
        len = sheet->Length();
        hgt = sheet->Height();
        sprintf_s(fmt, sizeof(fmt),
            "  图纸实测(NXOpen): 长=%.3f, 高=%.3f", len, hgt);
        print_msg(fmt);

        UF_DRAW_info_t info;
        memset(&info, 0, sizeof(info));
        int rc = UF_DRAW_ask_drawing_info(sheet->Tag(), &info);
        if (rc == 0)
        {
            double ufH = 0.0, ufL = 0.0;
            const char* sizeKind = "未知";
            if (info.size_state == UF_DRAW_METRIC_SIZE)
            {
                sizeKind = "标准公制";
                switch (info.size.metric_size_code)
                {
                case UF_DRAW_A0: ufH = 841.0;  ufL = 1189.0; break;
                case UF_DRAW_A1: ufH = 594.0;  ufL = 841.0;  break;
                case UF_DRAW_A2: ufH = 420.0;  ufL = 594.0;  break;
                case UF_DRAW_A3: ufH = 297.0;  ufL = 420.0;  break;
                case UF_DRAW_A4: ufH = 210.0;  ufL = 297.0;  break;
                default: break;
                }
            }
            else if (info.size_state == UF_DRAW_CUSTOM_SIZE)
            {
                sizeKind = "自定义";
                ufH = info.size.custom_size[0];   /* [0] = height */
                ufL = info.size.custom_size[1];   /* [1] = length */
            }
            sprintf_s(fmt, sizeof(fmt),
                "  图纸实测(UF_DRAW rc=%d): 尺寸类型=%s, 高=%.3f, 长=%.3f, 比例=%.3f, 单位=%s",
                rc, sizeKind, ufH, ufL, info.drawing_scale,
                (info.units == UF_PART_METRIC) ? "毫米" : "英寸");
            print_msg(fmt);
        }
        else
        {
            sprintf_s(fmt, sizeof(fmt),
                "  警告: UF_DRAW_ask_drawing_info 返回 %d（无法交叉核对图纸尺寸）", rc);
            print_msg(fmt);
        }

        NXOpen::View* sv = sheet->View();
        if (sv)
        {
            NXOpen::Point3d absOrg = sv->AbsoluteOrigin();
            sprintf_s(fmt, sizeof(fmt),
                "  图纸视图绝对原点: (%.3f, %.3f, %.3f)（非零则提示图纸原点不在绝对原点）",
                absOrg.X, absOrg.Y, absOrg.Z);
            print_msg(fmt);
        }
        return (len > 1e-6 && hgt > 1e-6);
    }
    catch (const NXOpen::NXException& e)
    {
        print_msg(std::string("  警告: 图纸实测尺寸失败: ") + e.Message());
        return false;
    }
    catch (...)
    {
        print_msg("  警告: 图纸实测尺寸失败（未知异常）");
        return false;
    }
}

//------------------------------------------------------------------------------
// 曲线 2D 包围盒（参数采样求点；圆/圆弧用圆心±半径补精确界）
//------------------------------------------------------------------------------
static bool ask_curve_bbox_2d(tag_t t, double b[4])
{
    bool got = false;
    const int kSamples = 16;
    for (int i = 0; i <= kSamples; ++i)
    {
        double par = (double)i / (double)kSamples;
        double pt[3], tg[3], pn[3], bn[3], torsion = 0.0, roc = 0.0;
        if (UF_MODL_ask_curve_props(t, par, pt, tg, pn, bn, &torsion, &roc) != 0)
            continue;
        if (!got)
        {
            b[0] = b[2] = pt[0];
            b[1] = b[3] = pt[1];
            got = true;
        }
        else
        {
            if (pt[0] < b[0]) b[0] = pt[0];
            if (pt[1] < b[1]) b[1] = pt[1];
            if (pt[0] > b[2]) b[2] = pt[0];
            if (pt[1] > b[3]) b[3] = pt[1];
        }
    }
    int type = 0, subtype = 0;
    UF_OBJ_ask_type_and_subtype(t, &type, &subtype);
    if (got && type == UF_circle_type)
    {
        UF_CURVE_arc_t arcData;
        if (UF_CURVE_ask_arc_data(t, &arcData) == 0)
        {
            const double r = arcData.radius;
            const double cx = arcData.arc_center[0];
            const double cy = arcData.arc_center[1];
            if (cx - r < b[0]) b[0] = cx - r;
            if (cy - r < b[1]) b[1] = cy - r;
            if (cx + r > b[2]) b[2] = cx + r;
            if (cy + r > b[3]) b[3] = cy + r;
        }
    }
    return got;
}

//==============================================================================
// 实测模板图框实际中心（图纸坐标）+ 图框图层集合
// 背景：整体居中目标若按配置假定“图幅中心=(L/2, H/2)、原点在角点”计算，
// 当模板图框相对图纸有留边、或图纸原点存在偏移时，视图会整体偏离图框视觉中心。
// 本函数按图层遍历图纸上的模板曲线（视图创建前调用），取“图框级大框曲线”
// 并集包围盒中心作为图框实际中心；并回显图纸视图边界/绝对原点供诊断。
//==============================================================================
bool ask_sheet_frame_center(NXOpen::Part* part,
                            NXOpen::Drawings::DraftingDrawingSheet* sheet,
                            NXOpen::Point3d& frameCenter,
                            std::vector<int>& frameLayers)
{
    frameLayers.clear();
    if (!part || !sheet) return false;

    char fmt[512];
    double L = 0.0, H = 0.0;
    if (!ask_sheet_actual_size(sheet, L, H))
    {
        print_msg("  警告: 图纸实测尺寸不可用，图框中心探测跳过");
        return false;
    }

    try
    {
        // 图纸视图边界（图纸坐标，诊断原点偏移）
        double sb[4] = { 0.0, 0.0, L, H };
        int rcSb = -1;
        NXOpen::View* sv = sheet->View();
        if (sv) rcSb = UF_DRAW_ask_view_borders(sv->Tag(), sb);
        sprintf_s(fmt, sizeof(fmt),
            "  图纸视图边界(图纸坐标 rc=%d): (%.3f, %.3f, %.3f, %.3f)",
            rcSb, sb[0], sb[1], sb[2], sb[3]);
        print_msg(fmt);

        // 按图层遍历模板曲线，累计图框级大框曲线的并集包围盒
        const double kFrameRatio = 0.6;   // 图框判定阈值：包围盒宽/高均 >= 0.6 倍图幅
        double u[4] = { 0.0, 0.0, 0.0, 0.0 };
        bool have = false;
        int totalCurves = 0, frameCurves = 0;
        std::set<int> layers;
        for (int ly = UF_LAYER_MIN_LAYER; ly <= UF_LAYER_MAX_LAYER; ++ly)
        {
            tag_t obj = NULL_TAG;
            bool layerHasFrame = false;
            for (;;)
            {
                int rc = UF_LAYER_cycle_by_layer(ly, &obj);
                if (rc != 0 || obj == NULL_TAG) break;
                int type = 0, subtype = 0;
                if (UF_OBJ_ask_type_and_subtype(obj, &type, &subtype) != 0) continue;
                if (type != UF_line_type && type != UF_circle_type &&
                    type != UF_conic_type && type != UF_spline_type)
                    continue;
                ++totalCurves;
                double cb[4] = { 0.0, 0.0, 0.0, 0.0 };
                if (!ask_curve_bbox_2d(obj, cb)) continue;
                const double w = cb[2] - cb[0];
                const double h = cb[3] - cb[1];
                if (w < kFrameRatio * L || h < kFrameRatio * H) continue;
                ++frameCurves;
                layerHasFrame = true;
                if (!have)
                {
                    u[0] = cb[0]; u[1] = cb[1]; u[2] = cb[2]; u[3] = cb[3];
                    have = true;
                }
                else
                {
                    if (cb[0] < u[0]) u[0] = cb[0];
                    if (cb[1] < u[1]) u[1] = cb[1];
                    if (cb[2] > u[2]) u[2] = cb[2];
                    if (cb[3] > u[3]) u[3] = cb[3];
                }
            }
            if (layerHasFrame) layers.insert(ly);
        }
        for (std::set<int>::const_iterator it = layers.begin(); it != layers.end(); ++it)
            frameLayers.push_back(*it);

        sprintf_s(fmt, sizeof(fmt),
            "  图框探测: 模板曲线共 %d 条, 图框级大框 %d 条",
            totalCurves, frameCurves);
        print_msg(fmt);

        if (!have)
        {
            frameCenter = NXOpen::Point3d(sb[0] + L / 2.0, sb[1] + H / 2.0, 0.0);
            sprintf_s(fmt, sizeof(fmt),
                "  警告: 未找到图框级曲线，整体居中目标兜底取图纸几何中心 (%.3f, %.3f)",
                frameCenter.X, frameCenter.Y);
            print_msg(fmt);
            return false;
        }

        NXOpen::Point3d c((u[0] + u[2]) / 2.0, (u[1] + u[3]) / 2.0, 0.0);
        sprintf_s(fmt, sizeof(fmt),
            "  图框并集包围盒: (%.3f, %.3f, %.3f, %.3f), 中心(%.3f, %.3f)",
            u[0], u[1], u[2], u[3], c.X, c.Y);
        print_msg(fmt);

        // 图框中心应落在图纸边界内；若越界，尝试按“曲线坐标为绝对坐标、
        // 图纸视图原点存在偏移”修正：图纸坐标 = 绝对坐标 - 视图绝对原点
        const bool inSheet =
            c.X >= sb[0] - 1.0 && c.X <= sb[2] + 1.0 &&
            c.Y >= sb[1] - 1.0 && c.Y <= sb[3] + 1.0;
        if (!inSheet && sv)
        {
            NXOpen::Point3d o = sv->AbsoluteOrigin();
            NXOpen::Point3d c2(c.X - o.X, c.Y - o.Y, 0.0);
            const bool inSheet2 =
                c2.X >= sb[0] - 1.0 && c2.X <= sb[2] + 1.0 &&
                c2.Y >= sb[1] - 1.0 && c2.Y <= sb[3] + 1.0;
            if (inSheet2)
            {
                sprintf_s(fmt, sizeof(fmt),
                    "  图框中心越出图纸边界，按图纸视图绝对原点偏移修正: (%.3f, %.3f) -> (%.3f, %.3f)",
                    c.X, c.Y, c2.X, c2.Y);
                print_msg(fmt);
                c = c2;
            }
            else
            {
                sprintf_s(fmt, sizeof(fmt),
                    "  警告: 图框中心(%.3f, %.3f)越出图纸边界且无法修正，兜底取图纸几何中心",
                    c.X, c.Y);
                print_msg(fmt);
                c = NXOpen::Point3d(sb[0] + L / 2.0, sb[1] + H / 2.0, 0.0);
            }
        }

        frameCenter = c;
        sprintf_s(fmt, sizeof(fmt),
            "  图框实际中心(图纸坐标): (%.3f, %.3f)", frameCenter.X, frameCenter.Y);
        print_msg(fmt);
        return true;
    }
    catch (const NXOpen::NXException& e)
    {
        print_msg(std::string("  警告: 图框中心探测失败: ") + e.Message());
        return false;
    }
    catch (...)
    {
        print_msg("  警告: 图框中心探测失败（未知异常）");
        return false;
    }
}

//==============================================================================
// 运行时查找已有剖视图（遍历 part->DraftingViews()，找第一个 SectionView）
//==============================================================================
NXOpen::Drawings::SectionView* find_section_view(NXOpen::Part* part)
{
    if (!part) return NULL;
    for (NXOpen::Drawings::DraftingViewCollection::iterator it =
        part->DraftingViews()->begin();
        it != part->DraftingViews()->end(); ++it)
    {
        NXOpen::Drawings::SectionView* sv =
            dynamic_cast<NXOpen::Drawings::SectionView*>(*it);
        if (sv) return sv;
    }
    return NULL;
}

//==============================================================================
// 运行时查找已有中心线（遍历 part->Annotations()->Centerlines()，找第一个 Centerline2d）
//==============================================================================
NXOpen::Annotations::Centerline2d* find_centerline(NXOpen::Part* part)
{
    if (!part) return NULL;
    NXOpen::Annotations::CenterlineCollection* clc =
        part->Annotations()->Centerlines();
    if (!clc) return NULL;
    for (NXOpen::Annotations::CenterlineCollection::iterator it = clc->begin();
        it != clc->end(); ++it)
    {
        NXOpen::Annotations::Centerline2d* cl2d =
            dynamic_cast<NXOpen::Annotations::Centerline2d*>(*it);
        if (cl2d) return cl2d;
    }
    return NULL;
}

} // namespace CommonUtils

//==============================================================================
// 曲线枚举与标注放置点计算（Step4 线性标注自动化使用）
//==============================================================================
namespace CommonUtils {

//------------------------------------------------------------------------------
// build_curve_info —— 由 tag 构造 CurveInfo（含取消隐藏 + 端点/切矢 + 圆弧参数）
// 多策略枚举共用的转换逻辑（原单策略版内联代码提取）
//------------------------------------------------------------------------------
static CurveInfo build_curve_info(tag_t t, int& unblankCount)
{
    int type = 0, subtype = 0;
    UF_OBJ_ask_type_and_subtype(t, &type, &subtype);

    // 检查并取消隐藏状态（拆分/截面生成后曲线可能被隐藏）
    UF_OBJ_disp_props_t props;
    if (UF_OBJ_ask_display_properties(t, &props) == 0 &&
        props.blank_status == UF_OBJ_BLANKED)
    {
        UF_OBJ_set_blank_status(t, UF_OBJ_NOT_BLANKED);
        ++unblankCount;
    }

    CurveInfo ci;
    memset(&ci, 0, sizeof(ci));
    ci.tag = t;
    ci.type = type;
    ci.subtype = subtype;
    ci.is_closed = false;
    ci.has_props = false;
    ci.arc_radius = 0.0;

    // 获取 parm=0.0 处的端点和切矢
    double pt[3], tg[3], pn[3], bn[3], torsion = 0.0, roc = 0.0;
    bool ok0 = UF_MODL_ask_curve_props(t, 0.0, pt, tg, pn, bn, &torsion, &roc) == 0;
    if (ok0)
    {
        memcpy(ci.start_pt, pt, sizeof(double) * 3);
        memcpy(ci.start_tg, tg, sizeof(double) * 3);
    }
    // 获取 parm=1.0 处的端点和切矢
    bool ok1 = UF_MODL_ask_curve_props(t, 1.0, pt, tg, pn, bn, &torsion, &roc) == 0;
    if (ok1)
    {
        memcpy(ci.end_pt, pt, sizeof(double) * 3);
        memcpy(ci.end_tg, tg, sizeof(double) * 3);
    }
    ci.has_props = ok0 || ok1;

    // 对于圆弧类型，额外获取中心和半径
    if (type == UF_circle_type)
    {
        UF_CURVE_arc_t arcData;
        if (UF_CURVE_ask_arc_data(t, &arcData) == 0)
        {
            ci.arc_center[0] = arcData.arc_center[0];
            ci.arc_center[1] = arcData.arc_center[1];
            ci.arc_center[2] = arcData.arc_center[2];
            ci.arc_radius = arcData.radius;
            // 判断是否封闭圆（起点与终点距离 < 容差）
            const double kDupTol = 0.01;
            const double dx = ci.end_pt[0] - ci.start_pt[0];
            const double dy = ci.end_pt[1] - ci.start_pt[1];
            const double dz = ci.end_pt[2] - ci.start_pt[2];
            if (dx * dx + dy * dy + dz * dz < kDupTol * kDupTol)
            {
                ci.is_closed = true;
            }
        }
    }
    return ci;
}

//------------------------------------------------------------------------------
// enumerate_view_curves —— 多策略枚举视图内所有曲线/边，返回 CurveInfo 列表
// 策略A: UF_VIEW_cycle_objects + UF_VIEW_VISIBLE_OBJECTS（原策略）
// 策略B: NXOpen DraftingView->DraftingBodies()->DraftingCurves()
//        （截面边是视图 DraftingBody 所有的 DraftingCurve，
//        录制宏通过 draftingCurves.FindObject("(Section Edge) FACE xx") 访问）
// 策略C: UF_DRAW_ask_sxsolids_of_sxview → ask_sxedges_of_sxsolid
//        → ask_curve_of_sxedge（剖视图专用 UF 截面边 API）
// 三策略结果按 tag 去重合并，每策略输出诊断日志
//------------------------------------------------------------------------------
std::vector<CurveInfo> enumerate_view_curves(NXOpen::View* view)
{
    std::vector<CurveInfo> result;
    if (!view) return result;

    char fmt[512];
    int unblankCount = 0;
    std::set<tag_t> seen;   // 按 tag 去重

    // ---- 确保视图已更新（再生投影几何） ----
    NXOpen::Session* sess = get_session();
    silent_update(sess);
    NXOpen::Drawings::DraftingView* dv = dynamic_cast<NXOpen::Drawings::DraftingView*>(view);
    if (dv) dv->Update();

    // ---- 策略A：UF_VIEW_cycle_objects 遍历视图成员，过滤曲线/边类 ----
    int countA = 0;
    {
        tag_t curObj = NULL_TAG;
        for (;;)
        {
            int rc = UF_VIEW_cycle_objects(view->Tag(), UF_VIEW_VISIBLE_OBJECTS, &curObj);
            if (rc != 0 || curObj == NULL_TAG) break;
            int type = 0, subtype = 0;
            if (UF_OBJ_ask_type_and_subtype(curObj, &type, &subtype) != 0) continue;
            bool keep = false;
            if (type == UF_line_type || type == UF_circle_type ||
                type == UF_conic_type || type == UF_spline_type)
            {
                keep = true;
            }
            else if (type == UF_solid_type && subtype == UF_solid_edge_subtype)
            {
                keep = true;
            }
            if (!keep) continue;
            if (seen.insert(curObj).second)
            {
                result.push_back(build_curve_info(curObj, unblankCount));
                ++countA;
            }
        }
    }
    sprintf_s(fmt, sizeof(fmt),
        "[枚举诊断] 策略A UF_VIEW_cycle_objects: 找到 %d 条", countA);
    print_msg(fmt);

    // ---- 策略B：DraftingBody 的 DraftingCurve 集合（截面边所属） ----
    int countB = 0;
    if (dv)
    {
        try
        {
            NXOpen::Drawings::DraftingBodyCollection* bodies = dv->DraftingBodies();
            if (bodies)
            {
                for (NXOpen::Drawings::DraftingBodyCollection::iterator bit = bodies->begin();
                    bit != bodies->end(); ++bit)
                {
                    NXOpen::Drawings::DraftingBody* body = *bit;
                    if (!body) continue;
                    NXOpen::Drawings::DraftingCurveCollection* curvesColl = body->DraftingCurves();
                    if (!curvesColl) continue;
                    for (NXOpen::Drawings::DraftingCurveCollection::iterator cit = curvesColl->begin();
                        cit != curvesColl->end(); ++cit)
                    {
                        NXOpen::Drawings::DraftingCurve* dc = *cit;
                        if (!dc) continue;
                        tag_t t = dc->Tag();
                        if (t == NULL_TAG || !seen.insert(t).second) continue;
                        result.push_back(build_curve_info(t, unblankCount));
                        ++countB;
                        // 打印前 3 条诊断（截面边 JournalIdentifier 形如
                        // "(Section Edge) FACE 49 ..."；Name() 实测为空，
                        // 另附 UF 类型/子类型以确认对象真实形态）
                        if (countB <= 3)
                        {
                            std::string jid;
                            try { jid = dc->JournalIdentifier().GetText(); }
                            catch (...) { jid = "<无JournalIdentifier>"; }
                            int bType = 0, bSub = 0;
                            UF_OBJ_ask_type_and_subtype(t, &bType, &bSub);
                            sprintf_s(fmt, sizeof(fmt),
                                "  [枚举诊断] 策略B 示例 #%d: %s (tag=%llu, type=%d, subtype=%d)",
                                countB, jid.c_str(), (unsigned long long)t, bType, bSub);
                            print_msg(fmt);
                        }
                    }
                }
            }
        }
        catch (const NXOpen::NXException& e)
        {
            print_msg(std::string("  [枚举诊断] 策略B 异常: ") + e.Message());
        }
        catch (...)
        {
            print_msg("  [枚举诊断] 策略B 异常（未知）");
        }
    }
    sprintf_s(fmt, sizeof(fmt),
        "[枚举诊断] 策略B DraftingBody: 找到 %d 条", countB);
    print_msg(fmt);

    // ---- 策略C：UF_DRAW 截面边 API（仅剖视图适用） ----
    // UF_DRAW_ask_sxsolids_of_sxview(view, leg, &n, &solids)
    //   → UF_DRAW_ask_sxedges_of_sxsolid(solid, &n, &edges)
    //   → UF_DRAW_ask_curve_of_sxedge(edge, &curve)
    // （签名已对照 NX12 uf_draw.h 核实；UF_DRAW_ask_view_members 在 NX12 中不存在）
    int countC = 0;
    {
        bool cAvail = false;
        NXOpen::Drawings::SectionView* sv =
            dynamic_cast<NXOpen::Drawings::SectionView*>(view);
        for (int leg = UF_DRAW_sxline_leg1; leg <= UF_DRAW_sxline_leg2; ++leg)
        {
            int nSolids = 0;
            tag_t* solids = NULL;
            int rcS = UF_DRAW_ask_sxsolids_of_sxview(
                view->Tag(), (UF_DRAW_sxline_leg_t)leg, &nSolids, &solids);
            if (rcS != 0 || nSolids == 0 || !solids)
            {
                if (solids) UF_free(solids);
                continue;
            }
            cAvail = true;
            for (int i = 0; i < nSolids; ++i)
            {
                int nEdges = 0;
                tag_t* edges = NULL;
                if (UF_DRAW_ask_sxedges_of_sxsolid(solids[i], &nEdges, &edges) != 0
                    || !edges)
                {
                    if (edges) UF_free(edges);
                    continue;
                }
                for (int j = 0; j < nEdges; ++j)
                {
                    tag_t curveTag = NULL_TAG;
                    // 折弯段截面无关联曲线时返回 UF_DRAW_no_sxedge_curve 且 curveTag=NULL
                    if (UF_DRAW_ask_curve_of_sxedge(edges[j], &curveTag) != 0
                        || curveTag == NULL_TAG) continue;
                    if (!seen.insert(curveTag).second) continue;
                    result.push_back(build_curve_info(curveTag, unblankCount));
                    ++countC;
                }
                UF_free(edges);
            }
            UF_free(solids);
            // 普通（非回转）剖视图仅 leg1 有效，leg2 失败不视为不可用
            if (sv == NULL) break;
        }
        sprintf_s(fmt, sizeof(fmt),
            "[枚举诊断] 策略C UF_DRAW截面边: %s，找到 %d 条",
            cAvail ? "可用" : "不可用（无 sxsolid 或非剖视图）", countC);
        print_msg(fmt);
    }

    // ---- 几何去重：同一段几何往往同时存在"实体边(solid edge)"与"制图曲线
    // (DraftingCurve)"两个对象（策略A 收实体边、策略B/C 收 DraftingCurve）。
    // 录制 VB 宏（000R_VB.vb）的尺寸/坐标标注关联对象全部是 DraftingCurve；
    // 实体边会导致坐标标注报 "The first object associativity type is invalid"、
    // 线性标注推断失败，且两条对象都会参与配对造成重复标注。
    // 因此按"类型 + 端点坐标（0.02mm 容差）"去重，优先保留非实体边对象。
    {
        const double kGeoTol = 0.02;
        const int nBefore = (int)result.size();
        std::vector<CurveInfo> deduped;
        for (size_t i = 0; i < result.size(); ++i)
        {
            const CurveInfo& ci = result[i];
            bool found = false;
            for (size_t j = 0; j < deduped.size() && !found; ++j)
            {
                const CurveInfo& dj = deduped[j];
                // 圆类与直线/样条类分开比较（圆还要求圆心/半径一致）
                const bool ciCircle = (ci.type == UF_circle_type);
                const bool djCircle = (dj.type == UF_circle_type);
                if (ciCircle != djCircle) continue;
                // 端点距离（允许起终点互换）
                double d1 = 0, d2 = 0, d3 = 0, d4 = 0;
                for (int k = 0; k < 3; ++k)
                {
                    double a = ci.start_pt[k] - dj.start_pt[k]; d1 += a * a;
                    a = ci.end_pt[k] - dj.end_pt[k];   d2 += a * a;
                    a = ci.start_pt[k] - dj.end_pt[k]; d3 += a * a;
                    a = ci.end_pt[k] - dj.start_pt[k]; d4 += a * a;
                }
                bool sameGeo = (sqrt(d1) < kGeoTol && sqrt(d2) < kGeoTol) ||
                               (sqrt(d3) < kGeoTol && sqrt(d4) < kGeoTol);
                if (ciCircle && djCircle && sameGeo)
                {
                    // 圆心/半径容差复核（实体边无 arc 数据时退化为端点比较）
                    double dc = 0;
                    for (int k = 0; k < 3; ++k)
                    {
                        double a = ci.arc_center[k] - dj.arc_center[k]; dc += a * a;
                    }
                    if (sqrt(dc) >= kGeoTol) sameGeo = false;
                    if (fabs(ci.arc_radius - dj.arc_radius) >= kGeoTol) sameGeo = false;
                }
                if (!sameGeo) continue;
                found = true;
                // 已收录的是实体边而新条目是制图曲线 -> 用制图曲线替换
                const bool djSolid = (dj.type == UF_solid_type &&
                    dj.subtype == UF_solid_edge_subtype);
                const bool ciSolid = (ci.type == UF_solid_type &&
                    ci.subtype == UF_solid_edge_subtype);
                if (djSolid && !ciSolid) deduped[j] = ci;
            }
            if (!found) deduped.push_back(ci);
        }
        result.swap(deduped);
        if (nBefore != (int)result.size())
        {
            sprintf_s(fmt, sizeof(fmt),
                "[枚举诊断] 几何去重: %d -> %d（实体边/DraftingCurve 重复，保留制图曲线）",
                nBefore, (int)result.size());
            print_msg(fmt);
        }
    }

    if (unblankCount > 0)
    {
        sprintf_s(fmt, sizeof(fmt),
            "[CommonUtils] 已取消隐藏 %d 条曲线/边", unblankCount);
        print_msg(fmt);
    }

    // 统计端点属性求取失败的曲线（失败者端点保持 (0,0,0)，调用方须按
    // has_props 过滤，否则无效关联点会导致标注创建失败）
    int noProps = 0;
    for (size_t i = 0; i < result.size(); ++i)
        if (!result[i].has_props) ++noProps;

    sprintf_s(fmt, sizeof(fmt),
        "[枚举诊断] 合并去重后共 %d 条 (A=%d, B=%d, C=%d)，其中端点求取失败 %d 条",
        (int)result.size(), countA, countB, countC, noProps);
    print_msg(fmt);

    return result;
}

//------------------------------------------------------------------------------
// classify_edges —— 按类型分类曲线为直线段和圆弧
//------------------------------------------------------------------------------
void classify_edges(const std::vector<CurveInfo>& curves,
                    std::vector<CurveInfo>& lines,
                    std::vector<CurveInfo>& arcs)
{
    lines.clear();
    arcs.clear();
    for (size_t i = 0; i < curves.size(); ++i)
    {
        if (curves[i].type == UF_circle_type)
        {
            arcs.push_back(curves[i]);
        }
        else
        {
            // line_type / conic_type / spline_type / solid_edge 均归入直线段
            lines.push_back(curves[i]);
        }
    }
}

//------------------------------------------------------------------------------
// map_model_to_drawing —— 模型/绝对坐标 -> 图纸坐标（封装 UF_VIEW_map_model_to_drawing）
// 头文件证据：uf_view.h L1532-1556："Maps a point in absolute space to drawing
// coordinates"，输入 member_view（图纸成员视图）+ 绝对坐标 3D 点，
// 输出图纸 2D 点；变换自动包含视图比例/旋转/朝向/视图原点。
//------------------------------------------------------------------------------
bool map_model_to_drawing(tag_t memberView, const double modelPt[3], double drawPt[2])
{
    double pt[3] = { modelPt[0], modelPt[1], modelPt[2] };
    double out[2] = { 0.0, 0.0 };
    if (UF_VIEW_map_model_to_drawing(memberView, pt, out) != 0)
        return false;
    drawPt[0] = out[0];
    drawPt[1] = out[1];
    return true;
}

//------------------------------------------------------------------------------
// calc_linear_placement —— 计算线性标注放置点（全图纸坐标版）
// 由两边映射后的起止点在图纸坐标系下判断水平/垂直，放置在视图边界外侧 offset 处；
// 沿边方向的坐标取两边中点平均，使标注落在被测边对应位置附近。
//------------------------------------------------------------------------------
NXOpen::Point3d calc_linear_placement(const double s1[2], const double e1[2],
                                      const double s2[2], const double e2[2],
                                      const double borders[4], double offset)
{
    // 两边中点（图纸坐标）
    double mid1x = (s1[0] + e1[0]) / 2.0, mid1y = (s1[1] + e1[1]) / 2.0;
    double mid2x = (s2[0] + e2[0]) / 2.0, mid2y = (s2[1] + e2[1]) / 2.0;

    // 方向由图纸坐标判断：两边方向分量平均，|dx|>|dy| -> 边水平
    double dx = (fabs(e1[0] - s1[0]) + fabs(e2[0] - s2[0])) / 2.0;
    double dy = (fabs(e1[1] - s1[1]) + fabs(e2[1] - s2[1])) / 2.0;

    double placeX, placeY;
    if (dx > dy)
    {
        // 边水平 -> 垂直标注：Y = 视图上缘 + offset，X = 两边中点 X 平均
        placeY = borders[3] + offset;
        placeX = (mid1x + mid2x) / 2.0;
    }
    else
    {
        // 边垂直 -> 水平标注：X = 视图右缘 + offset，Y = 两边中点 Y 平均
        placeX = borders[2] + offset;
        placeY = (mid1y + mid2y) / 2.0;
    }

    // 防护：若映射异常导致沿边坐标远离视图，钳制到边界外侧附近，避免标注飞走
    const double kMargin = offset + 40.0;
    if (placeX < borders[0] - kMargin) placeX = borders[0] - offset;
    if (placeX > borders[2] + kMargin) placeX = borders[2] + offset;
    if (placeY < borders[1] - kMargin) placeY = borders[1] - offset;
    if (placeY > borders[3] + kMargin) placeY = borders[3] + offset;

    return NXOpen::Point3d(placeX, placeY, 0.0);
}

//------------------------------------------------------------------------------
// calc_radial_placement —— 计算径向/直径标注放置点（全图纸坐标版）
// 径方向 = 圆心 -> 圆弧中点（图纸坐标）；放置点 = 圆心 + 方向×(半径+offset)；
// 结果钳制到视图边界外扩 1.5×offset 的范围内，确保标注落在视图附近。
//------------------------------------------------------------------------------
NXOpen::Point3d calc_radial_placement(const double centerDraw[2], const double midDraw[2],
                                      double drawRadius,
                                      const double borders[4], double offset)
{
    double dirX = midDraw[0] - centerDraw[0];
    double dirY = midDraw[1] - centerDraw[1];
    double len = sqrt(dirX * dirX + dirY * dirY);
    if (len < 1e-6)
    {
        // 中点与圆心重合（完整圆或退化）：默认向右上方放置
        dirX = dirY = 0.7071067811865476;
    }
    else
    {
        dirX /= len;
        dirY /= len;
    }

    double placeX = centerDraw[0] + dirX * (drawRadius + offset);
    double placeY = centerDraw[1] + dirY * (drawRadius + offset);

    // 钳制：放置点应落在视图边界外扩范围内的图纸侧，避免飞离视图
    const double pad = offset * 1.5;
    if (placeX < borders[0] - pad) placeX = borders[0] - offset * 0.5;
    if (placeX > borders[2] + pad) placeX = borders[2] + offset * 0.5;
    if (placeY < borders[1] - pad) placeY = borders[1] - offset * 0.5;
    if (placeY > borders[3] + pad) placeY = borders[3] + offset * 0.5;

    return NXOpen::Point3d(placeX, placeY, 0.0);
}

} // namespace CommonUtils
