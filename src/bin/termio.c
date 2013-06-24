#include "private.h"
#include <Ecore_IMF.h>
#include <Ecore_IMF_Evas.h>
#include <Elementary.h>
#include <Ecore_Input.h>
#include "scrolio.h"
#include "termio.h"
#include "termiolink.h"
#include "termpty.h"
#include "termcmd.h"
#include "utf8.h"
#include "col.h"
#include "keyin.h"
#include "config.h"
#include "utils.h"
#include "media.h"
#include "dbus.h"

typedef struct _Termio Termio;

struct _Termio
{
   Evas_Object_Smart_Clipped_Data __clipped_data;
   struct {
      int size;
      const char *name;
      int chw, chh;
   } font;
   struct {
      int w, h;
      Evas_Object *obj;
   } grid;
   struct {
      Evas_Object *obj, *selo_top, *selo_bottom, *selo_theme;
      int x, y;
      struct {
         int x, y;
      } sel1, sel2;
      Eina_Bool sel : 1;
      Eina_Bool makesel : 1;
   } cur;
   struct {
      int cx, cy;
      int button;
   } mouse;
   struct {
      struct {
         int x, y;
      } sel1, sel2;
      Eina_Bool sel : 1;
   } backup;
   struct {
      char *string;
      int x1, y1, x2, y2;
      int suspend;
      Eina_List *objs;
      struct {
         Evas_Object *dndobj;
         Evas_Coord x, y;
         Eina_Bool down : 1;
         Eina_Bool dnd : 1;
         Eina_Bool dndobjdel : 1;
      } down;
   } link;
   Evas_Object *scrolio;
   int zoom_fontsize_start;
   int scroll;
   unsigned int last_keyup;
   Eina_List *mirrors;
   Eina_List *seq;
   Evas_Object *event;
   Termpty *pty;
   Ecore_Animator *anim;
   Ecore_Timer *delayed_size_timer;
   Ecore_Timer *link_do_timer;
   Ecore_Job *mouse_move_job;
   Ecore_Timer *mouseover_delay;
   Evas_Object *win, *theme, *glayer;
   Config *config;
   Ecore_IMF_Context *imf;
   const char *sel_str;
   Eina_List *cur_chids;
   Ecore_Job *sel_reset_job;
   double set_sel_at;
   Elm_Sel_Type sel_type;
   Eina_Bool jump_on_change : 1;
   Eina_Bool jump_on_keypress : 1;
   Eina_Bool have_sel : 1;
   Eina_Bool noreqsize : 1;
   Eina_Bool composing : 1;
   Eina_Bool didclick : 1;
   Eina_Bool bottom_right : 1;
   Eina_Bool top_left : 1;
   Eina_Bool boxsel : 1;
   Eina_Bool reset_sel : 1;
   Eina_Bool debugwhite : 1;
};

static Evas_Smart *_smart = NULL;
static Evas_Smart_Class _parent_sc = EVAS_SMART_CLASS_INIT_NULL;

static Eina_List *terms = NULL;

static void _smart_calculate(Evas_Object *obj);
static void _smart_mirror_del(void *data, Evas *evas __UNUSED__, Evas_Object *obj, void *info __UNUSED__);
static void _lost_selection(void *data, Elm_Sel_Type selection);

static inline Eina_Bool
_should_inline(const Evas_Object *obj)
{
   const Config *config = termio_config_get(obj);
   const Evas *e;
   const Evas_Modifier *mods;

   if (!config->helper.inline_please) return EINA_FALSE;

   e = evas_object_evas_get(obj);
   mods = evas_key_modifier_get(e);

   if (evas_key_modifier_is_set(mods, "Control"))  return EINA_FALSE;

   return EINA_TRUE;
}

static void
_activate_link(Evas_Object *obj)
{
   Termio *sd = evas_object_smart_data_get(obj);
   Config *config = termio_config_get(obj);
   char buf[PATH_MAX], *s, *escaped;
   const char *path = NULL, *cmd = NULL;
   Eina_Bool url = EINA_FALSE, email = EINA_FALSE, handled = EINA_FALSE;
   int type;
   
   if (!sd) return;
   if (!config) return;
   if (!sd->link.string) return;
   if (link_is_url(sd->link.string))
     {
        if (casestartswith(sd->link.string, "file://"))
          // TODO: decode string: %XX -> char
          path = sd->link.string + sizeof("file://") - 1;
        else
          url = EINA_TRUE;
     }
   else if (sd->link.string[0] == '/')
     path = sd->link.string;
   else if (link_is_email(sd->link.string))
     email = EINA_TRUE;

   if (url && casestartswith(sd->link.string, "mailto:"))
     {
        email = EINA_TRUE;
        url = EINA_FALSE;
     }

   s = eina_str_escape(sd->link.string);
   if (!s) return;
   if (email)
     {
        const char *p = s;

        // run mail client
        cmd = "xdg-email";
        
        if ((config->helper.email) &&
            (config->helper.email[0]))
          cmd = config->helper.email;

        if (casestartswith(s, "mailto:"))
          p += sizeof("mailto:") - 1;

        escaped = ecore_file_escape_name(p);
        if (escaped)
          {
             snprintf(buf, sizeof(buf), "%s %s", cmd, escaped);
             free(escaped);
          }
     }
   else if (path)
     {
        // locally accessible file
        cmd = "xdg-open";
        
        escaped = ecore_file_escape_name(s);
        if (escaped)
          {
             type = media_src_type_get(sd->link.string);
             if (_should_inline(obj))
               {
                  if ((type == TYPE_IMG) ||
                      (type == TYPE_SCALE) ||
                      (type == TYPE_EDJE))
                    {
                       evas_object_smart_callback_call(obj, "popup", NULL);
                       handled = EINA_TRUE;
                    }
                  else if (type == TYPE_MOV)
                    {
                       evas_object_smart_callback_call(obj, "popup", NULL);
                       handled = EINA_TRUE;
                    }
               }
             if (!handled)
               {
                  if ((type == TYPE_IMG) ||
                      (type == TYPE_SCALE) ||
                      (type == TYPE_EDJE))
                    {
                       if ((config->helper.local.image) &&
                           (config->helper.local.image[0]))
                         cmd = config->helper.local.image;
                    }
                  else if (type == TYPE_MOV)
                    {
                       if ((config->helper.local.video) &&
                           (config->helper.local.video[0]))
                         cmd = config->helper.local.video;
                    }
                  else
                    {
                       if ((config->helper.local.general) &&
                           (config->helper.local.general[0]))
                         cmd = config->helper.local.general;
                    }
                  snprintf(buf, sizeof(buf), "%s %s", cmd, escaped);
                  free(escaped);
               }
          }
     }
   else if (url)
     {
        // remote file needs ecore-con-url
        cmd = "xdg-open";
        
        escaped = ecore_file_escape_name(s);
        if (escaped)
          {
             type = media_src_type_get(sd->link.string);
             if (_should_inline(obj))
               {
                  if ((type == TYPE_IMG) ||
                      (type == TYPE_SCALE) ||
                      (type == TYPE_EDJE))
                    {
                       // XXX: begin fetch of url, once done, show
                       evas_object_smart_callback_call(obj, "popup", NULL);
                       handled = EINA_TRUE;
                    }
                  else if (type == TYPE_MOV)
                    {
                       // XXX: if no http:// add
                       evas_object_smart_callback_call(obj, "popup", NULL);
                       handled = EINA_TRUE;
                    }
               }
             if (!handled)
               {
                  if ((type == TYPE_IMG) ||
                      (type == TYPE_SCALE) ||
                      (type == TYPE_EDJE))
                    {
                       if ((config->helper.url.image) &&
                           (config->helper.url.image[0]))
                         cmd = config->helper.url.image;
                    }
                  else if (type == TYPE_MOV)
                    {
                       if ((config->helper.url.video) &&
                           (config->helper.url.video[0]))
                         cmd = config->helper.url.video;
                    }
                  else
                    {
                       if ((config->helper.url.general) &&
                           (config->helper.url.general[0]))
                         cmd = config->helper.url.general;
                    }
                  snprintf(buf, sizeof(buf), "%s %s", cmd, escaped);
                  free(escaped);
               }
          }
     }
   else
     {
        free(s);
        return;
     }
   free(s);
   if (!handled) ecore_exe_run(buf, NULL);
}

static void
_cb_link_down(void *data, Evas *e __UNUSED__, Evas_Object *obj __UNUSED__, void *event)
{
   Evas_Event_Mouse_Down *ev = event;
   Termio *sd = evas_object_smart_data_get(data);
   if (!sd) return;
   
   if (ev->button != 1) return;
   sd->link.down.down = EINA_TRUE;
   sd->link.down.x = ev->canvas.x;
   sd->link.down.y = ev->canvas.y;
}

static Eina_Bool
_cb_link_up_delay(void *data)
{
   Termio *sd = evas_object_smart_data_get(data);
   if (!sd) return EINA_FALSE;
   
   sd->link_do_timer = NULL;
   if (!sd->didclick) _activate_link(data);
   sd->didclick = EINA_FALSE;
   return EINA_FALSE;
}

static void
_cb_link_up(void *data, Evas *e __UNUSED__, Evas_Object *obj __UNUSED__, void *event)
{
   Evas_Event_Mouse_Up *ev = event;
   Termio *sd = evas_object_smart_data_get(data);
   Evas_Coord dx, dy;
   
   if (!sd) return;
   dx = abs(ev->canvas.x - sd->link.down.x);
   dy = abs(ev->canvas.y - sd->link.down.y);
   if ((ev->button == 1) && (sd->link.down.down) &&
       ((dx <= elm_config_finger_size_get()) &&
           (dy <= elm_config_finger_size_get())))
     {
        if (sd->link_do_timer) ecore_timer_del(sd->link_do_timer);
        sd->link_do_timer = ecore_timer_add(0.2, _cb_link_up_delay, data);
     }
   if ((ev->button == 1) && (sd->link.down.down))
     {
        sd->link.down.down = EINA_FALSE;
     }
}

#if !((ELM_VERSION_MAJOR == 1) && (ELM_VERSION_MINOR < 8))
static void
_cb_link_drag_move(void *data, Evas_Object *obj, Evas_Coord x, Evas_Coord y, Elm_Xdnd_Action action)
{
   const Evas_Modifier *em = NULL;
   Termio *sd = evas_object_smart_data_get(data);
   if (!sd) return;
   
   printf("dnd %i %i act %i\n", x, y, action);
   em = evas_key_modifier_get(evas_object_evas_get(sd->event));
   if (em)
     {
        if (evas_key_modifier_is_set(em, "Control"))
          elm_drag_action_set(obj, ELM_XDND_ACTION_COPY);
        else
          elm_drag_action_set(obj, ELM_XDND_ACTION_MOVE);
     }
}

static void
_cb_link_drag_accept(void *data, Evas_Object *obj __UNUSED__, Eina_Bool doaccept)
{
   Termio *sd = evas_object_smart_data_get(data);
   if (!sd) return;
   
   printf("dnd accept: %i\n", doaccept);
}

static void
_cb_link_drag_done(void *data, Evas_Object *obj __UNUSED__)
{
   Termio *sd = evas_object_smart_data_get(data);
   if (!sd) return;
   
   printf("dnd done\n");
   sd->link.down.dnd = EINA_FALSE;
   if ((sd->link.down.dndobjdel) && (sd->link.down.dndobj))
     evas_object_del(sd->link.down.dndobj);
   sd->link.down.dndobj = NULL;
}

static Evas_Object *
_cb_link_icon_new(void *data, Evas_Object *par, Evas_Coord *xoff, Evas_Coord *yoff)
{
   Evas_Object *icon;
   Termio *sd = evas_object_smart_data_get(data);
   if (!sd) return NULL;
   
   icon = elm_button_add(par);
   elm_object_text_set(icon, sd->link.string);
   *xoff = 0;
   *yoff = 0;
   return icon;
}
#endif

static void
_cb_link_move(void *data, Evas *e __UNUSED__, Evas_Object *obj, void *event)
{
   Evas_Event_Mouse_Move *ev = event;
   Termio *sd = evas_object_smart_data_get(data);
   Evas_Coord dx, dy;
   if (!sd) return;
   
   if (!sd->link.down.down) return;
   dx = abs(ev->cur.canvas.x - sd->link.down.x);
   dy = abs(ev->cur.canvas.y - sd->link.down.y);
   if ((sd->link.string) &&
       ((dx > elm_config_finger_size_get()) ||
           (dy > elm_config_finger_size_get())))
     {
        sd->link.down.down = EINA_FALSE;
        sd->link.down.dnd = EINA_TRUE;
#if !((ELM_VERSION_MAJOR == 1) && (ELM_VERSION_MINOR < 8))
        printf("dnd start %s %i %i\n", sd->link.string,
               evas_key_modifier_is_set(ev->modifiers, "Control"),
               evas_key_modifier_is_set(ev->modifiers, "Shift"));
        if (evas_key_modifier_is_set(ev->modifiers, "Control"))
          elm_drag_start(obj, ELM_SEL_FORMAT_IMAGE, sd->link.string,
                         ELM_XDND_ACTION_COPY,
                         _cb_link_icon_new, data,
                         _cb_link_drag_move, data,
                         _cb_link_drag_accept, data,
                         _cb_link_drag_done, data);
        else
          elm_drag_start(obj, ELM_SEL_FORMAT_IMAGE, sd->link.string,
                         ELM_XDND_ACTION_MOVE,
                         _cb_link_icon_new, data,
                         _cb_link_drag_move, data,
                         _cb_link_drag_accept, data,
                         _cb_link_drag_done, data);
        sd->link.down.dndobj = obj;
        sd->link.down.dndobjdel = EINA_FALSE;
#endif
     }
}

static void
_update_link(Evas_Object *obj, Eina_Bool same_link, Eina_Bool same_geom)
{
   Termio *sd = evas_object_smart_data_get(obj);
   
   if (!sd) return;
   
   if (!same_link)
     {
        // check link and re-probe/fetch create popup preview
     }
   
   if (!same_geom)
     {
        Evas_Coord ox, oy, ow, oh;
        Evas_Object *o;
        // fix up edje objects "underlining" the link
        int y;

        evas_object_geometry_get(obj, &ox, &oy, &ow, &oh);
        if (!sd->link.suspend)
          {
             EINA_LIST_FREE(sd->link.objs, o)
               {
                  if (sd->link.down.dndobj == o)
                    {
                       sd->link.down.dndobjdel = EINA_TRUE;
                       evas_object_hide(o);
                    }
                  else
                    evas_object_del(o);
               }
             if (sd->link.string)
               {
                  if ((sd->link.string[0] == '/') || (link_is_url(sd->link.string)))
                    {
                       Evas_Coord ox, oy;
                       Ecore_X_Window xwin;

                       evas_object_geometry_get(obj, &ox, &oy, NULL, NULL);

                       ox += sd->mouse.cx * sd->font.chw;
                       oy += sd->mouse.cy * sd->font.chh;
                       xwin = elm_win_xwindow_get(sd->win);
                       ty_dbus_link_mousein(xwin, sd->link.string, ox, oy);
                    }
                  for (y = sd->link.y1; y <= sd->link.y2; y++)
                    {
                       o = edje_object_add(evas_object_evas_get(obj));
                       evas_object_smart_member_add(o, obj);
                       theme_apply(o, sd->config, "terminology/link");

                       if (y == sd->link.y1)
                         {
                            evas_object_move(o, ox + (sd->link.x1 * sd->font.chw),
                                             oy + (y * sd->font.chh));
                            if (sd->link.y1 == sd->link.y2)
                              evas_object_resize(o,
                                                 ((sd->link.x2 - sd->link.x1 + 1) * sd->font.chw),
                                                 sd->font.chh);
                            else
                              evas_object_resize(o,
                                                 ((sd->grid.w - sd->link.x1) * sd->font.chw),
                                                 sd->font.chh);
                         }
                       else if (y == sd->link.y2)
                         {
                            evas_object_move(o, ox, oy + (y * sd->font.chh));
                            evas_object_resize(o,
                                               ((sd->link.x2 + 1) * sd->font.chw),
                                               sd->font.chh);
                         }
                       else
                         {
                            evas_object_move(o, ox, oy + (y * sd->font.chh));
                            evas_object_resize(o, (sd->grid.w * sd->font.chw),
                                               sd->font.chh);
                         }

                       sd->link.objs = eina_list_append(sd->link.objs, o);
                       evas_object_show(o);
                       evas_object_event_callback_add(o, EVAS_CALLBACK_MOUSE_DOWN,
                                                      _cb_link_down, obj);
                       evas_object_event_callback_add(o, EVAS_CALLBACK_MOUSE_UP,
                                                      _cb_link_up, obj);
                       evas_object_event_callback_add(o, EVAS_CALLBACK_MOUSE_MOVE,
                                                      _cb_link_move, obj);
                    }
               }
          }
     }
}

static void
_smart_mouseover_apply(Evas_Object *obj)
{
   char *s;
   int x1 = 0, y1 = 0, x2 = 0, y2 = 0;
   Eina_Bool same_link = EINA_FALSE, same_geom = EINA_FALSE;
   Termio *sd = evas_object_smart_data_get(obj);

   if (!sd) return;

   s = _termio_link_find(obj, sd->mouse.cx, sd->mouse.cy,
                         &x1, &y1, &x2, &y2);
   if (!s)
     {
        if (sd->link.string)
          {
             if ((sd->link.string[0] == '/') || (link_is_url(sd->link.string)))
               {
                  Evas_Coord ox, oy;
                  Ecore_Window xwin;

                  evas_object_geometry_get(obj, &ox, &oy, NULL, NULL);

                  ox += sd->mouse.cx * sd->font.chw;
                  oy += sd->mouse.cy * sd->font.chh;
                  xwin = elm_win_xwindow_get(sd->win);
                  ty_dbus_link_mouseout(xwin, sd->link.string, ox, oy);
               }
             free(sd->link.string);
             sd->link.string = NULL;
           }
        sd->link.x1 = -1;
        sd->link.y1 = -1;
        sd->link.x2 = -1;
        sd->link.y2 = -1;
        _update_link(obj, same_link, same_geom);
        return;
     }

   if ((sd->link.string) && (!strcmp(sd->link.string, s)))
     same_link = EINA_TRUE;
   if (sd->link.string) free(sd->link.string);
   sd->link.string = s;

   if ((x1 == sd->link.x1) && (y1 == sd->link.y1) &&
       (x2 == sd->link.x2) && (y2 == sd->link.y2))
     same_geom = EINA_TRUE;
   if (((sd->link.suspend != 0) && (sd->link.objs)) ||
       ((sd->link.suspend == 0) && (!sd->link.objs)))
     same_geom = EINA_FALSE;
   sd->link.x1 = x1;
   sd->link.y1 = y1;
   sd->link.x2 = x2;
   sd->link.y2 = y2;
   _update_link(obj, same_link, same_geom);
}

static Eina_Bool
_smart_mouseover_delay(void *data)
{
   Termio *sd = evas_object_smart_data_get(data);
   
   if (!sd) return EINA_FALSE;
   sd->mouseover_delay = NULL;
   _smart_mouseover_apply(data);
   return EINA_FALSE;
}

#define INT_SWAP(_a, _b) do {    \
    int _swap = _a; _a = _b; _b = _swap; \
} while (0)

static void
_smart_media_clicked(void *data, Evas_Object *obj, void *info __UNUSED__)
{
//   Termio *sd = evas_object_smart_data_get(data);
   Termblock *blk;
   const char *file = media_get(obj);
   if (!file) return;
   blk = evas_object_data_get(obj, "blk");
   if (blk)
     {
        if (blk->link)
          {
             int type = media_src_type_get(blk->link);
             Config *config = termio_config_get(data);
             
             if (config)
               {
                  if ((!config->helper.inline_please) ||
                      (!((type == TYPE_IMG) || (type == TYPE_SCALE) ||
                         (type == TYPE_EDJE) || (type == TYPE_MOV))))
                    {
                       const char *cmd = NULL;
                       
                       file = blk->link;
                       if ((config->helper.local.general) &&
                           (config->helper.local.general[0]))
                         cmd = config->helper.local.general;
                       if (cmd)
                         {
                            char buf[PATH_MAX];
                            
                            snprintf(buf, sizeof(buf), "%s %s", cmd, file);
                            ecore_exe_run(buf, NULL);
                            return;
                         }
                    }
                  file = blk->link;
               }
          }
     }
   evas_object_smart_callback_call(data, "popup", (void *)file);
}

static void
_smart_media_del(void *data, Evas *e __UNUSED__, Evas_Object *obj, void *info __UNUSED__)
{
   Termblock *blk = data;
   
   if (blk->obj == obj)
     {
        evas_object_event_callback_del_full
          (blk->obj, EVAS_CALLBACK_DEL,
              _smart_media_del, blk);
        blk->obj = NULL;
     }
}

static void
_block_edje_signal_cb(void *data, Evas_Object *obj __UNUSED__, const char *sig, const char *src)
{
   Termblock *blk = data;
   Termio *sd = evas_object_smart_data_get(blk->pty->obj);
   char *buf = NULL, *chid = NULL;
   int buflen = 0;
   Eina_List *l;
   
   if (!sd) return;
   if ((!blk->chid) || (!sd->cur_chids)) return;
   EINA_LIST_FOREACH(sd->cur_chids, l, chid)
     {
        if (!(!strcmp(blk->chid, chid))) break;
        chid = NULL;
     }
   if (!chid) return;
   if ((!strcmp(sig, "drag")) ||
       (!strcmp(sig, "drag,start")) ||
       (!strcmp(sig, "drag,stop")) ||
       (!strcmp(sig, "drag,step")) ||
       (!strcmp(sig, "drag,set")))
     {
        int v1, v2;
        double f1 = 0.0, f2 = 0.0;
        
        edje_object_part_drag_value_get(blk->obj, src, &f1, &f2);
        v1 = (int)(f1 * 1000.0);
        v2 = (int)(f2 * 1000.0);
        buf = alloca(strlen(src) + strlen(blk->chid) + 256);
        buflen = sprintf(buf, "%c};%s\n%s\n%s\n%i\n%i", 0x1b,
                         blk->chid, sig, src, v1, v2);
        termpty_write(sd->pty, buf, buflen + 1);
     }
   else
     {
        buf = alloca(strlen(sig) + strlen(src) + strlen(blk->chid) + 128);
        buflen = sprintf(buf, "%c}signal;%s\n%s\n%s", 0x1b,
                         blk->chid, sig, src);
        termpty_write(sd->pty, buf, buflen + 1);
     }
}

static void
_block_edje_message_cb(void *data, Evas_Object *obj __UNUSED__, Edje_Message_Type type, int id, void *msg)
{
   Termblock *blk = data;
   Termio *sd = evas_object_smart_data_get(blk->pty->obj);
   char *chid = NULL, buf[4096];
   Eina_List *l;
   int buflen;
   
   if (!sd) return;
   if ((!blk->chid) || (!sd->cur_chids)) return;
   EINA_LIST_FOREACH(sd->cur_chids, l, chid)
     {
        if (!(!strcmp(blk->chid, chid))) break;
        chid = NULL;
     }
   if (!chid) return;
   switch (type)
     {
      case EDJE_MESSAGE_STRING:
          {
             Edje_Message_String *m = msg;
             
             buflen = sprintf(buf, "%c}message;%s\n%i\nstring\n%s", 0x1b,
                              blk->chid, id, m->str);
             termpty_write(sd->pty, buf, buflen + 1);
          }
        break;
      case EDJE_MESSAGE_INT:
          {
             Edje_Message_Int *m = msg;
             
             buflen = snprintf(buf, sizeof(buf),
                               "%c}message;%s\n%i\nint\n%i", 0x1b,
                               blk->chid, id, m->val);
             termpty_write(sd->pty, buf, buflen + 1);
          }
        break;
      case EDJE_MESSAGE_FLOAT:
          {
             Edje_Message_Float *m = msg;
             
             buflen = snprintf(buf, sizeof(buf),
                               "%c}message;%s\n%i\nfloat\n%i", 0x1b,
                               blk->chid, id, (int)(m->val * 1000.0));
             termpty_write(sd->pty, buf, buflen + 1);
          }
        break;
      case EDJE_MESSAGE_STRING_SET:
          {
             Edje_Message_String_Set *m = msg;
             int i;
             char zero[1] = { 0 };
             
             buflen = snprintf(buf, sizeof(buf),
                               "%c}message;%s\n%i\nstring_set\n%i", 0x1b,
                               blk->chid, id, m->count);
             termpty_write(sd->pty, buf, buflen);
             for (i = 0; i < m->count; i++)
               {
                  termpty_write(sd->pty, "\n", 1);
                  termpty_write(sd->pty, m->str[i], strlen(m->str[i]));
               }
             termpty_write(sd->pty, zero, 1);
         }
        break;
      case EDJE_MESSAGE_INT_SET:
          {
             Edje_Message_Int_Set *m = msg;
             int i;
             char zero[1] = { 0 };
             
             buflen = snprintf(buf, sizeof(buf),
                               "%c}message;%s\n%i\nint_set\n%i", 0x1b,
                               blk->chid, id, m->count);
             termpty_write(sd->pty, buf, buflen);
             for (i = 0; i < m->count; i++)
               {
                  termpty_write(sd->pty, "\n", 1);
                  buflen = snprintf(buf, sizeof(buf), "%i", m->val[i]);
                  termpty_write(sd->pty, buf, buflen);
               }
             termpty_write(sd->pty, zero, 1);
          }
        break;
      case EDJE_MESSAGE_FLOAT_SET:
          {
             Edje_Message_Float_Set *m = msg;
             int i;
             char zero[1] = { 0 };
             
             buflen = snprintf(buf, sizeof(buf),
                               "%c}message;%s\n%i\nfloat_set\n%i", 0x1b,
                               blk->chid, id, m->count);
             termpty_write(sd->pty, buf, buflen);
             for (i = 0; i < m->count; i++)
               {
                  termpty_write(sd->pty, "\n", 1);
                  buflen = snprintf(buf, sizeof(buf), "%i", (int)(m->val[i] * 1000.0));
                  termpty_write(sd->pty, buf, buflen);
               }
             termpty_write(sd->pty, zero, 1);
          }
        break;
      case EDJE_MESSAGE_STRING_INT:
          {
             Edje_Message_String_Int *m = msg;
             
             buflen = snprintf(buf, sizeof(buf),
                               "%c}message;%s\n%i\nstring_int\n%s\n%i", 0x1b,
                               blk->chid, id, m->str, m->val);
             termpty_write(sd->pty, buf, buflen + 1);
          }
        break;
      case EDJE_MESSAGE_STRING_FLOAT:
          {
             Edje_Message_String_Float *m = msg;
             
             buflen = snprintf(buf, sizeof(buf),
                               "%c}message;%s\n%i\nstring_float\n%s\n%i", 0x1b,
                               blk->chid, id, m->str, (int)(m->val * 1000.0));
             termpty_write(sd->pty, buf, buflen + 1);
          }
        break;
      case EDJE_MESSAGE_STRING_INT_SET:
          {
             Edje_Message_String_Int_Set *m = msg;
             int i;
             char zero[1] = { 0 };
             
             buflen = snprintf(buf, sizeof(buf),
                               "%c}message;%s\n%i\nstring_int_set\n%i", 0x1b,
                               blk->chid, id, m->count);
             termpty_write(sd->pty, buf, buflen);
             termpty_write(sd->pty, "\n", 1);
             termpty_write(sd->pty, m->str, strlen(m->str));
             for (i = 0; i < m->count; i++)
               {
                  termpty_write(sd->pty, "\n", 1);
                  buflen = snprintf(buf, sizeof(buf), "%i", m->val[i]);
                  termpty_write(sd->pty, buf, buflen);
               }
             termpty_write(sd->pty, zero, 1);
          }
        break;
      case EDJE_MESSAGE_STRING_FLOAT_SET:
          {
             Edje_Message_String_Float_Set *m = msg;
             int i;
             char zero[1] = { 0 };
             
             buflen = snprintf(buf, sizeof(buf),
                               "%c}message;%s\n%i\nstring_float_set\n%i", 0x1b,
                               blk->chid, id, m->count);
             termpty_write(sd->pty, buf, buflen);
             termpty_write(sd->pty, "\n", 1);
             termpty_write(sd->pty, m->str, strlen(m->str));
             for (i = 0; i < m->count; i++)
               {
                  termpty_write(sd->pty, "\n", 1);
                  buflen = snprintf(buf, sizeof(buf), "%i", (int)(m->val[i] * 1000.0));
                  termpty_write(sd->pty, buf, buflen);
               }
             termpty_write(sd->pty, zero, 1);
          }
        break;
      default:
        break;
     }
}

static void
_block_edje_cmds(Termpty *ty, Termblock *blk, Eina_List *cmds, Eina_Bool created)
{
   Eina_List *l;
   char *s;
        
#define ISCMD(cmd) !strcmp(s, cmd)
#define GETS(var) l = l->next; if (!l) return; var = l->data
#define GETI(var) l = l->next; if (!l) return; var = atoi(l->data)
#define GETF(var) l = l->next; if (!l) return; var = (double)atoi(l->data) / 1000.0
   l = cmds;
   while (l)
     {
        s = l->data;
        
        /////////////////////////////////////////////////////////////////////
        if (ISCMD("text")) // set text part
          {
             char *prt, *txt;
             
             GETS(prt);
             GETS(txt);
             edje_object_part_text_set(blk->obj, prt, txt);
          }
        /////////////////////////////////////////////////////////////////////
        else if (ISCMD("emit")) // emit signal
          {
             char *sig, *src;
             
             GETS(sig);
             GETS(src);
             edje_object_signal_emit(blk->obj, sig, src);
          }
        /////////////////////////////////////////////////////////////////////
        else if (ISCMD("drag")) // set dragable
          {
             char *prt, *val;
             double v1, v2;
             
             GETS(prt);
             GETS(val);
             GETF(v1);
             GETF(v2);
             if (!strcmp(val, "value"))
               edje_object_part_drag_value_set(blk->obj, prt, v1, v2);
             else if (!strcmp(val, "size"))
               edje_object_part_drag_size_set(blk->obj, prt, v1, v2);
             else if (!strcmp(val, "step"))
               edje_object_part_drag_step_set(blk->obj, prt, v1, v2);
             else if (!strcmp(val, "page"))
               edje_object_part_drag_page_set(blk->obj, prt, v1, v2);
          }
        /////////////////////////////////////////////////////////////////////
        else if (ISCMD("message")) // send message
          {
             int id;
             char *typ;
             
             GETI(id);
             GETS(typ);
             if (!strcmp(typ, "string"))
               {
                  Edje_Message_String *m;
                  
                  m = alloca(sizeof(Edje_Message_String));
                  GETS(m->str);
                  edje_object_message_send(blk->obj, EDJE_MESSAGE_STRING,
                                           id, m);
               }
             else if (!strcmp(typ, "int"))
               {
                  Edje_Message_Int *m;
                  
                  m = alloca(sizeof(Edje_Message_Int));
                  GETI(m->val);
                  edje_object_message_send(blk->obj, EDJE_MESSAGE_INT,
                                           id, m);
               }
             else if (!strcmp(typ, "float"))
               {
                  Edje_Message_Float *m;
                  
                  m = alloca(sizeof(Edje_Message_Float));
                  GETF(m->val);
                  edje_object_message_send(blk->obj, EDJE_MESSAGE_FLOAT,
                                           id, m);
               }
             else if (!strcmp(typ, "string_set"))
               {
                  Edje_Message_String_Set *m;
                  int i, count;
                  
                  GETI(count);
                  m = alloca(sizeof(Edje_Message_String_Set) + 
                             ((count - 1) * sizeof(char *)));
                  m->count = count;
                  for (i = 0; i < m->count; i++)
                    {
                       GETS(m->str[i]);
                    }
                  edje_object_message_send(blk->obj,
                                           EDJE_MESSAGE_STRING_SET,
                                           id, m);
               }
             else if (!strcmp(typ, "int_set"))
               {
                  Edje_Message_Int_Set *m;
                  int i, count;
                  
                  GETI(count);
                  m = alloca(sizeof(Edje_Message_Int_Set) + 
                             ((count - 1) * sizeof(int)));
                  m->count = count;
                  for (i = 0; i < m->count; i++)
                    {
                       GETI(m->val[i]);
                    }
                  edje_object_message_send(blk->obj,
                                           EDJE_MESSAGE_INT_SET,
                                           id, m);
               }
             else if (!strcmp(typ, "float_set"))
               {
                  Edje_Message_Float_Set *m;
                  int i, count;
                  
                  GETI(count);
                  m = alloca(sizeof(Edje_Message_Float_Set) +
                             ((count - 1) * sizeof(double)));
                  m->count = count;
                  for (i = 0; i < m->count; i++)
                    {
                       GETF(m->val[i]);
                    }
                  edje_object_message_send(blk->obj,
                                           EDJE_MESSAGE_FLOAT_SET,
                                           id, m);
               }
             else if (!strcmp(typ, "string_int"))
               {
                  Edje_Message_String_Int *m;
                  
                  m = alloca(sizeof(Edje_Message_String_Int));
                  GETS(m->str);
                  GETI(m->val);
                  edje_object_message_send(blk->obj, EDJE_MESSAGE_STRING_INT,
                                           id, m);
               }
             else if (!strcmp(typ, "string_float"))
               {
                  Edje_Message_String_Float *m;
                  
                  m = alloca(sizeof(Edje_Message_String_Float));
                  GETS(m->str);
                  GETF(m->val);
                  edje_object_message_send(blk->obj, EDJE_MESSAGE_STRING_FLOAT,
                                           id, m);
               }
             else if (!strcmp(typ, "string_int_set"))
               {
                  Edje_Message_String_Int_Set *m;
                  int i, count;
                  
                  GETI(count);
                  m = alloca(sizeof(Edje_Message_String_Int_Set) + 
                             ((count - 1) * sizeof(int)));
                  GETS(m->str);
                  m->count = count;
                  for (i = 0; i < m->count; i++)
                    {
                       GETI(m->val[i]);
                    }
                  edje_object_message_send(blk->obj,
                                           EDJE_MESSAGE_STRING_INT_SET,
                                           id, m);
               }
             else if (!strcmp(typ, "string_float_set"))
               {
                  Edje_Message_String_Float_Set *m;
                  int i, count;
                  
                  GETI(count);
                  m = alloca(sizeof(Edje_Message_String_Float_Set) + 
                             ((count - 1) * sizeof(double)));
                  GETS(m->str);
                  m->count = count;
                  for (i = 0; i < m->count; i++)
                    {
                       GETF(m->val[i]);
                    }
                  edje_object_message_send(blk->obj,
                                           EDJE_MESSAGE_STRING_FLOAT_SET,
                                           id, m);
               }
          }
        /////////////////////////////////////////////////////////////////////
        else if (ISCMD("chid")) // set callback channel id
          {
             char *chid;
             
             GETS(chid);
             if (!blk->chid)
               {
                  blk->chid = eina_stringshare_add(chid);
                  termpty_block_chid_update(ty, blk);
               }
             if (created)
               {
                  edje_object_signal_callback_add(blk->obj, "*", "*",
                                                  _block_edje_signal_cb,
                                                  blk);
                  edje_object_message_handler_set(blk->obj,
                                                  _block_edje_message_cb,
                                                  blk);
               }
          }
        if (l) l = l->next;
     }
}

static void
_block_edje_activate(Evas_Object *obj, Termblock *blk)
{
   Termio *sd = evas_object_smart_data_get(obj);
   Eina_Bool ok = EINA_FALSE;
   
   if (!sd) return;
   if ((!blk->path) || (!blk->link)) return;
   blk->obj = edje_object_add(evas_object_evas_get(obj));
   if (blk->path[0] == '/')
     ok = edje_object_file_set(blk->obj, blk->path, blk->link);
   else if (!strcmp(blk->path, "THEME"))
     ok = edje_object_file_set(blk->obj, 
                               config_theme_path_default_get
                               (sd->config),
                               blk->link);
   else
     {
        char path[PATH_MAX], home[PATH_MAX];
        
        if (homedir_get(home, sizeof(home)))
          {
             snprintf(path, sizeof(path), "%s/.terminology/objlib/%s",
                      home, blk->path);
             ok = edje_object_file_set(blk->obj, path, blk->link);
          }
        if (!ok)
          {
             snprintf(path, sizeof(path), "%s/objlib/%s",
                      elm_app_data_dir_get(), blk->path);
             ok = edje_object_file_set(blk->obj, path, blk->link);
          }
     }
   evas_object_smart_member_add(blk->obj, obj);
   evas_object_stack_above(blk->obj, sd->event);
   evas_object_show(blk->obj);
   evas_object_data_set(blk->obj, "blk", blk);

   if (ok)
     {
        _block_edje_cmds(sd->pty, blk, blk->cmds, EINA_TRUE);
        //scrolio_pty_update(sd->scrolio, sd->pty);
     }
}

static void
_block_media_activate(Evas_Object *obj, Termblock *blk)
{
   Termio *sd = evas_object_smart_data_get(obj);
   int type = 0;
   int media = MEDIA_STRETCH;
        
   if (!sd) return;
   if (blk->scale_stretch) media = MEDIA_STRETCH;
   else if (blk->scale_center) media = MEDIA_POP;
   else if (blk->scale_fill) media = MEDIA_BG;
   else if (blk->thumb) media = MEDIA_THUMB;
//   media = MEDIA_POP;
   if (!blk->was_active_before) media |= MEDIA_SAVE;
   else media |= MEDIA_RECOVER | MEDIA_SAVE;
   blk->obj = media_add(obj, blk->path, sd->config, media, &type);
   evas_object_event_callback_add
     (blk->obj, EVAS_CALLBACK_DEL, _smart_media_del, blk);
   blk->type = type;
   evas_object_smart_member_add(blk->obj, obj);
   evas_object_stack_above(blk->obj, sd->grid.obj);
   evas_object_show(blk->obj);
   evas_object_data_set(blk->obj, "blk", blk);
   if (blk->thumb)
     evas_object_smart_callback_add
     (blk->obj, "clicked", _smart_media_clicked, obj);
}

static void
_block_activate(Evas_Object *obj, Termblock *blk)
{
   Termio *sd = evas_object_smart_data_get(obj);
   if (!sd) return;
   if (blk->active) return;
   blk->active = EINA_TRUE;
   if (blk->obj) return;
   if (blk->edje) _block_edje_activate(obj, blk);
   else _block_media_activate(obj, blk);
   blk->was_active_before = EINA_TRUE;
   if (!blk->was_active)
     sd->pty->block.active = eina_list_append(sd->pty->block.active, blk);
}

static void
_smart_apply(Evas_Object *obj)
{
   Termio *sd = evas_object_smart_data_get(obj);
   Evas_Coord ox, oy, ow, oh;
   Eina_List *l, *ln;
   Termblock *blk;
   int j, x, y, w, ch1 = 0, ch2 = 0, inv = 0, jinx=0;

   if (!sd) return;
   evas_object_geometry_get(obj, &ox, &oy, &ow, &oh);
   
   EINA_LIST_FOREACH(sd->pty->block.active, l, blk)
     {
        blk->was_active = blk->active;
        blk->active = EINA_FALSE;
     }
   inv = sd->pty->state.reverse;
   termpty_cellcomp_freeze(sd->pty);
   for (y = 0; y < sd->grid.h; y++)
     {
        Termcell *cells;
        Evas_Textgrid_Cell *tc;

        w = 0; j = 0;
        cells = termpty_cellrow_get(sd->pty, y - sd->scroll, &w);
        tc = evas_object_textgrid_cellrow_get(sd->grid.obj, y);
        if (!tc) continue;
        ch1 = -1;
        for (x = 0; x < sd->grid.w; x++)
          {
             if ((!cells) || (x >= w))
               {
                  if ((tc[x].codepoint != 0) ||
                      (tc[x].bg != COL_INVIS) ||
                      (tc[x].bg_extended))
                    {
                       if (ch1 < 0) ch1 = x;
                       ch2 = x;
                    }
                  tc[x].codepoint = 0;
                  if (inv) tc[x].bg = COL_INVERSEBG;
                  else tc[x].bg = COL_INVIS;
                  tc[x].bg_extended = 0;
                  tc[x].double_width = 0;
               }
             else
               {
                  int bid, bx = 0, by = 0;
                  
                  bid = termpty_block_id_get(&(cells[j]), &bx, &by);
                  if (bid >= 0)
                    {
                       if (ch1 < 0) ch1 = x;
                       ch2 = x;
                       tc[x].codepoint = 0;
                       tc[x].fg_extended = 0;
                       tc[x].bg_extended = 0;
                       tc[x].underline = 0;
                       tc[x].strikethrough = 0;
                       tc[x].fg = COL_INVIS;
                       tc[x].bg = COL_INVIS;
#if defined(SUPPORT_DBLWIDTH)
                       tc[x].double_width = 0;
#endif
                       blk = termpty_block_get(sd->pty, bid);
                       if (blk)
                         {
                            _block_activate(obj, blk);
                            blk->x = (x - bx);
                            blk->y = (y - by);
                            evas_object_move(blk->obj,
                                             ox + (blk->x * sd->font.chw),
                                             oy + (blk->y * sd->font.chh));
                            evas_object_resize(blk->obj,
                                               blk->w * sd->font.chw,
                                               blk->h * sd->font.chh);
                         }
                    }
                  else if (cells[j].att.invisible)
                    {
                       if ((tc[x].codepoint != 0) ||
                           (tc[x].bg != COL_INVIS) ||
                           (tc[x].bg_extended))
                         {
                            if (ch1 < 0) ch1 = x;
                            ch2 = x;
                         }
                       tc[x].codepoint = 0;
                       if (inv) tc[x].bg = COL_INVERSEBG;
                       else tc[x].bg = COL_INVIS;
                       tc[x].bg_extended = 0;
#if defined(SUPPORT_DBLWIDTH)
                       tc[x].double_width = cells[j].att.dblwidth;
#endif
                       if ((tc[x].double_width) && (tc[x].codepoint == 0) &&
                           (ch2 == x - 1))
                         ch2 = x;
                    }
                  else
                    {
                       int bold, fg, bg, fgext, bgext, codepoint;
                       
                       // colors
                       bold = cells[j].att.bold;
                       fgext = cells[j].att.fg256;
                       bgext = cells[j].att.bg256;
                       codepoint = cells[j].codepoint;
                       
                       if (cells[j].att.inverse ^ inv)
                         {
                            fgext = 0;
                            bgext = 0;
                            fg = cells[j].att.fg;
                            bg = cells[j].att.bg;
                            if (fg == COL_DEF) fg = COL_INVERSEBG;
                            if (bg == COL_DEF) bg = COL_INVERSE;
                            INT_SWAP(bg, fg);
                            if (bold)
                              {
                                 fg += 12;
                                 bg += 12;
                              }
                            if (cells[j].att.faint)
                              {
                                 fg += 24;
                                 bg += 24;
                              }
                            if (cells[j].att.fgintense) fg += 48;
                            if (cells[j].att.bgintense) bg += 48;
                         }
                       else
                         {
                            fg = cells[j].att.fg;
                            bg = cells[j].att.bg;
                            
                            if (!fgext)
                              {
                                 if (bold) fg += 12;
                              }
                            if (!bgext)
                              {
                                 if (bg == COL_DEF) bg = COL_INVIS;
                              }
                            if (cells[j].att.faint)
                              {
                                 if (!fgext) fg += 24;
                                 if (!bgext) bg += 24;
                              }
                            if (cells[j].att.fgintense) fg += 48;
                            if (cells[j].att.bgintense) bg += 48;
                            if (((codepoint == ' ') || (codepoint == 0)) &&
                                (!cells[j].att.strike) &&
                                (!cells[j].att.underline))
                              fg = COL_INVIS;
                         }
                       if ((tc[x].codepoint != codepoint) ||
                           (tc[x].fg != fg) ||
                           (tc[x].bg != bg) ||
                           (tc[x].fg_extended != fgext) ||
                           (tc[x].bg_extended != bgext) ||
                           (tc[x].underline != cells[j].att.underline) ||
                           (tc[x].strikethrough != cells[j].att.strike) ||
                           (sd->debugwhite))
                         {
                            if (ch1 < 0) ch1 = x;
                            ch2 = x;
                         }
                       tc[x].fg_extended = fgext;
                       tc[x].bg_extended = bgext;
                       if (sd->debugwhite)
                         {
                            if (cells[j].att.newline)
                              tc[x].strikethrough = 1;
                            else
                              tc[x].strikethrough = 0;
                            if (cells[j].att.autowrapped)
                              tc[x].underline = 1;
                            else
                              tc[x].underline = 0;
//                            if (cells[j].att.tab)
//                              tc[x].underline = 1;
//                            else
//                              tc[x].underline = 0;
                            if ((cells[j].att.newline) ||
                                (cells[j].att.autowrapped))
                              {
                                 fg = 8;
                                 bg = 4;
                                 codepoint = '!';
                              }
                         }
                       else
                         {
                            tc[x].underline = cells[j].att.underline;
                            tc[x].strikethrough = cells[j].att.strike;
                         }
                       tc[x].fg = fg;
                       tc[x].bg = bg;
                       tc[x].codepoint = codepoint;
#if defined(SUPPORT_DBLWIDTH)
                       tc[x].double_width = cells[j].att.dblwidth;
#endif
                       if ((tc[x].double_width) && (tc[x].codepoint == 0) &&
                           (ch2 == x - 1))
                         ch2 = x;
                       // cells[j].att.italic // never going 2 support
                       // cells[j].att.blink
                       // cells[j].att.blink2
                    }
               }
             j++;
          }
        evas_object_textgrid_cellrow_set(sd->grid.obj, y, tc);
        /* only bothering to keep 1 change span per row - not worth doing
         * more really */
        if (ch1 >= 0)
          evas_object_textgrid_update_add(sd->grid.obj, ch1, y,
                                          ch2 - ch1 + 1, 1);
          //printf("I'm fine thanx\n");
     }
   termpty_cellcomp_thaw(sd->pty);
   
   EINA_LIST_FOREACH_SAFE(sd->pty->block.active, l, ln, blk)
     {
        if (!blk->active)
          {
             blk->was_active = EINA_FALSE;
             // XXX: move to func
             if (blk->obj)
               {
                  // XXX: handle if edje not media
                  evas_object_event_callback_del_full
                    (blk->obj, EVAS_CALLBACK_DEL,
                        _smart_media_del, blk);
                  evas_object_del(blk->obj);
                  blk->obj = NULL;
               }
             sd->pty->block.active = eina_list_remove_list
               (sd->pty->block.active, l);
          }
     }
   if ((sd->scroll != 0) || (sd->pty->state.hidecursor))
     evas_object_hide(sd->cur.obj);
   else
     evas_object_show(sd->cur.obj);
   sd->cur.x = sd->pty->state.cx;
   sd->cur.y = sd->pty->state.cy;
   evas_object_move(sd->cur.obj,
                    ox + (sd->cur.x * sd->font.chw),
                    oy + (sd->cur.y * sd->font.chh));
   if (sd->cur.sel)
     {
        int start_x, start_y, end_x, end_y;
        int size_top, size_bottom;

        start_x = sd->cur.sel1.x;
        start_y = sd->cur.sel1.y;
        end_x = sd->cur.sel2.x;
        end_y = sd->cur.sel2.y;

        if (sd->boxsel)
          {
             if (start_y > end_y)
               INT_SWAP(start_y, end_y);
             if (start_x > end_x)
               INT_SWAP(start_x, end_x);
           }
         else
           {
              if ((start_y > end_y) ||
                  ((start_y == end_y) && (end_x < start_x)))
                {
                   INT_SWAP(start_y, end_y);
                   INT_SWAP(start_x, end_x);

                   if (sd->top_left)
                     {
                        sd->top_left = EINA_FALSE;
                        sd->bottom_right = EINA_TRUE;
                        edje_object_signal_emit(sd->cur.selo_theme,
                                                "mouse,out",
                                                "zone.top_left");
                        edje_object_signal_emit(sd->cur.selo_theme,
                                                "mouse,in",
                                                "zone.bottom_right");
                     }
                   else if (sd->bottom_right)
                     {
                        sd->top_left = EINA_TRUE;
                        sd->bottom_right = EINA_FALSE;
                        edje_object_signal_emit(sd->cur.selo_theme,
                                                "mouse,out",
                                                "zone.bottom_right");
                        edje_object_signal_emit(sd->cur.selo_theme,
                                                "mouse,in",
                                                "zone.top_left");
                     }
                }
           }
        size_top = start_x * sd->font.chw;

        size_bottom = (sd->grid.w - end_x - 1) * sd->font.chw;

        evas_object_size_hint_min_set(sd->cur.selo_top,
                                      size_top,
                                      sd->font.chh);
        evas_object_size_hint_max_set(sd->cur.selo_top,
                                      size_top,
                                      sd->font.chh);
        evas_object_size_hint_min_set(sd->cur.selo_bottom,
                                      size_bottom,
                                      sd->font.chh);
        evas_object_size_hint_max_set(sd->cur.selo_bottom,
                                      size_bottom,
                                      sd->font.chh);
        evas_object_move(sd->cur.selo_theme,
                         ox,
                         oy + ((start_y + sd->scroll) * sd->font.chh));
        evas_object_resize(sd->cur.selo_theme,
                           sd->grid.w * sd->font.chw,
                           (end_y + 1 - start_y) * sd->font.chh);

        if (sd->boxsel)
          {
             edje_object_signal_emit(sd->cur.selo_theme,
                                  "mode,oneline", "terminology");
          }
        else
          {
             if ((start_y == end_y) ||
                 ((start_x == 0) && (end_x == (sd->grid.w - 1))))
               {
                  edje_object_signal_emit(sd->cur.selo_theme,
                                          "mode,oneline", "terminology");
               }
             else if ((start_y == (end_y - 1)) &&
                      (start_x > end_x))
               {
                  edje_object_signal_emit(sd->cur.selo_theme,
                                          "mode,disjoint", "terminology");
               }
             else if (start_x == 0)
               {
                  edje_object_signal_emit(sd->cur.selo_theme,
                                          "mode,topfull", "terminology");
               }
             else if (end_x == (sd->grid.w - 1))
               {
                  edje_object_signal_emit(sd->cur.selo_theme,
                                          "mode,bottomfull", "terminology");
               }
             else
               {
                  edje_object_signal_emit(sd->cur.selo_theme,
                                          "mode,multiline", "terminology");
               }
          }
        evas_object_show(sd->cur.selo_theme);
     }
   else
     evas_object_hide(sd->cur.selo_theme);
   if (sd->mouseover_delay) ecore_timer_del(sd->mouseover_delay);
   sd->mouseover_delay = ecore_timer_add(0.05, _smart_mouseover_delay, obj);
   //printf("How are you today?\n?");
}

static void
_smart_size(Evas_Object *obj, int w, int h, Eina_Bool force)
{
   Termio *sd = evas_object_smart_data_get(obj);
   if (!sd) return;

   if (w < 1) w = 1;
   if (h < 1) h = 1;
   if (!force)
     {
        if ((w == sd->grid.w) && (h == sd->grid.h)) return;
     }
   sd->grid.w = w;
   sd->grid.h = h;

   evas_event_freeze(evas_object_evas_get(obj));
   evas_object_textgrid_size_set(sd->grid.obj, sd->grid.w, sd->grid.h);

   evas_object_resize(sd->cur.obj, sd->font.chw, sd->font.chh);
   evas_object_size_hint_min_set(obj, sd->font.chw, sd->font.chh);
   if (!sd->noreqsize)
     evas_object_size_hint_request_set(obj,
                                       sd->font.chw * sd->grid.w,
                                       sd->font.chh * sd->grid.h);
   termpty_resize(sd->pty, w, h);

   _smart_calculate(obj);
   _smart_apply(obj);
   if (sd->scrolio)
     {
        scrolio_miniview_resize(sd->scrolio, sd->pty, w * sd->font.chw, h * sd->font.chh);
        evas_object_smart_callback_call(obj, "miniview,show", NULL);
     }
   evas_event_thaw(evas_object_evas_get(obj));
}

static Eina_Bool
_smart_cb_delayed_size(void *data)
{
   Evas_Object *obj = data;
   Termio *sd = evas_object_smart_data_get(obj);
   Evas_Coord ow = 0, oh = 0;
   int w, h;

   if (!sd) return EINA_FALSE;
   sd->delayed_size_timer = NULL;

   evas_object_geometry_get(obj, NULL, NULL, &ow, &oh);

   w = ow / sd->font.chw;
   h = oh / sd->font.chh;
   _smart_size(obj, w, h, EINA_FALSE);
   return EINA_FALSE;
}

static Eina_Bool
_smart_cb_change(void *data)
{
   Evas_Object *obj = data;
   Termio *sd;
   sd = evas_object_smart_data_get(obj);
   if (!sd) return EINA_FALSE;
   sd->anim = NULL;
   _smart_apply(obj);
   evas_object_smart_callback_call(obj, "changed", NULL);
   if (sd->scrolio)
     scrolio_miniview_update_scroll(sd->scrolio, termio_scroll_get(obj));
   return EINA_FALSE;
}

static void
_smart_update_queue(Evas_Object *obj, Termio *sd)
{
   if (sd->anim) return;
   sd->anim = ecore_animator_add(_smart_cb_change, obj);
   if (sd->scrolio)
     scrolio_miniview_update_scroll(sd->scrolio, termio_scroll_get(obj));
}

static void
_lost_selection_reset_job(void *data)
{
   Termio *sd = evas_object_smart_data_get(data);
   if (!sd) return;
   sd->sel_reset_job = NULL;
   elm_cnp_selection_set(sd->win, sd->sel_type,
                         ELM_SEL_FORMAT_TEXT,
                         sd->sel_str, strlen(sd->sel_str));
   elm_cnp_selection_loss_callback_set(sd->win, sd->sel_type,
                                       _lost_selection, data);
}

static void
_lost_selection(void *data, Elm_Sel_Type selection)
{
   Eina_List *l;
   Evas_Object *obj;
   double t = ecore_time_get();
   EINA_LIST_FOREACH(terms, l, obj)
     {
        Termio *sd = evas_object_smart_data_get(obj);
        if (!sd) continue;
        if ((t - sd->set_sel_at) < 0.2) /// hack
          {
             if ((sd->have_sel) && (sd->sel_str) && (!sd->reset_sel))
               {
                  sd->reset_sel = EINA_TRUE;
                  if (sd->sel_reset_job) ecore_job_del(sd->sel_reset_job);
                  sd->sel_reset_job = ecore_job_add
                    (_lost_selection_reset_job, data);
               }
             continue;
          }
        if (sd->have_sel)
          {
             if (sd->sel_str)
               {
                  eina_stringshare_del(sd->sel_str);
                  sd->sel_str = NULL;
               }
             sd->cur.sel = 0;
             elm_object_cnp_selection_clear(sd->win, selection);
             _smart_update_queue(obj, sd);
             sd->have_sel = EINA_FALSE;
          }
     }
}

static void
_take_selection(Evas_Object *obj, Elm_Sel_Type type)
{
   Termio *sd = evas_object_smart_data_get(obj);
   int start_x = 0, start_y = 0, end_x = 0, end_y = 0;
   char *s = NULL;
   size_t len;

   if (!sd) return;
   if (sd->cur.sel)
     {
        start_x = sd->cur.sel1.x;
        start_y = sd->cur.sel1.y;
        end_x = sd->cur.sel2.x;
        end_y = sd->cur.sel2.y;
     }

   if (sd->boxsel)
     {
        int i;
        Eina_Strbuf *sb;

        if (start_y > end_y)
          INT_SWAP(start_y, end_y);
        if (start_x > end_x)
          INT_SWAP(start_x, end_x);

        sb = eina_strbuf_new();
        for (i = start_y; i <= end_y; i++)
          {
             char *tmp = termio_selection_get(obj, start_x, i, end_x, i,
                                              &len);

             eina_strbuf_append_length(sb, tmp, len);
             if (len && tmp[len - 1] != '\n')
               eina_strbuf_append_char(sb, '\n');
             free(tmp);
          }
        len = eina_strbuf_length_get(sb);
        s = eina_strbuf_string_steal(sb);
        eina_strbuf_free(sb);
     }
   else if (!start_y && !end_y && !start_x && !end_x && sd->link.string)
     s = strdup(sd->link.string);
   else if ((start_x != end_x) || (start_y != end_y))
     {
        if ((start_y > end_y) || ((start_y == end_y) && (end_x < start_x)))
          {
             INT_SWAP(start_y, end_y);
             INT_SWAP(start_x, end_x);
          }
        s = termio_selection_get(obj, start_x, start_y, end_x, end_y, &len);
     }

   if (s)
     {
        if (sd->win)
          {
             sd->have_sel = EINA_FALSE;
             sd->reset_sel = EINA_FALSE;
             sd->set_sel_at = ecore_time_get(); // hack
             sd->sel_type = type;
             elm_cnp_selection_set(sd->win, type,
                                   ELM_SEL_FORMAT_TEXT, s, len);
             elm_cnp_selection_loss_callback_set(sd->win, type,
                                                 _lost_selection, obj);
             sd->have_sel = EINA_TRUE;
             if (sd->sel_str) eina_stringshare_del(sd->sel_str);
             sd->sel_str = eina_stringshare_add(s);
          }
        free(s);
     }
}

static Eina_Bool
_getsel_cb(void *data, Evas_Object *obj __UNUSED__, Elm_Selection_Data *ev)
{
   Termio *sd = evas_object_smart_data_get(data);
   if (!sd) return EINA_FALSE;

   if (ev->format == ELM_SEL_FORMAT_TEXT)
     {
        if (ev->len > 0)
          {
             char *tmp, *s;
             size_t i;

             // apparently we have to convert \n into \r in terminal land.
             tmp = malloc(ev->len);
             if (tmp)
               {
                  s = ev->data;
                  for (i = 0; i < ev->len; i++)
                    {
                       tmp[i] = s[i];
                       if (tmp[i] == '\n') tmp[i] = '\r';
                    }
                  termpty_write(sd->pty, tmp, ev->len - 1);
                  free(tmp);
               }
          }
     }
   return EINA_TRUE;
}

static void
_paste_selection(Evas_Object *obj, Elm_Sel_Type type)
{
   Termio *sd = evas_object_smart_data_get(obj);
   if (!sd) return;
   if (!sd->win) return;
   elm_cnp_selection_get(sd->win, type, ELM_SEL_FORMAT_TEXT,
                         _getsel_cb, obj);
}

static void
_font_size_set(Evas_Object *obj, int size)
{
   Termio *sd = evas_object_smart_data_get(obj);
   Config *config = termio_config_get(obj);
   if (!sd) return;

   if (size < 5) size = 5;
   else if (size > 100) size = 100;
   if (config)
     {
        Evas_Coord mw = 1, mh = 1;
        int gw, gh;
        
        config->temporary = EINA_TRUE;
        config->font.size = size;
        gw = sd->grid.w;
        gh = sd->grid.h;
        sd->noreqsize = 1;
        termio_config_update(obj);
        sd->noreqsize = 0;
        evas_object_size_hint_min_get(obj, &mw, &mh);
        evas_object_data_del(obj, "sizedone");
        evas_object_size_hint_request_set(obj, mw * gw, mh * gh);
     }
}

void
termio_font_size_set(Evas_Object *obj, int size)
{
   _font_size_set(obj, size);
}

void
termio_grid_size_set(Evas_Object *obj, int w, int h)
{
   Termio *sd = evas_object_smart_data_get(obj);
   Evas_Coord mw = 1, mh = 1;
   
   if (w < 1) w = 1;
   if (h < 1) h = 1;
   if (!sd) return;
   evas_object_size_hint_min_get(obj, &mw, &mh);
   evas_object_data_del(obj, "sizedone");
   evas_object_size_hint_request_set(obj, mw * w, mh * h);
}

static void
_smart_cb_key_up(void *data, Evas *e __UNUSED__, Evas_Object *obj __UNUSED__, void *event)
{
   Evas_Event_Key_Up *ev = event;
   Termio *sd;

   sd = evas_object_smart_data_get(data);
   if (!sd) return;
   sd->last_keyup = ev->timestamp;
   if (sd->imf)
     {
        Ecore_IMF_Event_Key_Up imf_ev;
        ecore_imf_evas_event_key_up_wrap(ev, &imf_ev);
        if (ecore_imf_context_filter_event
            (sd->imf, ECORE_IMF_EVENT_KEY_UP, (Ecore_IMF_Event *)&imf_ev))
          return;
     }
}

static Eina_Bool
_is_modifier(const char *key)
{
   if ((!strncmp(key, "Shift", 5)) ||
       (!strncmp(key, "Control", 7)) ||
       (!strncmp(key, "Alt", 3)) ||
       (!strncmp(key, "Meta", 4)) ||
       (!strncmp(key, "Super", 5)) ||
       (!strncmp(key, "Hyper", 5)) ||
       (!strcmp(key, "Scroll_Lock")) ||
       (!strcmp(key, "Num_Lock")) ||
       (!strcmp(key, "Caps_Lock")))
     return EINA_TRUE;
   return EINA_FALSE;
}

static void
_compose_seq_reset(Termio *sd)
{
   char *str;
   
   EINA_LIST_FREE(sd->seq, str) eina_stringshare_del(str);
   sd->composing = EINA_FALSE;
}

static Eina_Bool
_handle_alt_ctrl(const char *keyname, Evas_Object *term)
{
   if (!strcmp(keyname, "equal"))
     termcmd_do(term, NULL, NULL, "f+");
   else if (!strcmp(keyname, "minus"))
     termcmd_do(term, NULL, NULL, "f-");
   else if (!strcmp(keyname, "0"))
     termcmd_do(term, NULL, NULL, "f");
   else if (!strcmp(keyname, "9"))
     termcmd_do(term, NULL, NULL, "fb");
   else
     return EINA_FALSE;

   return EINA_TRUE;
}

static Eina_Bool
_handle_shift(Evas_Event_Key_Down *ev, int by, Evas_Object *term, Termio *sd)
{
   if (!strcmp(ev->keyname, "Prior"))
     {
        sd->scroll += by;
        if (sd->scroll > sd->pty->backscroll_num)
          sd->scroll = sd->pty->backscroll_num;
        _smart_update_queue(term, sd);
     }
   else if (!strcmp(ev->keyname, "Next"))
     {
        sd->scroll -= by;
        if (sd->scroll < 0) sd->scroll = 0;
        _smart_update_queue(term, sd);
     }
   else if (!strcmp(ev->keyname, "Insert"))
     {
        if (evas_key_modifier_is_set(ev->modifiers, "Control"))
          _paste_selection(term, ELM_SEL_TYPE_PRIMARY);
        else
          _paste_selection(term, ELM_SEL_TYPE_CLIPBOARD);
     }
   else if (!strcmp(ev->keyname, "KP_Add"))
     {
        Config *config = termio_config_get(term);
        
        if (config) _font_size_set(term, config->font.size + 1);
     }
   else if (!strcmp(ev->keyname, "KP_Subtract"))
     {
        Config *config = termio_config_get(term);
        
        if (config) _font_size_set(term, config->font.size - 1);
     }
   else if (!strcmp(ev->keyname, "KP_Multiply"))
     {
        Config *config = termio_config_get(term);
        
        if (config) _font_size_set(term, 10);
     }
   else if (!strcmp(ev->keyname, "KP_Divide"))
     _take_selection(term, ELM_SEL_TYPE_CLIPBOARD);
   else
     return EINA_FALSE;

   return EINA_TRUE;
}

void
termio_miniview_hide(Evas_Object *obj)
{
   Termio *sd = evas_object_smart_data_get(obj);
   if (!sd) return;

   scrolio_miniview_hide(sd->scrolio);
   sd->scrolio = NULL;
}

Evas_Object *
termio_miniview_show(Evas_Object *obj, int x, int y, int w, int h)
{
   Termio *sd = evas_object_smart_data_get(obj);
   if (!sd) return;

   sd->scrolio = (Evas_Object *) scrolio_miniview_add(obj, sd->font.chw, sd->font.chh,
                                sd->pty, sd->pty->backscroll_num,
                                termio_scroll_get(obj), x, y, w, h);
   return sd->scrolio;
}

void
_smart_cb_key_down(void *data, Evas *e __UNUSED__, Evas_Object *obj __UNUSED__, void *event)
{
   Evas_Event_Key_Down *ev = event;
   Termio *sd;
   Ecore_Compose_State state;
   char *compres = NULL;

   sd = evas_object_smart_data_get(data);
   if (!sd) return;
   if ((!evas_key_modifier_is_set(ev->modifiers, "Alt")) &&
       (evas_key_modifier_is_set(ev->modifiers, "Control")) &&
       (!evas_key_modifier_is_set(ev->modifiers, "Shift")))
     {
        if (!strcmp(ev->keyname, "Prior"))
          {
             evas_object_smart_callback_call(data, "prev", NULL);
             goto end;
          }
        else if (!strcmp(ev->keyname, "Next"))
          {
             evas_object_smart_callback_call(data, "next", NULL);
             goto end;
          }
        else if (!strcmp(ev->keyname, "1"))
          {
             _compose_seq_reset(sd);
             evas_object_smart_callback_call(data, "tab,1", NULL);
             goto end;
          }
        else if (!strcmp(ev->keyname, "2"))
          {
             _compose_seq_reset(sd);
             evas_object_smart_callback_call(data, "tab,2", NULL);
             goto end;
          }
        else if (!strcmp(ev->keyname, "3"))
          {
             _compose_seq_reset(sd);
             evas_object_smart_callback_call(data, "tab,3", NULL);
             goto end;
          }
        else if (!strcmp(ev->keyname, "4"))
          {
             _compose_seq_reset(sd);
             evas_object_smart_callback_call(data, "tab,4", NULL);
             goto end;
          }
        else if (!strcmp(ev->keyname, "5"))
          {
             _compose_seq_reset(sd);
             evas_object_smart_callback_call(data, "tab,5", NULL);
             goto end;
          }
        else if (!strcmp(ev->keyname, "6"))
          {
             _compose_seq_reset(sd);
             evas_object_smart_callback_call(data, "tab,6", NULL);
             goto end;
          }
        else if (!strcmp(ev->keyname, "7"))
          {
             _compose_seq_reset(sd);
             evas_object_smart_callback_call(data, "tab,7", NULL);
             goto end;
          }
        else if (!strcmp(ev->keyname, "8"))
          {
             _compose_seq_reset(sd);
             evas_object_smart_callback_call(data, "tab,8", NULL);
             goto end;
          }
        else if (!strcmp(ev->keyname, "9"))
          {
             _compose_seq_reset(sd);
             evas_object_smart_callback_call(data, "tab,9", NULL);
             goto end;
          }
        else if (!strcmp(ev->keyname, "0"))
          {
             _compose_seq_reset(sd);
             evas_object_smart_callback_call(data, "tab,0", NULL);
             goto end;
          }
     }
   if ((!evas_key_modifier_is_set(ev->modifiers, "Alt")) &&
       (evas_key_modifier_is_set(ev->modifiers, "Control")) &&
       (evas_key_modifier_is_set(ev->modifiers, "Shift")))
     {
        if (!strcmp(ev->keyname, "Prior"))
          {
             _compose_seq_reset(sd);
             evas_object_smart_callback_call(data, "split,h", NULL);
             goto end;
          }
        else if (!strcmp(ev->keyname, "Next"))
          {
             _compose_seq_reset(sd);
             evas_object_smart_callback_call(data, "split,v", NULL);
             goto end;
          }
        else if (!strcmp(ev->keyname, "t"))
          {
             _compose_seq_reset(sd);
             evas_object_smart_callback_call(data, "new", NULL);
             goto end;
          }
        else if (!strcmp(ev->keyname, "Home"))
          {
             _compose_seq_reset(sd);
             evas_object_smart_callback_call(data, "select", NULL);
             goto end;
          }
        else if (!strcmp(ev->keyname, "c"))
          {
             _compose_seq_reset(sd);
             _take_selection(data, ELM_SEL_TYPE_CLIPBOARD);
             goto end;
          }
        else if (!strcmp(ev->keyname, "v"))
          {
             _compose_seq_reset(sd);
             _paste_selection(data, ELM_SEL_TYPE_CLIPBOARD);
             goto end;
          }
        else if (!strcmp(ev->keyname, "f"))
          {
             evas_object_smart_callback_call(data, "miniview,toggle", NULL);
             goto end;
          }
     }
   if ((evas_key_modifier_is_set(ev->modifiers, "Alt")) &&
       (!evas_key_modifier_is_set(ev->modifiers, "Shift")) &&
       (!evas_key_modifier_is_set(ev->modifiers, "Control")))
     {
        if (!strcmp(ev->keyname, "Home"))
          {
             _compose_seq_reset(sd);
             evas_object_smart_callback_call(data, "cmdbox", NULL);
             goto end;
          }
        else if (!strcmp(ev->keyname, "Return"))
          {
             _compose_seq_reset(sd);
             _paste_selection(data, ELM_SEL_TYPE_PRIMARY);
             goto end;
          }
     }
   if ((evas_key_modifier_is_set(ev->modifiers, "Alt")) &&
       (evas_key_modifier_is_set(ev->modifiers, "Control")) &&
       (!evas_key_modifier_is_set(ev->modifiers, "Shift")))
     {
        if (_handle_alt_ctrl(ev->keyname, data))
          {
             _compose_seq_reset(sd);
             goto end;
          }
     }
   if (sd->imf)
     {
        // EXCEPTION. Don't filter modifiers alt+shift -> breaks emacs
        // and jed (alt+shift+5 for search/replace for example)
        // Don't filter modifiers alt, is used by shells
        if (!evas_key_modifier_is_set(ev->modifiers, "Alt"))
          {
             Ecore_IMF_Event_Key_Down imf_ev;

             ecore_imf_evas_event_key_down_wrap(ev, &imf_ev);
             if (!sd->composing)
               {
                  if (ecore_imf_context_filter_event
                      (sd->imf, ECORE_IMF_EVENT_KEY_DOWN, (Ecore_IMF_Event *)&imf_ev))
                    goto end;
               }
          }
     }
   if ((evas_key_modifier_is_set(ev->modifiers, "Shift")) &&
       (ev->keyname))
     {
        int by = sd->grid.h - 2;

        if (by < 1) by = 1;

        if (_handle_shift(ev, by, data, sd))
          {
             _compose_seq_reset(sd);
             goto end;
          }
     }
   if (sd->jump_on_keypress)
     {
        if (!_is_modifier(ev->key))
          {
             sd->scroll = 0;
             _smart_update_queue(data, sd);
          }
     }
   // if term app asked fro kbd lock - dont handle here
   if (sd->pty->state.kbd_lock) return;
   // if app asked us to not do autorepeat - ignore pree is it is the same
   // timestamp as last one
   if ((sd->pty->state.no_autorepeat) &&
       (ev->timestamp == sd->last_keyup)) return;
   if (!sd->composing)
     {
        _compose_seq_reset(sd);
        sd->seq = eina_list_append(sd->seq, eina_stringshare_add(ev->key));
        state = ecore_compose_get(sd->seq, &compres);
        if (state == ECORE_COMPOSE_MIDDLE) sd->composing = EINA_TRUE;
        else sd->composing = EINA_FALSE;
        if (!sd->composing) _compose_seq_reset(sd);
        else goto end;
     }
   else
     {
        if (_is_modifier(ev->key)) goto end;
        sd->seq = eina_list_append(sd->seq, eina_stringshare_add(ev->key));
        state = ecore_compose_get(sd->seq, &compres);
        if (state == ECORE_COMPOSE_NONE) _compose_seq_reset(sd);
        else if (state == ECORE_COMPOSE_DONE)
          {
             _compose_seq_reset(sd);
             if (compres)
               {
                  termpty_write(sd->pty, compres, strlen(compres));
                  free(compres);
                  compres = NULL;
               }
             goto end;
          }
        else goto end;
     }
   keyin_handle(sd->pty, ev);
end:
   if (sd->config->flicker_on_key)
     edje_object_signal_emit(sd->cur.obj, "key,down", "terminology");
}

static void
_imf_cursor_set(Termio *sd)
{
   /* TODO */
   Evas_Coord cx, cy, cw, ch;
   evas_object_geometry_get(sd->cur.obj, &cx, &cy, &cw, &ch);
   if (sd->imf)
     ecore_imf_context_cursor_location_set(sd->imf, cx, cy, cw, ch);
   /*
    ecore_imf_context_cursor_position_set(sd->imf, 0); // how to get it?
    */
}

void
_smart_cb_focus_in(void *data, Evas *e __UNUSED__, Evas_Object *obj __UNUSED__, void *event __UNUSED__)
{
   Termio *sd;

   sd = evas_object_smart_data_get(data);
   if (!sd) return;
   if (sd->config->disable_cursor_blink)
     edje_object_signal_emit(sd->cur.obj, "focus,in,noblink", "terminology");
   else
     edje_object_signal_emit(sd->cur.obj, "focus,in", "terminology");
   if (!sd->win) return;
   elm_win_keyboard_mode_set(sd->win, ELM_WIN_KEYBOARD_TERMINAL);
   if (sd->imf)
     {
        ecore_imf_context_input_panel_show(sd->imf);
        ecore_imf_context_reset(sd->imf);
        ecore_imf_context_focus_in(sd->imf);
        _imf_cursor_set(sd);
     }
}

void
_smart_cb_focus_out(void *data, Evas *e __UNUSED__, Evas_Object *obj __UNUSED__, void *event __UNUSED__)
{
   Termio *sd;

   sd = evas_object_smart_data_get(data);
   if (!sd) return;
   edje_object_signal_emit(sd->cur.obj, "focus,out", "terminology");
   if (!sd->win) return;
   elm_win_keyboard_mode_set(sd->win, ELM_WIN_KEYBOARD_OFF);
   if (sd->imf)
     {
        ecore_imf_context_reset(sd->imf);
        _imf_cursor_set(sd);
        ecore_imf_context_focus_out(sd->imf);
        ecore_imf_context_input_panel_hide(sd->imf);
     }
}

static void
_smart_xy_to_cursor(Evas_Object *obj, Evas_Coord x, Evas_Coord y, int *cx, int *cy)
{
   Termio *sd;
   Evas_Coord ox, oy;

   sd = evas_object_smart_data_get(obj);
   if (!sd)
     {
        *cx = 0;
        *cy = 0;
        return;
     }
   evas_object_geometry_get(obj, &ox, &oy, NULL, NULL);
   *cx = (x - ox) / sd->font.chw;
   *cy = (y - oy) / sd->font.chh;
   if (*cx < 0) *cx = 0;
   else if (*cx >= sd->grid.w) *cx = sd->grid.w - 1;
   if (*cy < 0) *cy = 0;
   else if (*cy >= sd->grid.h) *cy = sd->grid.h - 1;
}

static void
_sel_line(Evas_Object *obj, int cx __UNUSED__, int cy)
{
   Termio *sd = evas_object_smart_data_get(obj);
   if (!sd) return;

   sd->cur.sel = 1;
   sd->cur.makesel = 0;
   sd->cur.sel1.x = 0;
   sd->cur.sel1.y = cy;
   sd->cur.sel2.x = sd->grid.w - 1;
   sd->cur.sel2.y = cy;
}

static Eina_Bool
_codepoint_is_wordsep(const Config *config, int g)
{
   int i;

   if (g == 0) return EINA_TRUE;
   if (!config->wordsep) return EINA_FALSE;
   for (i = 0;;)
     {
        int g2 = 0;

        if (!config->wordsep[i]) break;
        i = evas_string_char_next_get(config->wordsep, i, &g2);
        if (i < 0) break;
        if (g == g2) return EINA_TRUE;
     }
   return EINA_FALSE;
}

static void
_sel_word(Evas_Object *obj, int cx, int cy)
{
   Termio *sd = evas_object_smart_data_get(obj);
   Termcell *cells;
   int x, w = 0;
   if (!sd) return;

   termpty_cellcomp_freeze(sd->pty);
   cells = termpty_cellrow_get(sd->pty, cy, &w);
   if (!cells)
     {
        termpty_cellcomp_thaw(sd->pty);
        return;
     }
   sd->cur.sel = 1;
   sd->cur.makesel = 0;
   sd->cur.sel1.x = cx;
   sd->cur.sel1.y = cy;
   for (x = sd->cur.sel1.x; x >= 0; x--)
     {
#if defined(SUPPORT_DBLWIDTH)
        if ((cells[x].codepoint == 0) && (cells[x].att.dblwidth) &&
            (x > 0))
          x--;
#endif
        if (x >= w) break;
        if (_codepoint_is_wordsep(sd->config, cells[x].codepoint)) break;
        sd->cur.sel1.x = x;
     }
   sd->cur.sel2.x = cx;
   sd->cur.sel2.y = cy;
   for (x = sd->cur.sel2.x; x < sd->grid.w; x++)
     {
#if defined(SUPPORT_DBLWIDTH)
        if ((cells[x].codepoint == 0) && (cells[x].att.dblwidth) &&
            (x < (sd->grid.w - 1)))
          {
             sd->cur.sel2.x = x;
             x++;
          }
#endif
        if (x >= w) break;
        if (_codepoint_is_wordsep(sd->config, cells[x].codepoint)) break;
        sd->cur.sel2.x = x;
     }
   termpty_cellcomp_thaw(sd->pty);
}

static void
_sel_word_to(Evas_Object *obj, int cx, int cy)
{
   Termio *sd = evas_object_smart_data_get(obj);
   Termcell *cells;
   int x, w = 0;
   if (!sd) return;

   termpty_cellcomp_freeze(sd->pty);
   cells = termpty_cellrow_get(sd->pty, cy, &w);
   if (!cells)
     {
        termpty_cellcomp_thaw(sd->pty);
        return;
     }
   if (sd->cur.sel1.x > cx || sd->cur.sel1.y > cy)
     {
        sd->cur.sel1.x = cx;
        sd->cur.sel1.y = cy;
        for (x = sd->cur.sel1.x; x >= 0; x--)
          {
#if defined(SUPPORT_DBLWIDTH)
             if ((cells[x].codepoint == 0) && (cells[x].att.dblwidth) &&
                 (x > 0))
               x--;
#endif
             if (x >= w) break;
             if (_codepoint_is_wordsep(sd->config, cells[x].codepoint)) break;
             sd->cur.sel1.x = x;
          }
     }
   else if (sd->cur.sel2.x < cx || sd->cur.sel2.y < cy)
     {
        sd->cur.sel2.x = cx;
        sd->cur.sel2.y = cy;
        for (x = sd->cur.sel2.x; x < sd->grid.w; x++)
          {
#if defined(SUPPORT_DBLWIDTH)
             if ((cells[x].codepoint == 0) && (cells[x].att.dblwidth) &&
                 (x < (sd->grid.w - 1)))
               {
                  sd->cur.sel2.x = x;
                  x++;
               }
#endif
             if (x >= w) break;
             if (_codepoint_is_wordsep(sd->config, cells[x].codepoint)) break;
             sd->cur.sel2.x = x;
          }
     }
   termpty_cellcomp_thaw(sd->pty);
}

static Eina_Bool
_rep_mouse_down(Termio *sd, Evas_Event_Mouse_Down *ev, int cx, int cy)
{
   char buf[64];
   Eina_Bool ret = EINA_FALSE;
   int btn;

   if (sd->pty->mouse_mode == MOUSE_OFF) return EINA_FALSE;
   if (!sd->mouse.button)
     {
        /* Need to remember the first button pressed for terminal handling */
        sd->mouse.button = ev->button;
     }

   btn = ev->button - 1;
   switch (sd->pty->mouse_ext)
     {
      case MOUSE_EXT_NONE:
        if ((cx < (0xff - ' ')) && (cy < (0xff - ' ')))
          {
             if (sd->pty->mouse_mode == MOUSE_X10)
               {
                  if (btn <= 2)
                    {
                       buf[0] = 0x1b;
                       buf[1] = '[';
                       buf[2] = 'M';
                       buf[3] = btn + ' ';
                       buf[4] = cx + 1 + ' ';
                       buf[5] = cy + 1 + ' ';
                       buf[6] = 0;
                       termpty_write(sd->pty, buf, strlen(buf));
                       ret = EINA_TRUE;
                    }
               }
             else
               {
                  int shift = evas_key_modifier_is_set(ev->modifiers, "Shift") ? 4 : 0;
                  int meta = evas_key_modifier_is_set(ev->modifiers, "Alt") ? 8 : 0;
                  int ctrl = evas_key_modifier_is_set(ev->modifiers, "Control") ? 16 : 0;

                  if (btn > 2) btn = 0;
                  buf[0] = 0x1b;
                  buf[1] = '[';
                  buf[2] = 'M';
                  buf[3] = (btn | shift | meta | ctrl) + ' ';
                  buf[4] = cx + 1 + ' ';
                  buf[5] = cy + 1 + ' ';
                  buf[6] = 0;
                  termpty_write(sd->pty, buf, strlen(buf));
                  ret = EINA_TRUE;
               }
          }
        break;
      case MOUSE_EXT_UTF8: // ESC.[.M.BTN/FLGS.XUTF8.YUTF8
          {
             int shift = evas_key_modifier_is_set(ev->modifiers, "Shift") ? 4 : 0;
             int meta = evas_key_modifier_is_set(ev->modifiers, "Alt") ? 8 : 0;
             int ctrl = evas_key_modifier_is_set(ev->modifiers, "Control") ? 16 : 0;
             int v, i;

             if (btn > 2) btn = 0;
             buf[0] = 0x1b;
             buf[1] = '[';
             buf[2] = 'M';
             buf[3] = (btn | shift | meta | ctrl) + ' ';
             i = 4;
             v = cx + 1 + ' ';
             if (v <= 127) buf[i++] = v;
             else
               { // 14 bits for cx/cy - enough i think
                   buf[i++] = 0xc0 + (v >> 6);
                   buf[i++] = 0x80 + (v & 0x3f);
               }
             v = cy + 1 + ' ';
             if (v <= 127) buf[i++] = v;
             else
               { // 14 bits for cx/cy - enough i think
                   buf[i++] = 0xc0 + (v >> 6);
                   buf[i++] = 0x80 + (v & 0x3f);
               }
             buf[i] = 0;
             termpty_write(sd->pty, buf, strlen(buf));
             ret = EINA_TRUE;
          }
        break;
      case MOUSE_EXT_SGR: // ESC.[.<.NUM.;.NUM.;.NUM.M
          {
             int shift = evas_key_modifier_is_set(ev->modifiers, "Shift") ? 4 : 0;
             int meta = evas_key_modifier_is_set(ev->modifiers, "Alt") ? 8 : 0;
             int ctrl = evas_key_modifier_is_set(ev->modifiers, "Control") ? 16 : 0;

             snprintf(buf, sizeof(buf), "%c[<%i;%i;%iM", 0x1b,
                      (btn | shift | meta | ctrl), cx + 1, cy + 1);
             termpty_write(sd->pty, buf, strlen(buf));
             ret = EINA_TRUE;
          }
        break;
      case MOUSE_EXT_URXVT: // ESC.[.NUM.;.NUM.;.NUM.M
          {
             int shift = evas_key_modifier_is_set(ev->modifiers, "Shift") ? 4 : 0;
             int meta = evas_key_modifier_is_set(ev->modifiers, "Alt") ? 8 : 0;
             int ctrl = evas_key_modifier_is_set(ev->modifiers, "Control") ? 16 : 0;

             if (btn > 2) btn = 0;
             snprintf(buf, sizeof(buf), "%c[%i;%i;%iM", 0x1b,
                      (btn | shift | meta | ctrl) + ' ',
                      cx + 1, cy + 1);
             termpty_write(sd->pty, buf, strlen(buf));
             ret = EINA_TRUE;
          }
        break;
      default:
        break;
     }
   return ret;
}

static Eina_Bool
_rep_mouse_up(Termio *sd, Evas_Event_Mouse_Up *ev, int cx, int cy)
{
   char buf[64];
   Eina_Bool ret = EINA_FALSE;
   int shift, meta, ctrl;

   if ((sd->pty->mouse_mode == MOUSE_OFF) ||
       (sd->pty->mouse_mode == MOUSE_X10))
     return EINA_FALSE;
   if (sd->mouse.button == ev->button)
     sd->mouse.button = 0;

   shift = evas_key_modifier_is_set(ev->modifiers, "Shift") ? 4 : 0;
   meta = evas_key_modifier_is_set(ev->modifiers, "Alt") ? 8 : 0;
   ctrl = evas_key_modifier_is_set(ev->modifiers, "Control") ? 16 : 0;

   switch (sd->pty->mouse_ext)
     {
      case MOUSE_EXT_NONE:
        if ((cx < (0xff - ' ')) && (cy < (0xff - ' ')))
          {
             buf[0] = 0x1b;
             buf[1] = '[';
             buf[2] = 'M';
             buf[3] = (3 | shift | meta | ctrl) + ' ';
             buf[4] = cx + 1 + ' ';
             buf[5] = cy + 1 + ' ';
             buf[6] = 0;
             termpty_write(sd->pty, buf, strlen(buf));
             ret = EINA_TRUE;
          }
        break;
      case MOUSE_EXT_UTF8: // ESC.[.M.BTN/FLGS.XUTF8.YUTF8
          {
             int v, i;

             buf[0] = 0x1b;
             buf[1] = '[';
             buf[2] = 'M';
             buf[3] = (3 | shift | meta | ctrl) + ' ';
             i = 4;
             v = cx + 1 + ' ';
             if (v <= 127) buf[i++] = v;
             else
               { // 14 bits for cx/cy - enough i think
                   buf[i++] = 0xc0 + (v >> 6);
                   buf[i++] = 0x80 + (v & 0x3f);
               }
             v = cy + 1 + ' ';
             if (v <= 127) buf[i++] = v;
             else
               { // 14 bits for cx/cy - enough i think
                   buf[i++] = 0xc0 + (v >> 6);
                   buf[i++] = 0x80 + (v & 0x3f);
               }
             buf[i] = 0;
             termpty_write(sd->pty, buf, strlen(buf));
             ret = EINA_TRUE;
          }
        break;
      case MOUSE_EXT_SGR: // ESC.[.<.NUM.;.NUM.;.NUM.m
          {
             snprintf(buf, sizeof(buf), "%c[<%i;%i;%im", 0x1b,
                      (3 | shift | meta | ctrl), cx + 1, cy + 1);
             termpty_write(sd->pty, buf, strlen(buf));
             ret = EINA_TRUE;
          }
        break;
      case MOUSE_EXT_URXVT: // ESC.[.NUM.;.NUM.;.NUM.M
          {
             snprintf(buf, sizeof(buf), "%c[%i;%i;%iM", 0x1b,
                      (3 | shift | meta | ctrl) + ' ',
                      cx + 1, cy + 1);
             termpty_write(sd->pty, buf, strlen(buf));
             ret = EINA_TRUE;
          }
        break;
      default:
        break;
     }
   return ret;
}

static Eina_Bool
_rep_mouse_move(Termio *sd, Evas_Event_Mouse_Move *ev, int cx __UNUSED__, int cy __UNUSED__, Eina_Bool change)
{
   char buf[64];
   Eina_Bool ret = EINA_FALSE;
   int btn, shift, meta, ctrl;

   if ((sd->pty->mouse_mode == MOUSE_OFF) ||
       (sd->pty->mouse_mode == MOUSE_X10) ||
       (sd->pty->mouse_mode == MOUSE_NORMAL))
     return EINA_FALSE;

   if ((!sd->mouse.button) && (sd->pty->mouse_mode == MOUSE_NORMAL_BTN_MOVE))
     return EINA_FALSE;

   if (!change) return EINA_TRUE;

   btn = sd->mouse.button - 1;
   shift = evas_key_modifier_is_set(ev->modifiers, "Shift") ? 4 : 0;
   meta = evas_key_modifier_is_set(ev->modifiers, "Alt") ? 8 : 0;
   ctrl = evas_key_modifier_is_set(ev->modifiers, "Control") ? 16 : 0;

   switch (sd->pty->mouse_ext)
     {
      case MOUSE_EXT_NONE:
        if ((cx < (0xff - ' ')) && (cy < (0xff - ' ')))
          {
             if (btn > 2) btn = 0;
             buf[0] = 0x1b;
             buf[1] = '[';
             buf[2] = 'M';
             buf[3] = (btn | shift | meta | ctrl | 32) + ' ';
             buf[4] = cx + 1 + ' ';
             buf[5] = cy + 1 + ' ';
             buf[6] = 0;
             termpty_write(sd->pty, buf, strlen(buf));
             ret = EINA_TRUE;
          }
        break;
      case MOUSE_EXT_UTF8: // ESC.[.M.BTN/FLGS.XUTF8.YUTF8
          {
             int v, i;

             if (btn > 2) btn = 0;
             buf[0] = 0x1b;
             buf[1] = '[';
             buf[2] = 'M';
             buf[3] = (btn | shift | meta | ctrl | 32) + ' ';
             i = 4;
             v = cx + 1 + ' ';
             if (v <= 127) buf[i++] = v;
             else
               { // 14 bits for cx/cy - enough i think
                   buf[i++] = 0xc0 + (v >> 6);
                   buf[i++] = 0x80 + (v & 0x3f);
               }
             v = cy + 1 + ' ';
             if (v <= 127) buf[i++] = v;
             else
               { // 14 bits for cx/cy - enough i think
                   buf[i++] = 0xc0 + (v >> 6);
                   buf[i++] = 0x80 + (v & 0x3f);
               }
             buf[i] = 0;
             termpty_write(sd->pty, buf, strlen(buf));
             ret = EINA_TRUE;
          }
        break;
      case MOUSE_EXT_SGR: // ESC.[.<.NUM.;.NUM.;.NUM.M
          {
             snprintf(buf, sizeof(buf), "%c[<%i;%i;%iM", 0x1b,
                      (btn | shift | meta | ctrl | 32), cx + 1, cy + 1);
             termpty_write(sd->pty, buf, strlen(buf));
             ret = EINA_TRUE;
          }
        break;
      case MOUSE_EXT_URXVT: // ESC.[.NUM.;.NUM.;.NUM.M
          {
             if (btn > 2) btn = 0;
             snprintf(buf, sizeof(buf), "%c[%i;%i;%iM", 0x1b,
                      (btn | shift | meta | ctrl | 32) + ' ',
                      cx + 1, cy + 1);
             termpty_write(sd->pty, buf, strlen(buf));
             ret = EINA_TRUE;
          }
        break;
      default:
        break;
     }
   return ret;
}

#if defined(SUPPORT_DBLWIDTH)
static void
_selection_dbl_fix(Evas_Object *obj)
{
   Termio *sd;
   int w = 0;
   Termcell *cells;
   
   sd = evas_object_smart_data_get(obj);
   if (!sd) return;
   termpty_cellcomp_freeze(sd->pty);
   cells = termpty_cellrow_get(sd->pty, sd->cur.sel2.y - sd->scroll, &w);
   if (cells)
     {
        // if sel2 after sel1
        if ((sd->cur.sel2.y > sd->cur.sel1.y) ||
            ((sd->cur.sel2.y == sd->cur.sel1.y) &&
                (sd->cur.sel2.x >= sd->cur.sel1.x)))
          {
             if (sd->cur.sel2.x < (w - 1))
               {
                  if ((cells[sd->cur.sel2.x].codepoint != 0) &&
                      (cells[sd->cur.sel2.x].att.dblwidth))
                    sd->cur.sel2.x++;
               }
          }
        // else sel1 after sel 2
        else
          {
             if (sd->cur.sel2.x > 0)
               {
                  if ((cells[sd->cur.sel2.x].codepoint == 0) &&
                      (cells[sd->cur.sel2.x].att.dblwidth))
                    sd->cur.sel2.x--;
               }
          }
     }
   cells = termpty_cellrow_get(sd->pty, sd->cur.sel1.y - sd->scroll, &w);
   if (cells)
     {
        // if sel2 after sel1
        if ((sd->cur.sel2.y > sd->cur.sel1.y) ||
            ((sd->cur.sel2.y == sd->cur.sel1.y) &&
                (sd->cur.sel2.x >= sd->cur.sel1.x)))
          {
             if (sd->cur.sel1.x > 0)
               {
                  if ((cells[sd->cur.sel1.x].codepoint == 0) &&
                      (cells[sd->cur.sel1.x].att.dblwidth))
                    sd->cur.sel1.x--;
               }
          }
        // else sel1 after sel 2
        else
          {
             if (sd->cur.sel1.x < (w - 1))
               {
                  if ((cells[sd->cur.sel1.x].codepoint != 0) &&
                      (cells[sd->cur.sel1.x].att.dblwidth))
                    sd->cur.sel1.x++;
               }
          }
     }
   termpty_cellcomp_thaw(sd->pty);
}
#endif

static void
_selection_newline_extend_fix(Evas_Object *obj)
{
   Termio *sd;
   
   sd = evas_object_smart_data_get(obj);
   if ((!sd->top_left) && (sd->cur.sel2.y >= sd->cur.sel1.y))
     {
        if (((sd->cur.sel1.y == sd->cur.sel2.y) && 
             (sd->cur.sel1.x <= sd->cur.sel2.x)) ||
            (sd->cur.sel1.y < sd->cur.sel2.y))
          {
             char *lastline;
             int x1, y1, x2, y2;
             size_t len;

             if (sd->cur.sel1.y == sd->cur.sel2.y) x1 = sd->cur.sel1.x;
             else x1 = 0;
             x2 = sd->cur.sel2.x;
             y1 = y2 = sd->cur.sel2.y;
             lastline = termio_selection_get(obj, x1, y1, x2, y2, &len);
             if (lastline)
               {
                  if ((len > 0) && (lastline[len - 1] == '\n'))
                    {
                       sd->cur.sel2.x = sd->grid.w - 1;
#if defined(SUPPORT_DBLWIDTH)
                       _selection_dbl_fix(obj);
#endif
                    }
                  free(lastline);
               }
          }
     }
}

static void
_smart_cb_mouse_move_job(void *data)
{
   Termio *sd;
   
   sd = evas_object_smart_data_get(data);
   if (!sd) return;
   sd->mouse_move_job = NULL;
   if (sd->mouseover_delay) ecore_timer_del(sd->mouseover_delay);
   sd->mouseover_delay = ecore_timer_add(0.05, _smart_mouseover_delay, data);
}

static void
_edje_cb_bottom_right_in(void *data, Evas_Object *obj __UNUSED__,
                         const char *emission __UNUSED__, const char *source __UNUSED__)
{
   Termio *sd = data;

   sd->bottom_right = EINA_TRUE;
}

static void
_edje_cb_top_left_in(void *data, Evas_Object *obj __UNUSED__,
                     const char *emission __UNUSED__, const char *source __UNUSED__)
{
   Termio *sd = data;

   sd->top_left = EINA_TRUE;
}

static void
_edje_cb_bottom_right_out(void *data, Evas_Object *obj __UNUSED__,
                          const char *emission __UNUSED__, const char *source __UNUSED__)
{
   Termio *sd = data;

   sd->bottom_right = EINA_FALSE;
}

static void
_edje_cb_top_left_out(void *data, Evas_Object *obj __UNUSED__,
                      const char *emission __UNUSED__, const char *source __UNUSED__)
{
   Termio *sd = data;

   sd->top_left = EINA_FALSE;
}

static void
_smart_cb_mouse_down(void *data, Evas *e __UNUSED__, Evas_Object *obj __UNUSED__, void *event)
{
   Evas_Event_Mouse_Down *ev = event;
   Termio *sd;
   int cx, cy;

   sd = evas_object_smart_data_get(data);
   if (!sd) return;
   _smart_xy_to_cursor(data, ev->canvas.x, ev->canvas.y, &cx, &cy);
   sd->didclick = EINA_FALSE;
   if ((ev->button == 3) && evas_key_modifier_is_set(ev->modifiers, "Control"))
     {
        evas_object_smart_callback_call(data, "options", NULL);
        return;
     }
   if ((ev->button == 3) && evas_key_modifier_is_set(ev->modifiers, "Shift"))
     {
        termio_debugwhite_set(data, !sd->debugwhite);
        printf("debugwhite %i\n",  sd->debugwhite);
        return;
     }
   if (_rep_mouse_down(sd, ev, cx, cy)) return;
   if (ev->button == 1)
     {
        sd->boxsel = EINA_FALSE;
        if (ev->flags & EVAS_BUTTON_TRIPLE_CLICK)
          {
             _sel_line(data, cx, cy - sd->scroll);
             if (sd->cur.sel) _take_selection(data, ELM_SEL_TYPE_PRIMARY);
             sd->didclick = EINA_TRUE;
          }
        else if (ev->flags & EVAS_BUTTON_DOUBLE_CLICK)
          {
             if (evas_key_modifier_is_set(ev->modifiers, "Shift") && sd->backup.sel)
               {
                  sd->cur.sel = 1;
                  sd->cur.sel1.x = sd->backup.sel1.x;
                  sd->cur.sel1.y = sd->backup.sel1.y;
                  sd->cur.sel2.x = sd->backup.sel2.x;
                  sd->cur.sel2.y = sd->backup.sel2.y;
#if defined(SUPPORT_DBLWIDTH)
                  _selection_dbl_fix(data);
#endif
                  _sel_word_to(data, cx, cy - sd->scroll);
               }
             else
               {
                  _sel_word(data, cx, cy - sd->scroll);
               }
             if (sd->cur.sel) _take_selection(data, ELM_SEL_TYPE_PRIMARY);
             sd->didclick = EINA_TRUE;
          }
        else
          {
            if (evas_key_modifier_is_set(ev->modifiers, "Shift") ||
                evas_key_modifier_is_set(ev->modifiers, "Control") ||
                evas_key_modifier_is_set(ev->modifiers, "Alt"))
              {
                 sd->cur.sel1.x = cx;
                 sd->cur.sel1.y = cy - sd->scroll;
                 sd->cur.sel2.x = cx;
                 sd->cur.sel2.y = cy - sd->scroll;
                 sd->cur.sel = EINA_TRUE;
                 sd->cur.makesel = EINA_TRUE;
                 sd->boxsel = EINA_TRUE;
#if defined(SUPPORT_DBLWIDTH)
                 _selection_dbl_fix(data);
#endif
              }
             if (sd->top_left || sd->bottom_right)
               {
                  sd->cur.makesel = 1;
                  sd->cur.sel = 1;
                  if (sd->top_left)
                    {
                       sd->cur.sel1.x = cx;
                       sd->cur.sel1.y = cy - sd->scroll;
                    }
                  else
                    {
                       sd->cur.sel2.x = cx;
                       sd->cur.sel2.y = cy - sd->scroll;
                    }
#if defined(SUPPORT_DBLWIDTH)
                  _selection_dbl_fix(data);
#endif
               }
             else
               {
                  sd->backup.sel = sd->cur.sel;
                  sd->backup.sel1.x = sd->cur.sel1.x;
                  sd->backup.sel1.y = sd->cur.sel1.y;
                  sd->backup.sel2.x = sd->cur.sel2.x;
                  sd->backup.sel2.y = sd->cur.sel2.y;
                  if (sd->cur.sel)
                    {
                       sd->cur.sel = 0;
                       sd->didclick = EINA_TRUE;
                    }
                  sd->cur.makesel = 1;
                  sd->cur.sel1.x = cx;
                  sd->cur.sel1.y = cy - sd->scroll;
                  sd->cur.sel2.x = cx;
                  sd->cur.sel2.y = cy - sd->scroll;
#if defined(SUPPORT_DBLWIDTH)
                  _selection_dbl_fix(data);
#endif
               }
          }
        _smart_update_queue(data, sd);
     }
   else if (ev->button == 2)
     {
        _paste_selection(data, ELM_SEL_TYPE_PRIMARY);
     }
   else if (ev->button == 3)
     {
        elm_object_focus_set(data, EINA_TRUE);
        evas_object_smart_callback_call(data, "options", NULL);
     }
}

static void
_smart_cb_mouse_up(void *data, Evas *e __UNUSED__, Evas_Object *obj __UNUSED__, void *event)
{
   Evas_Event_Mouse_Up *ev = event;
   Termio *sd;
   int cx, cy;

   sd = evas_object_smart_data_get(data);
   if (!sd) return;
   _smart_xy_to_cursor(data, ev->canvas.x, ev->canvas.y, &cx, &cy);
   if (_rep_mouse_up(sd, ev, cx, cy)) return;
   if (sd->link.down.dnd) return;
   if (sd->cur.makesel)
     {
        sd->cur.makesel = 0;
        if (sd->cur.sel)
          {
             sd->didclick = EINA_TRUE;
             if (sd->top_left)
               {
                  sd->cur.sel1.x = cx;
                  sd->cur.sel1.y = cy - sd->scroll;
               }
             else
               {
                  sd->cur.sel2.x = cx;
                  sd->cur.sel2.y = cy - sd->scroll;
               }
#if defined(SUPPORT_DBLWIDTH)
             _selection_dbl_fix(data);
#endif
             if (sd->boxsel)
              {
                 sd->cur.sel2.x = cx;
                 sd->cur.sel2.y = cy - sd->scroll;
                 _smart_update_queue(data, sd);
                 _take_selection(data, ELM_SEL_TYPE_PRIMARY);
              }
            else
              {
                 _selection_newline_extend_fix(data);
                 _smart_update_queue(data, sd);
                 _take_selection(data, ELM_SEL_TYPE_PRIMARY);
              }
          }
     }
}

static void
_smart_cb_mouse_move(void *data, Evas *e __UNUSED__, Evas_Object *obj __UNUSED__, void *event)
{
   Evas_Event_Mouse_Move *ev = event;
   Termio *sd;
   int cx, cy;
   Eina_Bool mc_change = EINA_FALSE;

   sd = evas_object_smart_data_get(data);
   if (!sd) return;
   _smart_xy_to_cursor(data, ev->cur.canvas.x, ev->cur.canvas.y, &cx, &cy);
   if ((sd->mouse.cx != cx) || (sd->mouse.cy != cy)) mc_change = EINA_TRUE;
   sd->mouse.cx = cx;
   sd->mouse.cy = cy;
   if (_rep_mouse_move(sd, ev, cx, cy, mc_change)) return;
   if (sd->link.down.dnd)
     {
        sd->cur.makesel = 0;
        sd->cur.sel = 0;
        _smart_update_queue(data, sd);
        return;
     }
   if (sd->cur.makesel)
     {
        if (!sd->cur.sel)
          {
             if ((cx != sd->cur.sel1.x) ||
                 ((cy - sd->scroll) != sd->cur.sel1.y))
               sd->cur.sel = 1;
          }
        if (sd->top_left)
          {
             sd->cur.sel1.x = cx;
             sd->cur.sel1.y = cy - sd->scroll;
          }
        else
          {
             sd->cur.sel2.x = cx;
             sd->cur.sel2.y = cy - sd->scroll;
          }
#if defined(SUPPORT_DBLWIDTH)
        _selection_dbl_fix(data);
#endif
        if (!sd->boxsel)
          _selection_newline_extend_fix(data);
        _smart_update_queue(data, sd);
     }
   if (mc_change)
     {
        if (sd->mouse_move_job) ecore_job_del(sd->mouse_move_job);
        sd->mouse_move_job = ecore_job_add(_smart_cb_mouse_move_job, data);
     }
}

static void
_smart_cb_mouse_in(void *data, Evas *e __UNUSED__, Evas_Object *obj __UNUSED__, void *event __UNUSED__)
{
   int cx, cy;
   Evas_Event_Mouse_In *ev = event;
   Termio *sd = evas_object_smart_data_get(data);

   if (!sd) return;
   _smart_xy_to_cursor(data, ev->canvas.x, ev->canvas.y, &cx, &cy);
   sd->mouse.cx = cx;
   sd->mouse.cy = cy;
   termio_mouseover_suspend_pushpop(data, -1);
}

static void
_smart_cb_mouse_out(void *data, Evas *e __UNUSED__, Evas_Object *obj __UNUSED__, void *event __UNUSED__)
{
   Evas_Object *o;
   Termio *sd;

   sd = evas_object_smart_data_get(data);
   if (!sd) return;
   termio_mouseover_suspend_pushpop(data, 1);
   ty_dbus_link_hide();
}

static void
_smart_cb_mouse_wheel(void *data, Evas *e __UNUSED__, Evas_Object *obj __UNUSED__, void *event)
{
   Evas_Event_Mouse_Wheel *ev = event;
   Termio *sd;

   sd = evas_object_smart_data_get(data);
   if (!sd) return;
   if (evas_key_modifier_is_set(ev->modifiers, "Control")) return;
   if (evas_key_modifier_is_set(ev->modifiers, "Alt")) return;
   if (evas_key_modifier_is_set(ev->modifiers, "Shift")) return;

   if (sd->pty->mouse_mode == MOUSE_OFF)
     {

        sd->scroll -= (ev->z * 4);
        if (sd->scroll > sd->pty->backscroll_num)
          sd->scroll = sd->pty->backscroll_num;
        else if (sd->scroll < 0) sd->scroll = 0;
        _smart_update_queue(data, sd);
     }
   else
     {
       char buf[64];
       int cx, cy;

       _smart_xy_to_cursor(data, ev->canvas.x, ev->canvas.y, &cx, &cy);

       switch (sd->pty->mouse_ext)
         {
          case MOUSE_EXT_NONE:
            if ((cx < (0xff - ' ')) && (cy < (0xff - ' ')))
              {
                 int btn = (ev->z >= 0) ? 1 + 64 : 2 + 64;

                 buf[0] = 0x1b;
                 buf[1] = '[';
                 buf[2] = 'M';
                 buf[3] = btn + ' ';
                 buf[4] = cx + 1 + ' ';
                 buf[5] = cy + 1 + ' ';
                 buf[6] = 0;
                 termpty_write(sd->pty, buf, strlen(buf));
              }
            break;
          case MOUSE_EXT_UTF8: // ESC.[.M.BTN/FLGS.XUTF8.YUTF8
              {
                 int v, i;
                 int btn = (ev->z >= 0) ? 'a' : '`';

                 buf[0] = 0x1b;
                 buf[1] = '[';
                 buf[2] = 'M';
                 buf[3] = btn;
                 i = 4;
                 v = cx + 1 + ' ';
                 if (v <= 127) buf[i++] = v;
                 else
                   { // 14 bits for cx/cy - enough i think
                       buf[i++] = 0xc0 + (v >> 6);
                       buf[i++] = 0x80 + (v & 0x3f);
                   }
                 v = cy + 1 + ' ';
                 if (v <= 127) buf[i++] = v;
                 else
                   { // 14 bits for cx/cy - enough i think
                       buf[i++] = 0xc0 + (v >> 6);
                       buf[i++] = 0x80 + (v & 0x3f);
                   }
                 buf[i] = 0;
                 termpty_write(sd->pty, buf, strlen(buf));
              }
            break;
          case MOUSE_EXT_SGR: // ESC.[.<.NUM.;.NUM.;.NUM.M
              {
                 int btn = (ev->z >= 0) ? 1 + 64 : 2 + 64;
                 snprintf(buf, sizeof(buf), "%c[<%i;%i;%iM", 0x1b,
                          btn, cx + 1, cy + 1);
                 termpty_write(sd->pty, buf, strlen(buf));
              }
            break;
          case MOUSE_EXT_URXVT: // ESC.[.NUM.;.NUM.;.NUM.M
              {
                 int btn = (ev->z >= 0) ? 1 + 64 : 2 + 64;
                 snprintf(buf, sizeof(buf), "%c[%i;%i;%iM", 0x1b,
                          btn + ' ',
                          cx + 1, cy + 1);
                 termpty_write(sd->pty, buf, strlen(buf));
              }
            break;
          default:
            break;
         }
        if (sd->scrolio)
          scrolio_miniview_update_scroll(sd->scrolio, termio_scroll_get(obj));
     }
}

static void
_win_obj_del(void *data, Evas *e __UNUSED__, Evas_Object *obj, void *event __UNUSED__)
{
   Termio *sd;

   sd = evas_object_smart_data_get(data);
   if (!sd) return;
   if (obj == sd->win)
     {
        evas_object_event_callback_del_full(sd->win, EVAS_CALLBACK_DEL,
                                            _win_obj_del, data);
        sd->win = NULL;
     }
}

static void
_termio_config_set(Evas_Object *obj, Config *config)
{
   Termio *sd = evas_object_smart_data_get(obj);
   Evas_Coord w = 2, h = 2;

   sd->config = config;

   sd->jump_on_change = config->jump_on_change;
   sd->jump_on_keypress = config->jump_on_keypress;

   if (config->font.bitmap)
     {
        char buf[PATH_MAX];
        snprintf(buf, sizeof(buf), "%s/fonts/%s",
                 elm_app_data_dir_get(), config->font.name);
        sd->font.name = eina_stringshare_add(buf);
     }
   else
     sd->font.name = eina_stringshare_add(config->font.name);
   sd->font.size = config->font.size;

   evas_object_scale_set(sd->grid.obj, elm_config_scale_get());
   evas_object_textgrid_font_set(sd->grid.obj, sd->font.name, sd->font.size);
   evas_object_textgrid_size_set(sd->grid.obj, 1, 1);
   evas_object_textgrid_cell_size_get(sd->grid.obj, &w, &h);

   if (w < 1) w = 1;
   if (h < 1) h = 1;
   sd->font.chw = w;
   sd->font.chh = h;

   theme_apply(sd->cur.obj, config, "terminology/cursor");
   theme_auto_reload_enable(sd->cur.obj);
   evas_object_resize(sd->cur.obj, sd->font.chw, sd->font.chh);
   evas_object_show(sd->cur.obj);

   theme_apply(sd->cur.selo_theme, config, "terminology/selection");
   theme_auto_reload_enable(sd->cur.selo_theme);
   edje_object_part_swallow(sd->cur.selo_theme, "terminology.top_left", sd->cur.selo_top);
   edje_object_part_swallow(sd->cur.selo_theme, "terminology.bottom_right", sd->cur.selo_bottom);
}

static void
_cursor_cb_move(void *data, Evas *e __UNUSED__, Evas_Object *obj __UNUSED__, void *event __UNUSED__)
{
   Termio *sd;

   sd = evas_object_smart_data_get(data);
   if (!sd) return;
   _imf_cursor_set(sd);
}

static Evas_Event_Flags
_smart_cb_gest_long_move(void *data, void *event __UNUSED__)
{
//   Elm_Gesture_Taps_Info *p = event;
   Termio *sd = evas_object_smart_data_get(data);
   
   if (!sd) return EVAS_EVENT_FLAG_ON_HOLD;
   evas_object_smart_callback_call(data, "options", NULL);
   sd->didclick = EINA_TRUE;
   return EVAS_EVENT_FLAG_ON_HOLD;
}

static Evas_Event_Flags
_smart_cb_gest_zoom_start(void *data, void *event)
{
   Elm_Gesture_Zoom_Info *p = event;
   Termio *sd = evas_object_smart_data_get(data);
   Config *config = termio_config_get(data);
   
   if (!sd) return EVAS_EVENT_FLAG_ON_HOLD;
   if (config)
     {
        int sz;
        
        sd->zoom_fontsize_start = config->font.size;
        sz = (double)sd->zoom_fontsize_start * p->zoom;
        if (sz != config->font.size) _font_size_set(data, sz);
     }
   sd->didclick = EINA_TRUE;
   return EVAS_EVENT_FLAG_ON_HOLD;
}

static Evas_Event_Flags
_smart_cb_gest_zoom_move(void *data, void *event)
{
   Elm_Gesture_Zoom_Info *p = event;
   Termio *sd = evas_object_smart_data_get(data);
   Config *config = termio_config_get(data);
   
   if (!sd) return EVAS_EVENT_FLAG_ON_HOLD;
   if (config)
     {
        int sz = (double)sd->zoom_fontsize_start *
          (1.0 + ((p->zoom - 1.0) / 30.0));
        if (sz != config->font.size) _font_size_set(data, sz);
     }
   sd->didclick = EINA_TRUE;
   return EVAS_EVENT_FLAG_ON_HOLD;
}

static Evas_Event_Flags
_smart_cb_gest_zoom_end(void *data, void *event)
{
   Elm_Gesture_Zoom_Info *p = event;
   Termio *sd = evas_object_smart_data_get(data);
   Config *config = termio_config_get(data);
   
   if (!sd) return EVAS_EVENT_FLAG_ON_HOLD;
   if (config)
     {
        int sz = (double)sd->zoom_fontsize_start *
          (1.0 + ((p->zoom - 1.0) / 30.0));
        if (sz != config->font.size) _font_size_set(data, sz);
     }
   sd->didclick = EINA_TRUE;
   return EVAS_EVENT_FLAG_ON_HOLD;
}

static Evas_Event_Flags
_smart_cb_gest_zoom_abort(void *data, void *event __UNUSED__)
{
//   Elm_Gesture_Zoom_Info *p = event;
   Termio *sd = evas_object_smart_data_get(data);
   Config *config = termio_config_get(data);
   
   if (!sd) return EVAS_EVENT_FLAG_ON_HOLD;
   if (config)
     {
        if (sd->zoom_fontsize_start != config->font.size)
          _font_size_set(data, sd->zoom_fontsize_start);
     }
   sd->didclick = EINA_TRUE;
   return EVAS_EVENT_FLAG_ON_HOLD;
}

static void
_imf_event_commit_cb(void *data, Ecore_IMF_Context *ctx __UNUSED__, void *event)
{
   Termio *sd = data;
   char *str = event;
   DBG("IMF committed '%s'", str);
   if (!str) return;
   termpty_write(sd->pty, str, strlen(str));
}

static void
_smart_add(Evas_Object *obj)
{
   Termio *sd;
   Evas_Object *o;

   sd = calloc(1, sizeof(Termio));
   EINA_SAFETY_ON_NULL_RETURN(sd);
   evas_object_smart_data_set(obj, sd);

   _parent_sc.add(obj);

   /* Terminal output widget */
   o = evas_object_textgrid_add(evas_object_evas_get(obj));
   evas_object_pass_events_set(o, EINA_TRUE);
   evas_object_propagate_events_set(o, EINA_FALSE);
   evas_object_smart_member_add(o, obj);
   evas_object_show(o);
   sd->grid.obj = o;

   /* Setup cursor */
   o = edje_object_add(evas_object_evas_get(obj));
   evas_object_pass_events_set(o, EINA_TRUE);
   evas_object_propagate_events_set(o, EINA_FALSE);
   evas_object_smart_member_add(o, obj);
   sd->cur.obj = o;

   evas_object_event_callback_add(o, EVAS_CALLBACK_MOVE, _cursor_cb_move, obj);

   /* Setup the selection widget */
   o = evas_object_rectangle_add(evas_object_evas_get(obj));
   evas_object_pass_events_set(o, EINA_TRUE);
   evas_object_propagate_events_set(o, EINA_FALSE);
   sd->cur.selo_top = o;
   o = evas_object_rectangle_add(evas_object_evas_get(obj));
   evas_object_pass_events_set(o, EINA_TRUE);
   evas_object_propagate_events_set(o, EINA_FALSE);
   sd->cur.selo_bottom = o;
   o = edje_object_add(evas_object_evas_get(obj));
   evas_object_smart_member_add(o, obj);
   sd->cur.selo_theme = o;
   edje_object_signal_callback_add(o, "mouse,in", "zone.bottom_right", _edje_cb_bottom_right_in, sd);
   edje_object_signal_callback_add(o, "mouse,in", "zone.top_left", _edje_cb_top_left_in, sd);
   edje_object_signal_callback_add(o, "mouse,out", "zone.bottom_right", _edje_cb_bottom_right_out, sd);
   edje_object_signal_callback_add(o, "mouse,out", "zone.top_left", _edje_cb_top_left_out, sd);

   /* Setup event catcher */
   o = evas_object_rectangle_add(evas_object_evas_get(obj));
   evas_object_repeat_events_set(o, EINA_TRUE);
   evas_object_smart_member_add(o, obj);
   sd->event = o;
   evas_object_color_set(o, 0, 0, 0, 0);
   evas_object_show(o);

   evas_object_event_callback_add(o, EVAS_CALLBACK_MOUSE_DOWN,
                                  _smart_cb_mouse_down, obj);
   evas_object_event_callback_add(o, EVAS_CALLBACK_MOUSE_UP,
                                  _smart_cb_mouse_up, obj);
   evas_object_event_callback_add(o, EVAS_CALLBACK_MOUSE_MOVE,
                                  _smart_cb_mouse_move, obj);
   evas_object_event_callback_add(o, EVAS_CALLBACK_MOUSE_IN,
                                  _smart_cb_mouse_in, obj);
   evas_object_event_callback_add(o, EVAS_CALLBACK_MOUSE_OUT,
                                  _smart_cb_mouse_out, obj);
   evas_object_event_callback_add(o, EVAS_CALLBACK_MOUSE_WHEEL,
                                  _smart_cb_mouse_wheel, obj);

   evas_object_event_callback_add(obj, EVAS_CALLBACK_KEY_DOWN,
                                  _smart_cb_key_down, obj);
   evas_object_event_callback_add(obj, EVAS_CALLBACK_KEY_UP,
                                  _smart_cb_key_up, obj);
   evas_object_event_callback_add(obj, EVAS_CALLBACK_FOCUS_IN,
                                  _smart_cb_focus_in, obj);
   evas_object_event_callback_add(obj, EVAS_CALLBACK_FOCUS_OUT,
                                  _smart_cb_focus_out, obj);

   sd->link.suspend = 1;
   
   if (ecore_imf_init())
     {
        const char *imf_id = ecore_imf_context_default_id_get();
        Evas *e;

        if (!imf_id) sd->imf = NULL;
        else
          {
             const Ecore_IMF_Context_Info *imf_info;

             imf_info = ecore_imf_context_info_by_id_get(imf_id);
             if ((!imf_info->canvas_type) ||
                 (strcmp(imf_info->canvas_type, "evas") == 0))
               sd->imf = ecore_imf_context_add(imf_id);
             else
               {
                  imf_id = ecore_imf_context_default_id_by_canvas_type_get("evas");
                  if (imf_id) sd->imf = ecore_imf_context_add(imf_id);
               }
          }

        if (!sd->imf) goto imf_done;

        e = evas_object_evas_get(o);
        ecore_imf_context_client_window_set
          (sd->imf, (void *)ecore_evas_window_get(ecore_evas_ecore_evas_get(e)));
        ecore_imf_context_client_canvas_set(sd->imf, e);

        ecore_imf_context_event_callback_add
          (sd->imf, ECORE_IMF_CALLBACK_COMMIT, _imf_event_commit_cb, sd);

        /* make IMF usable by a terminal - no preedit, prediction... */
        ecore_imf_context_use_preedit_set
          (sd->imf, EINA_FALSE);
        ecore_imf_context_prediction_allow_set
          (sd->imf, EINA_FALSE);
        ecore_imf_context_autocapital_type_set
          (sd->imf, ECORE_IMF_AUTOCAPITAL_TYPE_NONE);
        ecore_imf_context_input_panel_layout_set
          (sd->imf, ECORE_IMF_INPUT_PANEL_LAYOUT_TERMINAL);
        ecore_imf_context_input_mode_set
          (sd->imf, ECORE_IMF_INPUT_MODE_FULL);
        ecore_imf_context_input_panel_language_set
          (sd->imf, ECORE_IMF_INPUT_PANEL_LANG_ALPHABET);
        ecore_imf_context_input_panel_return_key_type_set
          (sd->imf, ECORE_IMF_INPUT_PANEL_RETURN_KEY_TYPE_DEFAULT);
imf_done:
        if (sd->imf) DBG("Ecore IMF Setup");
        else WRN("Ecore IMF failed");
     }
   terms = eina_list_append(terms, obj);
}

static void
_smart_del(Evas_Object *obj)
{
   Evas_Object *o;
   Termio *sd = evas_object_smart_data_get(obj);
   char *chid;
   
   if (!sd) return;
   terms = eina_list_remove(terms, obj);
   EINA_LIST_FREE(sd->mirrors, o)
     {
        evas_object_event_callback_del_full(o, EVAS_CALLBACK_DEL,
                                            _smart_mirror_del, obj);
        evas_object_del(o);
     }
   if (sd->imf)
     {
        ecore_imf_context_event_callback_del
          (sd->imf, ECORE_IMF_CALLBACK_COMMIT, _imf_event_commit_cb);
        ecore_imf_context_del(sd->imf);
     }
   if (sd->cur.obj) evas_object_del(sd->cur.obj);
   if (sd->event) evas_object_del(sd->event);
   if (sd->cur.selo_top) evas_object_del(sd->cur.selo_top);
   if (sd->cur.selo_bottom) evas_object_del(sd->cur.selo_bottom);
   if (sd->cur.selo_theme) evas_object_del(sd->cur.selo_theme);
   if (sd->anim) ecore_animator_del(sd->anim);
   if (sd->delayed_size_timer) ecore_timer_del(sd->delayed_size_timer);
   if (sd->link_do_timer) ecore_timer_del(sd->link_do_timer);
   if (sd->mouse_move_job) ecore_job_del(sd->mouse_move_job);
   if (sd->mouseover_delay) ecore_timer_del(sd->mouseover_delay);
   if (sd->font.name) eina_stringshare_del(sd->font.name);
   if (sd->pty) termpty_free(sd->pty);
   if (sd->link.string) free(sd->link.string);
   if (sd->glayer) evas_object_del(sd->glayer);
   if (sd->win)
     evas_object_event_callback_del_full(sd->win, EVAS_CALLBACK_DEL,
                                         _win_obj_del, obj);
   EINA_LIST_FREE(sd->link.objs, o)
     {
        if (o == sd->link.down.dndobj) sd->link.down.dndobj = NULL;
        evas_object_del(o);
     }
   if (sd->link.down.dndobj) evas_object_del(sd->link.down.dndobj);
   _compose_seq_reset(sd);
   if (sd->sel_str) eina_stringshare_del(sd->sel_str);
   if (sd->sel_reset_job) ecore_job_del(sd->sel_reset_job);
   EINA_LIST_FREE(sd->cur_chids, chid) eina_stringshare_del(chid);
   sd->sel_str = NULL;
   sd->sel_reset_job = NULL;
   sd->link.down.dndobj = NULL;
   sd->cur.obj = NULL;
   sd->event = NULL;
   sd->cur.selo_top = NULL;
   sd->cur.selo_bottom = NULL;
   sd->cur.selo_theme = NULL;
   sd->anim = NULL;
   sd->delayed_size_timer = NULL;
   sd->font.name = NULL;
   sd->pty = NULL;
   sd->imf = NULL;
   sd->win = NULL;
   sd->glayer = NULL;
   ecore_imf_shutdown();

   _parent_sc.del(obj);
}

static void
_smart_resize(Evas_Object *obj, Evas_Coord w, Evas_Coord h)
{
   Termio *sd = evas_object_smart_data_get(obj);
   Evas_Coord ow, oh;
   if (!sd) return;
   evas_object_geometry_get(obj, NULL, NULL, &ow, &oh);
   if ((ow == w) && (oh == h)) return;
   evas_object_smart_changed(obj);
   if (!sd->delayed_size_timer) sd->delayed_size_timer = 
     ecore_timer_add(0.0, _smart_cb_delayed_size, obj);
   else ecore_timer_delay(sd->delayed_size_timer, 0.0);
   evas_object_resize(sd->event, ow, oh);
}

static void
_smart_calculate(Evas_Object *obj)
{
   Termio *sd = evas_object_smart_data_get(obj);
   Evas_Object *scr_obj;
   Evas_Coord ox, oy, ow, oh;

   if (!sd) return;

   evas_object_geometry_get(obj, &ox, &oy, &ow, &oh);
   evas_object_move(sd->grid.obj, ox, oy);
   evas_object_resize(sd->grid.obj,
                      sd->grid.w * sd->font.chw,
                      sd->grid.h * sd->font.chh);

   //evas_object_move(sd->scrolio.grid.obj, ox, oy);
   //evas_object_resize(sd->scrolio.grid.obj,
   //                   sd->grid.w * sd->font.chw,
   //                   sd->grid.h * sd->font.chh);

   //scr_obj = scrolio_grid_object_get(sd->scrolio);
   //evas_object_move(scr_obj, ox, oy);
   //evas_object_resize(scr_obj,
   //                   sd->grid.w * sd->font.chw,
   //                   sd->grid.h * sd->font.chh);

   evas_object_move(sd->cur.obj,
                    ox + (sd->cur.x * sd->font.chw),
                    oy + (sd->cur.y * sd->font.chh));
   evas_object_move(sd->event, ox, oy);
   evas_object_resize(sd->event, ow, oh);
}

static void
_smart_move(Evas_Object *obj, Evas_Coord x __UNUSED__, Evas_Coord y __UNUSED__)
{
   Termio *sd = evas_object_smart_data_get(obj);
   if (!sd) return;
   evas_object_smart_changed(obj);
   if (sd->scrolio)
     scrolio_miniview_move(sd->scrolio, x, y);
}

static void
_smart_init(void)
{
   static Evas_Smart_Class sc;

   evas_object_smart_clipped_smart_set(&_parent_sc);
   sc           = _parent_sc;
   sc.name      = "termio";
   sc.version   = EVAS_SMART_CLASS_VERSION;
   sc.add       = _smart_add;
   sc.del       = _smart_del;
   sc.resize    = _smart_resize;
   sc.move      = _smart_move;
   sc.calculate = _smart_calculate;
   _smart = evas_smart_class_new(&sc);
}

static void
_smart_pty_change(void *data)
{
   Evas_Object *obj = data;
   Termio *sd;
   sd = evas_object_smart_data_get(obj);
   if (!sd) return;

// if scroll to bottom on updates
   if (sd->jump_on_change)  sd->scroll = 0;
   _smart_update_queue(data, sd);
}

static void
_smart_pty_scroll(void *data)
{
   Evas_Object *obj = data;
   Termio *sd;
   int changed = 0;
   sd = evas_object_smart_data_get(obj);
   if (!sd) return;

   if ((!sd->jump_on_change) && // if NOT scroll to bottom on updates
       (sd->scroll > 0))
     {
        // adjust scroll position for added scrollback
        sd->scroll++;
        ERR("scroll: %d", sd->scroll);
        if (sd->scroll > sd->pty->backscroll_num)
          sd->scroll = sd->pty->backscroll_num;
        changed = 1;
     }
   if (sd->cur.sel)
     {
        sd->cur.sel1.y--;
        sd->cur.sel2.y--;
        changed = 1;
     }
   if (changed) _smart_update_queue(data, sd);
}

static void
_smart_pty_title(void *data)
{
   Evas_Object *obj = data;
   Termio *sd;
   sd = evas_object_smart_data_get(obj);
   if (!sd) return;
   if (!sd->win) return;
   evas_object_smart_callback_call(obj, "title,change", NULL);
//   elm_win_title_set(sd->win, sd->pty->prop.title);
}

static void
_smart_pty_icon(void *data)
{
   Evas_Object *obj = data;
   Termio *sd;
   sd = evas_object_smart_data_get(obj);
   if (!sd) return;
   if (!sd->win) return;
   evas_object_smart_callback_call(obj, "icon,change", NULL);
//   elm_win_icon_name_set(sd->win, sd->pty->prop.icon);
}

static void
_smart_pty_cancel_sel(void *data)
{
   Evas_Object *obj = data;
   Termio *sd;
   sd = evas_object_smart_data_get(obj);
   if (!sd) return;
   if (sd->cur.sel)
     {
        sd->cur.sel = 0;
        sd->cur.makesel = 0;
        _smart_update_queue(data, sd);
     }
}

static void
_smart_pty_exited(void *data)
{
   evas_object_smart_callback_call(data, "exited", NULL);
}

static void
_smart_pty_bell(void *data)
{
   Termio *sd = evas_object_smart_data_get(data);
   if (!sd) return;
   evas_object_smart_callback_call(data, "bell", NULL);
   edje_object_signal_emit(sd->cur.obj, "bell", "terminology");
}

static void
_smart_pty_command(void *data)
{
   Evas_Object *obj = data;
   Termio *sd = evas_object_smart_data_get(obj);
   if (!sd) return;
   if (!sd->pty->cur_cmd) return;
   if (sd->pty->cur_cmd[0] == 'i')
     {
        if ((sd->pty->cur_cmd[1] == 's') ||
            (sd->pty->cur_cmd[1] == 'c') ||
            (sd->pty->cur_cmd[1] == 'f') ||
            (sd->pty->cur_cmd[1] == 't') ||
            (sd->pty->cur_cmd[1] == 'j'))
          {
             const char *p, *p0, *p1, *path = NULL;
             char *pp;
             int ww = 0, hh = 0, repch;
             Eina_List *strs = NULL;
             
             // exact size in CHAR CELLS - WW (decimal) width CELLS,
             // HH (decimal) in CELLS.
             // 
             // isCWW;HH;PATH
             //  OR
             // isCWW;HH;LINK\nPATH
             //  OR specific to 'j' (edje)
             // ijCWW;HH;PATH\nGROUP[commands]
             //  WHERE [commands] is an optional string set of:
             // \nCMD\nP1[\nP2][\nP3][[\nCMD2\nP21[\nP22]]...
             //  CMD is the command, P1, P2, P3 etc. are parameters (P2 and
             //  on are optional depending on CMD)
             repch = sd->pty->cur_cmd[2];
             if (repch)
               {
                  char *link = NULL;
                  
                  for (p0 = p = &(sd->pty->cur_cmd[3]); *p; p++)
                    {
                       if (*p == ';')
                         {
                            ww = strtol(p0, NULL, 10);
                            p++;
                            break;
                         }
                    }
                  for (p0 = p; *p; p++)
                    {
                       if (*p == ';')
                         {
                            hh = strtol(p0, NULL, 10);
                            p++;
                            break;
                         }
                    }
                  if (sd->pty->cur_cmd[1] == 'j')
                    {
                       // parse from p until end of string - one newline
                       // per list item in strs
                       p0 = p1 = p;
                       for (;;)
                         {
                            // end of str param
                            if ((*p1 == '\n') || (*p1 == '\r') || (!*p1))
                              {
                                 // if string is non-empty...
                                 if ((p1 - p0) >= 1)
                                   {
                                      // allocate, fill and add to list
                                      pp = malloc(p1 - p0 + 1);
                                      if (pp)
                                        {
                                           strncpy(pp, p0, p1 - p0);
                                           pp[p1 - p0] = 0;
                                           strs = eina_list_append(strs, pp);
                                        }
                                   }
                                 // end of string buffer
                                 if (!*p1) break;
                                 p1++; // skip \n or \r
                                 p0 = p1;
                              }
                            else
                              p1++;
                         }
                    }
                  else
                    {
                       path = p;
                       p = strchr(path, '\n');
                       if (p)
                         {
                            link = strdup(path);
                            path = p + 1;
                            if (isspace(path[0])) path++;
                            pp = strchr(link, '\n');
                            if (pp) *pp = 0;
                            pp = strchr(link, '\r');
                            if (pp) *pp = 0;
                         }
                    }
                  if ((ww < 512) && (hh < 512))
                    {
                       Termblock *blk = NULL;

                       if (strs)
                         {
                            const char *file, *group;
                            Eina_List *l;
                            
                            file = eina_list_nth(strs, 0);
                            group = eina_list_nth(strs, 1);
                            l = eina_list_nth_list(strs, 2);
                            blk = termpty_block_new(sd->pty, ww, hh, file, group);
                            for (;l; l = l->next)
                              {
                                 pp = l->data;
                                 if (pp)
                                   blk->cmds = eina_list_append(blk->cmds, pp);
                                 l->data = NULL;
                              }
                         }
                       else
                         blk = termpty_block_new(sd->pty, ww, hh, path, link);
                       if (blk)
                         {
                            if (sd->pty->cur_cmd[1] == 's')
                              blk->scale_stretch = EINA_TRUE;
                            else if (sd->pty->cur_cmd[1] == 'c')
                              blk->scale_center = EINA_TRUE;
                            else if (sd->pty->cur_cmd[1] == 'f')
                              blk->scale_fill = EINA_TRUE;
                            else if (sd->pty->cur_cmd[1] == 't')
                              blk->thumb = EINA_TRUE;
                            else if (sd->pty->cur_cmd[1] == 'j')
                              blk->edje = EINA_TRUE;
                            termpty_block_insert(sd->pty, repch, blk);
                         }
                    }
                  if (link) free(link);
                  EINA_LIST_FREE(strs, pp) free(pp);
               }
             return;
          }
        else if (sd->pty->cur_cmd[1] == 'C')
          {
             Termblock *blk = NULL;
             const char *p, *p0, *p1;
             char *pp;
             Eina_List *strs = NULL;
             
             p = &(sd->pty->cur_cmd[2]);
             // parse from p until end of string - one newline
             // per list item in strs
             p0 = p1 = p;
             for (;;)
               {
                  // end of str param
                  if ((*p1 == '\n') || (*p1 == '\r') || (!*p1))
                    {
                       // if string is non-empty...
                       if ((p1 - p0) >= 1)
                         {
                            // allocate, fill and add to list
                            pp = malloc(p1 - p0 + 1);
                            if (pp)
                              {
                                 strncpy(pp, p0, p1 - p0);
                                 pp[p1 - p0] = 0;
                                 strs = eina_list_append(strs, pp);
                              }
                         }
                       // end of string buffer
                       if (!*p1) break;
                       p1++; // skip \n or \r
                       p0 = p1;
                    }
                  else
                    p1++;
               }
             if (strs)
               {
                  char *chid = strs->data;
                  blk = termpty_block_chid_get(sd->pty, chid);
                  if (blk)
                    {
                       _block_edje_cmds(sd->pty, blk, strs->next, EINA_FALSE);
                    }
               }
             EINA_LIST_FREE(strs, pp) free(pp);
          }
        else if (sd->pty->cur_cmd[1] == 'b')
          {
             sd->pty->block.on = EINA_TRUE;
          }
        else if (sd->pty->cur_cmd[1] == 'e')
          {
             sd->pty->block.on = EINA_FALSE;
          }
     }
   else if (sd->pty->cur_cmd[0] == 'q')
     {
        if (sd->pty->cur_cmd[1] == 's')
          {
             char buf[256];
             
             snprintf(buf, sizeof(buf), "%i;%i;%i;%i\n",
                      sd->grid.w, sd->grid.h, sd->font.chw, sd->font.chh);
             termpty_write(sd->pty, buf, strlen(buf));
             return;
          }
        else if (sd->pty->cur_cmd[1] == 'j')
          {
             const char *chid = &(sd->pty->cur_cmd[3]);
                  
             if (sd->pty->cur_cmd[2])
               {
                  if (sd->pty->cur_cmd[2] == '+')
                    {
                       sd->cur_chids = eina_list_append
                         (sd->cur_chids, eina_stringshare_add(chid));
                    }
                  else if (sd->pty->cur_cmd[2] == '-')
                    {
                       Eina_List *l;
                       char *chid2;
                       
                       EINA_LIST_FOREACH(sd->cur_chids, l, chid2)
                         {
                            if (!(!strcmp(chid, chid2)))
                              {
                                 sd->cur_chids =
                                   eina_list_remove_list(sd->cur_chids, l);
                                 eina_stringshare_del(chid2);
                                 break;
                              }
                         }
                    }
               }
             else
               {
                  EINA_LIST_FREE(sd->cur_chids, chid)
                    eina_stringshare_del(chid);
               }
             return;
          }
     }
   evas_object_smart_callback_call(obj, "command", (void *)sd->pty->cur_cmd);
}

#if !((ELM_VERSION_MAJOR == 1) && (ELM_VERSION_MINOR < 8))
static void
_smart_cb_drag_enter(void *data __UNUSED__, Evas_Object *o __UNUSED__)
{
   printf("dnd enter\n");
}

static void
_smart_cb_drag_leave(void *data __UNUSED__, Evas_Object *o __UNUSED__)
{
   printf("dnd leave\n");
}

static void
_smart_cb_drag_pos(void *data __UNUSED__, Evas_Object *o __UNUSED__, Evas_Coord x, Evas_Coord y, Elm_Xdnd_Action action)
{
   printf("dnd at %i %i act:%i\n", x, y, action);
}

static Eina_Bool
_smart_cb_drop(void *data, Evas_Object *o __UNUSED__, Elm_Selection_Data *ev)
{
   Evas_Object *obj = data;
   Termio *sd = evas_object_smart_data_get(obj);
   if (!sd) return EINA_TRUE;
   if (ev->action == ELM_XDND_ACTION_COPY)
     {
        if (strchr(ev->data, '\n'))
          {
             char *p, *p2, *p3, *tb;
             
             tb = malloc(strlen(ev->data) + 1);
             if (tb)
               {
                  for (p = ev->data; p;)
                    {
                       p2 = strchr(p, '\n');
                       p3 = strchr(p, '\r');
                       if (p2 && p3)
                         {
                            if (p3 < p2) p2 = p3;
                         }
                       else if (!p2) p3 = p2;
                       if (p2)
                         {
                            strncpy(tb, p, p2 - p);
                            tb[p2 - p] = 0;
                            p = p2;
                            while ((*p) && (isspace(*p))) p++;
                            if (strlen(tb) > 0)
                              evas_object_smart_callback_call
                              (obj, "popup,queue", tb);
                         }
                       else
                         {
                            strcpy(tb, p);
                            if (strlen(tb) > 0)
                              evas_object_smart_callback_call
                              (obj, "popup,queue", tb);
                            break;
                         }
                    }
                  free(tb);
               }
          }
        else
          evas_object_smart_callback_call(obj, "popup", ev->data);
     }
   else
     termpty_write(sd->pty, ev->data, strlen(ev->data));
   return EINA_TRUE;
}
#endif

Evas_Object *
termio_add(Evas_Object *parent, Config *config, const char *cmd, Eina_Bool login_shell, const char *cd, int w, int h)
{
   Evas *e;
   Evas_Object *obj, *g;
   Termio *sd;

   EINA_SAFETY_ON_NULL_RETURN_VAL(parent, NULL);
   e = evas_object_evas_get(parent);
   if (!e) return NULL;

   if (!_smart) _smart_init();
   obj = evas_object_smart_add(e, _smart);
   sd = evas_object_smart_data_get(obj);
   if (!sd) return obj;

   _termio_config_set(obj, config);

   sd->glayer = g = elm_gesture_layer_add(parent);
   elm_gesture_layer_attach(g, sd->event);

   elm_gesture_layer_cb_set(g, ELM_GESTURE_N_LONG_TAPS,
                            ELM_GESTURE_STATE_MOVE, _smart_cb_gest_long_move,
                            obj);
   
   elm_gesture_layer_cb_set(g, ELM_GESTURE_ZOOM,
                            ELM_GESTURE_STATE_START, _smart_cb_gest_zoom_start,
                            obj);
   elm_gesture_layer_cb_set(g, ELM_GESTURE_ZOOM,
                            ELM_GESTURE_STATE_MOVE, _smart_cb_gest_zoom_move,
                            obj);
   elm_gesture_layer_cb_set(g, ELM_GESTURE_ZOOM,
                            ELM_GESTURE_STATE_END, _smart_cb_gest_zoom_end,
                            obj);
   elm_gesture_layer_cb_set(g, ELM_GESTURE_ZOOM,
                            ELM_GESTURE_STATE_ABORT, _smart_cb_gest_zoom_abort,
                            obj);
   
#if !((ELM_VERSION_MAJOR == 1) && (ELM_VERSION_MINOR < 8))
   elm_drop_target_add(sd->event,
                       ELM_SEL_FORMAT_TEXT | ELM_SEL_FORMAT_IMAGE,
                       _smart_cb_drag_enter, obj,
                       _smart_cb_drag_leave, obj,
                       _smart_cb_drag_pos, obj,
                       _smart_cb_drop, obj);
#endif
   
   sd->pty = termpty_new(cmd, login_shell, cd, w, h, config->scrollback);
   sd->pty->obj = obj;
   sd->pty->cb.change.func = _smart_pty_change;
   sd->pty->cb.change.data = obj;
   sd->pty->cb.scroll.func = _smart_pty_scroll;
   sd->pty->cb.scroll.data = obj;
   sd->pty->cb.set_title.func = _smart_pty_title;
   sd->pty->cb.set_title.data = obj;
   sd->pty->cb.set_icon.func = _smart_pty_icon;
   sd->pty->cb.set_icon.data = obj;
   sd->pty->cb.cancel_sel.func = _smart_pty_cancel_sel;
   sd->pty->cb.cancel_sel.data = obj;
   sd->pty->cb.exited.func = _smart_pty_exited;
   sd->pty->cb.exited.data = obj;
   sd->pty->cb.bell.func = _smart_pty_bell;
   sd->pty->cb.bell.data = obj;
   sd->pty->cb.command.func = _smart_pty_command;
   sd->pty->cb.command.data = obj;
   _smart_size(obj, w, h, EINA_FALSE);
   return obj;
}

void
termio_win_set(Evas_Object *obj, Evas_Object *win)
{
   Termio *sd = evas_object_smart_data_get(obj);
   if (!sd) return;
   if (sd->win)
     {
        evas_object_event_callback_del_full(sd->win, EVAS_CALLBACK_DEL,
                                            _win_obj_del, obj);
        sd->win = NULL;
     }
   if (win)
     {
        sd->win = win;
        evas_object_event_callback_add(sd->win, EVAS_CALLBACK_DEL,
                                       _win_obj_del, obj);
     }
}

void
termio_theme_set(Evas_Object *obj, Evas_Object *theme)
{
   Termio *sd = evas_object_smart_data_get(obj);
   if (!sd) return;
   if (theme) sd->theme = theme;
}

Evas_Object *
termio_theme_get(Evas_Object *obj)
{
   Termio *sd = evas_object_smart_data_get(obj);
   if (!sd) return NULL;
   return sd->theme;
}

char *
termio_selection_get(Evas_Object *obj, int c1x, int c1y, int c2x, int c2y,
                     size_t *len)
{
   Termio *sd = evas_object_smart_data_get(obj);
   Eina_Strbuf *sb;
   char *s;
   int x, y;
   size_t len_backup;

   if (!sd) return NULL;
   sb = eina_strbuf_new();
   termpty_cellcomp_freeze(sd->pty);
   for (y = c1y; y <= c2y; y++)
     {
        Termcell *cells;
        int w, last0, v, start_x, end_x;

        w = 0;
        last0 = -1;
        cells = termpty_cellrow_get(sd->pty, y, &w);
        if (!cells) continue;
        if (w > sd->grid.w) w = sd->grid.w;
        if (y == c1y && c1x >= w)
          {
             eina_strbuf_append_char(sb, '\n');
             continue;
          }
        start_x = c1x;
        end_x = (c2x >= w) ? w - 1 : c2x;
        if (c1y != c2y)
          {
             if (y == c1y) end_x = w - 1;
             else if (y == c2y) start_x = 0;
             else
               {
                  start_x = 0;
                  end_x = w - 1;
               }
          }
        for (x = start_x; x <= end_x; x++)
          {
#if defined(SUPPORT_DBLWIDTH)
             if ((cells[x].codepoint == 0) && (cells[x].att.dblwidth))
               {
                  if (x < end_x) x++;
                  else break;
               }
#endif
             if (x >= w) break;
             if ((cells[x].codepoint == 0) || (cells[x].codepoint == ' '))
               {
                  if (last0 < 0) last0 = x;
               }
             else if (cells[x].att.newline)
               {
                  last0 = -1;
                  if ((y != c2y) || (x != end_x))
                    eina_strbuf_append_char(sb, '\n');
                  break;
               }
             else if (cells[x].att.tab)
               {
                  eina_strbuf_append_char(sb, '\t');
                  x = ((x + 8) / 8) * 8;
                  x--;
               }
             else
               {
                  char txt[8];
                  int txtlen;

                  if (last0 >= 0)
                    {
                       v = x - last0 - 1;
                       last0 = -1;
                       while (v >= 0)
                         {
                            eina_strbuf_append_char(sb, ' ');
                            v--;
                         }
                    }
                  txtlen = codepoint_to_utf8(cells[x].codepoint, txt);
                  if (txtlen > 0)
                    eina_strbuf_append_length(sb, txt, txtlen);
                  if ((x == (w - 1)) && (x != c2x))
                    {
                       if (!cells[x].att.autowrapped)
                         eina_strbuf_append_char(sb, '\n');
                    }
               }
          }
        if (last0 >= 0)
          {
             if (y == c2y)
               {
                  Eina_Bool have_more = EINA_FALSE;

                  for (x = end_x + 1; x < w; x++)
                    {
#if defined(SUPPORT_DBLWIDTH)
                       if ((cells[x].codepoint == 0) &&
                           (cells[x].att.dblwidth))
                         {
                            if (x < (w - 1)) x++;
                            else break;
                         }
#endif
                       if (((cells[x].codepoint != 0) &&
                            (cells[x].codepoint != ' ')) ||
                           (cells[x].att.newline) ||
                           (cells[x].att.tab))
                         {
                            have_more = EINA_TRUE;
                            break;
                         }
                    }
                  if (!have_more) eina_strbuf_append_char(sb, '\n');
                  else
                    {
                       for (x = last0; x <= end_x; x++)
                         {
#if defined(SUPPORT_DBLWIDTH)
                            if ((cells[x].codepoint == 0) &&
                                (cells[x].att.dblwidth))
                              {
                                 if (x < (w - 1)) x++;
                                 else break;
                              }
#endif
                            if (x >= w) break;
                            eina_strbuf_append_char(sb, ' ');
                         }
                    }
               }
             else eina_strbuf_append_char(sb, '\n');
          }
     }
   termpty_cellcomp_thaw(sd->pty);

   if (!len) len = &len_backup;
   *len = eina_strbuf_length_get(sb);
   if (!*len)
     {
        eina_strbuf_free(sb);
        return NULL;
     }
   s = eina_strbuf_string_steal(sb);
   eina_strbuf_free(sb);
   return s;
}

void
termio_config_update(Evas_Object *obj)
{
   Termio *sd = evas_object_smart_data_get(obj);
   Evas_Coord w, h;
   char buf[4096];

   if (!sd) return;

   if (sd->font.name) eina_stringshare_del(sd->font.name);
   sd->font.name = NULL;

   if (sd->config->font.bitmap)
     {
        snprintf(buf, sizeof(buf), "%s/fonts/%s",
                 elm_app_data_dir_get(), sd->config->font.name);
        sd->font.name = eina_stringshare_add(buf);
     }
   else
     sd->font.name = eina_stringshare_add(sd->config->font.name);
   sd->font.size = sd->config->font.size;

   sd->jump_on_change = sd->config->jump_on_change;
   sd->jump_on_keypress = sd->config->jump_on_keypress;

   termpty_backscroll_set(sd->pty, sd->config->scrollback);
   sd->scroll = 0;

   if (evas_object_focus_get(obj))
     {
        edje_object_signal_emit(sd->cur.obj, "focus,out", "terminology");
        if (sd->config->disable_cursor_blink)
          edje_object_signal_emit(sd->cur.obj, "focus,in,noblink", "terminology");
        else
          edje_object_signal_emit(sd->cur.obj, "focus,in", "terminology");
     }
   
   evas_object_scale_set(sd->grid.obj, elm_config_scale_get());
   evas_object_textgrid_font_set(sd->grid.obj, sd->font.name, sd->font.size);
   evas_object_textgrid_cell_size_get(sd->grid.obj, &w, &h);

   //evas_object_scale_set(sd->scrolio.grid.obj, elm_config_scale_get());
   //evas_object_textgrid_font_set(sd->scrolio.grid.obj, sd->font.name, sd->font.size);
   //evas_object_textgrid_cell_size_get(sd->scrolio.grid.obj, &w, &h);
   if (w < 1) w = 1;
   if (h < 1) h = 1;
   sd->font.chw = w;
   sd->font.chh = h;
   _smart_size(obj, sd->grid.w, sd->grid.h, EINA_TRUE);
}

Config *
termio_config_get(const Evas_Object *obj)
{
   Termio *sd = evas_object_smart_data_get(obj);
   EINA_SAFETY_ON_NULL_RETURN_VAL(sd, NULL);
   return sd->config;
}

void
termio_copy_clipboard(Evas_Object *obj)
{
   _take_selection(obj, ELM_SEL_TYPE_CLIPBOARD);
}

void
termio_paste_clipboard(Evas_Object *obj)
{
   _paste_selection(obj, ELM_SEL_TYPE_CLIPBOARD);
}

const char  *
termio_link_get(const Evas_Object *obj)
{
   Termio *sd = evas_object_smart_data_get(obj);
   EINA_SAFETY_ON_NULL_RETURN_VAL(sd, NULL);
   return sd->link.string;
}

void
termio_mouseover_suspend_pushpop(Evas_Object *obj, int dir)
{
   Termio *sd = evas_object_smart_data_get(obj);
   if (!sd) return;
   sd->link.suspend += dir;
   if (sd->link.suspend < 0) sd->link.suspend = 0;
   _smart_update_queue(obj, sd);
}

void
termio_event_feed_mouse_in(Evas_Object *obj)
{
   Evas *e;
   Termio *sd = evas_object_smart_data_get(obj);

   if (!sd) return;
   e = evas_object_evas_get(obj);
   evas_event_feed_mouse_in(e, 0, NULL);
}

void
termio_size_get(Evas_Object *obj, int *w, int *h)
{
   Termio *sd = evas_object_smart_data_get(obj);
   if (!sd)
     {
        if (w) *w = 0;
        if (h) *h = 0;
        return;
     }
   if (w) *w = sd->grid.w;
   if (h) *h = sd->grid.h;
}

int
termio_scroll_get(Evas_Object *obj)
{
   Termio *sd = evas_object_smart_data_get(obj);
   if (!sd) return 0;
   return sd->scroll;
}

void
termio_scroll_set(Evas_Object *obj, int scroll)
{
   Termio *sd = evas_object_smart_data_get(obj);
   sd->scroll = scroll;
   _smart_apply(obj);
}

pid_t
termio_pid_get(const Evas_Object *obj)
{
   Termio *sd = evas_object_smart_data_get(obj);
   if (!sd) return 0;
   return termpty_pid_get(sd->pty);
}

Eina_Bool
termio_cwd_get(const Evas_Object *obj, char *buf, size_t size)
{
   char procpath[PATH_MAX];
   Termio *sd = evas_object_smart_data_get(obj);
   pid_t pid;
   ssize_t siz;

   if (!sd) return EINA_FALSE;

   pid = termpty_pid_get(sd->pty);
   snprintf(procpath, sizeof(procpath), "/proc/%d/cwd", pid);
   if ((siz = readlink(procpath, buf, size)) < 1)
     {
        ERR("Could not load working directory %s: %s",
            procpath, strerror(errno));
        return EINA_FALSE;
     }
   buf[siz] = 0;
   return EINA_TRUE;
}

Evas_Object *
termio_textgrid_get(Evas_Object *obj)
{
   Termio *sd = evas_object_smart_data_get(obj);
   if (!sd) return NULL;

   return sd->grid.obj;
}

Evas_Object *
termio_win_get(Evas_Object *obj)
{
   Termio *sd = evas_object_smart_data_get(obj);
   if (!sd) return NULL;

   return sd->win;
}


static void
_smart_mirror_del(void *data, Evas *evas __UNUSED__, Evas_Object *obj, void *info __UNUSED__)
{
   Termio *sd = evas_object_smart_data_get(data);
   if (!sd) return;
   evas_object_event_callback_del_full(obj, EVAS_CALLBACK_DEL,
                                       _smart_mirror_del, data);
   sd->mirrors = eina_list_remove(sd->mirrors, obj);
}

Evas_Object *
termio_mirror_add(Evas_Object *obj)
{
   Evas_Object *img;
   Termio *sd = evas_object_smart_data_get(obj);
   Evas_Coord w = 0, h = 0;
   if (!sd) return NULL;
   img = evas_object_image_filled_add(evas_object_evas_get(obj));
   evas_object_image_source_set(img, obj);
   evas_object_geometry_get(obj, NULL, NULL, &w, &h);
   evas_object_resize(img, w, h);
   sd->mirrors = eina_list_append(sd->mirrors, img);
   evas_object_data_set(img, "termio", obj);
   evas_object_event_callback_add(img, EVAS_CALLBACK_DEL,
                                  _smart_mirror_del, obj);
   return img;
}

const char *
termio_title_get(Evas_Object *obj)
{
   Termio *sd = evas_object_smart_data_get(obj);
   if (!sd) return NULL;
   return sd->pty->prop.title;
}

const char *
termio_icon_name_get(Evas_Object *obj)
{
   Termio *sd = evas_object_smart_data_get(obj);
   if (!sd) return NULL;
   return sd->pty->prop.icon;
}

void
termio_debugwhite_set(Evas_Object *obj, Eina_Bool dbg)
{
   Termio *sd = evas_object_smart_data_get(obj);
   if (!sd) return;
   sd->debugwhite = dbg;
   _smart_apply(obj);
}
