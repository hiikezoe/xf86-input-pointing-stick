/*
 * Copyright (C) 2010 Hiroyuki Ikezoe
 *
 * Permission to use, copy, modify, distribute, and sell this software
 * and its documentation for any purpose is hereby granted without
 * fee, provided that the above copyright notice appear in all copies
 * and that both that copyright notice and this permission notice
 * appear in supporting documentation, and that the name of Red Hat
 * not be used in advertising or publicity pertaining to distribution
 * of the software without specific, written prior permission.  Red
 * Hat makes no representations about the suitability of this software
 * for any purpose.  It is provided "as is" without express or implied
 * warranty.
 *
 * THE AUTHORS DISCLAIM ALL WARRANTIES WITH REGARD TO THIS SOFTWARE,
 * INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS, IN
 * NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY SPECIAL, INDIRECT OR
 * CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS
 * OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT,
 * NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

typedef struct _PointingStickPrivateRec
{
    int x;
    int y;
    int pressure;
    Bool button_touched;
    Bool left_button;
    Bool right_button;
    Bool middle_button;
    int sensitivity;
    int speed;
    Bool scrolling;
    Bool middle_button_is_pressed;
    Time middle_button_click_expires;
    Time middle_button_timeout;
    Bool press_to_select;
    int press_to_select_threshold;
    Bool press_to_selecting;
    Bool has_abs_events;
    Bool is_trackpoint;
    char *trackpoint_sysfs_path;
} PointingStickPrivate;
/*
vi:ts=4:nowrap:ai:expandtab:sw=4
*/
