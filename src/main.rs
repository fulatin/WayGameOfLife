//! Wayland Conway's Game of Life — Rust rewrite of client.c

use std::io::Read;
use std::os::fd::BorrowedFd;
use std::os::unix::io::RawFd;
use std::time::{SystemTime, UNIX_EPOCH};

use memmap2::MmapMut;
use wayland_client::{
    Connection, Dispatch, Proxy, QueueHandle, WEnum,
    protocol::{
        wl_buffer::{self, WlBuffer},
        wl_callback::{self, WlCallback},
        wl_compositor::WlCompositor,
        wl_keyboard::{self, WlKeyboard, KeymapFormat},
        wl_registry::{self, WlRegistry},
        wl_seat::{self, WlSeat},
        wl_shm::{self, WlShm},
        wl_shm_pool::WlShmPool,
        wl_surface::WlSurface,
    },
};
use wayland_protocols::xdg::shell::client::{
    xdg_surface::{self, XdgSurface},
    xdg_toplevel::{self, XdgToplevel},
    xdg_wm_base::{self, XdgWmBase},
};
use xkbcommon::xkb;

// ─── Constants ───────────────────────────────────────────────────────────────

const GRID_W: usize = 100;
const GRID_H: usize = 100;
const INIT_W: usize = 700;
const INIT_H: usize = 700;

const fn stride(w: usize) -> usize { w * 4    }

// ─── SHM helpers ─────────────────────────────────────────────────────────────

unsafe fn randname(buf: &mut [u8; 6]) {
    let ns = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap()
        .subsec_nanos() as u64;
    let mut r = ns;
    for b in buf.iter_mut() {
        *b = b'A' + ((r & 15) + (r & 16) * 2) as u8;
        r >>= 5;
    }
}

unsafe fn create_shm_file() -> RawFd {
    for _ in 0..100 {
        let mut name = *b"/wl_shm-XXXXXX\0";
        randname((&mut name[8..14]).try_into().unwrap());
        let fd = libc::shm_open(
            name.as_ptr() as *const libc::c_char,
            libc::O_RDWR | libc::O_CREAT | libc::O_EXCL,
            0o600,
        );
        if fd >= 0 {
            libc::shm_unlink(name.as_ptr() as *const libc::c_char);
            return fd;
        }
        if std::io::Error::last_os_error().raw_os_error() != Some(libc::EEXIST) {
            break;
        }
    }
    -1
}

unsafe fn allocate_shm_file(size: usize) -> RawFd {
    let fd = create_shm_file();
    if fd < 0 {
        return -1;
    }
    loop {
        let ret = libc::ftruncate(fd, size as libc::off_t);
        if ret >= 0 || std::io::Error::last_os_error().raw_os_error() != Some(libc::EINTR) {
            if ret < 0 {
                libc::close(fd);
                return -1;
            }
            return fd;
        }
    }
}

// ─── AppState ────────────────────────────────────────────────────────────────

struct AppState {
    running: bool,

    compositor: Option<WlCompositor>,
    shm: Option<WlShm>,
    xdg_base: Option<XdgWmBase>,
    seat: Option<WlSeat>,
    keyboard: Option<WlKeyboard>,

    surface: Option<WlSurface>,
    xdg_surface: Option<XdgSurface>,
    xdg_toplevel: Option<XdgToplevel>,

    shm_pool: Option<WlShmPool>,
    pool_mmap: Option<MmapMut>,
    pool_size: usize,

    width: usize,
    height: usize,
    sugg_width: usize,
    sugg_height: usize,

    configured: bool,
    last_time: u32,
    frame_rate: u32,

    grid: [[i32; GRID_W]; GRID_H],

    xkb_context: xkb::Context,
    xkb_keymap: Option<xkb::Keymap>,
    xkb_state: Option<xkb::State>,
}

impl AppState {
    fn new() -> Self {
        Self {
            running: true,
            compositor: None,
            shm: None,
            xdg_base: None,
            seat: None,
            keyboard: None,
            surface: None,
            xdg_surface: None,
            xdg_toplevel: None,
            shm_pool: None,
            pool_mmap: None,
            pool_size: 0,
            width: INIT_W,
            height: INIT_H,
            sugg_width: 0,
            sugg_height: 0,
            configured: false,
            last_time: 0,
            frame_rate: 60,
            grid: [[0; GRID_W]; GRID_H],
            xkb_context: xkb::Context::new(xkb::CONTEXT_NO_FLAGS),
            xkb_keymap: None,
            xkb_state: None,
        }
    }
}

// ─── Game of Life ────────────────────────────────────────────────────────────

impl AppState {
    fn grid_at(&self, x: i32, y: i32) -> bool {
        x >= 0 && y >= 0 && (x as usize) < GRID_H && (y as usize) < GRID_W
    }

    fn grid_around(&self, x: usize, y: usize) -> i32 {
        let mut count = 0;
        for dx in -1i32..=1 {
            for dy in -1i32..=1 {
                let nx = x as i32 + dx;
                let ny = y as i32 + dy;
                if (dx != 0 || dy != 0)
                    && self.grid_at(nx, ny)
                    && self.grid[nx as usize][ny as usize] != 0
                {
                    count += 1;
                }
            }
        }
        count
    }

    fn grid_update(&mut self) {
        let mut next = [[0i32; GRID_W]; GRID_H];
        for (i, row) in next.iter_mut().enumerate() {
            for (j, cell) in row.iter_mut().enumerate() {
                let around = self.grid_around(i, j);
                *cell = if self.grid[i][j] != 0 {
                    if !(2..=3).contains(&around) { 0 } else { around }
                } else {
                    if around == 3 { around } else { 0 }
                };
            }
        }
        self.grid = next;
    }

    fn randomize_grid(&mut self) {
        // Simple LCG for deterministic-ish but varied initial state
        let mut seed = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .unwrap()
            .subsec_nanos();
        for i in 0..GRID_H {
            for j in 0..GRID_W {
                seed = seed.wrapping_mul(1103515245).wrapping_add(12345);
                self.grid[i][j] = ((seed >> 16) & 1) as i32;
            }
        }
    }
}

// ─── Drawing ─────────────────────────────────────────────────────────────────

impl AppState {
    fn write_pixels(&mut self) {
        let data = self.pool_mmap.as_deref_mut().unwrap_or(&mut []);
        let w = self.width;
        let h = self.height;
        for i in 0..h {
            for j in 0..w {
                let gx = i * GRID_H / h;
                let gy = j * GRID_W / w;
                let val = self.grid[gx][gy];
                let pixel = if val != 0 {
                    0xAA000000u32.wrapping_add(1000 * val as u32)
                } else {
                    0xAAFFFFFF
                };
                let off = (i * w + j) * 4;
                if off + 4 <= data.len() {
                    data[off..off + 4].copy_from_slice(&pixel.to_ne_bytes());
                }
            }
        }
    }

    fn draw_and_commit(&mut self, qh: &QueueHandle<Self>) {
        self.write_pixels();

        let pool = match &self.shm_pool {
            Some(p) => p,
            None => return,
        };
        let surface = match &self.surface {
            Some(s) => s,
            None => return,
        };

        let w = self.width as i32;
        let h = self.height as i32;
        let buffer = pool.create_buffer(
            0, w, h, w * 4, wl_shm::Format::Xrgb8888, qh, (),
        );

        surface.attach(Some(&buffer), 0, 0);
        surface.damage(0, 0, w, h);

        let _cb = surface.frame(qh, ());
        surface.commit();
    }

    fn resize_pool_if_needed(&mut self, qh: &QueueHandle<Self>) {
        let new_w = self.sugg_width;
        let new_h = self.sugg_height;
        if new_w == 0 || new_h == 0 {
            return;
        }
        let needed = new_h * stride(new_w);
        if needed > self.pool_size {
            // Replace pool
            self.pool_mmap.take();
            if let Some(p) = self.shm_pool.take() {
                p.destroy();
            }
            let shm = self.shm.as_ref().unwrap();
            let fd = unsafe { allocate_shm_file(needed) };
            assert!(fd >= 0, "allocate_shm_file failed");
            let mmap = unsafe { MmapMut::map_mut(fd) }.expect("mmap shm pool");
            let pool = shm.create_pool(unsafe { BorrowedFd::borrow_raw(fd) }, needed as i32, qh, ());
            unsafe { libc::close(fd); }
            self.shm_pool = Some(pool);
            self.pool_mmap = Some(mmap);
            self.pool_size = needed;
        }
        self.width = new_w;
        self.height = new_h;
        self.sugg_width = 0;
        self.sugg_height = 0;
    }
}

// ─── Wayland Dispatch impls ──────────────────────────────────────────────────

impl Dispatch<WlRegistry, ()> for AppState {
    fn event(
        state: &mut Self,
        registry: &WlRegistry,
        event: <WlRegistry as Proxy>::Event,
        _data: &(),
        _conn: &Connection,
        qh: &QueueHandle<Self>,
    ) {
        if let wl_registry::Event::Global { name, interface, version } = event {
            match interface.as_str() {
                "wl_compositor" => {
                    state.compositor = Some(registry.bind(name, version, qh, ()));
                }
                "wl_shm" => {
                    state.shm = Some(registry.bind(name, version, qh, ()));
                }
                "xdg_wm_base" => {
                    state.xdg_base = Some(registry.bind(name, version, qh, ()));
                }
                "wl_seat" => {
                    state.seat = Some(registry.bind(name, version, qh, ()));
                }
                _ => {}
            }
        }
    }
}

impl Dispatch<WlSeat, ()> for AppState {
    fn event(
        state: &mut Self,
        seat: &WlSeat,
        event: <WlSeat as Proxy>::Event,
        _data: &(),
        _conn: &Connection,
        qh: &QueueHandle<Self>,
    ) {
        if let wl_seat::Event::Capabilities { capabilities } = event {
            use wayland_client::protocol::wl_seat::Capability;
            if let WEnum::Value(caps) = capabilities {
                if caps.contains(Capability::Keyboard) && state.keyboard.is_none() {
                    let kb = seat.get_keyboard(qh, ());
                    state.keyboard = Some(kb);
                }
            }
        }
    }
}

impl Dispatch<WlKeyboard, ()> for AppState {
    fn event(
        state: &mut Self,
        _kb: &WlKeyboard,
        event: <WlKeyboard as Proxy>::Event,
        _data: &(),
        _conn: &Connection,
        _qh: &QueueHandle<Self>,
    ) {
        match event {
            wl_keyboard::Event::Keymap { format, fd, size } => {
                if format == WEnum::Value(KeymapFormat::XkbV1) {
                    let mut file = std::fs::File::from(fd);
                    let mut buf = vec![0u8; size as usize];
                    let _ = file.read_exact(&mut buf);
                    std::mem::forget(file);
                    // Truncate at first null — keymap data is null-terminated
                    let end = buf.iter().position(|&b| b == 0).unwrap_or(buf.len());
                    let keymap_str = std::str::from_utf8(&buf[..end]).unwrap_or("");
                    if !keymap_str.is_empty() {
                        state.xkb_keymap = Some(
                            xkb::Keymap::new_from_string(
                                &state.xkb_context,
                                keymap_str.to_string(),
                                xkb::KEYMAP_FORMAT_TEXT_V1,
                                xkb::KEYMAP_COMPILE_NO_FLAGS,
                            )
                            .unwrap(),
                        );
                        state.xkb_state =
                            Some(xkb::State::new(state.xkb_keymap.as_ref().unwrap()));
                    }
                }
            }
            wl_keyboard::Event::Enter { keys, .. } => {
                if let (Some(ref _km), Some(ref ks)) = (&state.xkb_keymap, &state.xkb_state) {
                    for chunk in keys.chunks_exact(4) {
                        let key = u32::from_ne_bytes([chunk[0], chunk[1], chunk[2], chunk[3]]);
                        let sym = ks.key_get_one_sym((key + 8).into());
                        let name = xkb::keysym_get_name(sym);
                        eprintln!("sym {} {:?}", name, sym);
                    }
                }
            }
            wl_keyboard::Event::Key { key, .. } => {
                println!("{key}");
            }
            _ => {}
        }
    }
}

impl Dispatch<WlCallback, ()> for AppState {
    fn event(
        state: &mut Self,
        _callback: &WlCallback,
        event: <WlCallback as Proxy>::Event,
        _data: &(),
        _conn: &Connection,
        qh: &QueueHandle<Self>,
    ) {
        if let wl_callback::Event::Done { callback_data } = event {
            if callback_data.wrapping_sub(state.last_time) >= 1000 / state.frame_rate {
                state.grid_update();
                state.last_time = callback_data;
            }
            state.draw_and_commit(qh);
        }
    }
}

impl Dispatch<WlBuffer, ()> for AppState {
    fn event(
        _state: &mut Self,
        buffer: &WlBuffer,
        event: <WlBuffer as Proxy>::Event,
        _data: &(),
        _conn: &Connection,
        _qh: &QueueHandle<Self>,
    ) {
        if let wl_buffer::Event::Release = event {
            buffer.destroy();
        }
    }
}

impl Dispatch<XdgWmBase, ()> for AppState {
    fn event(
        _state: &mut Self,
        xdg: &XdgWmBase,
        event: <XdgWmBase as Proxy>::Event,
        _data: &(),
        _conn: &Connection,
        _qh: &QueueHandle<Self>,
    ) {
        if let xdg_wm_base::Event::Ping { serial } = event {
            xdg.pong(serial);
            println!("get a ping");
        }
    }
}

impl Dispatch<XdgSurface, ()> for AppState {
    fn event(
        state: &mut Self,
        xdg: &XdgSurface,
        event: <XdgSurface as Proxy>::Event,
        _data: &(),
        _conn: &Connection,
        qh: &QueueHandle<Self>,
    ) {
        if let xdg_surface::Event::Configure { serial } = event {
            println!("xdg surface configuring");
            xdg.ack_configure(serial);

            if !state.configured {
                state.draw_and_commit(qh);
                state.configured = true;
            }
            state.last_time = 0;

            state.resize_pool_if_needed(qh);
        }
    }
}

impl Dispatch<XdgToplevel, ()> for AppState {
    fn event(
        state: &mut Self,
        _xdg: &XdgToplevel,
        event: <XdgToplevel as Proxy>::Event,
        _data: &(),
        _conn: &Connection,
        _qh: &QueueHandle<Self>,
    ) {
        match event {
            xdg_toplevel::Event::Configure { width, height, .. } => {
                println!("xdg toplevel configuring {width} {height}");
                if width != 0 && height != 0 {
                    state.sugg_width = width as usize;
                    state.sugg_height = height as usize;
                }
            }
            xdg_toplevel::Event::Close => {
                println!("I should close");
                state.running = false;
            }
            xdg_toplevel::Event::ConfigureBounds { width, height } => {
                println!("configure bounds {width} {height}");
            }
            _ => {}
        }
    }
}

// Stub impls for proxy types that never receive events but appear in registrations.
macro_rules! stub_dispatch {
    ($ty:ty) => {
        impl Dispatch<$ty, ()> for AppState {
            fn event(
                _: &mut Self,
                _: &$ty,
                _: <$ty as Proxy>::Event,
                _: &(),
                _: &Connection,
                _: &QueueHandle<Self>,
            ) {}
        }
    };
}

stub_dispatch!(WlCompositor);
stub_dispatch!(WlShm);
stub_dispatch!(WlShmPool);
stub_dispatch!(WlSurface);

// ─── Main ────────────────────────────────────────────────────────────────────

fn main() -> Result<(), Box<dyn std::error::Error>> {
    let conn = Connection::connect_to_env()?;
    let mut event_queue = conn.new_event_queue();
    let qh = event_queue.handle();
    let display = conn.display();

    let _registry = display.get_registry(&qh, ());
    let mut state = AppState::new();

    // Roundtrip to discover globals
    event_queue.roundtrip(&mut state)?;

    state.randomize_grid();

    // Create initial SHM pool
    let pool_size = INIT_H * stride(INIT_W);
    let fd = unsafe { allocate_shm_file(pool_size) };
    assert!(fd >= 0, "allocate_shm_file failed");
    let mmap = unsafe { MmapMut::map_mut(fd) }.expect("mmap shm pool");

    let shm = state.shm.as_ref().expect("wl_shm not available");
    let shm_pool = shm.create_pool(unsafe { BorrowedFd::borrow_raw(fd) }, pool_size as i32, &qh, ());
    unsafe { libc::close(fd); }

    state.shm_pool = Some(shm_pool);
    state.pool_mmap = Some(mmap);
    state.pool_size = pool_size;

    // Create surface hierarchy
    let compositor = state.compositor.as_ref().expect("wl_compositor not available");
    let xdg_base = state.xdg_base.as_ref().expect("xdg_wm_base not available");

    let surface = compositor.create_surface(&qh, ());
    let xdg_surface = xdg_base.get_xdg_surface(&surface, &qh, ());
    let xdg_toplevel = xdg_surface.get_toplevel(&qh, ());
    xdg_toplevel.set_title("Client".to_owned());

    // Initial commit triggers the compositor to send xdg_surface.configure
    surface.commit();

    state.surface = Some(surface);
    state.xdg_surface = Some(xdg_surface);
    state.xdg_toplevel = Some(xdg_toplevel);

    event_queue.roundtrip(&mut state)?;

    // Event loop
    while state.running {
        event_queue.blocking_dispatch(&mut state)?;
    }

    Ok(())
}
