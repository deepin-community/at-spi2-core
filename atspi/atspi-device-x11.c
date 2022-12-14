/*
 * AT-SPI - Assistive Technology Service Provider Interface
 * (Gnome Accessibility Project; http://developer.gnome.org/projects/gap)
 *
 * Copyright 2020 SUSE LLC.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#include "atspi-private.h"
#include "atspi-device-x11.h"

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/XInput2.h>
#include <X11/XKBlib.h>


#define ATSPI_VIRTUAL_MODIFIER_MASK 0x0000f000

typedef struct _AtspiDeviceX11Private AtspiDeviceX11Private;
struct _AtspiDeviceX11Private
{
  Display *display;
  Window window;
  GSource *source;
  int xi_opcode;
  int device_id;
  int device_id_alt;
  GSList *modifiers;
  GSList *key_grabs;
  guint virtual_mods_enabled;
  gboolean keyboard_grabbed;
  unsigned int numlock_physical_mask;
};

GObjectClass *device_x11_parent_class;

typedef struct _DisplaySource
{
  GSource source;
  
  Display *display;
  GPollFD  event_poll_fd;
} DisplaySource;

typedef struct
{
  guint keycode;
  guint modifier;
} AtspiX11KeyModifier;

typedef struct
{
  AtspiKeyDefinition *kd;
  gboolean enabled;
} AtspiX11KeyGrab;

static gboolean  
event_prepare (GSource *source, gint *timeout)
{
  Display *display = ((DisplaySource *)source)->display;
  gboolean retval;
  
  *timeout = -1;
  retval = XPending (display);
  
  return retval;
}

static gboolean  
event_check (GSource *source) 
{
  DisplaySource *display_source = (DisplaySource*)source;
  gboolean retval;

  if (display_source->event_poll_fd.revents & G_IO_IN)
    retval = XPending (display_source->display);
  else
    retval = FALSE;

  return retval;
}

static void
xi2keyevent (XIDeviceEvent *xievent, XEvent *xkeyevent)
{
  memset (xkeyevent, 0, sizeof (*xkeyevent));

  switch (xievent->evtype)
  {
  case XI_KeyPress:
    xkeyevent->type = KeyPress;
    break;
  case XI_KeyRelease:
    xkeyevent->type = KeyRelease;
    break;
  default:
    break;
  }
  xkeyevent->xkey.serial = xievent->serial;
  xkeyevent->xkey.send_event = xievent->send_event;
  xkeyevent->xkey.display = xievent->display;
  xkeyevent->xkey.window = xievent->event;
  xkeyevent->xkey.root = xievent->root;
  xkeyevent->xkey.subwindow = xievent->child;
  xkeyevent->xkey.time = xievent->time;
  xkeyevent->xkey.x = xievent->event_x;
  xkeyevent->xkey.y = xievent->event_y;
  xkeyevent->xkey.x_root = xievent->root_x;
  xkeyevent->xkey.y_root = xievent->root_y;
  xkeyevent->xkey.state = xievent->mods.effective;
  xkeyevent->xkey.keycode = xievent->detail;
  xkeyevent->xkey.same_screen = 1;
}

G_DEFINE_TYPE_WITH_CODE (AtspiDeviceX11, atspi_device_x11,
			  ATSPI_TYPE_DEVICE,
			  G_ADD_PRIVATE (AtspiDeviceX11))


static guint
find_virtual_mapping (AtspiDeviceX11 *x11_device, gint keycode)
{
  AtspiDeviceX11Private *priv = atspi_device_x11_get_instance_private (x11_device);
  GSList *l;

  for (l = priv->modifiers; l; l = l->next)
  {
    AtspiX11KeyModifier *entry = l->data;
    if (entry->keycode == keycode)
      return entry->modifier;
  }

  return 0;
}

static gboolean
grab_should_be_enabled (AtspiDeviceX11 *x11_device, AtspiX11KeyGrab *grab)
{
  AtspiDeviceX11Private *priv = atspi_device_x11_get_instance_private (x11_device);

  /* If the whole keyboard is grabbed, then all keys are grabbed elsewhere */
  if (priv->keyboard_grabbed)
    return FALSE;

  guint virtual_mods_used = grab->kd->modifiers & ATSPI_VIRTUAL_MODIFIER_MASK;
  return ((priv->virtual_mods_enabled & virtual_mods_used) == virtual_mods_used);
}

static gboolean
grab_has_active_duplicate (AtspiDeviceX11 *x11_device, AtspiX11KeyGrab *grab)
{
  AtspiDeviceX11Private *priv = atspi_device_x11_get_instance_private (x11_device);
  GSList *l;

  for (l = priv->key_grabs; l; l = l->next)
  {
    AtspiX11KeyGrab *other = l->data;
    if (other != grab && other->enabled && other->kd->keycode == grab->kd->keycode && (other->kd->modifiers & ~ATSPI_VIRTUAL_MODIFIER_MASK) == (grab->kd->modifiers & ~ATSPI_VIRTUAL_MODIFIER_MASK))
      return TRUE;
  }
  return FALSE;
}

static void
grab_key_aux (AtspiDeviceX11 *x11_device, int keycode, int modmask)
{
  AtspiDeviceX11Private *priv = atspi_device_x11_get_instance_private (x11_device);
  XIGrabModifiers xi_modifiers;
  XIEventMask eventmask;
			unsigned char mask[XIMaskLen (XI_LASTEVENT)] = { 0 };

  xi_modifiers.modifiers = modmask;
  xi_modifiers.status = 0;

			eventmask.deviceid = XIAllDevices;
			eventmask.mask_len = sizeof(mask);
			eventmask.mask = mask;

			XISetMask (mask, XI_KeyPress);
			XISetMask (mask, XI_KeyRelease);

  XIGrabKeycode (priv->display, XIAllMasterDevices, keycode, priv->window, XIGrabModeSync, XIGrabModeAsync, False, &eventmask, 1, &xi_modifiers);
}

static void
grab_key (AtspiDeviceX11 *x11_device, int keycode, int modmask)
{
  AtspiDeviceX11Private *priv = atspi_device_x11_get_instance_private (x11_device);

  grab_key_aux (x11_device, keycode, modmask);
  if (!(modmask & LockMask))
    grab_key_aux (x11_device, keycode, modmask | LockMask);
  if (!(modmask & priv->numlock_physical_mask))
  {
    grab_key_aux (x11_device, keycode, modmask | priv->numlock_physical_mask);
    if (!(modmask & LockMask))
      grab_key_aux (x11_device, keycode, modmask | LockMask | priv->numlock_physical_mask);
  }
}

static void
enable_key_grab (AtspiDeviceX11 *x11_device, AtspiX11KeyGrab *grab)
{
  AtspiDeviceX11Private *priv = atspi_device_x11_get_instance_private (x11_device);

  g_return_if_fail (priv->display != NULL);

  if (!grab_has_active_duplicate (x11_device, grab))
    grab_key (x11_device, grab->kd->keycode, grab->kd->modifiers & ~ATSPI_VIRTUAL_MODIFIER_MASK);
  grab->enabled = TRUE;
}

static void
ungrab_key_aux (AtspiDeviceX11 *x11_device, int keycode, int modmask)
{
  AtspiDeviceX11Private *priv = atspi_device_x11_get_instance_private (x11_device);
  XIGrabModifiers xi_modifiers;

  xi_modifiers.modifiers = modmask;
  xi_modifiers.status = 0;

  XIUngrabKeycode (priv->display, XIAllMasterDevices, keycode, priv->window, sizeof(xi_modifiers), &xi_modifiers);
}

static void
ungrab_key (AtspiDeviceX11 *x11_device, int keycode, int modmask)
{
  AtspiDeviceX11Private *priv = atspi_device_x11_get_instance_private (x11_device);

  ungrab_key_aux (x11_device, keycode, modmask);
  if (!(modmask & LockMask))
    ungrab_key_aux (x11_device, keycode, modmask | LockMask);
  if (!(modmask & priv->numlock_physical_mask))
  {
    ungrab_key_aux (x11_device, keycode, modmask | priv->numlock_physical_mask);
    if (!(modmask & LockMask))
      ungrab_key_aux (x11_device, keycode, modmask | LockMask | priv->numlock_physical_mask);
  }
}

static void
disable_key_grab (AtspiDeviceX11 *x11_device, AtspiX11KeyGrab *grab)
{
  AtspiDeviceX11Private *priv = atspi_device_x11_get_instance_private (x11_device);

  g_return_if_fail (priv->display != NULL);

  if (!grab->enabled)
    return;

  grab->enabled = FALSE;

  if (grab_has_active_duplicate (x11_device, grab))
    return;

  ungrab_key (x11_device, grab->kd->keycode, grab->kd->modifiers & ~ATSPI_VIRTUAL_MODIFIER_MASK);
}

static void
refresh_key_grabs (AtspiDeviceX11 *x11_device)
{
  AtspiDeviceX11Private *priv = atspi_device_x11_get_instance_private (x11_device);
  GSList *l;

  for (l = priv->key_grabs; l; l = l->next)
  {
    AtspiX11KeyGrab *grab = l->data;
    gboolean new_enabled = grab_should_be_enabled (x11_device, grab);
    if (new_enabled && !grab->enabled)
      enable_key_grab (x11_device, grab);
    else if (grab->enabled && !new_enabled)
      disable_key_grab (x11_device, grab);
  }
}

static void
set_virtual_modifier (AtspiDeviceX11 *x11_device, gint keycode, gboolean enabled)
{
  AtspiDeviceX11Private *priv = atspi_device_x11_get_instance_private (x11_device);
  guint modifier = find_virtual_mapping (x11_device, keycode);

  if (!modifier)
    return;

  if (enabled)
  {
    if (priv->virtual_mods_enabled & modifier)
      return;
    priv->virtual_mods_enabled |= modifier;
  }
  else
  {
    if (!(priv->virtual_mods_enabled & modifier))
      return;
    priv->virtual_mods_enabled &= ~modifier;
  }

  refresh_key_grabs (x11_device);
}

static gboolean
do_event_dispatch (gpointer user_data)
{
  AtspiDeviceX11 *device = user_data;
  AtspiDeviceX11Private *priv = atspi_device_x11_get_instance_private (device);
  Display *display = priv->display;
  XEvent xevent;
  char text[10];
  KeySym keysym;
  XComposeStatus status;
  guint modifiers;
 
  while (XPending (display))
  {
    XNextEvent (display, &xevent);
  XEvent keyevent;

    switch (xevent.type)
    {
    case KeyPress:
    case KeyRelease:
      XLookupString(&xevent.xkey, text, sizeof (text), &keysym, &status);
      modifiers = xevent.xkey.state | priv->virtual_mods_enabled;
      if (modifiers & priv->numlock_physical_mask)
      {
        modifiers |= (1 << ATSPI_MODIFIER_NUMLOCK);
        modifiers &= ~priv->numlock_physical_mask;
      }
      atspi_device_notify_key (ATSPI_DEVICE (device), (xevent.type == KeyPress), xevent.xkey.keycode, keysym, modifiers, text);
      break;
    case GenericEvent:
      if (xevent.xcookie.extension == priv->xi_opcode)
      {
        XGetEventData(priv->display, &xevent.xcookie);
        XIRawEvent *xiRawEv = (XIRawEvent *) xevent.xcookie.data;
        XIDeviceEvent *xiDevEv = (XIDeviceEvent *) xevent.xcookie.data;
        switch (xevent.xcookie.evtype)
        {
        case XI_KeyPress:
        case XI_KeyRelease:
          xi2keyevent (xiDevEv, &keyevent);
          XLookupString((XKeyEvent *)&keyevent, text, sizeof (text), &keysym, &status);
          if (text[0] < ' ')
            text[0] = '\0';
          /* The deviceid can change. Would be nice to find a better way of
             handling this */
          if (priv->device_id && priv->device_id_alt && xiDevEv->deviceid != priv->device_id && xiDevEv->deviceid != priv->device_id_alt)
            priv->device_id = priv->device_id_alt = 0;
          else if (priv->device_id && !priv->device_id_alt && xiDevEv->deviceid != priv->device_id)
            priv->device_id_alt = xiDevEv->deviceid;
          if (!priv->device_id)
            priv->device_id = xiDevEv->deviceid;
          set_virtual_modifier (device, xiRawEv->detail, xevent.xcookie.evtype == XI_KeyPress);
          modifiers = keyevent.xkey.state | priv->virtual_mods_enabled;
          if (modifiers & priv->numlock_physical_mask)
            modifiers |= (1 << ATSPI_MODIFIER_NUMLOCK);
          if (xiDevEv->deviceid == priv->device_id)
            atspi_device_notify_key (ATSPI_DEVICE (device), (xevent.xcookie.evtype == XI_KeyPress), xiRawEv->detail, keysym, modifiers, text);
          /* otherwise it's probably a duplicate event from a key grab */
          XFreeEventData (priv->display, &xevent.xcookie);
          break;
        }
      }
    default:
      if (XFilterEvent (&xevent, None))
        continue;
    }
  }
  return TRUE;
}

static gboolean  
event_dispatch (GSource *source, GSourceFunc callback, gpointer  user_data)
{
  if (callback)
    callback (user_data);
  return G_SOURCE_CONTINUE;
}

static GSourceFuncs event_funcs = {
  event_prepare,
  event_check,
  event_dispatch,
  NULL
};

static GSource *
display_source_new (Display *display)
{
  GSource *source = g_source_new (&event_funcs, sizeof (DisplaySource));
  DisplaySource *display_source = (DisplaySource *) source;
  g_source_set_name (source, "[at-spi2-core] display_source_funcs");
  
  display_source->display = display;
  
  return source;
}

static void 
create_event_source (AtspiDeviceX11 *device)
{
  AtspiDeviceX11Private *priv = atspi_device_x11_get_instance_private (device);
  DisplaySource *display_source;

  int connection_number = ConnectionNumber (priv->display);

  priv->source = display_source_new (priv->display);
  display_source = (DisplaySource *)priv->source;

  g_source_set_priority (priv->source, G_PRIORITY_DEFAULT);
  
  display_source->event_poll_fd.fd = connection_number;
  display_source->event_poll_fd.events = G_IO_IN;
  
  g_source_add_poll (priv->source, &display_source->event_poll_fd);
  g_source_set_can_recurse (priv->source, TRUE);
  g_source_set_callback (priv->source, do_event_dispatch, device, NULL);
  g_source_attach (priv->source, NULL);
}

static gboolean
check_virtual_modifier (AtspiDeviceX11 *x11_device, guint modifier)
{
  AtspiDeviceX11Private *priv = atspi_device_x11_get_instance_private (x11_device);
  GSList *l;

  if (modifier == (1 << ATSPI_MODIFIER_NUMLOCK))
    return TRUE;

  for (l = priv->modifiers; l; l = l->next)
  {
    AtspiX11KeyModifier *entry = l->data;
    if (entry->modifier == modifier)
      return TRUE;
  }

  return FALSE;
}

static guint
get_unused_virtual_modifier (AtspiDeviceX11 *x11_device)
{
  guint ret = 0x1000;

  while (ret < 0x10000)
  {
    if (!check_virtual_modifier (x11_device, ret))
      return ret;
    ret <<= 1;
  }

  return 0;
}

static guint
atspi_device_x11_map_modifier (AtspiDevice *device, gint keycode)
{
  AtspiDeviceX11 *x11_device = ATSPI_DEVICE_X11 (device);
  AtspiDeviceX11Private *priv = atspi_device_x11_get_instance_private (x11_device);
  XkbDescPtr desc;
  guint ret;
  AtspiX11KeyModifier *entry;

  desc = XkbGetMap (priv->display, XkbModifierMapMask, XkbUseCoreKbd);

  if (keycode < desc->min_key_code || keycode >= desc->max_key_code)
  {
    XkbFreeKeyboard (desc, XkbModifierMapMask, TRUE);
    g_warning ("Passed invalid keycode %d", keycode);
    return 0;
  }

  ret = desc->map->modmap[keycode];
  XkbFreeKeyboard (desc, XkbModifierMapMask, TRUE);
  if (ret & (ShiftMask | ControlMask))
    return ret;

  ret = find_virtual_mapping (x11_device, keycode);
  if (ret)
    return ret;

  ret = get_unused_virtual_modifier (x11_device);

  entry = g_new (AtspiX11KeyModifier, 1);
  entry->keycode = keycode;
  entry->modifier = ret;
  priv->modifiers = g_slist_append (priv->modifiers, entry);

  return ret;
}

static void
atspi_device_x11_unmap_modifier (AtspiDevice *device, gint keycode)
{
  AtspiDeviceX11 *x11_device = ATSPI_DEVICE_X11 (device);
  AtspiDeviceX11Private *priv = atspi_device_x11_get_instance_private (x11_device);
  GSList *l;

  for (l = priv->modifiers; l; l = l->next)
  {
    AtspiX11KeyModifier *entry = l->data;
    if (entry->keycode == keycode)
    {
      g_free (entry);
      priv->modifiers = g_slist_remove (priv->modifiers, entry);
      return;
    }
  }
}

static guint
atspi_device_x11_get_modifier (AtspiDevice *device, gint keycode)
{
  AtspiDeviceX11 *x11_device = ATSPI_DEVICE_X11 (device);
  AtspiDeviceX11Private *priv = atspi_device_x11_get_instance_private (x11_device);
  XkbDescPtr desc;
  guint ret;

  desc = XkbGetMap (priv->display, XkbModifierMapMask, XkbUseCoreKbd);

  if (keycode < desc->min_key_code || keycode >= desc->max_key_code)
  {
    XkbFreeKeyboard (desc, XkbModifierMapMask, TRUE);
    g_warning ("Passed invalid keycode %d", keycode);
    return 0;
  }

  ret = desc->map->modmap[keycode];
  XkbFreeKeyboard (desc, XkbModifierMapMask, TRUE);
  if (ret)
    return ret;

  return find_virtual_mapping (x11_device, keycode);
}

static void
atspi_device_x11_init (AtspiDeviceX11 *device)
{
  AtspiDeviceX11Private *priv = atspi_device_x11_get_instance_private (device);
  int first_event, first_error;

  priv->display=XOpenDisplay("");
  g_return_if_fail (priv->display != NULL);
  priv->window = DefaultRootWindow(priv->display);

  if (XQueryExtension(priv->display, "XInputExtension", &priv->xi_opcode, &first_event, &first_error)) 
  {
    int major = 2;
    int minor = 1;
    if (XIQueryVersion(priv->display, &major, &minor) != BadRequest)
    {
      XIEventMask eventmask;
      unsigned char mask[XIMaskLen (XI_LASTEVENT)] = { 0 };

      eventmask.deviceid = XIAllDevices;
      eventmask.mask_len = sizeof(mask);
      eventmask.mask = mask;

      XISetMask (mask, XI_KeyPress);
      XISetMask (mask, XI_KeyRelease);
      XISetMask (mask, XI_ButtonPress);
      XISetMask (mask, XI_ButtonRelease);
      XISetMask (mask, XI_Motion);
      XISelectEvents (priv->display, priv->window, &eventmask, 1);
      create_event_source (device);
    }
  }

  priv->numlock_physical_mask = XkbKeysymToModifiers (priv->display,
						      XK_Num_Lock);
}

static void
atspi_device_x11_finalize (GObject *object)
{
  AtspiDeviceX11 *device = ATSPI_DEVICE_X11 (object);
  AtspiDeviceX11Private *priv = atspi_device_x11_get_instance_private (device);
  GSList *l;

  for (l = priv->key_grabs; l; l = l->next)
  {
    AtspiX11KeyGrab *grab = l->data;
    disable_key_grab (device, grab);
    g_boxed_free (ATSPI_TYPE_KEY_DEFINITION, grab->kd);
    g_free (grab);
  }
  g_slist_free (priv->key_grabs);
  priv->key_grabs = NULL;

  g_slist_free_full (priv->modifiers, g_free);
  priv->modifiers = NULL;

  if (priv->source)
  {
    g_source_destroy ((GSource *) priv->source);
    g_source_unref ((GSource *) priv->source);
    priv->source = NULL;
    }

  device_x11_parent_class->finalize (object);
}


static void
atspi_device_x11_add_key_grab (AtspiDevice *device, AtspiKeyDefinition *kd)
{
  AtspiDeviceX11 *x11_device = ATSPI_DEVICE_X11 (device);
  AtspiDeviceX11Private *priv = atspi_device_x11_get_instance_private (x11_device);
  AtspiX11KeyGrab *grab;

  grab = g_new (AtspiX11KeyGrab, 1);
  grab->kd = g_boxed_copy (ATSPI_TYPE_KEY_DEFINITION, kd);
  grab->enabled = FALSE;
  priv->key_grabs = g_slist_append (priv->key_grabs, grab);
  if (grab_should_be_enabled (x11_device, grab))
    enable_key_grab (x11_device, grab);
}

static void
atspi_device_x11_remove_key_grab (AtspiDevice *device, guint id)
{
  AtspiDeviceX11 *x11_device = ATSPI_DEVICE_X11 (device);
  AtspiDeviceX11Private *priv = atspi_device_x11_get_instance_private (x11_device);
  AtspiKeyDefinition *kd;
  GSList *l;

  kd = atspi_device_get_grab_by_id (device, id);

  for (l = priv->key_grabs; l; l = g_slist_next (l))
  {
    AtspiX11KeyGrab *other = l->data;
    if (other->kd->keycode == kd->keycode && other->kd->modifiers == kd->modifiers)
    {
      disable_key_grab (x11_device, other);
      priv->key_grabs = g_slist_remove (priv->key_grabs, other);
      return;
    }
  }
}

static guint
atspi_device_x11_get_locked_modifiers (AtspiDevice *device)
{
  AtspiDeviceX11 *x11_device = ATSPI_DEVICE_X11 (device);
  AtspiDeviceX11Private *priv = atspi_device_x11_get_instance_private (x11_device);
  XkbStateRec state_rec;

  memset (&state_rec, 0, sizeof (state_rec));
  XkbGetState (priv->display, XkbUseCoreKbd, &state_rec);
  return state_rec.locked_mods;
}

static void
get_keycode_range (AtspiDeviceX11 *x11_device, int *min, int *max)
{
  AtspiDeviceX11Private *priv = atspi_device_x11_get_instance_private (x11_device);
  XkbDescPtr desc;

  desc = XkbGetMap (priv->display, XkbModifierMapMask, XkbUseCoreKbd);
  *min = desc->min_key_code;
  *max = desc->max_key_code;
  XkbFreeKeyboard (desc, XkbModifierMapMask, TRUE);
}

static gboolean
atspi_device_x11_grab_keyboard (AtspiDevice *device)
{
  AtspiDeviceX11 *x11_device = ATSPI_DEVICE_X11 (device);
  AtspiDeviceX11Private *priv = atspi_device_x11_get_instance_private (x11_device);
  int min, max;
  gint i;

  g_return_val_if_fail (priv->display != NULL, FALSE);
#if 0
  /* THis seems like the right thing to do, but it fails for me */
  return (XGrabKeyboard (priv->display, priv->window, TRUE, GrabModeAsync, GrabModeSync, CurrentTime)) == 0;
#else
  if (priv->keyboard_grabbed)
    return TRUE;
  priv->keyboard_grabbed = TRUE;
  refresh_key_grabs (x11_device);

  get_keycode_range (x11_device, &min, &max);
  for (i = min; i < max; i++)
    grab_key (x11_device, i, 0);
  return TRUE;
#endif
}

static void
atspi_device_x11_ungrab_keyboard (AtspiDevice *device)
{
  AtspiDeviceX11 *x11_device = ATSPI_DEVICE_X11 (device);
  AtspiDeviceX11Private *priv = atspi_device_x11_get_instance_private (x11_device);
  int min, max;
  gint i;

  g_return_if_fail (priv->display != NULL);
#if 0
  XUngrabKeyboard (priv->display, CurrentTime);
#else
  if (!priv->keyboard_grabbed)
    return;
  priv->keyboard_grabbed = FALSE;

  get_keycode_range (x11_device, &min, &max);
  for (i = min; i < max; i++)
    ungrab_key (x11_device, i, 0);

  refresh_key_grabs (x11_device);
#endif
}

static void
atspi_device_x11_class_init (AtspiDeviceX11Class *klass)
{
  AtspiDeviceClass *device_class = ATSPI_DEVICE_CLASS (klass);
  GObjectClass *object_class = (GObjectClass *) klass;

  device_x11_parent_class = g_type_class_peek_parent (klass);
  device_class->add_key_grab = atspi_device_x11_add_key_grab;
  device_class->map_modifier = atspi_device_x11_map_modifier;
  device_class->unmap_modifier = atspi_device_x11_unmap_modifier;
  device_class->get_modifier = atspi_device_x11_get_modifier;
  device_class->remove_key_grab = atspi_device_x11_remove_key_grab;
  device_class->get_locked_modifiers = atspi_device_x11_get_locked_modifiers;
  device_class->grab_keyboard = atspi_device_x11_grab_keyboard;
  device_class->ungrab_keyboard = atspi_device_x11_ungrab_keyboard;
  object_class->finalize = atspi_device_x11_finalize;
}

/**
 * atspi_device_x11_new:
 *
 * Creates a new #AtspiDeviceX11.
 *
 * Returns: (transfer full): a pointer to a newly-created #AtspiDeviceX11.
 *
 **/
AtspiDeviceX11 *
atspi_device_x11_new ()
{
  AtspiDeviceX11 *device = g_object_new (atspi_device_x11_get_type (), NULL);

  return device;
}
