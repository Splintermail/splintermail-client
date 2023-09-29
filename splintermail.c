#include "libcli/libcli.h"

#ifndef _WIN32

int main(int argc, char* argv[]){
    return do_main(default_ui_harness(), argc, argv, false);
}

#else // _WIN32

VOID WINAPI SvcCtrlHandler( DWORD dwCtrl ){
    switch(dwCtrl){
        case SERVICE_CONTROL_STOP:
            // announce we are gonna try and stop
            ReportSvcStatus(SERVICE_STOP_PENDING, NO_ERROR, 0);
             // Signal the service to stop
            citm_stop_service();
            return;

        case SERVICE_CONTROL_INTERROGATE:
            break;

        default:
            break;
   }
}

static int do_main_ret = 0;

static VOID WINAPI SvcMain(int argc, char* argv[]){
    // Register the handler function for the service

    g_svc_status_h = RegisterServiceCtrlHandler(SVCNAME, SvcCtrlHandler);

    if( !g_svc_status_h )
    {
        do_main_ret = 254;
        // TODO: should we be reporting a failed service here?
        return;
    }

    // These SERVICE_STATUS members remain as set here
    g_svc_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    g_svc_status.dwServiceSpecificExitCode = 0;

    // Report initial status to the SCM
    ReportSvcStatus(SERVICE_START_PENDING, NO_ERROR, 3000);

    // when ditm_loop gets launched, it will set SERVICE_RUNNING
    do_main_ret = do_main(default_ui_harness(), argc, argv, true);

    // the SERVICE_STOPPED announcment MUST come before SvcMain exists, apparently
    // TODO: we should find a way to report any errors
    ReportSvcStatus(SERVICE_STOPPED, NO_ERROR, 0);
}

int main(int argc, char* argv[]){

    // list of functions to be dispatched
    SERVICE_TABLE_ENTRY DispatchTable[] = {
        { "", (LPSERVICE_MAIN_FUNCTION) SvcMain },
        { NULL, NULL } };

    if(!StartServiceCtrlDispatcher( DispatchTable )){
        // if running as a service failed, we are probably just a CLI program
        return do_main(default_ui_harness(), argc, argv, false);
    }
    return do_main_ret;
}

#endif
