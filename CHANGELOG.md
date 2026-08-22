# CHANGELOG

本文件记录 E:\UG（UG_Draw_NXopen_Dill）的发布历史。版本规则：vMAJOR.MINOR.PATCH。

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
