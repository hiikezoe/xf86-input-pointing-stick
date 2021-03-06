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

/* Based on xf86-input-random/src/random.c */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <linux/input.h>
#include <linux/types.h>

#include <unistd.h>
#include <errno.h>
#include <dirent.h>

#include <xf86_OSproc.h>
#include <xf86.h>
#include <xf86Xinput.h>
#include <exevents.h>
#include <xf86Module.h>
#include <X11/Xatom.h>

static Atom prop_sensitivity = 0;
static Atom prop_speed = 0;
static Atom prop_scrolling = 0;
static Atom prop_middle_button_timeout = 0;
static Atom prop_press_to_select = 0;
static Atom prop_press_to_select_threshold = 0;

#if GET_ABI_MAJOR(ABI_XINPUT_VERSION) >= 7
#include <xserver-properties.h>
#endif

#include "pointingstick.h"
#include "pointingstick-properties.h"
#include "trackpoint.h"

#define LONG_BITS (sizeof(long) * 8)
#define NLONGS(x) (((x) + LONG_BITS - 1) / LONG_BITS)
#define TestBit(bit, array) ((array[(bit) / LONG_BITS]) & (1L << ((bit) % LONG_BITS)))
#define SYSCALL(call) while (((call) == -1) && (errno == EINTR))

static int          pre_init       (InputDriverPtr drv,
                                    InputInfoPtr info,
                                    int flags);
static void         uninit         (InputDriverPtr drv,
                                    InputInfoPtr pInfo,
                                    int flags);
static pointer      plug           (pointer module,
                                    pointer options,
                                    int *errmaj,
                                    int  *errmin);
static void         unplug         (pointer module);
static void         read_input     (InputInfoPtr pInfo);
static int          device_control (DeviceIntPtr device,
                                    int what);

_X_EXPORT InputDriverRec POINTINGSTICK = {
    1,
    "pointingstick",
    NULL,
    pre_init,
    uninit,
    NULL,
    0
};

static XF86ModuleVersionInfo version_rec = {
    "pointingstick",
    MODULEVENDORSTRING,
    MODINFOSTRING1,
    MODINFOSTRING2,
    XORG_VERSION_CURRENT,
    PACKAGE_VERSION_MAJOR, PACKAGE_VERSION_MINOR,
    PACKAGE_VERSION_PATCHLEVEL,
    ABI_CLASS_XINPUT,
    ABI_XINPUT_VERSION,
    MOD_CLASS_XINPUT,
    {0, 0, 0, 0}
};

_X_EXPORT XF86ModuleData pointingstickModuleData = {
    &version_rec,
    &plug,
    &unplug
};

static void
unplug (pointer module)
{
}

static pointer
plug (pointer        module,
      pointer        options,
      int            *errmaj,
      int            *errmin)
{
    xf86AddInputDriver(&POINTINGSTICK, module, 0);
    return module;
}

static Bool
is_pointingstick (InputInfoPtr local)
{
    int rc;
    unsigned long evbits[NLONGS(EV_MAX)] = {0};
    PointingStickPrivate *priv = local->private;

    SYSCALL(rc = ioctl(local->fd, EVIOCGBIT(0, sizeof(evbits)), evbits));
    if (rc < 0)
        return FALSE;
    if (!TestBit(EV_SYN, evbits) ||
        !TestBit(EV_KEY, evbits)) {
        return FALSE;
    }

    if (TestBit(EV_ABS, evbits)) {
        unsigned long absbits[NLONGS(ABS_MAX)] = {0};
        SYSCALL(rc = ioctl(local->fd, EVIOCGBIT(EV_ABS, sizeof(absbits)), absbits));
        if (rc < 0)
            return FALSE;

        if (!TestBit(ABS_X, absbits) ||
            !TestBit(ABS_Y, absbits) ||
            !TestBit(ABS_PRESSURE, absbits)) {
            return FALSE;
        }
        priv->has_abs_events = TRUE;
    } else if (TestBit(EV_REL, evbits)) {
        unsigned long relbits[NLONGS(REL_MAX)] = {0};
        SYSCALL(rc = ioctl(local->fd, EVIOCGBIT(EV_REL, sizeof(relbits)), relbits));
        if (rc < 0)
            return FALSE;

        if (!TestBit(REL_X, relbits) ||
            !TestBit(REL_Y, relbits)) {
            return FALSE;
        }
        priv->has_abs_events = FALSE;
        priv->pressure = 1;
    } else {
        return FALSE;
    }

    priv->is_trackpoint = pointingstick_is_trackpoint(local);

    return TRUE;
}

static void
set_default_values (InputInfoPtr local)
{
    PointingStickPrivate *priv = local->private;
    int sensitivity, speed = -1;
    int press_to_select_threshold;
    Bool press_to_select;

    priv->trackpoint_sysfs_path = NULL;

    if (priv->is_trackpoint) {
        sensitivity = trackpoint_get_sensitivity(local);
        speed = trackpoint_get_speed(local);
        press_to_select = trackpoint_get_press_to_select(local);
        press_to_select_threshold = trackpoint_get_press_to_select_threshold(local);
    } else {
        if (priv->has_abs_events)
            sensitivity = 100;
        else
            sensitivity = 255;
        press_to_select = FALSE;
        press_to_select_threshold = 8;
    }

    priv->sensitivity = xf86SetIntOption(local->options, "Sensitivity", sensitivity);
    if (speed > 0)
        priv->speed = xf86SetIntOption(local->options, "Speed", speed);
    priv->middle_button_timeout = xf86SetIntOption(local->options, "MiddleButtonTimeout", 100);
    priv->press_to_select = xf86SetBoolOption(local->options, "PressToSelect", press_to_select);
    priv->press_to_select_threshold = xf86SetIntOption(local->options,
                                                       "PressToSelectThreshold",
                                                       press_to_select_threshold);

    priv->scrolling = TRUE;
    priv->middle_button_is_pressed = FALSE;
    priv->press_to_selecting = FALSE;
    priv->button_touched = FALSE;
}

static int
pre_init(InputDriverPtr  drv,
         InputInfoPtr    info,
         int             flags)
{
    PointingStickPrivate *priv;

    priv = calloc(1, sizeof(PointingStickPrivate));
    if (!priv)
        goto end;

    info->private = priv;
    info->type_name = "POINTINGSTICK";
    info->read_input = read_input; /* new data avl */
    info->switch_mode = NULL; /* toggle absolute/relative mode */
    info->device_control = device_control; /* enable/disable dev */

    xf86ProcessCommonOptions(info, info->options);

    info->fd = xf86OpenSerial(info->options);
    if (info->fd == -1)
        goto end;

    if (!is_pointingstick(info))
        goto end;

    set_default_values(info);

    xf86Msg(X_PROBED, "%s found\n", info->name);

    return Success;

 end:
    if (info->fd != -1) {
        xf86CloseSerial(info->fd);
        info->fd = -1;
    }

    free(priv);
    info->private = NULL;

    return BadAlloc;
}

static void
uninit (InputDriverPtr drv,
        InputInfoPtr   local,
        int            flags)
{
    free(local->private);
    local->private = NULL;
    xf86DeleteInput(local, 0);
}

static void
pointingstick_control (DeviceIntPtr device, PtrCtrl *ctrl)
{
}

static int
set_property(DeviceIntPtr device,
             Atom atom,
             XIPropertyValuePtr val,
             BOOL checkonly)
{
    InputInfoPtr local = device->public.devicePrivate;
    PointingStickPrivate *priv = local->private;

    if (atom == prop_sensitivity) {
        int sensitivity;

        if (val->format != 8 || val->size != 1 || val->type != XA_INTEGER)
            return BadMatch;

        sensitivity = *((CARD8*)val->data);
        if (sensitivity < 1 || sensitivity > 255)
            return BadValue;

        if (!checkonly) {
            if (priv->is_trackpoint)
                trackpoint_set_sensitivity(local, sensitivity);
            priv->sensitivity = sensitivity;
        }
    }

    if (priv->is_trackpoint && atom == prop_speed) {
        int speed;

        if (val->format != 8 || val->size != 1 || val->type != XA_INTEGER)
            return BadMatch;

        speed = *((CARD8*)val->data);
        if (speed < 1 || speed > 255)
            return BadValue;

        if (!checkonly) {
            if (priv->is_trackpoint)
                trackpoint_set_speed(local, speed);
            priv->speed = speed;
        }
    }

    if (atom == prop_scrolling) {
        if (val->format != 8 || val->size != 1 || val->type != XA_INTEGER)
            return BadMatch;

        if (!checkonly)
            priv->scrolling = *((BOOL*)val->data);
    }

    if (atom == prop_middle_button_timeout) {
        int timeout;

        if (val->format != 16 || val->size != 1 || val->type != XA_INTEGER)
            return BadMatch;

        timeout = *((CARD16*)val->data);
        if (timeout < 0)
            return BadValue;

        if (!checkonly)
            priv->middle_button_timeout = timeout;
    }

    if (atom == prop_press_to_select) {
        if (val->format != 8 || val->size != 1 || val->type != XA_INTEGER)
            return BadMatch;

        if (!checkonly) {
            if (priv->is_trackpoint)
                trackpoint_set_press_to_select(local, *((BOOL*)val->data));
            priv->press_to_select = *((BOOL*)val->data);
        }
    }

    if (atom == prop_press_to_select_threshold) {
        int threshold;

        if (val->format != 8 || val->size != 1 || val->type != XA_INTEGER)
            return BadMatch;

        threshold = *((CARD8*)val->data);
        if (threshold < 1 || threshold > 127)
            return BadValue;

        if (!checkonly) {
            if (priv->is_trackpoint)
                trackpoint_set_press_to_select_threshold(local, threshold);
            priv->press_to_select_threshold = threshold;
        }
    }

    return Success;
}

static void
init_properties (DeviceIntPtr device)
{
    InputInfoPtr local = device->public.devicePrivate;
    PointingStickPrivate *priv = local->private;
    int rc;

    prop_sensitivity = MakeAtom(POINTINGSTICK_PROP_SENSITIVITY,
                                strlen(POINTINGSTICK_PROP_SENSITIVITY), TRUE);
    rc = XIChangeDeviceProperty(device, prop_sensitivity, XA_INTEGER, 8,
                                PropModeReplace, 1,
                                &priv->sensitivity,
                                FALSE);
    if (rc != Success)
        return;
    XISetDevicePropertyDeletable(device, prop_sensitivity, FALSE);

    if (priv->is_trackpoint) {
        prop_speed = MakeAtom(POINTINGSTICK_PROP_SPEED,
                              strlen(POINTINGSTICK_PROP_SPEED), TRUE);
        rc = XIChangeDeviceProperty(device, prop_speed, XA_INTEGER, 8,
                                    PropModeReplace, 1,
                                    &priv->speed,
                                    FALSE);
        if (rc != Success)
            return;
        XISetDevicePropertyDeletable(device, prop_speed, FALSE);
    }

    prop_scrolling = MakeAtom(POINTINGSTICK_PROP_SCROLLING,
                              strlen(POINTINGSTICK_PROP_SCROLLING), TRUE);
    rc = XIChangeDeviceProperty(device, prop_scrolling, XA_INTEGER, 8,
                                PropModeReplace, 1,
                                &priv->scrolling,
                                FALSE);
    if (rc != Success)
        return;
    XISetDevicePropertyDeletable(device, prop_scrolling, FALSE);

    prop_middle_button_timeout = MakeAtom(POINTINGSTICK_PROP_MIDDLE_BUTTON_TIMEOUT,
                                          strlen(POINTINGSTICK_PROP_MIDDLE_BUTTON_TIMEOUT),
                                          TRUE);
    rc = XIChangeDeviceProperty(device, prop_middle_button_timeout, XA_INTEGER, 16,
                                PropModeReplace, 1,
                                &priv->middle_button_timeout,
                                FALSE);
    if (rc != Success)
        return;
    XISetDevicePropertyDeletable(device, prop_middle_button_timeout, FALSE);

    prop_press_to_select = MakeAtom(POINTINGSTICK_PROP_PRESS_TO_SELECT,
                                    strlen(POINTINGSTICK_PROP_PRESS_TO_SELECT), TRUE);
    rc = XIChangeDeviceProperty(device, prop_press_to_select, XA_INTEGER, 8,
                                PropModeReplace, 1,
                                &priv->press_to_select,
                                FALSE);
    if (rc != Success)
        return;
    XISetDevicePropertyDeletable(device, prop_press_to_select, FALSE);

    prop_press_to_select_threshold = MakeAtom(POINTINGSTICK_PROP_PRESS_TO_SELECT_THRESHOLD,
                                              strlen(POINTINGSTICK_PROP_PRESS_TO_SELECT_THRESHOLD),
                                              TRUE);
    rc = XIChangeDeviceProperty(device, prop_press_to_select_threshold,
                                XA_INTEGER, 8,
                                PropModeReplace, 1,
                                &priv->press_to_select_threshold,
                                FALSE);
    if (rc != Success)
        return;
    XISetDevicePropertyDeletable(device, prop_press_to_select_threshold, FALSE);

    XIRegisterPropertyHandler(device, set_property, NULL, NULL);
}

static int
device_init (DeviceIntPtr device)
{
#define LOGICAL_MAX_BUTTONS 7
#define MAX_AXES 2
    unsigned char map[LOGICAL_MAX_BUTTONS + 1];
    int i;
#if GET_ABI_MAJOR(ABI_XINPUT_VERSION) >= 7
    Atom button_labels[LOGICAL_MAX_BUTTONS] = {0};
    Atom axes_labels[MAX_AXES] = {0};

    button_labels[0] = XIGetKnownProperty(BTN_LABEL_PROP_BTN_LEFT);
    button_labels[1] = XIGetKnownProperty(BTN_LABEL_PROP_BTN_MIDDLE);
    button_labels[2] = XIGetKnownProperty(BTN_LABEL_PROP_BTN_RIGHT);
    button_labels[3] = XIGetKnownProperty(BTN_LABEL_PROP_BTN_WHEEL_UP);
    button_labels[4] = XIGetKnownProperty(BTN_LABEL_PROP_BTN_WHEEL_DOWN);
    button_labels[5] = XIGetKnownProperty(BTN_LABEL_PROP_BTN_HWHEEL_LEFT);
    button_labels[6] = XIGetKnownProperty(BTN_LABEL_PROP_BTN_HWHEEL_RIGHT);

    axes_labels[0] = XIGetKnownProperty(AXIS_LABEL_PROP_REL_X);
    axes_labels[1] = XIGetKnownProperty(AXIS_LABEL_PROP_REL_Y);
#endif

    device->public.on = FALSE;
    for (i = 0; i <= LOGICAL_MAX_BUTTONS; i++)
        map[i] = i;

    InitPointerDeviceStruct((DevicePtr)device,
                            map, LOGICAL_MAX_BUTTONS,
#if GET_ABI_MAJOR(ABI_XINPUT_VERSION) >= 7
                            button_labels,
#endif
                            pointingstick_control,
                            GetMotionHistorySize(),
                            MAX_AXES
#if GET_ABI_MAJOR(ABI_XINPUT_VERSION) >= 7
                            , axes_labels
#endif
                            );

    for (i = 0; i < 2; i++) {
        xf86InitValuatorAxisStruct(device, i,
#if GET_ABI_MAJOR(ABI_XINPUT_VERSION) >= 7
                                   axes_labels[i],
#endif
                                   -1, -1, 1, 1, 1,
                                   Relative);
        xf86InitValuatorDefaults(device, i);
    }

    init_properties(device);

    return Success;
}

static int
device_on (DeviceIntPtr device)
{
    InputInfoPtr info = device->public.devicePrivate;

    xf86Msg(X_INFO, "%s: On.\n", info->name);
    if (device->public.on)
        return Success;

    info->fd = xf86OpenSerial(info->options);
    if (info->fd == -1) {
        xf86Msg(X_WARNING, "%s: cannot open input device\n", info->name);
        return BadAccess;
    }

    xf86AddEnabledDevice(info);
    device->public.on = TRUE;

    return Success;
}

static int
device_off (DeviceIntPtr device)
{
    InputInfoPtr info = device->public.devicePrivate;
    PointingStickPrivate *priv = info->private;

    xf86Msg(X_INFO, "%s: Off.\n", info->name);
    if (!device->public.on)
        return Success;

    if (info->fd != -1) {
        xf86RemoveEnabledDevice(info);
        xf86CloseSerial(info->fd);
        info->fd = -1;
    }

    free(priv->trackpoint_sysfs_path);
    priv->trackpoint_sysfs_path = NULL;

    device->public.on = FALSE;

    return Success;
}

static int
device_close (DeviceIntPtr device)
{
    InputInfoPtr info = device->public.devicePrivate;
    PointingStickPrivate *priv = info->private;
    xf86Msg(X_INFO, "%s: Close.\n", info->name);

    if (info->fd != -1) {
        xf86CloseSerial(info->fd);
        info->fd = -1;
    }
    free(priv->trackpoint_sysfs_path);
    priv->trackpoint_sysfs_path = NULL;

    return Success;
}

static int
device_control (DeviceIntPtr    device,
                int             what)
{
    int ret;

    switch (what) {
    case DEVICE_INIT:
        ret = device_init(device);
        break;
    case DEVICE_ON:
        ret = device_on(device);
        break;
    case DEVICE_OFF:
        ret = device_off(device);
        break;
    case DEVICE_CLOSE:
        ret = device_close(device);
        break;
    default:
        ret = BadValue;
        break;
    }
    return ret;
}

/* this function is based on SynapticsReadEvent() in xf86-input-synaptics/src/evcomm.c. */
static Bool
read_event (InputInfoPtr info, struct input_event *ev)
{
    ssize_t len;

    len = read(info->fd, ev, sizeof(*ev));
    if (len <= 0) {
        if (errno != EAGAIN)
            xf86MsgVerb(X_NONE, 0, "%s: Read error %s\n", info->name, strerror(errno));
        return FALSE;
    } else if (len % sizeof(*ev)) {
        xf86MsgVerb(X_NONE, 0, "%s: Read error, invalid number of bytes.", info->name);
        return FALSE;
    }

    return TRUE;
}

static Bool
read_event_until_sync (InputInfoPtr info)
{
    struct input_event ev;
    PointingStickPrivate *priv = info->private;
    int v;

    if (priv->is_trackpoint) {
        priv->x = 0;
        priv->y = 0;
    }

    while (read_event(info, &ev)) {
        switch (ev.type) {
        case EV_SYN:
            switch (ev.code) {
            case SYN_REPORT:
                return TRUE;
                break;
            }
            break;
        case EV_KEY:
            v = (ev.value ? 1 : 0);
            switch (ev.code) {
            case BTN_LEFT:
                priv->left_button = v;
                break;
            case BTN_RIGHT:
                priv->right_button = v;
                break;
            case BTN_MIDDLE:
                priv->middle_button = v;
                break;
            case BTN_TOOL_FINGER:
                break;
            case BTN_TOUCH:
                priv->button_touched = v;
                break;
            }
            break;
        case EV_REL:
            switch (ev.code) {
            case REL_X:
                priv->x += ev.value;
                break;
            case REL_Y:
                priv->y += ev.value;
                break;
            }
            break;
        case EV_ABS:
            switch (ev.code) {
            case ABS_X:
                priv->x = ev.value;
                break;
            case ABS_Y:
                priv->y = ev.value;
                break;
            case ABS_PRESSURE:
                priv->pressure = ev.value;
                break;
            }
            break;
        }
    }
    return FALSE;
}

static Bool
handle_middle_button (InputInfoPtr local)
{
    PointingStickPrivate *priv = local->private;

    if (priv->middle_button) {
        if (!priv->middle_button_is_pressed) {
            priv->middle_button_is_pressed = TRUE;
            priv->middle_button_click_expires = priv->middle_button_timeout +
                GetTimeInMillis();
            return TRUE;
        }
    } else {
        int ms;

        if (!priv->middle_button_is_pressed)
            return FALSE;

        priv->middle_button_is_pressed = FALSE;
        ms = priv->middle_button_click_expires - GetTimeInMillis();
        if (ms > 0) {
            xf86PostButtonEvent(local->dev, 0, 2, 1, 0, 0);
            xf86PostButtonEvent(local->dev, 0, 2, 0, 0, 0);
            return TRUE;
        }
    }
    return FALSE;
}

static void
post_event (InputInfoPtr local)
{
    PointingStickPrivate *priv = local->private;
    int x, y;

    xf86PostButtonEvent(local->dev, 0, 1, priv->left_button, 0, 0);
    xf86PostButtonEvent(local->dev, 0, 3, priv->right_button, 0, 0);

    if (!priv->is_trackpoint && priv->press_to_select) {
        if (priv->pressure > priv->press_to_select_threshold) {
            priv->press_to_selecting = TRUE;
            xf86PostButtonEvent(local->dev, 0, 1, 1, 0, 0);
        } else if (priv->press_to_selecting) {
            priv->press_to_selecting = FALSE;
            xf86PostButtonEvent(local->dev, 0, 1, 0, 0, 0);
        }
    }

    if (priv->scrolling) {
        if (handle_middle_button(local))
            return;
    } else {
        xf86PostButtonEvent(local->dev, 0, 2, priv->middle_button, 0, 0);
    }

    if (priv->pressure <= 0 || priv->pressure > 250)
        return;

    if (priv->is_trackpoint) {
        x = priv->x * priv->pressure;
        y = priv->y * priv->pressure;
    } else {
        priv->x = (abs(priv->x) <= 2) ? 0 : priv->x;
        priv->y = (abs(priv->y) <= 2) ? 0 : priv->y;
        x = priv->x * priv->pressure / (256 - priv->sensitivity);
        y = priv->y * priv->pressure / (256 - priv->sensitivity);
    }

    if (!priv->scrolling || !priv->middle_button_is_pressed) {
        xf86PostMotionEvent(local->dev,
                            0, /* is_absolute */
                            0, /* first_valuator */
                            2,
                            x,
                            y);
        return;
    }

    if (priv->middle_button_is_pressed) {
        if (y != 0) {
            int button = (y < 0) ? 4 : 5;
            xf86PostButtonEvent(local->dev, 0, button, 1, 0, 0);
            xf86PostButtonEvent(local->dev, 0, button, 0, 0, 0);
        }
        if (x != 0) {
            int button = (x < 0) ? 6 : 7;
            xf86PostButtonEvent(local->dev, 0, button, 1, 0, 0);
            xf86PostButtonEvent(local->dev, 0, button, 0, 0, 0);
        }
    }
}

static void
read_input (InputInfoPtr local)
{
    while (read_event_until_sync(local))
        post_event(local);
}

/*
vi:ts=4:nowrap:ai:expandtab:sw=4
*/
