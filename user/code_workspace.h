#define TREE_MAX 256
#define TREE_Y (34 + 52)
struct tree_node { char path[128]; int depth; bool directory, expanded; };
static struct tree_node tree[TREE_MAX];
static int tree_count, tree_top, tree_selected=-1, tree_pending=-1;
static char workspace[128]="/", folder_input[128];
static bool editing_folder;
static int build_pid=-1;
static char build_log[96], build_output[128];

static bool join_path(char *out, int size, const char *dir, const char *name) {
    if ((int)(strlen(dir)+strlen(name)+2)>size) return false;
    snprintf(out,size,!strcmp(dir,"/")?"%s%s":"%s/%s",dir,name); return true;
}
static void index_headers(const char *dir, const char *relative, int depth) {
    struct dirent_info ent;
    for(int i=0;dirread(dir,i,&ent)==1;i++) {
        if(ent.name[0]=='.') continue;
        char p[128], name[64];
        if(!join_path(p,sizeof(p),dir,ent.name)) continue;
        if(relative[0]) snprintf(name,sizeof(name),"%s/%s",relative,ent.name);
        else snprintf(name,sizeof(name),"%s",ent.name);
        if(ent.type==2) {if(depth<2) index_headers(p,name,depth+1);continue;}
        const char *ext=strrchr(ent.name,'.');
        if(!ext || strcmp(ext,".h") || ent.size>131072) continue;
        int fd=open(p,O_READ);if(fd<0) continue;
        char *s=malloc(ent.size+1);if(!s) {close(fd);return;}
        int total=0,n;
        while(total<(int)ent.size && (n=read(fd,s+total,ent.size-total))>0) total+=n;
        close(fd);s[total]=0;index_parse(s,name);free(s);
    }
}
static void tree_children(const char *dir,int at,int depth) {
    struct dirent_info ent;
    int start=at;
    for(int i=0;tree_count<TREE_MAX && dirread(dir,i,&ent)==1;i++) {
        if(!strcmp(ent.name,".") || !strcmp(ent.name,"..")) continue;
        struct tree_node node;memset(&node,0,sizeof(node));
        if(!join_path(node.path,sizeof(node.path),dir,ent.name)) continue;
        node.depth=depth;node.directory=ent.type==2;
        int pos=start;
        while(pos<at) {
            if(node.directory!=tree[pos].directory) {if(node.directory) break;}
            else if(strcmp(node.path,tree[pos].path)<0) break;
            pos++;
        }
        memmove(tree+pos+1,tree+pos,(tree_count-pos)*sizeof(tree[0]));
        tree[pos]=node;tree_count++;at++;
    }
    if(tree_count==TREE_MAX) strcpy(status_msg,"Explorer: 256 entries shown; collapse folders to see more");
}
static bool workspace_open(const char *p) {
    struct dirent_info st;
    if(dirstat(p,&st)<0 || st.type!=2) {strcpy(status_msg,"Cannot open workspace folder");return false;}
    char absolute[128];
    if(p[0]=='/') snprintf(absolute,sizeof(absolute),"%s",p);
    else {char cwd[128];getcwd(cwd,sizeof(cwd));if(!join_path(absolute,sizeof(absolute),cwd,p)) return false;}
    snprintf(workspace,sizeof(workspace),"%s",absolute);
    int n=strlen(workspace);while(n>1 && workspace[n-1]=='/') workspace[--n]=0;
    tree_count=tree_top=0;tree_selected=tree_pending=-1;
    tree_children(workspace,0,0);
    completion_hidden=true;
    return true;
}
static void folder_begin(void) {
    editing_folder=true;editing_name=false;
    snprintf(folder_input,sizeof(folder_input),"%s",workspace);
    strcpy(status_msg,"Folder path: Enter opens, Esc cancels");
}
static void folder_key(int k) {
    int n=strlen(folder_input);
    if(k==27) {editing_folder=false;return;}
    if(k=='\n' || k=='\r') {if(workspace_open(folder_input)) editing_folder=false;return;}
    if((k==8 || k==127) && n) {n--;while(n>0 && cont(folder_input[n])) n--;folder_input[n]=0;}
    else if(k>=32 && k<127 && n<123) {folder_input[n++]=k;folder_input[n]=0;}
    else if(k>=0xa0 && k<0x10000 && (k<KEY_UP || k>KEY_INS) && n<123) {
        n+=utf8_put(folder_input+n,k);folder_input[n]=0;
    }
}
static void tree_click(int x,int y) {
    (void)x;
    if(y< TREE_Y) { if(y>=34+28) folder_begin(); return; }
    int i=tree_top+(y-TREE_Y)/20;
    if(i<0 || i>=tree_count) return;
    tree_selected=i;
    if(tree[i].directory) {
        tree_pending=-1;
        if(tree[i].expanded) {
            int end=i+1;while(end<tree_count && tree[end].depth>tree[i].depth) end++;
            memmove(tree+i+1,tree+end,(tree_count-end)*sizeof(tree[0]));tree_count-=end-i-1;
            tree[i].expanded=false;
        } else {
            char dir[128];strcpy(dir,tree[i].path);
            tree_children(dir,i+1,tree[i].depth+1);tree[i].expanded=true;
        }
        return;
    }
    if(modified && tree_pending!=i) {
        tree_pending=i;strcpy(status_msg,"Unsaved! Save first or click this file again to discard");return;
    }
    int fd=open(tree[i].path,O_READ);
    if(fd<0) {strcpy(status_msg,"Cannot open selected file");return;} close(fd);
    if(strlen(tree[i].path)>=sizeof(path)) {strcpy(status_msg,"File path too long for editor");return;}
    if(!load(tree[i].path)) return;
    strcpy(path,tree[i].path);set_title();editing_name=false;tree_pending=-1;
    completion_hidden=true;
}
static void tree_draw(void) {
    gfx_fill_rect(0,34,SIDE_W,H-34-STATUS_H,RGB(37,37,38));
    gfx_text(12,34+8,"EXPLORER",CODE_FG,TRANSPARENT);
    gfx_text(174,34+8,"^  v",CODE_FG,TRANSPARENT);
    char label[32];snprintf(label,sizeof(label),"%.25s",workspace);
    gfx_text(12,34+30,label,CODE_TYPE,TRANSPARENT);
    int rows=(H-STATUS_H-TREE_Y)/20;
    if(tree_top>tree_count-rows) tree_top=tree_count-rows;
    if(tree_top<0) tree_top=0;
    for(int row=0;row<rows && tree_top+row<tree_count;row++) {
        int i=tree_top+row,y=TREE_Y+row*20;
        if(i==tree_selected) gfx_fill_rect(0,y,SIDE_W-1,20,RGB(55,55,61));
        const char *name=strrchr(tree[i].path,'/');name=name?name+1:tree[i].path;
        int x=10+tree[i].depth*12;if(x>SIDE_W-64) x=SIDE_W-64;
        gfx_text(x,y+2,tree[i].directory?(tree[i].expanded?"v":">"):" ",CODE_FG,TRANSPARENT);
        snprintf(label,sizeof(label),"%s",name);label[(SIDE_W-x-24)/8]=0;
        gfx_text(x+16,y+2,label,tree[i].directory?CODE_TYPE:CODE_FG,TRANSPARENT);
    }
}
static void build_start(void) {
    if(build_pid>0) return;
    if(!path[0]) {strcpy(status_msg,"Save the C source file before building");return;}
    const char *source_ext=strrchr(path,'.');
    if(!source_ext || strcmp(source_ext,".c")) {strcpy(status_msg,"Build requires a .c source file");return;}
    save();set_title();if(modified || editing_name) return;
    snprintf(build_output,sizeof(build_output),"%s",path);
    char *ext=strrchr(build_output,'.');if(ext) *ext=0;
    if(!strcmp(build_output,path)) {strcpy(status_msg,"Build requires a .c source file");return;}
    snprintf(build_log,sizeof(build_log),"/tmp/code-build-%d.log",getpid());
    int fd=open(build_log,O_WRITE);if(fd<0) {strcpy(status_msg,"Cannot create build log");return;}
    build_pid=fork();
    if(build_pid==0) {
        dup2(fd,STDOUT);dup2(fd,STDERR);close(fd);
        char *args[]={"cc",path,"-I",workspace,"-o",build_output,NULL};
        exec("/bin/cc",args);dprintf(STDERR,"Cannot start /bin/cc\n");exit(127);
    }
    close(fd);
    snprintf(status_msg,sizeof(status_msg),build_pid<0?"Cannot start compiler":"Building... /include + /lib");
}
static void build_poll(void) {
    if(build_pid<=0) return;
    int status;
    if(waitpid(build_pid,&status,WNOHANG)<=0) return;
    build_pid=-1;
    snprintf(status_msg,sizeof(status_msg),status?"Build failed: see %s":"Built %s",status?build_log:build_output);
    tree_count=tree_top=0;tree_selected=tree_pending=-1;tree_children(workspace,0,0);
}
