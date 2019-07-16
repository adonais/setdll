#include "CPP/Common/Common.h"

#ifdef _WIN32
#include "C/DllSecur.h"
#endif

#include "CPP/Common/MyException.h"
#include "CPP/Common/StdOutStream.h"
#include "CPP/Windows/ErrorMsg.h"
#include "CPP/Windows/NtCheck.h"
#include "CPP/7zip/UI/Common/ArchiveCommandLine.h"
#include "CPP/7zip/UI/Common/ExitCode.h"
#include "CPP/7zip/UI/Console/ConsoleClose.h"

#ifdef _MSC_VER
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "advapi32.lib")
#endif

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

#ifdef __cplusplus
extern "C"
#endif
    int MY_CDECL
    exec_zmain(int numArgs, WCHAR *args[])
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
