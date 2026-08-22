# UG_Draw_NXopen_Dill

Siemens NX12 (UG) 二次开发 C++ 工具集，基于 NXOpen C++ API，用于工程制图自动化。
全部工具已统一注册到 NX12 的 **twp工具箱** 菜单（部署说明见
[nx_app/README_twp工具箱部署说明.md](nx_app/README_twp工具箱部署说明.md)）。

## twp工具箱（NX12 菜单入口）

启动 NX12 后：**主菜单栏（帮助 左侧）→ twp工具箱**，或 **应用模块（Applications）页签 → twp工具箱**。

| 子菜单 | 工具 | DLL |
|--------|------|-----|
| 制图向导Step1-7 | 1.新建图纸与视图 / 2.图纸参数设置 / 3.坐标标注 / 4.线性标注 / 5.中心线 / 6.图层切换 / 7.尺寸后缀追加 | NX12_Step1~7_*.dll |
| 专项工具 | 打标图坐标创建 / 爆炸图自动布局 / 爆炸参数提取 / 球标标注 | dbt_step1.dll / explosion_step1.dll / explosion_step2.dll / balloon_step1.dll |
| 标题栏填写 | 填日期 / 填零件号 / 全部填写 | titleblock_fill.dll（ufsta 注册 TITLEFILL_APP__* 动作） |

- 挂载目录：E:\UG\nx_app（startup 放 .men，application 放 DLL）
- 一键构建+部署：E:\UG\build_and_deploy.bat
- 发布记录：CHANGELOG.md

## 项目组成

| 目录 | 功能 |
|------|------|
| NX12_NXOpenCPP_Wizard1/ | 多步骤制图向导（多模块）：新建图纸与视图、图纸参数设置、坐标/线性尺寸标注、中心线、图层切换、后缀追加、标题栏填写（MenuBar 注册） |
| NX12_NXOpenCPP_打标图/ | 打标图坐标创建（逐选圆弧 + 实时反馈） |
| NX12_NXOpenCPP_爆炸参数/ | 从装配爆炸图提取已爆炸组件的显示名与 dx/dy/dz 偏移参数 |
| NX12_NXOpenCPP_爆炸图/ | 装配爆炸图自动布局（原点可选、按组件宽度打包、距离在线编辑） |
| NX12_NXOpenCPP_球标/ | 球标标注（图形窗口直接点选、顺序编号、圆圈 + 引线） |
| nx_app/ | twp工具箱 部署目录（.men 注册文件 + 已编译 DLL + 部署说明） |
| _archive/ | 历史调试文件归档（.gitignore 排除，确认无用后可删除） |

## 开发环境

- Siemens NX 12.0（NXOpen C++ API，安装于 D:\Program Files\Siemens\NX 12.0）
- Visual Studio 2017 Community（VC++，v141 工具集），编译输出为 NX 可加载 x64 DLL

## 构建说明

1. 一键：运行 build_and_deploy.bat（Release x64，构建全部 5 个解决方案并部署到 nx_app\application）。
2. 或 Visual Studio 打开各项目 .sln（需 UGII_BASE_DIR 指向 NX12 安装目录）。
3. 编译产物（.dll/.pdb/.obj 等）经 .gitignore 排除，不进入版本库。

## 目录说明

- nx_app/startup/twp_toolbox.men：唯一菜单注册文件（GBK/936 编码、无 BOM、CRLF，NX12 兼容）。
- nx_app/application/：12 个 DLL + explosion_params.txt + 2 个占位 .men。
- 挂载点：NX12 的 UGII\menus\custom_dirs.dat 与 ug_custom_dirs.dat 末尾均含 E:\UG\nx_app。
