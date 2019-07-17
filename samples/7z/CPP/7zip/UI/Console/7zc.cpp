#include "StdAfx.h"

#ifdef _WIN32
#include "../../../../C/DllSecur.h"
#endif

#include "../../../Common/MyException.h"
#include "../../../Common/StdOutStream.h"

#include "../../../Windows/ErrorMsg.h"
#include "../../../Windows/NtCheck.h"

#include "../Common/ArchiveCommandLine.h"
#include "../Common/ExitCode.h"

#include "ConsoleClose.h"
#include <atomic>

typedef LPWSTR *(WINAPI *CommandLineToArgvWPtr)(LPCWSTR lpCmdLine, int *pNumArgs);

extern int
Internal_Main2(int numArgs, WCHAR *args[]);

static const char *const kException_CmdLine_Error_Message = "Command Line Error:";
static const char *const kExceptionErrorMessage = "ERROR:";
static const char *const kUserBreakMessage = "Break signaled";
static const char *const kMemoryExceptionMessage = "ERROR: Can't allocate required memory!";
static const char *const kUnknownExceptionMessage = "Unknown Error";
static const char *const kInternalExceptionMessage = "\n\nInternal Error #";

#define NT_CHECK_FAIL_ACTION                       \
    *g_StdStream << "Unsupported Windows version"; \
    return NExitCode::kFatalError;

CStdOutStream *g_StdStream = NULL;
CStdOutStream *g_ErrStream = NULL;

static void
FlushStreams()
{
    if (g_StdStream) g_StdStream->Flush();
}

static void
PrintError(const char *message)
{
    FlushStreams();
    if (g_ErrStream) *g_ErrStream << "\n\n" << message << endl;
}

using namespace NWindows;

// 简单的自旋锁类,防止exec_zmain1函数重入
class SpinLock {

public:
    SpinLock() : flag_(false)
    {}

    void lock()
    {
        bool expect = false;
        while (!flag_.compare_exchange_weak(expect, true))
        {
            expect = false;
        }
    }
    void unlock()
    {
        flag_.store(false);
    }

private:
    std::atomic<bool> flag_;
};

SpinLock fnSpinLock;

#ifdef __cplusplus
extern "C"
#endif
int MY_CDECL
exec_zmain2(int numArgs, WCHAR *args[])
{
    g_ErrStream = &g_StdErr;
    g_StdStream = &g_StdOut;

    NT_CHECK

    NConsoleClose::CCtrlHandlerSetter ctrlHandlerSetter;
    int res = 0;

    try
    {
#ifdef _WIN32
        My_SetDefaultDllDirectories();
#endif

        res = Internal_Main2(numArgs, args);
    }
    catch (const CNewException &)
    {
        PrintError(kMemoryExceptionMessage);
        return (NExitCode::kMemoryError);
    }
    catch (const NConsoleClose::CCtrlBreakException &)
    {
        PrintError(kUserBreakMessage);
        return (NExitCode::kUserBreak);
    }
    catch (const CMessagePathException &e)
    {
        PrintError(kException_CmdLine_Error_Message);
        if (g_ErrStream) *g_ErrStream << e << endl;
        return (NExitCode::kUserError);
    }
    catch (const CSystemException &systemError)
    {
        if (systemError.ErrorCode == E_OUTOFMEMORY)
        {
            PrintError(kMemoryExceptionMessage);
            return (NExitCode::kMemoryError);
        }
        if (systemError.ErrorCode == E_ABORT)
        {
            PrintError(kUserBreakMessage);
            return (NExitCode::kUserBreak);
        }
        if (g_ErrStream)
        {
            PrintError("System ERROR:");
            *g_ErrStream << NError::MyFormatMessage(systemError.ErrorCode) << endl;
        }
        return (NExitCode::kFatalError);
    }
    catch (NExitCode::EEnum &exitCode)
    {
        FlushStreams();
        if (g_ErrStream) *g_ErrStream << kInternalExceptionMessage << exitCode << endl;
        return (exitCode);
    }
    catch (const UString &s)
    {
        if (g_ErrStream)
        {
            PrintError(kExceptionErrorMessage);
            *g_ErrStream << s << endl;
        }
        return (NExitCode::kFatalError);
    }
    catch (const AString &s)
    {
        if (g_ErrStream)
        {
            PrintError(kExceptionErrorMessage);
            *g_ErrStream << s << endl;
        }
        return (NExitCode::kFatalError);
    }
    catch (const char *s)
    {
        if (g_ErrStream)
        {
            PrintError(kExceptionErrorMessage);
            *g_ErrStream << s << endl;
        }
        return (NExitCode::kFatalError);
    }
    catch (const wchar_t *s)
    {
        if (g_ErrStream)
        {
            PrintError(kExceptionErrorMessage);
            *g_ErrStream << s << endl;
        }
        return (NExitCode::kFatalError);
    }
    catch (int t)
    {
        if (g_ErrStream)
        {
            FlushStreams();
            *g_ErrStream << kInternalExceptionMessage << t << endl;
            return (NExitCode::kFatalError);
        }
    }
    catch (...)
    {
        PrintError(kUnknownExceptionMessage);
        return (NExitCode::kFatalError);
    }

    return res;
}

#ifdef __cplusplus
extern "C"
#endif
int MY_CDECL
exec_zmain1(LPCWSTR str)
{
    int     ret = -1;
    LPWSTR  *args = NULL;
    LPWSTR  lcmd = NULL;
    int     m_arg = 0;
    size_t  len = 0;
    CommandLineToArgvWPtr fnCommandLineToArgvW = NULL;
    HMODULE shell32 = NULL;
    if (!str || *str == 0)
    {
        return ret;
    }
    if ((shell32 = LoadLibraryW(L"shell32.dll")) == NULL)
    {
        return ret;
    }
    fnSpinLock.lock();
    do 
    {
        len = wcslen(str);
        len += MAX_PATH;
        fnCommandLineToArgvW = (CommandLineToArgvWPtr) GetProcAddress(shell32, "CommandLineToArgvW");
        if (fnCommandLineToArgvW == NULL)
        {
            break;
        }        
        lcmd = (WCHAR *)calloc(2, len);
        if ((lcmd = (WCHAR *)calloc(2, len)) == NULL)
        {
            break;
        }
        if (!GetModuleFileNameW(NULL,lcmd,len))
        {
            break;
        }
        wcsncat(lcmd, L" ", len); 
        wcsncat(lcmd, str, len);      
        args = fnCommandLineToArgvW((LPCWSTR)lcmd, &m_arg);
        if ( NULL != args )
        {
            ret = exec_zmain2(m_arg, args);
            LocalFree(args);
        }        
    }while(0);
    if (lcmd)
    {
        free(lcmd);
    }
    if (shell32)
    {
        FreeLibrary(shell32);
    }    
    fnSpinLock.unlock();
    return ret;
}

