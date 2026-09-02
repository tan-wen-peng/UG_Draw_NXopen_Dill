# CHANGELOG

本文件记录 E:\UG（UG_Draw_NXopen_Dill）的发布历史。版本规则：vMAJOR.MINOR.PATCH。

## [1.4.0] - 2026-09-01 —— Step9 v4 生成云线后自动删除参考草图几何

### 变更（Step9_CloudLines\NX12_Step9_CloudLines.cpp）
- 云线生成成功后自动删除作为参考的草图矩形（4 条直线）/ 圆形（完整圆或大圆弧）几何，
  草图本身保留；删除仅在对应形状云线创建成功后执行，失败则保留几何以便重试。
- 新增配置开关 kDeleteSourceGeometry（默认 true；改为 false 可保留参考几何）。
- 识别结构 RectInfo/CircleInfo/ShapeRec 增加来源曲线 tag 携带（矩形 4 线 / 圆 1 条曲线），
  删除走 UF_OBJ_delete_object 逐 tag 处理并记录日志。
- 幂等语义不变：已转换草图仍打 STEP9_SKETCH，其云线保留；如需改疏密请重画草图
  （或先关闭 kDeleteSourceGeometry 再运行）。

### 部署
- 与 1.1.0~1.3.0 相同：构建 NX12_MultiModule.sln（Release x64）后部署 DLL；
  twp_toolbox.men 菜单注册仍为 GBK 手工追加，未变。

## [1.3.0] - 2026-09-01 —— Step9 v3 云线参数对话框（波浪直径可调）

### 变更（Step9_CloudLines\NX12_Step9_CloudLines.cpp）
- 新增运行时“云线参数”对话框（Win32 内存 DLGTEMPLATE + DialogBoxIndirectW，与 Step6 同一
  对话框范式，无需资源文件）：输入“波浪直径（每波弦长，mm）”，默认 8.0，数值越小
  云线越密、越大越疏，直接控制半圆弧波浪大小；确定后本次运行按输入值生成，
  取消则本次不画云线（旧云线保留）。
- 配置常量 kWaveChord 更名为 kWaveChordDefault（仅作对话框默认值），生成逻辑改为
  运行时变量 waveChord 传递；草图识别与回退默认模式均生效。
- 编译修复随本版合入：补 #include <uf_object_types.h>（UF_line_type 等类型常量）、
  #include <NXOpen/DraftingManager.hxx>（Part::Drafting 完整类型）、
  自定义 PI 常量更名 kPI（避免与 uf_defs.h 的 PI 宏冲突）。
- Win32 防护（WIN32_LEAN_AND_MEAN / NOMINMAX / #undef CreateDialog）与 windows.h
  前置包含，与 Step6/Step8 同范式。

### 部署
- 与 1.1.0/1.2.0 相同：构建 NX12_MultiModule.sln（Release x64）后部署 DLL；
  twp_toolbox.men 菜单注册仍为 GBK 手工追加，未变。

## [1.2.0] - 2026-09-01 —— Step9 v2 草图驱动重构

### 变更（Step9_CloudLines 目录下 NX12_Step9_CloudLines.cpp 整文件重写）
- 新增草图驱动模式：扫描当前工作图纸上的制图草图（Part::Sketches() 遍历 +
  Sketch::IsDraftingSketch() 过滤 + 图纸视图 tag 匹配），GetAllGeometry() 枚举
  草图曲线并分类（直线/完整圆/圆弧/样条/圆锥/其它）。
- 矩形识别：4 条直线段闭合链（对边等长平行、邻边垂直；端点容差 0.01mm、
  角度容差 1 度、边长容差 5%），提取中心/宽/高/倾角（支持非水平矩形，云线按倾角旋转生成）。
- 圆形识别：完整圆（±2 度）或扫略角 ≥270 度的圆弧，提取圆心/半径；
  圆弧中心经矩阵换算到绝对坐标（单位阵直接使用）。
- 参数继承：云线位置/尺寸直接取自识别结果，替换 v1 硬编码常量；
  无可识别草图时回退 kFallback* 默认配置（等同 v1 行为，向后兼容）。
- 幂等升级：草图打 STEP9_SKETCH 属性不再重复处理；云线 STEP9_CLOUD 改为
  字符串属性（DEFAULT / SKETCH:<tag>），重画同源先删旧线；v1 整数属性旧云线自动迁移清理。
- 模块化：草图枚举/几何分类/矩形识别/圆形识别/云线生成/幂等管理拆分独立函数；
  逐草图 try/catch 隔离异常，识别失败优雅降级不中断流程。
- 代码规范：C++11/14（auto、范围 for、nullptr、constexpr），函数级 Doxygen 中文注释，
  文件 UTF-8 带 BOM；诊断日志输出草图枚举统计、每草图几何统计、识别形状参数与生成结果。

### 部署
- DLL 名/工程/解决方案接线不变（NX12_Step9_CloudLines.dll）；
  菜单注册（GBK 手工追加）与构建部署步骤沿用 1.1.0 条目。

## [1.1.0] - 2026-08-22 —— Step9 云线（矩形/圆形修订云线）

### 新增
- **Step9_CloudLines 模块**（NX12_Step9_CloudLines.dll，DLL 9/9）：在当前工作图纸上自动绘制云线：
  1. **矩形云线**：沿矩形边界（宽×高）交替外凸/内凹半圆弧波浪；
  2. **圆形云线**：沿圆周交替外凸/内凹半圆弧波浪。
- 实现说明：NX12 制图模块**无原生“云线”命令**（已核实 NX 12.0 安装目录菜单/资源无 Cloud 命令，西门子社区亦确认需自行绘制）；本模块用封闭周期样条
  （`UF_CURVE_create_spline_thru_pts`，degree=3、periodicity=1）拟合波浪半圆弧，效果等同 AutoCAD REVCLOUD 矩形/圆形样式，且为**单条封闭曲线**。
- 幂等保护：新云线打 `STEP9_CLOUD` 用户属性，重跑先删旧云线再重画，不会越画越多。
- 参数集中：模式（矩形/圆形/都画）、中心/宽高/半径、波幅、每波采样、图层、颜色均在
  `NX12_NXOpenCPP_Wizard1\Step9_CloudLines\NX12_Step9_CloudLines.cpp` 顶部配置区调整。

### 工程接线
- 新增 `Step9_CloudLines\NX12_Step9_CloudLines.vcxproj`（TargetName=NX12_Step9_CloudLines）与 .filters；
- `NX12_MultiModule.sln` 已加入 Step9 项目（含 CommonUtils 依赖与 Debug/Release x64 配置），`build_all.bat` 无需改动（整体构建解决方案）；
- README_多模块架构.md / README.md / nx_app 部署说明已同步更新。

### 部署说明（菜单注册待手工完成）
- `twp_toolbox.men` 与 `build_and_deploy.bat` 为 **GBK/ANSI（936）编码**，本次未直接改写（避免破坏编码）；
  按 `nx_app\README_twp工具箱部署说明.md` 第五节“未来新增工具（三步法）”手工追加按钮并重新构建部署即可：
  1. 构建 `NX12_MultiModule.sln`（Release x64），将 `bin\Release\NX12_Step9_CloudLines.dll` 复制到 `E:\UG\nx_app\application\`；
  2. 用记事本（另存为 ANSI/GBK）在 `startup\twp_toolbox.men` 的“制图向导”菜单块追加：
```
BUTTON TWP_WIZ_STEP9
LABEL 9.云线
ACTIONS NX12_Step9_CloudLines.dll
```
  3. 重启 NX 验证。
- 未部署前可通过 **Ctrl+U → 文件 → 执行 → NX Open** 直接加载 `bin\Release\NX12_Step9_CloudLines.dll` 使用。

## [1.0.2] - 2026-08-22 —— 注册层安全回退（启动零代码执行）

- 移除 startup 阶段的 MODIFY UG_APP_GATEWAY + LIBRARIES titleblock_fill 自动加载
  （避免未经验证的 DLL 在 NX 启动早期被加载，彻底排除注册层对 NX 启动与既有功能的影响）。
- 标题栏三个动作改为官方 MenuBarCppApp 范式：点击「应用模块→twp工具箱」按钮时加载注册。
- 删除 application\twp_gateway_append.men；更新 README 与 twp_verify.ps1 体检项。
- 核实结论：E:\UG 全部工具源码中没有任何"尺寸公差切换"相关逻辑
  （公差仅涉及 Step2 的公差文本字体/字高/宽高比设置与球标的公差文本样式），
  Step1-7 本轮仅重新链接、0 个函数重编译，功能与 8/19 版本完全一致。

## [1.0.1] - 2026-08-22 —— Step8 标题框填写 v3 全诊断修复版

### 问题
- 运行 Step8（titleblock_fill.dll）后，"日期"与"零件编号"右侧单元格均未填写；
  syslog 显示"主路线: UF_TABNOT，共找到 4 个表格注释 → 填写 0 处、跳过 0 处、失败 0 处"。
- 根因：NX 实际加载的 E:\UG\NX12_NXOpenCPP_Wizard1\bin\Release\ 与
  nx_app\application\ 下的 titleblock_fill.dll 均为 08-21 的 v1 旧产物（47,616 字节），
  8/22 的 v2 诊断版只存在于 bin\Release_v2 / nx_open_dll，从未部署；且 v1 无任何
  单元格转储，标签未命中时无法定位原因。

### 修复（Step8_TitleBlockFill\NX12_Step8_TitleBlockFill.cpp → v3）
1. 枚举统计细分 subtype（0=节/1=表格/2=行/3=列/11=明细表），并统计原生 NX TitleBlock
   对象个数（判断标题栏是普通表格注释还是 NX 原生 TitleBlock）。
2. 标签匹配增强：UTF-8 校验 + GBK(ANSI)→UTF-8 自动转码、去 BOM/全角空格/尾部冒号、
   紧凑比较（忽略全部空白）、ASCII 大小写不敏感；命中方式记入日志。
3. 遍历范围增加标题行（UF_TABNOT_ask_nth_header_row）；右邻格优先
   ask_relative_column(+1)（正确处理合并列）并回退列索引+1；最后一列安全跳过并记日志。
4. ask_cell_text / ask_evaluated_cell_text / ask_merge_info / set_cell_text /
   UF_TABNOT_update 全部记录返回码与 UF_get_fail_message 文本。
5. 可写判定扩展：占位符白名单扩充；旧日期识别支持 ./ - / 年月日 分隔（含无前导零），
   日期模式允许刷新旧日期；零件编号仍严格不覆盖非占位符内容。
6. 降级路线重写：遍历 EditTitleBlockBuilder->Cells()，标签格文本命中后经底层表格注释
   单元格几何定位右邻格，优先 SetCellValueForLabel、无标签名时 SetEditableText，
   记录 Lock 状态；主路线未命中且存在原生 TitleBlock 时自动追加降级路线。
7. 菜单模式下若两步均未命中任何标签，自动转储全部表格单元格（含 UTF-8 十六进制），
   一次运行即可定位编码/结构问题；Ctrl+U 入口保留完整诊断转储。
8. 日期格式固定 年.月.日（如 2026.08.22）；零件编号=当前图档文件名去扩展名（大小写
   不敏感）；只写标签右邻格，其余内容不动。

### 部署
- 重新编译 Step8 并部署到 bin\Release\、nx_app\application\、
  nx_open_dll\、bin\Release_v2\（四份 SHA-256 一致：94772BAC…）。
- 同步入库：Step8 工程文件、NX12_MultiModule.sln（含 Step8）、build_all.bat、
  date_marking.py（原型基准）、nx_app 部署三件套与 build_and_deploy.bat。

## [1.0.0] - 2026-08-22 —— twp工具箱 统一注册（本轮）

### 新增 / 变更
- **统一菜单注册**：新增 nx_app\startup\twp_toolbox.men（GBK/936、CRLF、无 BOM），
  一个入口 **twp工具箱** 覆盖全部 12 个工具：
  - MODIFY UG_APP_GATEWAY + LIBRARIES titleblock_fill（会话启动即注册标题栏动作，仿官方 nx_china_main_menu.men）
  - BEFORE UG_HELP 顶部级联（常驻主菜单栏，仿本机星空外挂 QuickCAM.men 已验证写法）
  - APPLICATION_BUTTON TWP_TOOLBOX + MENU_FILES（官方 MenuBarCppApp 范式，应用模块页签辅助入口）
  - 菜单树内联：制图向导Step1-7（7 按钮）/ 专项工具（4 按钮）/ 标题栏填写（3 按钮）
- **重新编译并部署全部 12 个 DLL** 到 nx_app\application\（此前部署的 titleblock_fill.dll 是旧版，
  缺少 ufsta/MenuBar 注册，已用最新 8/22 编译产物替换；其余 DLL 同步刷新）。
- **构建脚本**：新增根目录 build_and_deploy.bat，一键构建 5 个解决方案并自动部署。
- **文档**：新增/重写 nx_app\README_twp工具箱部署说明.md、根 README.md 工具箱章节、本文件。
- **挂载配置**：确认 NX12 与 NX2206 的 custom_dirs.dat / ug_custom_dirs.dat 均含 E:\UG\nx_app
  （备份 *.bak_twp）；用户级 UGII_BASE_DIR 指向 NX12 覆盖机器级残留。
- **清理**：实验菜单（twp_topbar/twp_test）、诊断脚本与截图、.workbuddy 会话数据、
  散落图片与陈旧日志移入 _archive\（gitignore）；部署目录只保留正式三件套与 12 个 DLL。

### 工具清单（v1.0.0）
Step1-7 制图向导 7 个 DLL、titleblock_fill.dll、dbt_step1.dll、explosion_step1.dll、
explosion_step2.dll、balloon_step1.dll，共 12 个 DLL、14 个菜单按钮。

## [0.1.0] - 2026-08-12 —— 初始提交
- NX12 NXOpen C++ 制图自动化工具集源码（Wizard1 多模块 + 打标图 + 爆炸图 + 爆炸参数 + 球标）。
- 上传 GitHub：https://github.com/tan-wen-peng/UG_Draw_NXopen_Dill.git（用户 tan-wen-peng）。
