# -*- coding: utf-8 -*-
"""提取花鼓零件 → E:/UG/_parts_stage/A_各零件汇总 (v3)
- 最长候选优先匹配(避免子串抢占) + 旧/不能用目录降权 + 扩展名优先级 .prt>step>stp>x_t>dwg>pdf
- 02TJK11SHXA / 02TJKXDRHXA 基础型号补收录(表中变体的基础模型)
"""
import os, re, shutil, csv, unicodedata, collections
import xlrd

ROOT = "Y:/"
STAGE = "E:/UG/A_各零件汇总"
XLS = "Y:/A_各零件汇总/花鼓零件明细表_20260902121949.xls"
SKIP_DIRS = {"#recycle", "A_各零件汇总"}
STALE_KW = ("旧", "不能用", "作废", "删除", "备份", "副本", "回收")
CAT_MAP = {10: "01_塔基壳体", 9: "02_套管", 11: "02_套管",
           4: "03_左侧盖", 5: "04_右侧盖_塔基端"}
BAD_WORDS = ("客供", "无", "/", "\\", "待定", "见", "同")
EXT_RANK = {".prt": 0, ".step": 1, ".stp": 2, ".x_t": 3, ".x_b": 4, ".sldprt": 5,
            ".igs": 6, ".dwg": 7, ".dxf": 8, ".pdf": 9, ".jt": 10}
# 手动补充：表中变体(TI/NB/BL/GL/GN/HA...)的基础型号（作为普通零件码参与全盘扫描）
EXTRA = {"01_塔基壳体": ["02TJK11SHXA", "02TJKXDRHXA"]}

def norm(s):
    s = unicodedata.normalize("NFKC", str(s)).upper()
    for ch in " \t\u3000－—":
        s = s.replace(ch, "")
    return s.replace("＊", "*").replace("×", "*")

def candidates(code):
    c = {code}
    no_star = code.replace("*", "")
    if no_star != code:
        c.add(no_star)
    m = re.match(r"^(.*)-(\d+)$", code)
    if m and len(m.group(1)) >= 4:
        b = m.group(1)
        c.add(b)
        bs = b.replace("*", "")
        if bs != b:
            c.add(bs)
    m2 = re.match(r"^(.*)-(\d+)$", no_star)
    if m2 and no_star != code and len(m2.group(1)) >= 4:
        c.add(m2.group(1))
    return {x for x in c if len(x) >= 4}

# 1. 解析表
wb = xlrd.open_workbook(XLS)
sh = wb.sheet_by_index(0)
part_infos = {}   # code -> set((col, cat))
for r in range(1, sh.nrows):
    for col, cat in CAT_MAP.items():
        v = sh.cell_value(r, col)
        if not isinstance(v, str): continue
        v = v.strip()
        if not v or any(b in v for b in BAD_WORDS): continue
        v = re.sub(r"[（(][^（）()]*[)）]", "", v).strip()
        if not v: continue
        code = norm(v)
        if len(code) >= 4:
            part_infos.setdefault(code, set()).add((col, cat))

# 补录基础型号(列-1标记，仅作收录，不参与"未找到"判定来源列展示时归到对应类)
for cat, codes in EXTRA.items():
    for code in codes:
        part_infos.setdefault(code, set()).add((-1, cat))

# 按「最长候选」降序（先决短码子串误吞长码）
def maxcand(p):
    return max(len(c) for c in candidates(p))
order = sorted(part_infos, key=lambda p: (-maxcand(p), p))

# 2. 扫描
picks = {}   # (cat, code) -> (ext_rank, stale, -bestlen, src)
for dp, dn, fns in os.walk(ROOT):
    dn[:] = [d for d in dn if d not in SKIP_DIRS]
    for fn in fns:
        ext = os.path.splitext(fn)[1].lower()
        if ext not in EXT_RANK: continue
        nfn = norm(fn)
        path_stale = 1 if any(k in dp.upper() or k in fn.upper() for k in STALE_KW) else 0
        # 对全部零件码计算实际最长命中长度，选择命中最精确的那个（解决短码抢占）
        best_code, best_len = None, 0
        for p in order:
            bl = 0
            for c in candidates(p):
                if c in nfn and len(c) > bl:
                    bl = len(c)
            if bl > best_len:
                best_len, best_code = bl, p
        if best_code:
            for col, cat in part_infos[best_code]:
                key = (cat, best_code)
                cur = picks.get(key)
                new = (EXT_RANK[ext], path_stale, -best_len, os.path.join(dp, fn))
                if cur is None or new[:3] < cur[:3]:
                    picks[key] = new

# 3. 复制
if os.path.isdir(STAGE):
    shutil.rmtree(STAGE)
os.makedirs(STAGE)
rows = []
by_cat = collections.defaultdict(list)
extra_note = []
for (cat, code), (rank, stale, nbl, src) in sorted(picks.items()):
    catdir = os.path.join(STAGE, cat)
    os.makedirs(catdir, exist_ok=True)
    ext = os.path.splitext(src)[1].lower()
    out = code.replace("*", "") + ext
    dst = os.path.join(catdir, out)
    shutil.copy2(src, dst)
    rows.append((cat, code, out, src))
    by_cat[cat].append(code)

# 报告
with open(os.path.join(STAGE, "提取明细.csv"), "w", newline="", encoding="utf-8-sig") as f:
    w = csv.writer(f); w.writerow(["分类", "零件代号", "目标文件名", "来源路径"]); w.writerows(rows)

allcodes = {c for c in part_infos}
foundcodes = {c for (_, c) in picks}
missing = collections.defaultdict(list)
for p in order:
    if p not in foundcodes:
        for col, cat in part_infos[p]:
            missing[cat].append(p)

print("复制完成: %d 文件" % len(rows))
for cat in sorted(set(CAT_MAP.values())):
    u = sorted(set(by_cat[cat]))
    print("  %s: %d 种零件 -> %s" % (cat, len(u), ", ".join(u)))

with open(os.path.join(STAGE, "未找到零件.txt"), "w", encoding="utf-8") as f:
    f.write("说明：以下零件代号在Y盘未找到对应模型/图纸文件。\n")
    f.write("其中 02TJK11SHXATI/NB/BL/GL/GN/HA 及 02TJKXDRHXA* 系列在盘上仅有基础型号 02TJK11SHXA / 02TJKXDRHXA，已收录基础型号文件。\n\n")
    for cat in sorted(missing):
        f.write("== %s ==\n" % cat)
        for p in sorted(set(missing[cat])):
            f.write("  %s\n" % p)
print("\n未找到:")
for cat, ps in sorted(missing.items()):
    print("  [%s] %d: %s" % (cat, len(set(ps)), " ".join(sorted(set(ps)))))
