# UG_Draw_NXopen_Dill

Siemens NX12 (UG) 二次开发 C++ 工具集，基于 NXOpen C++ API，用于工程制图自动化。

[![NX12](https://img.shields.io/badge/Siemens-NX12-0076a8)](https://www.plm.automation.siemens.com/)
[![Language](https://img.shields.io/badge/Language-C%2B%2B-f34b7d)](#)
[![Lines](https://img.shields.io/badge/源码-9333行-409eff)](#)
[![Blog](https://img.shields.io/badge/博客-TWP技术笔记-00d4ff)](https://www.twptech.site/)

> 机械工程师写代码，不是为了转行，是为了把「重复一千遍」的活，压缩成「检查一遍」的活。
> 开发故事与踩坑复盘见博客：[《我给 NX 写出图插件：跨图纸误收、垂直标注漏判、视图拾取三个坑》](https://www.twptech.site/posts/nx-drawing-plugin/)

## 项目组成

| 目录 | 功能 |
|------|------|
| `NX12_NXOpenCPP_Wizard1/` | 多步骤制图向导（模块化架构）：新建图纸与视图、图纸参数设置、坐标/线性尺寸标注、中心线、图层切换、后缀追加 |
| `NX12_NXOpenCPP_打标图/` | 打标图坐标创建（逐选圆弧 + 实时反馈） |
| `NX12_NXOpenCPP_爆炸参数/` | 从装配爆炸图提取已爆炸组件的显示名与 dx/dy/dz 偏移参数 |
| `NX12_NXOpenCPP_爆炸图/` | 装配爆炸图自动布局（原点可选、按组件宽度打包、距离在线编辑） |
| `NX12_NXOpenCPP_球标/` | 球标标注（图形窗口直接点选、顺序编号、圆圈 + 引线） |
| `nx_open_dll/` | 运行输出示例（explosion_params.txt） |

## 架构

```
NX12_MultiModule.sln
├── Step1_SheetAndViews      新建图纸与视图
├── Step2_SheetPreferences   图纸参数设置
├── Step3_OrdinateDimensions 坐标尺寸标注
├── Step4_LinearDimensions   线性尺寸标注
├── Step5_Centerlines        中心线（视图相关几何拾取）
├── Step6_LayerSwitch        图层切换
├── Step7_AppendSuffix       图纸编号后缀追加
└── Shared/NX12_CommonUtils  公共工具库（tag 遍历 / 几何查询 / 图层规范）
```

每个功能模块编译为独立 DLL，共享一个静态工具库；模块之间相互独立，可单独加载使用。

## 三个真实踩坑（详见博客）

1. **跨图纸误收**：批量出图时对象归属混乱 → 绕开 NXOpen 包装层，走 UF 原始 tag 通道 + 部件级遍历 + 归属校验。
2. **垂直标注漏判**：浮点坐标直接 `==` 比较 → 几何判断永远带容差（epsilon），或直接 `UF_CURVE_ask_line_data` 取方向向量。
3. **视图相关几何拾取**：成员视图投影边选不中 → `UF_UI_set_cursor_view(0)` 切到「任意视图」后再拾取，`SetValue(对象, 所在视图, 拾取点)` 保证关联。

## 开发环境

- Siemens NX 12.0（NXOpen C++ API）
- Visual Studio 2017+（VC++ 项目，编译输出为 NX 可加载 DLL）

## 构建说明

1. 使用 Visual Studio 打开各项目目录下的 `.sln` 解决方案
2. 需配置 NXOpen 开发环境（`UGII_BASE_DIR` 等环境变量指向 NX12 安装目录）
3. `Wizard1` 项目提供 `build_all.bat` 一键构建脚本

## 目录说明

- 编译产物（`.dll` / `.pdb` / `.obj` 等）已通过 `.gitignore` 排除，仅保留源码与工程文件

## 版本

- **v0.2**（2026-08）补充架构图、踩坑清单与博客配套链接
- v0.1（2026-07）初始提交：7 步向导 + 4 个独立功能模块 + 共享工具库

## 版权与脱敏说明

- 代码为个人学习与工程实践产出；与公司业务相关的零件编号、产品名称、图纸规范均已脱敏。
- 需要完整可运行环境（NX12 + VS2017+）自行配置；欢迎提 Issue 交流。
