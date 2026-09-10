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
#include <array>
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
// ExplosionConfig -- 爆炸视图参数配置结构体
//------------------------------------------------------------------------------
/**
 * @brief 爆炸视图参数配置结构体
 * @details 封装所有可调爆炸参数，默认值精确复现当前硬编码行为。
 *          config_version 字段用于未来参数结构变更时的版本追踪与向后兼容。
 * @version 1
 */
struct ExplosionConfig
{
    int config_version;            ///< 配置版本号（当前: 1）

    // -- 间隙/距离控制 --
    double gapScaleFactor;         ///< 间隙缩放系数（1.0 = 当前行为，>1 增大间隙）
    double minGapOverride;         ///< 最小安全间隙覆盖值（<=0 表示使用默认 minSpacing*0.4）
    double posSideScaleFactor;     ///< 正侧独立缩放系数（<=0 表示使用 gapScaleFactor）
    double negSideScaleFactor;     ///< 负侧独立缩放系数（<=0 表示使用 gapScaleFactor）

    // -- 方向与参考 --
    double direction[3];           ///< 爆炸方向向量（0,0,0 = 交互选择）
    bool hasCustomDirection;       ///< 是否使用自定义方向（false = 弹出矢量对话框）
    std::string referenceCompName; ///< 基准组件显示名（空 = 交互选择）

    // -- 诊断 --
    bool enableDiagnostics;        ///< 启用详细诊断日志

    /// 默认构造函数：精确复现当前硬编码行为
    ExplosionConfig()
        : config_version(1)
        , gapScaleFactor(1.0)
        , minGapOverride(0.0)
        , posSideScaleFactor(0.0)
        , negSideScaleFactor(0.0)
        , direction{0.0, 0.0, 0.0}
        , hasCustomDirection(false)
        , referenceCompName()
        , enableDiagnostics(false)
    {}

    /// 获取正侧有效缩放系数
    double effectivePosScale() const {
        return (posSideScaleFactor > 0.0) ? posSideScaleFactor : gapScaleFactor;
    }
    /// 获取负侧有效缩放系数
    double effectiveNegScale() const {
        return (negSideScaleFactor > 0.0) ? negSideScaleFactor : gapScaleFactor;
    }

    /// 保存配置到 INI 风格文件（指定预设 section）
    bool saveToFile(const std::string &path, const std::string &presetName) const;
    /// 从 INI 风格文件加载指定预设（含版本兼容 + 旧格式向后兼容）
    static bool loadFromFile(const std::string &path, const std::string &presetName, ExplosionConfig &cfg);
    /// 列出文件中所有预设名
    static std::vector<std::string> listPresets(const std::string &path);
};

//------------------------------------------------------------------------------
// getDllDirectory -- 获取 DLL 所在目录路径
//------------------------------------------------------------------------------
/**
 * @brief 获取 DLL 所在目录路径
 * @return DLL 目录字符串（含末尾反斜杠）
 */
static std::string getDllDirectory()
{
    HMODULE hMod = NULL;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCSTR>(&getDllDirectory), &hMod))
        return "";
    char dllPath[MAX_PATH] = {0};
    if (!GetModuleFileNameA(hMod, dllPath, MAX_PATH))
        return "";
    std::string dir(dllPath);
    size_t pos = dir.find_last_of("\\/");
    return (pos != std::string::npos) ? dir.substr(0, pos + 1) : "";
}

//------------------------------------------------------------------------------
// ExplosionConfig::saveToFile
//------------------------------------------------------------------------------
/**
 * @brief 保存配置到 INI 风格文件的指定预设 section
 * @details 若文件已存在，保留其他 section 并覆盖/新建目标 section；
 *          若文件不存在，创建新文件并写入目标 section。
 * @param path 配置文件路径
 * @param presetName 预设名（用作 section 名，如 "Preset_Default"）
 */
bool ExplosionConfig::saveToFile(const std::string &path, const std::string &presetName) const
{
    std::string sectionHeader = "[" + presetName + "]";

    // 读取现有文件内容（按 section 拆分）
    std::vector<std::string> existingLines;
    {
        std::ifstream fin(path.c_str());
        if (fin.is_open())
        {
            std::string ln;
            while (std::getline(fin, ln))
            {
                if (!ln.empty() && ln.back() == '\r') ln.pop_back();
                existingLines.push_back(ln);
            }
            fin.close();
        }
    }

    // 构建新 section 内容
    std::vector<std::string> newSection;
    newSection.push_back(sectionHeader);
    {
        std::ostringstream os;
        os << "config_version=" << config_version; newSection.push_back(os.str());
    }
    {
        std::ostringstream os;
        os << "gapScaleFactor=" << gapScaleFactor; newSection.push_back(os.str());
    }
    {
        std::ostringstream os;
        os << "minGapOverride=" << minGapOverride; newSection.push_back(os.str());
    }
    {
        std::ostringstream os;
        os << "posSideScaleFactor=" << posSideScaleFactor; newSection.push_back(os.str());
    }
    {
        std::ostringstream os;
        os << "negSideScaleFactor=" << negSideScaleFactor; newSection.push_back(os.str());
    }
    {
        std::ostringstream os;
        os << "direction_x=" << direction[0]; newSection.push_back(os.str());
    }
    {
        std::ostringstream os;
        os << "direction_y=" << direction[1]; newSection.push_back(os.str());
    }
    {
        std::ostringstream os;
        os << "direction_z=" << direction[2]; newSection.push_back(os.str());
    }
    {
        std::ostringstream os;
        os << "hasCustomDirection=" << (hasCustomDirection ? 1 : 0); newSection.push_back(os.str());
    }
    {
        std::ostringstream os;
        os << "referenceCompName=" << referenceCompName; newSection.push_back(os.str());
    }
    {
        std::ostringstream os;
        os << "enableDiagnostics=" << (enableDiagnostics ? 1 : 0); newSection.push_back(os.str());
    }

    // 查找目标 section 在现有内容中的行范围
    int sectionStart = -1, sectionEnd = -1;
    for (int i = 0; i < (int)existingLines.size(); i++)
    {
        const std::string &ln = existingLines[i];
        if (ln.size() >= 2 && ln[0] == '[')
        {
            if (sectionStart >= 0 && sectionEnd < 0)
                sectionEnd = i; // 下一个 section 开始
            if (ln == sectionHeader)
                sectionStart = i;
        }
    }
    if (sectionStart >= 0 && sectionEnd < 0)
        sectionEnd = (int)existingLines.size();

    // 重写文件
    std::ofstream f(path.c_str(), std::ios::out | std::ios::trunc);
    if (!f.is_open()) return false;

    if (sectionStart >= 0)
    {
        // 写入目标 section 之前的行
        for (int i = 0; i < sectionStart; i++)
            f << existingLines[i] << "\r\n";
        // 写入新 section
        for (int i = 0; i < (int)newSection.size(); i++)
            f << newSection[i] << "\r\n";
        // 写入目标 section 之后的行
        for (int i = sectionEnd; i < (int)existingLines.size(); i++)
            f << existingLines[i] << "\r\n";
    }
    else
    {
        // 新 section：保留所有现有内容，追加到末尾
        for (int i = 0; i < (int)existingLines.size(); i++)
            f << existingLines[i] << "\r\n";
        if (!existingLines.empty())
            f << "\r\n"; // 空行分隔
        for (int i = 0; i < (int)newSection.size(); i++)
            f << newSection[i] << "\r\n";
    }

    f.close();
    return true;
}

//------------------------------------------------------------------------------
// ExplosionConfig::loadFromFile
//------------------------------------------------------------------------------
/**
 * @brief 从 INI 风格文件加载指定预设（含版本兼容 + 旧格式向后兼容）
 * @details 优先查找 [presetName] section；若未找到，回退查找旧格式
 *          [ExplosionConfig] section 以实现向后兼容。
 * @param path 配置文件路径
 * @param presetName 预设名（section 名）
 * @param cfg 输出配置
 */
bool ExplosionConfig::loadFromFile(const std::string &path, const std::string &presetName, ExplosionConfig &cfg)
{
    std::ifstream f(path.c_str());
    if (!f.is_open()) return false;

    std::string targetHeader = "[" + presetName + "]";
    ExplosionConfig loaded;
    std::string line;
    bool inSection = false;
    bool foundTarget = false;
    int fileVersion = 0;

    // 第一遍：尝试查找目标 presetName section
    while (std::getline(f, line))
    {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.empty() || line[0] == ';' || line[0] == '#')
            continue;
        if (line[0] == '[')
        {
            if (inSection) break; // 已找到并读完目标 section，遇到下一个 section 时退出
            inSection = (line == targetHeader);
            foundTarget = foundTarget || inSection;
            continue;
        }
        if (!inSection) continue;

        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);

        if      (key == "config_version")      { loaded.config_version = atoi(val.c_str()); fileVersion = loaded.config_version; }
        else if (key == "gapScaleFactor")       loaded.gapScaleFactor = atof(val.c_str());
        else if (key == "minGapOverride")       loaded.minGapOverride = atof(val.c_str());
        else if (key == "posSideScaleFactor")   loaded.posSideScaleFactor = atof(val.c_str());
        else if (key == "negSideScaleFactor")   loaded.negSideScaleFactor = atof(val.c_str());
        else if (key == "direction_x")          loaded.direction[0] = atof(val.c_str());
        else if (key == "direction_y")          loaded.direction[1] = atof(val.c_str());
        else if (key == "direction_z")          loaded.direction[2] = atof(val.c_str());
        else if (key == "hasCustomDirection")   loaded.hasCustomDirection = (atoi(val.c_str()) != 0);
        else if (key == "referenceCompName")    loaded.referenceCompName = val;
        else if (key == "enableDiagnostics")    loaded.enableDiagnostics = (atoi(val.c_str()) != 0);
    }

    if (foundTarget && fileVersion >= 1)
    {
        cfg = loaded;
        return true;
    }

    // 第二遍：向后兼容旧格式 [ExplosionConfig] section
    f.clear();
    f.seekg(0);
    loaded = ExplosionConfig();
    inSection = false;
    fileVersion = 0;

    while (std::getline(f, line))
    {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.empty() || line[0] == ';' || line[0] == '#')
            continue;
        if (line[0] == '[')
        {
            if (inSection) break;
            inSection = (line == "[ExplosionConfig]");
            continue;
        }
        if (!inSection) continue;

        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);

        if      (key == "config_version")      { loaded.config_version = atoi(val.c_str()); fileVersion = loaded.config_version; }
        else if (key == "gapScaleFactor")       loaded.gapScaleFactor = atof(val.c_str());
        else if (key == "minGapOverride")       loaded.minGapOverride = atof(val.c_str());
        else if (key == "posSideScaleFactor")   loaded.posSideScaleFactor = atof(val.c_str());
        else if (key == "negSideScaleFactor")   loaded.negSideScaleFactor = atof(val.c_str());
        else if (key == "direction_x")          loaded.direction[0] = atof(val.c_str());
        else if (key == "direction_y")          loaded.direction[1] = atof(val.c_str());
        else if (key == "direction_z")          loaded.direction[2] = atof(val.c_str());
        else if (key == "hasCustomDirection")   loaded.hasCustomDirection = (atoi(val.c_str()) != 0);
        else if (key == "referenceCompName")    loaded.referenceCompName = val;
        else if (key == "enableDiagnostics")    loaded.enableDiagnostics = (atoi(val.c_str()) != 0);
    }

    if (fileVersion < 1)
        return false;

    cfg = loaded;
    return true;
}

//------------------------------------------------------------------------------
// ExplosionConfig::listPresets
//------------------------------------------------------------------------------
/**
 * @brief 列出配置文件中所有预设名（section 名）
 * @details 扫描文件中所有 [xxx] 行，提取 section 名。
 *          旧格式 [ExplosionConfig] 也会被列出。
 * @param path 配置文件路径
 * @return 预设名列表
 */
std::vector<std::string> ExplosionConfig::listPresets(const std::string &path)
{
    std::vector<std::string> presets;
    std::ifstream f(path.c_str());
    if (!f.is_open()) return presets;

    std::string line;
    while (std::getline(f, line))
    {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.empty() || line[0] != '[') continue;
        size_t end = line.find(']');
        if (end == std::string::npos) continue;
        std::string name = line.substr(1, end - 1);
        if (!name.empty())
            presets.push_back(name);
    }
    return presets;
}

//==============================================================================
// Anonymous namespace: internal helper functions & types
//==============================================================================
namespace {

/** @brief 全局打印: 输出到 NX 信息窗口 */
void print(const std::string &msg)
{
    NXOpen::ListingWindow *lw = NXOpen::Session::GetSession()->ListingWindow();
    if (!lw->IsOpen()) lw->Open();
    lw->WriteLine(msg);
}

void print(const char *msg)
{
    NXOpen::ListingWindow *lw = NXOpen::Session::GetSession()->ListingWindow();
    if (!lw->IsOpen()) lw->Open();
    lw->WriteLine(msg);
}

/** @brief 类选择器回调: 仅允许选择组件 */
int selInitComponent(UF_UI_selection_p_t select, void *userData)
{
    UF_UI_mask_t mask;
    mask.object_type = UF_component_type;
    mask.object_subtype = UF_all_subtype;
    mask.solid_type = 0;
    UF_UI_set_sel_mask(select, UF_UI_SEL_MASK_CLEAR_AND_ENABLE_SPECIFIC, 1, &mask);
    return UF_UI_SEL_SUCCESS;
}

/** @brief 将组件 tag 归一化为 part occurrence tag */
tag_t toPartOccurrence(tag_t rootOcc, tag_t t)
{
    if (UF_ASSEM_is_occurrence(t))
        return t;
    tag_t occ = UF_ASSEM_ask_part_occ_of_inst(rootOcc, t);
    return (occ == NULL_TAG) ? t : occ;
}

/** @brief 获取对象(支持 occurrence)的绝对坐标包围盒中心 */
bool getGeometricCenter(tag_t objTag, double center[3])
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

/** @brief 获取组件绝对位置: 优先几何中心, 失败回退组件原点 */
bool getComponentPosition(tag_t occ, double pos[3])
{
    if (getGeometricCenter(occ, pos))
        return true;
    char partName[256] = {0}, refsetName[256] = {0}, instName[256] = {0};
    double csys[9];
    double tx[4][4];
    return UF_ASSEM_ask_component_data(occ, partName, refsetName,
                                       instName, pos, csys, tx) == 0;
}

/** @brief 获取组件显示名(instance 名) */
std::string getComponentName(tag_t occ)
{
    char partName[256] = {0}, refsetName[256] = {0}, instName[256] = {0};
    double csys[9], tx[4][4], pos[3];
    if (UF_ASSEM_ask_component_data(occ, partName, refsetName,
                                    instName, pos, csys, tx) == 0 &&
        instName[0] != '\0')
        return std::string(instName);
    return "(unnamed)";
}

/** @brief 获取 WCS 原点与矩阵(行向量约定) */
bool getWcsInfo(double origin[3], double mtx[9])
{
    tag_t wcsTag;
    if (UF_CSYS_ask_wcs(&wcsTag) != 0)
        return false;
    tag_t mtxTag;
    if (UF_CSYS_ask_csys_info(wcsTag, &mtxTag, origin) != 0)
        return false;
    return UF_CSYS_ask_matrix_values(mtxTag, mtx) == 0;
}

/** @brief 计算组件集合的包围盒对角线长度 */
bool getAssemblyExtent(const std::vector<tag_t> &occList, double &diag)
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
    if (!any) return false;
    double dx = maxV[0] - minV[0], dy = maxV[1] - minV[1], dz = maxV[2] - minV[2];
    diag = sqrt(dx * dx + dy * dy + dz * dz);
    return true;
}

/** @brief 递归查找指定 occurrence tag 对应的 NXOpen Component */
NXOpen::Assemblies::Component *findComponentByTag(
    NXOpen::Assemblies::Component *comp, tag_t occTag)
{
    if (comp->Tag() == occTag)
        return comp;
    std::vector<NXOpen::Assemblies::Component *> children = comp->GetChildren();
    for (size_t i = 0; i < children.size(); i++)
    {
        NXOpen::Assemblies::Component *r = findComponentByTag(children[i], occTag);
        if (r != NULL) return r;
    }
    return NULL;
}

/** @brief 在指定位置按坐标系矩阵画三轴临时线段+文字标签 */
void showAxisHint(const double originPos[3], const double csysMatrix[9],
                  double axisLen, const char *labelPrefix)
{
    const int axisColor[3] = {186, 36, 211};
    const char *axisNames[3] = {"X", "Y", "Z"};
    UF_OBJ_disp_props_t solidProps, dashProps, textProps;
    solidProps.font = 1; solidProps.line_width = -1;
    dashProps.font = 2; dashProps.line_width = -1;
    textProps.font = 1; textProps.line_width = -1;
    UF_DISP_view_type_t viewMode = static_cast<UF_DISP_view_type_t>(UF_DISP_ALL_ACTIVE_VIEWS);
    for (int a = 0; a < 3; a++)
    {
        double ax = csysMatrix[a * 3 + 0], ay = csysMatrix[a * 3 + 1], az = csysMatrix[a * 3 + 2];
        double p1[3] = {originPos[0], originPos[1], originPos[2]};
        double p2[3] = {originPos[0] + ax * axisLen, originPos[1] + ay * axisLen, originPos[2] + az * axisLen};
        double p3[3] = {originPos[0] - ax * axisLen, originPos[1] - ay * axisLen, originPos[2] - az * axisLen};
        solidProps.color = axisColor[a]; dashProps.color = axisColor[a];
        UF_DISP_display_temporary_line(NULL_TAG, viewMode, p1, p2, &solidProps);
        UF_DISP_display_temporary_line(NULL_TAG, viewMode, p1, p3, &dashProps);
        char label[8];
        sprintf(label, "%s%s+", labelPrefix, axisNames[a]);
        textProps.color = axisColor[a];
        UF_DISP_display_temporary_text(NULL_TAG, viewMode, label, p2,
            static_cast<UF_DISP_text_ref_t>(UF_DISP_MIDDLECENTER), &textProps, 0.0, 0);
        sprintf(label, "%s%s-", labelPrefix, axisNames[a]);
        UF_DISP_display_temporary_text(NULL_TAG, viewMode, label, p3,
            static_cast<UF_DISP_text_ref_t>(UF_DISP_MIDDLECENTER), &textProps, 0.0, 0);
    }
}

/** @brief 方向轴选择: 弹出 NX 原生矢量对话框 */
bool askDirection(const double originPos[3], double axisLen,
                  double dir[3], std::string &desc)
{
    const double absMtx[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    showAxisHint(originPos, absMtx, axisLen, "");
    double wcsOrigin[3], wcsMtx[9];
    if (getWcsInfo(wcsOrigin, wcsMtx))
        showAxisHint(wcsOrigin, wcsMtx, axisLen, "W");
    print("Pick the spread axis in the Vector dialog:");
    print("  - XC/YC/ZC in the type list = absolute axes");
    print("  - CSYS axes in graphics = WCS relative axes");
    print("  - or pick edges/faces/two points for any direction.");
    print("  - Press OK in the dialog to confirm the direction.");
    int vecMode = UF_UI_INFERRED;
    double vecOrigin[3] = {0.0, 0.0, 0.0};
    int vecResp = 0;
    UF_UI_specify_vector("Specify spread-out direction axis (Vector dialog)", &vecMode,
                         UF_UI_DISP_TEMP_VECTOR, dir, vecOrigin, &vecResp);
    UF_DISP_regenerate_display();
    if (vecResp != UF_UI_OK) return false;
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
    default: { stringstream ss; ss << "Vector (mode " << vecMode << ")"; desc = ss.str(); break; }
    }
    return true;
}

/** @brief 将绝对坐标位移转换为组件局部坐标位移 (R^T * absDisp) */
void askLocalDisplacement(tag_t occ, const double absDisp[3], double localDisp[3])
{
    char partName[256] = {0}, refsetName[256] = {0}, instName[256] = {0};
    double pos[3], csys[9], xform[4][4];
    if (UF_ASSEM_ask_component_data(occ, partName, refsetName,
                                    instName, pos, csys, xform) == 0)
    {
        for (int i = 0; i < 3; i++)
            localDisp[i] = xform[0][i] * absDisp[0] +
                           xform[1][i] * absDisp[1] +
                           xform[2][i] * absDisp[2];
    }
    else
    {
        localDisp[0] = absDisp[0];
        localDisp[1] = absDisp[1];
        localDisp[2] = absDisp[2];
    }
}

/** @brief 捕捉组件沿指定方向的精确规格尺寸(精确包围盒 + AABB 回退 + NaN 守卫) */
bool askExtentAlongDir(tag_t occ, const char *name, const double dir[3],
                       double &centerT, double &halfW)
{
    double d[3] = {dir[0], dir[1], dir[2]};
    double len = sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (!(len > 1.0e-12) || !(len == len) || len > 1.0e30)
    {
        print(std::string("  [extent] ") + name + ": bad dir vector -> fallback");
        return false;
    }
    d[0] /= len; d[1] /= len; d[2] /= len;
    double mtx[9];
    int rc = UF_MTX3_initialize_x(d, mtx);
    if (rc != 0)
    { stringstream ss; ss << "  [extent] " << name << ": UF_MTX3_initialize_x rc=" << rc; print(ss.str()); return false; }
    tag_t mtxTag = NULL_TAG;
    rc = UF_CSYS_create_matrix(mtx, &mtxTag);
    if (rc != 0)
    { stringstream ss; ss << "  [extent] " << name << ": UF_CSYS_create_matrix rc=" << rc; print(ss.str()); return false; }
    double csysOrigin[3] = {0.0, 0.0, 0.0};
    tag_t csysTag = NULL_TAG;
    rc = UF_CSYS_create_temp_csys(csysOrigin, mtxTag, &csysTag);
    if (rc != 0)
    { stringstream ss; ss << "  [extent] " << name << ": UF_CSYS_create_temp_csys rc=" << rc; print(ss.str()); UF_OBJ_delete_object(mtxTag); return false; }
    double minCorner[3] = {0.0, 0.0, 0.0};
    double dirs[3][3] = {{0.0}};
    double dists[3] = {0.0, 0.0, 0.0};
    rc = UF_MODL_ask_bounding_box_exact(occ, csysTag, minCorner, dirs, dists);
    UF_OBJ_delete_object(csysTag);
    UF_OBJ_delete_object(mtxTag);
    if (rc != 0)
    { stringstream ss; ss << "  [extent] " << name << ": UF_MODL_ask_bounding_box_exact rc=" << rc; print(ss.str()); return false; }
    centerT = minCorner[0] * d[0] + minCorner[1] * d[1] + minCorner[2] * d[2] + dists[0] * 0.5;
    halfW = dists[0] * 0.5;
    if (!(centerT == centerT) || !(halfW == halfW) ||
        fabs(centerT) > 1.0e30 || fabs(halfW) > 1.0e30 || halfW < 0.0)
    {
        stringstream ss;
        ss << "  [extent] " << name << ": invalid exact bbox (centerT=" << centerT
           << ", halfW=" << halfW << ") -> fallback";
        print(ss.str()); return false;
    }
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
            print(ss.str()); return false;
        }
    }
    return true;
}

/** @brief 爆炸项: 组件 occurrence + 沿轴投影 + 沿轴半宽 + 名称 */
struct SpreadItem {
    tag_t occ;
    double t;      ///< 相对分界点沿方向轴的投影
    double halfW;  ///< 组件沿轴方向包围盒半宽
    std::string name;
};

/** @brief 正侧排序: 按投影 t 升序 (近→远) */
bool cmpPosSide(const SpreadItem &a, const SpreadItem &b) { return a.t < b.t; }
/** @brief 负侧排序: 按投影 t 降序 (近→远, 负值大者近) */
bool cmpNegSide(const SpreadItem &a, const SpreadItem &b) { return a.t > b.t; }

//------------------------------------------------------------------------------
// computeGroups -- 按轴向投影将组件分为正负两侧
//------------------------------------------------------------------------------
/**
 * @brief 按轴向投影将组件分为正负两侧
 * @param[in] selOccs 选中的组件 occurrence 列表
 * @param[in] originPos 原点/基准点位置
 * @param[in] dir 方向单位向量
 * @param[in] originOcc 原点组件 tag（跳过该组件）
 * @param[out] posSide 正侧组件列表
 * @param[out] negSide 负侧组件列表
 * @param[out] maxWidth 所有组件沿轴最大宽度
 * @param[in] diag 启用诊断日志
 * @return 成功分组的组件数
 */
int computeGroups(const std::vector<tag_t> &selOccs,
                  const double originPos[3], const double dir[3],
                  tag_t originOcc,
                  std::vector<SpreadItem> &posSide,
                  std::vector<SpreadItem> &negSide,
                  double &maxWidth, bool diag)
{
    double originDot = originPos[0] * dir[0] + originPos[1] * dir[1] + originPos[2] * dir[2];
    for (size_t i = 0; i < selOccs.size(); i++)
    {
        if (selOccs[i] == originOcc) continue;
        std::string compName = getComponentName(selOccs[i]);
        double centerT = 0.0, halfW = 0.0, t = 0.0;
        bool haveExtent = askExtentAlongDir(selOccs[i], compName.c_str(), dir, centerT, halfW);
        if (haveExtent)
        {
            t = centerT - originDot;
        }
        else
        {
            double pos[3];
            if (!getComponentPosition(selOccs[i], pos))
            {
                print("  skip (no geometry/position): " + compName);
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
        if (width > maxWidth) maxWidth = width;
        SpreadItem it = {selOccs[i], t, halfW, compName};
        if (t >= 0.0) posSide.push_back(it);
        else          negSide.push_back(it);
        if (diag) {
            stringstream ss; ss << "  [diag] computeGroups: " << compName
                << " t=" << t << " halfW=" << halfW; print(ss.str());
        }
    }
    return (int)(posSide.size() + negSide.size());
}

//------------------------------------------------------------------------------
// computePackedLayout -- 计算打包布局目标位置
//------------------------------------------------------------------------------
/**
 * @brief 计算打包布局目标位置（按组件宽度累加 + 均匀间隙）
 * @param[in] posSide 正侧组件（按近→远排序）
 * @param[in] negSide 负侧组件（按近→远排序）
 * @param[in] edgeHalf 原点组件沿轴半宽
 * @param[in] gap 相邻组件间隙
 * @param[in] cfg 爆炸配置（正负侧独立缩放），可为 NULL
 * @param[out] vals 每个组件的沿轴目标位置
 * @param[in] diag 启用诊断日志
 */
void computePackedLayout(const std::vector<SpreadItem> &posSide,
                         const std::vector<SpreadItem> &negSide,
                         double edgeHalf, double gap,
                         const ExplosionConfig *cfg,
                         std::vector<double> &vals, bool diag)
{
    size_t nPos = posSide.size(), nNeg = negSide.size();
    vals.resize(nPos + nNeg, 0.0);
    double prevEdge = edgeHalf + gap;
    for (size_t k = 0; k < nPos; k++)
    {
        vals[k] = prevEdge + posSide[k].halfW;
        prevEdge = vals[k] + posSide[k].halfW + gap;
    }
    if (cfg != NULL) {
        double ps = cfg->effectivePosScale();
        for (size_t k = 0; k < nPos; k++) vals[k] = edgeHalf + (vals[k] - edgeHalf) * ps;
    }
    prevEdge = edgeHalf + gap;
    for (size_t k = 0; k < nNeg; k++)
    {
        vals[nPos + k] = prevEdge + negSide[k].halfW;
        prevEdge = vals[nPos + k] + negSide[k].halfW + gap;
    }
    if (cfg != NULL) {
        double ns = cfg->effectiveNegScale();
        for (size_t k = 0; k < nNeg; k++) vals[nPos + k] = edgeHalf + (vals[nPos + k] - edgeHalf) * ns;
    }
    if (diag) {
        for (size_t i = 0; i < vals.size(); i++) {
            stringstream ss; ss << "  [diag] packedLayout[" << i << "] = " << vals[i]; print(ss.str());
        }
    }
}

//------------------------------------------------------------------------------
// applyExplosionMoves -- 对所有组件应用爆炸位移
//------------------------------------------------------------------------------
/**
 * @brief 对所有组件应用爆炸位移
 * @param[in] explosionTag 爆炸图 tag
 * @param[in] items 待移动组件列表
 * @param[in] targets 每个组件的沿轴目标位置（带符号）
 * @param[in] dir 方向向量
 * @param[out] movedCount 成功移动的组件数
 * @param[in] sideLabel 侧标签 ("+side" 或 "-side")
 * @param[in] diag 启用诊断日志
 */
void applyExplosionMoves(tag_t explosionTag,
                         const std::vector<SpreadItem> &items,
                         const std::vector<double> &targets,
                         const double dir[3],
                         int &movedCount, const char *sideLabel, bool diag)
{
    for (size_t k = 0; k < items.size(); k++)
    {
        double target = targets[k];
        if (fabs(target) < 1.0e-9)
        { print("  " + items[k].name + ": target=0 -> no move"); continue; }
        double deltaT = target - items[k].t;
        if (fabs(deltaT) < 1.0e-9)
        { print("  " + items[k].name + ": already at target -> no move"); continue; }
        double absDisp[3] = { dir[0] * deltaT, dir[1] * deltaT, dir[2] * deltaT };
        double localDisp[3];
        askLocalDisplacement(items[k].occ, absDisp, localDisp);
        if (diag) {
            stringstream ss; ss << "  [diag] " << sideLabel << " " << items[k].name
                << " deltaT=" << deltaT << " absDisp=(" << absDisp[0] << "," << absDisp[1] << "," << absDisp[2]
                << ") localDisp=(" << localDisp[0] << "," << localDisp[1] << "," << localDisp[2] << ")";
            print(ss.str());
        }
        double transform[4][4] = {
            {1.0, 0.0, 0.0, localDisp[0]}, {0.0, 1.0, 0.0, localDisp[1]},
            {0.0, 0.0, 1.0, localDisp[2]}, {0.0, 0.0, 0.0, 1.0}
        };
        int xrc = UF_ASSEM_explode_component(explosionTag, items[k].occ, transform);
        if (xrc == 0)
        {
            movedCount++;
            stringstream ssItem;
            ssItem << "  " << items[k].name << " | " << sideLabel << " | t=" << items[k].t
                   << " target=" << target << " move " << deltaT;
            print(ssItem.str());
        }
        else
        {
            stringstream ssErr;
            ssErr << "  ERROR: UF_ASSEM_explode_component rc=" << xrc << " for " << items[k].name;
            print(ssErr.str());
        }
    }
}

//------------------------------------------------------------------------------
// detectOverlaps -- 检测同侧相邻组件沿轴是否重叠
//------------------------------------------------------------------------------
/**
 * @brief 检测同侧相邻组件沿轴是否重叠
 * @param[in] side 组件列表
 * @param[in] vals 目标位置数组
 * @param[in] valOffset vals 中的起始偏移
 * @param[in] sideLabel 侧标签 ("+side" 或 "-side")
 */
void detectOverlaps(const std::vector<SpreadItem> &side,
                    const std::vector<double> &vals,
                    size_t valOffset, const char *sideLabel)
{
    for (size_t k = 1; k < side.size(); k++)
    {
        double gapK = vals[valOffset + k] - vals[valOffset + k - 1]
                      - side[k].halfW - side[k - 1].halfW;
        if (gapK < -1.0e-6)
        {
            stringstream ssOv;
            ssOv << "  WARNING: " << sideLabel << " overlap! " << side[k - 1].name << " and "
                 << side[k].name << " overlap by " << -gapK
                 << " (targets " << vals[valOffset + k - 1] << " / " << vals[valOffset + k] << ")";
            print(ssOv.str());
        }
    }
}

//------------------------------------------------------------------------------
// groupAndMove -- 按轴向投影分组并逐组件输入目标位置 (协调器)
//------------------------------------------------------------------------------
/**
 * @brief 按轴向投影分组并逐组件输入目标位置: 正侧向+dir, 负侧向-dir
 * @param[in] explosionTag 爆炸图 tag
 * @param[in] selOccs 选中的组件 occurrence 列表
 * @param[in] originOcc 原点组件 tag
 * @param[in] originPos 原点位置
 * @param[in] dir 方向单位向量
 * @param[in] minSpacing 最小间距基准值
 * @param[in] dirDesc 方向描述字符串
 * @param[in,out] keepVals 持久编辑状态(组件名→已确认目标位置), 可为 NULL
 * @param[in] cfg 爆炸配置, 可为 NULL
 * @return 移动组件数, -1 = 用户取消
 */
int groupAndMove(tag_t explosionTag, const std::vector<tag_t> &selOccs,
                 tag_t originOcc, const double originPos[3],
                 const double dir[3], double minSpacing, const char *dirDesc,
                 std::map<std::string, double> *keepVals,
                 const ExplosionConfig *cfg)
{
    bool diag = (cfg != NULL && cfg->enableDiagnostics);

    // 1. computeGroups
    std::vector<SpreadItem> posSide, negSide;
    double maxWidth = 0.0;
    computeGroups(selOccs, originPos, dir, originOcc, posSide, negSide, maxWidth, diag);

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

    // 打印分组预览
    print("Grouping along the axis (measured exact size along axis):");
    if (nPos > 0)
    {
        print("  +side (move along +dir):");
        for (size_t k = 0; k < nPos; k++)
        {
            stringstream ssPrev;
            ssPrev << "    " << posSide[k].name << "  [size along axis = "
                   << posSide[k].halfW * 2.0 << ", pos = " << posSide[k].t << "]";
            print(ssPrev.str());
        }
    }
    if (nNeg > 0)
    {
        print("  -side (move along -dir):");
        for (size_t k = 0; k < nNeg; k++)
        {
            stringstream ssPrev;
            ssPrev << "    " << negSide[k].name << "  [size along axis = "
                   << negSide[k].halfW * 2.0 << ", pos = " << negSide[k].t << "]";
            print(ssPrev.str());
        }
    }

    // 2. 计算 gap (使用 cfg 覆盖)
    double gap = maxWidth * 0.3;
    double gapMin = (cfg != NULL && cfg->minGapOverride > 0.0)
                    ? cfg->minGapOverride : minSpacing * 0.4;
    if (cfg != NULL) gap *= cfg->gapScaleFactor;
    if (gap < gapMin) gap = gapMin;
    {
        stringstream ssSp;
        ssSp << "Packed target positions: uniform gap = " << gap
             << " (max component width along axis x 0.3";
        if (cfg != NULL && cfg->gapScaleFactor != 1.0)
            ssSp << " x scale " << cfg->gapScaleFactor;
        ssSp << ", min " << gapMin << ")";
        print(ssSp.str());
    }
    print("Enter target position along the axis per component (0 = no move).");
    print("Default: components packed by their widths, adjacent gap uniform");
    print("         (widths respected, no overlap even for different lengths).");

    // 3. 计算 edgeHalf
    double edgeHalf = 0.0;
    if (originOcc != NULL_TAG)
    {
        std::string oname = getComponentName(originOcc);
        double ocT = 0.0;
        if (!askExtentAlongDir(originOcc, oname.c_str(), dir, ocT, edgeHalf))
        {
            double obbox[6];
            if (UF_MODL_ask_bounding_box(originOcc, obbox) == 0)
                edgeHalf = ((obbox[3] - obbox[0]) * fabs(dir[0]) +
                            (obbox[4] - obbox[1]) * fabs(dir[1]) +
                            (obbox[5] - obbox[2]) * fabs(dir[2])) * 0.5;
        }
    }
    { stringstream ssEdge; ssEdge << "Origin edge half-width along axis = " << edgeHalf; print(ssEdge.str()); }

    // 4. computePackedLayout
    std::vector<double> vals;
    computePackedLayout(posSide, negSide, edgeHalf, gap, cfg, vals, diag);

    // 填充 uc1609 名称数组
    static_assert(sizeof(std::array<char, 16>) == 16, "uc1609 name array layout");
    std::vector<std::array<char, 16>> itemNames(nTotal);
    for (auto &name : itemNames) name.fill(0);
    for (size_t k = 0; k < nPos; k++)
        strncpy(itemNames[k].data(), posSide[k].name.c_str(), 15);
    for (size_t k = 0; k < nNeg; k++)
        strncpy(itemNames[nPos + k].data(), negSide[k].name.c_str(), 15);

    // 5. keepVals 预填
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

    // 6. uc1609 对话框
    int ip5 = 0;
    int inputResp = uc1609("Enter target position along axis per component (first +side, then -side)",
                           reinterpret_cast<char(*)[16]>(itemNames.data()), (int)nTotal, vals.data(), &ip5);
    { stringstream ssIR; ssIR << "uc1609 response = " << inputResp << " (>=3 = confirmed, <3 = cancel)"; print(ssIR.str()); }
    if (inputResp < 3) return -1;

    // 7. 回写 keepVals
    if (keepVals != NULL)
    {
        for (size_t k = 0; k < nPos; k++)
            (*keepVals)[posSide[k].name] = vals[k];
        for (size_t k = 0; k < nNeg; k++)
            (*keepVals)[negSide[k].name] = vals[nPos + k];
    }

    // 8. 构建带符号 targets 并 applyExplosionMoves
    int moved = 0;
    std::vector<double> posTargets(nPos), negTargets(nNeg);
    for (size_t k = 0; k < nPos; k++) posTargets[k] = vals[k];
    for (size_t k = 0; k < nNeg; k++) negTargets[k] = -vals[nPos + k];
    applyExplosionMoves(explosionTag, posSide, posTargets, dir, moved, "+side", diag);
    applyExplosionMoves(explosionTag, negSide, negTargets, dir, moved, "-side", diag);

    // 9. detectOverlaps
    detectOverlaps(posSide, vals, 0, "+side");
    detectOverlaps(negSide, vals, nPos, "-side");

    stringstream ss;
    ss << "Spread-out: dir " << dirDesc << " (" << dir[0] << ", " << dir[1]
       << ", " << dir[2] << "), moved " << moved << " / " << nTotal;
    print(ss.str());
    return moved;
}

/** @brief 递归收集装配中的所有叶子组件 */
void collectLeafComponents(NXOpen::Assemblies::Component *comp,
                           std::vector<NXOpen::Assemblies::Component *> &out)
{
    std::vector<NXOpen::Assemblies::Component *> children = comp->GetChildren();
    for (size_t i = 0; i < children.size(); i++)
    {
        std::vector<NXOpen::Assemblies::Component *> sub = children[i]->GetChildren();
        if (sub.empty()) out.push_back(children[i]);
        else collectLeafComponents(children[i], out);
    }
}

/** @brief 爆炸参数项: 组件显示名 + 爆炸位移向量 */
struct ExplodeParam { std::string displayName; double dx, dy, dz; };

/** @brief 读取 dll 同目录下 explosion_params.txt 参数文件 */
bool loadExplodeParams(std::vector<ExplodeParam> &params)
{
    HMODULE hMod = NULL;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCSTR>(&loadExplodeParams), &hMod))
        return false;
    char dllPath[MAX_PATH] = {0};
    if (!GetModuleFileNameA(hMod, dllPath, MAX_PATH)) return false;
    std::string dir(dllPath);
    size_t pos = dir.find_last_of("\\/");
    if (pos == std::string::npos) return false;
    std::string paramPath = dir.substr(0, pos + 1) + "explosion_params.txt";
    std::ifstream in(paramPath.c_str());
    if (!in.is_open()) return false;
    std::string line;
    while (std::getline(in, line))
    {
        if (line.empty()) continue;
        size_t p1 = line.find('|'); if (p1 == std::string::npos) continue;
        size_t p2 = line.find('|', p1 + 1); if (p2 == std::string::npos) continue;
        size_t p3 = line.find('|', p2 + 1); if (p3 == std::string::npos) continue;
        ExplodeParam p;
        p.displayName = line.substr(0, p1);
        p.dx = atof(line.substr(p1 + 1, p2 - p1 - 1).c_str());
        p.dy = atof(line.substr(p2 + 1, p3 - p2 - 1).c_str());
        p.dz = atof(line.substr(p3 + 1).c_str());
        params.push_back(p);
    }
    return !params.empty();
}

} // anonymous namespace

//------------------------------------------------------------------------------
// NXOpen c++ test class
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
	BasePart *workPart, *displayPart;
	NXMessageBox *mb;
	ListingWindow *lw;
	LogFile *lf;

	// -- Explosion config management --
	ExplosionConfig explCfg;
	std::string configFilePath;
	std::vector<std::string> presetNames; ///< 可用预设列表

	void loadConfig();
	void saveConfig();
	void configureExplosionParams();
	void loadPresetList();
	void selectPreset();
	void saveAsNewPreset();
};

Session *(MyClass::theSession) = NULL;
UI *(MyClass::theUI) = NULL;

MyClass::MyClass()
{
	MyClass::theSession = NXOpen::Session::GetSession();
	MyClass::theUI = UI::GetUI();
	mb = theUI->NXMessageBox();
	lw = theSession->ListingWindow();
	lf = theSession->LogFile();
    workPart = theSession->Parts()->BaseWork();
	displayPart = theSession->Parts()->BaseDisplay();
	configFilePath = getDllDirectory() + "explosion_config.txt";
}

MyClass::~MyClass() {}

void MyClass::print(const NXString &msg)
{	if(! lw->IsOpen() ) lw->Open(); lw->WriteLine(msg); }
void MyClass::print(const string &msg)
{	if(! lw->IsOpen() ) lw->Open(); lw->WriteLine(msg); }
void MyClass::print(const char * msg)
{	if(! lw->IsOpen() ) lw->Open(); lw->WriteLine(msg); }

//------------------------------------------------------------------------------
// loadPresetList -- scan config file for all preset section names
//------------------------------------------------------------------------------
void MyClass::loadPresetList()
{
	presetNames = ExplosionConfig::listPresets(configFilePath);
	stringstream ss;
	ss << "Found " << presetNames.size() << " preset(s) in config file:";
	for (size_t i = 0; i < presetNames.size(); i++)
		ss << " " << presetNames[i];
	print(ss.str());
}

//------------------------------------------------------------------------------
// loadConfig -- try loading config from DLL directory
//------------------------------------------------------------------------------
void MyClass::loadConfig()
{
	loadPresetList();
	// Try loading the first preset if available, otherwise try old format
	if (!presetNames.empty())
	{
		if (ExplosionConfig::loadFromFile(configFilePath, presetNames[0], explCfg))
		{
			stringstream ss;
			ss << "Loaded explosion config preset [" << presetNames[0]
			   << "] from: " << configFilePath
			   << " (version " << explCfg.config_version << ")";
			print(ss.str());
		}
	}
	else
	{
		// Backward compat: try old [ExplosionConfig] section
		if (ExplosionConfig::loadFromFile(configFilePath, "ExplosionConfig", explCfg))
		{
			stringstream ss;
			ss << "Loaded explosion config (legacy format) from: " << configFilePath
			   << " (version " << explCfg.config_version << ")";
			print(ss.str());
		}
	}
}

//------------------------------------------------------------------------------
// saveConfig -- save current config to DLL directory (first preset or default)
//------------------------------------------------------------------------------
void MyClass::saveConfig()
{
	// Save to the first preset if available, otherwise use "Preset_Default"
	std::string preset = presetNames.empty() ? "Preset_Default" : presetNames[0];
	if (explCfg.saveToFile(configFilePath, preset))
	{
		stringstream ss;
		ss << "Saved explosion config [" << preset << "] to: " << configFilePath;
		print(ss.str());
	}
	else
		print("Warning: could not save explosion config.");
}

//------------------------------------------------------------------------------
// selectPreset -- interactive preset selection via uc1603
//------------------------------------------------------------------------------
void MyClass::selectPreset()
{
	loadPresetList();
	if (presetNames.empty())
	{
		print("No presets found in config file.");
		return;
	}

	// Build uc1603 items (max 38 chars per item)
	int n = (int)presetNames.size();
	std::vector<std::array<char, 38>> items(n);
	for (int i = 0; i < n; i++)
	{
		items[i].fill(0);
		strncpy(items[i].data(), presetNames[i].c_str(), 37);
	}

	int resp = uc1603("Select preset to load:", 3,
	                  reinterpret_cast<char(*)[38]>(items.data()), n);
	// uc1603 type=3: resp is 1-based index of selected item
	int idx = resp - 1;
	if (idx < 0 || idx >= n)
	{
		print("Preset selection cancelled.");
		return;
	}

	ExplosionConfig loaded;
	if (ExplosionConfig::loadFromFile(configFilePath, presetNames[idx], loaded))
	{
		explCfg = loaded;
		stringstream ss;
		ss << "Loaded preset [" << presetNames[idx] << "]"
		   << " (version " << explCfg.config_version << ")";
		print(ss.str());
	}
	else
	{
		stringstream ss;
		ss << "Error: failed to load preset [" << presetNames[idx] << "]";
		print(ss.str());
	}
}

//------------------------------------------------------------------------------
// saveAsNewPreset -- save current config as a new preset via uc1609
//------------------------------------------------------------------------------
void MyClass::saveAsNewPreset()
{
	// Use uc1609 to input preset name (single value, label = "PresetName")
	std::array<char, 16> nameBuf;
	nameBuf.fill(0);
	strncpy(nameBuf.data(), "NewPreset", 15);
	double dummyVal = 0.0;
	int ip5 = 0;
	int resp = uc1609("Enter preset name (max 15 chars)",
	                  reinterpret_cast<char(*)[16]>(&nameBuf),
	                  1, &dummyVal, &ip5);
	if (resp < 3)
	{
		print("Save preset cancelled.");
		return;
	}

	std::string presetName(nameBuf.data());
	if (presetName.empty())
	{
		print("Error: empty preset name.");
		return;
	}

	if (explCfg.saveToFile(configFilePath, presetName))
	{
		stringstream ss;
		ss << "Saved current config as preset [" << presetName << "]";
		print(ss.str());
		loadPresetList(); // refresh preset list
	}
	else
	{
		print("Error: failed to save preset.");
	}
}

//------------------------------------------------------------------------------
// configureExplosionParams -- interactive config via uc1603/uc1609
//------------------------------------------------------------------------------
void MyClass::configureExplosionParams()
{
	bool hasMultiplePresets = (presetNames.size() > 1);

	if (hasMultiplePresets)
	{
		const char cfgItems[][38] = {
			"Use default params",
			"Select preset",
			"Customize params",
			"Save as new preset",
			"Cancel"
		};
		int cfgResp = uc1603("Explosion parameters:", 3, cfgItems, 5);
		if (cfgResp == 1)
		{
			explCfg = ExplosionConfig(); // reset to defaults
			print("Using default explosion parameters.");
		}
		else if (cfgResp == 2)
		{
			selectPreset();
		}
		else if (cfgResp == 3)
		{
			// Customize params (same logic as below)
			static_assert(sizeof(std::array<char, 16>) == 16, "uc1609 layout");
			std::vector<std::array<char, 16>> names(4);
			for (auto &n : names) n.fill(0);
			strncpy(names[0].data(), "GapScale", 15);
			strncpy(names[1].data(), "MinGap", 15);
			strncpy(names[2].data(), "+SideScale", 15);
			strncpy(names[3].data(), "-SideScale", 15);
			double vals[4] = { explCfg.gapScaleFactor, explCfg.minGapOverride,
			                   explCfg.posSideScaleFactor, explCfg.negSideScaleFactor };
			int ip5 = 0;
			int resp = uc1609("Set explosion parameters", reinterpret_cast<char(*)[16]>(names.data()),
			                  4, vals, &ip5);
			if (resp >= 3)
			{
				explCfg.gapScaleFactor = vals[0];
				explCfg.minGapOverride = vals[1];
				explCfg.posSideScaleFactor = vals[2];
				explCfg.negSideScaleFactor = vals[3];
				saveConfig();
				stringstream ss;
				ss << "Config updated: gapScale=" << explCfg.gapScaleFactor
				   << " minGap=" << explCfg.minGapOverride
				   << " +sideScale=" << explCfg.posSideScaleFactor
				   << " -sideScale=" << explCfg.negSideScaleFactor;
				print(ss.str());
			}
			else
				print("Config customization cancelled, using previous values.");
		}
		else if (cfgResp == 4)
		{
			saveAsNewPreset();
		}
		else
		{
			print("Config menu cancelled, using current values.");
		}
	}
	else
	{
		const char cfgItems[][38] = {
			"Use default params",
			"Customize params",
			"Save as new preset",
			"Cancel"
		};
		int cfgResp = uc1603("Explosion parameters:", 3, cfgItems, 4);
		if (cfgResp == 1)
		{
			explCfg = ExplosionConfig(); // reset to defaults
			print("Using default explosion parameters.");
		}
		else if (cfgResp == 2)
		{
			static_assert(sizeof(std::array<char, 16>) == 16, "uc1609 layout");
			std::vector<std::array<char, 16>> names(4);
			for (auto &n : names) n.fill(0);
			strncpy(names[0].data(), "GapScale", 15);
			strncpy(names[1].data(), "MinGap", 15);
			strncpy(names[2].data(), "+SideScale", 15);
			strncpy(names[3].data(), "-SideScale", 15);
			double vals[4] = { explCfg.gapScaleFactor, explCfg.minGapOverride,
			                   explCfg.posSideScaleFactor, explCfg.negSideScaleFactor };
			int ip5 = 0;
			int resp = uc1609("Set explosion parameters", reinterpret_cast<char(*)[16]>(names.data()),
			                  4, vals, &ip5);
			if (resp >= 3)
			{
				explCfg.gapScaleFactor = vals[0];
				explCfg.minGapOverride = vals[1];
				explCfg.posSideScaleFactor = vals[2];
				explCfg.negSideScaleFactor = vals[3];
				saveConfig();
				stringstream ss;
				ss << "Config updated: gapScale=" << explCfg.gapScaleFactor
				   << " minGap=" << explCfg.minGapOverride
				   << " +sideScale=" << explCfg.posSideScaleFactor
				   << " -sideScale=" << explCfg.negSideScaleFactor;
				print(ss.str());
			}
			else
				print("Config customization cancelled, using previous values.");
		}
		else if (cfgResp == 3)
		{
			saveAsNewPreset();
		}
		else
		{
			print("Config menu cancelled, using current values.");
		}
	}
}

//------------------------------------------------------------------------------
// Step 1: explosion view creation
//------------------------------------------------------------------------------
void MyClass::do_it()
{
	Part *workPart = dynamic_cast<Part *>(theSession->Parts()->Work());
	if (workPart == NULL)
	{ print("Error: No work part, cannot create explosion."); return; }

	Assemblies::ComponentAssembly *assembly = workPart->ComponentAssembly();
	if (assembly == NULL)
	{ print("Error: Work part is not an assembly, cannot create explosion."); return; }

	ModelingView *workView = workPart->ModelingViews()->WorkView();
	if (workView == NULL)
	{ print("Error: Cannot get work view."); return; }

	// ------------------------------------------------------------------
	// Step 1: New explosion (reuse "Explosion 1" if exists)
	// ------------------------------------------------------------------
	theSession->SetUndoMark(Session::MarkVisibilityVisible, "New Explosion");

	Assemblies::Explosion *explosion = NULL;
	for (Assemblies::ExplosionCollection::iterator it = assembly->Explosions()->begin();
	     it != assembly->Explosions()->end(); ++it)
	{
		if (strcmp((*it)->Name().GetText(), "Explosion 1") == 0)
		{ explosion = *it; break; }
	}
	if (explosion == NULL)
	{ explosion = assembly->Explosions()->Create("Explosion 1"); print("Created explosion: Explosion 1"); }
	else
		print("Explosion \"Explosion 1\" already exists, reuse it.");

	// ------------------------------------------------------------------
	// Step 2: Explode components
	// ------------------------------------------------------------------
	theSession->SetUndoMark(Session::MarkVisibilityVisible, "Edit Explosion");

	{
		std::vector<ExplodeParam> params;
		bool hasParams = loadExplodeParams(params);

		if (hasParams)
		{
			// Strategy 1: replay from params file
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
							{1.0, 0.0, 0.0, params[j].dx}, {0.0, 1.0, 0.0, params[j].dy},
							{0.0, 0.0, 1.0, params[j].dz}, {0.0, 0.0, 0.0, 1.0}
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
			// Strategy 2: interactive "spread out" mode
			// Load config and let user customize
			loadConfig();
			configureExplosionParams();

			tag_t rootOcc = assembly->RootComponent()->Tag();

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
					// Sub-assembly hint
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

					// Origin/reference component selection
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
						if (objs2 != NULL) UF_free(objs2);
						if (picked)
						{
							originOcc = toPartOccurrence(rootOcc, originSel);
							haveOriginPos = getComponentPosition(originOcc, originPos);
							if (!haveOriginPos)
								print("Error: Cannot get position of origin component.");
						}
						else
							print("No component picked, fall back to WCS origin as reference.");
					}
					else if (originChoice == 2)
						print("No origin component: all selected components spread out.");
					else
						print("Spread-out cancelled.");

					if (!haveOriginPos)
					{
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
							print("Error: WCS unavailable, spread-out cancelled.");
					}

					if (haveOriginPos)
					{
						// Adaptive axis length
						double diag = 300.0;
						getAssemblyExtent(selOccs, diag);
						double axisLen = (diag * 0.12 > 50.0) ? diag * 0.12 : 50.0;

						// Interactive main loop
						std::map<std::string, double> editVals;
						double dir[3] = {0.0, 0.0, 1.0};
						std::string dirDesc;
						bool haveDir = false;
						while (true)
						{
							if (!haveDir)
							{
								if (!askDirection(originPos, axisLen, dir, dirDesc))
									break;
								haveDir = true;
							}

							int moved = groupAndMove(explosion->Tag(), selOccs, originOcc,
							                         originPos, dir, kExplodeDistance, dirDesc.c_str(),
							                         &editVals, &explCfg);

							if (moved == 0)
							{
								print("Nothing moved (all components are already at their targets). Re-picking axis...");
								haveDir = false;
								continue;
							}

							const char nextItems[][38] = {
								"Modify target positions (same axis)",
								"Re-pick axis",
								"Finish"
							};
							int next = uc1603("Spread-out done. What next?", 3, nextItems, 3);
							if (next == 1) { }
							else if (next == 2) haveDir = false;
							else break;
						}
					}
				}
			}
			print("Spread-out interaction finished.");
		}
	}

	// ------------------------------------------------------------------
	// Step 3: Show explosion in work view
	// ------------------------------------------------------------------
	explosion->Show(workView);
	print("Explosion shown in work view.");

	// ------------------------------------------------------------------
	// Step 4: Save view as "ExplosionView01"
	// ------------------------------------------------------------------
	theSession->SetUndoMark(Session::MarkVisibilityVisible, "Save As");

	View *savedView = NULL;
	for (ViewCollection::iterator it = workPart->Views()->begin();
	     it != workPart->Views()->end(); ++it)
	{
		if (strcmp((*it)->Name().GetText(), "ExplosionView01") == 0)
		{ savedView = *it; break; }
	}

	if (savedView == NULL)
	{
		savedView = workPart->Views()->SaveAsPreservingCase(workView, "ExplosionView01", false, false);
		print("Saved explosion view: ExplosionView01");
	}
	else
	{
		explosion->Show(savedView);
		print("View \"ExplosionView01\" already exists, reuse and refresh.");
	}

	print("Step 1 explosion view creation done.");
}

//------------------------------------------------------------------------------
// Entry point
//------------------------------------------------------------------------------
extern "C" DllExport void ufusr( char *parm, int *returnCode, int rlen )
{
    try
    {
		MyClass *theMyClass = new MyClass();
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
