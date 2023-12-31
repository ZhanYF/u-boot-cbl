// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2018 Ondrej Jirman <megous@megous.com>
 */
#include <common.h>
#include <bootflow.h>
#include <bootstd.h>
#include <command.h>
#include <cli_hush.h>
#include <video.h>
#include <video_font.h>
#include <dm/uclass.h>
#include <dm/device.h>
#include <linux/delay.h>
#include <video_font_8x16.h>
#include <touchpanel.h>

#define VIDEO_FONT_HEIGHT 16
#define VIDEO_FONT_WIDTH 8
#define video_fontdata video_fontdata_8x16

// first some generic drawing primitives

struct painter {
	u8* fb;
	u8* fb_end;
	u8* cur;
	u32 line_length;
	u32 bpp;
	u32 rows;
	u32 cols;
};

static void painter_set_xy(struct painter* p, uint x, uint y)
{
	p->cur = p->fb + min(y, p->rows - 1) * p->line_length + min(x, p->cols - 1) * p->bpp;
}

static void painter_move_dxy(struct painter* p, int dx, int dy)
{
	p->cur += dy * p->line_length + dx * p->bpp;

	if (p->cur >= p->fb_end)
		p->cur = p->fb_end - 1;

	if (p->cur < p->fb)
		p->cur = p->fb;
}

static void painter_rect_fill(struct painter* p, uint w, uint h, u32 color)
{
	int x, y;
	u32* cur;

	for (y = 0; y < h; y++) {
		cur = (u32*)(p->cur + p->line_length * y);

		for (x = 0; x < w; x++)
			*(cur++) = color;
	}
}

static void painter_line_h(struct painter* p, int dx, u32 color)
{
	if (dx < 0) {
		painter_move_dxy(p, 0, dx);
		painter_rect_fill(p, 1, -dx, color);
	} else {
		painter_rect_fill(p, 1, dx, color);
		painter_move_dxy(p, 0, dx);
	}
}

static void painter_line_v(struct painter* p, int dy, u32 color)
{
	if (dy < 0) {
		painter_move_dxy(p, 0, dy);
		painter_rect_fill(p, 1, -dy, color);
	} else {
		painter_rect_fill(p, 1, dy, color);
		painter_move_dxy(p, 0, dy);
	}
}

static void painter_bigchar(struct painter* p, char ch, u32 color)
{
	int i, row;
	void *line = p->cur;

	for (row = 0; row < VIDEO_FONT_HEIGHT * 2; row++) {
		uchar bits = video_fontdata[ch * VIDEO_FONT_HEIGHT + row / 2];
		uint32_t *dst = line;

		for (i = 0; i < VIDEO_FONT_WIDTH; i++) {
			if (bits & 0x80) {
				*dst = color;
				*(dst+1) = color;
			}

			bits <<= 1;
			dst+=2;
		}

		line += p->line_length;
	}

	painter_move_dxy(p, VIDEO_FONT_WIDTH * 2, 0);
}

static void painter_char(struct painter* p, char ch, u32 color)
{
	int i, row;
	void *line = p->cur;

	for (row = 0; row < VIDEO_FONT_HEIGHT; row++) {
		uchar bits = video_fontdata[ch * VIDEO_FONT_HEIGHT + row];
		uint32_t *dst = line;

		for (i = 0; i < VIDEO_FONT_WIDTH; i++) {
			if (bits & 0x80)
				*dst = color;

			bits <<= 1;
			dst++;
		}

		line += p->line_length;
	}

	painter_move_dxy(p, VIDEO_FONT_WIDTH, 0);
}

// menu command

struct ui_item {
	int x, y, w, h;
	char text[40];
	uint32_t fill;
	uint32_t text_color;
	int id;
};

static void ui_draw(struct ui_item *items, int n_items, struct painter *p)
{
	for (int idx = 0; idx < n_items; idx++) {
                struct ui_item* i = items + idx;

		painter_set_xy(p, i->x, i->y);
		painter_rect_fill(p, i->w, i->h, i->fill);

		size_t max_chars = i->w / (VIDEO_FONT_WIDTH * 2) - 1;

		max_chars = min(max_chars, strlen(i->text));

		int text_w = i->w - max_chars * VIDEO_FONT_WIDTH * 2;
		int text_h = i->h - VIDEO_FONT_HEIGHT * 2;

		painter_set_xy(p,
			       i->x + text_w / 2,
			       i->y + text_h / 2);

		for (int j = 0; j < max_chars; j++)
			painter_bigchar(p, i->text[j], i->text_color);
	}
}

static struct ui_item* ui_hit_find(struct ui_item* items, int n_items, int x, int y)
{
	for (int idx = 0; idx < n_items; idx++) {
                struct ui_item* i = items + idx;

		if (x >= i->x && x <= i->x + i->w && y >= i->y && y <= i->y + i->h)
			return i;
	}

	return NULL;
}

static int handle_tmenu(struct cmd_tbl *cmdtp, int flag, int argc, char *const argv[], int no_touch)
{
	struct udevice *vdev, *tdev;
	struct video_priv *vpriv;
	struct touchpanel_touch touches[10];
	int ret;

	if (argc < 2)
		return CMD_RET_USAGE;

        // set some params: (parse from argv in the future)
	char* const* items = argv + 1;
	int n_items = argc - 1;

	ret = uclass_first_device_err(UCLASS_VIDEO, &vdev);
	if (ret)
		return CMD_RET_FAILURE;

	if (!no_touch) {
		ret = uclass_first_device_err(UCLASS_TOUCHPANEL, &tdev);
		if (ret)
			return CMD_RET_FAILURE;
	}

	vpriv = dev_get_uclass_priv(vdev);

	if (vpriv->bpix != VIDEO_BPP32) {
		printf("tmenu requires 32BPP video device\n");
		return CMD_RET_FAILURE;
	}

        struct painter p = {
		.fb = vpriv->fb,
		.fb_end = vpriv->fb + vpriv->fb_size,
		.cur = vpriv->fb,
		.line_length = vpriv->line_length,
		.bpp = VNBYTES(vpriv->bpix),
		.cols = vpriv->xsize,
		.rows = vpriv->ysize,
	};

	// prepare ui_items and lay them out
	struct ui_item ui_items[n_items];
        int border = 40, max_total_h = min(800, vpriv->ysize - 2 * border);
        int gap = 10;

        int cols = 1;
        while ((max_total_h - gap * (DIV_ROUND_UP(n_items, cols) - 1)) / DIV_ROUND_UP(n_items, cols) < 100)
                cols++;
        int rows = DIV_ROUND_UP(n_items, cols);

        int item_h = (max_total_h - gap * (DIV_ROUND_UP(n_items, cols) - 1)) / rows;
        int item_w = (vpriv->xsize - 2 * border - (cols - 1) * gap) / cols;

        int top = vpriv->ysize - border - rows * (item_h + gap) - gap;

        for (int idx = 0; idx < n_items; idx++) {
                struct ui_item *i = ui_items + idx;

                int col = idx % cols;
                int row = idx / cols;

                i->x = border + col * (gap + item_w);
                i->y = top + row * (gap + item_h);
                i->w = item_w;
                i->h = item_h;
                i->fill = 0xff755f10;
                i->text_color = 0xffffffff;
                i->id = idx;

		snprintf(i->text, sizeof i->text, "%s", items[idx]);

		//printf("ui_item[%d] x=%d y=%d w=%d h=%d text=%s\n", idx, i->x, i->y, i->w, i->h, i->text);
	}

	int selected = -1;
	int redraw = 1;

	if (!no_touch) {
		ret = touchpanel_start(tdev);
		if (ret < 0) {
			printf("Failed to start %s, err=%d\n", tdev->name, ret);
			return CMD_RET_FAILURE;
		}
	}

next:
	while (1) {
		if (redraw) {
			ui_draw(ui_items, n_items, &p);
			video_sync(vdev, true);
			redraw = 0;
		}

		if (no_touch)
			return CMD_RET_SUCCESS;

		// don't be too busy reading i2c
		udelay(50 * 1000);

		// handle input
		ret = touchpanel_get_touches(tdev, touches, ARRAY_SIZE(touches));
		if (ret < 0) {
			printf("Failed to get touches from %s, err=%d\n", tdev->name, ret);
			return CMD_RET_FAILURE;
		}

		/* find first matching tap down */
		for (int idx = 0; idx < ret; idx++) {
			int tx = touches[idx].x;
			int ty = touches[idx].y;

			struct ui_item* hit = ui_hit_find(ui_items, n_items, tx, ty);
			if (hit) {
				selected = hit->id;
				hit->fill = 0xffb19019;
				redraw = 1;
				goto next;
			}
		}

		if (selected != -1) {
			// we are done
			char buf[16];
			snprintf(buf, sizeof buf, "ret=%d", selected);
			set_local_var(buf, 1);
			selected = -1;
			redraw = 1;
			break;
		}
	}

	ret = touchpanel_stop(tdev);
	if (ret < 0) {
		printf("Failed to stop %s, err=%d\n", tdev->name, ret);
		return CMD_RET_FAILURE;
	}

	return CMD_RET_SUCCESS;
}

static int do_tmenu_render(struct cmd_tbl *cmdtp, int flag, int argc, char *const argv[])
{
	return handle_tmenu(cmdtp, flag, argc, argv, 1);
}

static int do_tmenu_input(struct cmd_tbl *cmdtp, int flag, int argc, char *const argv[])
{
	return handle_tmenu(cmdtp, flag, argc, argv, 0);
}

static int do_tmenu(struct cmd_tbl *cmdtp, int flag, int argc, char *const argv[])
{
	return handle_tmenu(cmdtp, flag, argc, argv, 0);
}

U_BOOT_CMD(tmenu, 30, 1, do_tmenu, "tmenu", "tmenu item1 [item2...] - show touch menu and wait for input");
U_BOOT_CMD(tmenu_render, 30, 1, do_tmenu_render, "tmenu_render", "tmenu_render item1 [item2...] - show touch menu");
U_BOOT_CMD(tmenu_input, 30, 1, do_tmenu_input, "tmenu_input", "tmenu_input item1 [item2...] - wait for touch menu input");

#include <pxe_utils.h>
#include <extlinux.h>
#include <mapmem.h>
#include <stdlib.h>
#include <image.h>
#include <splash.h>
#include <sysreset.h>
#include <video_console.h>

int pxe_label_boot(struct pxe_context *ctx, struct pxe_label *label);

int extlinux_getfile(struct pxe_context *ctx, const char *file_path,
			    char *file_addr, ulong *sizep);

enum {
	ACTION_BOOT = 1,
	ACTION_POWEROFF,
	ACTION_CONSOLE,
	ACTION_USB_STORAGE,
};

struct tmenu_boot_item {
	int id;
	char* label;
	struct bootflow *bflow;

	struct pxe_context *pxe_ctx;
	struct pxe_label *pxe_label;
	int action;
};

static int do_tmenu_bootflow(struct cmd_tbl *cmdtp, int flag, int argc, char *const argv[])
{
	struct udevice *vdev, *tdev, *cdev;
	struct video_priv *vpriv;
	struct touchpanel_touch touches[10];
	int cmd_ret;
	int ret;

start_again:
	cmd_ret = CMD_RET_FAILURE;

	struct bootstd_priv *std;
	ret = bootstd_get_priv(&std);
	if (ret)
		return CMD_RET_FAILURE;

	// how many items to reserve
	int extra_items = 3;

	struct tmenu_boot_item items[64] = {};
	int n_items = 0;
	int bmp_loaded = 0;

	struct bootflow *bflow;
	for (ret = bootflow_first_glob(&bflow); !ret; ret = bootflow_next_glob(&bflow)) {
		if (bflow->state == BOOTFLOWST_READY) {
			//printf("flow: (%s) %s - %s\n", dev_get_parent(bflow->dev)->name, bflow->os_name ? bflow->os_name : bflow->name, bflow->method->name);

			if (!strcmp(bflow->method->name, "extlinux")) {
				struct cmd_tbl *cmdtp = calloc(1, sizeof(*cmdtp));
				struct extlinux_info *pxe_info = calloc(1, sizeof(*pxe_info));
				struct pxe_context *ctx = calloc(1, sizeof(*ctx));

				if (!ctx || !cmdtp ||!pxe_info)
					continue;

				ulong addr = map_to_sysmem(bflow->buf);
				pxe_info->dev = bflow->method;
				pxe_info->bflow = bflow;
				int ret = pxe_setup_ctx(ctx, cmdtp, extlinux_getfile, pxe_info, true, bflow->fname, false);
				if (ret)
					continue;

				struct pxe_menu *cfg = parse_pxefile(ctx, addr);
				if (!cfg) {
					printf("Error parsing config file\n");
					continue;
				}

				if (cfg->bmp && !bmp_loaded && get_pxe_file(ctx, cfg->bmp, image_load_addr) == 1)
					bmp_loaded = 1;

				struct list_head *pos;
				list_for_each(pos, &cfg->labels) {
					struct pxe_label *label = list_entry(pos, struct pxe_label, list);
					struct tmenu_boot_item *it = &items[n_items++];

					//printf("label %s - %s %s\n", label->num, label->name, label->menu);

					it->id = n_items;
					it->label = label->menu ? label->menu : label->name;
					it->bflow = bflow;
					it->pxe_ctx = ctx;
					it->pxe_label = label;
					it->action = ACTION_BOOT;

					if (n_items == (ARRAY_SIZE(items) - extra_items))
						break;
				}
			} else {
				struct tmenu_boot_item *it = &items[n_items++];

				it->id = n_items;
				it->label = strdup(bflow->os_name ? bflow->os_name : bflow->name);
				it->bflow = bflow;
				it->action = ACTION_BOOT;
			}

			if (n_items == (ARRAY_SIZE(items) - extra_items))
				break;
		}
	}

	struct tmenu_boot_item *it = &items[n_items++];

	it->id = n_items;
	it->label = "U-Boot Console";
	it->action = ACTION_CONSOLE;

	it = &items[n_items++];

	it->id = n_items;
	it->label = "USB access to eMMC";
	it->action = ACTION_USB_STORAGE;

	it = &items[n_items++];

	it->id = n_items;
	it->label = "Power off";
	it->action = ACTION_POWEROFF;

	ret = uclass_first_device_err(UCLASS_VIDEO, &vdev);
	if (ret)
		return CMD_RET_FAILURE;

	ret = uclass_first_device_err(UCLASS_TOUCHPANEL, &tdev);
	if (ret)
		return CMD_RET_FAILURE;

	vpriv = dev_get_uclass_priv(vdev);

	if (vpriv->bpix != VIDEO_BPP32) {
		printf("tmenu requires 32BPP video device\n");
		return CMD_RET_FAILURE;
	}

        /* prep done, start doing the UI work */

	env_set("stdout", "serial");
	env_set("stderr", "serial");

	if (bmp_loaded) {
		video_clear(vdev);
		bmp_display(image_load_addr, BMP_ALIGN_CENTER, BMP_ALIGN_CENTER);
	}

        struct painter p = {
		.fb = vpriv->fb,
		.fb_end = vpriv->fb + vpriv->fb_size,
		.cur = vpriv->fb,
		.line_length = vpriv->line_length,
		.bpp = VNBYTES(vpriv->bpix),
		.cols = vpriv->xsize,
		.rows = vpriv->ysize,
	};

	// prepare ui_items and lay them out
	struct ui_item ui_items[n_items];
        int border = 40, max_total_h = min(700, vpriv->ysize - 2 * border);
        int gap = 10;

        int cols = 1;
        while ((max_total_h - gap * (DIV_ROUND_UP(n_items, cols) - 1)) / DIV_ROUND_UP(n_items, cols) < 100)
                cols++;
        int rows = DIV_ROUND_UP(n_items, cols);

        int item_h = (max_total_h - gap * (DIV_ROUND_UP(n_items, cols) - 1)) / rows;
        int item_w = (vpriv->xsize - 2 * border - (cols - 1) * gap) / cols;

        int top = vpriv->ysize - border - rows * (item_h + gap) - gap;

        for (int idx = 0; idx < n_items; idx++) {
                struct ui_item *i = ui_items + idx;

                int col = idx % cols;
                int row = idx / cols;

                i->x = border + col * (gap + item_w);
                i->y = top + row * (gap + item_h);
                i->w = item_w;
                i->h = item_h;
                i->fill = 0xff755f10;
                i->text_color = 0xffffffff;
                i->id = idx;

		snprintf(i->text, sizeof i->text, "%s", items[idx].label);

		//printf("ui_item[%d] x=%d y=%d w=%d h=%d text=%s\n", idx, i->x, i->y, i->w, i->h, i->text);
	}

	int selected = -1;
	int redraw = 1;

	ret = touchpanel_start(tdev);
	if (ret < 0) {
		printf("Failed to start %s, err=%d\n", tdev->name, ret);
		goto out_restore_console;
	}

next:
	while (1) {
		if (redraw) {
			ui_draw(ui_items, n_items, &p);
			video_sync(vdev, true);
			redraw = 0;
		}

		// don't be too busy reading i2c
		udelay(50 * 1000);

		// handle input
		ret = touchpanel_get_touches(tdev, touches, ARRAY_SIZE(touches));
		if (ret < 0) {
			printf("Failed to get touches from %s, err=%d\n", tdev->name, ret);
			break;
		}

		/* find first matching tap down */
		for (int idx = 0; idx < ret; idx++) {
			int tx = touches[idx].x;
			int ty = touches[idx].y;

			struct ui_item* hit = ui_hit_find(ui_items, n_items, tx, ty);
			if (hit) {
				selected = hit->id;
				hit->fill = 0xffb19019;
				redraw = 1;
				goto next;
			}
		}

		if (selected != -1) {
			struct tmenu_boot_item *it = &items[selected];

			if (it->action == ACTION_CONSOLE) {
				cmd_ret = CMD_RET_SUCCESS;
				break;
			}

			else if (it->action == ACTION_USB_STORAGE) {
				cmd_ret = CMD_RET_SUCCESS;
				break;
			}

			else if (it->action == ACTION_POWEROFF) {
				ret = sysreset_walk(SYSRESET_POWER_OFF);
				if (ret == -EINPROGRESS)
					mdelay(1000);
				break;
			}

			if (bmp_loaded) {
				video_clear(vdev);
				bmp_display(image_load_addr, BMP_ALIGN_CENTER, BMP_ALIGN_CENTER);
			}

			std->cur_bootflow = it->bflow;

			if (it->pxe_label) {
				pxe_label_boot(it->pxe_ctx, it->pxe_label);
			} else {
				bootflow_boot(it->bflow);
			}

			selected = -1;
			redraw = 1;
			break;
		}
	}

out_stop_touch:
	ret = touchpanel_stop(tdev);
	if (ret < 0)
		printf("Failed to stop %s, err=%d\n", tdev->name, ret);

out_restore_console:
	//video_clear(vdev);
	if (!uclass_first_device_err(UCLASS_VIDEO_CONSOLE, &cdev))
		vidconsole_clear_and_reset(cdev);

	env_set("stdout", "serial,vidconsole");
	env_set("stderr", "serial,vidconsole");

	if (selected >= 0) {
		struct tmenu_boot_item *it = &items[selected];

		if (it->action == ACTION_USB_STORAGE) {
			cli_simple_run_command("ums 0 mmc 0", 0);
			cli_simple_run_command("bootflow scan", 0);
			goto start_again;
		}
	}

	return cmd_ret;
}

U_BOOT_CMD(tmenu_bootflow, 4, 1, do_tmenu_bootflow, "tmenu_bootflow", "tmenu_bootflow - show bootflow menu");
