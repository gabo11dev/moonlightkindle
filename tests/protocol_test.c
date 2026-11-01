#include <client.h>
#include <errors.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <curl/curl.h>
extern void gs_destroy(PSERVER_DATA server);
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL line %d: %s (%s)\n",__LINE__,#x,gs_error?gs_error:""); return 1; } } while(0)
int main(int argc, char **argv) {
    CHECK(argc == 3); curl_global_init(CURL_GLOBAL_DEFAULT);
    for (int i=0; i<3; i++) {
        SERVER_DATA server={0};
        CHECK(gs_init(&server,"127.0.0.1",atoi(argv[1]),argv[2],0,true)==GS_OK);
        CHECK(server.paired && server.modes && server.modes->width==640);
        PAPP_LIST list=NULL; CHECK(gs_applist(&server,&list)==GS_OK);
        CHECK(list && list->id==123 && !strcmp(list->name,"Desktop"));
        STREAM_CONFIGURATION cfg; LiInitializeStreamConfiguration(&cfg);
        const int widths[]={640,1448,800}, heights[]={360,1072,600}, fps[]={10,1,30};
        cfg.width=widths[i]; cfg.height=heights[i]; cfg.fps=fps[i]; cfg.bitrate=1000;
        cfg.supportedVideoFormats=VIDEO_FORMAT_H264; cfg.audioConfiguration=AUDIO_CONFIGURATION_STEREO;
        CHECK(gs_start_app(&server,&cfg,123,false,true,0)==GS_ERROR);
        while (list) { PAPP_LIST next=list->next; free(list->name); free(list); list=next; }
        gs_destroy(&server);
    }
    curl_global_cleanup(); puts("HTTP/TLS, custom modes, 1 FPS, credentials, apps and repeated sessions: OK"); return 0;
}
