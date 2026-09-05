//------------------------------------------------------------------------------
// NX12 Step9：云线（修订云线）自动绘制（DLL 9/9）
//------------------------------------------------------------------------------
// 版本历史：
//   v7 (2026-09-01) 曲线级幂等，草图可复用（本文件最新）：
//     · 废弃草图级 STEP9_SKETCH 标记（它导致整个草图被永久跳过，无法再画新图形
//       生成云线）；改为曲线级 STEP9_DONE 标记：云线创建成功后先标记来源曲线、
//       再删除参考几何，新画的矩形/圆永远会被处理，旧云线保留不删。
//     · kReprocessMarkedSketches 配置项随草图级标记一并移除。
//   v6 (2026-09-01) 编译兼容修复：
//     · NXObjectManager::Get 返回 TaggedObject*，删除参考几何时先 dynamic_cast 到
//       NXObject* 再交给 Sketch::DeleteObjects（修复 C2440/E0144）；
//     · 预定义 DECLSPEC_NOINITALL 置空，屏蔽 SDK 10.0.19041 winnt.h 的
//       no_init_all 属性（旧工具链/IntelliSense 不认识，修复 E1097）；
//     · 两处日志改 std::to_string 拼接输出，规避 VS2017 sprintf_s 格式检查器
//       对“格式符紧跟中文字符”的误报（C4474/C4477/C4313）。
//   v5 (2026-09-01) 修复草图参考几何删除：
//     · 草图曲线不能直接 UF_OBJ_delete_object（NX 返回失败），改为草图专属
//       Sketch::DeleteObjects（自动清理相关约束），失败逐条记录并保留；
//     · 修复图纸名日志乱码：该行改用 char 缓冲 + const char* 重载输出
//       （std::string 重载经 NXString 本地编码转换会导致中文乱码）。
//   v4 (2026-09-01) 参考几何自动删除：
//     · 云线生成成功后，自动删除作为参考的草图矩形（4 条直线）/ 圆形（圆或大圆弧）
//       几何，草图本身保留；由配置开关 kDeleteSourceGeometry 控制（默认开启，
//       改为 false 可保留参考几何）；
//     · 仅在该形状的云线创建成功后才删除；创建失败时保留草图几何以便重试；
//     · 幂等语义不变：已转换草图仍打 STEP9_SKETCH 标记，其云线保留（v7 起改为曲线级 STEP9_DONE）。
//   v3 (2026-09-01) 云线参数对话框：
//     · 新增运行时“云线参数”对话框（Win32 内存 DLGTEMPLATE + DialogBoxIndirectW，
//       与 Step6 同一范式，无需资源文件）：输入“波浪直径（每波弦长，mm）”，
//       默认 8.0，数值越小云线越密、越大越疏，直接控制半圆弧波浪大小；
//     · 确定：本次运行按输入值生成；取消：本次不画云线（旧云线保留）；
//     · 配置常量 kWaveChord 更名为 kWaveChordDefault（仅作对话框默认值），
//       生成逻辑改用运行时变量传递。
//   v1 (2026-08-22) 初版：矩形/圆形云线按硬编码配置绘制（封闭周期样条拟合波浪半圆弧）。
//   v2 (2026-09-01) 草图驱动重构（本文件）：
//     1) 扫描当前工作图纸上的制图草图（Sketch），自动识别用户手动绘制的矩形与圆形；
//     2) 矩形识别：4 条直线段闭合链（对边等长平行、邻边垂直），提取中心/宽/高/倾角；
//     3) 圆形识别：完整圆（或扫略角 >= 270 度的圆弧），提取圆心/半径；
//     4) 云线参数（位置/尺寸）直接继承自识别结果，替代硬编码配置常量；
//     5) 向后兼容：无可识别草图时回退 kFallback* 默认配置（等同 v1 行为）；
//     6) 幂等保护：已转换曲线打 STEP9_DONE 属性不再重复处理（曲线级幂等，草图可复用）；云线打 STEP9_CLOUD
//        字符串属性（值 = "DEFAULT" 或 "SKETCH:<tag>"），重画同源云线前先删旧线；
//        v1 整数属性旧云线自动迁移清理，不误删其它样条；
//     7) 模块化拆分：草图枚举 / 几何分类 / 矩形识别 / 圆形识别 / 云线生成 /
//        幂等管理各自独立函数，异常按草图隔离捕获，识别失败优雅降级不中断流程。
//
// 说明：
//   · NX12 制图模块没有原生"云线"命令（已核实 NX 12.0 安装目录全部 .men /
//     资源文件中无 Cloud 命令），本模块以"封闭周期样条"拟合波浪半圆弧实现，
//     效果等同 AutoCAD REVCLOUD 的矩形/圆形样式。
//   · 云线落在当前图纸的制图曲线空间（图纸坐标，单位 mm，左下角原点，
//     与 Step1 的视图放置坐标同一约定）。
//   · 全部行为参数在下方"配置区"调整（草图驱动开关 / 回退默认参数 / 波幅 /
//     采样密度 / 容差 / 图层 / 颜色）。
//------------------------------------------------------------------------------

// Win32 防护（本模块需要 windows.h 用于云线参数对话框）
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
// 兼容旧工具链/IntelliSense：SDK 10.0.19041 winnt.h 的 DECLSPEC_NOINITALL
// 使用 VS2019 16.5+ 才支持的 no_init_all 属性，旧版下置空（仅损失零初始化提示）
#ifndef DECLSPEC_NOINITALL
#define DECLSPEC_NOINITALL
#endif
#include <windows.h>
#include <stdlib.h>                            // _wtof（对话框数值解析）
#include <wchar.h>                             // swprintf_s（对话框默认值回显）
#ifdef CreateDialog
#undef CreateDialog
#endif

// 共享头文件（含 UF/NXOpen 基础 includes）
#include "../Shared/NX12_CommonConfig.h"
#include "../Shared/NX12_CommonUtils.h"

// Step9 专有 includes
#include <NXOpen/NXMessageBox.hxx>          // ufusr 异常对话框
#include <NXOpen/NXObjectManager.hxx>       // UF tag -> NXObject（当前图纸回退路线）
#include <NXOpen/Sketch.hxx>                // Sketch / GetAllGeometry / IsDraftingSketch
#include <NXOpen/SketchCollection.hxx>      // Part::Sketches() 枚举
#include <NXOpen/DraftingManager.hxx>          // Part::Drafting() 完整类型（EnterDraftingApplication）
#include <NXOpen/ErrorList.hxx>                // Sketch::DeleteObjects 返回的错误列表
#include <uf_attr.h>                        // UF_ATTR_*（STEP9_CLOUD / STEP9_DONE 用户属性）
#include <uf_csys.h>                        // UF_CSYS_ask_matrix_values（圆弧中心矩阵换算）
#include <uf_object_types.h>                // UF_line_type / UF_circle_type / UF_spline_type / UF_conic_type（草图几何分类）
#include <math.h>                           // sqrt / cos / sin / fabs / atan2
#include <stdio.h>                          // sprintf_s
#include <vector>
#include <set>
#include <string>
#include <functional>

//==============================================================================
// 配置区（静态常量，便于调整；改完重新编译即可）
//==============================================================================

// —— 草图驱动 ——
static constexpr bool  kSketchDriven            = true;   // 草图驱动开关：true=扫描图纸草图；false=直接回退默认参数
static constexpr bool  kDeleteSourceGeometry     = true;   // true=云线生成成功后自动删除参考草图几何（矩形4线/圆）；false=保留

// —— 识别容差 ——
static constexpr double kEndPointTolerance         = 0.01;  // 直线端点闭合判定容差（mm）
static constexpr double kRectLengthTolerance       = 0.05;  // 矩形对边等长相对容差（5%）
static constexpr double kParallelAngleToleranceDeg = 1.0;   // 平行/垂直角度容差（度）
static constexpr double kFullCircleToleranceDeg    = 2.0;   // 完整圆判定角度容差（度）
static constexpr int    kMinArcSweepDeg           = 270;   // 圆弧按圆处理的最小扫略角（度）
static constexpr double kIdentityMatrixTolerance   = 1.0e-9;// 圆弧矩阵单位阵判定容差

// —— 回退默认参数（v1 行为，无可用草图时使用；图纸坐标，mm，左下角原点） ——
static constexpr int    kFallbackCloudMode     = 0;      // 0=矩形+圆形都画 1=仅矩形 2=仅圆形
static constexpr double kFallbackRectCenterX   = 60.0;   // 矩形中心 X
static constexpr double kFallbackRectCenterY   = 105.0;  // 矩形中心 Y
static constexpr double kFallbackRectWidth     = 80.0;   // 矩形宽（X 方向）
static constexpr double kFallbackRectHeight    = 50.0;   // 矩形高（Y 方向）
static constexpr double kFallbackCircleCenterX = 200.0;  // 圆心 X
static constexpr double kFallbackCircleCenterY = 130.0;  // 圆心 Y
static constexpr double kFallbackCircleRadius  = 35.0;   // 圆半径

// —— 云线波浪与样式 ——
static constexpr double kWaveChordDefault = 8.0; // 波浪直径默认值（每波弦长≈2×波幅/半圆弧半径），mm；越小云线越密（运行时对话框可调）
static constexpr int    kPointsPerWave = 12;    // 每波采样点数（越多越圆顺；>= 6）
static constexpr int    kCloudLayer    = 1;     // 目标图层 1..256；0=保持当前图层
static constexpr int    kCloudColor    = 4;     // NX 颜色索引（4=红，修订标注常用色）

// —— 幂等标记属性名（勿与其它模块冲突） ——
static const char kAttrCloudTitle[]  = "STEP9_CLOUD";   // 云线（值：DEFAULT / SKETCH:<tag>）
static const char kAttrDoneTitle[]   = "STEP9_DONE";    // 已转换曲线（整数 1，曲线级幂等；旧版草图级 STEP9_SKETCH 不再读取）

// 圆周率（math.h 的 M_PI 需 _USE_MATH_DEFINES，直接自备常量）
static constexpr double kPI = 3.14159265358979323846;   // 与 uf_defs.h 的 PI 宏区分

//==============================================================================
// 云线参数对话框（Win32 内存模板，无需资源文件；与 Step6 同一范式）
// 界面：波浪直径（每波弦长，mm）编辑框 + 确定/取消。
// 数值越小云线越密（每波半圆弧半径越小）；默认值来自 kWaveChordDefault。
//==============================================================================
namespace CloudDlg
{
	const int IDC_WAVE_CHORD = 1000;

	struct Result
	{
		double waveChord;   // 波浪直径/每波弦长（mm）
	};
	static Result g_result;

	struct Buf
	{
		BYTE d[2048];
		size_t n;
		Buf() : n(0) { memset(d, 0, sizeof(d)); }
		void W(WORD v)      { memcpy(d + n, &v, 2); n += 2; }
		void DW(DWORD v)    { memcpy(d + n, &v, 4); n += 4; }
		void Str(const WCHAR* s) { size_t L = wcslen(s) + 1; memcpy(d + n, s, L * 2); n += L * 2; }
		void Align()        { n = (n + 3) & ~(size_t)3; }
	};

	// 写入一个控件（类名用原子序号）
	static void AddItem(Buf& b, DWORD style, short x, short y, short cx, short cy,
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

	static INT_PTR CALLBACK DlgProc(HWND h, UINT m, WPARAM w, LPARAM l)
	{
		switch (m)
		{
		case WM_INITDIALOG:
		{
			WCHAR buf[32];
			swprintf_s(buf, 32, L"%.2f", g_result.waveChord);
			SetWindowTextW(GetDlgItem(h, IDC_WAVE_CHORD), buf);
			return TRUE;
		}
		case WM_COMMAND:
			switch (LOWORD(w))
			{
			case IDOK:
			{
				WCHAR t[32];
				GetWindowTextW(GetDlgItem(h, IDC_WAVE_CHORD), t, 32);
				double v = _wtof(t);
				if (!(v > 0.0) || v > 1000.0)
				{
					MessageBoxW(h, L"波浪直径必须是 0~1000 之间的正数（mm）",
						L"云线参数", MB_ICONWARNING | MB_OK);
					return TRUE;
				}
				g_result.waveChord = v;
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

	/** 弹出云线参数对话框；返回 true=确定（结果写入 out），false=取消 */
	static bool Show(double defaultChord, Result& out)
	{
		g_result.waveChord = defaultChord;

		Buf b;
		// DLGTEMPLATE
		b.DW(WS_POPUP | WS_VISIBLE | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME);
		b.DW(0);
		// 控件总数：标题标签 + 编辑框 + 提示标签 + 确定 + 取消
		b.W(5);
		b.W(0); b.W(0); b.W(300); b.W(88);
		b.W(0);                         // 无菜单
		b.W(0);                         // 默认类
		b.Str(L"云线参数");

		// 标签：波浪直径
		AddItem(b, WS_CHILD | WS_VISIBLE | SS_LEFT,
			8, 10, 150, 10, 9000, 0x0082, L"波浪直径(每波弦长) mm:");
		// 编辑框：波浪直径（默认 8.0，越小云线越密）
		AddItem(b, WS_CHILD | WS_VISIBLE | WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL,
			162, 8, 60, 12, IDC_WAVE_CHORD, 0x0081, L"");
		// 提示标签
		AddItem(b, WS_CHILD | WS_VISIBLE | SS_LEFT,
			8, 30, 280, 10, 9001, 0x0082, L"提示：数值越小云线越密，越大越疏（默认 8.0 mm）");

		AddItem(b, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
			60, 56, 44, 14, IDOK, 0x0080, L"确定");
		AddItem(b, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
			120, 56, 44, 14, IDCANCEL, 0x0080, L"取消");

		HWND parent = GetActiveWindow();
		INT_PTR r = DialogBoxIndirectW(NULL, (LPCDLGTEMPLATEW)(void*)b.d, parent, DlgProc);
		if (r == 1) { out = g_result; return true; }
		return false;
	}
}

//==============================================================================
// 基础几何类型与工具
//==============================================================================

/** 云线采样点（图纸 XY 平面） */
struct CloudPt { double x; double y; };

/** 直线段记录（图纸坐标，来自 UF_CURVE_ask_line_data 绝对坐标） */
struct LineRec { tag_t tag; double sx; double sy; double ex; double ey; };

/** 圆弧记录（候选圆） */
struct ArcRec { tag_t tag; double cx; double cy; double r; double sweepDeg; bool full; };

/** 矩形识别结果 */
struct RectInfo { double cx; double cy; double width; double height; double angleDeg; std::vector<tag_t> lineTags; };

/** 圆形识别结果 */
struct CircleInfo { double cx; double cy; double radius; tag_t tag; };

/** 识别的形状（矩形/圆形统一载体） */
struct ShapeRec
{
    enum class Kind { Rect, Circle };
    Kind kind;
    double cx;        // 中心 X（矩形/圆）
    double cy;        // 中心 Y
    double width;     // 矩形宽
    double height;    // 矩形高
    double radius;    // 圆半径
    double angleDeg;  // 矩形倾角（度，宽边方向）
    std::vector<tag_t> sourceTags;  // 参考草图几何 tag（矩形=4 条直线；圆=圆/大圆弧曲线）
};

/** 草图几何统计（诊断日志用） */
struct SketchGeomStats
{
    int lines = 0;         // 直线
    int circles = 0;       // 完整圆
    int arcCandidates = 0; // 圆弧（扫略角 >= kMinArcSweepDeg，按圆处理）
    int smallArcs = 0;     // 小圆弧（忽略）
    int splines = 0;       // 样条
    int conics = 0;        // 圆锥曲线
    int others = 0;        // 其它（点等）
    int doneSkipped = 0;   // 已打 STEP9_DONE 的曲线（已转换，跳过）
};

//==============================================================================
// 云线几何（矩形/圆形 → 波浪采样点 → 周期样条）
//==============================================================================

/** 两点距离（图纸平面） */
static double dist2d(double ax, double ay, double bx, double by)
{
    double dx = ax - bx;
    double dy = ay - by;
    return sqrt(dx * dx + dy * dy);
}

/** 弧度转度 */
static double rad2deg(double rad) { return rad * 180.0 / kPI; }

/** 度转弧度 */
static double deg2rad(double deg) { return deg * kPI / 180.0; }

/**
 * @brief 追加一个半圆弧波浪的采样点（从起点到终点，凸向 (nx,ny)，弦长 = 2r）。
 * @param pts   输出点集（追加）
 * @param bx,by 波浪弦起点（图纸坐标）
 * @param ex,ey 波浪弦终点
 * @param nx,ny 凸起方向单位向量
 * @param r     半圆弧半径（= 弦长一半）
 * @param perWave 每波采样点数（不含终点，终点即下一波起点）
 */
static void append_wave_points(std::vector<CloudPt>& pts,
                               double bx, double by, double ex, double ey,
                               double nx, double ny, double r, int perWave)
{
    for (int i = 0; i < perWave; ++i)
    {
        double u = (double)i / perWave;              // 0 .. (k-1)/k
        double off = 2.0 * r * sqrt(u * (1.0 - u));  // 半圆弧凸起量，u=0.5 时 = r
        CloudPt p;
        p.x = bx + (ex - bx) * u + nx * off;
        p.y = by + (ey - by) * u + ny * off;
        pts.push_back(p);
    }
}

/**
 * @brief 生成以 (0,0) 为中心、边平行坐标轴的矩形云线点环（首波外凸、逐波交替内外）。
 * @param w,h   矩形宽/高（>0）
 * @param chord 每波弦长参考值（>0）
 * @param perWave 每波采样点数（>=6）
 * @param pts   输出点集（先清空再填充）
 */
static void generate_rect_cloud_local(double w, double h, double chord, int perWave,
                                      std::vector<CloudPt>& pts)
{
    pts.clear();
    if (w <= 0.0 || h <= 0.0 || chord <= 0.0 || perWave < 6) return;

    const double hw = w / 2.0, hh = h / 2.0;
    struct Side { double ax, ay, dx, dy, nx, ny; };
    // 下边 → 右边 → 上边 → 左边（顺时针，法向一律朝外）
    const Side sides[4] = {
        { -hw, -hh,  w, 0.0,  0.0, -1.0 },
        {  hw, -hh,  0.0, h,  1.0,  0.0 },
        {  hw,  hh, -w, 0.0,  0.0,  1.0 },
        { -hw,  hh,  0.0, -h, -1.0,  0.0 }
    };
    for (int s = 0; s < 4; ++s)
    {
        double len = (s % 2 == 0) ? w : h;             // 该边长度
        int nWaves = (int)floor(len / chord + 0.5);    // 该边波数（整数）
        if (nWaves < 2) nWaves = 2;
        double r = len / nWaves / 2.0;                 // 该边实际半圆弧半径
        for (int j = 0; j < nWaves; ++j)
        {
            double u0 = (double)j / nWaves;
            double u1 = (double)(j + 1) / nWaves;
            double bx = sides[s].ax + sides[s].dx * u0;
            double by = sides[s].ay + sides[s].dy * u0;
            double ex = sides[s].ax + sides[s].dx * u1;
            double ey = sides[s].ay + sides[s].dy * u1;
            double sign = ((j % 2) == 0) ? 1.0 : -1.0; // 首波外凸，逐波交替
            append_wave_points(pts, bx, by, ex, ey,
                               sides[s].nx * sign, sides[s].ny * sign, r, perWave);
        }
    }
}

/**
 * @brief 生成矩形云线点环（中心 + 宽/高 + 倾角，支持草图识别的非水平矩形）。
 * @param cx,cy 矩形中心
 * @param w,h   矩形宽（较长边）/高
 * @param angleDeg 矩形宽边与 +X 轴夹角（度）
 * @param chord / perWave 波浪参数
 * @param pts   输出点集
 */
static void generate_rect_cloud(double cx, double cy, double w, double h, double angleDeg,
                                double chord, int perWave, std::vector<CloudPt>& pts)
{
    std::vector<CloudPt> local;
    generate_rect_cloud_local(w, h, chord, perWave, local);
    double a = deg2rad(angleDeg);
    double ux = cos(a), uy = sin(a);   // 局部 X 轴（宽方向）
    double vx = -uy, vy = ux;          // 局部 Y 轴（X 轴逆时针 90°）
    pts.clear();
    for (const auto& p : local)
    {
        CloudPt q;
        q.x = cx + ux * p.x + vx * p.y;
        q.y = cy + uy * p.x + vy * p.y;
        pts.push_back(q);
    }
}

/**
 * @brief 生成圆形云线点环（圆周整数个波，首波外凸、逐波交替内外）。
 * @param cx,cy 圆心；R 半径（>0）
 * @param chord / perWave 波浪参数
 * @param pts   输出点集
 */
static void generate_circle_cloud(double cx, double cy, double R,
                                  double chord, int perWave, std::vector<CloudPt>& pts)
{
    pts.clear();
    if (R <= 0.0 || chord <= 0.0 || perWave < 6) return;

    double circ = 2.0 * kPI * R;
    int nWaves = (int)floor(circ / chord + 0.5);
    if (nWaves < 8) nWaves = 8;
    double r = circ / nWaves / 2.0;                    // 每波实际半圆弧半径
    for (int j = 0; j < nWaves; ++j)
    {
        double a0 = (double)j / nWaves * 2.0 * kPI;
        double a1 = (double)(j + 1) / nWaves * 2.0 * kPI;
        double sign = ((j % 2) == 0) ? 1.0 : -1.0;
        for (int i = 0; i < perWave; ++i)
        {
            double u = (double)i / perWave;
            double ang = a0 + (a1 - a0) * u;
            double off = 2.0 * r * sqrt(u * (1.0 - u)) * sign;
            double rr = R + off;
            CloudPt p;
            p.x = cx + rr * cos(ang);
            p.y = cy + rr * sin(ang);
            pts.push_back(p);
        }
    }
}

/**
 * @brief 识别出的形状 → 云线采样点（矩形/圆形统一分发）。
 * @return true=生成成功且非空
 */
static bool shape_to_points(const ShapeRec& s, double chord, int perWave,
                            std::vector<CloudPt>& pts)
{
    if (s.kind == ShapeRec::Kind::Rect)
    {
        generate_rect_cloud(s.cx, s.cy, s.width, s.height, s.angleDeg, chord, perWave, pts);
        return !pts.empty();
    }
    if (s.kind == ShapeRec::Kind::Circle)
    {
        generate_circle_cloud(s.cx, s.cy, s.radius, chord, perWave, pts);
        return !pts.empty();
    }
    pts.clear();
    return false;
}

/**
 * @brief 用采样点创建封闭周期样条（degree=3，periodicity=1，点首尾不重复）。
 * @param pts 采样点（>=8）
 * @param splineTag 输出样条 tag
 * @return true=创建成功
 */
static bool create_periodic_spline(const std::vector<CloudPt>& pts, tag_t* splineTag)
{
    int n = (int)pts.size();
    if (n < 8)
    {
        CommonUtils::print_msg("  [Step9] 采样点不足，无法创建云线样条");
        return false;
    }

    std::vector<UF_CURVE_pt_slope_crvatr_t> pd(n);
    for (int i = 0; i < n; ++i)
    {
        UF_CURVE_pt_slope_crvatr_t& d = pd[i];
        d.point[0] = pts[i].x;
        d.point[1] = pts[i].y;
        d.point[2] = 0.0;
        d.slope_type = UF_CURVE_SLOPE_NONE;
        d.slope[0] = d.slope[1] = d.slope[2] = 0.0;
        d.crvatr_type = UF_CURVE_CRVATR_NONE;
        d.crvatr[0] = d.crvatr[1] = d.crvatr[2] = 0.0;
    }

    int rc = UF_CURVE_create_spline_thru_pts(3, 1, n, &pd[0], nullptr, 0, splineTag);
    if (rc != 0)
    {
        char err[133] = { 0 };
        UF_get_fail_message(rc, err);
        char fmt[512];
        sprintf_s(fmt, sizeof(fmt),
                  "  [Step9] UF_CURVE_create_spline_thru_pts 失败 rc=%d (%s)", rc, err);
        CommonUtils::print_msg(fmt);
        return false;
    }
    return true;
}

/** 设置云线图层/颜色 */
static void style_cloud(tag_t tag)
{
    if (kCloudLayer >= 1 && kCloudLayer <= 256)
        UF_OBJ_set_layer(tag, kCloudLayer);
    if (kCloudColor >= 0)
        UF_OBJ_set_color(tag, kCloudColor);
}

//==============================================================================
// 幂等属性管理（云线 STEP9_CLOUD / 曲线 STEP9_DONE）
//==============================================================================

/** 云线属性读取结果 */
enum class CloudAttr { None, Legacy, Source };

/**
 * @brief 读取样条的 STEP9_CLOUD 属性。
 * @param tag 样条 tag
 * @param source 输出来源字符串（属性为字符串时）
 * @return None=无标记；Legacy=v1 整数属性旧云线；Source=字符串来源
 */
static CloudAttr read_cloud_attr(tag_t tag, std::string& source)
{
    source.clear();
    UF_ATTR_info_t info;
    UF_ATTR_init_user_attribute_info(&info);
    logical has = FALSE;
    int rc = UF_ATTR_get_user_attribute_with_title_and_type(
        tag, kAttrCloudTitle, UF_ATTR_any, UF_ATTR_NOT_ARRAY, &info, &has);
    CloudAttr result = CloudAttr::None;
    if (rc == 0 && has)
    {
        if (info.type == UF_ATTR_string)
        {
            if (info.string_value != nullptr) source = info.string_value;
            result = CloudAttr::Source;
        }
        else
        {
            result = CloudAttr::Legacy;   // v1 整数属性（或其它类型），按旧云线迁移清理
        }
    }
    UF_ATTR_free_user_attribute_info_strings(&info);
    return result;
}

/**
 * @brief 给云线样条写 STEP9_CLOUD 字符串属性（值=DEFAULT 或 SKETCH:<tag>）。
 * @param tag 样条 tag
 * @param source 来源字符串
 */
static void set_cloud_source(tag_t tag, const std::string& source)
{
    UF_ATTR_info_t info;
    UF_ATTR_init_user_attribute_info(&info);
    info.type = UF_ATTR_string;
    info.title = const_cast<char*>(kAttrCloudTitle);
    info.string_value = const_cast<char*>(source.c_str());
    UF_ATTR_set_user_attribute(tag, &info, TRUE);
}

/**
 * @brief 按谓词删除云线样条（先收集后删除，避免遍历中删除）。
 * @param part 工作部件
 * @param pred 谓词：输入(属性类型, 来源字符串)，返回 true 表示删除
 * @return 删除数量
 */
static int delete_clouds_where(NXOpen::Part* part,
                               const std::function<bool(CloudAttr, const std::string&)>& pred)
{
    std::vector<tag_t> doomed;
    tag_t obj = NULL_TAG;
    for (;;)
    {
        if (UF_OBJ_cycle_objs_in_part(part->Tag(), UF_spline_type, &obj) != 0) break;
        if (obj == NULL_TAG) break;
        std::string src;
        CloudAttr attr = read_cloud_attr(obj, src);
        if (attr != CloudAttr::None && pred(attr, src))
            doomed.push_back(obj);
    }
    int deleted = 0;
    for (auto t : doomed)
        if (UF_OBJ_delete_object(t) == 0) ++deleted;
    return deleted;
}

/** 删除 v1 遗留的整数属性旧云线（迁移清理） */
static int delete_legacy_clouds(NXOpen::Part* part)
{
    return delete_clouds_where(part, [](CloudAttr a, const std::string&)
    {
        return a == CloudAttr::Legacy;
    });
}

/** 删除指定来源的云线（重画同源前先删旧线） */
static int delete_clouds_by_source(NXOpen::Part* part, const std::string& source)
{
    return delete_clouds_where(part, [&source](CloudAttr a, const std::string& s)
    {
        return a == CloudAttr::Source && s == source;
    });
}

/** 默认云线来源标记 */
static std::string make_default_source() { return std::string("DEFAULT"); }

/** 草图云线来源标记 */
static std::string make_sketch_source(tag_t sketchTag)
{
    char buf[64];
    sprintf_s(buf, sizeof(buf), "SKETCH:%llu", (unsigned long long)sketchTag);
    return std::string(buf);
}

/**
 * @brief 判断草图曲线是否已转换（带 STEP9_DONE 整数属性）。
 * 曲线级幂等：已转换曲线跳过，新画的曲线总是会处理（草图本身不再打标记）。
 */
static bool is_curve_done(tag_t curveTag)
{
    if (curveTag == NULL_TAG) return false;
    UF_ATTR_info_t info;
    UF_ATTR_init_user_attribute_info(&info);
    logical has = FALSE;
    UF_ATTR_get_user_attribute_with_title_and_type(
        curveTag, kAttrDoneTitle, UF_ATTR_integer, UF_ATTR_NOT_ARRAY, &info, &has);
    UF_ATTR_free_user_attribute_info_strings(&info);
    return has != FALSE;
}

/** 给草图曲线打 STEP9_DONE 标记（云线创建成功后调用，先标记后删除） */
static void mark_curve_done(tag_t curveTag)
{
    if (curveTag == NULL_TAG) return;
    UF_ATTR_info_t info;
    UF_ATTR_init_user_attribute_info(&info);
    info.type = UF_ATTR_integer;
    info.title = const_cast<char*>(kAttrDoneTitle);
    info.integer_value = 1;
    UF_ATTR_set_user_attribute(curveTag, &info, TRUE);
}

/**
 * @brief 创建一条云线样条并设置样式与来源属性。
 * @param pts  采样点
 * @param kind 中文类型名（矩形/圆形，日志用）
 * @param sourceId 来源标记（DEFAULT 或 SKETCH:<tag>），写入 STEP9_CLOUD 字符串属性
 * @return 新样条 tag；失败返回 NULL_TAG
 */
static tag_t draw_one_cloud(const std::vector<CloudPt>& pts, const char* kind,
                            const std::string& sourceId)
{
    tag_t tag = NULL_TAG;
    char fmt[512];
    sprintf_s(fmt, sizeof(fmt), "[Step9] 正在生成%s 云线：采样点 %d 个，来源 %s ...",
              kind, (int)pts.size(), sourceId.c_str());
    CommonUtils::print_msg(fmt);
    if (!create_periodic_spline(pts, &tag))
        return NULL_TAG;
    style_cloud(tag);
    set_cloud_source(tag, sourceId);
    sprintf_s(fmt, sizeof(fmt), "[Step9] %s 云线已创建 tag=%llu（图层 %d，颜色 %d，来源 %s）",
              kind, (unsigned long long)tag, kCloudLayer, kCloudColor, sourceId.c_str());
    CommonUtils::print_msg(fmt);
    return tag;
}

//==============================================================================
// 草图枚举与几何分类
//==============================================================================

/**
 * @brief 收集当前工作图纸上的制图草图。
 * 过滤规则：Sketch::IsDraftingSketch() 为 true；且草图 View 与图纸 View 一致
 * （两者任一为 NULL 时按属于本图纸处理）。
 * @param part 工作部件
 * @param sheet 当前图纸
 * @param out 输出草图列表
 * @param total 部件内草图总数（日志用）
 * @param drafting 制图草图总数
 * @param otherSheet 属于其它图纸的制图草图数
 */
static void collect_sheet_sketches(NXOpen::Part* part,
                                   NXOpen::Drawings::DraftingDrawingSheet* sheet,
                                   std::vector<NXOpen::Sketch*>& out,
                                   int& total, int& drafting, int& otherSheet)
{
    out.clear();
    total = 0; drafting = 0; otherSheet = 0;
    tag_t sheetViewTag = (sheet != nullptr && sheet->View() != nullptr)
                         ? sheet->View()->Tag() : NULL_TAG;
    for (auto it = part->Sketches()->begin(); it != part->Sketches()->end(); ++it)
    {
        NXOpen::Sketch* sk = *it;
        if (sk == nullptr) continue;
        ++total;
        if (!sk->IsDraftingSketch()) continue;   // 建模草图不属于图纸，跳过
        ++drafting;
        tag_t skViewTag = (sk->View() != nullptr) ? sk->View()->Tag() : NULL_TAG;
        if (sheetViewTag != NULL_TAG && skViewTag != NULL_TAG && skViewTag != sheetViewTag)
        {
            ++otherSheet;                        // 挂在其它图纸上
            continue;
        }
        out.push_back(sk);
    }
}

/**
 * @brief 圆弧中心从圆弧自身 CSYS 换算到绝对坐标（图纸平面）。
 * 矩阵为单位阵（制图草图常态）时直接使用 arc_center；否则按矩阵旋转换算并提示。
 */
static bool map_arc_center(const UF_CURVE_arc_t& ad, double out[2])
{
    if (ad.matrix_tag == NULL_TAG)
    {
        out[0] = ad.arc_center[0]; out[1] = ad.arc_center[1];
        return true;
    }
    double m[9];
    if (UF_CSYS_ask_matrix_values(ad.matrix_tag, m) != 0)
    {
        out[0] = ad.arc_center[0]; out[1] = ad.arc_center[1];
        return true;
    }
    bool ident = fabs(m[0] - 1.0) <= kIdentityMatrixTolerance &&
                 fabs(m[4] - 1.0) <= kIdentityMatrixTolerance &&
                 fabs(m[8] - 1.0) <= kIdentityMatrixTolerance &&
                 fabs(m[1]) <= kIdentityMatrixTolerance &&
                 fabs(m[2]) <= kIdentityMatrixTolerance &&
                 fabs(m[3]) <= kIdentityMatrixTolerance &&
                 fabs(m[5]) <= kIdentityMatrixTolerance &&
                 fabs(m[6]) <= kIdentityMatrixTolerance &&
                 fabs(m[7]) <= kIdentityMatrixTolerance;
    if (ident)
    {
        out[0] = ad.arc_center[0]; out[1] = ad.arc_center[1];
        return true;
    }
    out[0] = m[0] * ad.arc_center[0] + m[1] * ad.arc_center[1] + m[2] * ad.arc_center[2];
    out[1] = m[3] * ad.arc_center[0] + m[4] * ad.arc_center[1] + m[5] * ad.arc_center[2];
    CommonUtils::print_msg("  [Step9] 提示：圆弧矩阵非单位阵，已按矩阵换算圆心坐标");
    return true;
}

/**
 * @brief 枚举一个草图内的几何并按类型分类（直线/圆/圆弧/样条/圆锥/其它）。
 * 曲线端点/圆心取自 UF 原始 tag 通道：直线端点即绝对坐标；
 * 圆弧中心按矩阵换算到绝对坐标（矩阵为单位阵时直接使用）。
 * @param sk 草图
 * @param lines 输出直线列表
 * @param arcs 输出候选圆列表（完整圆或扫略角 >= kMinArcSweepDeg 的圆弧）
 * @param stats 输出统计（诊断日志）
 */
static void collect_sketch_geometry(NXOpen::Sketch* sk,
                                    std::vector<LineRec>& lines,
                                    std::vector<ArcRec>& arcs,
                                    SketchGeomStats& stats)
{
    lines.clear();
    arcs.clear();
    stats = SketchGeomStats();
    std::vector<NXOpen::NXObject*> geoms = sk->GetAllGeometry();
    for (auto* obj : geoms)
    {
        if (obj == nullptr) continue;
        tag_t tag = obj->Tag();
        int t = 0, sub = 0;
        if (UF_OBJ_ask_type_and_subtype(tag, &t, &sub) != 0) { ++stats.others; continue; }
        if (is_curve_done(tag)) { ++stats.doneSkipped; continue; }   // 曲线级幂等：已转换曲线跳过

        if (t == UF_line_type)
        {
            UF_CURVE_line_t ld;
            if (UF_CURVE_ask_line_data(tag, &ld) != 0) { ++stats.others; continue; }
            LineRec lr;
            lr.tag = tag;
            lr.sx = ld.start_point[0]; lr.sy = ld.start_point[1];
            lr.ex = ld.end_point[0];   lr.ey = ld.end_point[1];
            lines.push_back(lr);
            ++stats.lines;
        }
        else if (t == UF_circle_type)
        {
            UF_CURVE_arc_t ad;
            if (UF_CURVE_ask_arc_data(tag, &ad) != 0) { ++stats.others; continue; }
            double sweep = ad.end_angle - ad.start_angle;
            if (sweep < 0.0) sweep += 2.0 * kPI;
            double fullTol = deg2rad(kFullCircleToleranceDeg);
            bool full = fabs(sweep - 2.0 * kPI) <= fullTol;
            double sweepDeg = rad2deg(sweep);
            if (!full && sweepDeg < (double)kMinArcSweepDeg) { ++stats.smallArcs; continue; }
            double cc[2];
            if (!map_arc_center(ad, cc)) { ++stats.others; continue; }
            ArcRec ar;
            ar.tag = tag; ar.cx = cc[0]; ar.cy = cc[1]; ar.r = ad.radius;
            ar.sweepDeg = sweepDeg; ar.full = full;
            arcs.push_back(ar);
            if (full) ++stats.circles; else ++stats.arcCandidates;
        }
        else if (t == UF_spline_type) { ++stats.splines; }
        else if (t == UF_conic_type)  { ++stats.conics; }
        else                          { ++stats.others; }
    }
}

//==============================================================================
// 矩形/圆形识别
//==============================================================================

/** 点距（容差用） */
static bool points_close(double ax, double ay, double bx, double by, double tol)
{
    return dist2d(ax, ay, bx, by) <= tol;
}

/**
 * @brief 在直线集中寻找一个 4 线闭合矩形（首条命中即返回，命中的线标记已用）。
 * 判据：端点首尾闭合（容差 kEndPointTolerance）；对边等长（kRectLengthTolerance）
 * 且平行、邻边垂直（kParallelAngleToleranceDeg）。
 * @param lines 直线列表
 * @param used  已用直线 tag 集合（命中后写入 4 条）
 * @param out   输出中心/宽/高/倾角
 * @return true=识别成功
 */
static bool detect_rectangle(const std::vector<LineRec>& lines, std::set<tag_t>& used,
                             RectInfo& out)
{
    const double tol = kEndPointTolerance;
    const int n = (int)lines.size();
    for (int i = 0; i < n; ++i)
    {
        if (used.count(lines[i].tag) != 0) continue;
        for (int dirStart = 0; dirStart < 2; ++dirStart)   // 两个起始方向都试
        {
            int chain[4];
            chain[0] = i;
            std::set<tag_t> visited;
            visited.insert(lines[i].tag);
            double curX = (dirStart == 0) ? lines[i].ex : lines[i].sx;
            double curY = (dirStart == 0) ? lines[i].ey : lines[i].sy;
            double startX = (dirStart == 0) ? lines[i].sx : lines[i].ex;
            double startY = (dirStart == 0) ? lines[i].sy : lines[i].ey;
            bool ok = true;
            int cnt = 1;
            while (cnt < 4)
            {
                int next = -1;
                bool rev = false;
                for (int j = 0; j < n; ++j)
                {
                    if (j == i || visited.count(lines[j].tag) != 0) continue;
                    if (points_close(lines[j].sx, lines[j].sy, curX, curY, tol)) { next = j; rev = false; break; }
                    if (points_close(lines[j].ex, lines[j].ey, curX, curY, tol)) { next = j; rev = true; break; }
                }
                if (next < 0) { ok = false; break; }
                chain[cnt] = next;
                visited.insert(lines[next].tag);
                ++cnt;
                if (rev) { curX = lines[next].sx; curY = lines[next].sy; }
                else     { curX = lines[next].ex; curY = lines[next].ey; }
            }
            if (!ok) continue;
            if (!points_close(curX, curY, startX, startY, tol)) continue;   // 闭合失败

            // 四边形状校验
            double dx[4], dy[4], len[4];
            for (int k = 0; k < 4; ++k)
            {
                const LineRec& L = lines[chain[k]];
                dx[k] = L.ex - L.sx;
                dy[k] = L.ey - L.sy;
                len[k] = sqrt(dx[k] * dx[k] + dy[k] * dy[k]);
                if (len[k] < tol) { ok = false; break; }
            }
            if (!ok) continue;

            // 对边：与第 0 条平行且等长的边（正方形四边等长也适用）
            double angTol = sin(deg2rad(kParallelAngleToleranceDeg));
            int opp0 = -1;
            for (int k = 1; k < 4; ++k)
            {
                double d0k = dx[0] * dx[k] + dy[0] * dy[k];
                if (fabs(fabs(d0k) - len[0] * len[k]) <= angTol * len[0] * len[k]) { opp0 = k; break; }
            }
            if (opp0 < 0) continue;
            if (fabs(len[0] - len[opp0]) > kRectLengthTolerance * len[0]) continue;
            int a1 = -1, a2 = -1;
            for (int k = 1; k < 4; ++k)
                if (k != opp0) { if (a1 < 0) a1 = k; else a2 = k; }
            if (a1 < 0 || a2 < 0) continue;
            if (fabs(len[a1] - len[a2]) > kRectLengthTolerance * len[a1]) continue;
            double d12 = dx[a1] * dx[a2] + dy[a1] * dy[a2];
            if (fabs(fabs(d12) - len[a1] * len[a2]) > angTol * len[a1] * len[a2]) continue;
            double dAdj = dx[0] * dx[a1] + dy[0] * dy[a1];
            if (fabs(dAdj) > angTol * len[0] * len[a1]) continue;

            // 中心 = 8 个端点平均
            double cx = 0.0, cy = 0.0;
            for (int k = 0; k < 4; ++k)
            {
                const LineRec& L = lines[chain[k]];
                cx += L.sx + L.ex;
                cy += L.sy + L.ey;
            }
            cx /= 8.0;
            cy /= 8.0;

            // 宽 = 较长组，高 = 较短组；倾角取宽组方向（归一化到 (-90°, 90°]）
            double w, h, ang;
            if (len[0] >= len[a1]) { w = len[0]; h = len[a1]; ang = atan2(dy[0], dx[0]); }
            else                   { w = len[a1]; h = len[0]; ang = atan2(dy[a1], dx[a1]); }
            double angleDeg = rad2deg(ang);
            while (angleDeg > 90.0)  angleDeg -= 180.0;
            while (angleDeg <= -90.0) angleDeg += 180.0;

            out.cx = cx; out.cy = cy;
            out.width = w; out.height = h; out.angleDeg = angleDeg;
            out.lineTags.clear();
            for (int k = 0; k < 4; ++k)
            {
                used.insert(lines[chain[k]].tag);
                out.lineTags.push_back(lines[chain[k]].tag);   // 记录来源直线，云线成功后删除
            }
            return true;
        }
    }
    return false;
}

/**
 * @brief 从候选圆列表中提取圆形（按圆心+半径去重）。
 */
static void detect_circles(const std::vector<ArcRec>& arcs, std::vector<CircleInfo>& out)
{
    out.clear();
    for (const auto& a : arcs)
    {
        if (a.r <= 0.0) continue;
        bool dup = false;
        for (const auto& c : out)
        {
            if (dist2d(a.cx, a.cy, c.cx, c.cy) <= kEndPointTolerance &&
                fabs(a.r - c.radius) <= kEndPointTolerance)
            {
                dup = true;
                break;
            }
        }
        if (dup) continue;
        CircleInfo c;
        c.cx = a.cx; c.cy = a.cy; c.radius = a.r; c.tag = a.tag;
        out.push_back(c);
    }
}

/**
 * @brief 识别草图内的矩形与圆形（优先矩形，再圆形）。
 * @param sk 草图
 * @param out 输出识别出的形状
 * @return 识别出的形状数量
 */
static int recognize_sketch_shapes(NXOpen::Sketch* sk, std::vector<ShapeRec>& out)
{
    out.clear();
    std::vector<LineRec> lines;
    std::vector<ArcRec> arcs;
    SketchGeomStats stats;
    collect_sketch_geometry(sk, lines, arcs, stats);

    char fmt[512];
    sprintf_s(fmt, sizeof(fmt),
              "[Step9 草图] 名称 %s tag=%llu 几何统计：直线 %d / 完整圆 %d / 大圆弧 %d / 小圆弧 %d / 样条 %d / 圆锥 %d / 其它 %d / 已转换跳过 %d",
              sk->Name().GetUTF8Text(), (unsigned long long)sk->Tag(),
              stats.lines, stats.circles, stats.arcCandidates, stats.smallArcs,
              stats.splines, stats.conics, stats.others, stats.doneSkipped);
    CommonUtils::print_msg(fmt);

    std::set<tag_t> used;
    RectInfo ri;
    while (detect_rectangle(lines, used, ri))
    {
        ShapeRec s;
        s.kind = ShapeRec::Kind::Rect;
        s.cx = ri.cx; s.cy = ri.cy;
        s.width = ri.width; s.height = ri.height;
        s.angleDeg = ri.angleDeg;
        s.radius = 0.0;
        s.sourceTags = ri.lineTags;
        out.push_back(s);
        sprintf_s(fmt, sizeof(fmt),
                  "[Step9 识别] 矩形：中心(%.2f, %.2f) 宽 %.2f 高 %.2f 倾角 %.2f 度（4 条直线闭合）",
                  ri.cx, ri.cy, ri.width, ri.height, ri.angleDeg);
        CommonUtils::print_msg(fmt);
    }

    std::vector<CircleInfo> circles;
    detect_circles(arcs, circles);
    for (const auto& c : circles)
    {
        ShapeRec s;
        s.kind = ShapeRec::Kind::Circle;
        s.cx = c.cx; s.cy = c.cy;
        s.radius = c.radius;
        s.width = 0.0; s.height = 0.0; s.angleDeg = 0.0;
        s.sourceTags.clear();
        s.sourceTags.push_back(c.tag);
        out.push_back(s);
        sprintf_s(fmt, sizeof(fmt),
                  "[Step9 识别] 圆：圆心(%.2f, %.2f) 半径 %.2f", c.cx, c.cy, c.radius);
        CommonUtils::print_msg(fmt);
    }
    return (int)out.size();
}

/**
 * @brief 云线生成成功后删除参考草图几何（矩形 4 条直线 / 圆或大圆弧曲线）。
 * 草图曲线必须通过 Sketch::DeleteObjects 删除（会同时清理相关约束），
 * 直接 UF_OBJ_delete_object 会被 NX 拒绝；由配置开关 kDeleteSourceGeometry
 * 控制（默认开启），草图本身保留，失败保留几何并记录日志。
 * @param sk 来源草图
 * @param s 已生成云线的形状（携带来源曲线 tag）
 * @return 实际删除的曲线数
 */
static int delete_shape_sources(NXOpen::Sketch* sk, const ShapeRec& s)
{
    if (!kDeleteSourceGeometry || s.sourceTags.empty()) return 0;
    if (sk == nullptr)
    {
        CommonUtils::print_msg("  [Step9] 警告：草图指针为空，跳过参考几何删除");
        return 0;
    }
    std::vector<NXOpen::NXObject*> objs;
    for (auto t : s.sourceTags)
    {
        if (t == NULL_TAG) continue;
        NXOpen::TaggedObject* to = NXOpen::NXObjectManager::Get(t);   // NX12 返回 TaggedObject*
        NXOpen::NXObject* o = dynamic_cast<NXOpen::NXObject*>(to);
        if (o != nullptr) objs.push_back(o);
    }
    if (objs.empty()) return 0;

    int errors = 0;
    try
    {
        NXOpen::ErrorList* errList = sk->DeleteObjects(objs);
        if (errList != nullptr)
        {
            errors = errList->Length();
            delete errList;
        }
    }
    catch (const NXOpen::NXException& e)
    {
        CommonUtils::print_msg(std::string("  [Step9] 警告：Sketch::DeleteObjects 异常: ") + e.Message());
        errors = (int)objs.size();
    }
    catch (...)
    {
        CommonUtils::print_msg("  [Step9] 警告：Sketch::DeleteObjects 未知异常，参考几何保留");
        errors = (int)objs.size();
    }
    if (errors > 0)
    {
        std::string msg = "  [Step9] 警告：参考几何删除失败 ";
        msg += std::to_string(errors);
        msg += " 条（保留在草图中）";
        CommonUtils::print_msg(msg.c_str());
    }
    return (int)objs.size() - errors;
}

//==============================================================================
// do_it —— Step9 入口：制图 → 当前图纸 → 草图识别 → 云线生成（无草图回退默认）
//==============================================================================
static void do_it()
{
    try
    {
        // ===== 进入制图环境 =====
        NXOpen::Part* part = dynamic_cast<NXOpen::Part*>(CommonUtils::get_session()->Parts()->BaseWork());
        if (part == nullptr) { CommonUtils::print_msg("错误：无工作部件"); return; }

        CommonUtils::get_session()->ApplicationSwitchImmediate("UG_APP_DRAFTING");
        part->Drafting()->EnterDraftingApplication();
        CommonUtils::print_msg("[Step9] 已进入制图模块");

        // ===== 获取当前工作/活动图纸 =====
        NXOpen::Drawings::DraftingDrawingSheet* sheet =
            part->DraftingDrawingSheets()->CurrentDrawingSheet();
        if (sheet == nullptr)
        {
            tag_t curTag = NULL_TAG;
            if (UF_DRAW_ask_current_drawing(&curTag) == 0 && curTag != NULL_TAG)
                sheet = dynamic_cast<NXOpen::Drawings::DraftingDrawingSheet*>(
                    NXOpen::NXObjectManager::Get(curTag));
        }
        if (sheet == nullptr)
        {
            CommonUtils::print_msg("[Step9] 警告：当前未激活任何图纸，请先在制图环境中打开目标图纸再运行");
            return;
        }
        sheet->Open();
        {
            char fmt[512];
            sprintf_s(fmt, sizeof(fmt), "[Step9] 当前工作图纸 %s", sheet->Name().GetUTF8Text());
            CommonUtils::print_msg(fmt);
        }

        // ===== 云线参数对话框：波浪直径（每波弦长）控制云线疏密 =====
        CloudDlg::Result dlgRes;
        if (!CloudDlg::Show(kWaveChordDefault, dlgRes))
        {
            CommonUtils::print_msg("[Step9] 用户取消云线参数对话框，本次不画云线（旧云线保留）");
            return;
        }
        const double waveChord = dlgRes.waveChord;
        {
            char fmt[512];
            sprintf_s(fmt, sizeof(fmt), "[Step9] 本次波浪直径（每波弦长）= %.2f mm（默认 %.2f mm，越小云线越密）",
                      waveChord, (double)kWaveChordDefault);
            CommonUtils::print_msg(fmt);
        }

        // ===== v1 → v2 迁移：清理旧版整数属性云线 =====
        {
            int removed = delete_legacy_clouds(part);
            char fmt[512];
            sprintf_s(fmt, sizeof(fmt), "[Step9] 迁移清理：删除旧版(v1)云线 %d 条", removed);
            CommonUtils::print_msg(fmt);
        }

        int sketchClouds = 0;
        if (kSketchDriven)
        {
            // ===== 草图驱动：扫描当前图纸上的制图草图 =====
            std::vector<NXOpen::Sketch*> sketches;
            int total = 0, drafting = 0, otherSheet = 0;
            collect_sheet_sketches(part, sheet, sketches, total, drafting, otherSheet);
            {
                char fmt[512];
                sprintf_s(fmt, sizeof(fmt),
                          "[Step9 草图枚举] 部件草图共 %d 个：制图草图 %d 个（本图纸 %d 个，其它图纸 %d 个）",
                          total, drafting, (int)sketches.size(), otherSheet);
                CommonUtils::print_msg(fmt);
            }

            int processed = 0;
            for (auto* sk : sketches)
            {
                if (sk == nullptr) continue;
                try
                {
                    std::vector<ShapeRec> shapes;
                    int found = recognize_sketch_shapes(sk, shapes);
                    if (found <= 0)
                    {
                        CommonUtils::print_msg("  [Step9 草图] 未识别到新的矩形或圆形（已转换曲线自动跳过），本次无需处理");
                        continue;
                    }

                    // 曲线级幂等：新曲线生成新云线，旧云线保留；不再按草图整批删除
                    std::string src = make_sketch_source(sk->Tag());

                    int ok = 0;
                    for (const auto& s : shapes)
                    {
                        std::vector<CloudPt> pts;
                        if (!shape_to_points(s, waveChord, kPointsPerWave, pts))
                        {
                            CommonUtils::print_msg("  [Step9 草图] 形状参数无效，跳过该形状");
                            continue;
                        }
                        const char* kind = (s.kind == ShapeRec::Kind::Rect) ? "矩形" : "圆形";
                        if (draw_one_cloud(pts, kind, src) != NULL_TAG)
                        {
                            ++ok;
                            for (auto t : s.sourceTags) mark_curve_done(t);   // 先标记，防删除失败时重复转换
                            int removed = delete_shape_sources(sk, s);       // 成功后删除参考几何（草图 API）
                            if (removed > 0)
                            {
                                std::string msg = "  [Step9] 云线生成成功，已删除参考";
                                msg += kind;
                                msg += "几何 ";
                                msg += std::to_string(removed);
                                msg += " 条（开关 kDeleteSourceGeometry）";
                                CommonUtils::print_msg(msg.c_str());
                            }
                        }
                    }

                    if (ok > 0)
                    {
                        ++processed;
                        sketchClouds += ok;
                        char fmt[512];
                        sprintf_s(fmt, sizeof(fmt), "  [Step9 草图] 转换完成：新生成云线 %d 条（旧云线保留，草图可继续加图形复用）", ok);
                        CommonUtils::print_msg(fmt);
                    }
                }
                catch (const NXOpen::NXException& e)
                {
                    CommonUtils::print_msg(std::string("  [Step9 草图] NXException: ") + e.Message());
                }
                catch (const std::exception& e)
                {
                    CommonUtils::print_msg(std::string("  [Step9 草图] Exception: ") + e.what());
                }
                catch (...)
                {
                    CommonUtils::print_msg("  [Step9 草图] Unknown Exception（已跳过该草图）");
                }
            }
            {
                char fmt[512];
                sprintf_s(fmt, sizeof(fmt),
                          "[Step9 草图驱动] 处理草图 %d 个，新生成云线 %d 条（曲线级幂等，草图可复用）",
                          processed, sketchClouds);
                CommonUtils::print_msg(fmt);
            }
        }

        // ===== 向后兼容回退：无可识别草图时使用默认参数（v1 行为） =====
        if (sketchClouds == 0)
        {
            char fmt[512];
            sprintf_s(fmt, sizeof(fmt),
                      "[Step9] 未从草图生成任何云线，回退默认配置参数（kFallback*，模式=%d）",
                      kFallbackCloudMode);
            CommonUtils::print_msg(fmt);

            int removed = delete_clouds_by_source(part, make_default_source());
            if (removed > 0)
            {
                sprintf_s(fmt, sizeof(fmt), "[Step9] 旧默认云线已删除 %d 条，即将重画", removed);
                CommonUtils::print_msg(fmt);
            }

            int created = 0;
            std::vector<CloudPt> pts;
            if (kFallbackCloudMode != 2)
            {
                generate_rect_cloud(kFallbackRectCenterX, kFallbackRectCenterY,
                                    kFallbackRectWidth, kFallbackRectHeight, 0.0,
                                    waveChord, kPointsPerWave, pts);
                if (!pts.empty() && draw_one_cloud(pts, "矩形(默认)", make_default_source()) != NULL_TAG)
                    ++created;
            }
            if (kFallbackCloudMode != 1)
            {
                generate_circle_cloud(kFallbackCircleCenterX, kFallbackCircleCenterY,
                                      kFallbackCircleRadius, waveChord, kPointsPerWave, pts);
                if (!pts.empty() && draw_one_cloud(pts, "圆形(默认)", make_default_source()) != NULL_TAG)
                    ++created;
            }
            if (created == 0)
            {
                CommonUtils::print_msg("[Step9] 未创建任何云线，请检查配置区参数");
                return;
            }
        }

        // ===== 静默更新（云线进入显示） =====
        CommonUtils::silent_update(CommonUtils::get_session());

        CommonUtils::print_msg("========== Step9 云线绘制 完成 ==========");
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
