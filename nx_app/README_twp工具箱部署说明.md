# twp工具箱 —— NX12 统一菜单部署说明（v1.5）

将 E:\UG 下的全部 NXOpen C++ 工具统一注册到 NX12，统一入口 **twp工具箱**。
主入口在 **主菜单栏（帮助 菜单左侧，常驻可见）**；同时在 **应用模块（Applications）页签** 注册了同名应用按钮。
适用版本：**NX 12.0**（MenuScript VERSION 120）。

---

## 一、工具清单（13 个工具、15 个按钮）

| # | 工具名称 | DLL 文件 | 入口函数/注册方式 | 菜单动作名 ACTIONS | NX 中可加载 | .men 状态 |
|---|---------|---------|------------------|-------------------|------------|----------|
| 1 | 1.新建图纸与视图 | NX12_Step1_SheetAndViews.dll | ufusr（直接调用） | NX12_Step1_SheetAndViews.dll | ✅ | 已注册 |
| 2 | 2.图纸参数设置 | NX12_Step2_SheetPreferences.dll | ufusr（直接调用） | NX12_Step2_SheetPreferences.dll | ✅ | 已注册 |
| 3 | 3.坐标标注 | NX12_Step3_OrdinateDimensions.dll | ufusr（直接调用） | NX12_Step3_OrdinateDimensions.dll | ✅ | 已注册 |
| 4 | 4.线性标注 | NX12_Step4_LinearDimensions.dll | ufusr（直接调用） | NX12_Step4_LinearDimensions.dll | ✅ | 已注册 |
| 5 | 5.中心线 | NX12_Step5_Centerlines.dll | ufusr（直接调用） | NX12_Step5_Centerlines.dll | ✅ | 已注册 |
| 6 | 6.图层切换 | NX12_Step6_LayerSwitch.dll | ufusr（直接调用） | NX12_Step6_LayerSwitch.dll | ✅ | 已注册 |
| 7 | 7.尺寸后缀追加 | NX12_Step7_AppendSuffix.dll | ufusr（直接调用） | NX12_Step7_AppendSuffix.dll | ✅ | 已注册 |
| 8 | 9.云线（草图驱动：矩形/圆形修订云线，直径可调，成功即删参考几何） | NX12_Step9_CloudLines.dll | ufusr（直接调用） | NX12_Step9_CloudLines.dll | ✅ | **待手工追加**（见第五节） |
| 9 | 打标图坐标创建 | dbt_step1.dll | ufusr（直接调用） | dbt_step1.dll | ✅ | 已注册 |
| 10 | 爆炸图自动布局 | explosion_step1.dll | ufusr（直接调用） | explosion_step1.dll | ✅ | 已注册 |
| 11 | 爆炸参数提取 | explosion_step2.dll | ufusr（直接调用） | explosion_step2.dll | ✅ | 已注册 |
| 12 | 球标标注 | balloon_step1.dll | ufusr（直接调用） | balloon_step1.dll | ✅ | 已注册 |
| 13 | 填日期 / 填零件号 / 全部填写 | titleblock_fill.dll | **ufsta + MenuBarManager**（RegisterApplication "TITLEFILL_APP" + AddMenuAction×3，仿官方 MenuBarCppApp） | TITLEFILL_APP__fill_date / TITLEFILL_APP__fill_partno / TITLEFILL_APP__fill_both | ✅ | 已注册 |

说明：
- 前 12 个（含 Step9 云线）是纯 ufusr 工具：ACTIONS 直接写磁盘 DLL 文件名，NX 点按钮时加载 DLL 并调用 ufusr，执行完按 ufusr_ask_unload=Immediately 卸载。
- titleblock_fill.dll 是 MenuBar 型应用：动作名在 NX12_Step8_TitleBlockFill.cpp 的 ufsta 中通过 AddMenuAction("TITLEFILL_APP__...") 注册，.men 中 ACTIONS 必须与其**逐字一致**；ufusr_ask_unload 固定返回 AtTermination。
  **首次使用「标题栏填写」前，请先到「应用模块」页签点击一次 twp工具箱 按钮**（触发 LIBRARIES 加载并注册动作），此后三个按钮即可正常使用。
- 旧的整体式 NX12_NXOpenCPP_Wizard1.dll（2026-08-07）已被 Step1-7 多模块版取代，不再注册。

## 二、菜单结构

```
NX 主菜单栏（帮助 左侧，常驻）
└── twp工具箱
    ├── 制图向导Step1-9
    │   ├── 1.新建图纸与视图      → NX12_Step1_SheetAndViews.dll
    │   ├── 2.图纸参数设置        → NX12_Step2_SheetPreferences.dll
    │   ├── 3.坐标标注            → NX12_Step3_OrdinateDimensions.dll
    │   ├── 4.线性标注            → NX12_Step4_LinearDimensions.dll
    │   ├── 5.中心线              → NX12_Step5_Centerlines.dll
    │   ├── 6.图层切换            → NX12_Step6_LayerSwitch.dll
    │   ├── 7.尺寸后缀追加        → NX12_Step7_AppendSuffix.dll
    │   └── 9.云线（草图驱动，直径可调，成功即删参考几何） → NX12_Step9_CloudLines.dll（待追加）
    ├── 专项工具
    │   ├── 打标图坐标创建        → dbt_step1.dll
    │   ├── 爆炸图自动布局        → explosion_step1.dll
    │   ├── 爆炸参数提取          → explosion_step2.dll（读取同目录 explosion_params.txt）
    │   └── 球标标注              → balloon_step1.dll
    └── 标题栏填写（先点一次应用模块按钮激活）
        ├── 填日期                → TITLEFILL_APP__fill_date
        ├── 填零件号              → TITLEFILL_APP__fill_partno
        └── 全部填写              → TITLEFILL_APP__fill_both

应用模块（Applications）页签：twp工具箱 应用按钮（官方 MenuBarCppApp 范式）
```

## 三、文件清单与位置

```
E:\UG\nx_app\                       ← 挂载根目录（已写入 custom_dirs.dat）
├── startup\twp_toolbox.men           ← 唯一注册文件（顶部级联 + 应用按钮 + 全部菜单树；启动不加载任何 DLL）
├── application\                      ← 13 个 DLL + 数据文件（NX 按 startup/application 约定查找；Step9 云线 DLL 构建部署后计入）
│   ├── NX12_Step1~7_*.dll + NX12_Step9_CloudLines.dll（8 个制图向导 DLL）
│   ├── titleblock_fill.dll
│   ├── dbt_step1.dll  explosion_step1.dll  explosion_step2.dll  balloon_step1.dll
│   ├── explosion_params.txt          ← 爆炸参数提取的数据文件，必须与 DLL 同目录
│   └── twp_toolbox_app.men           ← MENU_FILES 占位文件（防止重复级联）
├── twp_verify.ps1                    ← 一键体检脚本（33 项检查）
├── twp_custom_dirs.dat               ← 隔离启动用自定义目录清单（单行 E:\UG\nx_app）
├── start_nx12_twp.bat                ← 隔离启动器（显式设置 UGII_* 环境变量后启动 NX12）
└── README_twp工具箱部署说明.md        ← 本文件

E:\UG\build_and_deploy.bat           ← 一键构建全部 5 个解决方案并自动部署 DLL
E:\UG\_archive\                      ← 历史调试文件归档（确认无用后可整目录删除）
```

挂载配置（本机已完成，含备份 *.bak_twp）：
- D:\Program Files\Siemens\NX 12.0\UGII\menus\custom_dirs.dat — 末尾追加一行 E:\UG\nx_app
- D:\Program Files\Siemens\NX 12.0\UGII\menus\ug_custom_dirs.dat — 同样追加（NX12 实际默认读这份）
- E:\Program Files\Siemens\NX2206\UGII\menus\custom_dirs.dat 与 ug_custom_dirs.dat — 同样追加（备机用）
- 用户环境变量：UGII_BASE_DIR=D:\Program Files\Siemens\NX 12.0（覆盖机器级残留的 NX2206 值）

## 四、注册机制说明（为什么这样写）

| 段落 | 写法 | 作用 | 范式出处 |
|------|------|------|---------|
| 顶部级联 | BEFORE UG_HELP + CASCADE_BUTTON TWP_TOOLBOX_MENU + END_OF_AFTER | 主菜单栏常驻入口，启动即可见 | 本机星空外挂 D:\QuickCAM\Startup\QuickCAM.men（已验证） |
| 应用按钮 | MENU UG_APPLICATION + APPLICATION_BUTTON TWP_TOOLBOX + LABEL + LIBRARIES + MENU_FILES | Applications 页签同名按钮，**点击时才加载 titleblock_fill.dll** | NX12 官方 UGOPEN\SampleNXOpenApplications\C++\MenuBarCppApp\MenuBarCppAppButton.men |
| 菜单树 | 全部 MENU 块内联在 startup 文件 | 启动即解析，不依赖应用激活 | QuickCAM 同款做法 |

> 安全约定（v1.1 起）：startup 阶段**不做** MODIFY UG_APP_GATEWAY + LIBRARIES 自动加载。
> 菜单注册对 NX 启动完全被动——不加载任何 DLL、不执行任何用户代码、不改任何 NX 设置；
> 所有 DLL 都在"点击按钮/激活应用"时才加载。这样可以彻底排除注册层对 NX 启动与既有功能的影响。

## 五、中文编码规则（重要，NX12 乱码的根因）

- .men 文件必须使用 **GBK/ANSI（代码页 936）编码、无 BOM、CRLF 换行**。
- 用 UTF-8 保存时，NX12 会把每个汉字按 2 字节 GBK 解释成另外的字符 → 菜单显示乱码。
- 检查方法（PowerShell）：
```powershell
$t = [System.IO.File]::ReadAllText('E:\UG\nx_app\startup\twp_toolbox.men', [System.Text.Encoding]::GetEncoding(936))
$t   # 应能正常显示中文；文件前 3 字节不能是 EF BB BF
```
- 手工修复方法：记事本打开 → 另存为 → 编码选 **ANSI** → 覆盖。
- 约定：按钮/菜单的**内部标识符（BUTTON/CASCADE_BUTTON/ACTIONS 名）全部保持 ASCII**，仅 LABEL 用中文。

## 六、一键构建与部署

```bat
E:\UG\build_and_deploy.bat
```
- 依次构建：Wizard1 多模块（9 个 DLL，含 Step9 云线）、打标图、爆炸图、爆炸参数、球标（Release x64）。
- 输出位置：各项目 bin\Release\ / x64\Release\，脚本随后自动复制到 E:\UG\nx_app\application\。
- 手工构建时注意：NX12_NXOpenCPP_打标图 项目用 $(UGII_BASE_DIR) 属性，需传入 /p:UGII_BASE_DIR="D:\Program Files\Siemens\NX 12.0"（脚本已处理）。

## 七、重启 NX 并验证

1. 完全退出 NX（确认任务管理器无 ugraf.exe 残留）。
2. 平时方式启动 NX12（双击桌面图标即可；环境变量已配好）。
   - 备选：双击 E:\UG\nx_app\start_nx12_twp.bat 隔离启动（只挂本工具箱，用于排障对照）。
3. **验证入口**：主菜单栏最右侧（帮助 左侧）应出现 **twp工具箱** 级联菜单；「应用模块」页签应出现 **twp工具箱** 按钮。
4. 展开三级菜单，核对 15 个按钮中文无乱码（Step9 云线按钮待手工追加后为 15 个）。
5. 抽查：点「1.新建图纸与视图」确认向导启动；标题栏工具先点一次应用模块按钮再点「填日期/填零件号/全部填写」。
6. 排障日志：NX「文件→帮助→日志文件」，或 %LOCALAPPDATA%\Temp\*.syslog，搜索 MB_LOADED_MENU_FILE twp、titleblock_fill、MB_LIBRARY_LOAD_FAILED。
7. 一键体检：powershell -ExecutionPolicy Bypass -File E:\UG\nx_app\twp_verify.ps1

## 八、常见问题排查

### 1. 菜单完全不显示（连顶部级联都没有）
- 检查 D:\...\NX 12.0\UGII\menus\custom_dirs.dat 与 ug_custom_dirs.dat 是否含 E:\UG\nx_app（无前导空格、无注释符）。
- 确认目录名严格为 startup、application。
- 确认 .men 第一个有效内容是 VERSION 120（注释 ! 行除外）。
- 确认 UGII_BASE_DIR 指向 NX12（机器级残留 NX2206 会读错安装目录的 dat 文件）。
- syslog 里应出现 MB_LOADED_MENU_FILE Loaded startup menu file: E:\UG\nx_app\startup\twp_toolbox.men。

### 2. DLL 未加载 / 点按钮无反应
- ACTIONS 写的是**磁盘 DLL 文件名**（含 .dll）；LIBRARIES 写**不带扩展名**的文件名。二者与 application 目录中实际文件逐字一致。
- DLL 必须是 x64（dumpbin /headers xxx.dll | findstr machine 应为 8664）。
- 若按钮点击后 NX 崩溃：DLL 加载了但初始化失败，查 syslog 中 MB_LIBRARY_LOAD_FAILED。

### 3. 标题栏三个按钮点了没反应
- 原因：TITLEFILL_APP__* 是 ufsta 注册动作，DLL 尚未在会话中注册。
- 处理：到「应用模块」页签**点击一次 twp工具箱 按钮**（触发 LIBRARIES titleblock_fill 加载与动作注册），再点「填日期/填零件号/全部填写」。
- 核对代码：NX12_Step8_TitleBlockFill.cpp 中 AddMenuAction("TITLEFILL_APP__fill_date"...) 等三个字面量与 .men 完全一致。

### 4. 中文乱码
- 按第五节把 .men 转成 GBK/ANSI（936）、无 BOM、CRLF；不要用 UTF-8 BOM。

### 5. 未来新增工具（三步法）
1. 编译好的 x64 DLL 复制到 E:\UG\nx_app\application\。
2. 在 startup\twp_toolbox.men 对应 MENU 块追加：
```
BUTTON TWP_XX_NEW
LABEL 新工具中文名
ACTIONS 新工具DLL名.dll
```
   若新工具是 MenuBar 型（ufsta 注册动作），动作名要与代码 AddMenuAction 逐字一致，并在 APPLICATION_BUTTON 块的 LIBRARIES 中加入该 DLL 名（参照 titleblock_fill）。
3. 保存为 GBK 编码，重启 NX。

## 九、缺失内容与最小补全方案（现状评估）

- **已完整**：13 个工具全部有源码、可编译；其中 12 个已部署并注册菜单，Step9 云线 DLL 待构建部署 + 按第五节追加菜单按钮。
- **未做（不影响使用，按需补）**：
  1. 工具栏图标/位图（BITMAP）：当前为纯文字菜单，无图标。
  2. NX2206 专用重编译：DLL 按 NX12 头文件编译，在 NX2206 使用前应针对其 UGOPEN 重新编译（挂载点已就绪）。
  3. 英文/繁体语言环境：LABEL 只有简体中文。
- **禁止事项**：不要为了注册去改各工具的核心逻辑；注册层仅限 .men 与 DLL 文件名的映射关系。

## 十、版本管理与清理

- Git 仓库：E:\UG（origin: github.com/tan-wen-peng/UG_Draw_NXopen_Dill.git），发布记录见 E:\UG\CHANGELOG.md。
- .gitignore 排除：.vs、编译输出（dll/pdb/obj/lib…）、_archive\、.git_bak\。
- 清理说明：历史调试文件在 E:\UG\_archive\，确认无用后可整体删除；.git_bak 是合并仓库前的旧历史备份。
