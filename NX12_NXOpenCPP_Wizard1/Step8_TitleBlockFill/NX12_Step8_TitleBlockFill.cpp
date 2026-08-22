//------------------------------------------------------------------------------
// NX12 Step8：标题框日期/零件编号自动填写（DLL 8/8）—— v3 全诊断修复版
// 功能：
//   步骤1：系统日期（年.月.日，如 2026.08.22）填入标题框"日期"标签右侧单元格；
//   步骤2：当前图档文件名（不带路径、去掉 .prt 扩展名，大小写不敏感）
//          填入"零件编号"标签右侧单元格。
// 约束：只允许写这两个标签的右邻格，不得改动其他内容/图纸/视图/模型。
// 逻辑基准：E:\UG\date_marking.py（Python 原型，已废弃）。
//
// v3 变更（相对 v2，针对"运行后两格均未填写"问题）：
//   1) 枚举统计细分 subtype（0=节/1=表格/2=行/3=列/11=明细表），并单独统计
//      原生 NX TitleBlock 对象个数，一次运行即可判断标题栏是"普通表格注释"
//      还是"NX 原生 TitleBlock 对象"。
//   2) 标签匹配增强：UTF-8 合法性校验 + GBK(ANSI)→UTF-8 自动转码、去 BOM、
//      去全角空格与尾部冒号、紧凑比较（忽略全部空白）、ASCII 大小写不敏感；
//      命中方式（精确/紧凑/忽略大小写）全部记入日志。
//   3) 遍历范围增加标题行（UF_TABNOT_ask_nth_header_row，标题栏首行常为
//      标题行而非普通节行）；右邻格优先用 UF_TABNOT_ask_relative_column(+1)
//      （能正确处理合并列），失败回退列索引+1；标签位于最后一列时打印安全
//      跳过日志。
//   4) ask_cell_text / ask_evaluated_cell_text / ask_relative_column /
//      ask_merge_info / set_cell_text / UF_TABNOT_update 的返回码全部记录，
//      非 0 时附 UF_get_fail_message 文本（区分单元格锁定/电子表格驱动/
//      权限不足/API 失败）。
//   5) 可写判定扩展：占位符白名单扩充（中英文常见占位符）；旧日期识别支持
//      "."、"-"、"/" 与中文"年月日"分隔（含无前导零月/日），日期模式允许
//      刷新旧日期；零件编号严格不覆盖非占位符内容。
//   6) 降级路线重写：遍历 EditTitleBlockBuilder->Cells()，按标签格文本命中
//      后经底层表格注释单元格几何（ask_row_of_cell/ask_column_of_cell/
//      ask_relative_column）定位右邻格，优先 SetCellValueForLabel（单元格
//      标签名），无标签名时 SetEditableText，并记录 Lock 状态；主路线扫到
//      表格但未命中任何标签、且存在原生 TitleBlock 时也会自动追加该路线。
//   7) 菜单模式（非诊断）下若两步均未命中任何标签，自动转储全部表格单元格
//      （含 UTF-8 十六进制与求值文本），保证一次运行即可定位编码/结构问题。
//   8) 日期格式固定"年.月.日"（2026.08.22）；零件编号=当前图档文件名去扩展名
//      （大小写不敏感）；只写标签右邻格，其余内容不动。
//------------------------------------------------------------------------------

// Win32 防护（本模块需要 windows.h 做 GBK/UTF-8 转码）
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#ifdef CreateDialog
#undef CreateDialog
#endif

// 共享头文件
#include "../Shared/NX12_CommonConfig.h"
#include "../Shared/NX12_CommonUtils.h"

// Step8 专有 includes
#include <NXOpen/NXMessageBox.hxx>
#include <NXOpen/DisplayableObject.hxx>
#include <NXOpen/Drafting_DraftingApplicationManager.hxx>   // Part::DraftingManager()->TitleBlocks()（降级路线）
#include <NXOpen/Annotations_TitleBlockCollection.hxx>      // TitleBlockCollection / CreateEditTitleBlockBuilder
#include <NXOpen/Annotations_BaseTitleBlockBuilder.hxx>     // Builder::Cells()
#include <NXOpen/Annotations_EditTitleBlockBuilder.hxx>     // Get/SetCellValueForLabel（降级路线）
#include <NXOpen/Annotations_TitleBlock.hxx>
#include <NXOpen/Annotations_TitleBlockCellBuilder.hxx>     // Label/Lock/Text/EditableText/Cell()
#include <NXOpen/Annotations_TitleBlockCellBuilderList.hxx>
#include <NXOpen/MenuBar_MenuBarManager.hxx>                // ufsta 菜单应用注册
#include <NXOpen/MenuBar_MenuButtonEvent.hxx>
#include <NXOpen/Callback.hxx>                              // make_callback
#include <uf_tabnot.h>          // UF_TABNOT_*（表格注释单元格读写）
#include <uf_object_types.h>    // UF_tabular_note_type=165 / subtype 常量
#include <time.h>               // localtime_s
#include <stdio.h>              // sprintf_s
#include <string>
#include <vector>
#include <set>

//==============================================================================
// 配置区（静态常量，便于调整）
//==============================================================================

// 日期格式：%Y.%m.%d → 2026.08.22（sprintf_s("%04d.%02d.%02d") 实现）
static const char* const DATE_FMT_NOTE = "%Y.%m.%d";

// "日期"标签表（逐行精确匹配；支持中文与英文大小写）
static const char* const DATE_LABELS[] = { "日期", "Date", "DATE", "date" };

// "零件编号"标签表（逐行精确匹配；如需兼容其它叫法可扩展 "零件号"/"图号" 等）
static const char* const PARTNO_LABELS[] = { "零件编号" };

// 占位符白名单（目标格首个非空行清洗后等于其中任一项即允许覆盖写入；
// 匹配同样走"精确/紧凑/忽略大小写"三档，因此 "年  月  日" 也会命中 "年 月 日"）
static const char* const PLACEHOLDERS[] = {
	"<日期>", "YYYY.MM.DD", "yyyy.mm.dd", "YYYY/MM/DD", "年 月 日",
	"Date", "DATE", "date",
	"XXXX.XX.XX", "xxxx.xx.xx", "XXXX-XX-XX", "XXXX/XX/XX",
	"<零件编号>", "<零件号>", "XXXX", "xxxx", "XXX", "xxx",
	"待填", "待填写", "待定", "N/A", "NA", "None", "none", "/"
};

//==============================================================================
// 小型 RAII：包装 UF_TABNOT_ask_cell_text 返回的 char*（须 UF_free 释放）
//==============================================================================
struct UfCharPtr
{
	char* p;
	UfCharPtr() : p(NULL) {}
	~UfCharPtr() { if (p) { UF_free(p); p = NULL; } }
	const char* c_str() const { return p ? p : ""; }
private:
	UfCharPtr(const UfCharPtr&);
	UfCharPtr& operator=(const UfCharPtr&);
};

//==============================================================================
// 编码处理
// NX 内部文本通常为 UTF-8，但个别环境/旧部件中 UF_TABNOT 可能返回 GBK(ANSI)
// 字节串。策略：读到文本先做 UTF-8 合法性校验，非法则按系统代码页(CP_ACP，
// 中文系统即 GBK)转成 UTF-8；一旦发现 ANSI 文本（g_cellTextIsAnsi），写入时
// 也把非 ASCII 值转回 CP_ACP，保证读写编码一致。
//==============================================================================
static bool g_cellTextIsAnsi = false;   // 本次运行是否检出 ANSI(GBK) 单元格文本

// 严格 UTF-8 合法性校验（含 overlong 检查）
static bool is_valid_utf8(const std::string& s)
{
	size_t i = 0, n = s.size();
	while (i < n)
	{
		unsigned char c = (unsigned char)s[i];
		if (c < 0x80) { ++i; continue; }
		int len; unsigned int cp;
		if ((c & 0xE0) == 0xC0)      { len = 2; cp = c & 0x1F; }
		else if ((c & 0xF0) == 0xE0) { len = 3; cp = c & 0x0F; }
		else if ((c & 0xF8) == 0xF0) { len = 4; cp = c & 0x07; }
		else return false;
		if (i + len > n) return false;
		for (int k = 1; k < len; ++k)
		{
			unsigned char cc = (unsigned char)s[i + k];
			if ((cc & 0xC0) != 0x80) return false;
			cp = (cp << 6) | (cc & 0x3F);
		}
		if (len == 2 && cp < 0x80) return false;
		if (len == 3 && cp < 0x800) return false;
		if (len == 4 && cp < 0x10000) return false;
		i += len;
	}
	return true;
}

// ANSI(CP_ACP) → UTF-8；已是合法 UTF-8 或纯 ASCII 时原样返回
static std::string ansi_to_utf8(const std::string& s)
{
	if (s.empty() || is_valid_utf8(s)) return s;
	int wLen = MultiByteToWideChar(CP_ACP, 0, s.c_str(), (int)s.size(), NULL, 0);
	if (wLen <= 0) return s;
	std::vector<WCHAR> w(wLen);
	if (MultiByteToWideChar(CP_ACP, 0, s.c_str(), (int)s.size(), &w[0], wLen) != wLen)
		return s;
	int uLen = WideCharToMultiByte(CP_UTF8, 0, &w[0], wLen, NULL, 0, NULL, NULL);
	if (uLen <= 0) return s;
	std::string out((size_t)uLen, '\0');
	WideCharToMultiByte(CP_UTF8, 0, &w[0], wLen, &out[0], uLen, NULL, NULL);
	return out;
}

// UTF-8 → ANSI(CP_ACP)；纯 ASCII 原样返回（日期/编号多为 ASCII，避免无谓转换）
static std::string utf8_to_ansi(const std::string& s)
{
	bool nonAscii = false;
	for (size_t i = 0; i < s.size(); ++i)
		if ((unsigned char)s[i] >= 0x80) { nonAscii = true; break; }
	if (!nonAscii) return s;
	int wLen = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), NULL, 0);
	if (wLen <= 0) return s;
	std::vector<WCHAR> w(wLen);
	if (MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], wLen) != wLen)
		return s;
	int aLen = WideCharToMultiByte(CP_ACP, 0, &w[0], wLen, NULL, 0, NULL, NULL);
	if (aLen <= 0) return s;
	std::string out((size_t)aLen, '\0');
	WideCharToMultiByte(CP_ACP, 0, &w[0], wLen, &out[0], aLen, NULL, NULL);
	return out;
}

// 单元格文本 → 统一 UTF-8；converted 指示是否发生了 GBK→UTF-8 转码
static std::string normalize_cell_text(const std::string& raw, bool& converted)
{
	converted = false;
	std::string s = raw;
	// 去掉可能存在的 UTF-8 BOM
	if (s.size() >= 3 && s.compare(0, 3, "\xEF\xBB\xBF") == 0) s.erase(0, 3);
	if (!s.empty() && !is_valid_utf8(s))
	{
		std::string t = ansi_to_utf8(s);
		if (t != s) { converted = true; g_cellTextIsAnsi = true; s = t; }
	}
	return s;
}

// 写入值编码：检出 ANSI 单元格文本时按 CP_ACP 编码，否则 UTF-8 原样
static std::string encode_for_cell(const std::string& utf8Value)
{
	return g_cellTextIsAnsi ? utf8_to_ansi(utf8Value) : utf8Value;
}

//==============================================================================
// 单元格文本清洗：trim 首尾空白（ASCII 空白 + 全角空格 U+3000 的 UTF-8 序列
// E3 80 80），并去掉尾部 ':' / '：'（全角冒号 UTF-8 序列 EF BC 9A）。
// 反复执行直至稳定（如 "日期 ：" → "日期"）。
//==============================================================================
static std::string clean_cell_text(const std::string& raw)
{
	std::string s = raw;
	const char* kFwSpace = "\xE3\x80\x80";    // U+3000 全角空格 UTF-8
	const char* kFwColon = "\xEF\xBC\x9A";    // U+FF1A 全角冒号 UTF-8
	for (;;)
	{
		bool changed = false;

		// 尾部 ASCII 空白
		while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n'))
		{ s.pop_back(); changed = true; }
		// 首部 ASCII 空白
		size_t start = 0;
		while (start < s.size() && (s[start] == ' ' || s[start] == '\t' || s[start] == '\r' || s[start] == '\n'))
			++start;
		if (start > 0) { s.erase(0, start); changed = true; }

		// 首部/尾部 全角空格 U+3000
		while (s.size() >= 3 && s.compare(0, 3, kFwSpace) == 0) { s.erase(0, 3); changed = true; }
		while (s.size() >= 3 && s.compare(s.size() - 3, 3, kFwSpace) == 0) { s.erase(s.size() - 3); changed = true; }

		// 尾部半角冒号 ':' 与全角冒号 '：'
		if (!s.empty() && s.back() == ':') { s.pop_back(); changed = true; }
		if (s.size() >= 3 && s.compare(s.size() - 3, 3, kFwColon) == 0) { s.erase(s.size() - 3); changed = true; }

		if (!changed) break;
	}
	return s;
}

// 紧凑文本：去掉全部 ASCII 空白与全角空格（用于"日 期"、"零件 编号"等情形）
static std::string compact_text(const std::string& s)
{
	std::string o;
	o.reserve(s.size());
	for (size_t i = 0; i < s.size();)
	{
		unsigned char c = (unsigned char)s[i];
		if (c == ' ' || c == '\t' || c == '\r' || c == '\n') { ++i; continue; }
		if (c == 0xE3 && i + 2 < s.size() &&
			(unsigned char)s[i + 1] == 0x80 && (unsigned char)s[i + 2] == 0x80)
		{ i += 3; continue; }   // U+3000 全角空格
		o += (char)c;
		++i;
	}
	return o;
}

// ASCII 小写（仅对 A-Z 转换，不影响 UTF-8 多字节字符）
static std::string ascii_lower(const std::string& s)
{
	std::string o = s;
	for (size_t i = 0; i < o.size(); ++i)
		if (o[i] >= 'A' && o[i] <= 'Z') o[i] = (char)(o[i] - 'A' + 'a');
	return o;
}

//==============================================================================
// 按 \n / \r 拆行（\r\n 视为一个换行）。空串返回单个空行。
//==============================================================================
static void split_lines(const std::string& s, std::vector<std::string>& out)
{
	out.clear();
	std::string cur;
	for (size_t i = 0; i < s.size(); ++i)
	{
		char c = s[i];
		if (c == '\n' || c == '\r')
		{
			out.push_back(cur);
			cur.clear();
			if (c == '\r' && i + 1 < s.size() && s[i + 1] == '\n') ++i;
		}
		else cur.push_back(c);
	}
	out.push_back(cur);
}

//==============================================================================
// 显示用转义：\n→"\\n" \r→"\\r" \t→"\\t"（便于单行打印多行单元格）
//==============================================================================
static std::string escape_text(const std::string& s)
{
	std::string o;
	o.reserve(s.size() + 8);
	for (size_t i = 0; i < s.size(); ++i)
	{
		switch (s[i])
		{
		case '\n': o += "\\n"; break;
		case '\r': o += "\\r"; break;
		case '\t': o += "\\t"; break;
		default:   o += s[i]; break;
		}
	}
	return o;
}

//==============================================================================
// UTF-8 十六进制字节序列（编码对照诊断用）
//==============================================================================
static std::string hex_bytes(const std::string& s)
{
	std::string o;
	char b[8];
	for (size_t i = 0; i < s.size(); ++i)
	{
		sprintf_s(b, sizeof(b), "%02X ", (unsigned char)s[i]);
		o += b;
	}
	if (!o.empty()) o.erase(o.size() - 1);   // 去末尾空格
	return o;
}

//==============================================================================
// 旧日期判定（仅"日期"步骤允许刷新旧日期；零件编号不允许覆盖非占位符内容）。
// 支持：2026.08.22 / 2026-08-22 / 2026/08/22（月日可无前导零，同一分隔符）、
//       2026年8月22日 / 2026年08月22日。
//==============================================================================
static bool digits_at(const std::string& s, size_t pos, size_t count)
{
	if (pos + count > s.size()) return false;
	for (size_t i = pos; i < pos + count; ++i)
		if (s[i] < '0' || s[i] > '9') return false;
	return true;
}

static bool is_old_date(const std::string& s)
{
	// 1) 4 位年 + 分隔符(. - /) + 1~2 位月 + 同分隔符 + 1~2 位日
	if (s.size() >= 8 && digits_at(s, 0, 4) &&
		(s[4] == '.' || s[4] == '-' || s[4] == '/'))
	{
		size_t p = 5, m = p;
		while (m < s.size() && s[m] >= '0' && s[m] <= '9') ++m;
		if (m > p && m - p <= 2 && m < s.size() && s[m] == s[4])
		{
			size_t q = m + 1, d = q;
			while (d < s.size() && s[d] >= '0' && s[d] <= '9') ++d;
			if (d > q && d - q <= 2 && d == s.size())
				return true;
		}
	}

	// 2) 中文分隔：2026年8月22日 / 2026年08月22日
	static const char* kYear  = "\xE5\xB9\xB4";   // 年
	static const char* kMonth = "\xE6\x9C\x88";   // 月
	static const char* kDay   = "\xE6\x97\xA5";   // 日
	size_t p = 0;
	if (!digits_at(s, p, 4)) return false;
	p += 4;
	if (p + 3 > s.size() || s.compare(p, 3, kYear) != 0) return false;
	p += 3;
	size_t m = p;
	while (m < s.size() && s[m] >= '0' && s[m] <= '9') ++m;
	if (m == p || m - p > 2) return false;
	p = m;
	if (p + 3 > s.size() || s.compare(p, 3, kMonth) != 0) return false;
	p += 3;
	size_t d = p;
	while (d < s.size() && s[d] >= '0' && s[d] <= '9') ++d;
	if (d == p || d - p > 2) return false;
	p = d;
	if (p + 3 > s.size() || s.compare(p, 3, kDay) != 0) return false;
	p += 3;
	return p == s.size();
}

//==============================================================================
// 标签精确匹配（清洗后文本 ∈ 标签表；含三档：精确 / 紧凑去空白 / 忽略大小写）
//==============================================================================
static bool match_one_label(const std::string& cleaned,
	const char* const labels[], size_t nLabels, std::string* modeOut)
{
	for (size_t i = 0; i < nLabels; ++i)
	{
		const std::string lbl = labels[i];
		if (cleaned == lbl)
		{
			if (modeOut) *modeOut = "精确";
			return true;
		}
		const std::string cc = compact_text(cleaned);
		const std::string lc = compact_text(lbl);
		if (cc == lc)
		{
			if (modeOut) *modeOut = "紧凑(忽略空白)";
			return true;
		}
		const std::string clw = ascii_lower(cc);
		const std::string llw = ascii_lower(lc);
		if (!clw.empty() && clw == llw)
		{
			if (modeOut) *modeOut = "紧凑+忽略大小写";
			return true;
		}
	}
	return false;
}

static bool is_placeholder(const std::string& cleaned)
{
	const size_t n = sizeof(PLACEHOLDERS) / sizeof(PLACEHOLDERS[0]);
	return match_one_label(cleaned, PLACEHOLDERS, n, NULL);
}

//==============================================================================
// 多行标签匹配：按 \n/\r 拆行，逐行清洗后匹配；任一行命中即视为标签格。
// 输入应为 normalize_cell_text 之后的 UTF-8 文本。
//==============================================================================
static bool match_label_multiline(const std::string& normRaw,
	const char* const labels[], size_t nLabels,
	std::string& hitLine, std::string& hitMode)
{
	std::vector<std::string> lines;
	split_lines(normRaw, lines);
	for (size_t i = 0; i < lines.size(); ++i)
	{
		std::string c = clean_cell_text(lines[i]);
		if (c.empty()) continue;
		if (match_one_label(c, labels, nLabels, &hitMode))
		{
			hitLine = c;
			return true;
		}
	}
	return false;
}

//==============================================================================
// 目标值格可写判定（多行：取第一个非空行判定）
//   空（无任何非空行） / 占位符白名单 → 可写；
//   refreshOldDate=true（日期模式）时额外允许覆盖旧日期。
// outFirst 输出第一个非空清洗行（日志用）。
//==============================================================================
static bool judge_writable(const std::string& targetNorm, bool refreshOldDate,
	std::string& outFirst, std::string& outReason)
{
	std::vector<std::string> lines;
	split_lines(targetNorm, lines);
	outFirst.clear();
	for (size_t i = 0; i < lines.size(); ++i)
	{
		std::string c = clean_cell_text(lines[i]);
		if (!c.empty()) { outFirst = c; break; }
	}
	if (outFirst.empty())
	{
		outReason = "空(可写)";
		return true;
	}
	if (is_placeholder(outFirst))
	{
		outReason = "占位符(可写)";
		return true;
	}
	if (refreshOldDate && is_old_date(outFirst))
	{
		outReason = "旧日期(可刷新)";
		return true;
	}
	outReason = refreshOldDate ? "非空且非占位符/非旧日期(不覆盖)" : "非空且非占位符(不覆盖)";
	return false;
}

//==============================================================================
// 读取单元格定义文本（自动 UF_free）。成功返回 true 并写 out（未清洗原文）。
//==============================================================================
static bool ask_cell_text_str(tag_t cell, std::string& out, int* rcOut)
{
	UfCharPtr txt;
	int rc = UF_TABNOT_ask_cell_text(cell, &txt.p);
	if (rcOut) *rcOut = rc;
	if (rc != 0) { out.clear(); return false; }
	out = txt.c_str();
	return true;
}

// 读取单元格求值文本（区分"定义文本=引用表达式"与"实际显示值"）
static bool ask_evaluated_cell_text_str(tag_t cell, std::string& out, int* rcOut)
{
	UfCharPtr txt;
	int rc = UF_TABNOT_ask_evaluated_cell_text(cell, &txt.p);
	if (rcOut) *rcOut = rc;
	if (rc != 0) { out.clear(); return false; }
	out = txt.c_str();
	return true;
}

//==============================================================================
// 填写统计
//==============================================================================
struct FillStats
{
	int filled;     // 成功写入次数
	int skipped;    // 目标格已有内容且不允许覆盖（跳过）
	int failures;   // set_cell_text 失败次数（锁定/电子表格驱动等）
	int hits;       // 命中标签格的次数（含随后被跳过的）
	FillStats() : filled(0), skipped(0), failures(0), hits(0) {}
	void add(const FillStats& o)
	{
		filled += o.filled; skipped += o.skipped; failures += o.failures; hits += o.hits;
	}
};

//==============================================================================
// 索引基数探测：ask_nth_* 系列头文件未注明 0/1 基。先试索引 0，失败
// （返回码非 0 或 NULL_TAG）再试索引 1；打印探测过程便于定位基数问题。
//==============================================================================
typedef int (*AskNthFn)(tag_t, int, tag_t*);

static int detect_index_base(const char* what, tag_t owner, AskNthFn fn, bool diagnostic)
{
	char fmt[512];
	for (int base = 0; base <= 1; ++base)
	{
		tag_t t = NULL_TAG;
		int rc = fn(owner, base, &t);
		if (diagnostic)
		{
			sprintf_s(fmt, sizeof(fmt), "    [基数探测] %s index=%d rc=%d tag=%llu",
				what, base, rc, (unsigned long long)t);
			CommonUtils::print_msg(fmt);
		}
		if (rc == 0 && t != NULL_TAG)
			return base;
	}
	sprintf_s(fmt, sizeof(fmt), "    [基数探测] %s 索引 0/1 均失败，默认按 0 基处理", what);
	CommonUtils::print_msg(fmt);
	return 0;
}

//==============================================================================
// 枚举统计（细分 subtype，便于判断标题栏真实形态）
//==============================================================================
struct NoteCensus
{
	int total;      // cycle 返回的 type=165 对象总数
	int tables;     // subtype 1 表格注释（主路线处理对象）
	int sections;   // subtype 0 节
	int rows;       // subtype 2 行
	int columns;    // subtype 3 列
	int partsLists; // subtype 11 明细表
	int others;     // 其它 subtype
	NoteCensus() : total(0), tables(0), sections(0), rows(0), columns(0), partsLists(0), others(0) {}
};

//==============================================================================
// 枚举部件内表格注释（type=165，过滤 subtype==1 保留表格本体）
//==============================================================================
static void enum_tabular_notes(NXOpen::Part* workPart,
	std::vector<tag_t>& filtered, bool diagnostic, NoteCensus& census)
{
	char fmt[512];
	filtered.clear();
	census = NoteCensus();

	tag_t obj = NULL_TAG;
	for (;;)
	{
		int rc = UF_OBJ_cycle_objs_in_part(workPart->Tag(), UF_tabular_note_type, &obj);
		if (rc != 0)
		{
			char errBuf[133]; errBuf[0] = '\0';
			UF_get_fail_message(rc, errBuf);
			sprintf_s(fmt, sizeof(fmt),
				"  [枚举] UF_OBJ_cycle_objs_in_part 失败 rc=%d (%s)，中止枚举", rc, errBuf);
			CommonUtils::print_msg(fmt);
			break;
		}
		if (obj == NULL_TAG) break;   // 枚举结束
		++census.total;

		int t = 0, s = 0;
		int rc2 = UF_OBJ_ask_type_and_subtype(obj, &t, &s);
		if (diagnostic)
		{
			sprintf_s(fmt, sizeof(fmt),
				"  [枚举] #%d tag=%llu type=%d subtype=%d (ask_type_and_subtype rc=%d)",
				census.total, (unsigned long long)obj, t, s, rc2);
			CommonUtils::print_msg(fmt);
		}
		if (t != UF_tabular_note_type) { ++census.others; continue; }
		switch (s)
		{
		case UF_tabular_note_subtype:        ++census.tables;     filtered.push_back(obj); break;
		case UF_tabular_note_section_subtype: ++census.sections;  break;
		case UF_tabular_note_row_subtype:     ++census.rows;      break;
		case UF_tabular_note_column_subtype:  ++census.columns;   break;
		case UF_parts_list_subtype:           ++census.partsLists; break;
		default:                              ++census.others;    break;
		}
	}

	sprintf_s(fmt, sizeof(fmt),
		"[枚举结果] type=165 对象共 %d 个：表格注释 %d / 节 %d / 行 %d / 列 %d / 明细表 %d / 其它 %d（主路线取表格注释 %d 个）",
		census.total, census.tables, census.sections, census.rows, census.columns,
		census.partsLists, census.others, (int)filtered.size());
	CommonUtils::print_msg(fmt);

	if (filtered.empty())
	{
		CommonUtils::print_msg(
			"  [提示] 未扫到 subtype==1 的表格注释。若标题框确实存在，可能原因："
			"标题框是 NX 原生 TitleBlock 对象（将走降级路线）、表格位于组件/模板部件中"
			"（当前扫描的是工作部件），或标题框不是表格类对象（普通注释/直线图框）。");
	}
}

//==============================================================================
// 统计原生 TitleBlock 对象（判断标题栏是否为 NX 原生 TitleBlock）
//==============================================================================
static int count_title_blocks(NXOpen::Part* workPart)
{
	char fmt[512];
	try
	{
		NXOpen::Annotations::TitleBlockCollection* tbColl =
			workPart->DraftingManager()->TitleBlocks();
		if (!tbColl)
		{
			CommonUtils::print_msg("[结构] 原生 TitleBlock 集合不可用（TitleBlocks() 返回 NULL）");
			return 0;
		}
		int n = 0;
		for (NXOpen::Annotations::TitleBlockCollection::iterator it = tbColl->begin();
			it != tbColl->end(); ++it)
		{
			if (*it) ++n;
		}
		sprintf_s(fmt, sizeof(fmt), "[结构] NX 原生 TitleBlock 对象: %d 个", n);
		CommonUtils::print_msg(fmt);
		return n;
	}
	catch (const NXOpen::NXException& e)
	{
		sprintf_s(fmt, sizeof(fmt), "[结构] 枚举原生 TitleBlock 异常: %s", e.Message());
		CommonUtils::print_msg(fmt);
		return 0;
	}
	catch (...)
	{
		CommonUtils::print_msg("[结构] 枚举原生 TitleBlock 异常（未知）");
		return 0;
	}
}

//==============================================================================
// 诊断模式：标签候选表十六进制打印（与单元格 hex 对照，定位编码问题）
//==============================================================================
static void dump_label_table_hex(const char* const labels[], size_t nLabels, const char* groupName)
{
	char fmt[512];
	sprintf_s(fmt, sizeof(fmt), "--- 标签候选表 [%s] UTF-8 十六进制 ---", groupName);
	CommonUtils::print_msg(fmt);
	for (size_t i = 0; i < nLabels; ++i)
	{
		std::string s = labels[i];
		sprintf_s(fmt, sizeof(fmt), "  候选%02d: '%s' [%d 字节]", (int)i, labels[i], (int)s.size());
		CommonUtils::print_msg(fmt);
		CommonUtils::print_msg(std::string("          ") + hex_bytes(s));
	}
	const size_t nPh = sizeof(PLACEHOLDERS) / sizeof(PLACEHOLDERS[0]);
	CommonUtils::print_msg("--- 占位符白名单 UTF-8 十六进制 ---");
	for (size_t i = 0; i < nPh; ++i)
	{
		std::string s = PLACEHOLDERS[i];
		sprintf_s(fmt, sizeof(fmt), "  占位%02d: '%s' [%d 字节]", (int)i, PLACEHOLDERS[i], (int)s.size());
		CommonUtils::print_msg(fmt);
		CommonUtils::print_msg(std::string("          ") + hex_bytes(s));
	}
}

//==============================================================================
// 诊断转储：完整打印每个表格的结构与所有单元格文本（多行以 \n 转义显示，
// 含原始十六进制、求值文本与合并信息；带输出量上限防刷屏）
//==============================================================================
static void dump_all_tables(const std::vector<tag_t>& notes)
{
	char fmt[512];
	CommonUtils::print_msg("===== 表格单元格完整转储 =====");

	const int kMaxTables  = 8;
	const int kMaxSections = 12;
	const int kMaxRows    = 40;
	const int kMaxCols    = 40;
	const int kMaxLines   = 1200;
	int lineBudget = kMaxLines;

	for (size_t ni = 0; ni < notes.size() && ni < (size_t)kMaxTables && lineBudget > 0; ++ni)
	{
		tag_t note = notes[ni];

		int nSec = 0, nCols = 0, nHR = 0;
		int rcS = UF_TABNOT_ask_nm_sections(note, &nSec);
		int rcC = UF_TABNOT_ask_nm_columns(note, &nCols);
		int rcH = UF_TABNOT_ask_nm_header_rows(note, &nHR);
		sprintf_s(fmt, sizeof(fmt),
			"[表格 %d] tag=%llu 节=%d(rc=%d) 列=%d(rc=%d) 标题行=%d(rc=%d)",
			(int)ni + 1, (unsigned long long)note, nSec, rcS, nCols, rcC, nHR, rcH);
		CommonUtils::print_msg(fmt);
		--lineBudget;

		if (rcS != 0 || nSec <= 0 || rcC != 0 || nCols <= 0)
		{
			CommonUtils::print_msg("  结构查询失败或为空，跳过该表格");
			--lineBudget;
			continue;
		}

		int baseSec = detect_index_base("ask_nth_section", note, UF_TABNOT_ask_nth_section, true);
		int baseCol = detect_index_base("ask_nth_column", note, UF_TABNOT_ask_nth_column, true);
		sprintf_s(fmt, sizeof(fmt), "  [基数结论] section=%d 基, column=%d 基", baseSec, baseCol);
		CommonUtils::print_msg(fmt);
		--lineBudget;

		std::vector<tag_t> cols(nCols, NULL_TAG);
		for (int ci = 0; ci < nCols && ci < kMaxCols; ++ci)
		{
			int rc = UF_TABNOT_ask_nth_column(note, ci + baseCol, &cols[ci]);
			if (rc != 0 || cols[ci] == NULL_TAG)
			{
				sprintf_s(fmt, sizeof(fmt), "  ask_nth_column(index=%d) rc=%d tag=%llu",
					ci + baseCol, rc, (unsigned long long)cols[ci]);
				CommonUtils::print_msg(fmt);
				if (--lineBudget <= 0) break;
			}
		}

		// 标题行
		if (rcH == 0 && nHR > 0 && lineBudget > 0)
		{
			int baseHR = detect_index_base("ask_nth_header_row", note, UF_TABNOT_ask_nth_header_row, true);
			for (int hi = 0; hi < nHR && hi < kMaxRows && lineBudget > 0; ++hi)
			{
				tag_t row = NULL_TAG;
				int rc = UF_TABNOT_ask_nth_header_row(note, hi + baseHR, &row);
				if (rc != 0 || row == NULL_TAG)
				{
					sprintf_s(fmt, sizeof(fmt), "  ask_nth_header_row(index=%d) rc=%d tag=%llu",
						hi + baseHR, rc, (unsigned long long)row);
					CommonUtils::print_msg(fmt);
					--lineBudget;
					continue;
				}
				for (int ci = 0; ci < nCols && ci < kMaxCols && lineBudget > 0; ++ci)
				{
					if (cols[ci] == NULL_TAG) continue;
					tag_t cell = NULL_TAG;
					int rcCell = UF_TABNOT_ask_cell_at_row_col(row, cols[ci], &cell);
					if (rcCell != 0 || cell == NULL_TAG)
					{
						sprintf_s(fmt, sizeof(fmt), "  [H%d c%d] ask_cell_at_row_col rc=%d", hi, ci, rcCell);
						CommonUtils::print_msg(fmt);
						--lineBudget;
						continue;
					}
					std::string raw; int arc = 0;
					if (!ask_cell_text_str(cell, raw, &arc))
					{
						sprintf_s(fmt, sizeof(fmt), "  [H%d c%d] ask_cell_text rc=%d", hi, ci, arc);
						CommonUtils::print_msg(fmt);
						--lineBudget;
						continue;
					}
					std::string ev; int erc = 0;
					ask_evaluated_cell_text_str(cell, ev, &erc);
					bool conv = false;
					std::string norm = normalize_cell_text(raw, conv);
					sprintf_s(fmt, sizeof(fmt), "  [H%d c%d] 原文='%s'", hi, ci, escape_text(raw).c_str());
					CommonUtils::print_msg(fmt); --lineBudget;
					sprintf_s(fmt, sizeof(fmt), "           hex: %s%s", hex_bytes(raw).c_str(),
						conv ? "  ← 非UTF-8，已按GBK转码" : "");
					CommonUtils::print_msg(fmt); --lineBudget;
					if (!ev.empty() && ev != raw)
					{
						sprintf_s(fmt, sizeof(fmt), "           求值文本='%s' (rc=%d)", escape_text(ev).c_str(), erc);
						CommonUtils::print_msg(fmt); --lineBudget;
					}
				}
			}
		}

		// 节 → 行
		for (int si = 0; si < nSec && si < kMaxSections && lineBudget > 0; ++si)
		{
			tag_t section = NULL_TAG;
			int rc = UF_TABNOT_ask_nth_section(note, si + baseSec, &section);
			if (rc != 0 || section == NULL_TAG)
			{
				sprintf_s(fmt, sizeof(fmt), "  ask_nth_section(index=%d) rc=%d tag=%llu",
					si + baseSec, rc, (unsigned long long)section);
				CommonUtils::print_msg(fmt);
				--lineBudget;
				continue;
			}

			int nRows = 0;
			int rcR = UF_TABNOT_ask_nm_rows_in_section(section, &nRows);
			sprintf_s(fmt, sizeof(fmt), "  section[%d] tag=%llu rows=%d(rc=%d)",
				si, (unsigned long long)section, nRows, rcR);
			CommonUtils::print_msg(fmt);
			--lineBudget;
			if (rcR != 0 || nRows <= 0) continue;

			int baseRow = detect_index_base("ask_nth_row_in_section", section,
				UF_TABNOT_ask_nth_row_in_section, true);

			for (int ri = 0; ri < nRows && ri < kMaxRows && lineBudget > 0; ++ri)
			{
				tag_t row = NULL_TAG;
				rc = UF_TABNOT_ask_nth_row_in_section(section, ri + baseRow, &row);
				if (rc != 0 || row == NULL_TAG)
				{
					sprintf_s(fmt, sizeof(fmt), "  ask_nth_row_in_section(index=%d) rc=%d tag=%llu",
						ri + baseRow, rc, (unsigned long long)row);
					CommonUtils::print_msg(fmt);
					--lineBudget;
					continue;
				}

				for (int ci = 0; ci < nCols && ci < kMaxCols && lineBudget > 0; ++ci)
				{
					if (cols[ci] == NULL_TAG) continue;
					tag_t cell = NULL_TAG;
					int rcCell = UF_TABNOT_ask_cell_at_row_col(row, cols[ci], &cell);
					if (rcCell != 0 || cell == NULL_TAG)
					{
						sprintf_s(fmt, sizeof(fmt), "  [s%d r%d c%d] ask_cell_at_row_col rc=%d", si, ri, ci, rcCell);
						CommonUtils::print_msg(fmt);
						--lineBudget;
						continue;
					}
					std::string raw; int arc = 0;
					if (!ask_cell_text_str(cell, raw, &arc))
					{
						sprintf_s(fmt, sizeof(fmt), "  [s%d r%d c%d] ask_cell_text rc=%d", si, ri, ci, arc);
						CommonUtils::print_msg(fmt);
						--lineBudget;
						continue;
					}
					std::string ev; int erc = 0;
					ask_evaluated_cell_text_str(cell, ev, &erc);
					bool conv = false;
					std::string norm = normalize_cell_text(raw, conv);
					(void)norm;
					sprintf_s(fmt, sizeof(fmt), "  [s%d r%d c%d] 原文='%s'", si, ri, ci, escape_text(raw).c_str());
					CommonUtils::print_msg(fmt); --lineBudget;
					sprintf_s(fmt, sizeof(fmt), "           hex: %s%s", hex_bytes(raw).c_str(),
						conv ? "  ← 非UTF-8，已按GBK转码" : "");
					CommonUtils::print_msg(fmt); --lineBudget;
					if (!ev.empty() && ev != raw)
					{
						sprintf_s(fmt, sizeof(fmt), "           求值文本='%s' (rc=%d)", escape_text(ev).c_str(), erc);
						CommonUtils::print_msg(fmt); --lineBudget;
					}
					tag_t sr = NULL_TAG, sc = NULL_TAG, er = NULL_TAG, ec = NULL_TAG;
					if (UF_TABNOT_ask_merge_info(cell, &sr, &sc, &er, &ec) == 0 && sr != NULL_TAG)
					{
						sprintf_s(fmt, sizeof(fmt), "           合并单元格(父格): 行=%llu 列=%llu → 行=%llu 列=%llu",
							(unsigned long long)sr, (unsigned long long)sc,
							(unsigned long long)er, (unsigned long long)ec);
						CommonUtils::print_msg(fmt); --lineBudget;
					}
				}
			}
		}
	}
	if (lineBudget <= 0)
		CommonUtils::print_msg("===== 转储因行数上限被截断 =====");
	CommonUtils::print_msg("===== 表格单元格完整转储结束 =====");
}

//==============================================================================
// process_row_cells —— 单行遍历（普通节行与标题行共用）
// 多行匹配定位标签格，命中后经 ask_relative_column(+1) 取同行右邻格
// （失败回退列索引+1）；最后一列安全跳过。写入判定：空/占位符 → 写入，
// refreshOldDate=true 时额外允许覆盖旧日期；其余跳过并记日志。
// 所有关键 API 返回码全部记录。
//==============================================================================
static void process_row_cells(tag_t note, tag_t row, const char* rowDesc,
	const std::vector<tag_t>& cols,
	const char* const labels[], size_t nLabels,
	const char* value, bool refreshOldDate,
	const char* stepName, bool diagnostic,
	FillStats& st, bool& noteModified, std::set<tag_t>& writtenCells)
{
	char fmt[512];
	const int nCols = (int)cols.size();

	for (int ci = 0; ci < nCols; ++ci)
	{
		if (cols[ci] == NULL_TAG) continue;

		// ---- 取当前单元格 ----
		tag_t cell = NULL_TAG;
		int rcCell = UF_TABNOT_ask_cell_at_row_col(row, cols[ci], &cell);
		if (rcCell != 0 || cell == NULL_TAG)
		{
			if (diagnostic)
			{
				sprintf_s(fmt, sizeof(fmt), "  [%s] [%s c%d] ask_cell_at_row_col rc=%d，跳过",
					stepName, rowDesc, ci, rcCell);
				CommonUtils::print_msg(fmt);
			}
			continue;
		}

		// ---- 读标签格文本（返回码记录） ----
		std::string raw; int arc = 0;
		if (!ask_cell_text_str(cell, raw, &arc))
		{
			char errBuf[133]; errBuf[0] = '\0';
			UF_get_fail_message(arc, errBuf);
			sprintf_s(fmt, sizeof(fmt), "  [%s] [%s c%d] ask_cell_text 失败 rc=%d (%s)，跳过",
				stepName, rowDesc, ci, arc, errBuf);
			CommonUtils::print_msg(fmt);
			continue;
		}

		bool conv = false;
		std::string norm = normalize_cell_text(raw, conv);
		if (conv)
		{
			sprintf_s(fmt, sizeof(fmt), "  [%s] [%s c%d] 检测到非UTF-8(GBK?)单元格文本，已转码，原文hex: %s",
				stepName, rowDesc, ci, hex_bytes(raw).c_str());
			CommonUtils::print_msg(fmt);
		}

		// ---- 多行标签匹配 ----
		std::string hitLine, hitMode;
		if (!match_label_multiline(norm, labels, nLabels, hitLine, hitMode))
		{
			// 近似命中诊断：含候选子串但拆行后无精确匹配 → 打印 hex
			for (size_t li = 0; li < nLabels; ++li)
			{
				if (norm.find(labels[li]) != std::string::npos)
				{
					sprintf_s(fmt, sizeof(fmt),
						"  [%s] 近似命中(未精确匹配): 表格tag=%llu %s c%d 原文='%s' 含候选'%s'",
						stepName, (unsigned long long)note, rowDesc, ci,
						escape_text(raw).c_str(), labels[li]);
					CommonUtils::print_msg(fmt);
					CommonUtils::print_msg(std::string("        hex: ") + hex_bytes(raw));
					break;
				}
			}
			continue;
		}

		// ---- 命中标签 ----
		++st.hits;
		sprintf_s(fmt, sizeof(fmt),
			"  [%s] 命中标签: 表格tag=%llu %s c%d 命中行='%s' 匹配方式=%s 原格文本='%s'",
			stepName, (unsigned long long)note, rowDesc, ci,
			hitLine.c_str(), hitMode.c_str(), escape_text(raw).c_str());
		CommonUtils::print_msg(fmt);

		// ---- 取同行右邻列（优先相对列 API，可正确处理合并列） ----
		tag_t rightCol = NULL_TAG;
		int rrc = UF_TABNOT_ask_relative_column(cols[ci], 1, &rightCol);
		if (rrc != 0 || rightCol == NULL_TAG)
		{
			sprintf_s(fmt, sizeof(fmt),
				"  [%s] ask_relative_column(+1) rc=%d，回退为列索引+1", stepName, rrc);
			CommonUtils::print_msg(fmt);
			if (ci + 1 < nCols) rightCol = cols[ci + 1];
		}
		if (rightCol == NULL_TAG)
		{
			sprintf_s(fmt, sizeof(fmt),
				"  [%s] 命中标签 '%s' 但无右邻格（最后一列），安全跳过 (表格tag=%llu %s c%d)",
				stepName, hitLine.c_str(), (unsigned long long)note, rowDesc, ci);
			CommonUtils::print_msg(fmt);
			continue;
		}

		// ---- 取右邻目标单元格 ----
		tag_t targetCell = NULL_TAG;
		int tcc = UF_TABNOT_ask_cell_at_row_col(row, rightCol, &targetCell);
		if (tcc != 0 || targetCell == NULL_TAG)
		{
			sprintf_s(fmt, sizeof(fmt),
				"  [%s] 右邻格获取失败 rc=%d，安全跳过 (表格tag=%llu %s c%d)",
				stepName, tcc, (unsigned long long)note, rowDesc, ci);
			CommonUtils::print_msg(fmt);
			continue;
		}

		// ---- 读目标格旧值（返回码记录；读取失败按空处理并尝试写入） ----
		std::string targetRaw; int tArc = 0;
		bool tReadOk = ask_cell_text_str(targetCell, targetRaw, &tArc);
		if (!tReadOk)
		{
			char errBuf[133]; errBuf[0] = '\0';
			UF_get_fail_message(tArc, errBuf);
			sprintf_s(fmt, sizeof(fmt),
				"  [%s] 目标格 ask_cell_text 失败 rc=%d (%s)，按空值处理并尝试写入",
				stepName, tArc, errBuf);
			CommonUtils::print_msg(fmt);
			targetRaw.clear();
		}
		bool tConv = false;
		std::string tNorm = normalize_cell_text(targetRaw, tConv);
		if (tConv)
		{
			sprintf_s(fmt, sizeof(fmt), "  [%s] 目标格原文非UTF-8(GBK?)，已转码，hex: %s",
				stepName, hex_bytes(targetRaw).c_str());
			CommonUtils::print_msg(fmt);
		}

		// 求值文本（区分电子表格/表达式驱动单元格）
		std::string tEval; int tErc = 0;
		if (ask_evaluated_cell_text_str(targetCell, tEval, &tErc) && !tEval.empty() && tEval != targetRaw)
		{
			sprintf_s(fmt, sizeof(fmt), "  [%s] 目标格求值文本='%s' (ask_evaluated_cell_text rc=%d，定义文本与求值不同)",
				stepName, escape_text(tEval).c_str(), tErc);
			CommonUtils::print_msg(fmt);
		}

		// 合并信息（诊断）
		if (diagnostic)
		{
			tag_t sr = NULL_TAG, sc = NULL_TAG, er = NULL_TAG, ec = NULL_TAG;
			int mrc = UF_TABNOT_ask_merge_info(targetCell, &sr, &sc, &er, &ec);
			sprintf_s(fmt, sizeof(fmt), "  [%s] 目标格合并信息: rc=%d 起(行%llu,列%llu) 止(行%llu,列%llu)%s",
				stepName, mrc, (unsigned long long)sr, (unsigned long long)sc,
				(unsigned long long)er, (unsigned long long)ec,
				sr != NULL_TAG ? "" : "（未合并）");
			CommonUtils::print_msg(fmt);
		}

		// ---- 写入判定 ----
		std::string firstLine, reason;
		bool writable = judge_writable(tNorm, refreshOldDate, firstLine, reason);
		sprintf_s(fmt, sizeof(fmt),
			"  [%s] 目标右邻格: 表格tag=%llu %s c%d 旧值='%s' 首行='%s' 判定=%s",
			stepName, (unsigned long long)note, rowDesc, ci,
			escape_text(targetRaw).c_str(), firstLine.c_str(), reason.c_str());
		CommonUtils::print_msg(fmt);

		if (!writable)
		{
			++st.skipped;
			sprintf_s(fmt, sizeof(fmt), "  [%s] 跳过: %s", stepName, reason.c_str());
			CommonUtils::print_msg(fmt);
			continue;
		}

		// ---- 写入（编码与读回一致） ----
		std::string enc = encode_for_cell(value);
		int wrc = UF_TABNOT_set_cell_text(targetCell, enc.c_str());
		sprintf_s(fmt, sizeof(fmt),
			"  [%s] set_cell_text rc=%d (表格tag=%llu %s c%d 写入值='%s'%s)",
			stepName, wrc, (unsigned long long)note, rowDesc, ci, value,
			enc != value ? "  [值已按ANSI(GBK)编码]" : "");
		CommonUtils::print_msg(fmt);
		if (wrc != 0)
		{
			++st.failures;
			char errBuf[133]; errBuf[0] = '\0';
			UF_get_fail_message(wrc, errBuf);
			sprintf_s(fmt, sizeof(fmt),
				"  [%s] 写入失败 rc=%d (%s)。该格可能被锁定/由电子表格或属性驱动/权限不足；"
				"定义文本='%s'，求值文本='%s'。继续处理其余格。",
				stepName, wrc, errBuf,
				escape_text(targetRaw).c_str(), escape_text(tEval).c_str());
			CommonUtils::print_msg(fmt);
			continue;
		}
		++st.filled;
		noteModified = true;
		writtenCells.insert(targetCell);
		sprintf_s(fmt, sizeof(fmt), "已填写: %s (表格tag=%llu, %s, c%d)",
			value, (unsigned long long)note, rowDesc, ci);
		CommonUtils::print_msg(fmt);
	}
}

//==============================================================================
// fill_label_cells —— 主路线 UF_TABNOT（覆盖节行与标题行；行级去重防双写）
//==============================================================================
static FillStats fill_label_cells(const std::vector<tag_t>& notes,
	const char* const labels[], size_t nLabels,
	const char* value, bool refreshOldDate,
	const char* stepName, bool diagnostic)
{
	FillStats st;
	char fmt[512];

	sprintf_s(fmt, sizeof(fmt), "[Step8 %s] 主路线: UF_TABNOT，共 %d 个表格注释",
		stepName, (int)notes.size());
	CommonUtils::print_msg(fmt);

	for (size_t ni = 0; ni < notes.size(); ++ni)
	{
		tag_t note = notes[ni];
		bool noteModified = false;
		std::set<tag_t> writtenCells;
		std::set<tag_t> seenRows;

		// ---- 列集合（整表共享） ----
		int nCols = 0;
		int rcC = UF_TABNOT_ask_nm_columns(note, &nCols);
		if (rcC != 0 || nCols <= 0)
		{
			sprintf_s(fmt, sizeof(fmt), "  [%s] 表格tag=%llu ask_nm_columns rc=%d nCols=%d，跳过",
				stepName, (unsigned long long)note, rcC, nCols);
			CommonUtils::print_msg(fmt);
			continue;
		}
		int baseCol = detect_index_base("ask_nth_column", note, UF_TABNOT_ask_nth_column, diagnostic);
		std::vector<tag_t> cols(nCols, NULL_TAG);
		for (int ci = 0; ci < nCols; ++ci)
		{
			int rc = UF_TABNOT_ask_nth_column(note, ci + baseCol, &cols[ci]);
			if ((rc != 0 || cols[ci] == NULL_TAG) && diagnostic)
			{
				sprintf_s(fmt, sizeof(fmt), "  [%s] ask_nth_column(index=%d) rc=%d tag=%llu",
					stepName, ci + baseCol, rc, (unsigned long long)cols[ci]);
				CommonUtils::print_msg(fmt);
			}
		}

		// ---- 节与标题行数量（结构总览） ----
		int nSec = 0;
		int rcS = UF_TABNOT_ask_nm_sections(note, &nSec);
		int nHR = 0;
		int rcH = UF_TABNOT_ask_nm_header_rows(note, &nHR);
		sprintf_s(fmt, sizeof(fmt),
			"  [%s] 表格tag=%llu: 节=%d(rc=%d) 列=%d(rc=%d) 标题行=%d(rc=%d)",
			stepName, (unsigned long long)note, nSec, rcS, nCols, rcC, nHR, rcH);
		CommonUtils::print_msg(fmt);

		// ---- 标题行遍历（标题栏首行常为标题行，不属于任何节） ----
		if (rcH == 0 && nHR > 0)
		{
			int baseHR = detect_index_base("ask_nth_header_row", note, UF_TABNOT_ask_nth_header_row, diagnostic);
			for (int hi = 0; hi < nHR; ++hi)
			{
				tag_t row = NULL_TAG;
				int rc = UF_TABNOT_ask_nth_header_row(note, hi + baseHR, &row);
				if (rc != 0 || row == NULL_TAG)
				{
					sprintf_s(fmt, sizeof(fmt), "  [%s] ask_nth_header_row(index=%d) rc=%d tag=%llu",
						stepName, hi + baseHR, rc, (unsigned long long)row);
					CommonUtils::print_msg(fmt);
					continue;
				}
				if (!seenRows.insert(row).second) continue;
				char rowDesc[32];
				sprintf_s(rowDesc, sizeof(rowDesc), "标题行%d", hi);
				process_row_cells(note, row, rowDesc, cols, labels, nLabels,
					value, refreshOldDate, stepName, diagnostic,
					st, noteModified, writtenCells);
			}
		}

		// ---- 节 → 行遍历 ----
		if (rcS == 0 && nSec > 0)
		{
			int baseSec = detect_index_base("ask_nth_section", note, UF_TABNOT_ask_nth_section, diagnostic);
			for (int si = 0; si < nSec; ++si)
			{
				tag_t section = NULL_TAG;
				int rc = UF_TABNOT_ask_nth_section(note, si + baseSec, &section);
				if (rc != 0 || section == NULL_TAG)
				{
					sprintf_s(fmt, sizeof(fmt), "  [%s] ask_nth_section(index=%d) rc=%d tag=%llu",
						stepName, si + baseSec, rc, (unsigned long long)section);
					CommonUtils::print_msg(fmt);
					continue;
				}

				int nRows = 0;
				rc = UF_TABNOT_ask_nm_rows_in_section(section, &nRows);
				if (rc != 0 || nRows <= 0)
				{
					if (diagnostic)
					{
						sprintf_s(fmt, sizeof(fmt), "  [%s] section[%d] ask_nm_rows_in_section rc=%d rows=%d",
							stepName, si, rc, nRows);
						CommonUtils::print_msg(fmt);
					}
					continue;
				}
				int baseRow = detect_index_base("ask_nth_row_in_section", section,
					UF_TABNOT_ask_nth_row_in_section, diagnostic);

				for (int ri = 0; ri < nRows; ++ri)
				{
					tag_t row = NULL_TAG;
					rc = UF_TABNOT_ask_nth_row_in_section(section, ri + baseRow, &row);
					if (rc != 0 || row == NULL_TAG)
					{
						sprintf_s(fmt, sizeof(fmt), "  [%s] ask_nth_row_in_section(index=%d) rc=%d tag=%llu",
							stepName, ri + baseRow, rc, (unsigned long long)row);
						CommonUtils::print_msg(fmt);
						continue;
					}
					if (!seenRows.insert(row).second) continue;
					char rowDesc[32];
					sprintf_s(rowDesc, sizeof(rowDesc), "节%d行%d", si, ri);
					process_row_cells(note, row, rowDesc, cols, labels, nLabels,
						value, refreshOldDate, stepName, diagnostic,
						st, noteModified, writtenCells);
				}
			}
		}
		else if (diagnostic)
		{
			sprintf_s(fmt, sizeof(fmt), "  [%s] 表格tag=%llu 无可用节（ask_nm_sections rc=%d nSec=%d），仅尝试标题行",
				stepName, (unsigned long long)note, rcS, nSec);
			CommonUtils::print_msg(fmt);
		}

		// 每个写过的表格刷新一次
		if (noteModified)
		{
			int urc = UF_TABNOT_update(note);
			sprintf_s(fmt, sizeof(fmt), "  [%s] UF_TABNOT_update rc=%d (表格tag=%llu)",
				stepName, urc, (unsigned long long)note);
			CommonUtils::print_msg(fmt);
			if (urc != 0)
			{
				char errBuf[133]; errBuf[0] = '\0';
				UF_get_fail_message(urc, errBuf);
				sprintf_s(fmt, sizeof(fmt), "  [%s] 警告: UF_TABNOT_update 失败 (%s)", stepName, errBuf);
				CommonUtils::print_msg(fmt);
			}
		}
	}
	return st;
}

//==============================================================================
// fill_via_titleblock_builder —— 降级路线（NX 原生 TitleBlock）
// 触发条件：主路线扫不到表格注释；或主路线扫到表格但未命中任何标签、
// 且部件存在原生 TitleBlock 对象。
// 实现：CreateEditTitleBlockBuilder → 遍历 Cells()，按标签格 Text/EditableText
// 命中"日期/零件编号"后，经底层表格注释单元格几何（ask_row_of_cell /
// ask_column_of_cell / ask_relative_column）定位同行右邻格；
// 优先 SetCellValueForLabel（右邻格有标签名），无标签名时 SetEditableText；
// Lock 状态、旧值、写入结果全部记录。
//==============================================================================
static FillStats fill_via_titleblock_builder(NXOpen::Part* workPart,
	const char* const labels[], size_t nLabels,
	const char* value, bool refreshOldDate, const char* stepName)
{
	FillStats st;
	char fmt[512];

	NXOpen::Annotations::TitleBlockCollection* tbColl = NULL;
	try { tbColl = workPart->DraftingManager()->TitleBlocks(); }
	catch (...) { tbColl = NULL; }
	if (!tbColl)
	{
		sprintf_s(fmt, sizeof(fmt), "[%s] 降级路线: TitleBlocks() 不可用，无法填写", stepName);
		CommonUtils::print_msg(fmt);
		return st;
	}

	std::vector<NXOpen::Annotations::TitleBlock*> tbList;
	try
	{
		for (NXOpen::Annotations::TitleBlockCollection::iterator it = tbColl->begin();
			it != tbColl->end(); ++it)
		{
			NXOpen::Annotations::TitleBlock* tb = *it;
			if (tb) tbList.push_back(tb);
		}
	}
	catch (...) {}
	if (tbList.empty())
	{
		sprintf_s(fmt, sizeof(fmt), "[%s] 降级路线: TitleBlocks 集合为空，无法填写", stepName);
		CommonUtils::print_msg(fmt);
		return st;
	}

	sprintf_s(fmt, sizeof(fmt), "[%s] 降级路线: 找到 %d 个原生标题块，使用 EditTitleBlockBuilder",
		stepName, (int)tbList.size());
	CommonUtils::print_msg(fmt);

	NXOpen::Annotations::EditTitleBlockBuilder* builder = NULL;
	try { builder = tbColl->CreateEditTitleBlockBuilder(tbList); }
	catch (const NXOpen::NXException& e)
	{
		sprintf_s(fmt, sizeof(fmt), "[%s] 降级路线: CreateEditTitleBlockBuilder 异常: %s", stepName, e.Message());
		CommonUtils::print_msg(fmt);
	}
	catch (...)
	{
		sprintf_s(fmt, sizeof(fmt), "[%s] 降级路线: CreateEditTitleBlockBuilder 未知异常", stepName);
		CommonUtils::print_msg(fmt);
	}
	if (!builder)
	{
		CommonUtils::print_msg("降级路线: CreateEditTitleBlockBuilder 失败");
		return st;
	}

	try
	{
		NXOpen::Annotations::TitleBlockCellBuilderList* cells = builder->Cells();
		int nCells = cells ? cells->Length() : 0;
		sprintf_s(fmt, sizeof(fmt), "[%s] 降级路线: 标题块共 %d 个单元格", stepName, nCells);
		CommonUtils::print_msg(fmt);

		// 建立 底层表格注释单元格tag → TitleBlockCellBuilder 映射
		std::vector<std::pair<tag_t, NXOpen::Annotations::TitleBlockCellBuilder*> > tag2cb;
		for (int i = 0; i < nCells; ++i)
		{
			NXOpen::Annotations::TitleBlockCellBuilder* cb = cells->FindItem(i);
			if (!cb) continue;
			tag_t t = NULL_TAG;
			try
			{
				NXOpen::DisplayableObject* d = cb->Cell();
				if (d) t = d->Tag();
			}
			catch (...) { t = NULL_TAG; }
			tag2cb.push_back(std::make_pair(t, cb));
		}

		for (int i = 0; i < nCells; ++i)
		{
			NXOpen::Annotations::TitleBlockCellBuilder* cb = cells->FindItem(i);
			if (!cb) continue;

			// 读取该格标签名 / 显示文本 / 可编辑文本 / 锁定状态
			std::string cellLabel, text, editable;
			bool locked = false, lockKnown = false;
			try { cellLabel = cb->Label().GetUTF8Text(); } catch (...) {}
			try { text = cb->Text().GetUTF8Text(); } catch (...) {}
			try { editable = cb->EditableText().GetUTF8Text(); } catch (...) {}
			try { locked = cb->Lock(); lockKnown = true; } catch (...) {}

			// 标签格匹配：显示文本（求值）与可编辑文本都试
			std::string hitLine, hitMode;
			bool matched = match_label_multiline(text, labels, nLabels, hitLine, hitMode);
			if (!matched)
				matched = match_label_multiline(editable, labels, nLabels, hitLine, hitMode);
			if (!matched)
			{
				// 单元格标签名（如 DB_DATE/DB_PART_NO）直接等于候选时仅记录，不写入
				// （无法保证是"标签文字右侧"的对应格，遵守只改右邻格约束）
				for (size_t li = 0; li < nLabels; ++li)
				{
					if (!cellLabel.empty() && cellLabel == labels[li])
					{
						sprintf_s(fmt, sizeof(fmt),
							"  [%s] 降级路线: 单元格标签名='%s' 与候选'%s'相同，但按右邻格约束仅记录不写入",
							stepName, cellLabel.c_str(), labels[li]);
						CommonUtils::print_msg(fmt);
					}
				}
				continue;
			}

			++st.hits;
			sprintf_s(fmt, sizeof(fmt),
				"  [%s] 降级路线命中标签格: 命中行='%s'(%s) 显示文本='%s' 可编辑文本='%s' 标签名='%s'",
				stepName, hitLine.c_str(), hitMode.c_str(),
				escape_text(text).c_str(), escape_text(editable).c_str(), cellLabel.c_str());
			CommonUtils::print_msg(fmt);

			// ---- 经底层表格注释单元格几何定位右邻格 ----
			tag_t cellTag = NULL_TAG;
			try
			{
				NXOpen::DisplayableObject* d = cb->Cell();
				if (d) cellTag = d->Tag();
			}
			catch (...) { cellTag = NULL_TAG; }
			if (cellTag == NULL_TAG)
			{
				CommonUtils::print_msg("  [降级路线] 标签格无底层表格注释单元格，跳过");
				continue;
			}
			tag_t rowTag = NULL_TAG, colTag = NULL_TAG;
			int rcRow = UF_TABNOT_ask_row_of_cell(cellTag, &rowTag);
			int rcCol = UF_TABNOT_ask_column_of_cell(cellTag, &colTag);
			sprintf_s(fmt, sizeof(fmt),
				"  [降级路线] ask_row_of_cell rc=%d row=%llu; ask_column_of_cell rc=%d col=%llu",
				rcRow, (unsigned long long)rowTag, rcCol, (unsigned long long)colTag);
			CommonUtils::print_msg(fmt);
			if (rcRow != 0 || rowTag == NULL_TAG || rcCol != 0 || colTag == NULL_TAG)
			{
				CommonUtils::print_msg("  [降级路线] 标签格行列定位失败，跳过");
				continue;
			}
			tag_t rightColTag = NULL_TAG;
			int rcRel = UF_TABNOT_ask_relative_column(colTag, 1, &rightColTag);
			sprintf_s(fmt, sizeof(fmt), "  [降级路线] ask_relative_column(+1) rc=%d rightCol=%llu",
				rcRel, (unsigned long long)rightColTag);
			CommonUtils::print_msg(fmt);
			if (rcRel != 0 || rightColTag == NULL_TAG)
			{
				sprintf_s(fmt, sizeof(fmt), "  [%s] 降级路线: 标签 '%s' 无右邻列（最后一列），安全跳过",
					stepName, hitLine.c_str());
				CommonUtils::print_msg(fmt);
				continue;
			}
			tag_t targetTag = NULL_TAG;
			int rcCell2 = UF_TABNOT_ask_cell_at_row_col(rowTag, rightColTag, &targetTag);
			if (rcCell2 != 0 || targetTag == NULL_TAG)
			{
				sprintf_s(fmt, sizeof(fmt), "  [降级路线] 右邻格 ask_cell_at_row_col rc=%d，跳过", rcCell2);
				CommonUtils::print_msg(fmt);
				continue;
			}

			// ---- 找右邻格对应的 builder 单元 ----
			NXOpen::Annotations::TitleBlockCellBuilder* rightCb = NULL;
			for (size_t k = 0; k < tag2cb.size(); ++k)
				if (tag2cb[k].first != NULL_TAG && tag2cb[k].first == targetTag)
				{ rightCb = tag2cb[k].second; break; }

			// 右邻格旧值：优先 builder 显示文本，无 builder 时退回 UF 读取
			std::string oldVal;
			std::string rightLabel;
			bool rightLocked = false, rightLockKnown = false;
			if (rightCb)
			{
				try { rightLabel = rightCb->Label().GetUTF8Text(); } catch (...) {}
				try { oldVal = rightCb->Text().GetUTF8Text(); } catch (...) {}
				if (oldVal.empty())
				{ try { oldVal = rightCb->EditableText().GetUTF8Text(); } catch (...) {} }
				try { rightLocked = rightCb->Lock(); rightLockKnown = true; } catch (...) {}
			}
			else
			{
				int oRc = 0;
				ask_cell_text_str(targetTag, oldVal, &oRc);
			}

			bool oConv = false;
			std::string oldNorm = normalize_cell_text(oldVal, oConv);
			std::string firstLine, reason;
			bool writable = judge_writable(oldNorm, refreshOldDate, firstLine, reason);
			sprintf_s(fmt, sizeof(fmt),
				"  [%s] 降级路线目标格: 旧值='%s' 首行='%s' 判定=%s 锁定=%s 标签名='%s'",
				stepName, escape_text(oldVal).c_str(), firstLine.c_str(), reason.c_str(),
				rightLockKnown ? (rightLocked ? "是" : "否") : "未知",
				rightLabel.c_str());
			CommonUtils::print_msg(fmt);

			if (!writable)
			{
				++st.skipped;
				continue;
			}
			if (rightLockKnown && rightLocked)
			{
				sprintf_s(fmt, sizeof(fmt), "  [%s] 降级路线: 目标格已锁定，尝试写入（可能失败）", stepName);
				CommonUtils::print_msg(fmt);
			}

			// ---- 写入：优先按单元格标签名，无标签名时改可编辑文本 ----
			try
			{
				if (rightCb)
				{
					if (!rightLabel.empty())
						builder->SetCellValueForLabel(rightLabel, value);
					else
						rightCb->SetEditableText(value);
				}
				else
				{
					std::string enc = encode_for_cell(value);
					int wrc = UF_TABNOT_set_cell_text(targetTag, enc.c_str());
					sprintf_s(fmt, sizeof(fmt), "  [降级路线] UF_TABNOT_set_cell_text rc=%d", wrc);
					CommonUtils::print_msg(fmt);
					if (wrc != 0)
					{
						char errBuf[133]; errBuf[0] = '\0';
						UF_get_fail_message(wrc, errBuf);
						sprintf_s(fmt, sizeof(fmt), "  [降级路线] 写入失败 rc=%d (%s)", wrc, errBuf);
						CommonUtils::print_msg(fmt);
						++st.failures;
						continue;
					}
					tag_t noteTag = NULL_TAG;
					if (UF_TABNOT_ask_tabular_note_of_column(rightColTag, &noteTag) == 0 && noteTag != NULL_TAG)
						UF_TABNOT_update(noteTag);
				}
				++st.filled;
				sprintf_s(fmt, sizeof(fmt), "已填写(降级路线): %s (右邻格标签名='%s')", value,
					rightLabel.empty() ? "<无>" : rightLabel.c_str());
				CommonUtils::print_msg(fmt);
			}
			catch (const NXOpen::NXException& e)
			{
				++st.failures;
				sprintf_s(fmt, sizeof(fmt), "  [%s] 降级路线写入异常: %s", stepName, e.Message());
				CommonUtils::print_msg(fmt);
			}
			catch (...)
			{
				++st.failures;
				sprintf_s(fmt, sizeof(fmt), "  [%s] 降级路线写入未知异常", stepName);
				CommonUtils::print_msg(fmt);
			}
		}

		if (st.filled > 0)
		{
			builder->Commit();
			CommonUtils::print_msg("  [降级路线] Commit 成功");
		}
		else
		{
			CommonUtils::print_msg("  [降级路线] 无写入，不执行 Commit");
		}
	}
	catch (const NXOpen::NXException& e)
	{
		sprintf_s(fmt, sizeof(fmt), "  [%s] 降级路线异常: %s", stepName, e.Message());
		CommonUtils::print_msg(fmt);
	}
	catch (...)
	{
		sprintf_s(fmt, sizeof(fmt), "  [%s] 降级路线未知异常", stepName);
		CommonUtils::print_msg(fmt);
	}
	builder->Destroy();
	return st;
}

//==============================================================================
// fill_all —— 主路线优先；无表格注释，或主路线扫到表格但未命中任何标签
// 且存在原生 TitleBlock 时，自动走/追加降级路线（日志明确打印所走路线）。
//==============================================================================
static FillStats fill_all(NXOpen::Part* workPart, const std::vector<tag_t>& notes,
	bool tbAvail, const char* const labels[], size_t nLabels,
	const char* value, bool refreshOldDate, const char* stepName, bool diagnostic)
{
	if (!notes.empty())
	{
		FillStats st = fill_label_cells(notes, labels, nLabels, value,
			refreshOldDate, stepName, diagnostic);   // 主路线
		if (st.hits == 0 && st.filled == 0 && tbAvail)
		{
			char fmt[512];
			sprintf_s(fmt, sizeof(fmt),
				"[%s] 主路线未命中任何标签，且部件存在原生 TitleBlock → 追加降级路线",
				stepName);
			CommonUtils::print_msg(fmt);
			st.add(fill_via_titleblock_builder(workPart, labels, nLabels, value,
				refreshOldDate, stepName));
		}
		else if (st.hits == 0 && st.filled == 0 && !tbAvail)
		{
			CommonUtils::print_msg(
				"  主路线未命中任何标签，且部件不存在原生 TitleBlock（无法降级）。"
				"请查看下方单元格转储核对标签文字与编码。");
		}
		return st;
	}

	char fmt[512];
	sprintf_s(fmt, sizeof(fmt), "[%s] 未找到表格注释，尝试降级路线 (NXOpen TitleBlocks)", stepName);
	CommonUtils::print_msg(fmt);
	if (!tbAvail)
	{
		CommonUtils::print_msg("  TitleBlocks 集合为空/不可用，两条路线均无法填写");
		return FillStats();
	}
	return fill_via_titleblock_builder(workPart, labels, nLabels, value,
		refreshOldDate, stepName);
}

//==============================================================================
// step1_fill_date —— 生成系统日期字符串（年.月.日，如 2026.08.22）
//==============================================================================
static void make_date_string(char* out, size_t outSize)
{
	time_t now = time(NULL);
	struct tm tmNow;
	localtime_s(&tmNow, &now);
	sprintf_s(out, outSize, "%04d.%02d.%02d",
		tmNow.tm_year + 1900, tmNow.tm_mon + 1, tmNow.tm_mday);
	(void)DATE_FMT_NOTE;   // 配置区格式说明，实际格式由上方 sprintf_s 决定
}

//==============================================================================
// step2_fill_partno —— 取当前图档文件名（不带路径、去 .prt，大小写不敏感）
// FullPath 为空（未保存的新部件）时打印警告并返回 false（不影响日期步骤）。
//==============================================================================
static bool get_part_number(NXOpen::Part* workPart, std::string& partNo)
{
	std::string path;
	try
	{
		NXString fp = workPart->FullPath();
		path = fp.GetUTF8Text();
	}
	catch (const NXOpen::NXException& e)
	{
		CommonUtils::print_msg(std::string("警告: FullPath() 获取失败: ") + e.Message());
		return false;
	}
	catch (...)
	{
		CommonUtils::print_msg("警告: FullPath() 获取失败（未知异常）");
		return false;
	}

	if (path.empty())
	{
		CommonUtils::print_msg("警告: FullPath 为空（部件可能尚未保存），跳过零件编号填写");
		return false;
	}

	size_t pos = path.find_last_of("\\/");
	std::string name = (pos == std::string::npos) ? path : path.substr(pos + 1);

	// 大小写不敏感去 ".prt" 扩展名
	if (name.size() > 4)
	{
		std::string ext = name.substr(name.size() - 4);
		for (size_t i = 0; i < ext.size(); ++i)
			if (ext[i] >= 'A' && ext[i] <= 'Z') ext[i] = (char)(ext[i] - 'A' + 'a');
		if (ext == ".prt")
			name = name.substr(0, name.size() - 4);
	}

	if (name.empty())
	{
		CommonUtils::print_msg("警告: 文件名为空，跳过零件编号填写");
		return false;
	}
	partNo = name;
	return true;
}

//==============================================================================
// do_it —— Step8 入口逻辑
// mode: 1=仅日期 2=仅零件编号 3=两者（ufusr 一键全填走 3）
// diagnostic: true=诊断模式（ufusr/Ctrl+U）：无论是否命中都完整打印环境、
//             枚举统计、表格结构与全部单元格转储、标签候选表十六进制；
//             false=菜单按钮回调：保留全部关键判定日志；若两步均未命中任何
//             标签，自动转储全部表格单元格（一次运行即可定位问题）。
//==============================================================================
static void do_it(int mode, bool diagnostic)
{
	try
	{
		// ===== 环境检查 =====
		NXOpen::Session* session = CommonUtils::get_session();
		NXOpen::Part* workPart = session->Parts()->Work();
		if (!workPart)
		{
			CommonUtils::print_msg("请先打开一个部件！");
			return;
		}
		int numDrawings = 0;
		int rcDraw = UF_DRAW_ask_num_drawings(&numDrawings);
		if (rcDraw == 0 && numDrawings == 0)
		{
			CommonUtils::print_msg("当前部件没有图纸，请先进入制图模块（标题框填写依赖制图环境）");
			return;
		}

		g_cellTextIsAnsi = false;   // 每次运行重新探测编码

		char fmt[512];
		if (diagnostic)
		{
			CommonUtils::print_msg("=============================================================");
			CommonUtils::print_msg("===== Step8 标题框填写 —— 诊断模式 (Ctrl+U 入口) =====");
			CommonUtils::print_msg("=============================================================");
			std::string partName, partPath;
			try { partName = workPart->Name().GetUTF8Text(); } catch (...) {}
			try { partPath = workPart->FullPath().GetUTF8Text(); } catch (...) {}
			sprintf_s(fmt, sizeof(fmt), "扫描部件名: %s", partName.c_str());
			CommonUtils::print_msg(fmt);
			sprintf_s(fmt, sizeof(fmt), "扫描部件路径: %s", partPath.c_str());
			CommonUtils::print_msg(fmt);
			sprintf_s(fmt, sizeof(fmt), "图纸数量: %d (UF_DRAW_ask_num_drawings rc=%d)",
				numDrawings, rcDraw);
			CommonUtils::print_msg(fmt);
		}
		else
		{
			CommonUtils::print_msg("===== Step8 标题框填写（菜单模式） =====");
		}

		session->SetUndoMark(NXOpen::Session::MarkVisibilityVisible, "Step8 TitleBlockFill");

		// ===== 枚举表格注释（一次枚举，两步骤共用） =====
		std::vector<tag_t> notes;
		NoteCensus census;
		CommonUtils::print_msg("----- 枚举表格注释 (UF_OBJ_cycle_objs_in_part, type=165) -----");
		enum_tabular_notes(workPart, notes, diagnostic, census);

		// ===== 原生 TitleBlock 统计（判断标题栏形态 + 降级触发条件） =====
		bool tbAvail = count_title_blocks(workPart) > 0;

		// ===== 诊断转储（仅诊断模式） =====
		if (diagnostic)
		{
			const size_t nD = sizeof(DATE_LABELS) / sizeof(DATE_LABELS[0]);
			dump_label_table_hex(DATE_LABELS, nD, "日期");
			const size_t nP = sizeof(PARTNO_LABELS) / sizeof(PARTNO_LABELS[0]);
			dump_label_table_hex(PARTNO_LABELS, nP, "零件编号");
			dump_all_tables(notes);
		}

		int filledDate = 0, filledPartNo = 0, skipped = 0, failures = 0, totalHits = 0;

		// ===== 步骤1：日期 =====
		if (mode & 1)
		{
			CommonUtils::print_msg("===== Step8 步骤1: 填写标题框日期 =====");
			CommonUtils::print_msg("候选标签: 日期 / Date / DATE / date");
			char dateStr[32];
			make_date_string(dateStr, sizeof(dateStr));
			sprintf_s(fmt, sizeof(fmt), "当前日期: %s", dateStr);
			CommonUtils::print_msg(fmt);

			const size_t nLabels = sizeof(DATE_LABELS) / sizeof(DATE_LABELS[0]);
			FillStats st = fill_all(workPart, notes, tbAvail, DATE_LABELS, nLabels, dateStr,
				true,   /* 日期模式：允许刷新旧日期 */
				"步骤1(日期)", diagnostic);
			filledDate = st.filled;
			skipped += st.skipped;
			failures += st.failures;
			totalHits += st.hits;
			sprintf_s(fmt, sizeof(fmt), "----- 步骤1 结果: 命中标签 %d 处，填写 %d 处，跳过 %d 处，失败 %d 处 -----",
				st.hits, st.filled, st.skipped, st.failures);
			CommonUtils::print_msg(fmt);
		}

		// ===== 步骤2：零件编号 =====
		if (mode & 2)
		{
			CommonUtils::print_msg("===== Step8 步骤2: 填写标题框零件编号 =====");
			CommonUtils::print_msg("候选标签: 零件编号");
			std::string partNo;
			if (get_part_number(workPart, partNo))
			{
				sprintf_s(fmt, sizeof(fmt), "零件编号(文件名): %s", partNo.c_str());
				CommonUtils::print_msg(fmt);

				const size_t nLabels = sizeof(PARTNO_LABELS) / sizeof(PARTNO_LABELS[0]);
				FillStats st = fill_all(workPart, notes, tbAvail, PARTNO_LABELS, nLabels, partNo.c_str(),
					false,   /* 零件编号：不覆盖任何已有非占位符内容 */
					"步骤2(零件编号)", diagnostic);
				filledPartNo = st.filled;
				skipped += st.skipped;
				failures += st.failures;
				totalHits += st.hits;
				sprintf_s(fmt, sizeof(fmt), "----- 步骤2 结果: 命中标签 %d 处，填写 %d 处，跳过 %d 处，失败 %d 处 -----",
					st.hits, st.filled, st.skipped, st.failures);
				CommonUtils::print_msg(fmt);
			}
		}

		// ===== 汇总 =====
		sprintf_s(fmt, sizeof(fmt),
			"===== 完成，共填写 %d 处日期，%d 处零件编号；命中标签 %d 处，跳过 %d 处，失败 %d 处 =====",
			filledDate, filledPartNo, totalHits, skipped, failures);
		CommonUtils::print_msg(fmt);

		// 菜单模式且完全未命中 → 自动转储单元格（一次运行即可拿到诊断数据）
		if (!diagnostic && totalHits == 0 && !notes.empty())
		{
			CommonUtils::print_msg("  [自动诊断] 两步均未命中任何标签，自动转储全部表格单元格（含十六进制）：");
			dump_all_tables(notes);
			CommonUtils::print_msg(
				"  [自动诊断] 请将以上 ListingWindow 完整输出（从 [枚举结果] 到转储结束）反馈，"
				"以便核对标签文字、编码与行列结构。");
		}
		if (diagnostic)
		{
			CommonUtils::print_msg("[诊断模式] 如未填写成功，请将以上 ListingWindow 完整输出反馈。");
		}

		// 刷新视图，确保写入即时可见
		CommonUtils::silent_update(session);
	}
	catch (const NXOpen::NXException& e) { CommonUtils::print_msg(std::string("NXException: ") + e.Message()); }
	catch (const std::exception& e) { CommonUtils::print_msg(std::string("Exception: ") + e.what()); }
	catch (...) { CommonUtils::print_msg("Unknown Exception"); }
}

//==============================================================================
// 菜单应用类（仿官方 MenuBarCppApp 范式：ufsta 入口 + RegisterApplication
// + AddMenuAction + make_callback；ufusr_ask_unload 必须 AtTermination）
//==============================================================================
class TitleFillMenuBarApp
{
public:
	TitleFillMenuBarApp();

private:
	static NXOpen::Session* theSession;
	static NXOpen::UI* theUI;
	static int registered;   // 防重复注册标志

	void InitializeCallbacks();

	// RegisterApplication 的 init/enter/exit 回调（本应用无状态，直接返回 0）
	int AppInitCB();
	int AppEnterCB();
	int AppExitCB();

	// 三个菜单动作回调
	NXOpen::MenuBar::MenuBarManager::CallbackStatus FillDateCB(NXOpen::MenuBar::MenuButtonEvent*);
	NXOpen::MenuBar::MenuBarManager::CallbackStatus FillPartNoCB(NXOpen::MenuBar::MenuButtonEvent*);
	NXOpen::MenuBar::MenuBarManager::CallbackStatus FillBothCB(NXOpen::MenuBar::MenuButtonEvent*);

	// 回调公共执行体：异常仅 print_msg 不抛出（菜单回调来自 UI 线程事件）
	void RunMode(int mode, const char* actionName);
};

NXOpen::Session* TitleFillMenuBarApp::theSession = NULL;
NXOpen::UI* TitleFillMenuBarApp::theUI = NULL;
int TitleFillMenuBarApp::registered = 0;

TitleFillMenuBarApp::TitleFillMenuBarApp()
{
	try
	{
		theSession = NXOpen::Session::GetSession();
		theUI = NXOpen::UI::GetUI();
		InitializeCallbacks();
	}
	catch (const NXOpen::NXException& ex)
	{
		std::cerr << "TitleFillMenuBarApp 构造异常: " << ex.Message() << std::endl;
	}
	catch (...)
	{
		std::cerr << "TitleFillMenuBarApp 构造未知异常" << std::endl;
	}
}

void TitleFillMenuBarApp::InitializeCallbacks()
{
	if (registered != 0) return;   // 防重复注册

	// 注：官方 MenuBarCppApp 示例在构造函数里做 UF_initialize，其菜单回调内
	//     再 UF_initialize/UF_terminate 配对；本工程（Step7/球标等）已验证在
	//     NX 内部模式下直接调用 UF_* 无需 UF_initialize，故此处沿用直接调用
	//     惯例，不做 UF_initialize/UgTerminate 包装。
	theUI->MenuBarManager()->RegisterApplication("TITLEFILL_APP",
		NXOpen::make_callback(this, &TitleFillMenuBarApp::AppInitCB),
		NXOpen::make_callback(this, &TitleFillMenuBarApp::AppEnterCB),
		NXOpen::make_callback(this, &TitleFillMenuBarApp::AppExitCB),
		true,   // supportsDrawings（本应用服务于制图标题框，必须支持图纸）
		true,   // supportsDesignInContext
		true);  // supportsUndo（do_it 内已打可见 Undo Mark）

	theUI->MenuBarManager()->AddMenuAction("TITLEFILL_APP__fill_date",
		NXOpen::make_callback(this, &TitleFillMenuBarApp::FillDateCB));
	theUI->MenuBarManager()->AddMenuAction("TITLEFILL_APP__fill_partno",
		NXOpen::make_callback(this, &TitleFillMenuBarApp::FillPartNoCB));
	theUI->MenuBarManager()->AddMenuAction("TITLEFILL_APP__fill_both",
		NXOpen::make_callback(this, &TitleFillMenuBarApp::FillBothCB));

	registered = 1;
}

int TitleFillMenuBarApp::AppInitCB()  { return 0; }
int TitleFillMenuBarApp::AppEnterCB() { return 0; }
int TitleFillMenuBarApp::AppExitCB()  { return 0; }

void TitleFillMenuBarApp::RunMode(int mode, const char* actionName)
{
	try
	{
		CommonUtils::print_msg(std::string("[Step8] 菜单动作: ") + actionName);
		do_it(mode, false);   // 菜单按钮：简洁输出（含关键判定日志 + 未命中自动转储）
	}
	catch (const NXOpen::NXException& e)
	{
		CommonUtils::print_msg(std::string("[Step8] 菜单动作异常: ") + e.Message());
	}
	catch (const std::exception& e)
	{
		CommonUtils::print_msg(std::string("[Step8] 菜单动作异常: ") + e.what());
	}
	catch (...)
	{
		CommonUtils::print_msg("[Step8] 菜单动作未知异常");
	}
}

NXOpen::MenuBar::MenuBarManager::CallbackStatus
TitleFillMenuBarApp::FillDateCB(NXOpen::MenuBar::MenuButtonEvent*)
{
	RunMode(1, "fill_date(仅日期)");
	return NXOpen::MenuBar::MenuBarManager::CallbackStatusContinue;
}

NXOpen::MenuBar::MenuBarManager::CallbackStatus
TitleFillMenuBarApp::FillPartNoCB(NXOpen::MenuBar::MenuButtonEvent*)
{
	RunMode(2, "fill_partno(仅零件编号)");
	return NXOpen::MenuBar::MenuBarManager::CallbackStatusContinue;
}

NXOpen::MenuBar::MenuBarManager::CallbackStatus
TitleFillMenuBarApp::FillBothCB(NXOpen::MenuBar::MenuButtonEvent*)
{
	RunMode(3, "fill_both(日期+零件编号)");
	return NXOpen::MenuBar::MenuBarManager::CallbackStatusContinue;
}

//==============================================================================
// ufusr —— DLL 入口点（Ctrl+U 直接运行：一键全填 + 完整诊断转储；
// 三层异常保护，仿 Step7）
//==============================================================================
extern "C" DllExport void ufusr(char *parm, int *returnCode, int rlen)
{
	try
	{
		do_it(3, true);   // 一键全填 + 诊断模式（完整转储，便于排障）
	}
	catch (const NXOpen::NXException& e1)
	{
		NXOpen::UI::GetUI()->NXMessageBox()->Show("NXException", NXOpen::NXMessageBox::DialogTypeError, e1.Message());
	}
	catch (const std::exception& e2)
	{
		NXOpen::UI::GetUI()->NXMessageBox()->Show("Exception", NXOpen::NXMessageBox::DialogTypeError, e2.what());
	}
	catch (...)
	{
		NXOpen::UI::GetUI()->NXMessageBox()->Show("Exception", NXOpen::NXMessageBox::DialogTypeError, "Unknown Exception.");
	}
}

//==============================================================================
// ufsta —— 菜单应用入口（startup 目录 .men 文件加载时由 NX 调用）
//==============================================================================
static TitleFillMenuBarApp* theTitleFillApp = NULL;

extern "C" DllExport void ufsta(char *param, int *retcod, int param_len)
{
	theTitleFillApp = new TitleFillMenuBarApp();
}

//------------------------------------------------------------------------------
// Unload Handler
// 注册了菜单回调（RegisterApplication/AddMenuAction）的 DLL 必须返回
// AtTermination：回调指针指向本 DLL 代码段，若像 Step7 等纯 ufusr 工具那样
// 返回 Immediately（执行完即卸载），后续点击菜单将跳转到已卸载的代码导致
// NX 崩溃。故本模块固定 AtTermination，会话结束时才卸载。
//------------------------------------------------------------------------------
extern "C" DllExport int ufusr_ask_unload()
{
	return (int)NXOpen::Session::LibraryUnloadOptionAtTermination;
}
