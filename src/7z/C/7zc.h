#ifndef __7ZC_AR_H__
#define __7ZC_AR_H__

#ifdef __cplusplus
extern "C" {
#endif

extern int __cdecl exec_zmain1(const wchar_t *args);
extern int __cdecl exec_zmain2(int numArgs, wchar_t *args[]);
extern bool __stdcall get_process_name(char *path, int len);

#ifdef __cplusplus
}
#endif

#endif  // __7ZC_AR_H__
