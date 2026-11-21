
#include <assert.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>
#include <dirent.h>
#include <string.h>
#include <stdbool.h>
static unsigned stat_calls,unlink_calls,scan_opens,scan_index;
static size_t first_file_size=10;
static bool read_error,duplicate;
static DIR *fixture_open(const char *path) {
 assert(!strcmp(path,"/spiffs"));scan_opens++;scan_index=0;return (DIR *)&scan_index;
}
static struct dirent *fixture_read(DIR *dir) {
 assert(dir==(DIR *)&scan_index);
 static struct dirent entry;
 const char *names[]={"unrelated.txt","p4m00-0-s0.bin","p4m31-1-b1.tmp","p4m99-0-s0.bin"};
 if (scan_index==4) {if(read_error)errno=EIO;return NULL;}
 strcpy(entry.d_name,names[duplicate&&scan_index==2?1:scan_index]);scan_index++;
 return &entry;
}
static int fixture_close(DIR *dir){assert(dir==(DIR *)&scan_index);return 0;}
static int fixture_stat(const char *path,struct stat *out){
 stat_calls++;out->st_mode=0100000;
 if(!strcmp(path,"/spiffs/p4m00-0-s0.bin"))out->st_size=first_file_size;
 else{assert(!strcmp(path,"/spiffs/p4m31-1-b1.tmp"));out->st_size=10;}
 return 0;
}
static int missing_unlink(const char *path){(void)path;unlink_calls++;errno=ENOENT;return -1;}
#define stat(path,out) fixture_stat(path,out)
#define opendir fixture_open
#define readdir fixture_read
#define closedir fixture_close
#define unlink missing_unlink
#include "pet_p4_component_media.c"
#undef stat
#undef opendir
#undef readdir
#undef closedir
#undef unlink
int main(void){
 pet_cext_runtime_t empty={0};
 assert(pet_cmedia_persist(3,0,&empty));
 printf("ordinary component: stat=%u cleanup=%u\n",stat_calls,unlink_calls);
 assert(stat_calls==0&&scan_opens==0);
 assert(unlink_calls==2*(PET_SAMPLE_MAX_SLOTS+PET_CEXT_CLIP_MAX+PET_CEXT_BIG_MAX));
 /* Outstanding publication still rejects even a zero-media install. */
 g_pending.serial=1;unsigned prior=unlink_calls;
 assert(!pet_cmedia_persist(3,0,&empty)&&unlink_calls==prior);g_pending.serial=0;
 assert(!pet_cmedia_persist(PET_CEV_COMPONENTS,0,&empty));
 assert(!pet_cmedia_persist(3,2,&empty));
 /* A real sample must retain the existing full on-disk budget check. */
 uint8_t sample[]={'P','4','A','1',1,0,0,0,0,0};
 assert(pet_cmedia_stage_chunk(PET_CEXT_MEDIA_SAMPLE,"tone",0,sample,sizeof(sample),NULL,0));
 empty.sample_count=1;strcpy(empty.sample[0].id,"tone");
 stat_calls=0;
 assert(disk_budget_ok()&&stat_calls==2&&scan_opens==1);
 first_file_size=PET_CMEDIA_DEVICE_BUDGET_BYTES;assert(!disk_budget_ok());first_file_size=10;
 read_error=true;stat_calls=0;assert(!disk_budget_ok()&&!stat_calls);read_error=false;
 duplicate=true;assert(!disk_budget_ok()&&!stat_calls);duplicate=false;
 g_stage.used=PET_CMEDIA_DEVICE_BUDGET_BYTES+1u;stat_calls=0;
 assert(!disk_budget_ok()&&!stat_calls);g_stage.used=sizeof(sample);
 pet_cmedia_stage_reset();return 0;
}
