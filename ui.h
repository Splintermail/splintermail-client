#ifndef UI_C
#define UI_C

#include "common.h"
#include "win_compat.h"

#ifdef _WIN32
// stuff for windows service to work
#define SVCNAME "splintermail"
extern SERVICE_STATUS g_svc_status;
extern SERVICE_STATUS_HANDLE g_svc_status_h;

VOID ReportSvcStatus(DWORD current_state, DWORD exit_code, DWORD wait_hint);
#endif // _WIN32

int do_main(int argc, char* argv[], bool windows_service);

#endif // UI_C
