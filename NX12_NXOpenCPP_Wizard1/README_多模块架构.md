# NX12 壳体出图插件 —— 多模块架构

## 1. 概述

本项目将原单一 `NX12_NXOpenCPP_Wizard1.cpp` 拆分为 **9 个独立 DLL** + **共享基础设施**，实现功能模块化：

- 每个 DLL 可独立通过 **Ctrl+U**（文件 → 执行 → NX Open）加载执行
- 各模块通过当前图纸 / 视图对象在运行时传递上下文，彼此解耦
- 原始 `NX12_NXOpenCPP_Wizard1` 项目保留在解决方案中，作为回退基线（不参与构建）

---

## 2. 项目结构

```
e:\UG\NX12_NXOpenCPP_Wizard1\
├── Shared\                          # 共享基础设施
│   ├── NX12_CommonConfig.h          # 配置结构体（ShellDrawingConfig / SheetDesc）
│   ├── NX12_CommonUtils.h           # 工具函数声明
│   ├── NX12_CommonUtils.cpp         # 工具函数实现
│   └── NX12_CommonUtils.vcxproj     # 静态库项目
├── NX12_Common.props                # MSBuild 共享属性（编译/链接配置）
├── Step1_SheetAndViews\              # DLL 1 — 创建图纸与视图
│   └── NX12_Step1_SheetAndViews.cpp
├── Step2_SheetPreferences\           # DLL 2 — 图纸首选项
│   └── NX12_Step2_SheetPreferences.cpp
├── Step3_OrdinateDimensions\         # DLL 3 — 坐标标注
│   └── NX12_Step3_OrdinateDimensions.cpp
├── Step4_LinearDimensions\           # DLL 4 — 线性/直径标注
│   └── NX12_Step4_LinearDimensions.cpp
├── Step5_Centerlines\                # DLL 5 — 中心线
│   └── NX12_Step5_Centerlines.cpp
├── Step6_LayerSwitch\                # DLL 6 — 图层切换
│   └── NX12_Step6_LayerSwitch.cpp
├── Step7_AppendSuffix\               # DLL 7 — 尺寸后缀标号
│   └── NX12_Step7_AppendSuffix.cpp
├── Step8_TitleBlockFill\              # DLL 8 — 标题框日期/零件编号填写
│   └── NX12_Step8_TitleBlockFill.cpp
├── Step9_CloudLines\                  # DLL 9 — 云线（矩形/圆形修订云线）
│   └── NX12_Step9_CloudLines.cpp
├── NX12_NXOpenCPP_Wizard1\           # 原始项目（基线，不参与构建）
├── NX12_MultiModule.sln              # 主解决方案
├── build_all.bat                     # 一键构建脚本
└── bin\Release\                      # 统一输出目录（9 个 DLL + 1 个 .lib）
```

---

## 3. 各 DLL 功能与执行顺序

| 顺序 | DLL | 功能 | 依赖 |
|:----:|-----|------|------|
| 1 | `NX12_Step1_SheetAndViews.dll` | 创建图纸（A4 横向）、载体基础视图、A-A 全剖视图、两步法精确定位、剖视比例 | 无（首先运行） |
| 2 | `NX12_Step2_SheetPreferences.dll` | 图纸首选项：尺寸文本字体/大小、视图标签样式 | Step1（需有图纸） |
| 5 | `NX12_Step5_Centerlines.dll` | 交互拾取投影边生成中心线 | Step1（需有剖视图） |
| 3 | `NX12_Step3_OrdinateDimensions.dll` | 点选一条线段自动成链 + 带捕捉点选原点 + 对话框指定方向/间隔，按坐标值排序链式生成坐标标注 | Step1（需有剖视图） |
| 4 | `NX12_Step4_LinearDimensions.dll` | 交互拾取边生成线性/直径标注 | Step1（需有剖视图） |
| 6 | `NX12_Step6_LayerSwitch.dll` | 工序图层切换对话框 | 无（独立运行） |
| 7 | `NX12_Step7_AppendSuffix.dll` | 剖视图尺寸按 D/L 分组追加 (D1)/(L1) 后缀 | Step1（需有剖视图） |
| 8 | `NX12_Step8_TitleBlockFill.dll` | 标题框“日期”/“零件编号”自动填写 | Step1（需有图纸/标题框） |
| 9 | `NX12_Step9_CloudLines.dll` | 扫描当前图纸草图自动生成矩形/圆形云线（修订云线）；无草图回退默认配置 | Step1（需有打开图纸 + 制图草图） |

**推荐执行顺序：** Step1 → Step5 → Step3 → Step4 → Step2 → Step6 → Step7 → Step8 → Step9

> Step7/Step8/Step9 均在 Step1 之后任意时机独立执行（Step9 要求目标图纸处于打开/显示状态）。

> **说明：** Step3（v2 交互式）坐标原点由用户带捕捉点选，不再依赖 Step5 中心线，可在 Step1 之后任意时机运行。Step2 和 Step6 可在任意时机独立执行。

---

## 4. 构建方法

### 一键构建

双击 `build_all.bat` 即可构建所有模块。

### Visual Studio 构建

打开 `NX12_MultiModule.sln`，在 VS2017 中构建。

### 环境要求

| 项目 | 要求 |
|------|------|
| 编译器 | Visual Studio 2017（v141 工具集） |
| NX SDK | `D:\Program Files\Siemens\NX 12.0\UGOPEN` |
| 配置 | Release \| x64 |
| MFC | 动态链接（Dynamic） |
| 字符集 | Unicode |

### 输出

构建产物输出到 `bin\Release\` 目录：

- 9 个 DLL（`NX12_Step1_SheetAndViews.dll` ~ `NX12_Step9_CloudLines.dll`）
- 1 个静态库（`NX12_CommonUtils.lib`）

---

## 5. 使用方法

### 加载执行

在 NX12 中，通过以下方式加载 DLL：

- 快捷键 **Ctrl+U**
- 菜单：**文件 → 执行 → NX Open**

每个 DLL 独立运行，通过当前图纸 / 视图对象传递上下文。

### 配置修改

- **各 DLL 的 cpp 文件**：顶部配置区可调整该模块的运行时行为
- **`Shared\NX12_CommonConfig.h`**：`ShellDrawingConfig` 结构体中的全局配置（模板路径、视图参数、标注开关等）

---

## 6. 共享基础设施说明

### NX12_CommonConfig.h

共享配置头文件，定义了两个核心结构体：

- **`ShellDrawingConfig`**：全局出图配置
  - 模板路径 / 零件路径
  - 图纸参数（A4 横向：297 × 210）
  - 基础视图参数（建模视图名、放置点）
  - A-A 全剖视图参数（剖切线半长、目标中心、比例、标签）
  - 功能开关：中心线创建、自动坐标标注、尺寸标注
  - 工序图层映射（最多 `MAX_PROCESS_COUNT = 8` 道工序）

- **`SheetDesc`**：单张图纸描述
  - 图纸名称（幂等匹配依据）
  - 图幅尺寸、比例

### NX12_CommonUtils.h / NX12_CommonUtils.cpp

共享工具函数库（编译为静态库）：

- `print_msg` — 日志输出（同时写入 NX 信息窗口和 Listing Window）
- `silent_update` — 静默更新（抑制视图刷新的批量操作）
- `center_view` — 两步法精确定位（先粗定位再精确对齐到目标坐标）
- `find_section_view` — 运行时按名称查找剖视图
- `find_centerline` — 运行时查找中心线对象

### NX12_Common.props

MSBuild 共享属性文件，统一所有项目的编译链接配置：

- 平台工具集：v141
- Include 路径：`D:\Program Files\Siemens\NX 12.0\UGOPEN`
- 链接库：50+ NXOpen 库（`libufun.lib`、`libnxopencpp.lib`、`libnxopencpp_drafting.lib` 等）
- 目标平台：x64

---

## 7. 回退方案

- 原始 `NX12_NXOpenCPP_Wizard1` 项目保留在解决方案中，**不参与构建**
- 如需回退，在 VS 中右键该项目 → 生成，即可恢复单一插件模式
- 原始项目包含完整的单文件实现，功能等价于所有 Step 模块的合集

---

## 8. 注意事项

1. **Step3 交互式（v2）**：先点选一条投影线段（沿相连轮廓自动成链，确认框可重选），再带捕捉点选坐标原点（端点/中点/圆心），对话框指定测量方向（垂直=测X / 水平=测Y）与排列间隔（mm）；每条线段两端点按坐标值升序排序后链式生成；重复运行会追加标注（仅告警不拦截），请自查重复标注；**Step3 只作用于当前活动图纸、绝不切换图纸**（当前图纸无剖视图时提示手动激活，不会自动去找别的图纸）
2. **Step1 幂等保护**：同名图纸复用（不会重复创建）、已有视图跳过，可安全重复执行
3. **即时卸载**：每个 DLL 加载后立即卸载（`LibraryUnloadOptionImmediately`），不驻留内存
4. **windows.h 防护**：每个需要对话框的 DLL 保留 `WIN32_LEAN_AND_MEAN`、`NOMINMAX`、`#undef CreateDialog`，避免与 NXOpen 头文件冲突
5. **Step9 云线说明（v7 草图驱动 + 直径对话框 + 参考几何自动删除 + 草图复用）**：NX12 制图模块无原生“云线”命令，Step9 用封闭周期样条（`UF_CURVE_create_spline_thru_pts`，degree=3、periodicity=1）拟合波浪半圆弧；优先扫描当前图纸上的制图草图（`Sketch::IsDraftingSketch` + 图纸视图匹配），自动识别 4 直线闭合矩形与完整圆/大圆弧并继承其位置尺寸生成云线；无可用草图时回退 `kFallback*` 默认配置（v1 行为）；幂等标记：草图 `STEP9_SKETCH`（不重复处理）、云线 `STEP9_CLOUD`（值 `DEFAULT` / `SKETCH:<tag>`，重画同源先删旧线，v1 整数属性旧云线自动迁移清理）；波浪直径（云线疏密）运行时由“云线参数”对话框输入（默认 8.0 mm，配置常量 `kWaveChordDefault` 为默认值，取消对话框则本次不画）；云线生成成功后自动删除参考草图几何（开关 `kDeleteSourceGeometry`，默认开启，改为 false 可保留）；曲线级幂等（`STEP9_DONE`）：同一草图可反复加画矩形/圆形重复运行，新图形总是被转换、旧云线保留；其余参数与容差在 `Step9_CloudLines\NX12_Step9_CloudLines.cpp` 顶部配置区调整
6. **Step9 排障**：若信息窗口显示“已创建”但图纸上看不到云线（个别环境把曲线建到了模型空间），先核对云线图层（默认 1）是否被隐藏、识别出的坐标是否落在图幅内；草图已打 `STEP9_SKETCH` 标记后如需随草图修改重画，把 `kReprocessMarkedSketches` 改为 true 重跑（或删除草图上的该属性）
5. **配置集中管理**：换零件或换模板时，只需修改 `NX12_CommonConfig.h` 中的配置值
