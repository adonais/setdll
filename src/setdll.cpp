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
#define STR_IS_NUL(s) (s == NULL || *s == 0)

typedef int(WINAPI *SHFileOperationWPtr)(LPSHFILEOPSTRUCTW lpFileOp);
typedef BOOL(WINAPI *PathFileExistsWPtr)(LPCWSTR pszPath);
typedef BOOL(WINAPI *PathAppendWPtr)(LPWSTR pszPath, LPCWSTR pszMore);
typedef BOOL(WINAPI *PathRemoveFileSpecWPtr)(LPWSTR pszPath);
static PathFileExistsWPtr fnPathFileExistsW;
static PathAppendWPtr fnPathAppendW;
static PathRemoveFileSpecWPtr fnPathRemoveFileSpecW;

//////////////////////////////////////////////////////////////////////////////
//
static BOOLEAN s_fRemove = FALSE;
static CHAR s_szDllPath[MAX_PATH] = {0};

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
        printf("setdll.exe: LoadLibraryEx(%s) failed with error %lu.\n", pszDllPath, GetLastError());
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
        printf("Couldn't open input file: %ls, error: %lu\n", szOrg, GetLastError());
        bGood = FALSE;
        goto end;
    }

    hNew = CreateFileW(szNew, GENERIC_WRITE | GENERIC_READ, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (hNew == INVALID_HANDLE_VALUE)
    {
        printf("Couldn't open output file: %ls, error: %lu\n", szNew, GetLastError());
        bGood = FALSE;
        goto end;
    }

    if ((pBinary = DetourBinaryOpen(hOld)) == NULL)
    {
        printf("DetourBinaryOpen failed: %lu\n", GetLastError());
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
                printf("DetourBinaryEditImports failed: %lu\n", GetLastError());
            }
        }

        if (!DetourBinaryEditImports(pBinary, NULL, ListBywayCallback, ListFileCallback, NULL, NULL))
        {

            printf("DetourBinaryEditImports failed: %lu\n", GetLastError());
        }

        if (!DetourBinaryWrite(pBinary, hNew))
        {
            printf("DetourBinaryWrite failed: %lu\n", GetLastError());
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
                    printf("Warning: Couldn't delete %ls: %lu\n", szOld, dwError);
                    bGood = FALSE;
                }
            }
            if (!MoveFileExW(szNew, szOrg, MOVEFILE_COPY_ALLOWED | MOVEFILE_REPLACE_EXISTING))
            {
                printf("Error: Couldn't install %ls as %ls: %lu\n", szNew, szOrg, GetLastError());
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
        "    /t:file.exe          : Test PE file bits\n"
        "    /?                   : This help screen\n"
        "    -7 --help            : 7z command help screen\n";
    char path[N_SIZE] = {0};
    int len = (int)strlen(k_help)+N_SIZE;
    char *s_help = (char*)calloc(len, sizeof(char));
    if (s_help  && get_process_name(path, N_SIZE))
    {
        sprintf_s(s_help, len, k_help, path);
        printf("%s", s_help);
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
