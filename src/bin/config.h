#ifndef _CONFIG_H__
#define _CONFIG_H__ 1

typedef struct _Config Config;

/* TODO: separate config per terminal (tab, window) and global. */

struct _Config
{
   int version;
   struct {
      const char    *name;
      const char    *orig_name; /* not in EET */
      int            size;
      int            orig_size; /* not in EET */
      unsigned char  bitmap;
      unsigned char  orig_bitmap; /* not in EET */
   } font;
   struct {
      const char    *email;
      struct {
         const char    *general;
         const char    *video;
         const char    *image;
      } url, local;
      Eina_Bool      inline_please;
   } helper;
   const char       *theme;
   const char       *background;
   const char       *wordsep;
   int               scrollback;
   double            tab_zoom;
   int               vidmod;
   Eina_Bool         jump_on_keypress;
   Eina_Bool         jump_on_change;
   Eina_Bool         flicker_on_key;
   Eina_Bool         disable_cursor_blink;
   Eina_Bool         disable_visual_bell;
   Eina_Bool         translucent;
   Eina_Bool         mute;
   Eina_Bool         urg_bell;
   Eina_Bool         multi_instance;
   Eina_Bool         custom_geometry;
   int               cg_width;
   int               cg_height;
   Eina_Bool         miniview;

   Eina_Bool         temporary; /* not in EET */
   const char       *config_key; /* not in EET, the key that config was loaded */
};

void config_init(void);
void config_shutdown(void);
void config_sync(const Config *config_src, Config *config);
void config_save(Config *config, const char *key);
Config *config_load(const char *key);
Config *config_fork(Config *config);
void config_del(Config *config);

const char *config_theme_path_get(const Config *config);
const char *config_theme_path_default_get(const Config *config);

#endif
