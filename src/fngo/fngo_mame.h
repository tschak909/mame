/* license:BSD-3-Clause
 * copyright-holders:Thomas Cherryhomes
 *
 * fngo_mame.h -- MAME's Atari 7800 (a7800, a7800p) with the FujiNet
 * cartridge, as a library: the whole interface of libmame_fngo.
 *
 * Built by `make TARGET=fngo SUBTARGET=a7800 OSD=fngo` (scripts/src/osd/
 * fngo.lua). MAME runs headless on a thread the host lends it
 * (fngo_mame_run); every finished frame, every block of samples and every
 * line of console output comes back through callbacks, and the host owns
 * the window, the speakers, the keyboard and the gamepads. The cartridge
 * slot always holds the FujiNet cartridge (src/devices/bus/a7800/fujinet.h),
 * which boots CONFIG, or a game the host stages, at power-on.
 *
 * Threads. fngo_mame_run() makes its caller the emulation thread. Functions
 * marked "emulation thread" may only be called from it -- normally from the
 * `service` callback, which runs once per frame and, while the debugger
 * holds the machine, over and over. Everything else may be called from any
 * thread. One instance per process: MAME's own state is process-wide.
 */
#ifndef FNGO_MAME_H
#define FNGO_MAME_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32)
#  if defined(FNGO_MAME_BUILD)
#    define FNGO_API __declspec(dllexport)
#  else
#    define FNGO_API __declspec(dllimport)
#  endif
#else
#  define FNGO_API __attribute__((visibility("default")))
#endif

#define FNGO_MAME_API_VERSION 1
#define FNGO_MAME_SAMPLE_RATE 48000

typedef struct fngo_mame fngo_mame;

/* MAME's osd_output_channel */
enum {
    FNGO_LOG_ERROR = 0,
    FNGO_LOG_WARNING,
    FNGO_LOG_INFO,
    FNGO_LOG_DEBUG,
    FNGO_LOG_VERBOSE,
    FNGO_LOG_LOG
};

typedef struct {
    /* A finished frame (emulation thread): XRGB8888, `width` pixels a row,
     * no padding. `paced` is 0 while the debugger holds the machine: do not
     * wait for the display then. Called for every emulated frame, changed or
     * not, so it is the place to pace emulation. */
    void (*frame)(void *user, const uint32_t *xrgb, int width, int height,
                  uint64_t frame_no, int paced);
    /* Interleaved stereo at FNGO_MAME_SAMPLE_RATE (emulation thread). */
    void (*audio)(void *user, const int16_t *stereo, int frames);
    /* Once per frame, and repeatedly while the debugger holds the machine
     * (emulation thread): apply input, run the host's queued work. */
    void (*service)(void *user);
    /* MAME's console output, one FNGO_LOG_* channel (any thread). */
    void (*log)(void *user, int channel, const char *text);
    /* A MAME output changed (emulation thread): "fujinet_mode",
     * "fujinet_handover". */
    void (*output)(void *user, const char *name, int32_t value);
    /* The debugger has stopped the machine (emulation thread). */
    void (*debug_stopped)(void *user);
    void *user;
} fngo_mame_callbacks;

/* ---- lifecycle ----------------------------------------------------------- */

FNGO_API int fngo_mame_api_version(void);
FNGO_API const char *fngo_mame_build_version(void);

/* `data_dir` holds MAME's own files (cfg/, comments/, snap/ ...); it is
 * created as needed. NULL if an instance exists already. */
FNGO_API fngo_mame *fngo_mame_create(const fngo_mame_callbacks *callbacks, const char *data_dir);
/* After fngo_mame_run() has returned. */
FNGO_API void fngo_mame_destroy(fngo_mame *m);

/* The next machine: `system` "a7800" (NTSC) or "a7800p" (PAL); `bios` one of
 * the system's BIOS names ("a7800", "a7800pr" / "a7800p", or "none" for
 * none at all); `rompath` MAME's ROM search path, where the BIOS images live
 * (<rompath>/a7800/7800.u7 ...). Takes effect at the next hard reset. */
FNGO_API void fngo_mame_configure(fngo_mame *m, const char *system, const char *bios, const char *rompath);

/* Runs machines, across hard resets, until fngo_mame_stop(). The caller
 * becomes the emulation thread. 0, or MAME's EMU_ERR_* for the machine that
 * could not run (2: missing files, such as the BIOS). */
FNGO_API int fngo_mame_run(fngo_mame *m);
FNGO_API void fngo_mame_stop(fngo_mame *m);

enum {
    FNGO_RESET_SOFT = 0,     /* the console's power switch: the cartridge boots again */
    FNGO_RESET_HARD = 1      /* a new machine, with the latest fngo_mame_configure() */
};
FNGO_API void fngo_mame_reset(fngo_mame *m, int kind);

/* The service callback runs soon, even while the debugger holds the machine. */
FNGO_API void fngo_mame_wake(fngo_mame *m);

/* Whether the next machine starts with the debugger holding it. */
FNGO_API void fngo_mame_start_stopped(fngo_mame *m, int on);

/* ---- the FujiNet cartridge (any thread) ------------------------------------ */

/* fujinet-pc's BoIP listener. For the next machine. */
FNGO_API void fngo_mame_fujinet_link(fngo_mame *m, const char *host, int port, int debug);

enum {
    FNGO_BOOT_NONE = 0,      /* CONFIG */
    FNGO_BOOT_STAGED,        /* the image, through the boot block, the loader
                                and the hand-over, as a network boot */
    FNGO_BOOT_DIRECT         /* the image in the cart's SRAM from power-on */
};
/* What the cartridge boots at the next power-on (soft or hard reset).
 * `mapper` is an a78map kind name ("a78_sg" ...) or NULL for the image's
 * own. The image is copied. */
FNGO_API void fngo_mame_fujinet_boot(fngo_mame *m, int mode, const uint8_t *image, uint32_t size, const char *mapper);
/* A claimed FujiNet client in place of the built-in CONFIG, or NULL. For the
 * next machine. */
FNGO_API void fngo_mame_fujinet_client(fngo_mame *m, const uint8_t *image, uint32_t size);
/* The High Score Cart ROM (4K) and whether games get the HSC. For the next
 * machine. */
FNGO_API void fngo_mame_fujinet_hsc(fngo_mame *m, const uint8_t *rom, uint32_t size, int on);

typedef struct {
    uint32_t instance;           /* which cartridge published it */
    int present;                 /* a cartridge is running */
    int link_up;                 /* connected to fujinet-pc */
    int worker;                  /* its mailbox service is running */
    uint8_t mode;                /* 0 boot block, 1 loading, 2 game, 3 FujiNet app */
    uint8_t handover;            /* 0 none, 1 the BIOS, 2 the loader started it */
    uint8_t ackseq, err;
    uint8_t boot_state, boot_pct, boot_err;
    uint8_t load_state, load_pct;
    uint8_t mapper;              /* a78map kind of the live image */
    uint8_t staged, staged_kind;
    uint32_t staged_crc;
    uint32_t live_crc;           /* CRC-32 of the image last loaded */
    uint8_t hsc;                 /* bit0 ROM installed, bit1 on, bit2 saved, bit3 unsaved */
    uint8_t tv;                  /* 0 NTSC, 1 PAL */
    uint8_t inptctrl;
    uint8_t inpt_locked;
    int booted_image;            /* a game (not CONFIG, not a client) is running */
    uint32_t queue_depth;
    char live_kind[16];
    char link_error[128];
} fngo_mame_cart_status;

/* 1 and *out filled while a cartridge is running, else 0. */
FNGO_API int fngo_mame_fujinet_status(fngo_mame_cart_status *out);
/* The newest cartridge built; its status is current once out->instance
 * reaches it. */
FNGO_API uint32_t fngo_mame_fujinet_latest(void);

/* ---- images (any thread) --------------------------------------------------- */

typedef struct {
    char kind[16];               /* a78map kind name: "a78_rom", "a78_sg" ... */
    uint32_t crc;                /* CRC-32 of the image after any header */
    uint32_t size;               /* bytes after the header */
    uint32_t offset;             /* 128 for an .a78 header, else 0 */
    int claim;                   /* a FujiNet app: "FUJI" at $FF70 */
    int pokey;                   /* bit0 at $4000, bit1 at $0450 */
    int biosok;                  /* bit0 the NTSC BIOS starts it, bit1 the PAL one */
    int in_db;                   /* found in the cartridge's CRC database */
    uint32_t ram_size;           /* cart RAM */
} fngo_mame_plan_t;

/* How the cartridge would map an image: 0 and *out filled, or an error with
 * a reason in `why`: 1 too big, 2 a mapper the cartridge does not
 * support, 3 empty. */
FNGO_API int fngo_mame_plan(const uint8_t *image, uint32_t size, const char *mapper,
                            fngo_mame_plan_t *out, char *why, int why_size);

/* The built-in CONFIG (no .a78 header). */
FNGO_API const uint8_t *fngo_mame_config_rom(uint32_t *size);

/* The first member of a .zip or .7z whose name ends in one of `exts`
 * (";"-separated, e.g. ".a78;.bin"), or, if `path` is not an archive, the
 * file itself. 0 and *data (free with fngo_mame_free) on success. */
FNGO_API int fngo_mame_archive_read(const char *path, const char *exts, uint8_t **data, uint32_t *size,
                                    char *name, int name_size);
FNGO_API void fngo_mame_free(void *p);

/* ---- input (emulation thread) ---------------------------------------------- */

/* An input field of port `tag` (":JOYSTICKS", ":BUTTONS", ":CONSOLE" ...)
 * selected by `mask`: digital fields pressed while `value` is nonzero,
 * analog fields held at `value`. 0, or -1 if there is no such field. */
FNGO_API int fngo_mame_ioport_set(fngo_mame *m, const char *tag, uint32_t mask, int32_t value);
FNGO_API int fngo_mame_ioport_clear(fngo_mame *m, const char *tag, uint32_t mask);
/* A DIP switch or configuration setting's value. */
FNGO_API int fngo_mame_ioport_setting(fngo_mame *m, const char *tag, uint32_t mask, uint32_t value);
/* The port as the machine reads it now. */
FNGO_API int fngo_mame_ioport_read(fngo_mame *m, const char *tag, uint32_t *value);

/* ---- the debugger (emulation thread) ---------------------------------------- */

FNGO_API int fngo_mame_debug_command(fngo_mame *m, const char *command);   /* 0, or MAME's error position + 1 */
FNGO_API int fngo_mame_debug_validate(fngo_mame *m, const char *command);  /* 0 if it would parse */
FNGO_API void fngo_mame_debug_break(fngo_mame *m);
FNGO_API void fngo_mame_debug_go(fngo_mame *m);
enum { FNGO_STEP_INTO = 0, FNGO_STEP_OVER, FNGO_STEP_OUT };
FNGO_API void fngo_mame_debug_step(fngo_mame *m, int kind);
FNGO_API int fngo_mame_debug_stopped(fngo_mame *m);

/* The engine underneath, for a structured debugger window. */
typedef struct {
    uint32_t pc, a, x, y, p, sp;
    uint64_t cycles;             /* CPU cycles since power-on */
    int beam_x, beam_y;          /* where the screen's beam is */
    uint64_t frame;
} fngo_mame_cpu;
FNGO_API int fngo_mame_cpu_get(fngo_mame *m, fngo_mame_cpu *out);
enum { FNGO_REG_PC = 0, FNGO_REG_A, FNGO_REG_X, FNGO_REG_Y, FNGO_REG_P, FNGO_REG_SP };
FNGO_API int fngo_mame_cpu_set(fngo_mame *m, int reg, uint32_t value);

/* One instruction: its text ("lda $1234,x"), its length in bytes (and the
 * bytes, up to 8). Returns the length, 0 if none. */
FNGO_API int fngo_mame_disassemble(fngo_mame *m, uint32_t address, char *text, int text_size, uint8_t *bytes);

typedef struct {
    int index;
    int enabled;
    uint32_t address;
    char condition[128];
} fngo_mame_bp;
/* An execute breakpoint: its index, or -1 (a condition that does not parse). */
FNGO_API int fngo_mame_bp_set(fngo_mame *m, uint32_t address, const char *condition);
FNGO_API int fngo_mame_bp_clear(fngo_mame *m, int index);
FNGO_API int fngo_mame_bp_enable(fngo_mame *m, int index, int enabled);
FNGO_API int fngo_mame_bp_list(fngo_mame *m, fngo_mame_bp *out, int max);

enum { FNGO_WP_READ = 1, FNGO_WP_WRITE = 2 };
typedef struct {
    int index;
    int enabled;
    int type;                    /* FNGO_WP_* bits */
    uint32_t address, length;
    char condition[128];
} fngo_mame_wp;
FNGO_API int fngo_mame_wp_set(fngo_mame *m, int type, uint32_t address, uint32_t length, const char *condition);
FNGO_API int fngo_mame_wp_clear(fngo_mame *m, int index);
FNGO_API int fngo_mame_wp_enable(fngo_mame *m, int index, int enabled);
FNGO_API int fngo_mame_wp_list(fngo_mame *m, fngo_mame_wp *out, int max);

/* Run until the PC reaches `address` / to the next VBLANK. */
FNGO_API void fngo_mame_debug_run_to(fngo_mame *m, uint32_t address);
FNGO_API void fngo_mame_debug_frame(fngo_mame *m);
/* A debugger edit of the 6502's program space (RAM takes it). */
FNGO_API int fngo_mame_write(fngo_mame *m, uint32_t address, uint8_t value);
/* The debugger console's lines after line number *seq ("\n"-separated, the
 * oldest first); *seq advances to the newest. Start from 0. Returns the
 * length written. */
FNGO_API int fngo_mame_console_text(fngo_mame *m, uint32_t *seq, char *dst, int size);
/* A symbol for the debugger's expressions (labels from a symbol file). */
FNGO_API int fngo_mame_symbol_add(fngo_mame *m, const char *name, uint32_t value);

/* MAME's debug views, as text grids. */
enum {
    FNGO_VIEW_CONSOLE = 1,
    FNGO_VIEW_STATE,
    FNGO_VIEW_DISASM,
    FNGO_VIEW_MEMORY,
    FNGO_VIEW_LOG,
    FNGO_VIEW_BREAKPOINTS,
    FNGO_VIEW_WATCHPOINTS
};

/* fngo_cell.attr bits: MAME's DCA_* */
#define FNGO_ATTR_CHANGED   0x01
#define FNGO_ATTR_SELECTED  0x02
#define FNGO_ATTR_INVALID   0x04
#define FNGO_ATTR_DISABLED  0x08
#define FNGO_ATTR_ANCILLARY 0x10
#define FNGO_ATTR_CURRENT   0x20
#define FNGO_ATTR_COMMENT   0x40
#define FNGO_ATTR_VISITED   0x80

typedef struct fngo_view fngo_view;
typedef struct { uint8_t ch, attr; } fngo_cell;
typedef struct {
    int total_cols, total_rows;  /* the whole view */
    int cols, rows;              /* the visible part */
    int left, top;               /* where it starts */
    int cursor_col, cursor_row, cursor_visible;
} fngo_view_geom;

FNGO_API fngo_view *fngo_mame_view_alloc(fngo_mame *m, int type);
FNGO_API void fngo_mame_view_free(fngo_mame *m, fngo_view *v);
/* The visible window: size and top-left position (-1 keeps one). */
FNGO_API void fngo_mame_view_set(fngo_view *v, int cols, int rows, int left, int top);
/* The visible window's cells, row by row (cols x rows of them, up to
 * `max`); returns how many were written. */
FNGO_API int fngo_mame_view_get(fngo_view *v, fngo_cell *cells, int max, fngo_view_geom *geom);
/* The view's sources ("6502 ':maincpu'", ...), NUL-separated into `names`;
 * returns how many there are. */
FNGO_API int fngo_mame_view_sources(fngo_view *v, char *names, int size);
FNGO_API void fngo_mame_view_source(fngo_view *v, int index);
/* The address expression of a disassembly or memory view. */
FNGO_API void fngo_mame_view_expression(fngo_view *v, const char *expression);
FNGO_API void fngo_mame_view_click(fngo_view *v, int button, int col, int row);
FNGO_API void fngo_mame_view_char(fngo_view *v, int ch);
/* The address under a disassembly view's cursor. */
FNGO_API int fngo_mame_view_selected_address(fngo_view *v, uint32_t *address);

/* ---- the machine's state (emulation thread) ---------------------------------- */

/* The 6502's program space, without side effects. Returns bytes read. */
FNGO_API int fngo_mame_read(fngo_mame *m, uint32_t address, uint8_t *dst, int n);
/* A registered save-state item, by device tag and name (":maria", "m_dll"),
 * copied raw; returns its size in bytes, or -1. */
FNGO_API int fngo_mame_save_item(fngo_mame *m, const char *device, const char *name, void *dst, int size);
/* The machine's palette as XRGB; returns the number of entries. */
FNGO_API int fngo_mame_palette(fngo_mame *m, uint32_t *xrgb, int max);
/* The emulated frame count and the machine's time in seconds. */
FNGO_API uint64_t fngo_mame_frame_number(fngo_mame *m);
FNGO_API double fngo_mame_time(fngo_mame *m);

#ifdef __cplusplus
}
#endif

#endif /* FNGO_MAME_H */
