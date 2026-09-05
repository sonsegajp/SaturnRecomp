#include <windows.h>
#include <wchar.h>
/* Per-title entry point. All hardware fixes stay in the shared runtime. */
int WINAPI wWinMain(HINSTANCE a,HINSTANCE b,PWSTR c,int d) {
    (void)a;(void)b;(void)c;(void)d;
    wchar_t folder[32768],runtime[32768],cmd[32768],settings[32768],absolute[32768],value[16];
    if (!GetModuleFileNameW(NULL,folder,32768)) return 1;
    wchar_t *slash=wcsrchr(folder,L'\\');if(!slash)return 1;*slash=0;
    swprintf(runtime,32768,L"%ls\\..\\..\\runtime\\saturnwin.exe",folder);
    swprintf(settings,32768,L"%ls\\..\\..\\settings.ini",folder);
    if(!GetFullPathNameW(settings,32768,absolute,NULL))return 1;
    /* A directly launched game must not inherit old tracing, input scripts,
       mute flags, or display overrides from a development shell. */
    LPWCH block=GetEnvironmentStringsW();
    if(block){for(wchar_t *p=block;*p;p+=wcslen(p)+1){
        if(wcsncmp(p,L"SATURN_",7)==0||wcsncmp(p,L"SDL_AUDIODRIVER=",16)==0){
            wchar_t key[512];wchar_t *eq=wcschr(p,L'=');
            if(eq&&(size_t)(eq-p)<512){wcsncpy(key,p,eq-p);key[eq-p]=0;SetEnvironmentVariableW(key,NULL);}
        }
    }FreeEnvironmentStringsW(block);}
    GetPrivateProfileStringW(L"Video",L"Interpolation",L"0",value,16,absolute);
    SetEnvironmentVariableW(L"SATURN_PRESENT_HZ",wcscmp(value,L"120")==0?L"120":NULL);
    SetEnvironmentVariableW(L"SATURN_SETTINGS_FILE",absolute);
    SetEnvironmentVariableW(L"SATURN_SMPCFILE",L"console.bin");
    if(GetFileAttributesW(runtime)==INVALID_FILE_ATTRIBUTES){MessageBoxW(NULL,L"The shared runtime is missing. Open SaturnRecomp to restore it.",L"SaturnRecomp",MB_ICONERROR);return 1;}
    if(!SetCurrentDirectoryW(folder))return 1;
    if(GetFileAttributesW(L"game.toml")==INVALID_FILE_ATTRIBUTES){MessageBoxW(NULL,L"This game's configuration is missing. Import the disc in SaturnRecomp.",L"SaturnRecomp",MB_ICONERROR);return 1;}
    swprintf(cmd,32768,L"\"%ls\" game.toml",runtime);
    SECURITY_ATTRIBUTES sa={sizeof sa,NULL,TRUE};
    HANDLE log=CreateFileW(L"runtime.log",GENERIC_WRITE,FILE_SHARE_READ,&sa,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,NULL);
    HANDLE input=CreateFileW(L"NUL",GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,&sa,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,NULL);
    STARTUPINFOW si={sizeof si};PROCESS_INFORMATION pi={0};
    if(log!=INVALID_HANDLE_VALUE&&input!=INVALID_HANDLE_VALUE){si.dwFlags=STARTF_USESTDHANDLES;si.hStdOutput=log;si.hStdError=log;si.hStdInput=input;}
    BOOL ok=CreateProcessW(runtime,cmd,NULL,NULL,TRUE,CREATE_NO_WINDOW,NULL,folder,&si,&pi);
    if(log!=INVALID_HANDLE_VALUE)CloseHandle(log);if(input!=INVALID_HANDLE_VALUE)CloseHandle(input);
    if(!ok){MessageBoxW(NULL,L"The game could not start. Open SaturnRecomp to restore its runtime.",L"SaturnRecomp",MB_ICONERROR);return 1;}
    CloseHandle(pi.hThread);WaitForSingleObject(pi.hProcess,INFINITE);
    DWORD code=1;GetExitCodeProcess(pi.hProcess,&code);CloseHandle(pi.hProcess);
    if(code)MessageBoxW(NULL,L"The game runtime stopped with an error. Its runtime.log contains the details.",L"SaturnRecomp",MB_ICONERROR);
    return (int)code;
}
