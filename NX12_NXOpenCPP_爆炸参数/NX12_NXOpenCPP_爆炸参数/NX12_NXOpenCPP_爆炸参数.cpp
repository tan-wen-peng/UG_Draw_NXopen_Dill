//------------------------------------------------------------------------------
// NX12_NXOpenCPP_爆炸参数  --  Step 2: 爆炸参数提取
//
// 参照提取工具脚本 d:\A_UG\05_爆炸图\提取爆炸参数.vb 的逻辑实现:
//   手动创建的爆炸位移数据不会被录制器记录, 本 dll 从当前工作部件的
//   已存在爆炸图中提取每个已爆炸组件的爆炸位移, 生成参数文件,
//   供 step1 dll(explosion_step1.dll) 自动化重放手动移动过程.
//
// 提取流程(与 VB 一致):
//   1) 校验当前部件为装配, 用 UF_ASSEM_ask_explosions 列出所有爆炸图;
//   2) 对每个爆炸图: 递归收集所有叶子组件(无子组件的组件),
//      逐一用 UF_ASSEM_ask_comp_explosion 查询爆炸状态与 4x4 位移矩阵,
//      rc==0 且状态为 UF_ASSEM_exploded 的组件提取显示名与
//      dx=transform[0][3], dy=transform[1][3], dz=transform[2][3];
//   3) 汇总所有爆炸图的条目, 写出参数文件:
//        E:\UG\nx_open_dll\explosion_params.txt
//      每行格式: 组件显示名|dx|dy|dz (dx/dy/dz 为爆炸位移, 单位与装配一致)
//
// 与 VB 一致的错误处理:
//   - 当前部件不是装配 → 打印错误并返回
//   - 没有爆炸图 → 提示先打开包含手动爆炸数据的部件并返回
//   - 单个组件查询失败(rc!=0)或未爆炸 → 跳过
//
// 与 VB 的差异(修正):
//   - 参数文件写为 UTF-8 无 BOM(VB 的 .NET File.WriteAllLines 使用
//     Encoding.UTF8 会写出 BOM, 而 step1 用 std::ifstream 逐行读取时
//     不会剥离 BOM, 首行组件显示名会被 BOM 前缀污染导致匹配失败),
//     行尾 CRLF 与 VB 一致.
//------------------------------------------------------------------------------

// Mandatory UF Includes
#include <uf.h>
#include <uf_assem.h>
#include <uf_obj.h>

// Internal Includes
#include <NXOpen/ListingWindow.hxx>
#include <NXOpen/NXMessageBox.hxx>
#include <NXOpen/UI.hxx>

// Internal+External Includes
#include <NXOpen/Assemblies_Component.hxx>
#include <NXOpen/Assemblies_ComponentAssembly.hxx>
#include <NXOpen/NXException.hxx>
#include <NXOpen/NXObject.hxx>
#include <NXOpen/NXString.hxx>
#include <NXOpen/Part.hxx>
#include <NXOpen/PartCollection.hxx>
#include <NXOpen/Session.hxx>

// Std C++ Includes
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

// Windows API (CreateDirectoryA/GetFileAttributesA)
#include <windows.h>

using namespace NXOpen;
using std::string;
using std::exception;

//------------------------------------------------------------------------------
// 全局打印: 输出到 NX 信息窗口
//------------------------------------------------------------------------------
static void print(const char *msg)
{
    NXOpen::ListingWindow *lw = NXOpen::Session::GetSession()->ListingWindow();
    if (!lw->IsOpen()) lw->Open();
    lw->WriteLine(msg);
}

//------------------------------------------------------------------------------
// 递归收集装配中的所有叶子组件(无子组件的组件), 与 VB CollectLeaves 一致
//------------------------------------------------------------------------------
static void collectLeaves(NXOpen::Assemblies::Component *comp,
                          std::vector<NXOpen::Assemblies::Component *> &out)
{
    std::vector<NXOpen::Assemblies::Component *> children = comp->GetChildren();
    for (size_t i = 0; i < children.size(); i++)
    {
        if (children[i]->GetChildren().empty())
            out.push_back(children[i]);
        else
            collectLeaves(children[i], out);
    }
}

//------------------------------------------------------------------------------
// 提取当前部件所有爆炸图的爆炸参数并写出参数文件
//------------------------------------------------------------------------------
static void extractExplosionParams()
{
    Session *theSession = Session::GetSession();
    Part *workPart = theSession->Parts()->Work();

    // VB: If workPart.ComponentAssembly() Is Nothing → 错误返回
    if (workPart->ComponentAssembly() == NULL)
    {
        print("Error: current part is not an assembly.");
        return;
    }

    // 列出当前部件的所有爆炸图 (VB: ufs.Assem.AskExplosions)
    int numExpl = 0;
    tag_t *explTags = NULL;
    int rc1 = UF_ASSEM_ask_explosions(workPart->Tag(), &numExpl, &explTags);
    {
        char buf[128];
        sprintf(buf, "Explosion count in current part: %d (rc=%d)", numExpl, rc1);
        print(buf);
    }

    if (numExpl <= 0)
    {
        if (explTags != NULL)
            UF_free(explTags);
        print("No explosion found. Open a part containing manual explosion data first.");
        return;
    }

    // 汇总所有爆炸图的条目(与 VB 一致: 多个爆炸图的条目全部累加进文件)
    std::vector<std::string> lines;

    for (int k = 0; k < numExpl; k++)
    {
        tag_t explTag = explTags[k];

        // VB: ufs.Obj.AskName(explTag, explName)
        char explName[133] = {0};
        UF_OBJ_ask_name(explTag, explName);
        print("");
        {
            std::string head = "===== Explosion: ";
            head += explName;
            head += " =====";
            print(head.c_str());
        }

        // 收集所有叶子组件 (VB: CollectLeaves(rootComponent, leaves))
        NXOpen::Assemblies::Component *rootComp =
            workPart->ComponentAssembly()->RootComponent();
        std::vector<NXOpen::Assemblies::Component *> leaves;
        collectLeaves(rootComp, leaves);
        {
            char buf[128];
            sprintf(buf, "Leaf component count: %d", (int)leaves.size());
            print(buf);
        }

        // 逐一查询爆炸状态与位移, 提取已爆炸组件的显示名与 dx/dy/dz
        // (VB: ufs.Assem.AskCompExplosion; rc=0 且 status=Exploded 才提取)
        int explodedCount = 0;
        for (size_t i = 0; i < leaves.size(); i++)
        {
            UF_ASSEM_expl_status_t status = UF_ASSEM_unexploded;
            double tx[4][4];
            int rc = UF_ASSEM_ask_comp_explosion(explTag, leaves[i]->Tag(), &status, tx);
            if (rc == 0 && status == UF_ASSEM_exploded)
            {
                explodedCount++;
                double dx = tx[0][3];
                double dy = tx[1][3];
                double dz = tx[2][3];
                std::string dispName = leaves[i]->DisplayName().GetText();

                // 信息窗打印 F6 精度(与 VB ToString("F6") 一致)
                {
                    char buf[512];
                    sprintf(buf, "%s | dx=%.6f dy=%.6f dz=%.6f",
                            dispName.c_str(), dx, dy, dz);
                    print(buf);
                }
                // 参数文件条目 F9 精度(与 VB ToString("F9") 一致)
                char buf[512];
                sprintf(buf, "%s|%.9f|%.9f|%.9f", dispName.c_str(), dx, dy, dz);
                lines.push_back(buf);
            }
        }
        {
            char buf[128];
            sprintf(buf, "Exploded component count in this explosion: %d", explodedCount);
            print(buf);
        }
    }
    UF_free(explTags);

    // 写出参数文件 (UTF-8 无 BOM, CRLF 行尾; VB: File.WriteAllLines + Encoding.UTF8)
    const char *outDir = "E:\\UG\\nx_open_dll";
    if (GetFileAttributesA(outDir) == INVALID_FILE_ATTRIBUTES)
        CreateDirectoryA(outDir, NULL);
    std::string outPath = std::string(outDir) + "\\explosion_params.txt";

    std::ofstream f(outPath.c_str(), std::ios::out | std::ios::binary | std::ios::trunc);
    for (size_t i = 0; i < lines.size(); i++)
        f << lines[i] << "\r\n";
    f.close();

    print("");
    {
        char buf[256];
        sprintf(buf, "Parameter file written: %s (%d entries)", outPath.c_str(), (int)lines.size());
        print(buf);
    }
    print("Put the parameter file in the same folder as explosion_step1.dll, then run step1.");
}

//------------------------------------------------------------------------------
// Entry point(s) for unmanaged internal NXOpen C/C++ programs
//------------------------------------------------------------------------------
//  Explicit Execution
extern "C" DllExport void ufusr(char *parm, int *returnCode, int rlen)
{
    try
    {
        extractExplosionParams();
    }
    catch (const NXException &e1)
    {
        UI::GetUI()->NXMessageBox()->Show("NXException", NXOpen::NXMessageBox::DialogTypeError, e1.Message());
    }
    catch (const exception &e2)
    {
        UI::GetUI()->NXMessageBox()->Show("Exception", NXOpen::NXMessageBox::DialogTypeError, e2.what());
    }
    catch (...)
    {
        UI::GetUI()->NXMessageBox()->Show("Exception", NXOpen::NXMessageBox::DialogTypeError, "Unknown Exception.");
    }
}

//------------------------------------------------------------------------------
// Unload Handler
//------------------------------------------------------------------------------
extern "C" DllExport int ufusr_ask_unload()
{
    return (int)NXOpen::Session::LibraryUnloadOptionImmediately;
}
