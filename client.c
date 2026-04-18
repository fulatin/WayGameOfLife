#include "./wayland_client.h"
#include "./xdg-client.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <wayland-client-core.h>
#include <xkbcommon/xkbcommon.h>

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

#define GRID_WIDTH 100
#define GRID_HEIGHT 100

struct state {
  struct wl_display *disp;
  struct wl_seat *seat;
  struct wl_compositor *compositor;
  struct wl_shm *shm;
  struct wl_shm_pool *shm_pool;
  struct xdg_wm_base *xdg_base;
  struct xdg_toplevel *tp_surf;
  struct wl_surface *surface;
  struct wl_keyboard *keyboard;
  struct xdg_surface *xdg_suf;
  struct xdg_toplevel *xdg_tp;

  struct xkb_context *xkb_context;
  struct xkb_keymap *xkb_keymap;
  struct xkb_state *xkb_state;
  int offset;
  uint32_t last_time;
  int frame_rate;
  int sugg_width, sugg_heigth;
  int grid[GRID_HEIGHT][GRID_WIDTH];
  int configured;
  int pool_size;
  // uint32_t *buffer;
  // int buffer_fd;
};

int grid_check_valid_position(int grid[GRID_HEIGHT][GRID_WIDTH], int x, int y) {
  return x >= 0 && y >= 0 && x < GRID_HEIGHT && y < GRID_WIDTH;
}

int grid_get_around(int grid[GRID_HEIGHT][GRID_WIDTH], int x, int y) {
  int total = 0;
  for (int i = -1; i <= 1; ++i) {
    for (int j = -1; j <= 1; ++j) {
      int nowx = x + i;
      int nowy = y + j;
      if (grid_check_valid_position(grid, nowx, nowy) &&
          (nowx != x || nowy != y) && grid[nowx][nowy] != 0) {
        total++;
      }
    }
  }
  return total;
}

void grid_update(int grid[GRID_HEIGHT][GRID_WIDTH]) {
  int tmp[GRID_HEIGHT][GRID_WIDTH];
  for (int i = 0; i < GRID_HEIGHT; ++i) {
    for (int j = 0; j < GRID_WIDTH; ++j) {
      int around = grid_get_around(grid, i, j);
      if (grid[i][j] != 0) {
        if (around < 2 || around > 3)
          tmp[i][j] = 0;
        else
          tmp[i][j] = around;
      } else {
        if (around == 3)
          tmp[i][j] = around;
        else
          tmp[i][j] = 0;
      }
    }
  }
  for (int i = 0; i < GRID_HEIGHT; ++i) {
    for (int j = 0; j < GRID_WIDTH; ++j) {
      grid[i][j] = tmp[i][j];
    }
  }
}

int width = 700;
int height = 700;
#define STRIDE(width) (width * 4)

#define SHM_POLL_SIZE(height, stride) = height * stride;

void my_xdg_wm_base_ping_callback(void *data, struct xdg_wm_base *xdg_wm_base,
                                  uint32_t serial) {
  xdg_wm_base_pong(xdg_wm_base, serial);
  printf("get a ping\n");
}

struct wl_buffer_listener my_wl_buf_listener;

void wl_buf_release_handler(void *data, struct wl_buffer *wl_buffer) {
  wl_buffer_destroy(wl_buffer);
}

struct wl_buffer_listener my_wl_buf_listener = {.release =
                                                    wl_buf_release_handler};

struct wl_buffer *draw_buffer_data(struct state *state) {
  int fd = allocate_shm_file(height * STRIDE(width));
  uint32_t *data = (uint32_t *)mmap(0, height * STRIDE(width),
                                    PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);

  for (int i = 0; i < height; ++i) {
    for (int j = 0; j < width; ++j) {
      int map_grid_posx = i * GRID_HEIGHT / height;

      int map_grid_posy = j * GRID_WIDTH / width;
      if (state->grid[map_grid_posx][map_grid_posy]) {
        data[+i * width + j] =
            0xAA000000 + 1000 * state->grid[map_grid_posx][map_grid_posy];
      } else {
        data[+i * width + j] = 0xAAFFFFFF;
      }
    }
  }
  struct wl_shm_pool *smp =
      wl_shm_create_pool(state->shm, fd, height * STRIDE(width));
  struct wl_buffer *buf = wl_shm_pool_create_buffer(
      smp, 0, width, height, STRIDE(width), WL_SHM_FORMAT_XRGB8888);
  wl_buffer_add_listener(buf, &my_wl_buf_listener, state);
  munmap(data, height * STRIDE(width));
  close(fd);
  return buf;
}
struct wl_buffer *create_buffer(struct state *state) {
  struct wl_shm_pool *smp = state->shm_pool;
  struct wl_buffer *buf = wl_shm_pool_create_buffer(
      smp, 0, width, height, STRIDE(width), WL_SHM_FORMAT_XRGB8888);
  wl_buffer_set_user_data(buf, (void *)1);
  wl_buffer_add_listener(buf, &my_wl_buf_listener, state);
  return buf;
}
void update_buffer(struct state *state) {
  uint32_t *data = wl_shm_pool_get_user_data(state->shm_pool);
  for (int i = 0; i < height; ++i) {
    for (int j = 0; j < width; ++j) {
      int map_grid_posx = i * GRID_HEIGHT / height;

      int map_grid_posy = j * GRID_WIDTH / width;
      if (state->grid[map_grid_posx][map_grid_posy]) {
        data[i * width + j] =
            0xAA000000 + 1000 * state->grid[map_grid_posx][map_grid_posy];
      } else {
        data[i * width + j] = 0xAAFFFFFF;
      }
    }
  }
}

void my_wl_keyboard_keymap_callback(void *data, struct wl_keyboard *wl_keyboard,
                                    uint32_t format, int32_t fd,
                                    uint32_t size) {
  assert(format == WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1);
  struct state *my_state = (struct state *)data;
  char *formate_data = (char *)mmap(NULL, size, PROT_READ, MAP_SHARED, fd, 0);
  xkb_keymap_unref(my_state->xkb_keymap);
  xkb_state_unref(my_state->xkb_state);
  my_state->xkb_keymap = xkb_keymap_new_from_string(
      my_state->xkb_context, formate_data, XKB_KEYMAP_FORMAT_TEXT_V1,
      XKB_KEYMAP_COMPILE_NO_FLAGS);
  my_state->xkb_state = xkb_state_new(my_state->xkb_keymap);

  munmap(formate_data, size);
  close(fd);
}
void my_wl_keyboard_enter_callback(void *data, struct wl_keyboard *wl_keyboard,
                                   uint32_t serial, struct wl_surface *surface,
                                   struct wl_array *keys) {
  struct state *my_state = (struct state *)data;
  int32_t *key;
  wl_array_for_each(key, keys) {
    char buf[128];
    xkb_keysym_t sym = xkb_state_key_get_one_sym(my_state->xkb_state, *key + 8);
    xkb_keysym_get_name(sym, buf, sizeof(buf));
    fprintf(stderr, "sym %s %d\n", buf, sym);
  }
}

void my_wl_keyboard_leave_callback(void *data, struct wl_keyboard *wl_keyboard,
                                   uint32_t serial,
                                   struct wl_surface *surface) {}

void my_wl_keyboard_key_callback(void *data, struct wl_keyboard *wl_keyboard,
                                 uint32_t serial, uint32_t time, uint32_t key,
                                 uint32_t state) {
  printf("%d\n", key);
}
void my_wl_keyboard_modifiers_callback(void *data,
                                       struct wl_keyboard *wl_keyboard,
                                       uint32_t serial, uint32_t mods_depressed,
                                       uint32_t mods_latched,
                                       uint32_t mods_locked, uint32_t group) {}

void my_wl_keyboard_repeat_info_callback(void *data,
                                         struct wl_keyboard *wl_keyboard,
                                         int32_t rate, int32_t delay) {}
struct wl_keyboard_listener my_wl_keyboard_listener = {
    .keymap = my_wl_keyboard_keymap_callback,
    .enter = my_wl_keyboard_enter_callback,
    .leave = my_wl_keyboard_leave_callback,
    .key = my_wl_keyboard_key_callback,
    .repeat_info = my_wl_keyboard_repeat_info_callback,
    .modifiers = my_wl_keyboard_modifiers_callback};

void my_wl_seat_capability_callback(void *data, struct wl_seat *wl_seat,
                                    uint32_t capability) {
  struct state *my_state = (struct state *)data;
  if ((capability & WL_SEAT_CAPABILITY_KEYBOARD) != 0 &&
      my_state->keyboard == NULL) {
    my_state->keyboard = wl_seat_get_keyboard(wl_seat);
    wl_keyboard_add_listener(my_state->keyboard, &my_wl_keyboard_listener,
                             data);
  }
}

void my_wl_seat_name_callback(void *data, struct wl_seat *wl_seat,
                              const char *name) {}

struct xdg_wm_base_listener xwb_listener = {.ping =
                                                my_xdg_wm_base_ping_callback};

struct wl_seat_listener my_wl_seat_listener = {
    .capabilities = my_wl_seat_capability_callback,
    .name = my_wl_seat_name_callback};

void reg_global_callback(void *data, struct wl_registry *reg, uint32_t name,
                         const char *interface, uint32_t version) {
  struct state *my_state = (struct state *)data;
  if (strcmp(wl_compositor_interface.name, interface) == 0) {
    fflush(stdout);
    my_state->compositor = (struct wl_compositor *)wl_registry_bind(
        reg, name, &wl_compositor_interface, version);
  } else if (strcmp(wl_shm_interface.name, interface) == 0) {
    my_state->shm = (struct wl_shm *)wl_registry_bind(
        reg, name, &wl_shm_interface, version);
  } else if (strcmp(xdg_wm_base_interface.name, interface) == 0) {
    my_state->xdg_base = (struct xdg_wm_base *)wl_registry_bind(
        reg, name, &xdg_wm_base_interface, version);
    xdg_wm_base_add_listener(my_state->xdg_base, &xwb_listener, my_state);
  } else if (strcmp(wl_seat_interface.name, interface) == 0) {
    my_state->seat = (struct wl_seat *)wl_registry_bind(
        reg, name, &wl_seat_interface, version);
    wl_seat_add_listener(my_state->seat, &my_wl_seat_listener, my_state);
  }
  return;
}

void reg_global_remove_handle(void *data, struct wl_registry *reg,
                              uint32_t name) {}

struct wl_registry_listener my_reg_handle = {
    .global = reg_global_callback, .global_remove = reg_global_remove_handle};

void xdg_suf_configure(void *data, struct xdg_surface *xdg_surface,
                       uint32_t serial) {
  printf("xdg surface configuring\n");
  xdg_surface_ack_configure(xdg_surface, serial);
  struct state *my_state = data;
  // 绘制内容到共享内存
  if (!my_state->configured) {

    update_buffer(my_state);
    // 创建一个新的 wl_buffer（注意：这里最好实现双缓冲，避免重用竞争）
    struct wl_buffer *buf = create_buffer(my_state);

    wl_surface_attach(my_state->surface, buf, 0, 0);
    wl_surface_damage(my_state->surface, 0, 0, width, height);
    wl_surface_commit(my_state->surface);
    my_state->configured = 1;
  }
  // 更新 last_time，为后续帧率控制做准备
  // 注意：这里获取时间戳比较麻烦，可以简单地设为 0，让第一帧不受限
  my_state->last_time = 0;
  if (my_state->sugg_heigth == 0 && my_state->sugg_width == 0) {
    return;
  }
  if (my_state->sugg_heigth * STRIDE(my_state->sugg_width) >
      my_state->pool_size) {
    uint32_t *dataptr =
        (uint32_t *)wl_shm_pool_get_user_data(my_state->shm_pool);
    munmap(dataptr, height * STRIDE(width));
    wl_shm_pool_destroy(my_state->shm_pool);
    height = my_state->sugg_heigth;
    width = my_state->sugg_width;
    int fd = allocate_shm_file(height * STRIDE(width));
    uint32_t *data = (uint32_t *)mmap(
        0, height * STRIDE(width), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    my_state->shm_pool =
        wl_shm_create_pool(my_state->shm, fd, height * STRIDE(width));
    // surface frame callback
    close(fd);
    wl_shm_pool_set_user_data(my_state->shm_pool, data);
  }
  height = my_state->sugg_heigth;
  width = my_state->sugg_width;
  my_state->sugg_heigth = 0;
  my_state->sugg_width = 0;
}
struct xdg_surface_listener xdg_suf_listener = {.configure = xdg_suf_configure};

void xdg_tp_configure(void *data, struct xdg_toplevel *xdg_toplevel,
                      int32_t width, int32_t height, struct wl_array *states) {
  printf("xdg toplevel configuring %d %d\n", width, height);
  struct state *my_state = data;
  if (width == 0 && height == 0)
    return;
  my_state->sugg_width = width;
  my_state->sugg_heigth = height;
}
void xdg_tp_close(void *data, struct xdg_toplevel *xdg_toplevel) {
  printf("I should close\n");
}

void xdg_tp_configure_bounds(void *data, struct xdg_toplevel *xdg_toplevel,
                             int32_t width, int32_t height) {
  printf("configure bounds %d %d\n", width, height);
}

void xdg_tp_wm_capabilities(void *data, struct xdg_toplevel *xdg_toplevel,
                            struct wl_array *capabilities) {}
struct xdg_toplevel_listener xdg_tp_suf_listener = {
    .configure = xdg_tp_configure,
    .close = xdg_tp_close,
    .configure_bounds = xdg_tp_configure_bounds,
    .wm_capabilities = xdg_tp_wm_capabilities};

struct wl_callback_listener frame_callback_listener;
void done(void *data, struct wl_callback *wl_callback, uint32_t callback_data) {
  printf("enter done %d\n", callback_data);
  struct state *my_state = data;
  if (callback_data - my_state->last_time >= 1000 / my_state->frame_rate) {
    grid_update(my_state->grid);
    my_state->last_time = callback_data;
  }

  struct wl_buffer *buf = create_buffer(my_state);
  update_buffer(my_state);
  wl_surface_attach(my_state->surface, buf, 0, 0);
  wl_surface_damage(my_state->surface, 0, 0, width, height);
  struct wl_callback *cb = wl_surface_frame(my_state->surface);
  wl_callback_add_listener(cb, &frame_callback_listener, my_state);

  wl_surface_commit(my_state->surface);
  wl_callback_destroy(wl_callback);
}

struct wl_callback_listener frame_callback_listener = {.done = done};
int main() {
  struct state my_state = {0};
  struct wl_display *disp = wl_display_connect(NULL);

  struct wl_registry *reg = wl_display_get_registry(disp);
  wl_registry_add_listener(reg, &my_reg_handle, &my_state);

  wl_display_roundtrip(disp);
  for (int i = 0; i < GRID_HEIGHT; ++i) {
    for (int j = 0; j < GRID_WIDTH; ++j) {
      my_state.grid[i][j] = rand() % 2;
    }
  }

  // xkb init
  my_state.xkb_context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);

  my_state.frame_rate = 60;
  my_state.pool_size = height * STRIDE(width);
  int fd = allocate_shm_file(height * STRIDE(width));

  uint32_t *data = (uint32_t *)mmap(0, height * STRIDE(width),
                                    PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  my_state.shm_pool =
      wl_shm_create_pool(my_state.shm, fd, height * STRIDE(width));
  // surface frame callback
  close(fd);
  wl_shm_pool_set_user_data(my_state.shm_pool, data);
  uint32_t *shmpooladdr = wl_shm_pool_get_user_data(my_state.shm_pool);

  printf("%p %p\n", data, shmpooladdr);
  struct wl_surface *xdg_wlsurf =
      wl_compositor_create_surface(my_state.compositor);
  my_state.surface = xdg_wlsurf;
  struct xdg_surface *xdgsuf =
      xdg_wm_base_get_xdg_surface(my_state.xdg_base, xdg_wlsurf);
  xdg_surface_add_listener(xdgsuf, &xdg_suf_listener, &my_state);
  struct xdg_toplevel *xdgtpsuf = xdg_surface_get_toplevel(xdgsuf);
  xdg_toplevel_set_title(xdgtpsuf, "Client");
  xdg_toplevel_add_listener(xdgtpsuf, &xdg_tp_suf_listener, &my_state);
  struct wl_callback *cb = wl_surface_frame(xdg_wlsurf);
  wl_callback_add_listener(cb, &frame_callback_listener, &my_state);
  wl_surface_commit(xdg_wlsurf);
  while (wl_display_dispatch(disp) != -1) {
  }
  return 0;
}
