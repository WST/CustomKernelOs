/* Filesystem checks run only under an isolated mkdtemp HOME. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include "tls.h"
#include "browser_profile.h"
static int failures;
#define CHECK(c) do {if(!(c)){fprintf(stderr,"profile FAIL line %d\n",__LINE__);failures++;}} while(0)
int main(void) {
    char directory[]="/tmp/myos-browser-profile-XXXXXX";CHECK(mkdtemp(directory));if(failures)return 1;
    setenv("HOME",directory,1);
    const char *origin="https://example.test",*data="{\"token\":\"saved\"}";char out[32768],path[900],folder[900],backup[920],outside[920];
    CHECK(profile_write(origin,data,strlen(data))==0);
    CHECK(profile_read(origin,out,sizeof(out))==(int)strlen(data) && !strcmp(out,data));
    CHECK(profile_read("http://example.test",out,sizeof(out))==0);
    CHECK(profile_read("https://example.test:8443",out,sizeof(out))==0);
    CHECK(profile_path(origin,path,sizeof(path))==1);
    snprintf(folder,sizeof(folder),"%s/.myos-browser",directory);struct stat st;
    CHECK(stat(folder,&st)==0 && (st.st_mode&0777)==0700);
    CHECK(stat(path,&st)==0 && (st.st_mode&0777)==0600);
    snprintf(backup,sizeof(backup),"%s.saved",path);CHECK(rename(path,backup)==0);
    snprintf(outside,sizeof(outside),"%s/outside",directory);FILE *f=fopen(outside,"wb");CHECK(f);if(f){fputs("untouched",f);fclose(f);}
    CHECK(symlink(outside,path)==0);CHECK(profile_read(origin,out,sizeof(out))<0);
    CHECK(profile_write(origin,data,strlen(data))==0);f=fopen(outside,"rb");CHECK(f);if(f){memset(out,0,sizeof(out));CHECK(fread(out,1,9,f)==9);fclose(f);CHECK(!strcmp(out,"untouched"));}
    f=fopen(path,"wb");CHECK(f);if(f){fputs("broken",f);fclose(f);}CHECK(profile_read(origin,out,sizeof(out))<0);
    CHECK(profile_write(origin,data,strlen(data))==0);CHECK(profile_read(origin,out,sizeof(out))>0);
    CHECK(chmod(folder,0755)==0);CHECK(profile_write(origin,"{}",2)<0);CHECK(profile_read(origin,out,sizeof(out))<0);CHECK(chmod(folder,0700)==0);
    CHECK(profile_write(origin,out,32768)<0);char huge[400];memset(huge,'x',399);huge[399]=0;CHECK(profile_write(huge,"{}",2)<0);
    unlink(path);unlink(backup);unlink(outside);rmdir(folder);rmdir(directory);
    printf("browser profile: %s\n",failures?"FAIL":"OK");return failures?1:0;
}
