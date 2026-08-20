# UG_Draw_NXopen_Dill

Siemens NX12 (UG) 二次开发 C++ 工具集，基于 NXOpen C++ API，用于工程制图自动化。

## 项目组成

| 目录 | 功能 |
|------|------|
| `NX12_NXOpenCPP_Wizard1/` | 多步骤制图向导（模块化架构）：新建图纸与视图、图纸参数设置、坐标/线性尺寸标注、中心线、图层切换、后缀追加 |
| `NX12_NXOpenCPP_打标图/` | 打标图坐标创建（逐选圆弧 + 实时反馈） |
| `NX12_NXOpenCPP_爆炸参数/` | 从装配爆炸图提取已爆炸组件的显示名与 dx/dy/dz 偏移参数 |
| `NX12_NXOpenCPP_爆炸图/` | 装配爆炸图自动布局（原点可选、按组件宽度打包、距离在线编辑） |
| `NX12_NXOpenCPP_球标/` | 球标标注（图形窗口直接点选、顺序编号、圆圈 + 引线） |
| `nx_open_dll/` | 运行输出示例（explosion_params.txt） |

## 开发环境

- Siemens NX 12.0（NXOpen C++ API）
- Visual Studio 2017+（VC++ 项目，编译输出为 NX 可加载 DLL）

## 构建说明

1. 使用 Visual Studio 打开各项目目录下的 `.sln` 解决方案
2. 需配置 NXOpen 开发环境（`UGII_BASE_DIR` 等环境变量指向 NX12 安装目录）
3. `Wizard1` 项目提供 `build_all.bat` 一键构建脚本

## 目录说明

- 编译产物（`.dll` / `.pdb` / `.obj` 等）已通过 `.gitignore` 排除，仅保留源码与工程文件
