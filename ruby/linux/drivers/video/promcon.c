/* $Id$
 * Console driver utilizing PROM sun terminal emulation
 *
 * Copyright (C) 1998  Eddie C. Dost  (ecd@skynet.be)
 * Copyright (C) 1998  Jakub Jelinek  (jj@ultra.linux.cz)
 */

#include <linux/config.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/mm.h>
#include <linux/tty.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/vt_kern.h>
#include <linux/init.h>
#include <linux/kd.h>

#include <asm/oplib.h>
#include <asm/uaccess.h>

static short pw = 80 - 1, ph = 34 - 1;
static short px, py;
static unsigned long promcon_uni_pagedir[2];

extern u8 promfont_unicount[];
extern u16 promfont_unitable[];

#define PROMCON_COLOR 0

#if PROMCON_COLOR
#define inverted(s)	((((s) & 0x7700) == 0x0700) ? 0 : 1)
#else
#define inverted(s)	(((s) & 0x0800) ? 1 : 0)
#endif

static __inline__ void
promcon_puts(char *buf, int cnt)
{
	prom_printf("%*.*s", cnt, cnt, buf);
}

static int
promcon_start(struct vc_data *vc, char *b)
{
	unsigned short *s = (unsigned short *)
			(vc->vc_origin + py * vc->vc_size_row + (px << 1));

	if (px == pw) {
		unsigned short *t = s - 1;

		if (inverted(*s) && inverted(*t))
			return sprintf(b, "\b\033[7m%c\b\033[@%c\033[m",
				       *s, *t);
		else if (inverted(*s))
			return sprintf(b, "\b\033[7m%c\033[m\b\033[@%c",
				       *s, *t);
		else if (inverted(*t))
			return sprintf(b, "\b%c\b\033[@\033[7m%c\033[m",
				       *s, *t);
		else
			return sprintf(b, "\b%c\b\033[@%c", *s, *t);
	}

	if (inverted(*s))
		return sprintf(b, "\033[7m%c\033[m\b", *s);
	else
		return sprintf(b, "%c\b", *s);
}

static int
promcon_end(struct vc_data *vc, char *b)
{
	unsigned short *s = (unsigned short *)
			(vc->vc_origin + py * vc->vc_size_row + (px << 1));
	char *p = b;

	b += sprintf(b, "\033[%d;%dH", py + 1, px + 1);

	if (px == pw) {
		unsigned short *t = s - 1;

		if (inverted(*s) && inverted(*t))
			b += sprintf(b, "\b%c\b\033[@\033[7m%c\033[m", *s, *t);
		else if (inverted(*s))
			b += sprintf(b, "\b%c\b\033[@%c", *s, *t);
		else if (inverted(*t))
			b += sprintf(b, "\b\033[7m%c\b\033[@%c\033[m", *s, *t);
		else
			b += sprintf(b, "\b\033[7m%c\033[m\b\033[@%c", *s, *t);
		return b - p;
	}

	if (inverted(*s))
		b += sprintf(b, "%c\b", *s);
	else
		b += sprintf(b, "\033[7m%c\033[m\b", *s);
	return b - p;
}

const char __init *promcon_startup(struct vt_struct *vt, int init)
{
	const char *display_desc = "PROM";
	struct vc_data *vc;
	char buf[40];
	int node;

	vt->default_mode = vc = &prom_default;
	
	node = prom_getchild(prom_root_node);
	node = prom_searchsiblings(node, "options");
	if (prom_getproperty(node,  "screen-#columns", buf, 40) != -1) {
		pw = simple_strtoul(buf, NULL, 0);
		if (pw < 10 || pw > 256)
			pw = 80;
		pw--;
	}
	if (prom_getproperty(node,  "screen-#rows", buf, 40) != -1) {
		ph = simple_strtoul(buf, NULL, 0);
		if (ph < 10 || ph > 256)
			ph = 34;
		ph--;
	}
	promcon_puts("\033[H\033[J", 6);
	vc->vc_can_do_color = PROMCON_COLOR;
	if (init) {
		vc->vc_cols = pw + 1;
		vc->vc_rows = ph + 1;
	}
	return display_desc;
}

static void __init 
promcon_init_unimap(struct vc_data *vc)
{
	mm_segment_t old_fs = get_fs();
	struct unipair *p, *p1;
	u16 *q;
	int i, j, k;
	
	p = kmalloc(256*sizeof(struct unipair), GFP_KERNEL);
	if (!p) return;
	
	q = promfont_unitable;
	p1 = p;
	k = 0;
	for (i = 0; i < 256; i++)
		for (j = promfont_unicount[i]; j; j--) {
			p1->unicode = *q++;
			p1->fontpos = i;
			p1++;
			k++;
		}
	set_fs(KERNEL_DS);
	con_clear_unimap(vc, NULL);
	con_set_unimap(vc, k, p);
	con_protect_unimap(vc, 1);
	set_fs(old_fs);
	kfree(p);
}

static void
promcon_init(struct vc_data *vc)
{
	unsigned long p;
	
	vc->vc_can_do_color = PROMCON_COLOR;
	vc->vc_cols = vc->display_fg->default_mode->vc_cols;
	vc->vc_rows = vc->display_fg->default_mode->vc_rows;
	p = *vc->vc_uni_pagedir_loc;
	if (vc->vc_uni_pagedir_loc == &vc->vc_uni_pagedir ||
	    !--vc->vc_uni_pagedir_loc[1])
		con_free_unimap(vc);
	vc->vc_uni_pagedir_loc = promcon_uni_pagedir;
	promcon_uni_pagedir[1]++;
	if (!promcon_uni_pagedir[0] && p) 
		promcon_init_unimap(vc);
}

static void
promcon_deinit(struct vc_data *vc)
{
	/* When closing the last console, reset video origin */
	if (!--promcon_uni_pagedir[1])
		con_free_unimap(vc);
	vc->vc_uni_pagedir_loc = &vc->vc_uni_pagedir;
	con_set_default_unimap(vc);
}

static int
promcon_switch(struct vc_data *vc)
{
	return 1;
}

static unsigned short *
promcon_repaint_line(unsigned short *s, unsigned char *buf, unsigned char **bp)
{
	int cnt = pw + 1;
	int attr = -1;
	unsigned char *b = *bp;

	while (cnt--) {
		if (attr != inverted(*s)) {
			attr = inverted(*s);
			if (attr) {
				strcpy (b, "\033[7m");
				b += 4;
			} else {
				strcpy (b, "\033[m");
				b += 3;
			}
		}
		*b++ = *s++;
		if (b - buf >= 224) {
			promcon_puts(buf, b - buf);
			b = buf;
		}
	}
	*bp = b;
	return s;
}

static void
promcon_putcs(struct vc_data *vc, const unsigned short *s,
	      int count, int y, int x)
{
	unsigned char buf[256], *b = buf;
	unsigned short attr = scr_readw(s);
	unsigned char save;
	int i, last = 0;

	if (vc->display_fg->vt_blanked)
		return;
	
	if (count <= 0)
		return;

	b += promcon_start(vc, b);

	if (x + count >= pw + 1) {
		if (count == 1) {
			x -= 1;
			save = *(unsigned short *)(vc->vc_origin
						   + y * vc->vc_size_row
						   + (x << 1));

			if (px != x || py != y) {
				b += sprintf(b, "\033[%d;%dH", y + 1, x + 1);
				px = x;
				py = y;
			}

			if (inverted(attr))
				b += sprintf(b, "\033[7m%c\033[m", scr_readw(s++));
			else
				b += sprintf(b, "%c", scr_readw(s++));

			strcpy(b, "\b\033[@");
			b += 4;

			if (inverted(save))
				b += sprintf(b, "\033[7m%c\033[m", save);
			else
				b += sprintf(b, "%c", save);

			px++;

			b += promcon_end(vc, b);
			promcon_puts(buf, b - buf);
			return;
		} else {
			last = 1;
			count = pw - x - 1;
		}
	}

	if (inverted(attr)) {
		strcpy(b, "\033[7m");
		b += 4;
	}

	if (px != x || py != y) {
		b += sprintf(b, "\033[%d;%dH", y + 1, x + 1);
		px = x;
		py = y;
	}

	for (i = 0; i < count; i++) {
		if (b - buf >= 224) {
			promcon_puts(buf, b - buf);
			b = buf;
		}
		*b++ = scr_readw(s++);
	}

	px += count;

	if (last) {
		save = scr_readw(s++);
		b += sprintf(b, "%c\b\033[@%c", scr_readw(s++), save);
		px++;
	}

	if (inverted(attr)) {
		strcpy(b, "\033[m");
		b += 3;
	}

	b += promcon_end(vc, b);
	promcon_puts(buf, b - buf);
}

static void
promcon_putc(struct vc_data *vc, int c, int y, int x)
{
	unsigned short s;

	if (vc->display_fg->vt_blanked)
		return;
	
	scr_writew(c, &s);
	promcon_putcs(vc, &s, 1, y, x);
}

static void
promcon_clear(struct vc_data *vc, int sy, int sx, int height, int width)
{
	unsigned char buf[256], *b = buf;
	int i, j;

	if (vc->display_fg->vt_blanked)
		return;
	
	b += promcon_start(vc, b);

	if (!sx && width == pw + 1) {

		if (!sy && height == ph + 1) {
			strcpy(b, "\033[H\033[J");
			b += 6;
			b += promcon_end(vc, b);
			promcon_puts(buf, b - buf);
			return;
		} else if (sy + height == ph + 1) {
			b += sprintf(b, "\033[%dH\033[J", sy + 1);
			b += promcon_end(vc, b);
			promcon_puts(buf, b - buf);
			return;
		}

		b += sprintf(b, "\033[%dH", sy + 1);
		for (i = 1; i < height; i++) {
			strcpy(b, "\033[K\n");
			b += 4;
		}

		strcpy(b, "\033[K");
		b += 3;

		b += promcon_end(vc, b);
		promcon_puts(buf, b - buf);
		return;

	} else if (sx + width == pw + 1) {

		b += sprintf(b, "\033[%d;%dH", sy + 1, sx + 1);
		for (i = 1; i < height; i++) {
			strcpy(b, "\033[K\n");
			b += 4;
		}

		strcpy(b, "\033[K");
		b += 3;

		b += promcon_end(vc, b);
		promcon_puts(buf, b - buf);
		return;
	}

	for (i = sy + 1; i <= sy + height; i++) {
		b += sprintf(b, "\033[%d;%dH", i, sx + 1);
		for (j = 0; j < width; j++)
			*b++ = ' ';
		if (b - buf + width >= 224) {
			promcon_puts(buf, b - buf);
			b = buf;
		}
	}

	b += promcon_end(vc, b);
	promcon_puts(buf, b - buf);
}
                        
static void
promcon_bmove(struct vc_data *vc, int sy, int sx, int dy, int dx,
	      int height, int width)
{
	char buf[256], *b = buf;

	if (vc->display_fg->vt_blanked)
		return;
	
	b += promcon_start(vc, b);
	if (sy == dy && height == 1) {
		if (dx > sx && dx + width == vc->vc_cols)
			b += sprintf(b, "\033[%d;%dH\033[%d@\033[%d;%dH",
				     sy + 1, sx + 1, dx - sx, py + 1, px + 1);
		else if (dx < sx && sx + width == vc->vc_cols)
			b += sprintf(b, "\033[%d;%dH\033[%dP\033[%d;%dH",
				     dy + 1, dx + 1, sx - dx, py + 1, px + 1);

		b += promcon_end(vc, b);
		promcon_puts(buf, b - buf);
		return;
	}

	/*
	 * FIXME: What to do here???
	 * Current console.c should not call it like that ever.
	 */
	prom_printf("\033[7mFIXME: bmove not handled\033[m\n");
}

static void
promcon_cursor(struct vc_data *vc, int mode)
{
	char buf[32], *b = buf;

	switch (mode) {
	case CM_ERASE:
		break;

	case CM_MOVE:
	case CM_DRAW:
		b += promcon_start(vc, b);
		if (px != vc->vc_x || py != vc->vc_y) {
			px = vc->vc_x;
			py = vc->vc_y;
			b += sprintf(b, "\033[%d;%dH", py + 1, px + 1);
		}
		promcon_puts(buf, b - buf);
		break;
	}
}

static int
promcon_font_op(struct vc_data *vc, struct console_font_op *op)
{
	return -ENOSYS;
}
        
static int
promcon_blank(struct vc_data *vc, int blank)
{
	if (blank) {
		promcon_puts("\033[H\033[J\033[7m \033[m\b", 15);
		return 0;
	} else {
		/* Let console.c redraw */
		return 1;
	}
}

static int
promcon_scroll(struct vc_data *vc, int t, int b, int dir, int count)
{
	unsigned char buf[256], *p = buf;
	unsigned short *s;
	int i;

	if (vc->display_fg->vt_blanked)
		return 0;
	
	p += promcon_start(vc, p);

	switch (dir) {
	case SM_UP:
		if (b == ph + 1) {
			p += sprintf(p, "\033[%dH\033[%dM", t + 1, count);
			px = 0;
			py = t;
			p += promcon_end(vc, p);
			promcon_puts(buf, p - buf);
			break;
		}

		s = (unsigned short *)((t + count) * vc->vc_size_row +
					vc->vc_origin);

		p += sprintf(p, "\033[%dH", t + 1);

		for (i = t; i < b - count; i++)
			s = promcon_repaint_line(s, buf, &p);

		for (; i < b - 1; i++) {
			strcpy(p, "\033[K\n");
			p += 4;
			if (p - buf >= 224) {
				promcon_puts(buf, p - buf);
				p = buf;
			}
		}

		strcpy(p, "\033[K");
		p += 3;

		p += promcon_end(vc, p);
		promcon_puts(buf, p - buf);
		break;

	case SM_DOWN:
		if (b == ph + 1) {
			p += sprintf(p, "\033[%dH\033[%dL", t + 1, count);
			px = 0;
			py = t;
			p += promcon_end(vc, p);
			promcon_puts(buf, p - buf);
			break;
		}

		s = (unsigned short *)(vc->vc_origin + t * vc->vc_size_row);

		p += sprintf(p, "\033[%dH", t + 1);

		for (i = t; i < t + count; i++) {
			strcpy(p, "\033[K\n");
			p += 4;
			if (p - buf >= 224) {
				promcon_puts(buf, p - buf);
				p = buf;
			}
		}

		for (; i < b; i++)
			s = promcon_repaint_line(s, buf, &p);

		p += promcon_end(vc, p);
		promcon_puts(buf, p - buf);
		break;
	}

	return 0;
}

#if !(PROMCON_COLOR)
static u8 promcon_build_attr(struct vc_data *vc, u8 _color, u8 _intensity, u8 _blink, u8 _underline, u8 _reverse)
{
	return (_reverse) ? 0xf : 0x7;
}
#endif

/*
 *  The console 'switch' structure for the VGA based console
 */

static int promcon_dummy(void)
{
        return 0;
}

#define DUMMY (void *) promcon_dummy

const struct consw prom_con = {
	con_startup:		promcon_startup,
	con_init:		promcon_init,
	con_deinit:		promcon_deinit,
	con_clear:		promcon_clear,
	con_putc:		promcon_putc,
	con_putcs:		promcon_putcs,
	con_cursor:		promcon_cursor,
	con_scroll:		promcon_scroll,
	con_bmove:		promcon_bmove,
	con_switch:		promcon_switch,
	con_blank:		promcon_blank,
	con_font_op:		promcon_font_op,
	con_resize:		DUMMY,
	con_set_palette:	DUMMY,
	con_scrolldelta:	DUMMY,
#if !(PROMCON_COLOR)
	con_build_attr:		promcon_build_attr,
#endif
};

void __init prom_con_init(void)
{
	const char *display_desc = NULL;
        struct vt_struct *vt;
	struct vc_data *vc;
	long q;

        /* Alloc the mem we need */
	vt = (struct vt_struct *) kmalloc(sizeof(struct vt_struct),GFP_KERNEL);
        if (!vt) return;
	vc = (struct vc_data *) kmalloc(sizeof(struct vc_data), GFP_KERNEL);
	vt->kmalloced = 1;
	vt->vt_sw = &prom_con;
	vt->vcs.vc_cons[0] = vc;
        display_desc = create_vt(vt, 0);
	q = (long) kmalloc(vc->vc_screenbuf_size, GFP_KERNEL);       
	if (!display_desc || !q) {
		kfree(vt->vcs.vc_cons[0]);
                kfree(vt);
		if (q)
			kfree((char *) q);		
                return;
        }
	vc->vc_screenbuf = (unsigned short *) q;
	vc_init(vc, 1);
	promcon_init_unimap(vt->fg_console);
}
