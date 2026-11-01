#include <client.h>
#include <errors.h>
#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
extern void gs_destroy(PSERVER_DATA server);
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL line %d: %s [%s]\n",__LINE__,#x,gs_error?gs_error:""); return 1; } } while(0)
int main(int argc,char **argv) {
    CHECK(argc==4); curl_global_init(CURL_GLOBAL_DEFAULT);
    int mode=atoi(argv[3]);
    for(int round=0;round<(mode==0?2:1);round++) {
        if(round) { char backup[4096];snprintf(backup,sizeof(backup),"%s.old",argv[2]); CHECK(rename(argv[2],backup)==0);CHECK(mkdir(argv[2],0700)==0); }
        SERVER_DATA server={0};
        CHECK(gs_init(&server,"127.0.0.1",atoi(argv[1]),argv[2],0,true)==GS_OK);
        CHECK(!server.paired);
        int ret=gs_pair(&server,"1234");
        if(mode) {
            CHECK(ret==GS_ERROR);
            CHECK(gs_error && strstr(gs_error,mode==1?"503":"409"));
            CHECK(strstr(gs_error,"PIN request"));
            CHECK(!strstr(gs_error,"cleanup") && !strstr(gs_error,"Certificate verification failed"));
            puts("PIN rejection: original status preserved, no stale TLS error: OK");
            gs_destroy(&server);break;
        }
        CHECK(ret==GS_OK && server.paired);
        PAPP_LIST list=NULL;CHECK(gs_applist(&server,&list)==GS_OK);CHECK(list && list->id==123);
        while(list){PAPP_LIST n=list->next;free(list->name);free(list);list=n;}
        gs_destroy(&server);
        CHECK(gs_init(&server,"127.0.0.1",atoi(argv[1]),argv[2],0,true)==GS_OK);
        CHECK(server.paired);gs_destroy(&server);
    }
    curl_global_cleanup();
    if(!mode) puts("Full cryptographic PIN exchange, exact TLS client certificate, reconnect and new identity after folder replacement: OK");
    return 0;
}
