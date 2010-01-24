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

#include <xf86_OSproc.h>
#include <xf86.h>
#include <xf86Xinput.h>
#include <exevents.h>
#include <xf86Module.h>
#include <X11/Xatom.h>

static Atom prop_sensitivity = 0;
static Atom prop_scrolling = 0;
static Atom prop_middle_button_timeout = 0;
static Atom prop_press_to_select = 0;
static Atom prop_press_to_select_threshold = 0;

#if GET_ABI_MAJOR(ABI_XINPUT_VERSION) >= 7
#include <xserver-properties.h>
#endif

#include "pointingstick.h"
#include "pointingstick-properties.h"

#define LONG_BITS (sizeof(long) * 8)
#define NLONGS(x) (((x) + LONG_BITS - 1) / LONG_BITS)
#define TestBit(bit, array) ((array[(bit) / LONG_BITS]) & (1L << ((bit) % LONG_BITS)))
#define SYSCALL(call) while (((call) == -1) && (errno == EINTR))

static InputInfoPtr pre_init       (InputDriverPtr drv,
                                    IDevPtr dev,
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
    unsigned long absbits[NLONGS(ABS_MAX)] = {0};

    if (!strstr(local->name, "Styk"))
        return FALSE;

    SYSCALL(rc = ioctl(local->fd, EVIOCGBIT(0, sizeof(evbits)), evbits));
    if (rc < 0)
        return FALSE;
    if (!TestBit(EV_SYN, evbits) ||
        !TestBit(EV_ABS, evbits) ||
        !TestBit(EV_KEY, evbits)) {
        return FALSE;
    }

    SYSCALL(rc = ioctl(local->fd, EVIOCGBIT(EV_ABS, sizeof(absbits)), absbits));
    if (rc < 0)
        return FALSE;
    if (!TestBit(ABS_X, absbits) ||
        !TestBit(ABS_Y, absbits) ||
        !TestBit(ABS_PRESSURE, absbits)) {
        return FALSE;
    }

    return TRUE;
}

static InputInfoPtr
pre_init(InputDriverPtr  drv,
         IDevPtr         dev,
         int             flags)
{
    InputInfoPtr local;
    PointingStickPrivate *priv;
    Bool success = FALSE;

    if (!(local = xf86AllocateInput(drv, 0)))
        return NULL;

    priv = xcalloc(1, sizeof(PointingStickPrivate));
    if (!priv)
        goto end;

    local->private = priv;
    local->name = dev->identifier;
    local->flags = XI86_POINTER_CAPABLE | XI86_SEND_DRAG_EVENTS;
    local->type_name = "POINTINGSTICK";
    local->conf_idev = dev;
    local->read_input = read_input; /* new data avl */
    local->switch_mode = NULL; /* toggle absolute/relative mode */
    local->device_control = device_control; /* enable/disable dev */

    xf86CollectInputOptions(local, NULL, NULL);
    xf86ProcessCommonOptions(local, local->options);

    local->fd = xf86OpenSerial(local->options);
    if (local->fd == -1)
        goto end;

    if (!is_pointingstick(local))
        goto end;

    xf86Msg(X_PROBED, "%s found\n", local->name);
    local->flags |= XI86_OPEN_ON_INIT;
    local->flags |= XI86_CONFIGURED;

    priv->sensitivity = 100;
    priv->scrolling = TRUE;
    priv->middle_button_timeout = 100;
    priv->middle_button_is_pressed = FALSE;
    priv->press_to_select = FALSE;
    priv->press_to_select_threshold = 8;
    priv->press_to_selecting = FALSE;

    success = TRUE;

 end:
    if (local->fd != -1) {
        xf86CloseSerial(local->fd);
        local->fd = -1;
    }

    if (!success) {
        local->private = NULL;
        xfree(priv);
        xf86DeleteInput(local, 0);
        local = NULL;
    }

    return local;
}

static void
uninit (InputDriverPtr drv,
        InputInfoPtr   local,
        int            flags)
{
    xfree(local->private);
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
    LocalDevicePtr local = (LocalDevicePtr) device->public.devicePrivate;
    PointingStickPrivate *priv = local->private;

    if (atom == prop_sensitivity) {
        int sensitivity;

        if (val->format != 8 || val->size != 1 || val->type != XA_INTEGER)
            return BadMatch;

        sensitivity = *((CARD8*)val->data);
        if (sensitivity < 1 || sensitivity > 255)
            return BadValue;

        if (!checkonly)
            priv->sensitivity = sensitivity;
    }

    if (atom == prop_scrolling) {
        if (val->format != 8 || val->size != 1 || val->type != XA_INTEGER)
            return BadMatch;

        if (!checkonly)
            priv->scrolling = *((BOOL*)val->data);
    }

    if (atom == prop_middle_button_timeout) {
        int timeout;

        if (val->format != 8 || val->size != 1 || val->type != XA_INTEGER)
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

        if (!checkonly)
            priv->press_to_select = *((BOOL*)val->data);
    }

    if (atom == prop_press_to_select_threshold) {
        int threshold;

        if (val->format != 8 || val->size != 1 || val->type != XA_INTEGER)
            return BadMatch;

        threshold = *((CARD8*)val->data);
        if (threshold < 1 || threshold > 127)
            return BadValue;

        if (!checkonly)
            priv->press_to_select_threshold = threshold;
    }

    return Success;
}

static void
init_properties (DeviceIntPtr device)
{
    LocalDevicePtr local = (LocalDevicePtr) device->public.devicePrivate;
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
                                   -1, -1, 1, 1, 1);
        xf86InitValuatorDefaults(device, i);
    }

    init_properties(device);

    return Success;
}

static int
device_on (DeviceIntPtr device)
{
    LocalDevicePtr local = (LocalDevicePtr) device->public.devicePrivate;

    xf86Msg(X_INFO, "%s: On.\n", local->name);
    if (device->public.on)
        return Success;

    local->fd = xf86OpenSerial(local->options);
    if (local->fd == -1) {
        xf86Msg(X_WARNING, "%s: cannot open input device\n", local->name);
        return BadAccess;
    }

    xf86AddEnabledDevice(local);
    device->public.on = TRUE;

    return Success;
}

static int
device_off (DeviceIntPtr device)
{
    LocalDevicePtr local = (LocalDevicePtr) device->public.devicePrivate;

    xf86Msg(X_INFO, "%s: Off.\n", local->name);
    if (!device->public.on)
        return Success;

    if (local->fd != -1) {
        xf86RemoveEnabledDevice(local);
        xf86CloseSerial(local->fd);
        local->fd = -1;
    }

    device->public.on = FALSE;

    return Success;
}

static int
device_close (DeviceIntPtr device)
{
    LocalDevicePtr local = (LocalDevicePtr) device->public.devicePrivate;
    xf86Msg(X_INFO, "%s: Close.\n", local->name);

    if (local->fd != -1) {
        xf86CloseSerial(local->fd);
        local->fd = -1;
    }

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
read_event (LocalDevicePtr local, struct input_event *ev)
{
    ssize_t len;

    len = read(local->fd, ev, sizeof(*ev));
    if (len <= 0) {
        if (errno != EAGAIN)
            xf86MsgVerb(X_NONE, 0, "%s: Read error %s\n", local->name, strerror(errno));
        return FALSE;
    } else if (len % sizeof(*ev)) {
        xf86MsgVerb(X_NONE, 0, "%s: Read error, invalid number of bytes.", local->name);
        return FALSE;
    }

    return TRUE;
}

static Bool
read_event_until_sync (LocalDevicePtr local)
{
    struct input_event ev;
    PointingStickPrivate *priv = local->private;
    int v;

    while (read_event(local, &ev)) {
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

static void
handle_scrolling (InputInfoPtr local)
{
    PointingStickPrivate *priv = local->private;

    if (priv->middle_button) {
        if (!priv->middle_button_is_pressed) {
            priv->middle_button_is_pressed = TRUE;
            priv->middle_button_click_expires = priv->middle_button_timeout +
                GetTimeInMillis();
        }
    } else {
        int ms;

        if (!priv->middle_button_is_pressed)
            return;

        priv->middle_button_is_pressed = FALSE;
        ms = priv->middle_button_click_expires - GetTimeInMillis();
        if (ms > 0) {
            xf86PostButtonEvent(local->dev, 0, 2, 1, 0, 0);
            xf86PostButtonEvent(local->dev, 0, 2, 0, 0, 0);
        }
    }
}

static void
post_event (InputInfoPtr local)
{
    PointingStickPrivate *priv = local->private;
    int x, y;

    xf86PostButtonEvent(local->dev, 0, 1, priv->left_button, 0, 0);
    xf86PostButtonEvent(local->dev, 0, 3, priv->right_button, 0, 0);

    if (priv->press_to_select) {
        if (priv->pressure > priv->press_to_select_threshold) {
            priv->press_to_selecting = TRUE;
            xf86PostButtonEvent(local->dev, 0, 1, 1, 0, 0);
        } else if (priv->press_to_selecting) {
            priv->press_to_selecting = FALSE;
            xf86PostButtonEvent(local->dev, 0, 1, 0, 0, 0);
        }
    }

    if (priv->scrolling) {
        handle_scrolling(local);
    } else {
        xf86PostButtonEvent(local->dev, 0, 2, priv->middle_button, 0, 0);
    }

    if (priv->pressure <= 0 || priv->pressure > 255)
        return;

    x = priv->x * priv->pressure / priv->sensitivity;
    y = priv->y * priv->pressure / priv->sensitivity;

    if (!priv->scrolling || !priv->middle_button) {
        xf86PostMotionEvent(local->dev,
                            0, /* is_absolute */
                            0, /* first_valuator */
                            2,
                            x,
                            y);
        return;
    }

    if (priv->middle_button) {
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
