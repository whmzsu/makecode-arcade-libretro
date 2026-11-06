/* ---------------------------------------------------------------------------
 * NX 环境探针（诊断用，不是产品）
 *
 * 目的：把 "NRO 一启动就崩" 拆成三个可独立判断的问题
 *   1) 工具链/libnx 与你的固件 ABI 是否对得上   -> 本探针能否正常显示
 *   2) 你是在 applet 模式（相册）还是 title takeover（全内存）下运行
 *   3) 固件版本 / 可用内存到底是多少
 *
 * 这个程序只用 libnx 的 console + hosversion + svcGetInfo，不链接任何
 * 第三方库、不建线程、不申请大块内存。所以：
 *   - 它跑得起来  => 工具链、hbmenu、内存环境都没问题，锅在 RetroArch 或核心
 *   - 它也崩了    => 与我们自己的代码无关，是 libnx 与固件 ABI 的问题
 * ------------------------------------------------------------------------- */
#include <switch.h>
#include <stdio.h>
#include <string.h>

#ifndef BUILD_LIBNX_VER
#define BUILD_LIBNX_VER "unknown"
#endif

static void print_mem(const char *label, InfoType t)
{
    u64 v = 0;
    Result rc = svcGetInfo(&v, t, CUR_PROCESS_HANDLE, 0);
    if (R_SUCCEEDED(rc))
        printf("%-24s %llu MB\n", label, (unsigned long long)(v / (1024ULL * 1024ULL)));
    else
        printf("%-24s <err 0x%x>\n", label, rc);
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    consoleInit(NULL);

    u32 hos   = hosversionGet();
    int major = (int)((hos >> 16) & 0xFF);
    int minor = (int)((hos >> 8) & 0xFF);
    int micro = (int)(hos & 0xFF);

    AppletType at = appletGetAppletType();

    printf("\n");
    printf("========================================\n");
    printf("  MakeCode Arcade - NX probe\n");
    printf("========================================\n\n");
    printf("build libnx : %s\n", BUILD_LIBNX_VER);
    printf("build time  : %s %s\n", __DATE__, __TIME__);
    printf("firmware    : %d.%d.%d\n", major, minor, micro);

    printf("applet type : ");
    switch (at) {
    case AppletType_Application:
        printf("Application (title takeover, full memory)\n");
        break;
    case AppletType_SystemApplication:
        printf("SystemApplication (full memory)\n");
        break;
    case AppletType_LibraryApplet:
        printf("LibraryApplet (album mode, LIMITED memory!)\n");
        break;
    case AppletType_OverlayApplet:
        printf("OverlayApplet (LIMITED memory!)\n");
        break;
    default:
        printf("other (%d)\n", (int)at);
        break;
    }

    printf("\n--- memory (self process) ---\n");
    print_mem("total:", InfoType_TotalMemorySize);
    print_mem("used:",  InfoType_UsedMemorySize);

    printf("\n----------------------------------------\n");
    printf("如果上面这些字看得见，说明:\n");
    printf("  * libnx 与你的固件 ABI 兼容\n");
    printf("  * hbmenu 启动路径正常\n");
    printf("  * 这个程序没有因为内存不足被干掉\n");
    printf("----------------------------------------\n\n");
    printf("60 秒后自动退出；想提前走可按 HOME 回主菜单\n");

    /* 故意不碰手柄 API：新旧 libnx 的手柄接口改过好几次，探针本身必须
     * 只依赖最稳定的那几个函数，否则会用自己的编译错误掩盖真正的结论。 */
    int ticks = 0;
    while (appletMainLoop())
    {
        if (++ticks > 60 * 60)   /* ~60 秒自动退出，别把用户卡在这 */
            break;
        consoleUpdate(NULL);
    }

    consoleExit(NULL);
    return 0;
}
