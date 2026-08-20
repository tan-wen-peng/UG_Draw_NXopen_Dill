//------------------------------------------------------------------------------
// NX12 Step6：工序图层切换（DLL 6/6）
// 功能：运行时对话框指定工序→图层映射，切换图层显隐状态或移动对象
//------------------------------------------------------------------------------

// Win32 防护（本模块需要 windows.h 用于对话框）
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <stdlib.h>
#ifdef CreateDialog
#undef CreateDialog
#endif

#include "../Shared/NX12_CommonConfig.h"
#include "../Shared/NX12_CommonUtils.h"
// Step6 专有 includes
#include <NXOpen/NXMessageBox.hxx>
#include <NXOpen/Selection.hxx>
#include <NXOpen/DisplayableObject.hxx>

//==============================================================================
// 工序图层映射对话框（Win32 内存模板，无需 .dlx 模板文件）
// 界面：顶部下拉框选择"本次运行的工序"，下方逐行编辑 工序 i -> 图层。
// 确定后插件将用户在视图内选中的对象移动到所选工序对应的图层。
//（NX12 中视图继承自 NXObject 而非 DisplayableObject，视图本身无法换图层，
//  因此换图层对象由用户在阶段7交互选择）
//==============================================================================
namespace LayerDlg
{
	const int IDC_PROCESS_COMBO = 1000;
	const int IDC_LAYER_BASE    = 1100;   // 工序 i 的编辑框 ID = IDC_LAYER_BASE + i
	const int IDC_MODE_MOVE     = 1200;   // 模式A 单选：移动对象到工序图层
	const int IDC_MODE_STATE    = 1201;   // 模式B 单选：切换工序图层显隐状态

	struct Result
	{
		int currentProcess;               // 0-based
		int layers[MAX_PROCESS_COUNT];
		int mode;                         // 0=模式A(移动对象) 1=模式B(切换图层显隐)
	};
	static Result g_result;
	static int g_processCount;
	static int g_defaultLayers[MAX_PROCESS_COUNT];

	struct Buf
	{
		BYTE d[8192];
		size_t n;
		Buf() : n(0) { memset(d, 0, sizeof(d)); }
		void W(WORD v)      { memcpy(d + n, &v, 2); n += 2; }
		void DW(DWORD v)    { memcpy(d + n, &v, 4); n += 4; }
		void Str(const WCHAR* s) { size_t L = wcslen(s) + 1; memcpy(d + n, s, L * 2); n += L * 2; }
		void Align()        { n = (n + 3) & ~(size_t)3; }
	};

	// 写入一个控件（类名用原子序号）
	void AddItem(Buf& b, DWORD style, short x, short y, short cx, short cy,
		WORD id, WORD clsOrd, const WCHAR* title)
	{
		b.Align();
		b.DW(style);                    // style
		b.DW(0);                        // dwExtendedStyle
		b.W((WORD)x); b.W((WORD)y); b.W((WORD)cx); b.W((WORD)cy);
		b.W(id);
		b.W(0xFFFF); b.W(clsOrd);       // class: atom
		if (title && title[0]) b.Str(title);
		else b.W(0);
		b.W(0);                         // creation data 长度
	}

	INT_PTR CALLBACK DlgProc(HWND h, UINT m, WPARAM w, LPARAM l)
	{
		switch (m)
		{
		case WM_INITDIALOG:
		{
			HWND cb = GetDlgItem(h, IDC_PROCESS_COMBO);
			WCHAR buf[16];
			for (int i = 0; i < g_processCount; ++i)
			{
				wsprintfW(buf, L"工序 %d", i + 1);
				SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)buf);
			}
			SendMessageW(cb, CB_SETCURSEL, 0, 0);
			for (int i = 0; i < g_processCount; ++i)
			{
				wsprintfW(buf, L"%d", g_defaultLayers[i]);
				SetWindowTextW(GetDlgItem(h, IDC_LAYER_BASE + i), buf);
			}
			// 默认选中模式A（移动对象到工序图层）
			CheckDlgButton(h, IDC_MODE_MOVE, BST_CHECKED);
			CheckDlgButton(h, IDC_MODE_STATE, BST_UNCHECKED);
			return TRUE;
		}
		case WM_COMMAND:
			switch (LOWORD(w))
			{
			case IDOK:
			{
				HWND cb = GetDlgItem(h, IDC_PROCESS_COMBO);
				int sel = (int)SendMessageW(cb, CB_GETCURSEL, 0, 0);
				if (sel < 0) sel = 0;
				g_result.currentProcess = sel;
				// 读取模式单选状态：0=模式A（移动对象） 1=模式B（切换显隐）
				g_result.mode =
					(IsDlgButtonChecked(h, IDC_MODE_STATE) == BST_CHECKED) ? 1 : 0;
				bool ok = true;
				WCHAR t[32];
				for (int i = 0; i < g_processCount; ++i)
				{
					GetWindowTextW(GetDlgItem(h, IDC_LAYER_BASE + i), t, 32);
					int v = _wtoi(t);
					if (v < 1 || v > 256) ok = false;
					g_result.layers[i] = v;
				}
				if (!ok)
				{
					MessageBoxW(h, L"图层号必须是 1~256 的整数",
						L"工序图层设置", MB_ICONWARNING | MB_OK);
					return TRUE;
				}
				EndDialog(h, 1);
				return TRUE;
			}
			case IDCANCEL:
				EndDialog(h, 0);
				return TRUE;
			}
			break;
		case WM_CLOSE:
			EndDialog(h, 0);
			return TRUE;
		}
		return FALSE;
	}

	// 返回 true=用户确定，结果写入 out
	bool Show(int count, const int* defLayers, Result& out)
	{
		g_processCount = count;
		for (int i = 0; i < count; ++i) g_defaultLayers[i] = defLayers[i];

		Buf b;
		// DLGTEMPLATE
		b.DW(WS_POPUP | WS_VISIBLE | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME);
		b.DW(0);
		// cdit 控件总数：标签+下拉框 + 模式单选x2 + 每工序(静态文本+编辑框) + 确定/取消
		b.W((WORD)(6 + count * 2));
		b.W(0); b.W(0); b.W(250); b.W((short)(58 + count * 15));
		b.W(0);                         // 无菜单
		b.W(0);                         // 默认类
		b.Str(L"工序图层设置");

		// 标签：本次运行工序
		AddItem(b, WS_CHILD | WS_VISIBLE | SS_LEFT,
			8, 8, 60, 10, 9000, 0x0082, L"本次工序:");
		// 下拉框：选择本次运行对应的工序
		AddItem(b, WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
			74, 6, 80, 80, IDC_PROCESS_COMBO, 0x0085, L"");

		// 模式选择单选按钮组（BS_AUTORADIOBUTTON 自动互斥勾选；
		// 首个按钮带 WS_GROUP 划定分组，第二/三个控件起 Tab 键切换范围正确）
		AddItem(b, WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_GROUP | BS_AUTORADIOBUTTON,
			8, 24, 112, 10, IDC_MODE_MOVE, 0x0080, L"移动对象到工序图层");
		AddItem(b, WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON,
			128, 24, 118, 10, IDC_MODE_STATE, 0x0080, L"切换工序图层显隐状态");

		short y = 40;
		WCHAR lbl[32];
		for (int i = 0; i < count; ++i)
		{
			wsprintfW(lbl, L"工序 %d  ->  图层", i + 1);
			AddItem(b, WS_CHILD | WS_VISIBLE | SS_LEFT,
				8, y, 90, 10, (WORD)(9100 + i), 0x0082, lbl);
			AddItem(b, WS_CHILD | WS_VISIBLE | WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL | ES_NUMBER,
				102, y - 2, 40, 12, (WORD)(IDC_LAYER_BASE + i), 0x0081, L"");
			y += 15;
		}

		AddItem(b, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
			30, y, 40, 14, IDOK, 0x0080, L"确定");
		AddItem(b, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
			80, y, 40, 14, IDCANCEL, 0x0080, L"取消");

		HWND parent = GetActiveWindow();
		INT_PTR r = DialogBoxIndirectW(NULL, (LPCDLGTEMPLATEW)(void*)b.d, parent, DlgProc);
		if (r == 1) { out = g_result; return true; }
		return false;
	}
}

//------------------------------------------------------------------------------
// 工序图层切换（双模式）
// 弹出对话框让用户指定"第几道工序对应哪个图层"（不硬编码），并选择模式：
// 模式A（默认）：确定后交互选择要换图层的对象（可在视图内多选，MB2/确定结束），
//   再用 LayerManager::MoveDisplayableObjects 移到所选工序对应的图层。
// 模式B：用 LayerManager::ChangeStates 切换图层显隐状态——当前工序图层
//   设为 StateSelectable，其余工序图层设为 StateHidden（不交互选对象）。
// 说明：NX12 中视图不是 DisplayableObject，无法直接换图层，
// 所以换图层对象由用户选择（新建的中心线/注释/曲线等均可选）。
//------------------------------------------------------------------------------
void phase_apply_layers(NXOpen::Part* part, const ShellDrawingConfig& cfg)
{
	LayerDlg::Result res;
	if (!LayerDlg::Show(cfg.processCount, cfg.defaultProcessLayers, res))
	{
		CommonUtils::print_msg("[Step6] 已跳过换图层（用户取消对话框）");
		return;
	}

	// 回显全部映射，便于核对
	for (int i = 0; i < cfg.processCount; ++i)
	{
		CommonUtils::print_msg(string("  - 工序 ") + std::to_string(i + 1) +
			" -> 图层 " + std::to_string(res.layers[i]));
	}

	int proc = res.currentProcess;
	int layer = res.layers[proc];
	if (layer < 1 || layer > 256)
	{
		CommonUtils::print_msg("[Step6] 已跳过换图层（所选工序图层号无效）");
		return;
	}

	// ---------- 模式B：切换工序图层显隐状态（不选对象） ----------
	// 当前所选工序的图层 -> StateSelectable（可见可选），
	// 其余工序的图层   -> StateHidden（隐藏）；
	// 与录制 111r_vb.vb 的工序切换语义一致。
	if (res.mode == 1)
	{
		try
		{
			int workLayer = part->Layers()->WorkLayer();

			std::vector<NXOpen::Layer::StateInfo> states;
			// 当前工序图层先入队（若多个工序映射到同一图层，以当前工序为准）；
			// 工作图层不能设为 Selectable/Hidden，保持 StateWorkLayer
			states.push_back(NXOpen::Layer::StateInfo(layer,
				(layer == workLayer) ? NXOpen::Layer::StateWorkLayer
									   : NXOpen::Layer::StateSelectable));

			for (int i = 0; i < cfg.processCount; ++i)
			{
				if (i == proc) continue;
				int ly = res.layers[i];
				if (ly < 1 || ly > 256) continue;   // 沿用图层号有效性校验
				bool dup = false;
				for (size_t k = 0; k < states.size(); ++k)
				{
					if (states[k].Layer == ly) { dup = true; break; }
				}
				if (dup) continue;
				states.push_back(NXOpen::Layer::StateInfo(ly,
					(ly == workLayer) ? NXOpen::Layer::StateWorkLayer
										: NXOpen::Layer::StateHidden));
			}

			part->Layers()->ChangeStates(states, true);

			// 回显每个图层切换后的实际状态
			for (size_t k = 0; k < states.size(); ++k)
			{
				NXOpen::Layer::State st = part->Layers()->GetState(states[k].Layer);
				const char* stName =
					(st == NXOpen::Layer::StateWorkLayer)  ? "工作(Work)"     :
					(st == NXOpen::Layer::StateSelectable) ? "可选(Selectable)" :
					(st == NXOpen::Layer::StateVisible)    ? "可见(Visible)"   :
																"隐藏(Hidden)";
				CommonUtils::print_msg(string("  - 图层 ") + std::to_string(states[k].Layer) +
					" 新状态: " + stName);
			}
			CommonUtils::print_msg(string("[Step6] 已切换工序图层显隐状态（工序 ") +
				std::to_string(proc + 1) + " 的图层 " + std::to_string(layer) + " 可见可选）");
		}
		catch (const NXOpen::NXException& e)
		{
			CommonUtils::print_msg(string("  警告: 切换图层状态失败: ") + e.Message());
		}
		return;
	}

	// ---------- 模式A：交互选择对象移动到所选工序对应图层 ----------
	try
	{
		// 允许在任意视图内选择（含成员视图内的对象）
		int oldCursorView = 1;
		UF_UI_ask_cursor_view(&oldCursorView);
		UF_UI_set_cursor_view(0);

		std::vector<NXOpen::NXObject*> picked;
		NXOpen::Selection::Response rsp = CommonUtils::get_ui()->SelectionManager()->SelectObjects(
			"选择要换图层的对象（可多选，确定/MB2 结束）", "工序换图层",
			NXOpen::Selection::SelectionScopeWorkPart, false, true, picked);

		UF_UI_set_cursor_view(oldCursorView);

		if (rsp != NXOpen::Selection::ResponseOk &&
			rsp != NXOpen::Selection::ResponseObjectSelected &&
			rsp != NXOpen::Selection::ResponseBack)
		{
			CommonUtils::print_msg("[Step6] 已跳过换图层（未选择对象）");
			return;
		}

		// 只保留可显示对象（MoveDisplayableObjects 的参数类型要求）
		std::vector<NXOpen::DisplayableObject*> objs;
		for (size_t i = 0; i < picked.size(); ++i)
		{
			NXOpen::DisplayableObject* d =
				dynamic_cast<NXOpen::DisplayableObject*>(picked[i]);
			if (d) objs.push_back(d);
		}
		if (objs.empty())
		{
			CommonUtils::print_msg("[Step6] 已跳过换图层（所选对象中没有可换图层的对象）");
			return;
		}

		part->Layers()->MoveDisplayableObjects(layer, objs);
		CommonUtils::print_msg(string("[Step6] 已将 ") + std::to_string((int)objs.size()) +
			" 个对象移动到图层 " + std::to_string(layer) +
			"（工序 " + std::to_string(proc + 1) + "）");
	}
	catch (const NXOpen::NXException& e)
	{
		CommonUtils::print_msg(string("  警告: 换图层失败: ") + e.Message());
	}
}

//==============================================================================
// Step6 配置区
//==============================================================================
static const int kProcessCount = 4;
static const int kDefaultLayers[MAX_PROCESS_COUNT] = { 21, 22, 23, 24 };

//==============================================================================
// do_it：入口逻辑
//==============================================================================
void do_it()
{
	try
	{
		NXOpen::Part* part = dynamic_cast<NXOpen::Part*>(CommonUtils::get_session()->Parts()->BaseWork());
		if (!part) { CommonUtils::print_msg("错误：无工作部件"); return; }

		ShellDrawingConfig cfg;
		cfg.processCount = kProcessCount;
		for (int i = 0; i < kProcessCount; ++i) cfg.defaultProcessLayers[i] = kDefaultLayers[i];

		phase_apply_layers(part, cfg);
		CommonUtils::print_msg("========== Step6 工序图层切换 完成 ==========");
	}
	catch (const NXOpen::NXException& e) { CommonUtils::print_msg(std::string("NXException: ") + e.Message()); }
	catch (...) { CommonUtils::print_msg("Unknown Exception"); }
}

//==============================================================================
// DLL 入口
//==============================================================================
extern "C" DllExport void ufusr(char* param, int* retcod, int param_len)
{
	try
	{
		do_it();
	}
	catch (const NXOpen::NXException& e)
	{
		NXOpen::UI::GetUI()->NXMessageBox()->Show("NXException", NXOpen::NXMessageBox::DialogTypeError, e.Message());
	}
	catch (const std::exception& e)
	{
		NXOpen::UI::GetUI()->NXMessageBox()->Show("Exception", NXOpen::NXMessageBox::DialogTypeError, e.what());
	}
	catch (...)
	{
		NXOpen::UI::GetUI()->NXMessageBox()->Show("Exception", NXOpen::NXMessageBox::DialogTypeError, "Unknown Exception.");
	}
}

extern "C" DllExport int ufusr_ask_unload()
{
	return (int)NXOpen::Session::LibraryUnloadOptionImmediately;
}
