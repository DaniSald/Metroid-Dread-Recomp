/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.txt and NOTICE.txt;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs. */
/* renderer=vulkan_native on Linux: the adapter between the SwitchRecomp runtime and
   ScarletNX's GPU front end + Vulkan renderer (runtime/snxvk, GPL-3.0, built into
   libsnxvk_linux.a with every symbol renamed snxv_*). This is the POSIX counterpart
   of the Windows half of snxvk_glue.c, kept separate so the Windows build is untouched.

   With it on, our nvdrv hands GPFIFO submissions to snxv_gpu_submit instead of our
   gpu.c, the vi present goes to the Vulkan renderer, mapping and memory changes are
   reported to its caches. Linux has no GetWriteWatch; buffer.c's content comparison
   path is used when plat_ww_scan returns -1. */
#define _GNU_SOURCE
#include "rt.h"
#include "gpu.h"
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <stdarg.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

int g_snxvk_want;
int g_snxvk_on;
int snxvk_dlss_mode;

char g_cachedir[1024];
int g_opt_headless;
int snxvk_opt_vsync = 1;

/* ---- host functions ScarletNX expects */
int pm_trylock(PMutex* m){ return pthread_mutex_trylock(&m->l) == 0 ? 1 : 0; }

typedef struct { const u8* data; u64 size; int borrowed; int heap; } PFileMap;

void plat_file_unmap(PFileMap* m);
int plat_file_map(PFileMap* m, const char* path){
  memset(m,0,sizeof *m);
  int fd=open(path,O_RDONLY); if(fd<0) return -1;
  struct stat st;
  if(fstat(fd,&st) || st.st_size<0){ close(fd); return -1; }
  if(st.st_size==0){ close(fd); return 0; }
  void* p=mmap(NULL,(size_t)st.st_size,PROT_READ,MAP_PRIVATE,fd,0);
  close(fd);
  if(p==MAP_FAILED) return -1;
  m->data=p; m->size=(u64)st.st_size; m->borrowed=1;
  return 0;
}
int plat_file_map_sized(PFileMap* m, const char* path, u64 size){
  if(plat_file_map(m,path)) return -1;
  if(m->size!=size){ plat_file_unmap(m); return -1; }
  return 0;
}
void plat_file_unmap(PFileMap* m){
  if(m->data){ if(m->borrowed) munmap((void*)m->data,(size_t)m->size); else free((void*)m->data); }
  memset(m,0,sizeof *m);
}
int plat_replace_file(const char* tmp, const char* dst){ return rename(tmp,dst); }

/* No userfaultfd write tracking in this build: -1 makes ScarletNX compare buffer contents. */
int plat_ww_scan(u8* p, u64 size, void (*cb)(u8* p, u64 size)){ (void)p;(void)size;(void)cb; return -1; }

int plat_setenv(const char* name, const char* value){ return setenv(name,value? value : "",1); }
const char* plat_dl_error(void){ const char* e=dlerror(); return e? e : "unknown error"; }
u32 plat_tid(void){ return (u32)syscall(SYS_gettid); }
int plat_cpus(int* ids, int max){
  cpu_set_t set;
  if(!sched_getaffinity(0,sizeof set,&set)){
    int n=0;
    for(int i=0;i<CPU_SETSIZE && n<max;i++) if(CPU_ISSET(i,&set)) ids[n++]=i;
    return n;
  }
  long n=sysconf(_SC_NPROCESSORS_ONLN); if(n<1) n=1; if(n>max) n=max;
  for(int i=0;i<n;i++) ids[i]=i;
  return (int)n;
}
const char* snx_build_commit(void){ return "b5f4f14f2bfc"; }
const char* snx_build_info(void){ return "ScarletNX renderer b5f4f14f2bfc in the SwitchRecomp Linux runtime"; }
char* snx_executable(const char* argv0){ (void)argv0; char b[1400]; snprintf(b,sizeof b,"%s%s",g_exedir,g_exename); return strdup(b); }
char* snx_format(const char* fmt, ...){
  va_list a; va_start(a,fmt); int n=vsnprintf(NULL,0,fmt,a); va_end(a); if(n<0) return NULL;
  char* p=malloc((size_t)n+1); if(!p) return NULL;
  va_start(a,fmt); vsnprintf(p,(size_t)n+1,fmt,a); va_end(a); return p;
}
char* snx_os_error(char* out, size_t size){
  const char* e=strerror(errno); snprintf(out,size,"%s",e? e : "unknown error");
  size_t n=strlen(out); while(n && (out[n-1]=='\r'||out[n-1]=='\n'||out[n-1]=='.')) out[--n]=0;
  return out;
}

/* ---- ScarletNX's own runtime hooks that this build doesn't provide */
typedef struct { u64 base, end, heap, heap_size, alias, alias_size, stack, stack_size, kern, kern_end; } SnxMemLayout;
SnxMemLayout g_layout={AS_BASE,AS_SIZE,HEAP_BASE,HEAP_SIZE,ALIAS_BASE,ALIAS_SIZE,STACK_BASE,STACK_SIZE,KERN_BASE,KERN_END};
int g_own_any;
int own_init(const void* hooks){ (void)hooks; return -1; }
int own_begin(u64 a, u64 n, void* r, u64 o){ (void)a;(void)n;(void)r;(void)o; return -1; }
void own_drop(u64 a, u64 n){ (void)a;(void)n; }
int own_hash(u64 a, u64 n, u64* h){ (void)a;(void)n;(void)h; return -1; }
void own_acquire(u64 a, u64 n){ (void)a;(void)n; }
u32 own_stage(u64 a, u64 n, u8* dst, void* out, u32 max){ (void)a;(void)n;(void)dst;(void)out;(void)max; return 0; }
u64 own_landings(void){ return 0; }
void syncpt_add_quiet(u32 id, u32 n){ extern void syncpt_incr(u32); while(n--) syncpt_incr(id); }
void syncpt_events(void){}
u64 kernel_find_symbol_exact(const char* name){ (void)name; return 0; }
void loader_segments(void* each, void* ctx){ (void)each;(void)ctx; }
int romfs_files(int (*each)(void*,const char*,u64,u64), void* ctx){
  extern int romfs_list(const char*, int (*)(void*,const char*,u64,u64), void*); return romfs_list("/system/shd/",each,ctx); }
int romfs_peek(u64 off, void* dst, u64 n){ return romfs_read(off,dst,n)? -1 : 0; }
int overlay_display_percent(void){ return 100; }
void prepare_screen_draw(void* l, int sw, int sh, const int safe[4], int pct){ (void)l;(void)sw;(void)sh;(void)safe;(void)pct; }
int prepare_screen_shown(void){ return 0; }
void vpad_showcase_draw(void* l, const int safe[4], int fps){ (void)l;(void)safe;(void)fps; }
int vpad_showcase_fps(u32 presents, u64 now){ (void)presents;(void)now; return 0; }
u64 unattended_ms(void){ return 0; }
int vic_gpu_aligned(const void* j){ (void)j; return 0; }
void vic_gpu_params(const void* j, void* p, const u32 in_base[16], u32 oc_base){ (void)j;(void)p;(void)in_base;(void)oc_base; }
void* vk_nvdec_hw_device(void){ return NULL; }
int vk_nvdec_write(const void* p){ (void)p; return 0; }

/* no on-screen controls or launcher overlay */
static const u32 g_atlas_px[1]={0};
const u32* ui_atlas(int* w, int* h, u32* version){ *w=1; *h=1; *version=1; return g_atlas_px; }
void ui_atlas_done(void){}
static PMutex g_ui_lk; static int g_ui_lk_init;
void ui_lock(void){ if(!g_ui_lk_init){ pm_init(&g_ui_lk); g_ui_lk_init=1; } pm_lock(&g_ui_lk); }
void ui_unlock(void){ pm_unlock(&g_ui_lk); }
void overlay_draw(void* l, int sw, int sh){ (void)l;(void)sw;(void)sh; }
void vpad_draw(void* l, const int safe[4]){ (void)l;(void)safe; }
int backend_hdr_peak_nits(void){ return 0; }   /* no DXGI on Linux: HDR output falls back to SDR */

/* ---- the ScarletNX side, renamed (same mirror as the Windows glue) */
typedef struct {
  const char* name; int (*start)(void); void (*mem_may_have_changed)(void);
  void* clear; void* draw; void* method_barrier; void* draw_texture; void* blit; void* vic_compose;
  void* nvdec_write; void* nvdec_hw_device; void* dma_copy; void* blit_mem;
  void (*rt_flush)(u64); void (*rt_reload)(u64); void* dispatch; void* cs_mem_need; void* mem_pending; void* gfx_mem_need;
  void* before_syncwait; void* stream_incr; void* release_write; void* release_need; void* release_read; void* release_value;
  void* render_predicate; void* render_predicate_drop;
  void* samples_reset; void* samples_enable; void* samples_report; void* stream_end; void* release_retire; void* report_stall;
  void (*present_buffer)(u64 cpu_addr, u32 w, u32 h, u32 pitch, u32 layout, u32 bhl, u32 fmt, const i32 crop[4], u32 transform);
  u64 (*screenshot)(const char* path); void (*refresh)(void); void (*save)(int at_exit); int (*release_window)(void);
  void* prepare;
} SnxRenderer;
extern const SnxRenderer snxv_vk_renderer;
extern void snxv_gpu_init(void);
extern void snxv_gpu_submit(const void* entries, u32 n, u32 channel, u32 incs, u32 wait_id, u32 wait_val, u32 expect);
extern void snxv_gpu_kick(void);
extern void snxv_gpu_channel_close(u32 channel);
extern void snxv_gpu_release_retire(u32 channel);
extern void snxv_gpu_mem_written(u64 gva, u64 size);
extern void snxv_gpu_mem_changed(u64 cpu_addr, u64 len);
extern void (*snxv_g_gpu_mapping_begin)(u64 gva, u64 size, int full);
extern void (*snxv_g_gpu_mapping_end)(void);
extern int snxv_g_renderer_up;

void snxvk_ini(const char* v){ g_snxvk_want = !strcmp(v,"vulkan_native"); }
void snxvk_gpu_init(void){ if(g_snxvk_want) snxv_gpu_init(); }

int snxvk_start(void){
  if(!g_snxvk_want) return 0;
  snprintf(g_cachedir,sizeof g_cachedir,"%svulkan_native/",g_exedir); plat_mkdir(g_cachedir);
  { extern int g_opt_vsync; snxvk_opt_vsync=g_opt_vsync; }
  { extern void snxv_swr_settings_set(float,int,int,int,int);
    extern float g_opt_resf; extern int g_opt_shadows,g_opt_aniso,g_opt_hdr;
    if(g_opt_hdr){ WARN("vulkan_native: HDR output is not ported to Linux; using SDR\n"); g_opt_hdr=0; }
    int rec= !(snxvk_dlss_mode>0 || g_opt_hdr) && !getenv("SWR_SNXVK_NO_RECORD_THREAD");
    snxv_swr_settings_set(g_opt_resf,g_opt_shadows,snxvk_opt_vsync,g_opt_aniso,rec);
    LOG("vulkan_native: command recording thread %s\n",rec? "on" : "off (HDR records directly)"); }
  { extern void snxv_swr_settings_prepare(int); extern int g_opt_prebuild;
    int prep=g_opt_prebuild && !getenv("SWR_VK_NO_PREPARE"); snxv_swr_settings_prepare(prep);
    if(prep) LOG("vulkan_native: the game's shaders are translated in the background, then the pipeline recipes made\n"); }
  if(!snxv_vk_renderer.start()){
    WARN("vulkan_native: Vulkan 1.3 renderer could not start (see above): using OpenGL\n"); return 0; }
  g_snxvk_on=1;
  LOG("renderer: native Vulkan (experimental, ScarletNX renderer; shaders translated to SPIR-V)\n");
  return 1;
}

void snxvk_submit(const void* entries, u32 n, u32 channel, u32 host_incs, u32 wait_id, u32 wait_val, u32 promised){
  snxv_gpu_submit(entries,n,channel,host_incs,wait_id,wait_val,promised); }
void snxvk_kick(void){ if(g_snxvk_on) snxv_gpu_kick(); }
void snxvk_channel_close(u32 id){ if(g_snxvk_on){ snxv_gpu_release_retire(id); snxv_gpu_channel_close(id); } }
volatile long long snxvk_gmmu_gen; volatile int snxvk_gmmu_busy;
void snxvk_map_begin(u64 gva, u64 size){ __atomic_add_fetch(&snxvk_gmmu_busy,1,__ATOMIC_RELAXED); __atomic_add_fetch(&snxvk_gmmu_gen,1,__ATOMIC_RELAXED); if(g_snxvk_on && snxv_g_gpu_mapping_begin) snxv_g_gpu_mapping_begin(gva,size,0); }
void snxvk_map_end(u64 gva, u64 size){ __atomic_add_fetch(&snxvk_gmmu_gen,1,__ATOMIC_RELAXED); __atomic_sub_fetch(&snxvk_gmmu_busy,1,__ATOMIC_RELAXED); if(!g_snxvk_on) return; snxv_gpu_mem_written(gva,size); if(snxv_g_gpu_mapping_end) snxv_g_gpu_mapping_end(); }
void snxvk_cpu_map_begin(u64 addr, u64 size){ if(g_snxvk_on && snxv_g_gpu_mapping_begin) snxv_g_gpu_mapping_begin(addr,size,1); }
void snxvk_cpu_map_end(u64 addr, u64 size, int changed){ if(!g_snxvk_on) return; if(snxv_g_gpu_mapping_end) snxv_g_gpu_mapping_end(); if(changed) snxv_gpu_mem_changed(addr,size); }

static void wait_deadline(u64 deadline){
  u64 now=plat_ns(); if(now>=deadline) return;
  if(deadline-now>500000ull) plat_sleep_ns((i64)(deadline-now-500000ull));
  while(plat_ns()<deadline) __builtin_ia32_pause(); }
int snxvk_pacing(void){ extern int g_opt_fps; return g_snxvk_on && !getenv("SWR_SNXVK_NO_PACING") && (g_opt_fps==1 || (g_opt_fps>=60 && g_opt_fps<=240)); }
void snxvk_present(u64 a, u32 w, u32 h, u32 pitch, u32 layout, u32 bhl, u32 fmt){
  static const i32 crop[4]={0,0,0,0};
  static u64 st_n,st_wait,st_pres,st_gap,last_out;
  u64 t_in=plat_ns(),t_w=t_in;
  if(snxvk_pacing()){ extern volatile int g_pace_fps; static u64 deadline; int pf=g_pace_fps;
    u64 per= pf>=30 && pf<=240? 1000000000ull/(u64)pf : 16666667ull;
    u64 next=deadline+per; if(deadline) wait_deadline(next);
    u64 now=plat_ns(); t_w=now; deadline= deadline && (i64)(now-next)<(i64)(per/2)? next : now; }
  { static u64 last_in; u64 t0=plat_ns(); snxv_vk_renderer.present_buffer(a,w,h,pitch,layout,bhl,fmt,crop,0); u64 t1=plat_ns();
    static int tr=-1; if(tr<0) tr=getenv("SWR_SNXVK_TRACE")!=NULL;
    if(t1-t0>250000000ull) LOG("vulkan_native: present_buffer took %.0f ms\n",(t1-t0)/1e6);
    if(tr && last_in && (t0-last_in>30000000ull || t1-t0>12000000ull)) LOG("vulkan_native: slow frame: game arrived %.1f ms after the last present, present took %.1f ms\n",(t0-last_in)/1e6,(t1-t0)/1e6);
    if(tr && last_out){ st_n++; st_wait+=t_w-t_in; st_pres+=t1-t0; st_gap+=t_in-last_out;
      if(st_n==600){ LOG("vulkan_native: per frame: game work %.2f ms, deadline wait %.2f ms, present call %.2f ms\n",st_gap/6e8,st_wait/6e8,st_pres/6e8); st_n=st_wait=st_pres=st_gap=0; } }
    last_out=t1; last_in=t1; } }
u64 snxvk_screenshot(const char* path){ return g_snxvk_on && snxv_vk_renderer.screenshot? snxv_vk_renderer.screenshot(path) : 0; }
void snxvk_tick(int tick){ if(!g_snxvk_on) return; if(tick%300==0 && snxv_vk_renderer.save) snxv_vk_renderer.save(0); }
void snxvk_exit(void){ if(g_snxvk_on && snxv_vk_renderer.save) snxv_vk_renderer.save(1); }
