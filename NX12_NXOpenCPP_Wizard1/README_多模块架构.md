# NX12 壳体出图插件 —— 多模块架构

## 1. 概述

本项目将原单一 `NX12_NXOpenCPP_Wizard1.cpp` 拆分为 **6 个独立 DLL** + **共享基础设施**，实现功能模块化：

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
├── NX12_NXOpenCPP_Wizard1\           # 原始项目（基线，不参与构建）
├── NX12_MultiModule.sln              # 主解决方案
├── build_all.bat                     # 一键构建脚本
└── bin\Release\                      # 统一输出目录（6 个 DLL + 1 个 .lib）
```

---

## 3. 各 DLL 功能与执行顺序

| 顺序 | DLL | 功能 | 依赖 |
|:----:|-----|------|------|
| 1 | `NX12_Step1_SheetAndViews.dll` | 创建图纸（A4 横向）、载体基础视图、A-A 全剖视图、两步法精确定位、剖视比例 | 无（首先运行） |
| 2 | `NX12_Step2_SheetPreferences.dll` | 图纸首选项：尺寸文本字体/大小、视图标签样式 | Step1（需有图纸） |
| 5 | `NX12_Step5_Centerlines.dll` | 交互拾取投影边生成中心线 | Step1（需有剖视图） |
| 3 | `NX12_Step3_OrdinateDimensions.dll` | 自动枚举剖视轮廓端点，成组水平/垂直坐标标注 | Step1 + Step5（需剖视图 + 中心线） |
| 4 | `NX12_Step4_LinearDimensions.dll` | 交互拾取边生成线性/直径标注 | Step1（需有剖视图） |
| 6 | `NX12_Step6_LayerSwitch.dll` | 工序图层切换对话框 | 无（独立运行） |

**推荐执行顺序：** Step1 → Step5 → Step3 → Step4 → Step2 → Step6

> **说明：** Step5 需在 Step3 之前运行，因为 Step3 的坐标标注以 Step5 创建的中心线作为基准。Step2 和 Step6 可在任意时机独立执行。

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

- 6 个 DLL（`NX12_Step1_SheetAndViews.dll` ~ `NX12_Step6_LayerSwitch.dll`）
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

1. **Step3 依赖 Step5**：Step3 的坐标标注以 Step5 创建的中心线作为基准；中心线不可用时 Step3 会降级警告跳过，不中断主流程
2. **Step1 幂等保护**：同名图纸复用（不会重复创建）、已有视图跳过，可安全重复执行
3. **即时卸载**：每个 DLL 加载后立即卸载（`LibraryUnloadOptionImmediately`），不驻留内存
4. **windows.h 防护**：每个需要对话框的 DLL 保留 `WIN32_LEAN_AND_MEAN`、`NOMINMAX`、`#undef CreateDialog`，避免与 NXOpen 头文件冲突
5. **配置集中管理**：换零件或换模板时，只需修改 `NX12_CommonConfig.h` 中的配置值
