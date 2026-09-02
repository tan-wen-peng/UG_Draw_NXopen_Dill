# UG_Draw_NXopen_Dill

Siemens NX12 (UG) 二次开发 C++ 工具集，基于 NXOpen C++ API，用于工程制图自动化。
全部工具已统一注册到 NX12 的 **twp工具箱** 菜单（部署说明见
[nx_app/README_twp工具箱部署说明.md](nx_app/README_twp工具箱部署说明.md)）。

## twp工具箱（NX12 菜单入口）

启动 NX12 后：**主菜单栏（帮助 左侧）→ twp工具箱**，或 **应用模块（Applications）页签 → twp工具箱**。

| 子菜单 | 工具 | DLL |
|--------|------|-----|
| 制图向导Step1-9 | 1.新建图纸与视图 / 2.图纸参数设置 / 3.坐标标注 / 4.线性标注 / 5.中心线 / 6.图层切换 / 7.尺寸后缀追加 / 8.标题框填写 / 9.云线（矩形/圆形修订云线） | NX12_Step1~9_*.dll（Step8 为 titleblock_fill.dll） |
| 专项工具 | 打标图坐标创建 / 爆炸图自动布局 / 爆炸参数提取 / 球标标注 | dbt_step1.dll / explosion_step1.dll / explosion_step2.dll / balloon_step1.dll |
| 标题栏填写 | 填日期 / 填零件号 / 全部填写 | titleblock_fill.dll（ufsta 注册 TITLEFILL_APP__* 动作） |

- 挂载目录：E:\UG\nx_app（startup 放 .men，application 放 DLL）
- 一键构建+部署：E:\UG\build_and_deploy.bat
- 发布记录：CHANGELOG.md

[![NX12](https://img.shields.io/badge/Siemens-NX12-0076a8)](https://www.plm.automation.siemens.com/)
[![Language](https://img.shields.io/badge/Language-C%2B%2B-f34b7d)](#)
[![Lines](https://img.shields.io/badge/源码-9333行-409eff)](#)
[![Blog](https://img.shields.io/badge/博客-TWP技术笔记-00d4ff)](https://www.twptech.site/)

> 机械工程师写代码，不是为了转行，是为了把「重复一千遍」的活，压缩成「检查一遍」的活。
> 开发故事与踩坑复盘见博客：[《我给 NX 写出图插件：跨图纸误收、垂直标注漏判、视图拾取三个坑》](https://www.twptech.site/posts/nx-drawing-plugin/)

## 项目组成

| 目录 | 功能 |
|------|------|
| NX12_NXOpenCPP_Wizard1/ | 多步骤制图向导（多模块）：新建图纸与视图、图纸参数设置、坐标/线性尺寸标注、中心线、图层切换、后缀追加、标题栏填写（MenuBar 注册）、云线绘制（草图驱动，直径对话框可调，成功即删参考几何） |
| NX12_NXOpenCPP_打标图/ | 打标图坐标创建（逐选圆弧 + 实时反馈） |
| NX12_NXOpenCPP_爆炸参数/ | 从装配爆炸图提取已爆炸组件的显示名与 dx/dy/dz 偏移参数 |
| NX12_NXOpenCPP_爆炸图/ | 装配爆炸图自动布局（原点可选、按组件宽度打包、距离在线编辑） |
| NX12_NXOpenCPP_球标/ | 球标标注（图形窗口直接点选、顺序编号、圆圈 + 引线） |
| nx_app/ | twp工具箱 部署目录（.men 注册文件 + 已编译 DLL + 部署说明） |
| _archive/ | 历史调试文件归档（.gitignore 排除，确认无用后可删除） |

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
├── Step8_TitleBlockFill     标题框日期/零件编号填写
├── Step9_CloudLines         草图驱动矩形/圆形云线（修订云线，直径可调，成功即删参考几何）绘制
└── Shared/NX12_CommonUtils  公共工具库（tag 遍历 / 几何查询 / 图层规范）
```

每个功能模块编译为独立 DLL，共享一个静态工具库；模块之间相互独立，可单独加载使用。

## 三个真实踩坑（详见博客）

1. **跨图纸误收**：批量出图时对象归属混乱 → 绕开 NXOpen 包装层，走 UF 原始 tag 通道 + 部件级遍历 + 归属校验。
2. **垂直标注漏判**：浮点坐标直接 `==` 比较 → 几何判断永远带容差（epsilon），或直接 `UF_CURVE_ask_line_data` 取方向向量。
3. **视图相关几何拾取**：成员视图投影边选不中 → `UF_UI_set_cursor_view(0)` 切到「任意视图」后再拾取，`SetValue(对象, 所在视图, 拾取点)` 保证关联。

## 开发环境

- Siemens NX 12.0（NXOpen C++ API，安装于 D:\Program Files\Siemens\NX 12.0）
- Visual Studio 2017 Community（VC++，v141 工具集），编译输出为 NX 可加载 x64 DLL

## 构建说明

1. 一键：运行 build_and_deploy.bat（Release x64，构建全部 5 个解决方案并部署到 nx_app\application）。
2. 或 Visual Studio 打开各项目 .sln（需 UGII_BASE_DIR 指向 NX12 安装目录）。
3. 编译产物（.dll/.pdb/.obj 等）经 .gitignore 排除，不进入版本库。

## 目录说明

- nx_app/startup/twp_toolbox.men：唯一菜单注册文件（GBK/936 编码、无 BOM、CRLF，NX12 兼容）。
- nx_app/application/：13 个 DLL + explosion_params.txt + 2 个占位 .men（Step9 云线 DLL 构建部署后计入）。
- 挂载点：NX12 的 UGII\menus\custom_dirs.dat 与 ug_custom_dirs.dat 末尾均含 E:\UG\nx_app。
- 编译产物（`.dll` / `.pdb` / `.obj` 等）已通过 `.gitignore` 排除，仅保留源码与工程文件

## 版本

- **v1.4.0**（2026-09-01）Step9 v4：生成云线后自动删除参考草图矩形/圆形几何（开关 kDeleteSourceGeometry）；详见 CHANGELOG.md
- **v1.3.0**（2026-09-01）Step9 v3：云线参数对话框（波浪直径可调，控制云线疏密）；详见 CHANGELOG.md
- **v1.2.0**（2026-09-01）Step9 v2 草图驱动重构：自动识别图纸草图矩形/圆形并转云线，无草图回退默认参数；详见 CHANGELOG.md
- **v1.1.0**（2026-08-22）Step9 云线：制图模块矩形/圆形修订云线自动绘制（封闭周期样条拟合半圆弧波浪，幂等重画）；详见 CHANGELOG.md
- **v1.0.2**（2026-08-22）注册层安全回退：移除启动阶段 DLL 自动加载，标题栏动作改为点击时注册
- **v1.0.1**（2026-08-22）Step8 标题框填写 v3 全诊断修复版：标签匹配增强、降级路线重写
- **v1.0.0**（2026-08-22）twp工具箱统一注册：12 个工具一键入口 + 构建部署 + 文档
- v0.1（2026-07）初始提交：7 步向导 + 4 个独立功能模块 + 共享工具库

## 版权与脱敏说明

- 代码为个人学习与工程实践产出；与公司业务相关的零件编号、产品名称、图纸规范均已脱敏。
- 需要完整可运行环境（NX12 + VS2017+）自行配置；欢迎提 Issue 交流。
