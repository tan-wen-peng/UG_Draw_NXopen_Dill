========================================================================
    NX Open API : NX12_NXOpenCPP_爆炸参数 Project Overview
========================================================================

Step 2: 爆炸参数提取 (explosion_step2.dll)
从当前工作部件的已存在爆炸图中提取每个已爆炸叶子组件的
显示名与爆炸位移(dx/dy/dz), 生成参数文件:
  E:\UG\nx_open_dll\explosion_params.txt
每行格式: 组件显示名|dx|dy|dz
供 step1 (explosion_step1.dll) 自动化重放手动爆炸过程.

参考实现: d:\A_UG\05_爆炸图\提取爆炸参数.vb
用法: NX 中打开含手动爆炸数据的装配 → 文件 → 执行 → NX Open
      → 选择 explosion_step2.dll 运行.

*.vcproj
    This is the main project file for projects generated using an Application Wizard. 
    It contains information about the version of the product that generated the file, and 
    information about the platforms, configurations, and project features selected with the
    Application Wizard.


This is a sample template file.

/////////////////////////////////////////////////////////////////////////////
Other notes:

/////////////////////////////////////////////////////////////////////////////
