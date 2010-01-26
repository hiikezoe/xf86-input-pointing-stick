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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <dirent.h>
#include <string.h>

#include <xf86.h>
#include <xf86Xinput.h>

#include "trackpoint.h"
#include "pointingstick.h"

#define SERIO_SYSFS_PATH "/sys/devices/platform/i8042"

static char *trackpoint_sysfs_path = NULL;

static int
serio_filter (const struct dirent *name)
{
    if (name->d_type != DT_DIR)
        return 0;

    return !strncmp(name->d_name, "serio", 5);
}

static int
sensitivity_filter (const struct dirent *name)
{
    if (name->d_type != DT_REG)
        return 0;

    return !strcmp(name->d_name, "sensitivity");
}

static Bool
find_sensitivity (const char *base_path)
{
    struct dirent **filelist;
    int n;

    n = scandir(base_path, &filelist, sensitivity_filter, alphasort);
    if (n <= 0)
        return FALSE;

    while (n--)
        free(filelist[n]);
    free(filelist);

    return TRUE;
}

static char *
find_trackpoint_sysfs_path (const char *base_path)
{
    struct dirent **seriolist;
    int n;
    char *serio_path = NULL;

    n = scandir(base_path, &seriolist, serio_filter, alphasort);
    if (n < 0)
        return NULL;

    while (n--) {
        char path[4096];
        snprintf(path, sizeof(path),
                 "%s/%s", base_path, seriolist[n]->d_name);
        if (find_sensitivity(path)) {
            serio_path = strdup(path);
            break;
        } else {
            serio_path = find_trackpoint_sysfs_path(path);
            if (serio_path)
                break;
        }
        free(seriolist[n]);
    }

    while (n--)
        free(seriolist[n]);
    free(seriolist);

    return serio_path;
}

static char *
get_trackpoint_sysfs_path (void)
{
    if (!trackpoint_sysfs_path)
        trackpoint_sysfs_path = find_trackpoint_sysfs_path(SERIO_SYSFS_PATH);

    return trackpoint_sysfs_path;
}

Bool
pointingstick_is_trackpoint (InputInfoPtr local)
{
    return get_trackpoint_sysfs_path() ? TRUE : FALSE;
}

static int
trackpoint_get_property (InputInfoPtr local,
                         const char *property_name)
{
    FILE *file;
    char property_path[4096];
    char property_string[4];
    size_t read_size;
    int property = -1;

    PointingStickPrivate *priv = local->private;
    if (!priv->is_trackpoint)
        return -1;

    snprintf(property_path, sizeof(property_path),
             "%s/%s",
             get_trackpoint_sysfs_path(),
             property_name);

    file = fopen(property_path, "r");
    if (!file)
        return -1;

    read_size = fread(property_string, sizeof(property_string), 1, file);
    if (read_size > 0)
        property = atoi(property_string);
    fclose(file);

    return property;
}

static int
trackpoint_set_property (InputInfoPtr local,
                         const char *property_name,
                         int property_value)
{
    FILE *file;
    char property_path[4096];
    char property_string[4];
    size_t written_size;
    int ret = Success;

    PointingStickPrivate *priv = local->private;
    if (!priv->is_trackpoint)
        return BadRequest;

    snprintf(property_path, sizeof(property_path),
             "%s/%s",
             get_trackpoint_sysfs_path(),
             property_name);

    file = fopen(property_path, "r+");
    if (!file)
        return BadAccess;

    snprintf(property_string, sizeof(property_string),
             "%d", property_value);
    written_size = fwrite(property_string, strlen(property_string), 1, file);
    if (written_size != strlen(property_string))
        ret = BadAccess;
    fclose(file);

    return ret;
}

int
trackpoint_get_sensitivity (InputInfoPtr local)
{
    int sensitivity;

    sensitivity = trackpoint_get_property(local, "sensitivity");

    if (sensitivity < 1)
        sensitivity = 1;
    else if (sensitivity > 255)
        sensitivity = 255;

    return sensitivity;
}

int
trackpoint_set_sensitivity (InputInfoPtr local, int sensitivity)
{
    return trackpoint_set_property(local, "sensitivity", sensitivity);
}

/*
vi:ts=4:nowrap:ai:expandtab:sw=4
*/
