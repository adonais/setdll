//////////////////////////////////////////////////////////////////////////////
//
//  Detours Test Program (setdll.cpp of setdll.exe)
//
//  Microsoft Research Detours Package
//
//  Copyright (c) Microsoft Corporation.  All rights reserved.
//
#ifndef UNICODE
#define UNICODE
#endif

#ifndef _UNICODE
#define _UNICODE
#endif

#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <process.h>
#include <windows.h>
#include <shlwapi.h>
#include <shellapi.h>
#include <detours.h>
#include "7zc.h"
#pragma warning(push)
#if _MSC_VER > 1400
#pragma warning(disable : 6102 6103) // /analyze warnings
#endif
#include <strsafe.h>
#pragma warning(pop)

#define BUFFSIZE 1024
#define LEN_NAME 6
#define UNUSED(c) (c) = (c)
#define is_valid_handle(x) (x != NULL && x != INVALID_HANDLE_VALUE)

typedef int(WINAPI *SHFileOperationWPtr)(LPSHFILEOPSTRUCTW lpFileOp);
typedef BOOL(WINAPI *PathFileExistsAPtr)(LPCSTR pszPath);
typedef BOOL(WINAPI *PathFileExistsWPtr)(LPCWSTR pszPath);
typedef BOOL(WINAPI *PathAppendWPtr)(LPWSTR pszPath, LPCWSTR pszMore);
typedef BOOL(WINAPI *PathRemoveFileSpecWPtr)(LPWSTR pszPath);
static PathFileExistsAPtr fnPathFileExistsA;
static PathFileExistsWPtr fnPathFileExistsW;
static PathAppendWPtr fnPathAppendW;
static PathRemoveFileSpecWPtr fnPathRemoveFileSpecW;

////////////////////////////////////////////////////////////// Error Messages.
//
VOID
AssertMessage(PCSTR szMsg, PCSTR szFile, DWORD nLine)
{
    printf("ASSERT(%s) failed in %s, line %d.", szMsg, szFile, nLine);
}

#define ASSERT(x)                                  \
    do                                             \
    {                                              \
        if (!(x))                                  \
        {                                          \
            AssertMessage(#x, __FILE__, __LINE__); \
            DebugBreak();                          \
        }                                          \
    } while (0)
;

//////////////////////////////////////////////////////////////////////////////
//
static BOOLEAN s_fRemove = FALSE;
static CHAR s_szDllPath[MAX_PATH] = "";

//////////////////////////////////////////////////////////////////////////////
//
//  This code verifies that the named DLL has been configured correctly
//  to be imported into the target process.  DLLs must export a function with
//  ordinal #1 so that the import table touch-up magic works.
//
static BOOL CALLBACK
ExportCallback(_In_opt_ PVOID pContext, _In_ ULONG nOrdinal, _In_opt_ LPCSTR pszName, _In_opt_ PVOID pCode)
{
    (void) pContext;
    (void) pCode;
    (void) pszName;

    if (nOrdinal == 1)
    {
        *((BOOL *) pContext) = TRUE;
    }
    return TRUE;
}

BOOL
DoesDllExportOrdinal1(PCHAR pszDllPath)
{
    HMODULE hDll = LoadLibraryExA(pszDllPath, NULL, DONT_RESOLVE_DLL_REFERENCES);
    if (hDll == NULL)
    {
        printf("setdll.exe: LoadLibraryEx(%s) failed with error %d.\n", pszDllPath, GetLastError());
        return FALSE;
    }

    BOOL validFlag = FALSE;
    DetourEnumerateExports(hDll, &validFlag, ExportCallback);
    FreeLibrary(hDll);
    return validFlag;
}

//////////////////////////////////////////////////////////////////////////////
//
static BOOL CALLBACK
ListBywayCallback(_In_opt_ PVOID pContext, _In_opt_ LPCSTR pszFile, _Outptr_result_maybenull_ LPCSTR *ppszOutFile)
{
    (void) pContext;

    *ppszOutFile = pszFile;
    return TRUE;
}

static BOOL CALLBACK
ListFileCallback(_In_opt_ PVOID pContext, _In_ LPCSTR pszOrigFile, _In_ LPCSTR pszFile, _Outptr_result_maybenull_ LPCSTR *ppszOutFile)
{
    (void) pContext;
    *ppszOutFile = pszFile;
    printf("    %s -> %s\n", pszOrigFile, pszFile);
    return TRUE;
}

static BOOL CALLBACK
AddBywayCallback(_In_opt_ PVOID pContext, _In_opt_ LPCSTR pszFile, _Outptr_result_maybenull_ LPCSTR *ppszOutFile)
{
    PBOOL pbAddedDll = (PBOOL) pContext;
    if (!pszFile && !*pbAddedDll)
    { // Add new byway.
        *pbAddedDll = TRUE;
        *ppszOutFile = s_szDllPath;
    }
    return TRUE;
}

static BOOL
SetFile(LPWSTR pszPath)
{
    BOOL bGood = TRUE;
    HANDLE hOld = INVALID_HANDLE_VALUE;
    HANDLE hNew = INVALID_HANDLE_VALUE;
    PDETOUR_BINARY pBinary = NULL;
    WCHAR szOrg[MAX_PATH];
    WCHAR szNew[MAX_PATH];
    WCHAR szOld[MAX_PATH];

    szOld[0] = '\0';
    szNew[0] = '\0';
    if (pszPath == NULL || pszPath[0] == 0)
    {
        return FALSE;
    }
    StringCchCopyW(szOrg, sizeof(szOrg), pszPath);
    StringCchCopyW(szNew, sizeof(szNew), szOrg);
    StringCchCatW(szNew, sizeof(szNew), L"#");
    StringCchCopyW(szOld, sizeof(szOld), szOrg);
    StringCchCatW(szOld, sizeof(szOld), L"~");
    printf("  %ls:\n", pszPath);

    hOld = CreateFileW(szOrg, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);

    if (hOld == INVALID_HANDLE_VALUE)
    {
        printf("Couldn't open input file: %ls, error: %d\n", szOrg, GetLastError());
        bGood = FALSE;
        goto end;
    }

    hNew = CreateFileW(szNew, GENERIC_WRITE | GENERIC_READ, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (hNew == INVALID_HANDLE_VALUE)
    {
        printf("Couldn't open output file: %ls, error: %d\n", szNew, GetLastError());
        bGood = FALSE;
        goto end;
    }

    if ((pBinary = DetourBinaryOpen(hOld)) == NULL)
    {
        printf("DetourBinaryOpen failed: %d\n", GetLastError());
        goto end;
    }

    if (hOld != INVALID_HANDLE_VALUE)
    {
        CloseHandle(hOld);
        hOld = INVALID_HANDLE_VALUE;
    }

    {
        BOOL bAddedDll = FALSE;

        DetourBinaryResetImports(pBinary);

        if (!s_fRemove)
        {
            if (!DetourBinaryEditImports(pBinary, &bAddedDll, AddBywayCallback, NULL, NULL, NULL))
            {
                printf("DetourBinaryEditImports failed: %d\n", GetLastError());
            }
        }

        if (!DetourBinaryEditImports(pBinary, NULL, ListBywayCallback, ListFileCallback, NULL, NULL))
        {

            printf("DetourBinaryEditImports failed: %d\n", GetLastError());
        }

        if (!DetourBinaryWrite(pBinary, hNew))
        {
            printf("DetourBinaryWrite failed: %d\n", GetLastError());
            bGood = FALSE;
        }

        DetourBinaryClose(pBinary);
        pBinary = NULL;

        if (hNew != INVALID_HANDLE_VALUE)
        {
            CloseHandle(hNew);
            hNew = INVALID_HANDLE_VALUE;
        }

        if (bGood)
        {
            if (!DeleteFileW(szOld))
            {
                DWORD dwError = GetLastError();
                if (dwError != ERROR_FILE_NOT_FOUND)
                {
                    printf("Warning: Couldn't delete %ls: %d\n", szOld, dwError);
                    bGood = FALSE;
                }
            }
            if (!MoveFileExW(szNew, szOrg, MOVEFILE_COPY_ALLOWED | MOVEFILE_REPLACE_EXISTING))
            {
                printf("Error: Couldn't install %ls as %ls: %d\n", szNew, szOrg, GetLastError());
                bGood = FALSE;
            }
        }

        DeleteFileW(szNew);
    }

end:
    if (pBinary)
    {
        DetourBinaryClose(pBinary);
        pBinary = NULL;
    }
    if (hNew != INVALID_HANDLE_VALUE)
    {
        CloseHandle(hNew);
        hNew = INVALID_HANDLE_VALUE;
    }
    if (hOld != INVALID_HANDLE_VALUE)
    {
        CloseHandle(hOld);
        hOld = INVALID_HANDLE_VALUE;
    }
    return bGood;
}

static HMODULE
init_shinfo(void)
{
    HMODULE shlwapi = LoadLibraryW(L"shlwapi.dll");
    if (shlwapi != NULL)
    {
        fnPathFileExistsA = (PathFileExistsAPtr) GetProcAddress(shlwapi, "PathFileExistsA");
        fnPathFileExistsW = (PathFileExistsWPtr) GetProcAddress(shlwapi, "PathFileExistsW");
        fnPathAppendW = (PathAppendWPtr) GetProcAddress(shlwapi, "PathAppendW");
        fnPathRemoveFileSpecW = (PathRemoveFileSpecWPtr) GetProcAddress(shlwapi, "PathRemoveFileSpecW");
        if (!(fnPathFileExistsA && fnPathFileExistsW && fnPathAppendW && fnPathRemoveFileSpecW))
        {
            FreeLibrary(shlwapi);
            shlwapi = NULL;
        }
    }
    return shlwapi;
}

static BOOL
fixed_file(LPCSTR path, LPCSTR desc, LPCSTR con, BOOL back)
{
    long pos = 0;
    char buff[BUFFSIZE + 1] = { 0 };
    FILE *fp = NULL;
    if (FAILED(fopen_s(&fp, path, "rb+")))
    {
        printf("fopen_s %s false\n", path);
        return FALSE;
    }
    while (fgets(buff, BUFFSIZE, fp) != NULL)
    {
        if (strstr(buff, desc) != NULL)
        {
            pos = ftell(fp);
            if (back)
            {
                pos -= (long) strlen(buff);
            }
            break;
        }
    }
    if (pos)
    {
        size_t bytes = 0;
        fseek(fp, 0, SEEK_END);
        long len = ftell(fp);
        len -= pos;
        printf("len = %d\n", len);
        fseek(fp, pos, SEEK_SET);
        char *next = (char *) calloc(len, sizeof(char));
        if (!next)
        {
            printf("calloc next false\n");
            fclose(fp);
            return FALSE;
        }
        char *backup = (char *) calloc(len, sizeof(char));
        DWORD offset = 0;
        if (!backup)
        {
            printf("calloc backup false\n");
            fclose(fp);
            free(next);
            return FALSE;
        }
        while (!feof(fp))
        {
            bytes = fread(next, 1, 128, fp);
            if (bytes > 0)
            {
                memcpy(backup + offset, next, bytes);
                offset += (DWORD) bytes;
            }
        }
        fseek(fp, pos, SEEK_SET);
        fwrite(con, strlen(con), 1, fp);
        fwrite(backup, offset, 1, fp);
        free(next);
        free(backup);
    }
    fclose(fp);
    return TRUE;
}

static BOOL
edit_files(LPCWSTR lpath)
{
    BOOL cn = FALSE;
    char f_xul[MAX_PATH + 1] = { 0 };
    char f_dtd[MAX_PATH + 1] = { 0 };
    char f_js[MAX_PATH + 1] = { 0 };
    char path[MAX_PATH + 1] = { 0 };
    LPCSTR js_desc1 = "this.onSaveableLink || this.onPlainTextLink);";
    LPCSTR js_desc2 = "Backwards-compatibility wrapper";
    LPCSTR js_inst1 = "    this.showItem(\"context-downloadlink\", this.onSaveableLink || this.onPlainTextLink);";
    LPCSTR js_inst2 =
        "\
  downloadLink() {\n\
    if (AppConstants.platform === \"win\") {\n\
    const exeName = \"upcheck.exe\";\n\
    let exe = Services.dirsvc.get(\"GreBinD\", Ci.nsIFile);\n\
    let cfile = Services.dirsvc.get(\"ProfD\", Ci.nsIFile);\n\
    exe.append(exeName);\n\
    cfile.append(\"cookies.sqlite\");\n\
    let process = Cc[\"@mozilla.org/process/util;1\"]\n\
                    .createInstance(Ci.nsIProcess);\n\
    process.init(exe);\n\
    process.startHidden = false;\n\
    process.noShell = true;\n\
    process.run(false, [\"-i\", this.linkURL, \"-b\", cfile.path, \"-m\", \"1\"], 6);\n\
    }\n\
  },\n\n";
    LPCSTR xul_desc = "gContextMenu.saveLink();";
    LPCSTR xul_inst =
        "\
      <menuitem id=\"context-downloadlink\"\n\
                label=\"&downloadLinkCmd.label;\"\n\
                accesskey=\"&downloadLinkCmd.accesskey;\"\n\
                oncommand=\"gContextMenu.downloadLink();\"/>\n";
    LPCSTR dtd_desc = "saveLinkCmd.accesskey";
    LPCSTR dtd_inst1 =
        "\
<!ENTITY downloadLinkCmd.label        \"使用 Upcheck 下载此链接\">\n\
<!ENTITY downloadLinkCmd.accesskey    \"\">\n";
    LPCSTR dtd_inst2 =
        "\
<!ENTITY downloadLinkCmd.label        \"Download Link With Upcheck\">\n\
<!ENTITY downloadLinkCmd.accesskey    \"\">\n";
    LPCSTR file0 = "chrome\\browser\\content\\browser\\browser.xhtml";
    LPCSTR file1 = "chrome\\browser\\content\\browser\\browser.xul";
    LPCSTR file2 = "chrome\\browser\\content\\browser\\nsContextMenu.js";
    LPCSTR file3 = "chrome\\zh-CN\\locale\\browser\\browser.dtd";
    LPCSTR file4 = "chrome\\en-US\\locale\\browser\\browser.dtd";
    if (!lpath)
    {
        printf("lpath is null\n");
        return FALSE;
    }
    if (!WideCharToMultiByte(CP_UTF8, 0, lpath, -1, path, sizeof(path), NULL, NULL))
    {
        printf("WideCharToMultiByte path return false\n");
        return FALSE;
    }
    _snprintf_s(f_dtd, MAX_PATH, "%s\\%s", path, file3);
    cn = fnPathFileExistsA(f_dtd);
    if (!cn)
    {
        _snprintf_s(f_dtd, MAX_PATH, "%s\\%s", path, file4);
        if (!fnPathFileExistsA(f_dtd))
        {
            return FALSE;
        }
    }
    _snprintf_s(f_xul, MAX_PATH, "%s\\%s", path, file1);
    _snprintf_s(f_js, MAX_PATH, "%s\\%s", path, file2);
    if (!fnPathFileExistsA(f_xul))
    {
        _snprintf_s(f_xul, MAX_PATH, "%s\\%s", path, file0);
    }    
    if (!(fnPathFileExistsA(f_xul) && fnPathFileExistsA(f_js)))
    {
        printf("file not exist\n");
        return FALSE;
    }
    if (!fixed_file(f_js, js_desc1, js_inst1, FALSE))
    {
        printf("fixed_file js_desc1 return false\n");
        return FALSE;
    }
    if (!fixed_file(f_js, js_desc2, js_inst2, TRUE))
    {
        printf("fixed_file js_desc2 return false\n");
        return FALSE;
    }
    if (!fixed_file(f_xul, xul_desc, xul_inst, FALSE))
    {
        printf("fixed_file xul_desc return false\n");
        return FALSE;
    }
    if (cn)
    {
        if (!fixed_file(f_dtd, dtd_desc, dtd_inst1, FALSE))
        {
            printf("fixed_file dtd_inst1 return false\n");
            return FALSE;
        }
    }
    else
    {
        if (!fixed_file(f_dtd, dtd_desc, dtd_inst2, FALSE))
        {
            printf("fixed_file dtd_inst2 return false\n");
            return FALSE;
        }
    }
    return TRUE;
}

static BOOL
erase_dir(LPCWSTR lpszDir, BOOL noRecycle = TRUE)
{
    int ret = -1;
    WCHAR *pszFrom = NULL;
    size_t len = 0;
    SHFileOperationWPtr fnSHFileOperationW = NULL;
    HMODULE shell32 = LoadLibraryW(L"shell32.dll");
    if (!shell32)
    {
        return FALSE;
    }
    if (!lpszDir)
    {
        return FALSE;
    }
    do
    {
        len = wcslen(lpszDir);
        fnSHFileOperationW = (SHFileOperationWPtr) GetProcAddress(shell32, "SHFileOperationW");
        if (fnSHFileOperationW == NULL)
        {
            break;
        }
        if ((pszFrom = (WCHAR *) calloc(len+4, sizeof(WCHAR))) == NULL)
        {
            break;
        }
        wcscpy_s(pszFrom, len + 2, lpszDir);
        pszFrom[len] = 0;
        pszFrom[len + 1] = 0;

        SHFILEOPSTRUCTW fileop;
        fileop.hwnd = NULL;                              // no status display
        fileop.wFunc = FO_DELETE;                        // delete operation
        fileop.pFrom = pszFrom;                          // source file name as double null terminated string
        fileop.pTo = NULL;                               // no destination needed
        fileop.fFlags = FOF_NOCONFIRMATION | FOF_SILENT; // do not prompt the user

        if (!noRecycle)
        {
            fileop.fFlags |= FOF_ALLOWUNDO;
        }
        fileop.fAnyOperationsAborted = FALSE;
        fileop.lpszProgressTitle = NULL;
        fileop.hNameMappings = NULL;
        // SHFileOperation returns zero if successful; otherwise nonzero
        ret = fnSHFileOperationW(&fileop);
    } while (0);
    if (pszFrom)
    {
        free(pszFrom);
    }
    if (shell32)
    {
        FreeLibrary(shell32);
    }
    return (0 == ret);
}

static WCHAR *
rand_str(WCHAR *str, const int len)
{
    int i;
    for (i = 0; i < len; ++i)
        str[i] = 'A' + rand() % 26;
    str[len] = '\0';
    return str;
}

static BOOL
Patched_File(LPCWSTR pfile)
{
    WCHAR aPath[MAX_PATH + 1] = { 0 };
    WCHAR aCmd[MAX_PATH + 1] = { 0 };
    WCHAR temp[LEN_NAME + 1];
    WCHAR omni[MAX_PATH + 1] = { 0 };
    if (pfile == NULL || pfile[1] != ':')
    {
        return FALSE;
    }
    if (!fnPathFileExistsW(pfile))
    {
        printf("dist file not exist\n");
        return FALSE;
    }
    if (!GetTempPathW(MAX_PATH, aPath))
    {
        return FALSE;
    }
    if (FAILED(StringCchCatW(aPath, MAX_PATH, L"omni_")))
    {
        return FALSE;
    }
    srand((unsigned int) time(NULL));
    if (FAILED(StringCchCatW(aPath, MAX_PATH, rand_str(temp, LEN_NAME))))
    {
        return FALSE;
    }
    _snwprintf_s(aCmd, MAX_PATH, L"x -aoa -o\"%s\" \"%s\"", aPath, pfile);
    if (exec_zmain1(aCmd) == -1)
    {
        printf("exec_zmain1 failed\n");
        return FALSE;
    }        
    _snwprintf_s(omni, MAX_PATH, L"%s", pfile);
    if (!(fnPathRemoveFileSpecW(omni) && fnPathAppendW(omni, L"omni.zip") && SetCurrentDirectoryW(aPath)))
    {
        printf("SetCurrentDirectory failed\n");
        return FALSE;
    }
    if (!edit_files(aPath))
    {
        printf("edit_files failed\n");
        return FALSE;
    }
    _snwprintf_s(aCmd, MAX_PATH, L"a -tzip -mx=0 -mmt=4 \"%s\" %s", omni, L"*");
    if (exec_zmain1(aCmd) != 0)
    {
        printf("compressed file failed\n");
        return FALSE;
    }
    erase_dir(aPath);
    return MoveFileExW(omni, pfile, MOVEFILE_COPY_ALLOWED | MOVEFILE_REPLACE_EXISTING);
}
//////////////////////////////////////////////////////////////////////////////
//

int __stdcall
get_file_bits(const wchar_t* path)
{
    IMAGE_DOS_HEADER dos_header;
    IMAGE_NT_HEADERS pe_header;
    int  	ret = 1;
    HANDLE	hFile = CreateFileW(path,GENERIC_READ,
                                FILE_SHARE_READ,NULL,OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL,NULL);
    if( !is_valid_handle(hFile) )
    {
        return ret;
    }
    do
    {
        DWORD readed = 0;
        DWORD m_ptr  = SetFilePointer( hFile,0,NULL,FILE_BEGIN );
        if ( INVALID_SET_FILE_POINTER == m_ptr )
        {
            break;
        }
        ret = ReadFile( hFile,&dos_header,sizeof(IMAGE_DOS_HEADER),&readed,NULL );
        if( ret && readed != sizeof(IMAGE_DOS_HEADER) && \
            dos_header.e_magic != IMAGE_DOS_SIGNATURE )
        {
            break;
        }
        m_ptr = SetFilePointer( hFile,dos_header.e_lfanew,NULL,FILE_BEGIN );
        if ( INVALID_SET_FILE_POINTER == m_ptr )
        {
            break;
        }
        ret = ReadFile( hFile,&pe_header,sizeof(IMAGE_NT_HEADERS),&readed,NULL );
        if( ret && readed != sizeof(IMAGE_NT_HEADERS) )
        {
            break;
        }
        if (pe_header.FileHeader.Machine == IMAGE_FILE_MACHINE_I386)
        {
            ret = 32;
            break;
        }
        if (pe_header.FileHeader.Machine == IMAGE_FILE_MACHINE_IA64 ||
            pe_header.FileHeader.Machine == IMAGE_FILE_MACHINE_AMD64)
        {
            ret = 64;
        }
    } while (0);
    CloseHandle(hFile);
    return ret;
}

//////////////////////////////////////////////////////////////////////////////
//
static void
PrintUsage(void)
{
    #define N_SIZE 64
    const char *k_help = 
        "Usage:\n"
        "    %s [options] binary_files\n"
        "Options:\n"
        "    /d:file.dll          : Add file.dll binary files\n"
        "    /r                   : Remove extra DLLs from binary files\n"
        "    /p:browser\\omni.ja   : Repair omni.ja to support Upcheck.exe\n"
        "    /t:file.exe          : Test PE file bits\n"        
        "    /?                   : This help screen\n"
        "    -7 --help            : 7z command help screen\n";    
    char path[N_SIZE] = {0};
    int len = (int)strlen(k_help)+N_SIZE;
    char *s_help = (char*)calloc(len, sizeof(char));
    if (s_help  && get_process_name(path, N_SIZE))
    {
        sprintf_s(s_help, len, k_help, path);
        printf(s_help);
		free(s_help);
    }    
    #undef N_SIZE
}

//////////////////////////////////////////////////////////////////////// main.
//
int CDECL
wmain(int argc, WCHAR **argv)
{
    BOOL fNeedHelp = FALSE;
    BOOL fNeedZip  = TRUE;
    LPWSTR pszFilePart = NULL;
    WCHAR w_szDllPath[MAX_PATH] = { 0 };
    int arg = 1;  
    for (; arg < argc; arg++)
    {
        if (argv[arg][0] == '-' || argv[arg][0] == '/')
        {
            WCHAR *argn = argv[arg] + 1;
            WCHAR *argp = argn;
            while (*argp && *argp != ':' && *argp != '=')
                argp++;
            if (*argp == ':' || *argp == '=') *argp++ = '\0';        
            switch (argn[0])
            {

                case 'd': // Set DLL
                case 'D':
		            if (wcslen(argp) < 2)
		            {
		                fNeedHelp = TRUE;
		                break;
		            }                  	
                    if ((wcschr(argp, ':') != NULL || wcschr(argp, '\\') != NULL) && GetFullPathNameW(argp, MAX_PATH, w_szDllPath, &pszFilePart))
                    {
                    }
                    else
                    {
                        StringCchPrintfW(w_szDllPath, MAX_PATH, L"%s", argp);
                    }
                    if (!WideCharToMultiByte(CP_ACP, 0, w_szDllPath, -1, s_szDllPath, sizeof(s_szDllPath), NULL, NULL))
                    {
                        s_szDllPath[0] = 0;
                    }
                    fNeedZip = FALSE;
                    break;

                case 'p': // Fixed omni
                case 'P':
		            if (wcslen(argp) < 2)
		            {
		                fNeedHelp = TRUE;
		                break;
		            }                	
                    if (argp[1] != ':' && GetFullPathNameW(argp, MAX_PATH, w_szDllPath, &pszFilePart))
                    {
                        // MessageBoxW(NULL, w_szDllPath, L"w_sz:", 0);
                    }
                    else
                    {
                        StringCchPrintfW(w_szDllPath, MAX_PATH, L"%s", argp);
                    }
                    if (!WideCharToMultiByte(CP_ACP, 0, w_szDllPath, -1, s_szDllPath, sizeof(s_szDllPath), NULL, NULL))
                    {
                        s_szDllPath[0] = 0;
                    }
                    if (strlen(s_szDllPath) > 1)
                    {
                        HMODULE shlwapi = init_shinfo();
                        if (shlwapi != NULL)
                        {
                            Patched_File(w_szDllPath);
                            FreeLibrary(shlwapi);
                            return 0;
                        }
                        return 1;
                    }
                    fNeedZip = FALSE;
                    break;

                case 't': // get PE file bits
                case 'T':
		            if (wcslen(argp) < 2)
		            {
		                fNeedHelp = TRUE;
		                break;
		            }                	
                    if (argp[1] != ':' && GetFullPathNameW(argp, MAX_PATH, w_szDllPath, &pszFilePart))
                    {
                        ;
                    }
                    else
                    {
                        StringCchPrintfW(w_szDllPath, MAX_PATH, L"%s", argp);
                    }
                    if (!WideCharToMultiByte(CP_ACP, 0, w_szDllPath, -1, s_szDllPath, sizeof(s_szDllPath), NULL, NULL))
                    {
                        s_szDllPath[0] = 0;
                    }
                    if (strlen(s_szDllPath) > 1)
                    {
                        int bits = get_file_bits(w_szDllPath);
                        if (bits == 32)
                        {
                            printf("PE32 executable (i386), for MS Windows\n");
                        }
                        else if (bits == 64)
                        {
                            printf("PE32+ executable (x86-64), for MS Windows\n");
                        }                            
                        return bits;
                    }
                    fNeedZip = FALSE;
                    break;

                case 'r': // Remove extra set DLLs.
                case 'R':
                    s_fRemove = TRUE;
                    fNeedZip = FALSE;
                    break;
                case '7':
                {
                    return exec_zmain2(argc, argv);
                }
                break;

                case '?': // Help
                    fNeedHelp = TRUE;
                    fNeedZip = FALSE;
                    break;

                default:
                    fNeedZip = TRUE;
                    break;
            }
        }
        else if (fNeedZip)
        {
            return exec_zmain2(argc, argv);
        }
    }
    if (argc == 1)
    {
        fNeedHelp = TRUE;
    }
    if (!s_fRemove && s_szDllPath[0] == 0)
    {
        fNeedHelp = TRUE;
    }
    if (fNeedHelp)
    {
        PrintUsage();
        return 1;
    }

    if (s_fRemove)
    {
        printf("Removing extra DLLs from binary files.\n");
    }
    else
    {
        if (!DoesDllExportOrdinal1(s_szDllPath))
        {
            printf("Error: %hs does not export function with ordinal #1.\n", s_szDllPath);
            return 2;
        }
        printf("Adding %hs to binary files.\n", s_szDllPath);
    }

    for (arg = 1; arg < argc; arg++)
    {
        if (argv[arg][0] != '-' && argv[arg][0] != '/')
        {
            SetFile(argv[arg]);
        }
    }
    return 0;
}

// End of File
