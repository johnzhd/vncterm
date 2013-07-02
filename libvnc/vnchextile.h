/*
 * QEMU VNC display driver
 * 
 * Copyright (C) 2006 Anthony Liguori <anthony@codemonkey.ws>
 * Copyright (C) 2006 Fabrice Bellard
 * Copyright (C) 2006 Christian Limpach <Christian.Limpach@xensource.com>
 * 
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#define CONCAT_I(a, b) a ## b
#define CONCAT(a, b) CONCAT_I(a, b)
#define pixel_t CONCAT(uint, CONCAT(BPP, _t))
#ifdef GENERIC
#define NAME CONCAT(generic_, BPP)
#else
#define NAME BPP
#endif

static void CONCAT(send_hextile_tile_, NAME)(struct VncClientState *vcs,
                                             uint8_t *data, int stride,
                                             int w, int h,
                                             void *last_bg_, 
                                             void *last_fg_,
                                             int *has_bg, int *has_fg)
{
    struct VncState *vs = vcs->vs;
    pixel_t *irow = (pixel_t *)data;
    int j, i;
    pixel_t *last_bg = (pixel_t *)last_bg_;
    pixel_t *last_fg = (pixel_t *)last_fg_;
    pixel_t bg = 0;
    pixel_t fg = 0;
    int n_colors = 0;
    int bg_count = 0;
    int fg_count = 0;
    int flags = 0;
    uint8_t pdata[(vcs->pix_bpp + 2) * 16 * 16];
    int n_pdata = 0;
    int n_subtiles = 0;

    for (j = 0; j < h; j++) {
	for (i = 0; i < w; i++) {
	    switch (n_colors) {
	    case 0:
		bg = irow[i];
		n_colors = 1;
		break;
	    case 1:
		if (irow[i] != bg) {
		    fg = irow[i];
		    n_colors = 2;
		}
		break;
	    case 2:
		if (irow[i] != bg && irow[i] != fg) {
		    n_colors = 3;
		} else {
		    if (irow[i] == bg)
			bg_count++;
		    else if (irow[i] == fg)
			fg_count++;
		}
		break;
	    default:
		break;
	    }
	}
	if (n_colors > 2)
	    break;
	irow += stride / sizeof(pixel_t);
    }

    if (n_colors > 1 && fg_count > bg_count) {
	pixel_t tmp = fg;
	fg = bg;
	bg = tmp;
    }

    if (!*has_bg || *last_bg != bg) {
	flags |= 0x02;
	*has_bg = 1;
	*last_bg = bg;
    }

    if (!*has_fg || *last_fg != fg) {
	flags |= 0x04;
	*has_fg = 1;
	*last_fg = fg;
    }

    switch (n_colors) {
    case 1:
	n_pdata = 0;
	break;
    case 2:
	flags |= 0x08;

	irow = (pixel_t *)data;
	
	for (j = 0; j < h; j++) {
	    int min_x = -1;
	    for (i = 0; i < w; i++) {
		if (irow[i] == fg) {
		    if (min_x == -1)
			min_x = i;
		} else if (min_x != -1) {
		    hextile_enc_cord(pdata + n_pdata, min_x, j, i - min_x, 1);
		    n_pdata += 2;
		    n_subtiles++;
		    min_x = -1;
		}
	    }
	    if (min_x != -1) {
		hextile_enc_cord(pdata + n_pdata, min_x, j, i - min_x, 1);
		n_pdata += 2;
		n_subtiles++;
	    }
	    irow += stride / sizeof(pixel_t);
	}
	break;
    case 3:
	flags |= 0x18;

	irow = (pixel_t *)data;

	if (!*has_bg || *last_bg != bg)
	    flags |= 0x02;

	for (j = 0; j < h; j++) {
	    int has_color = 0;
	    int min_x = -1;
	    pixel_t color = 0; /* shut up gcc */

	    for (i = 0; i < w; i++) {
		if (!has_color) {
		    if (irow[i] == bg)
			continue;
		    color = irow[i];
		    min_x = i;
		    has_color = 1;
		} else if (irow[i] != color) {
		    has_color = 0;
#ifdef GENERIC
            vnc_convert_pixel(vcs, pdata + n_pdata, color);
            n_pdata += vcs->pix_bpp;
#else
	        memcpy(pdata + n_pdata, &color, sizeof(color));
            n_pdata += sizeof(pixel_t);
#endif
		    hextile_enc_cord(pdata + n_pdata, min_x, j, i - min_x, 1);
		    n_pdata += 2;
		    n_subtiles++;

		    min_x = -1;
		    if (irow[i] != bg) {
    			color = irow[i];
    			min_x = i;
    			has_color = 1;
		    }
		}
	    }
	    if (has_color) {
#ifdef GENERIC
        vnc_convert_pixel(vcs, pdata + n_pdata, color);
        n_pdata += vcs->pix_bpp;
#else
        memcpy(pdata + n_pdata, &color, sizeof(color));
        n_pdata += sizeof(pixel_t);
#endif
		hextile_enc_cord(pdata + n_pdata, min_x, j, i - min_x, 1);
		n_pdata += 2;
		n_subtiles++;
	    }
	    irow += stride / sizeof(pixel_t);
	}

	/* A SubrectsColoured subtile invalidates the foreground color */
	*has_fg = 0;
	if (n_pdata > (w * h * sizeof(pixel_t))) {
	    n_colors = 4;
	    flags = 0x01;
	    *has_bg = 0;

	    /* we really don't have to invalidate either the bg or fg
	       but we've lost the old values.  oh well. */
	}
    default:
	break;
    }

    if (n_colors > 3) {
	flags = 0x01;
	*has_fg = 0;
	*has_bg = 0;
	n_colors = 4;
    }

    vnc_write_u8(vcs, flags);
    if (n_colors < 4) {
	if (flags & 0x02)
	    vcs->write_pixels(vcs, last_bg, sizeof(pixel_t));
	if (flags & 0x04)
	    vcs->write_pixels(vcs, last_fg, sizeof(pixel_t));
	if (n_subtiles) {
	    vnc_write_u8(vcs, n_subtiles);
	    vnc_write(vcs, pdata, n_pdata);
	}
    } else {
	for (j = 0; j < h; j++) {
	    vcs->write_pixels(vcs, data, w * vs->depth);
	    data += stride;
	}
    }
}

#undef NAME
#undef pixel_t
#undef CONCAT_I
#undef CONCAT
