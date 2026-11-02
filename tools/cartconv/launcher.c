/*
 * launcher.c — cartconv 的 Windows 拖拽入口
 *
 * 只做四件事，但每一件都必要：
 *   1. 把控制台代码页切成 UTF-8 —— 否则 node 输出的中文全是乱码
 *   2. 定位同目录下的 runtime\node.exe 和 cartconv.js
 *   3. 透传全部命令行参数（拖拽多个文件时 Windows 会依次传进来）
 *   4. 拖拽场景下退出前暂停 —— explorer 为拖拽新开的控制台会在进程退出时
 *      立刻关闭，用户根本看不到转换结果
 *
 * 编译（zig cc，自带 mingw 头，无需单独装工具链）：
 *   zig cc -target x86_64-windows-gnu -O2 -o cartconv.exe launcher.c
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <io.h>
#include <stdio.h>
#include <wchar.h>
#include <stdlib.h>

#define CMD_CAP 32768

/* 判断父进程是不是 explorer.exe —— 拖拽运行时必然是它 */
static int parent_is_explorer(void)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return 0;

    PROCESSENTRY32W pe;
    pe.dwSize = sizeof(pe);
    DWORD me = GetCurrentProcessId();
    DWORD ppid = 0;

    if (Process32FirstW(snap, &pe)) {
        do {
            if (pe.th32ProcessID == me) {
                ppid = pe.th32ParentProcessID;
                break;
            }
        } while (Process32NextW(snap, &pe));
    }
    if (!ppid) {
        CloseHandle(snap);
        return 0;
    }

    int is_explorer = 0;
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            if (pe.th32ProcessID == ppid) {
                is_explorer = (_wcsicmp(pe.szExeFile, L"explorer.exe") == 0);
                break;
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return is_explorer;
}

static void pause_if_dragged(void)
{
    /*
     * 只在"用户把文件拖到 exe 上"这一种情况下暂停：explorer 为拖拽开的
     * 控制台会在进程退出时立刻关闭，不暂停用户根本看不到结果。
     * 其它情形（cmd/bash 里手动运行、被管道调用）一律不暂停，
     * 否则批处理会卡死。
     */
    if (!_isatty(_fileno(stdin)))
        return;
    if (!parent_is_explorer())
        return;

    fputs("\n按回车键关闭...", stdout);
    fflush(stdout);
    (void)getchar();
}

static void append_quoted(wchar_t *buf, size_t cap, size_t *len, const wchar_t *s)
{
    size_t used = *len;
    /* 参数之间必须有空格，否则 Windows 会把几段引号拼成一个文件名 */
    if (used > 0 && used + 1 < cap)
        buf[used++] = L' ';
    if (used + 2 >= cap)
        return;
    buf[used++] = L'"';

    const wchar_t *p = s;
    while (*p) {
        size_t backslashes = 0;
        while (*p == L'\\') {
            backslashes++;
            p++;
        }
        if (*p == L'"') {
            /* 反斜杠必须加倍，再补一个转义引号 */
            for (size_t i = 0; i < backslashes * 2 + 1; i++)
                if (used + 1 < cap) buf[used++] = L'\\';
            if (used + 1 < cap) buf[used++] = L'"';
            p++;
        } else if (*p == L'\0') {
            /* 参数结尾的反斜杠也要加倍，否则会转义掉收尾引号 */
            for (size_t i = 0; i < backslashes * 2; i++)
                if (used + 1 < cap) buf[used++] = L'\\';
            break;
        } else {
            for (size_t i = 0; i < backslashes; i++)
                if (used + 1 < cap) buf[used++] = L'\\';
            if (used + 1 < cap) buf[used++] = *p;
            p++;
        }
    }
    if (used + 1 < cap) buf[used++] = L'"';
    buf[used] = L'\0';
    *len = used;
}

int wmain(int argc, wchar_t **argv)
{
    (void)SetConsoleOutputCP(65001);
    (void)SetConsoleCP(65001);

    wchar_t exe_path[MAX_PATH];
    DWORD n = GetModuleFileNameW(NULL, exe_path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) {
        fputs("[错误] 无法定位程序路径\n", stdout);
        pause_if_dragged();
        return 2;
    }
    /* 去掉文件名，留目录 */
    wchar_t *slash = wcsrchr(exe_path, L'\\');
    if (slash) *slash = L'\0';

    wchar_t node_exe[MAX_PATH * 2];
    wchar_t script[MAX_PATH * 2];
    _snwprintf(node_exe, MAX_PATH * 2, L"%ls\\runtime\\node.exe", exe_path);
    _snwprintf(script, MAX_PATH * 2, L"%ls\\cartconv.js", exe_path);

    if (GetFileAttributesW(node_exe) == INVALID_FILE_ATTRIBUTES ||
        GetFileAttributesW(script) == INVALID_FILE_ATTRIBUTES) {
        wprintf(L"[错误] 安装不完整：需要同目录下的 cartconv.js 和 runtime\\node.exe\n");
        wprintf(L"       当前目录：%ls\n", exe_path);
        pause_if_dragged();
        return 2;
    }

    wchar_t *cmd = (wchar_t *)calloc(CMD_CAP, sizeof(wchar_t));
    if (!cmd) {
        pause_if_dragged();
        return 2;
    }
    size_t len = 0;
    append_quoted(cmd, CMD_CAP, &len, node_exe);
    append_quoted(cmd, CMD_CAP, &len, script);
    for (int i = 1; i < argc; i++)
        append_quoted(cmd, CMD_CAP, &len, argv[i]);

    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    ZeroMemory(&pi, sizeof(pi));

    if (!CreateProcessW(NULL, cmd, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) {
        wprintf(L"[错误] 启动 node 失败（错误码 %lu）\n", GetLastError());
        wprintf(L"       命令行：%ls\n", cmd);
        free(cmd);
        pause_if_dragged();
        return 2;
    }
    if (getenv("CART_CONV_DEBUG"))
        fwprintf(stderr, L"[dbg] %ls\n", cmd);

    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    free(cmd);

    pause_if_dragged();
    return (int)(code > 255 ? 255 : code);
}
