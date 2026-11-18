/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments (main.c, from which this is built) */
/* U0U boot and main loop. Boot order: WDT first, boot-loop guard, fatal vectors, guards; then flash, LCD, input
 * (TIMER5 IRQ, 10 kHz), audio (ALNK0 IRQ). The main loop is web_frame of the browser build (drumbox_web.c) with the
 * ways out added: OCT- + OCT+ held 5 s enters UBOOT, the M-UPGRADE update (a package from the installer) and the
 * stock soft key's SysEx; two boots in a row that die in their first 30 s enter UBOOT too. */
extern uint32_t _data_start[], _data_end[], _data_load[], _bss_start[], _bss_end[];
extern uint32_t _pool_start[], _pool_end[], _rt_start[], _rt_end[], _rt_load[];

#include "timer5.c"                         /* input/USB tick; nested in the audio render */
extern void isr_timer5(void);

static void timer5_start(void)                 /* OSC /4 = 6 MHz, PRD 600 -> 10 kHz */
{
    fm1_timer5_start(isr_timer5, 4);   /* above ALNK0 (3): the scan nests into the render */
}

static void hexs(char *b, uint32_t v)
{
    uint32_t i;
    for (i = 0; i < 8u; i++)
        b[i] = "0123456789ABCDEF"[(v >> (28u - 4u * i)) & 15u];
    b[8] = 0;
}

static void fm1_fault(const fm1_crash_t *c)
{
    char b[12];
    uint32_t t0;
    fm1_audio_stop();
    lcd_fill(0, 0, 240, 240, UI_CRASH_BG);            /* fixed, outside the palettes */
    draw_text_line(0, 8, 240, &AF_M, "U0U CRASH", UI_CRASH_INK, UI_CRASH_BG, 1);
    hexs(b, c->vec);
    draw_text_line(10, 40, 220, &AF_M, b, UI_CRASH_INK, UI_CRASH_BG, 0);
    hexs(b, c->pc);
    draw_text_line(10, 60, 220, &AF_M, b, UI_CRASH_INK, UI_CRASH_BG, 0);
    hexs(b, c->emu);
    draw_text_line(10, 84, 220, &AF_M, b, UI_CRASH_INK, UI_CRASH_BG, 0);
    hexs(b, c->dbg);
    draw_text_line(10, 102, 220, &AF_M, b, UI_CRASH_INK, UI_CRASH_BG, 0);
    hexs(b, c->rets);
    draw_text_line(10, 120, 220, &AF_M, b, UI_CRASH_INK, UI_CRASH_BG, 0);
    t0 = fm1_ticks();
    while ((uint32_t)(fm1_ticks() - t0) < 4000u * 1000u * FM1_TICKS_PER_US)
        ;
    fm1_reboot();
}

/* the flash: one of the two 1 MiB parts of the FM-1, else RAM-only (st_* refuse, storage_hw.c) */
static void dbx_flash_boot(void)
{
    uint32_t f = irq_save(), id;
    id = FL_FAR(fl_jedec_ram)();
    flash_ok = id == 0x856014u || id == 0xB36014u;        /* the 1 MiB part: Puya P25Q80H, or the UC25HQ80 of newer units */
    irq_restore(f);
}

/* power-on: the lanes, the sequencer, the effects, what the flash holds, the MASTER pot as it stands (no glide), the audio */
static void dbx_power_on(void)
{
    uint32_t i, adc = 0;
    dbx_palette();
    lcd_fill(0, 0, 240, 240, T_BG);
    dbx_voices_init();
    dbx_seq_init();
    dbx_fx_init();
    if (flash_ok)
        dbx_store_load();                 /* the patterns, the kit and the settings the flash holds */
    (void)fm1_adc_read(FM1_ADC_MASTER);   /* (the first conversion after the pin setup: dropped) */
    for (i = 0; i < 3u; i++) {
        int32_t a = fm1_adc_read(FM1_ADC_MASTER);
        if (a >= 0) {
            adc = (uint32_t)a;
            break;
        }
    }
    dbx_audio.master_q12 = dbx_master_of_pot(adc);
    dbx_audio.g_cur = (int32_t)((dbx_audio.master_q12 * dbx_audio.vol_q12) >> 12);   /* no glide from silence */
    dbx_ui.master_knob = (int32_t)adc * 16;
    audio_init();
}

/* the host's real-time messages (the computer's clock, Start, Continue, Stop) off the USB-MIDI ring: run by the audio ISR's sequencer block (DBX_MIDI_DRAIN) */
static void dbx_midi_poll(void)
{
    if (midi_in_overflow) {                            /* a broken stream: drop it all */
        mi_r = mi_w;
        midi_in_overflow = 0;
        return;
    }
    while (mi_r != mi_w) {
        uint32_t at = mi_r % MQ, status = (midi_in_q[at] >> 8) & 0xFFu, ms = midi_in_ms[at];
        mi_r++;
        dbx_pk_n++;
        if (status >= 0xF8u)
            dbx_midi_realtime((uint8_t)status, ms);
    }
}

static uint32_t dbx_usb_powered(void)               /* a host has configured us and is not suspended: it powers the box (the battery shows a bolt) */
{
    return usb.config && !usb.suspended;
}

static void fm1_main(void)
{
    dbx_flash_boot();
#if FELUCCA_OTA
    if (flash_ok)
        ota_boot_cleanup();                             /* staging area left by an update */
#endif
    lcd_init();
    fm1_input_init();
    fm1_adc_init();
    dbx_power_on();
    usb_start();
    fm1_led_dim_level(0);                               /* the dim glow of the lit keys (DIM HI) */
    timer5_start();
    fm1_guard_lock_top();
    fm1_irq_enable_all();
    fm1_delay_ms(30);
    dbx_ms0 = fm1_ms;                                   /* the splash is counted from here (fm1_ms holds the boot so far) */

    for (;;) {
        uint32_t m = fm1_ms;
        fm1_wdt_feed();
        usb_retry(fm1_ms);
        if (fm1_ms > 30000u && bootguard.pending) {     /* a crash or hang in the first 30 s counts */
            bootguard.pending = 0;
            bootguard.failed = 0;
        }
        {   /* OCT- + OCT+ held 5 s: enter UBOOT with RAM intact (debug / update); a countdown shows from 2 s,
             * letting go cancels it */
            static uint32_t t0, shown;
            uint32_t both = PANEL_BTN(B_OCTDN) | PANEL_BTN(B_OCTUP);
            if ((fm1_in.buttons & both) != both) {
                if (shown) {
                    shown = 0;
                    dbx_ui.dirty |= DIRTY_ALL;          /* what the countdown covered */
                }
                t0 = fm1_ms;
            } else if (fm1_ms - t0 > 2000u && fm1_ms - t0 <= 5000u) {
                uint32_t left = (5000u - (fm1_ms - t0) + 999u) / 1000u;
                if (left != shown) {
                    char b[10] = "UPDATE  ";
                    shown = left;
                    b[7] = (char)('0' + left);
                    b[8] = 0;
                    lcd_fill(0, 0, 240, 240, T_BG);
                    draw_text_box(0, 110, 240, &AF_M, b, T_THEME, 1);
                }
            } else if (fm1_ms - t0 > 5000u) {
                fm1_audio_stop();
                lcd_fill(0, 0, 240, 240, T_BG);
                draw_text_box(0, 110, 240, &AF_M, "UBOOT", T_THEME, 1);
                usb_detach();
                fm1_delay_ms(30);
                bootguard.pending = 0;                  /* intentional reset: not a failed boot */
                fm1_enter_uboot();
            }
            if (shown)
                continue;                               /* the countdown owns the screen */
        }
#if FELUCCA_OTA
        ota_service();                                  /* M-UPGRADE handshake */
        if (usb.ota_req) {                              /* M-UPGRADE upgrade command */
            usb.ota_req = 0;
            if (flash_ok)
                ota_session();                          /* returns only if nothing was committed */
            lcd_fill(0, 0, 240, 240, T_BG);
            dbx_ui.dirty |= DIRTY_ALL;
        }
#endif
        if (usb.uboot_req) {                            /* SysEx F0 22 24 35 7D F7 from the host */
            fm1_audio_stop();
            lcd_fill(0, 0, 240, 240, T_BG);
            draw_text_box(0, 110, 240, &AF_M, "UBOOT (USB)", T_THEME, 1);
            fm1_delay_ms(20);
            usb_detach();
            fm1_delay_ms(30);
            bootguard.pending = 0;
            fm1_enter_uboot();
        }
        dbx_master_poll();
        dbx_batt_poll();
        dbx_ui_input();
        dbx_arp_poll();
        dbx_store_poll();
        dbx_ui_leds();
        dbx_ui_draw();
        while (fm1_ms - m < 15u)                        /* ~60 UI frames/s at most */
            fm1_wdt_feed();
    }
}

void fm1_cstart(void)
{
    uint32_t *s, *d, p3, src, wdt;
    fm1_time_init();
    fm1_reset_reason();
    p3 = fm1_boot.p3_rst;
    src = fm1_boot.rst_src;
    wdt = fm1_boot.wdt_con;
    fm1_wdt_arm(0x0D);
    if (bootguard.magic != BOOTGUARD_MAGIC) {
        bootguard.magic = BOOTGUARD_MAGIC;
        bootguard.failed = 0;
        bootguard.pending = 0;
    }
    if (bootguard.pending)
        bootguard.failed++;
    bootguard.pending = 1;
    if (bootguard.failed >= 2u) {
        bootguard.failed = 0;
        bootguard.pending = 0;
        fm1_enter_uboot();
    }
    fm1_irq_init();
    for (d = _bss_start; d < _bss_end; d++)
        *d = 0;
    for (d = _pool_start; d < _pool_end; d++)
        *d = 0;
    for (s = _data_load, d = _data_start; d < _data_end; s++, d++)
        *d = *s;
    for (s = _rt_load, d = _rt_start; d < _rt_end; s++, d++)
        *d = *s;                                /* flash driver code that must run from RAM (.ram_text, app.ld) */
    fm1_mailbox_clear();
    fm1_guard_enable(FM1_GUARD_STACK | FM1_GUARD_WRITE | FM1_GUARD_BUS | FM1_GUARD_PC);
    fm1_boot.p3_rst = (uint8_t)p3;
    fm1_boot.rst_src = src;
    fm1_boot.wdt_con = (uint8_t)wdt;
    fm1_main();
    for (;;)
        ;
}
