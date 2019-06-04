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

#include <stdio.h>
#include <stdlib.h>
#include <windows.h>
#include <shellapi.h>
#include <detours.h>
#pragma warning(push)
#if _MSC_VER > 1400
#pragma warning(disable:6102 6103) // /analyze warnings
#endif
#include <strsafe.h>
#pragma warning(pop)

#define UNUSED(c)       (c) = (c)
#define is_valid_handle(x) \
        (x != NULL && x != INVALID_HANDLE_VALUE)

////////////////////////////////////////////////////////////// Error Messages.
//
VOID AssertMessage(PCSTR szMsg, PCSTR szFile, DWORD nLine)
{
    printf("ASSERT(%s) failed in %s, line %d.", szMsg, szFile, nLine);
}

#define ASSERT(x)   \
do { if (!(x)) { AssertMessage(#x, __FILE__, __LINE__); DebugBreak(); }} while (0)
    ;


//////////////////////////////////////////////////////////////////////////////
//
static BOOLEAN  s_fRemove = FALSE;
static CHAR     s_szDllPath[MAX_PATH] = "";

//////////////////////////////////////////////////////////////////////////////
//
//  This code verifies that the named DLL has been configured correctly
//  to be imported into the target process.  DLLs must export a function with
//  ordinal #1 so that the import table touch-up magic works.
//
static BOOL CALLBACK ExportCallback(_In_opt_ PVOID pContext,
                                    _In_ ULONG nOrdinal,
                                    _In_opt_ LPCSTR pszName,
                                    _In_opt_ PVOID pCode)
{
    (void)pContext;
    (void)pCode;
    (void)pszName;

    if (nOrdinal == 1) {
        *((BOOL *)pContext) = TRUE;
    }
    return TRUE;
}

BOOL DoesDllExportOrdinal1(PCHAR pszDllPath)
{
    HMODULE hDll = LoadLibraryExA(pszDllPath, NULL, DONT_RESOLVE_DLL_REFERENCES);
    if (hDll == NULL) {
        printf("setdll.exe: LoadLibraryEx(%s) failed with error %d.\n",
               pszDllPath,
               GetLastError());
        return FALSE;
    }

    BOOL validFlag = FALSE;
    DetourEnumerateExports(hDll, &validFlag, ExportCallback);
    FreeLibrary(hDll);
    return validFlag;
}

//////////////////////////////////////////////////////////////////////////////
//
static BOOL CALLBACK ListBywayCallback(_In_opt_ PVOID pContext,
                                       _In_opt_ LPCSTR pszFile,
                                       _Outptr_result_maybenull_ LPCSTR *ppszOutFile)
{
    (void)pContext;

    *ppszOutFile = pszFile;
    return TRUE;
}

static BOOL CALLBACK ListFileCallback(_In_opt_ PVOID pContext,
                                      _In_ LPCSTR pszOrigFile,
                                      _In_ LPCSTR pszFile,
                                      _Outptr_result_maybenull_ LPCSTR *ppszOutFile)
{
    (void)pContext;
    *ppszOutFile = pszFile;
    printf("    %s -> %s\n", pszOrigFile, pszFile);
    return TRUE;
}

static BOOL CALLBACK AddBywayCallback(_In_opt_ PVOID pContext,
                                      _In_opt_ LPCSTR pszFile,
                                      _Outptr_result_maybenull_ LPCSTR *ppszOutFile)
{
    PBOOL pbAddedDll = (PBOOL)pContext;
    if (!pszFile && !*pbAddedDll) {                     // Add new byway.
        *pbAddedDll = TRUE;
        *ppszOutFile = s_szDllPath;
    }
    return TRUE;
}

BOOL SetFile(LPWSTR pszPath)
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

    hOld = CreateFileW(szOrg,
                       GENERIC_READ,
                       FILE_SHARE_READ,
                       NULL,
                       OPEN_EXISTING,
                       FILE_ATTRIBUTE_NORMAL,
                       NULL);

    if (hOld == INVALID_HANDLE_VALUE) {
        printf("Couldn't open input file: %ls, error: %d\n",
               szOrg, GetLastError());
        bGood = FALSE;
        goto end;
    }

    hNew = CreateFileW(szNew,
                       GENERIC_WRITE | GENERIC_READ, 0, NULL, CREATE_ALWAYS,
                       FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (hNew == INVALID_HANDLE_VALUE) {
        printf("Couldn't open output file: %ls, error: %d\n",
               szNew, GetLastError());
        bGood = FALSE;
        goto end;
    }

    if ((pBinary = DetourBinaryOpen(hOld)) == NULL) {
        printf("DetourBinaryOpen failed: %d\n", GetLastError());
        goto end;
    }

    if (hOld != INVALID_HANDLE_VALUE) {
        CloseHandle(hOld);
        hOld = INVALID_HANDLE_VALUE;
    }

    {
        BOOL bAddedDll = FALSE;

        DetourBinaryResetImports(pBinary);

        if (!s_fRemove) {
            if (!DetourBinaryEditImports(pBinary,
                                         &bAddedDll,
                                         AddBywayCallback, NULL, NULL, NULL)) {
                printf("DetourBinaryEditImports failed: %d\n", GetLastError());
            }
        }

        if (!DetourBinaryEditImports(pBinary, NULL,
                                     ListBywayCallback, ListFileCallback,
                                     NULL, NULL)) {

            printf("DetourBinaryEditImports failed: %d\n", GetLastError());
        }

        if (!DetourBinaryWrite(pBinary, hNew)) {
            printf("DetourBinaryWrite failed: %d\n", GetLastError());
            bGood = FALSE;
        }

        DetourBinaryClose(pBinary);
        pBinary = NULL;

        if (hNew != INVALID_HANDLE_VALUE) {
            CloseHandle(hNew);
            hNew = INVALID_HANDLE_VALUE;
        }

        if (bGood) {
            if (!DeleteFileW(szOld)) {
                DWORD dwError = GetLastError();
                if (dwError != ERROR_FILE_NOT_FOUND) {
                    printf("Warning: Couldn't delete %ls: %d\n", szOld, dwError);
                    bGood = FALSE;
                }
            }
            if (!MoveFileExW(szNew, szOrg, MOVEFILE_COPY_ALLOWED|MOVEFILE_REPLACE_EXISTING)) {
                printf("Error: Couldn't install %ls as %ls: %d\n",
                       szNew, szOrg, GetLastError());
                bGood = FALSE;
            }
        }

        DeleteFileW(szNew);
    }


  end:
    if (pBinary) {
        DetourBinaryClose(pBinary);
        pBinary = NULL;
    }
    if (hNew != INVALID_HANDLE_VALUE) {
        CloseHandle(hNew);
        hNew = INVALID_HANDLE_VALUE;
    }
    if (hOld != INVALID_HANDLE_VALUE) {
        CloseHandle(hOld);
        hOld = INVALID_HANDLE_VALUE;
    }
    return bGood;
}

//////////////////////////////////////////////////////////////////////////////
//
void PrintUsage(void)
{
    printf("Usage:\n"
           "    setdll [options] binary_files\n"
           "Options:\n"
           "    /d:file.dll  : Add file.dll binary files\n"
           "    /r           : Remove extra DLLs from binary files\n"
           "    /?           : This help screen.\n");
}

//////////////////////////////////////////////////////////////////////// main.
//
int CDECL wmain(int argc, WCHAR **argv)
{
    BOOL   fNeedHelp = FALSE;
    LPWSTR pszFilePart = NULL;
	WCHAR  w_szDllPath[MAX_PATH] = {0};
    int    arg = 1;
    for (; arg < argc; arg++) {
        if (argv[arg][0] == '-' || argv[arg][0] == '/') {
            WCHAR *argn = argv[arg] + 1;
            WCHAR *argp = argn;
            while (*argp && *argp != ':' && *argp != '=')
                argp++;
            if (*argp == ':' || *argp == '=')
                *argp++ = '\0';

            switch (argn[0]) {

              case 'd':                                 // Set DLL
              case 'D':
                if ((wcschr(argp, ':') != NULL || wcschr(argp, '\\') != NULL) &&
                    GetFullPathNameW(argp, sizeof(w_szDllPath), w_szDllPath, &pszFilePart)) {
                }
                else {
                    StringCchPrintfW(w_szDllPath, sizeof(w_szDllPath), L"%s", argp);
					if (!WideCharToMultiByte(CP_UTF8, 0, w_szDllPath, -1, s_szDllPath, sizeof(s_szDllPath), NULL, NULL))
					{
						s_szDllPath[0] = 0;
					}
                }
                break;

              case 'r':                                 // Remove extra set DLLs.
              case 'R':
                s_fRemove = TRUE;
                break;

              case '?':                                 // Help
                fNeedHelp = TRUE;
                break;

              default:
                fNeedHelp = TRUE;
                printf("Bad argument: %ls:%ls\n", argn, argp);
                break;
            }
        }
    }
    if (argc == 1) {
        fNeedHelp = TRUE;
    }
    if (!s_fRemove && s_szDllPath[0] == 0) {
        fNeedHelp = TRUE;
    }
    if (fNeedHelp) {
        PrintUsage();
        return 1;
    }


    if (s_fRemove) {
        printf("Removing extra DLLs from binary files.\n");
    }
    else {
        if (!DoesDllExportOrdinal1(s_szDllPath)) {
            printf("Error: %hs does not export function with ordinal #1.\n",
                   s_szDllPath);
            return 2;
        }
        printf("Adding %hs to binary files.\n", s_szDllPath);
    }

    for (arg = 1; arg < argc; arg++) {
        if (argv[arg][0] != '-' && argv[arg][0] != '/') {
            SetFile(argv[arg]);
        }
    }
    return 0;
}

// End of File
