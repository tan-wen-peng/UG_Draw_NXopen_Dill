# -*- coding: utf-8 -*-
# ==========================================================================
# 【已废弃 / DEPRECATED】
# 本脚本所用 NXOpen Python API（如 TabularNote.GetRows / cell.SetCellText
# 等）在 NX12 中不存在，无法在 NX12 上运行。
# 功能已由 Step8 C++ 版替代：
#   e:\UG\NX12_NXOpenCPP_Wizard1\Step8_TitleBlockFill\NX12_Step8_TitleBlockFill.cpp
#   （编译产物 titleblock_fill.dll，UF_TABNOT 实现，另含菜单部署与降级路线）
# 本文件仅保留作为逻辑基准（标签表/占位符白名单/右邻格写入策略的来源）。
# ==========================================================================
#
# NX Open Python - 自动填写制图标题栏日期
# 用法：打开图纸 → Ctrl+U → 选择此文件运行

import NXOpen
import NXOpen.Annotations
import datetime

def fill_drawing_dates():
    theSession = NXOpen.Session.GetSession()
    workPart = theSession.Parts.Work
    
    if workPart is None:
        print("请先打开一个部件！")
        return

    # ========== 日期格式，按需修改 ==========
    # "%Y.%m.%d"  →  2026.08.21
    # "%Y/%m/%d"  →  2026/08/21
    # "%Y年%m月%d日" → 2026年08月21日
    date_str = datetime.datetime.now().strftime("%Y.%m.%d")
    
    lw = theSession.ListingWindow
    lw.Open()
    lw.WriteLine("===== 自动填写制图日期 =====")
    lw.WriteLine("当前日期: " + date_str)

    # 获取所有表格注释（标题栏）
    tabularNotes = list(workPart.Annotations.TabularNotes)
    if len(tabularNotes) == 0:
        lw.WriteLine("未找到表格注释，请确认已进入制图模块且标题栏是表格注释。")
        return

    filled = 0

    for tabularNote in tabularNotes:
        rows = tabularNote.GetRows()
        
        for row in rows:
            cells = row.GetCells()
            
            for i, cell in enumerate(cells):
                text = cell.GetCellText().strip()
                
                # 匹配"日期"标签（支持中文、英文大小写）
                if text in ["日期", "Date", "DATE", "date"]:
                    
                    # 策略1：填右边相邻单元格（最常见）
                    if i + 1 < len(cells):
                        target = cells[i + 1]
                        targetText = target.GetCellText().strip()
                        
                        # 只填空的或占位符单元格，避免覆盖已有签名
                        if targetText == "" or targetText in ["<日期>", "YYYY.MM.DD", "年 月 日", "Date"]:
                            target.SetCellText(date_str)
                            filled += 1
                            lw.WriteLine("已填写: " + date_str)
                            continue
                    
                    # 策略2：如果右边没有，尝试下方单元格（竖向标题栏）
                    # （如需支持，可在此扩展）

    lw.WriteLine("===== 完成，共填写 {} 处日期 =====".format(filled))

if __name__ == '__main__':
    fill_drawing_dates()