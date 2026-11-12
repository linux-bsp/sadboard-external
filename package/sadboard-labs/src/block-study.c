#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <linux/dm-ioctl.h>
#include <linux/fs.h>
#include <linux/loop.h>
#include <sched.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/sysmacros.h>
#include <time.h>
#include <unistd.h>
/* All media are created here from RAM. No caller supplied block path. */
static char base[] = "/tmp/sadboard-block-XXXXXX", mountpoint[256], backing[256], node[256], filename[256], dmname[128];
static int ctl=-1, loopfd=-1, mapfd=-1, filefd=-1, backingfd=-1;
static bool map_created, loop_set, mounted, table_error, ram_mounted;
static FILE *events;
static volatile sig_atomic_t interrupted;
static unsigned event_count, kind, fixed, attempts, errors, error_no, committed, sector_size;
static uint64_t requested, completed, checksum;
static void event(const char *name, int64_t value)
{
    int saved = errno;
    struct timespec ts;clock_gettime(CLOCK_MONOTONIC,&ts);
    fprintf(events,"%u,%" PRIu64 ",%s,%" PRId64 "\n",event_count++,(uint64_t)ts.tv_sec*1000000000+(uint64_t)ts.tv_nsec,name,value);fflush(events);
    errno = saved;
}
static int dm(unsigned long command,unsigned flags,const char *target)
{
    union { struct dm_ioctl header; unsigned char bytes[4096]; } b;
    memset(&b,0,sizeof(b));struct dm_ioctl *h=&b.header;
    h->version[0]=DM_VERSION_MAJOR;h->version[1]=DM_VERSION_MINOR;h->version[2]=DM_VERSION_PATCHLEVEL;
    h->data_size=sizeof(b);h->data_start=sizeof(*h);h->flags=flags;
    snprintf(h->name,sizeof(h->name),"%s",dmname);
    if(target){struct dm_target_spec *s=(void*)(b.bytes+h->data_start);s->length=32768;
        snprintf(s->target_type,sizeof(s->target_type),"%s",target);
        char *p=(char*)(s+1);if(!strcmp(target,"linear")){struct stat st;if(fstat(loopfd,&st))return -1;snprintf(p,128,"%u:%u 0",major(st.st_rdev),minor(st.st_rdev));}
        s->next=(sizeof(*s)+strlen(p)+1+7)&~7U;h->target_count=1;}
    if(ioctl(ctl,command,h)<0)return -1;
    if(command==DM_DEV_CREATE){map_created=true;if(mknod(node,S_IFBLK|0600,(dev_t)h->dev))return -1;}
    return 0;
}
static int replace_table(bool fault)
{
    if(dm(DM_DEV_SUSPEND,DM_SUSPEND_FLAG|DM_SKIP_LOCKFS_FLAG,NULL))return -1;
    if(dm(DM_TABLE_LOAD,0,fault?"error":"linear"))return -1;
    if(dm(DM_DEV_SUSPEND,0,NULL))return -1;
    table_error=fault;event(fault?"map-error":"map-linear",1);return 0;
}
static int cleanup(void)
{
    int failed=0;
    if(table_error&&replace_table(false))failed=1;
    if(filefd>=0){close(filefd);filefd=-1;}
    if(mounted){if(umount(mountpoint)){fprintf(stderr,"block-study: inner unmount: %s\n",strerror(errno));failed=1;}else mounted=false;}
    if(mapfd>=0){close(mapfd);mapfd=-1;}
    if(map_created){
        /* The deferred removal request also handles SIGKILL/namespace teardown. */
        for(unsigned i=0;i<100;i++){
            if(!dm(DM_DEV_REMOVE,DM_DEFERRED_REMOVE,NULL)||errno==ENXIO||errno==ENODEV){
                if(dm(DM_DEV_STATUS,0,NULL)<0&&(errno==ENXIO||errno==ENODEV)){map_created=false;break;}
            }
            usleep(10000);
        }
        if(map_created){fprintf(stderr,"block-study: DM removal: %s\n",strerror(errno));failed=1;}
    }
    if(loopfd>=0){
        if(loop_set){if(ioctl(loopfd,LOOP_CLR_FD,0)&&errno!=ENXIO){fprintf(stderr,"block-study: loop cleanup: %s\n",strerror(errno));failed=1;}else loop_set=false;}
        close(loopfd);loopfd=-1;
    }
    if(backingfd>=0){close(backingfd);backingfd=-1;}
    if(ctl>=0){close(ctl);ctl=-1;}
    unlink(filename);unlink(node);unlink(backing);rmdir(mountpoint);
    if(ram_mounted){
        for(unsigned i=0;i<200;i++){if(!umount(base)){ram_mounted=false;break;}if(errno!=EBUSY)break;usleep(10000);}
        if(ram_mounted){fprintf(stderr,"block-study: RAM mount cleanup: %s\n",strerror(errno));failed=1;}
    }
    rmdir(base);
    event("cleanup",failed);return failed;
}
static void die(const char *what){int saved=errno;fprintf(stderr,"block-study: %s: %s\n",what,strerror(saved));cleanup();exit(1);}
static void terminate(int sig){interrupted=sig;}
static void readback(const char *csv, const void *data, size_t size)
{
    char path[4096]; if (snprintf(path,sizeof(path),"%s.bin",csv)>=(int)sizeof(path)) {errno=ENAMETOOLONG;die("readback path");}
    FILE *f=fopen(path,"wx");if(!f||fwrite(data,1,size,f)!=size||fclose(f))die("readback evidence");
}
static void write_all(int fd,const void *data,size_t n){const char *p=data;while(n){if(interrupted){errno=EINTR;die("cancelled");}ssize_t r=write(fd,p,n);if(r<=0)die("fixture copy");p+=r;n-=r;}}
static void create_backend(const char *fixture,unsigned mapping)
{
    int source=open(fixture,O_RDONLY|O_NOFOLLOW);struct stat st;
    if(source<0||fstat(source,&st)||!S_ISREG(st.st_mode)||st.st_size!=16*1024*1024)die("16MiB fixture");
    backingfd=open(backing,O_CREAT|O_EXCL|O_RDWR,0600);if(backingfd<0)die("RAM backing");
    char buffer[65536];ssize_t n;while((n=read(source,buffer,sizeof(buffer)))>0)write_all(backingfd,buffer,n);close(source);if(n<0)die("fixture read");
    int control=open("/dev/loop-control",O_RDWR);if(control<0)die("loop control");int number=ioctl(control,LOOP_CTL_GET_FREE);close(control);if(number<0)die("loop allocation");
    char lp[64];snprintf(lp,sizeof(lp),"/dev/loop%d",number);loopfd=open(lp,O_RDWR);if(loopfd<0||ioctl(loopfd,LOOP_SET_FD,backingfd))die("owned loop setup");loop_set=true;
    struct loop_info64 info={.lo_flags=LO_FLAGS_AUTOCLEAR};snprintf((char*)info.lo_file_name,LO_NAME_SIZE,"%.63s",backing);
    if(ioctl(loopfd,LOOP_SET_STATUS64,&info)||ioctl(loopfd,BLKSSZGET,&sector_size))die("loop status");
    event("loop",number);
    if(mapping){ctl=open("/dev/mapper/control",O_RDWR);if(ctl<0)die("DM control");
        snprintf(dmname,sizeof(dmname),"sadboard-%s",strrchr(base,'/')+1);if(dm(DM_DEV_CREATE,0,NULL))die("own DM create");map_created=true;
        if(dm(DM_TABLE_LOAD,0,"linear")||dm(DM_DEV_SUSPEND,0,NULL))die("own DM linear table");
        mapfd=open(node,O_RDWR);if(mapfd<0)die("own DM open");
        if(dm(DM_DEV_REMOVE,DM_DEFERRED_REMOVE,NULL))die("own DM deferred cleanup");
        if(mapping==1){ if(mount(node,mountpoint,"ext4",MS_NOSUID|MS_NODEV,"errors=continue"))die("private ext4 mount");mounted=true;event("mount",1); }
    }
}
int main(int argc,char **argv)
{
    if(argc!=5||strlen(argv[1])!=1||argv[1][0]<'0'||argv[1][0]>'4'||(strcmp(argv[2],"bug")&&strcmp(argv[2],"fixed")))return 64;
    kind=argv[1][0]-'0';fixed=!strcmp(argv[2],"fixed");events=fopen(argv[3],"wx");if(!events)return 1;
    fputs("index,ns,event,value\n",events);event("start",kind);
    signal(SIGTERM,terminate);signal(SIGINT,terminate);
    if(unshare(CLONE_NEWNS)||mount(NULL,"/",NULL,MS_REC|MS_PRIVATE,NULL))die("private mount namespace");
    struct statfs fs;if(statfs("/tmp",&fs)||fs.f_type!=0x01021994)die("/tmp must be tmpfs");
    if(!mkdtemp(base))die("private directory");
    char record[4096];if(snprintf(record,sizeof(record),"%s.resources",argv[3])>=(int)sizeof(record)){errno=ENAMETOOLONG;die("resource record");}
    FILE *resource=fopen(record,"wx");if(!resource||fprintf(resource,"%s\n",base)<0||fclose(resource))die("resource record");
    if(mount("sadboard-backend",base,"tmpfs",MS_NOSUID,"size=33554432,mode=0700"))die("private RAM backend");
    ram_mounted=true;
    snprintf(mountpoint,sizeof(mountpoint),"%s/mnt",base);snprintf(backing,sizeof(backing),"%s/backing.img",base);
    snprintf(node,sizeof(node),"%s/block",base);snprintf(filename,sizeof(filename),"%s/mnt/payload",base);if(mkdir(mountpoint,0700))die("mount directory");
    void *data=NULL;if(posix_memalign(&data,4096,65536)){errno=ENOMEM;die("buffer");}memset(data,0x5a,65536);
    if(kind==0){
        if(mount("sadboard-space",mountpoint,"tmpfs",MS_NOSUID|MS_NODEV,"size=1048576,nr_inodes=32,mode=0700"))die("private capped tmpfs");
        mounted=true;event("mount",1);
        filefd=open(filename,O_CREAT|O_EXCL|O_RDWR,0600);if(filefd<0)die("payload");requested=2*1024*1024;
        while(completed<requested){if(interrupted){errno=EINTR;die("cancelled");}attempts++;ssize_t n=write(filefd,data,65536);event("write",n);if(n<0){errors++;error_no=errno;event("errno",errno);break;}if(!n)die("zero write");completed+=n;}
        committed=fixed?(completed==requested):1;
    } else {
        create_backend(argv[4],(kind==1||kind==2)?1:(kind==4?2:0));
        if(kind==1){
            filefd=open(filename,O_CREAT|O_EXCL|O_RDWR,0600);if(filefd<0)die("payload");requested=65536;attempts=1;
            ssize_t n=write(filefd,data,requested);event("write",n);if(n!=65536)die("cached write");completed=n;
            if(replace_table(true))die("inject EIO");
            int r=fsync(filefd);event("fsync",r);if(r<0){errors++;error_no=errno;event("errno",errno);}else{errno=EPROTO;die("fsync unexpectedly succeeded");}
            committed=!fixed;
        }else if(kind==2){
            filefd=open(filename,O_CREAT|O_EXCL|O_RDWR|O_DIRECT,0600);if(filefd<0)die("direct file");requested=4096;attempts=1;
            ssize_t n=pwrite(filefd,fixed?data:(char*)data+1,requested,fixed?0:1);event("pwrite",n);if(n<0){errors++;error_no=errno;event("errno",errno);}else{completed=n;committed=1;if(fsync(filefd))die("direct fsync");memset(data,0,4096);if(pread(filefd,data,4096,0)!=4096)die("direct read");for(unsigned i=0;i<4096;i++)checksum+=((unsigned char*)data)[i];readback(argv[3],data,4096);}
        }else if(kind==3){
            if(pwrite(loopfd,data,4096,4096)!=4096||fsync(loopfd))die("offset fixture");
            requested=512;attempts=1;
            memset(data,0,4096);off_t offset=fixed?8*512:8*64;ssize_t n=pread(loopfd,data,512,offset);event("offset",offset);event("pread",n);if(n!=512)die("offset read");completed=n;
            for(unsigned i=0;i<512;i++)checksum+=((unsigned char*)data)[i];
            readback(argv[3],data,512);committed=checksum==512*0x5a;
        }else{
            /* All reads hit only our own DM error target. */
            if(replace_table(true))die("inject retry EIO");
            requested=4096;for(unsigned i=0;i<(fixed?1U:16U);i++){attempts++;ssize_t n=pread(mapfd,data,4096,0);event("pread",n);if(n<0){errors++;error_no=errno;event("errno",errno);}else{errno=EPROTO;die("error target read unexpectedly succeeded");}}
        }
    }
    event("commit",committed);free(data);if(cleanup())return 1;
    printf("{\"kind\":%u,\"fixed\":%u,\"attempts\":%u,\"errors\":%u,\"error_no\":%u,\"requested\":%" PRIu64 ",\"completed\":%" PRIu64 ",\"committed\":%u,\"checksum\":%" PRIu64 ",\"sector_size\":%u,\"maps\":%u,\"loops\":%u,\"mounts\":%u,\"backing_tmpfs\":1,\"cleanup\":1}\n",kind,fixed,attempts,errors,error_no,requested,completed,committed,checksum,sector_size,map_created,loop_set,mounted);
    fclose(events);return 0;
}
