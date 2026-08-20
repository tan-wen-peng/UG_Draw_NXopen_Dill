//------------------------------------------------------------------------------
// NX12_NXOpenCPP_爆炸图  --  Step 1: 爆炸图视图创建
//
// 依据录制文件 d:\A_UG\05_爆炸图\HW170F爆炸图0.vb 提取的核心操作实现:
//   1) 菜单 装配(A)->爆炸图(X)->新建爆炸(N)
//        workPart.ComponentAssembly.Explosions.Create("Explosion 1")
//   2) explosion.Show(workPart.ModelingViews.WorkView)
//   3) 菜单 视图(V)->操作(O)->另存为(A)
//        workPart.Views.SaveAsPreservingCase(workView, "爆炸图01", False, False)
//
// 说明:
//   - 录制文件中的视图平移/缩放/旋转(SetOrigin/ZoomAboutPoint/
//     SetRotationTranslationScale)属于观察操作, 与爆炸图创建逻辑无关, 已剔除.
//   - "自动爆炸组件(A)/编辑爆炸(E)"为对话框交互, 录制文件未记录任何
//     爆炸位移参数(手动拖拽不产生 journal 代码); NX12 的 NXOpen 无自动
//     爆炸接口(自动爆炸 API 为 NX1953 才引入). 自动化策略:
//       1) 优先读取 dll 同目录下的 explosion_params.txt 参数文件
//          (由 提取爆炸参数.vb 从手动爆炸数据中提取, 格式: 组件显示名|dx|dy|dz),
//          按组件显示名匹配, 用 UF_ASSEM_explode_component 精确重放手动移动;
//       2) 无参数文件时进入"左右散开"交互模式:
//           多选参与组件 → 可选原点/基准件(不动; 不选则用 WCS 原点分界,
//           全部散开) → 指定方向轴(向量构造器, 可选坐标系轴) → 按组件相对
//           分界点的轴向投影符号自动分组(正侧向正方向、负侧向负方向散开);
//           用 UF_MODL_ask_bounding_box_exact 捕捉每个组件沿移动轴的真实
//           规格尺寸(对齐移动轴的精确包围盒), 默认目标位置按组件真实宽度
//           打包累加(相邻组件间隙均匀, 线度不同的组件互不重叠/追上),
//           可逐组件修改目标位置, 用 UF_ASSEM_explode_component 写入位移
//           (位移在组件局部坐标系中解释, 需将绝对位移转换为局部位移),
//           确认后自动检测相邻组件是否重叠并打印警告;
//           目标位置对话框支持在线反复编辑: 修改/换轴后重新弹出并预填
//           上次确认值(按组件名记忆), 直到用户 Finish 才释放编辑状态.
//
// 幂等设计(可重复执行, 便于流水线化):
//   - 同名爆炸图 "Explosion 1" 已存在时复用, 否则新建
//   - 同名视图 "爆炸图01" 已存在时复用并刷新爆炸显示, 否则另存为新建
//------------------------------------------------------------------------------

// Mandatory UF Includes
#include <uf.h>
#include <uf_assem.h>
#include <uf_csys.h>
#include <uf_disp.h>
#include <uf_disp_types.h>
#include <uf_modl_utilities.h>
#include <uf_mtx.h>
#include <uf_obj.h>
#include <uf_object_types.h>
#include <uf_ui.h>

// Internal Includes
#include <NXOpen/ListingWindow.hxx>
#include <NXOpen/NXMessageBox.hxx>
#include <NXOpen/UI.hxx>

// Internal+External Includes
#include <NXOpen/Annotations.hxx>
#include <NXOpen/Assemblies_Component.hxx>
#include <NXOpen/Assemblies_ComponentAssembly.hxx>
#include <NXOpen/Assemblies_Explosion.hxx>
#include <NXOpen/Assemblies_ExplosionCollection.hxx>
#include <NXOpen/Body.hxx>
#include <NXOpen/BodyCollection.hxx>
#include <NXOpen/Face.hxx>
#include <NXOpen/Line.hxx>
#include <NXOpen/ModelingView.hxx>
#include <NXOpen/ModelingViewCollection.hxx>
#include <NXOpen/NXException.hxx>
#include <NXOpen/NXObject.hxx>
#include <NXOpen/Part.hxx>
#include <NXOpen/PartCollection.hxx>
#include <NXOpen/Session.hxx>
#include <NXOpen/View.hxx>
#include <NXOpen/ViewCollection.hxx>

// Std C++ Includes
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <vector>

// Windows API (获取 dll 路径)
#include <windows.h>

using namespace NXOpen;
using std::string;
using std::exception;
using std::stringstream;
using std::endl;
using std::cout;
using std::cerr;

//------------------------------------------------------------------------------
// 交互式爆炸默认距离(装配单位, 通常为 mm): uc1609 输入框的默认值
//------------------------------------------------------------------------------
const double kExplodeDistance = 50.0;

//------------------------------------------------------------------------------
// 全局打印: 输出到 NX 信息窗口(供全局辅助函数使用)
//------------------------------------------------------------------------------
static void print(const std::string &msg)
{
    NXOpen::ListingWindow *lw = NXOpen::Session::GetSession()->ListingWindow();
    if (!lw->IsOpen()) lw->Open();
    lw->WriteLine(msg);
}

static void print(const char *msg)
{
    NXOpen::ListingWindow *lw = NXOpen::Session::GetSession()->ListingWindow();
    if (!lw->IsOpen()) lw->Open();
    lw->WriteLine(msg);
}

//------------------------------------------------------------------------------
// 类选择器的选择初始化回调: 仅允许选择组件
//------------------------------------------------------------------------------
static int selInitComponent(UF_UI_selection_p_t select, void *userData)
{
    UF_UI_mask_t mask;
    mask.object_type = UF_component_type;
    mask.object_subtype = UF_all_subtype;
    mask.solid_type = 0;
    UF_UI_set_sel_mask(select, UF_UI_SEL_MASK_CLEAR_AND_ENABLE_SPECIFIC, 1, &mask);
    return UF_UI_SEL_SUCCESS;
}

//------------------------------------------------------------------------------
// 将选择返回的组件 tag 归一化为 part occurrence tag
// (UF_ASSEM_explode_component 要求 part occurrence)
//------------------------------------------------------------------------------
static tag_t toPartOccurrence(tag_t rootOcc, tag_t t)
{
    if (UF_ASSEM_is_occurrence(t))
        return t;
    tag_t occ = UF_ASSEM_ask_part_occ_of_inst(rootOcc, t);
    return (occ == NULL_TAG) ? t : occ;
}

//------------------------------------------------------------------------------
// 获取对象(支持 occurrence)的绝对坐标包围盒中心
//------------------------------------------------------------------------------
static bool getGeometricCenter(tag_t objTag, double center[3])
{
    double bbox[6];
    if (UF_MODL_ask_bounding_box(objTag, bbox) == 0)
    {
        center[0] = (bbox[0] + bbox[3]) * 0.5;
        center[1] = (bbox[1] + bbox[4]) * 0.5;
        center[2] = (bbox[2] + bbox[5]) * 0.5;
        return true;
    }
    return false;
}

//------------------------------------------------------------------------------
// 获取组件的绝对位置: 优先几何中心(包围盒中心, 与视觉一致), 失败回退组件原点
//------------------------------------------------------------------------------
static bool getComponentPosition(tag_t occ, double pos[3])
{
    if (getGeometricCenter(occ, pos))
        return true;
    char partName[256] = {0}, refsetName[256] = {0}, instName[256] = {0};
    double csys[9];
    double tx[4][4];
    return UF_ASSEM_ask_component_data(occ, partName, refsetName,
                                       instName, pos, csys, tx) == 0;
}

//------------------------------------------------------------------------------
// 获取组件显示名(instance 名, 打印明细用)
//------------------------------------------------------------------------------
static std::string getComponentName(tag_t occ)
{
    char partName[256] = {0}, refsetName[256] = {0}, instName[256] = {0};
    double csys[9], tx[4][4], pos[3];
    if (UF_ASSEM_ask_component_data(occ, partName, refsetName,
                                    instName, pos, csys, tx) == 0 &&
        instName[0] != '\0')
        return std::string(instName);
    return "(unnamed)";
}

//------------------------------------------------------------------------------
// 获取 WCS 的原点与矩阵(行向量约定 [0..2]=X, [3..5]=Y, [6..8]=Z)
//------------------------------------------------------------------------------
static bool getWcsInfo(double origin[3], double mtx[9])
{
    tag_t wcsTag;
    if (UF_CSYS_ask_wcs(&wcsTag) != 0)
        return false;
    tag_t mtxTag;
    if (UF_CSYS_ask_csys_info(wcsTag, &mtxTag, origin) != 0)
        return false;
    return UF_CSYS_ask_matrix_values(mtxTag, mtx) == 0;
}

//------------------------------------------------------------------------------
// 计算组件集合的包围盒对角线长度(用于轴长自适应与分界面容差)
//------------------------------------------------------------------------------
static bool getAssemblyExtent(const std::vector<tag_t> &occList, double &diag)
{
    double minV[3] = {1e30, 1e30, 1e30}, maxV[3] = {-1e30, -1e30, -1e30};
    bool any = false;
    for (size_t i = 0; i < occList.size(); i++)
    {
        double bbox[6];
        if (UF_MODL_ask_bounding_box(occList[i], bbox) == 0)
        {
            for (int k = 0; k < 3; k++)
            {
                if (bbox[k] < minV[k]) minV[k] = bbox[k];
                if (bbox[k + 3] > maxV[k]) maxV[k] = bbox[k + 3];
            }
            any = true;
        }
    }
    if (!any)
        return false;
    double dx = maxV[0] - minV[0], dy = maxV[1] - minV[1], dz = maxV[2] - minV[2];
    diag = sqrt(dx * dx + dy * dy + dz * dz);
    return true;
}

//------------------------------------------------------------------------------
// 递归查找指定 occurrence tag 对应的 NXOpen Component(用于子装配检测)
//------------------------------------------------------------------------------
static NXOpen::Assemblies::Component *findComponentByTag(
    NXOpen::Assemblies::Component *comp, tag_t occTag)
{
    if (comp->Tag() == occTag)
        return comp;
    std::vector<NXOpen::Assemblies::Component *> children = comp->GetChildren();
    for (size_t i = 0; i < children.size(); i++)
    {
        NXOpen::Assemblies::Component *r = findComponentByTag(children[i], occTag);
        if (r != NULL)
            return r;
    }
    return NULL;
}

//------------------------------------------------------------------------------
// 在指定位置按坐标系矩阵画三轴临时线段 + 文字标签
// 正轴实线/负轴虚线, 红X/绿Y/蓝Z; csysMatrix 9 元素, 行向量约定
// ([0..2]=X, [3..5]=Y, [6..8]=Z), 轴长正负各 axisLen
// labelPrefix 为标签前缀: 空=绝对轴(X+), "W"=WCS 轴(WX+)
//------------------------------------------------------------------------------
static void showAxisHint(const double originPos[3], const double csysMatrix[9],
                         double axisLen, const char *labelPrefix)
{
    const int axisColor[3] = {186, 36, 211};  // 红 绿 蓝
    const char *axisNames[3] = {"X", "Y", "Z"};
    UF_OBJ_disp_props_t solidProps, dashProps, textProps;
    solidProps.font = 1;          // 实线
    solidProps.line_width = -1;
    dashProps.font = 2;           // 虚线
    dashProps.line_width = -1;
    textProps.font = 1;
    textProps.line_width = -1;
    UF_DISP_view_type_t viewMode = static_cast<UF_DISP_view_type_t>(UF_DISP_ALL_ACTIVE_VIEWS);

    for (int a = 0; a < 3; a++)
    {
        double ax = csysMatrix[a * 3 + 0];
        double ay = csysMatrix[a * 3 + 1];
        double az = csysMatrix[a * 3 + 2];
        double p1[3] = {originPos[0], originPos[1], originPos[2]};
        double p2[3] = {originPos[0] + ax * axisLen,
                        originPos[1] + ay * axisLen,
                        originPos[2] + az * axisLen};
        double p3[3] = {originPos[0] - ax * axisLen,
                        originPos[1] - ay * axisLen,
                        originPos[2] - az * axisLen};
        solidProps.color = axisColor[a];
        dashProps.color = axisColor[a];
        UF_DISP_display_temporary_line(NULL_TAG, viewMode, p1, p2, &solidProps);
        UF_DISP_display_temporary_line(NULL_TAG, viewMode, p1, p3, &dashProps);

        // 正负轴端点文字标签
        char label[8];
        sprintf(label, "%s%s+", labelPrefix, axisNames[a]);
        textProps.color = axisColor[a];
        UF_DISP_display_temporary_text(NULL_TAG, viewMode, label, p2,
                                       static_cast<UF_DISP_text_ref_t>(UF_DISP_MIDDLECENTER),
                                       &textProps, 0.0, 0);
        sprintf(label, "%s%s-", labelPrefix, axisNames[a]);
        UF_DISP_display_temporary_text(NULL_TAG, viewMode, label, p3,
                                       static_cast<UF_DISP_text_ref_t>(UF_DISP_MIDDLECENTER),
                                       &textProps, 0.0, 0);
    }
}

//------------------------------------------------------------------------------
// 方向轴选择: 弹出 NX 原生"矢量"对话框(可视化坐标系点选)
//   - 类型下拉 XC/YC/ZC = 绝对坐标轴
//   - 图形区动态坐标系轴 = WCS 相对坐标轴
//   - 也可选边/面/两点/角度/基准轴等任意方向
// 弹窗前画出绝对坐标系与 WCS 两套参考轴; 返回 true 时 dir 为绝对坐标单位向量
//------------------------------------------------------------------------------
static bool askDirection(const double originPos[3], double axisLen,
                         double dir[3], std::string &desc)
{
    // 可视化参考: 绝对坐标系(从原点组件位置) + WCS 坐标系(从 WCS 原点)
    const double absMtx[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    showAxisHint(originPos, absMtx, axisLen, "");
    double wcsOrigin[3], wcsMtx[9];
    if (getWcsInfo(wcsOrigin, wcsMtx))
        showAxisHint(wcsOrigin, wcsMtx, axisLen, "W");

    print("Pick the spread axis in the Vector dialog:");
    print("  - XC/YC/ZC in the type list = absolute axes (X+/X- drawn at origin component)");
    print("  - CSYS axes in graphics = WCS relative axes (WX+/WX- drawn at WCS origin)");
    print("  - or pick edges/faces/two points for any direction.");
    print("  - Press OK in the dialog to confirm the direction.");

    int vecMode = UF_UI_INFERRED;
    double vecOrigin[3] = {0.0, 0.0, 0.0};
    int vecResp = 0;
    UF_UI_specify_vector("Specify spread-out direction axis (Vector dialog)", &vecMode,
                         UF_UI_DISP_TEMP_VECTOR, dir, vecOrigin, &vecResp);
    UF_DISP_regenerate_display();  // 刷新清除临时轴参考

    if (vecResp != UF_UI_OK)
        return false;

    switch (vecMode)
    {
    case UF_UI_XC_AXIS:          desc = "XC axis (absolute)"; break;
    case UF_UI_NEGATIVE_XC_AXIS: desc = "-XC axis (absolute)"; break;
    case UF_UI_YC_AXIS:          desc = "YC axis (absolute)"; break;
    case UF_UI_NEGATIVE_YC_AXIS: desc = "-YC axis (absolute)"; break;
    case UF_UI_ZC_AXIS:          desc = "ZC axis (absolute)"; break;
    case UF_UI_NEGATIVE_ZC_AXIS: desc = "-ZC axis (absolute)"; break;
    case UF_UI_FACE_NORMAL:      desc = "Face normal"; break;
    case UF_UI_DATUM_AXIS:       desc = "Datum axis"; break;
    case UF_UI_DATUM_PLANE:      desc = "Datum plane"; break;
    case UF_UI_EDGE_CURVE:       desc = "Edge/curve"; break;
    case UF_UI_TANGENT_TO_CURVE: desc = "Tangent to curve"; break;
    case UF_UI_TWO_POINTS:       desc = "Two points"; break;
    case UF_UI_AT_ANGLE:         desc = "At angle"; break;
    case UF_UI_INFERRED:         desc = "Inferred (picked in graphics)"; break;
    default:
    {
        stringstream ss;
        ss << "Vector (mode " << vecMode << ")";
        desc = ss.str();
        break;
    }
    }
    return true;
}

//------------------------------------------------------------------------------
// 将绝对坐标位移转换为组件局部坐标位移
// UF_ASSEM_explode_component 的 transform 平移列在组件自身 LCS 中解释,
// 所以需要用组件旋转矩阵的转置(=逆)将绝对位移映射到局部坐标
//------------------------------------------------------------------------------
static void askLocalDisplacement(tag_t occ, const double absDisp[3], double localDisp[3])
{
    char partName[256] = {0}, refsetName[256] = {0}, instName[256] = {0};
    double pos[3], csys[9], xform[4][4];
    if (UF_ASSEM_ask_component_data(occ, partName, refsetName,
                                    instName, pos, csys, xform) == 0)
    {
        // xform 的 3x3 旋转部分: R[i][j] = xform[i][j]
        // localDisp = R^T * absDisp
        for (int i = 0; i < 3; i++)
            localDisp[i] = xform[0][i] * absDisp[0] +
                           xform[1][i] * absDisp[1] +
                           xform[2][i] * absDisp[2];
    }
    else
    {
        // 取不到变换时假设组件无旋转, 直接用绝对位移
        localDisp[0] = absDisp[0];
        localDisp[1] = absDisp[1];
        localDisp[2] = absDisp[2];
    }
}

//------------------------------------------------------------------------------
// 捕捉组件沿指定方向的精确规格尺寸(带全程诊断日志)
// 用 UF_MTX3_initialize_x 构造 X 轴 = dir 的正交矩阵, 创建临时坐标系,
// UF_MODL_ask_bounding_box_exact 求对齐该坐标系的精确包围盒(支持 occurrence,
// 几何变换到装配空间), 得到组件沿移动轴的真实跨度和中心投影
// 返回: centerT = 组件中心沿 dir 的绝对投影, halfW = 沿轴真实半宽
// 失败(含结果异常)时返回 false, 调用方回退 AABB 方法; 每步错误码都记入日志
//------------------------------------------------------------------------------
static bool askExtentAlongDir(tag_t occ, const char *name, const double dir[3],
                              double &centerT, double &halfW)
{
    // 防御: dir 必须有限且非零, 手动归一化(UF_CSYS_create_matrix 要求单位正交矩阵)
    double d[3] = {dir[0], dir[1], dir[2]};
    double len = sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (!(len > 1.0e-12) || !(len == len) || len > 1.0e30)
    {
        print(std::string("  [extent] ") + name + ": bad dir vector -> fallback");
        return false;
    }
    d[0] /= len; d[1] /= len; d[2] /= len;

    // 以 dir 为 X 轴的正交矩阵(行向量约定 [0..2]=X, [3..5]=Y, [6..8]=Z)
    double mtx[9];
    int rc = UF_MTX3_initialize_x(d, mtx);
    if (rc != 0)
    {
        stringstream ss; ss << "  [extent] " << name << ": UF_MTX3_initialize_x rc=" << rc;
        print(ss.str());
        return false;
    }
    tag_t mtxTag = NULL_TAG;
    rc = UF_CSYS_create_matrix(mtx, &mtxTag);
    if (rc != 0)
    {
        stringstream ss; ss << "  [extent] " << name << ": UF_CSYS_create_matrix rc=" << rc;
        print(ss.str());
        return false;
    }
    double csysOrigin[3] = {0.0, 0.0, 0.0};
    tag_t csysTag = NULL_TAG;
    rc = UF_CSYS_create_temp_csys(csysOrigin, mtxTag, &csysTag);
    if (rc != 0)
    {
        stringstream ss; ss << "  [extent] " << name << ": UF_CSYS_create_temp_csys rc=" << rc;
        print(ss.str());
        UF_OBJ_delete_object(mtxTag);
        return false;
    }
    double minCorner[3] = {0.0, 0.0, 0.0};
    double dirs[3][3] = {{0.0}};
    double dists[3] = {0.0, 0.0, 0.0};
    rc = UF_MODL_ask_bounding_box_exact(occ, csysTag, minCorner, dirs, dists);
    UF_OBJ_delete_object(csysTag);
    UF_OBJ_delete_object(mtxTag);
    if (rc != 0)
    {
        stringstream ss; ss << "  [extent] " << name
                           << ": UF_MODL_ask_bounding_box_exact rc=" << rc;
        print(ss.str());
        return false;
    }
    // X 方向 = dir: 沿轴真实跨度 = dists[0];
    // 中心 = minCorner + dirs[0]*(dists[0]/2) + dirs[1]*(dists[1]/2) + dirs[2]*(dists[2]/2),
    // 其沿 dir 的投影 = dot(minCorner, dir) + dists[0]*0.5 (dirs[1]/dirs[2] 与 dir 垂直)
    centerT = minCorner[0] * d[0] + minCorner[1] * d[1] + minCorner[2] * d[2]
              + dists[0] * 0.5;
    halfW = dists[0] * 0.5;
    // 防御校验 1: 结果必须有限、跨度非负(临时 csys 不被采纳时该 API 可能
    // 返回 0 但输出异常值, 若直接采信会导致 transform 含 NaN/巨大值使爆炸失败)
    if (!(centerT == centerT) || !(halfW == halfW) ||
        fabs(centerT) > 1.0e30 || fabs(halfW) > 1.0e30 || halfW < 0.0)
    {
        stringstream ss;
        ss << "  [extent] " << name << ": invalid exact bbox (centerT=" << centerT
           << ", halfW=" << halfW << ") -> fallback";
        print(ss.str());
        return false;
    }
    // 防御校验 2(一致性交叉验证): 精确包围盒沿轴跨度理论上必 <= AABB 沿轴
    // 曼哈顿投影(后者是任意方向跨度的上界); 若超出说明临时坐标系未被采纳
    // 或结果坐标系错乱, 不可信 -> 回退
    double abbox[6];
    if (UF_MODL_ask_bounding_box(occ, abbox) == 0)
    {
        double aabbHalf = ((abbox[3] - abbox[0]) * fabs(d[0]) +
                           (abbox[4] - abbox[1]) * fabs(d[1]) +
                           (abbox[5] - abbox[2]) * fabs(d[2])) * 0.5;
        if (halfW > aabbHalf + 1.0e-6)
        {
            stringstream ss;
            ss << "  [extent] " << name << ": exact halfW " << halfW
               << " exceeds AABB upper bound " << aabbHalf << " -> fallback";
            print(ss.str());
            return false;
        }
    }
    return true;
}

//------------------------------------------------------------------------------
// 按轴向投影分组并逐组件输入目标位置: 正侧向+dir 移动, 负侧向-dir, 原点不动
// 投影 t>=0 归正侧, t<0 归负侧(不跳过任何组件);
// 一次输入框列出所有组件(先正侧后负侧), 默认值按组件沿轴宽度打包累加
// (相邻组件实际间隙 = 自适应 gap, 线度不同的组件互不重叠), 可逐个修改;
// 输入值 = 沿轴目标位置(距分界点), 移动量 = 目标位置 - 原始投影;
// 0 = 不移动; 返回移动数, -1 = 用户取消输入
// keepVals: 持久编辑状态(组件显示名 -> 已确认目标位置绝对值), 可为 NULL;
//   非空时对话框预填该组件上次确认值(无记录的组件用打包默认值), 确认后
//   回写; 状态由调用方持有, 直到用户 Finish 才释放 → 在线反复编辑看效果
//------------------------------------------------------------------------------
struct SpreadItem
{
    tag_t occ;
    double t;      // 相对分界点沿方向轴的投影
    double halfW;  // 组件沿轴方向包围盒半宽(打包布局用)
    std::string name;
};

static bool cmpPosSide(const SpreadItem &a, const SpreadItem &b) { return a.t < b.t; }  // 近→远
static bool cmpNegSide(const SpreadItem &a, const SpreadItem &b) { return a.t > b.t; }  // 近→远(负值大者近)

static int groupAndMove(tag_t explosionTag, const std::vector<tag_t> &selOccs,
                        tag_t originOcc, const double originPos[3],
                        const double dir[3], double minSpacing, const char *dirDesc,
                        std::map<std::string, double> *keepVals)
{
    std::vector<SpreadItem> posSide, negSide;
    double maxWidth = 0.0;  // 所有组件沿轴最大真实跨度(自适应默认间隙用)
    double originDot = originPos[0] * dir[0] + originPos[1] * dir[1] + originPos[2] * dir[2];
    for (size_t i = 0; i < selOccs.size(); i++)
    {
        if (selOccs[i] == originOcc)
            continue;  // 原点组件保持不动

        // 捕捉组件沿移动轴的精确规格尺寸(真实跨度与中心投影)
        std::string compName = getComponentName(selOccs[i]);
        double centerT = 0.0, halfW = 0.0;
        double t = 0.0;
        bool haveExtent = askExtentAlongDir(selOccs[i], compName.c_str(), dir, centerT, halfW);
        if (haveExtent)
        {
            t = centerT - originDot;
        }
        else
        {
            // 回退: 包围盒中心 + AABB 沿轴投影(过估计, V0.2 已验证的行为)
            double pos[3];
            if (!getComponentPosition(selOccs[i], pos))
            {
                stringstream ssSkip;
                ssSkip << "  skip (no geometry/position): " << compName;
                print(ssSkip.str());
                continue;
            }
            t = (pos[0] - originPos[0]) * dir[0] +
                (pos[1] - originPos[1]) * dir[1] +
                (pos[2] - originPos[2]) * dir[2];
            double bbox[6];
            if (UF_MODL_ask_bounding_box(selOccs[i], bbox) == 0)
                halfW = ((bbox[3] - bbox[0]) * fabs(dir[0]) +
                         (bbox[4] - bbox[1]) * fabs(dir[1]) +
                         (bbox[5] - bbox[2]) * fabs(dir[2])) * 0.5;
        }
        double width = halfW * 2.0;
        if (width > maxWidth)
            maxWidth = width;

        SpreadItem it = {selOccs[i], t, halfW, compName};
        if (t >= 0.0) posSide.push_back(it);
        else          negSide.push_back(it);
    }

    // 每侧按离原点由近到远排序
    std::sort(posSide.begin(), posSide.end(), cmpPosSide);
    std::sort(negSide.begin(), negSide.end(), cmpNegSide);

    size_t nPos = posSide.size(), nNeg = negSide.size();
    size_t nTotal = nPos + nNeg;
    {
        stringstream ssGrp;
        ssGrp << "Grouped: +side = " << nPos << ", -side = " << nNeg
              << ", total to move = " << nTotal;
        print(ssGrp.str());
    }
    if (nTotal == 0)
    {
        print("No component to move (nothing selected except origin).");
        return 0;
    }

    // 打印分组预览(含每个组件沿移动轴捕捉到的真实规格尺寸)
    print("Grouping along the axis (measured exact size along axis):");
    if (nPos > 0)
    {
        print("  +side (move along +dir):");
        for (size_t k = 0; k < nPos; k++)
        {
            stringstream ssPrev;
            ssPrev << "    " << posSide[k].name
                   << "  [size along axis = " << posSide[k].halfW * 2.0
                   << ", pos = " << posSide[k].t << "]";
            print(ssPrev.str());
        }
    }
    if (nNeg > 0)
    {
        print("  -side (move along -dir):");
        for (size_t k = 0; k < nNeg; k++)
        {
            stringstream ssPrev;
            ssPrev << "    " << negSide[k].name
                   << "  [size along axis = " << negSide[k].halfW * 2.0
                   << ", pos = " << negSide[k].t << "]";
            print(ssPrev.str());
        }
    }
    // 自适应默认间隙: 所有组件沿轴最大真实跨度 x 0.3, 下限 = 默认距离 x 0.4
    // (目标位置按组件实际尺寸打包累加, 相邻组件实际间隙相同且互不重叠)
    double gap = maxWidth * 0.3;
    double gapMin = minSpacing * 0.4;
    if (gap < gapMin) gap = gapMin;
    {
        stringstream ssSp;
        ssSp << "Packed target positions: uniform gap = " << gap
             << " (max component width along axis x 0.3, min " << gapMin << ")";
        print(ssSp.str());
    }
    print("Enter target position along the axis per component (0 = no move).");
    print("Default: components packed by their widths, adjacent gap uniform");
    print("         (widths respected, no overlap even for different lengths).");

    // 原点组件沿轴半宽(无原点时为 0, 正负侧第一个组件的打包起点)
    double edgeHalf = 0.0;
    if (originOcc != NULL_TAG)
    {
        std::string oname = getComponentName(originOcc);
        double ocT = 0.0;
        if (!askExtentAlongDir(originOcc, oname.c_str(), dir, ocT, edgeHalf))
        {
            // 回退: AABB 沿轴投影
            double obbox[6];
            if (UF_MODL_ask_bounding_box(originOcc, obbox) == 0)
                edgeHalf = ((obbox[3] - obbox[0]) * fabs(dir[0]) +
                            (obbox[4] - obbox[1]) * fabs(dir[1]) +
                            (obbox[5] - obbox[2]) * fabs(dir[2])) * 0.5;
        }
    }
    {
        stringstream ssEdge;
        ssEdge << "Origin edge half-width along axis = " << edgeHalf;
        print(ssEdge.str());
    }

    // 一次输入所有组件的目标位置(先正侧后负侧), 默认值按宽度打包累加
    char (*itemNames)[16] = new char[nTotal][16];
    memset(itemNames, 0, nTotal * 16);
    double *vals = new double[nTotal];
    double prevEdge = edgeHalf + gap;
    for (size_t k = 0; k < nPos; k++)
    {
        strncpy(itemNames[k], posSide[k].name.c_str(), 15);
        vals[k] = prevEdge + posSide[k].halfW;
        prevEdge = vals[k] + posSide[k].halfW + gap;
    }
    prevEdge = edgeHalf + gap;
    for (size_t k = 0; k < nNeg; k++)
    {
        strncpy(itemNames[nPos + k], negSide[k].name.c_str(), 15);
        vals[nPos + k] = prevEdge + negSide[k].halfW;
        prevEdge = vals[nPos + k] + negSide[k].halfW + gap;
    }
    
    // 持久编辑: 预填上次确认值(按组件名记忆), 换轴/修改后在线继续编辑
    if (keepVals != NULL)
    {
        int reused = 0;
        for (size_t k = 0; k < nPos; k++)
        {
            std::map<std::string, double>::const_iterator f = keepVals->find(posSide[k].name);
            if (f != keepVals->end()) { vals[k] = f->second; reused++; }
        }
        for (size_t k = 0; k < nNeg; k++)
        {
            std::map<std::string, double>::const_iterator f = keepVals->find(negSide[k].name);
            if (f != keepVals->end()) { vals[nPos + k] = f->second; reused++; }
        }
        if (reused > 0)
        {
            stringstream ssKeep;
            ssKeep << "Persistent edit: pre-filled last confirmed values for "
                   << reused << " component(s); released only at Finish.";
            print(ssKeep.str());
        }
    }
    
    int ip5 = 0;
    int inputResp = uc1609("Enter target position along axis per component (first +side, then -side)", itemNames, (int)nTotal, vals, &ip5);
    {
        stringstream ssIR;
        ssIR << "uc1609 response = " << inputResp << " (>=3 = confirmed, <3 = cancel)";
        print(ssIR.str());
    }
    if (inputResp < 3)  // Back/Cancel
    {
        delete[] itemNames;
        delete[] vals;
        return -1;
    }
    
    // 回写持久编辑状态(已确认的值保留, 直到 Finish 才释放)
    if (keepVals != NULL)
    {
        for (size_t k = 0; k < nPos; k++)
            (*keepVals)[posSide[k].name] = vals[k];
        for (size_t k = 0; k < nNeg; k++)
            (*keepVals)[negSide[k].name] = vals[nPos + k];
    }
    
    // 应用: 每个组件移动到沿轴的目标位置(target), 移动量 = target - 原始投影 t
    // 正侧 target = +输入值, 负侧 target = -输入值; 默认按宽度打包 → 间隙均匀
    // 关键: UF_ASSEM_explode_component 的 transform 平移列在组件自身局部坐标
    // 系(LCS)中解释, 必须将绝对位移转换到每个组件的 LCS 中
    int moved = 0;
    for (size_t k = 0; k < nPos; k++)
    {
        double target = vals[k];
        if (fabs(target) < 1.0e-9)
        {
            print("  " + posSide[k].name + ": target=0 -> no move");
            continue;  // 0 = 不移动
        }
        double deltaT = target - posSide[k].t;
        if (fabs(deltaT) < 1.0e-9)
        {
            print("  " + posSide[k].name + ": already at target -> no move");
            continue;  // 已在目标位置
        }
        double absDisp[3] = { dir[0] * deltaT, dir[1] * deltaT, dir[2] * deltaT };
        double localDisp[3];
        askLocalDisplacement(posSide[k].occ, absDisp, localDisp);
        double transform[4][4] = {
            {1.0, 0.0, 0.0, localDisp[0]},
            {0.0, 1.0, 0.0, localDisp[1]},
            {0.0, 0.0, 1.0, localDisp[2]},
            {0.0, 0.0, 0.0, 1.0}
        };
        int xrc = UF_ASSEM_explode_component(explosionTag, posSide[k].occ, transform);
        if (xrc == 0)
        {
            moved++;
            stringstream ssItem;
            ssItem << "  " << posSide[k].name << " | +side | t=" << posSide[k].t
                   << " target=" << target << " move " << deltaT;
            print(ssItem.str());
        }
        else
        {
            stringstream ssErr;
            ssErr << "  ERROR: UF_ASSEM_explode_component rc=" << xrc
                  << " for " << posSide[k].name;
            print(ssErr.str());
        }
    }
    for (size_t k = 0; k < nNeg; k++)
    {
        double target = -vals[nPos + k];  // 负侧目标位置为负
        if (fabs(vals[nPos + k]) < 1.0e-9)
        {
            print("  " + negSide[k].name + ": target=0 -> no move");
            continue;  // 0 = 不移动
        }
        double deltaT = target - negSide[k].t;
        if (fabs(deltaT) < 1.0e-9)
        {
            print("  " + negSide[k].name + ": already at target -> no move");
            continue;  // 已在目标位置
        }
        double absDisp[3] = { dir[0] * deltaT, dir[1] * deltaT, dir[2] * deltaT };
        double localDisp[3];
        askLocalDisplacement(negSide[k].occ, absDisp, localDisp);
        double transform[4][4] = {
            {1.0, 0.0, 0.0, localDisp[0]},
            {0.0, 1.0, 0.0, localDisp[1]},
            {0.0, 0.0, 1.0, localDisp[2]},
            {0.0, 0.0, 0.0, 1.0}
        };
        int xrc = UF_ASSEM_explode_component(explosionTag, negSide[k].occ, transform);
        if (xrc == 0)
        {
            moved++;
            stringstream ssItem;
            ssItem << "  " << negSide[k].name << " | -side | t=" << negSide[k].t
                   << " target=" << target << " move " << deltaT;
            print(ssItem.str());
        }
        else
        {
            stringstream ssErr;
            ssErr << "  ERROR: UF_ASSEM_explode_component rc=" << xrc
                  << " for " << negSide[k].name;
            print(ssErr.str());
        }
    }

    // 重叠检测: 按确认后的目标位置检查每侧相邻组件沿轴区间是否重叠
    // (默认打包值不重叠; 用户改过输入值可能重叠, 打印警告)
    for (size_t k = 1; k < nPos; k++)
    {
        double gapK = vals[k] - vals[k - 1] - posSide[k].halfW - posSide[k - 1].halfW;
        if (gapK < -1.0e-6)
        {
            stringstream ssOv;
            ssOv << "  WARNING: +side overlap! " << posSide[k - 1].name << " and "
                 << posSide[k].name << " overlap by " << -gapK
                 << " (targets " << vals[k - 1] << " / " << vals[k] << ")";
            print(ssOv.str());
        }
    }
    for (size_t k = 1; k < nNeg; k++)
    {
        double gapK = vals[nPos + k] - vals[nPos + k - 1]
                      - negSide[k].halfW - negSide[k - 1].halfW;
        if (gapK < -1.0e-6)
        {
            stringstream ssOv;
            ssOv << "  WARNING: -side overlap! " << negSide[k - 1].name << " and "
                 << negSide[k].name << " overlap by " << -gapK
                 << " (targets " << vals[nPos + k - 1] << " / " << vals[nPos + k] << ")";
            print(ssOv.str());
        }
    }

    delete[] itemNames;
    delete[] vals;

    stringstream ss;
    ss << "Spread-out: dir " << dirDesc << " (" << dir[0] << ", " << dir[1]
       << ", " << dir[2] << "), moved " << moved << " / " << nTotal;
    print(ss.str());
    return moved;
}

//------------------------------------------------------------------------------
// 递归收集装配中的所有叶子组件(无子组件的组件)
//------------------------------------------------------------------------------
static void collectLeafComponents(NXOpen::Assemblies::Component *comp,
                                  std::vector<NXOpen::Assemblies::Component *> &out)
{
    std::vector<NXOpen::Assemblies::Component *> children = comp->GetChildren();
    for (size_t i = 0; i < children.size(); i++)
    {
        std::vector<NXOpen::Assemblies::Component *> sub = children[i]->GetChildren();
        if (sub.empty())
            out.push_back(children[i]);
        else
            collectLeafComponents(children[i], out);
    }
}

//------------------------------------------------------------------------------
// 爆炸参数项: 组件显示名 + 爆炸位移向量
//------------------------------------------------------------------------------
struct ExplodeParam
{
    std::string displayName;
    double dx, dy, dz;
};

//------------------------------------------------------------------------------
// 读取本 dll 同目录下的 explosion_params.txt 参数文件
// (由 提取爆炸参数.vb 生成, 每行格式: 组件显示名|dx|dy|dz)
// 读取成功返回 true; 文件不存在或为空返回 false
//------------------------------------------------------------------------------
static bool loadExplodeParams(std::vector<ExplodeParam> &params)
{
    HMODULE hMod = NULL;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCSTR>(&loadExplodeParams), &hMod))
        return false;

    char dllPath[MAX_PATH] = {0};
    if (!GetModuleFileNameA(hMod, dllPath, MAX_PATH))
        return false;

    std::string dir(dllPath);
    size_t pos = dir.find_last_of("\\/");
    if (pos == std::string::npos)
        return false;
    std::string paramPath = dir.substr(0, pos + 1) + "explosion_params.txt";

    std::ifstream in(paramPath.c_str());
    if (!in.is_open())
        return false;

    std::string line;
    while (std::getline(in, line))
    {
        if (line.empty())
            continue;
        size_t p1 = line.find('|');
        if (p1 == std::string::npos)
            continue;
        size_t p2 = line.find('|', p1 + 1);
        if (p2 == std::string::npos)
            continue;
        size_t p3 = line.find('|', p2 + 1);
        if (p3 == std::string::npos)
            continue;

        ExplodeParam p;
        p.displayName = line.substr(0, p1);
        p.dx = atof(line.substr(p1 + 1, p2 - p1 - 1).c_str());
        p.dy = atof(line.substr(p2 + 1, p3 - p2 - 1).c_str());
        p.dz = atof(line.substr(p3 + 1).c_str());
        params.push_back(p);
    }
    return !params.empty();
}


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

private:
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
// Step 1: 爆炸图视图创建
//------------------------------------------------------------------------------
void MyClass::do_it()
{
	// 获取工作部件
	Part *workPart = dynamic_cast<Part *>(theSession->Parts()->Work());
	if (workPart == NULL)
	{
		print("Error: No work part, cannot create explosion.");
		return;
	}

	// 爆炸图只能建立在装配上
	Assemblies::ComponentAssembly *assembly = workPart->ComponentAssembly();
	if (assembly == NULL)
	{
		print("Error: Work part is not an assembly, cannot create explosion.");
		return;
	}

	ModelingView *workView = workPart->ModelingViews()->WorkView();
	if (workView == NULL)
	{
		print("Error: Cannot get work view.");
		return;
	}

	// ------------------------------------------------------------------
	// 步骤1: 新建爆炸图 (录制: Explosions.Create("Explosion 1"))
	// 同名爆炸图已存在时复用, 保证脚本可重复执行
	// ------------------------------------------------------------------
	theSession->SetUndoMark(Session::MarkVisibilityVisible, "New Explosion");

	Assemblies::Explosion *explosion = NULL;
	for (Assemblies::ExplosionCollection::iterator it = assembly->Explosions()->begin();
	     it != assembly->Explosions()->end(); ++it)
	{
		if (strcmp((*it)->Name().GetText(), "Explosion 1") == 0)
		{
			explosion = *it;
			break;
		}
	}

	if (explosion == NULL)
	{
		explosion = assembly->Explosions()->Create("Explosion 1");
		print("Created explosion: Explosion 1");
	}
	else
	{
		print("Explosion \"Explosion 1\" already exists, reuse it.");
	}

	// ------------------------------------------------------------------
	// 步骤2: 爆炸组件 (录制: 菜单 自动爆炸组件(A)/编辑爆炸(E), 手动移动)
	// 录制器未记录手动拖拽数据, 自动化策略:
	//   优先读取 dll 同目录 explosion_params.txt 参数文件精确重放;
	//   无参数文件时"左右散开"交互模式:
	//     多选参与组件 → 单选原点/基准件 → 指定方向轴 → 输左右距离
	//     → 按组件相对原点的轴向投影符号分组, 正侧向正方向、
	//       负侧向负方向散开, 原点组件保持不动
	// ------------------------------------------------------------------
	theSession->SetUndoMark(Session::MarkVisibilityVisible, "Edit Explosion");

	{
		std::vector<ExplodeParam> params;
		bool hasParams = loadExplodeParams(params);

		if (hasParams)
		{
			// 策略1: 按参数文件精确重放手动爆炸
			std::vector<Assemblies::Component *> leaves;
			collectLeafComponents(assembly->RootComponent(), leaves);
			int explodedCount = 0;
			for (size_t i = 0; i < leaves.size(); i++)
			{
				const char *dn = leaves[i]->DisplayName().GetText();
				for (size_t j = 0; j < params.size(); j++)
				{
					if (strcmp(dn, params[j].displayName.c_str()) == 0)
					{
						double transform[4][4] = {
							{1.0, 0.0, 0.0, params[j].dx},
							{0.0, 1.0, 0.0, params[j].dy},
							{0.0, 0.0, 1.0, params[j].dz},
							{0.0, 0.0, 0.0, 1.0}
						};
						if (UF_ASSEM_explode_component(explosion->Tag(),
						                               leaves[i]->Tag(), transform) == 0)
							explodedCount++;
						break;
					}
				}
			}

			stringstream ssParams;
			ssParams << "Exploded from params file (" << params.size()
			          << " entries), components exploded: " << explodedCount;
			print(ssParams.str());
		}
		else
		{
			// 策略2: "左右散开"交互模式
			tag_t rootOcc = assembly->RootComponent()->Tag();

			// 2.1 一次性选择所有参与爆炸的组件(可框选/多选)
			int resp = 0, count = 0;
			tag_p_t objs = NULL;
			UF_UI_select_with_class_dialog("Select all components to explode (OK to confirm)", "Spread Out - Select Components",
			                                UF_UI_SEL_SCOPE_WORK_PART_AND_OCC,
			                                selInitComponent, NULL,
			                                &resp, &count, &objs);
			if (resp != UF_UI_OK || count <= 0)
			{
				print("No components selected, spread-out cancelled.");
			}
			else
			{
				// 归一化为 part occurrence tag
				std::vector<tag_t> selOccs;
				for (int i = 0; i < count; i++)
				{
					int type, subtype;
					if (UF_OBJ_ask_type_and_subtype(objs[i], &type, &subtype) == 0 &&
					    type == UF_component_type)
						selOccs.push_back(toPartOccurrence(rootOcc, objs[i]));
				}
				UF_free(objs);

				if (selOccs.empty())
				{
					print("Selected objects contain no components, cancelled.");
				}
				else
				{
					// 子装配提示: 被选中的子装配将整体移动
					{
						NXOpen::Assemblies::Component *rootComp = assembly->RootComponent();
						for (size_t i = 0; i < selOccs.size(); i++)
						{
							NXOpen::Assemblies::Component *c = findComponentByTag(rootComp, selOccs[i]);
							if (c != NULL && !c->GetChildren().empty())
							{
								stringstream ssSub;
								ssSub << "Note: \"" << c->DisplayName().GetText()
								       << "\" is a sub-assembly, it will move as a whole.";
								print(ssSub.str());
							}
						}
					}

					// 2.2 原点/基准件(可选): 菜单选择是否指定一个保持不动的组件
					//     不指定时用 WCS 原点作为分界参考点, 所有选中组件都参与散开
					const char originItems[][38] = {
						"Pick origin component (stays)",
						"No origin (all spread out)",
						"Cancel"
					};
					int originChoice = uc1603("Spread-out reference:", 3, originItems, 3);
					tag_t originOcc = NULL_TAG;
					double originPos[3] = {0.0, 0.0, 0.0};
					bool haveOriginPos = false;

					if (originChoice == 1)
					{
						// 类选择器单选(与第一步相同, 已验证可靠)
						int resp2 = 0, count2 = 0;
						tag_p_t objs2 = NULL;
						UF_UI_select_with_class_dialog("Select ONE origin/reference component (it stays in place)", "Spread Out - Origin Component",
						                                UF_UI_SEL_SCOPE_WORK_PART_AND_OCC,
						                                selInitComponent, NULL,
						                                &resp2, &count2, &objs2);
						bool picked = (resp2 == UF_UI_OK && count2 >= 1 && objs2 != NULL);
						if (count2 > 1)
							print("More than one component selected, using the first as origin.");
						tag_t originSel = picked ? objs2[0] : NULL_TAG;
						if (objs2 != NULL)
							UF_free(objs2);
						if (picked)
						{
							originOcc = toPartOccurrence(rootOcc, originSel);
							haveOriginPos = getComponentPosition(originOcc, originPos);
							if (!haveOriginPos)
								print("Error: Cannot get position of origin component.");
						}
						else
						{
							print("No component picked, fall back to WCS origin as reference.");
						}
					}
					else if (originChoice == 2)
					{
						print("No origin component: all selected components spread out.");
					}
					else
					{
						print("Spread-out cancelled.");
					}

					if (!haveOriginPos)
					{
						// 无原点组件(未选/未选中): 用 WCS 原点作为分界参考点
						double wcsOrigin[3], wcsMtx[9];
						if (getWcsInfo(wcsOrigin, wcsMtx))
						{
							originPos[0] = wcsOrigin[0];
							originPos[1] = wcsOrigin[1];
							originPos[2] = wcsOrigin[2];
							haveOriginPos = true;
							print("Reference point: WCS origin (all components participate).");
						}
						else
						{
							print("Error: WCS unavailable, spread-out cancelled.");
						}
					}

					if (haveOriginPos)
					{
							// 2.3 自适应轴长(基于选中组件包围盒)
							double diag = 300.0;
							getAssemblyExtent(selOccs, diag);
							double axisLen = (diag * 0.12 > 50.0) ? diag * 0.12 : 50.0;

							// 2.4 交互主循环: 选轴 → 逐组件输距离 → 完成
							// 持久编辑状态: 组件名 -> 已确认目标距离;
							// 修改/换轴后对话框预填上次值, 直到 Finish 才释放
							std::map<std::string, double> editVals;
							double dir[3] = {0.0, 0.0, 1.0};
							std::string dirDesc;
							bool haveDir = false;
							while (true)
							{
								if (!haveDir)
								{
									if (!askDirection(originPos, axisLen, dir, dirDesc))
										break;  // 取消选轴
									haveDir = true;
								}

								// 分组并逐组件输入沿轴目标位置(默认均匀等距, 可逐个修改)
								int moved = groupAndMove(explosion->Tag(), selOccs, originOcc,
								                         originPos, dir, kExplodeDistance, dirDesc.c_str(),
								                         &editVals);
								// moved < 0 = 输入框取消: 编辑状态不释放, 落入下一步菜单

								if (moved == 0)
								{
									// 所有组件都已在其目标位置, 提示并重新选轴
									print("Nothing moved (all components are already at their targets). Re-picking axis...");
									haveDir = false;
									continue;
								}

								// 下一步: 修改目标位置 / 换轴(均预填上次值) / 完成
								const char nextItems[][38] = {
									"Modify target positions (same axis)",
									"Re-pick axis",
									"Finish"
								};
								int next = uc1603("Spread-out done. What next?", 3, nextItems, 3);
								if (next == 1)
								{
									// 同轴, 重新输入所有组件目标位置
								}
								else if (next == 2)
								{
									haveDir = false;  // 重新选轴
								}
								else
								{
									break;  // Finish / 取消
								}
							}
						}
					}
				}
			}
			print("Spread-out interaction finished.");
		}

	// ------------------------------------------------------------------
	// 步骤3: 在工作视图中显示爆炸 (录制: explosion.Show(WorkView))
	// ------------------------------------------------------------------
	explosion->Show(workView);
	print("Explosion shown in work view.");

	// ------------------------------------------------------------------
	// 步骤4: 视图另存为 "爆炸图01" (录制: SaveAsPreservingCase)
	// 同名视图已存在时复用并刷新其爆炸显示
	// ------------------------------------------------------------------
	theSession->SetUndoMark(Session::MarkVisibilityVisible, "Save As");

	View *savedView = NULL;
	for (ViewCollection::iterator it = workPart->Views()->begin();
	     it != workPart->Views()->end(); ++it)
	{
		if (strcmp((*it)->Name().GetText(), "ExplosionView01") == 0)
		{
			savedView = *it;
			break;
		}
	}

	if (savedView == NULL)
	{
		savedView = workPart->Views()->SaveAsPreservingCase(workView, "ExplosionView01", false, false);
		print("Saved explosion view: ExplosionView01");
	}
	else
	{
		// 复用已有视图, 将最新爆炸状态应用到该视图
		explosion->Show(savedView);
		print("View \"ExplosionView01\" already exists, reuse and refresh.");
	}

	print("Step 1 explosion view creation done.");
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
