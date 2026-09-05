# -*- coding: utf-8 -*-
import xlrd, os, csv, re, unicodedata, collections
XLS = r"Y:\A_各零件汇总\花鼓零件明细表_20260902121949.xls"

def norm(s):
    s = unicodedata.normalize("NFKC", str(s)).upper()
    for ch in " \t\u3000－—":
        s = s.replace(ch, "")
    return s.replace("＊", "*").replace("×", "*")

CAT = {10: "01_塔基壳体", 9: "02_套管", 11: "02_套管",
       4: "03_左侧盖", 5: "04_右侧盖_塔基端"}
BAD = ("客供", "无", "/", "\\", "待定", "见", "同")
wb = xlrd.open_workbook(XLS)
sh = wb.sheet_by_index(0)
exp = collections.defaultdict(set)
for r in range(1, sh.nrows):
    for col, cat in CAT.items():
        v = sh.cell_value(r, col)
        if not isinstance(v, str):
            continue
        v = v.strip()
        if not v or any(b in v for b in BAD):
            continue
        v = re.sub(r"[（(][^（）()]*[)）]", "", v).strip()
        if len(norm(v)) >= 4:
            exp[cat].add(norm(v))
exp["01_塔基壳体"] |= {"02TJK11SHXA", "02TJKXDRHXA"}

found = collections.defaultdict(set)
for row in csv.reader(open(r"E:\UG\A_各零件汇总\提取明细.csv", encoding="utf-8-sig")):
    if row and row[0] in exp:
        found[row[0]].add(row[1])

lines = []
tot = totf = 0
for cat in ["01_塔基壳体", "02_套管", "03_左侧盖", "04_右侧盖_塔基端"]:
    e, f = len(exp[cat]), len(found[cat])
    tot += e; totf += f
    lines.append("%s: 零件表%d种 / 找到%d种 / 覆盖率%0.0f%%" % (cat, e, f, 100 * f / e))
lines.append("合计: 零件表%d种 / 找到%d种 / 覆盖率%0.0f%%" % (tot, totf, 100 * totf / tot))
open(r"E:\UG\A_各零件汇总\覆盖率统计.txt", "w", encoding="utf-8").write("\n".join(lines))
print("\n".join(lines))
