#include <assert.h>
#include<xkbcommon/xkbcommon.h>
#include "./wayland_client.h"
#include "./xdg-client.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include <errno.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

static void randname(char *buf) {
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  long r = ts.tv_nsec;
  for (int i = 0; i < 6; ++i) {
    buf[i] = 'A' + (r & 15) + (r & 16) * 2;
    r >>= 5;
  }
}

static int create_shm_file(void) {
  int retries = 100;
  do {
    char name[] = "/wl_shm-XXXXXX";
    randname(name + sizeof(name) - 7);
    --retries;
    int fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (fd >= 0) {
      shm_unlink(name);
      return fd;
    }
  } while (retries > 0 && errno == EEXIST);
  return -1;
}

int allocate_shm_file(size_t size) {
  int fd = create_shm_file();
  if (fd < 0)
    return -1;
  int ret;
  do {
    ret = ftruncate(fd, size);
  } while (ret < 0 && errno == EINTR);
  if (ret < 0) {
    close(fd);
    return -1;
  }
  return fd;
}

#define GRID_WIDTH 200
#define GRID_HEIGHT 200

struct state {
  struct wl_display *disp;
  struct wl_seat *seat;
  struct wl_compositor *compositor;
  struct wl_shm *shm;
  struct xdg_wm_base *xdg_base;

  struct wl_surface *surface;
  struct wl_keyboard *keyboard;
  struct xdg_surface *xdg_suf;
  struct xdg_toplevel *xdg_tp;

  struct xkb_context *xkb_context;
  struct xkb_keymap *xkb_keymap;
  struct xkb_state *xkb_state;
  int offset;
  uint32_t time;


  int grid[GRID_HEIGHT][GRID_WIDTH];
};


int grid_check_valid_position(int grid[GRID_HEIGHT][GRID_WIDTH],int x,int y){
  return x>=0&&y>=0&&x<GRID_HEIGHT&&y<GRID_WIDTH;
}

int grid_get_around(int grid[GRID_HEIGHT][GRID_WIDTH],int x,int y){
  int total = 0;
  for(int i =-1;i<=1;++i){
    for(int j =-1;j<=1;++j){
      int nowx = x+i;
      int nowy = y+j;
      if(grid_check_valid_position(grid,nowx,nowy)&&(nowx!=x||nowy!=y)&&grid[nowx][nowy]!=0){
        total++;
      }
    }
  }
  return total;
}

void grid_update(int grid[GRID_HEIGHT][GRID_WIDTH]){
  int tmp[GRID_HEIGHT][GRID_WIDTH];
  for(int i =0;i<GRID_HEIGHT;++i){
    for(int j =0;j<=GRID_WIDTH;++j){
      int around = grid_get_around(grid,i,j);
      if(grid[i][j]!=0){
        if(around<2||around>3) tmp[i][j] = 0;
        else tmp[i][j] = around;
      }else{
        if(around==3) tmp[i][j] = around;
        else tmp[i][j] = 0;
      }
    }
  }
  for(int i =0;i<GRID_HEIGHT;++i){
    for(int j =0;j<GRID_WIDTH;++j){
      grid[i][j] = tmp[i][j];
    }
  }
}


const int width = 700;
const int height = 700;
const int stride = width * 4;
const int shm_pool_size = height * stride;

void my_xdg_wm_base_ping_callback(void *data, struct xdg_wm_base *xdg_wm_base,
                                  uint32_t serial) {
  xdg_wm_base_pong(xdg_wm_base, serial);
}

struct wl_buffer_listener my_wl_buf_listener;

void wl_buf_release_handler(void *data, struct wl_buffer *wl_buffer) {
  wl_buffer_destroy(wl_buffer);
}

struct wl_buffer_listener my_wl_buf_listener = {.release =
  wl_buf_release_handler};

struct wl_buffer* draw_buffer_data(struct state *state) {
  int fd = allocate_shm_file(height*stride);
  uint32_t *data = (uint32_t *)mmap(0, height*stride, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);

  for (int i = 0; i < height; ++i) {
    for (int j = 0; j < width; ++j) {
      int map_grid_posx = i*GRID_HEIGHT/height;

      int map_grid_posy = j*GRID_WIDTH/width;
      if (state->grid[map_grid_posx][map_grid_posy]) {
        data[+i * width + j] = 0xAA000000+100*state->grid[map_grid_posx][map_grid_posy];
      } else {
        data[+i * width + j] = 0xAAFFFFFF;
      }
    }
  }
  struct wl_shm_pool *smp = wl_shm_create_pool(state->shm, fd, height*stride);
  struct wl_buffer *buf = wl_shm_pool_create_buffer(smp, 0,width,  height,  stride,  WL_SHM_FORMAT_XRGB8888);
  wl_buffer_add_listener(buf,&my_wl_buf_listener,state);
  munmap(data, height*stride);
  close(fd);
  return buf;
}

void my_xdg_surface_configure_callback(void *data, struct xdg_surface *xdg_surface,
                                       uint32_t serial) {
  struct state *my_state = data;
  xdg_surface_ack_configure(xdg_surface, serial);
  struct wl_buffer *buf = draw_buffer_data(my_state);
  wl_surface_attach(my_state->surface, buf, 0, 0); 
  // wl_surface_damage_buffer(my_state->surface, 0, 0, width, height);
  wl_surface_commit(my_state->surface);
}


void my_wl_keyboard_keymap_callback(void *data,struct wl_keyboard* wl_keyboard,
                                    uint32_t format,int32_t fd,uint32_t size){
  assert(format==WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1);
  struct state *my_state  = (struct state*)data;
  char *formate_data = (char *) mmap(NULL, size, PROT_READ, MAP_SHARED, fd, 0);
  xkb_keymap_unref(my_state->xkb_keymap);
  xkb_state_unref(my_state->xkb_state);
  my_state->xkb_keymap = xkb_keymap_new_from_string(my_state->xkb_context,formate_data,XKB_KEYMAP_FORMAT_TEXT_V1,XKB_KEYMAP_COMPILE_NO_FLAGS);
  my_state->xkb_state = xkb_state_new(my_state->xkb_keymap); 

  munmap(formate_data,size);
  close(fd);
}
void my_wl_keyboard_enter_callback(void *data,
                                   struct wl_keyboard *wl_keyboard,
                                   uint32_t serial,
                                   struct wl_surface *surface,
                                   struct wl_array *keys){
  struct state * my_state = (struct state *)data;
  int32_t *key; 
  wl_array_for_each(key,keys){
    char buf[128];
    xkb_keysym_t sym = xkb_state_key_get_one_sym(my_state->xkb_state, *key+8);
    xkb_keysym_get_name(sym, buf, sizeof(buf));
    fprintf(stderr,"sym %s %d\n",buf,sym);

  } 
}


void my_wl_keyboard_leave_callback(void *data,
                                   struct wl_keyboard *wl_keyboard,
                                   uint32_t serial,
                                   struct wl_surface *surface){

}

void my_wl_keyboard_key_callback(void *data,
		                             struct wl_keyboard *wl_keyboard,
		                             uint32_t serial,
		                             uint32_t time,
		                             uint32_t key,
		                             uint32_t state){

}
void my_wl_keyboard_modifiers_callback(void *data,
			            struct wl_keyboard *wl_keyboard,
			            uint32_t serial,
			            uint32_t mods_depressed,
			            uint32_t mods_latched,
			            uint32_t mods_locked,
			            uint32_t group){}

void my_wl_keyboard_repeat_info_callback(void *data,
			                                   struct wl_keyboard *wl_keyboard,
			                                   int32_t rate,
			                                   int32_t delay){}
struct wl_keyboard_listener my_wl_keyboard_listener = {
  .keymap = my_wl_keyboard_keymap_callback,
  .enter = my_wl_keyboard_enter_callback,
  .leave = my_wl_keyboard_leave_callback,
  .key = my_wl_keyboard_key_callback,
  .repeat_info = my_wl_keyboard_repeat_info_callback,
  .modifiers = my_wl_keyboard_modifiers_callback
};

void my_wl_seat_capability_callback(void *data,struct wl_seat *wl_seat,uint32_t capability){
  struct state *my_state = (struct state *) data;
  if((capability&WL_SEAT_CAPABILITY_KEYBOARD)!=0&&my_state->keyboard==NULL){
    my_state->keyboard = wl_seat_get_keyboard(wl_seat);
    wl_keyboard_add_listener(my_state->keyboard,&my_wl_keyboard_listener,data);
  }  
}

void my_wl_seat_name_callback(void *data,struct wl_seat* wl_seat,const char *name){
}

struct xdg_wm_base_listener xwb_listener = {.ping = my_xdg_wm_base_ping_callback};

struct xdg_surface_listener xs_listener = {.configure =
  my_xdg_surface_configure_callback};

struct wl_seat_listener my_wl_seat_listener = {.capabilities = my_wl_seat_capability_callback,.name = my_wl_seat_name_callback};

void reg_global_callback(void *data, struct wl_registry *reg, uint32_t name,
                         const char *interface, uint32_t version) {
  struct state *my_state = (struct state *)data;
  if (strcmp(wl_compositor_interface.name, interface) == 0) {
    fflush(stdout);
    my_state->compositor = (struct wl_compositor *)wl_registry_bind(
      reg, name, &wl_compositor_interface, version);
  }else if (strcmp(wl_shm_interface.name, interface) == 0) {
    my_state->shm = (struct wl_shm *)wl_registry_bind(
      reg, name, &wl_shm_interface, version);
  }else if (strcmp(xdg_wm_base_interface.name, interface) == 0) {
    my_state->xdg_base = (struct xdg_wm_base *)wl_registry_bind(
      reg, name, &xdg_wm_base_interface, version);
    xdg_wm_base_add_listener(my_state->xdg_base, &xwb_listener, &my_state);
  }else if(strcmp(wl_seat_interface.name,interface) == 0){
    my_state->seat = (struct wl_seat *) wl_registry_bind(reg,name ,&wl_seat_interface,version);
    wl_seat_add_listener(my_state->seat,&my_wl_seat_listener,my_state);
  }
  return;
}

void reg_global_remove_handle(void *data, struct wl_registry *reg,
                              uint32_t name) {}

struct wl_registry_listener my_reg_handle = {
  .global = reg_global_callback, .global_remove = reg_global_remove_handle};

struct wl_callback_listener my_cb_handler;



void my_done(void *data, struct wl_callback *wl_callback,
             uint32_t callback_data) {
  wl_callback_destroy(wl_callback);
  struct state *my_state = (struct state *)data;
  struct wl_callback *cb = wl_surface_frame(my_state->surface);
  wl_callback_add_listener(cb, &my_cb_handler, my_state);

  my_state->offset += callback_data - my_state->time;
  my_state->time = callback_data;

}

int main() {
  struct state my_state = {0};
  struct wl_display *disp = wl_display_connect(NULL);


  struct wl_registry *reg = wl_display_get_registry(disp);
  wl_registry_add_listener(reg, &my_reg_handle, &my_state);
  wl_display_roundtrip(disp);

  my_state.surface = wl_compositor_create_surface(my_state.compositor);

  my_state.xdg_suf =
    xdg_wm_base_get_xdg_surface(my_state.xdg_base, my_state.surface);
  xdg_surface_add_listener(my_state.xdg_suf, &xs_listener, &my_state);
  my_state.xdg_tp = xdg_surface_get_toplevel(my_state.xdg_suf);
  xdg_toplevel_set_title(my_state.xdg_tp, "my wayland client");
  wl_surface_commit(my_state.surface);
  my_cb_handler.done = my_done;
  for(int i =0;i<GRID_HEIGHT;++i){
    for(int j =0;j<GRID_WIDTH;++j){
      my_state.grid[i][j]=rand()%2;
    }
  }


  // xkb init
  my_state.xkb_context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);

  // surface frame callback 
  struct wl_callback *cb = wl_surface_frame(my_state.surface);
  wl_callback_add_listener(cb, &my_cb_handler, &my_state);

  while (wl_display_dispatch(disp) != -1) {
    my_state.offset=0;
    grid_update(my_state.grid);
    struct wl_buffer *buf = draw_buffer_data(&my_state);
    wl_surface_attach(my_state.surface, buf, 0, 0);
    wl_surface_damage_buffer(my_state.surface, 0, 0, width,height);
    wl_surface_commit(my_state.surface);
    usleep(100000);
  }
  return 0;
}
