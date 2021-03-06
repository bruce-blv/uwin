/* Copyright (C) 1999 Joey Shutup.
 *
 * http://www.streetmap.co.uk/streetmap.dll?postcode2map?BS24+9TZ
 *
 * No guarantees or warranties or anything are provided or implied in any way
 * whatsoever.  Use this program at your own risk.  Permission to use this
 * program for any purpose is given, as long as the copyright is kept intact.
 */
/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */
#define FVWMTHEME_PRIVATE

#include "config.h"

#include <stdio.h>
#include <signal.h>

#include <X11/Xlib.h>
#include <X11/Xproto.h>
#include <X11/Intrinsic.h>

#include "libs/fvwmlib.h"
#include "libs/FShape.h"
#include "libs/Module.h"
#include "libs/Picture.h"
#include "libs/Colorset.h"
#include "libs/fvwmsignal.h"

/* Globals */
static Display *dpy;
static Window win;			/* need a window to create pixmaps */
static GC gc;				/* ditto */
static char *name;
static int namelen;
static int color_limit = 0;
static int fd[2];			/* communication pipes */
static Bool privateCells = False;	/* set when read/write colors used */
static Bool sharedCells = False;	/* set after a shared color is used */
static char *black = "black";
static char *gray = "gray";

/* forward declarations */
static RETSIGTYPE signal_handler(int signal);
static void set_signals(void);
static int error_handler(Display *dpy, XErrorEvent *e);
static void parse_config(void);
static void parse_config_line(char *line);
static void parse_message_line(char *line);
static void main_loop(void) __attribute__((__noreturn__));
static void parse_colorset(char *line);
static void add_to_junk(Pixmap pixmap);
static void feng_shui();

/* When FvemTheme destroys pixmaps it puts them on a list and only destoys them
 * after some period of inactivity. This is necessary because changing colorset
 * options rapidly may result in a module redrawing itself due to the first
 * change whie the second change is happening. If the module renders something
 * with the colorset affected by the second change there is a chance it may
 * reference pixmaps that FvwmTheme has destroyed */
struct junklist {
  struct junklist *prev;
  Pixmap pixmap;
};
static struct junklist *junk = NULL;

void MyXParseColor(
  Display *display, Colormap colormap, char *spec, XColor *exact_def_return)
{
  if (!XParseColor(display, colormap, spec, exact_def_return))
  {
    fprintf(stderr,"%s: can't parse color \"%s\"\n", name, (spec) ? spec : "");
    exact_def_return->red = 0;
    exact_def_return->green = 0;
    exact_def_return->blue = 0;
    exact_def_return->flags |= DoRed | DoGreen | DoBlue;
  }

  return;
}

int main(int argc, char **argv)
{
  XSetWindowAttributes xswa;
  XGCValues xgcv;

  name = GetFileNameFromPath(argv[0]);
  namelen = strlen(name);

  set_signals();

  /* try to make sure it is fvwm that spawned this module */
  if (argc != 6) {
    fprintf(stderr, "%s "VERSION" should only be executed by fvwm!\n", name);
    exit(1);
  }

  /* note the communication pipes */
  fd[0] = atoi(argv[1]);
  fd[1] = atoi(argv[2]);

  /* open the display and initialize the picture library */
  dpy = XOpenDisplay(NULL);
  if (!dpy) {
    fprintf(stderr,"%s: can't open display %s", name, XDisplayName(NULL));
    exit (1);
  }
  InitPictureCMap(dpy);

  /* This module allocates resouces that othe rmodules may rely on.
   * Set the closedown mode so that the pixmaps and colors are not freed
   * by the server if this module dies
   */
  XSetCloseDownMode(dpy, RetainTemporary);

  /* create a window to work in */
  xswa.background_pixmap = None;
  xswa.border_pixel = 0;
  xswa.colormap = Pcmap;
  win = XCreateWindow(dpy, RootWindow(dpy, DefaultScreen(dpy)), -2, -2, 2, 2,
		      0, Pdepth, InputOutput, Pvisual,
		      CWBackPixmap | CWBorderPixel | CWColormap, &xswa);

  /* create a GC */
  gc = fvwmlib_XCreateGC(dpy, win, 0, &xgcv);

  /* die horribly if either win or gc could not be created */
  XSync(dpy, False);

  /* install a non-fatal error handler */
  XSetErrorHandler(error_handler);

  /* get the initial configuration options */
  parse_config();

  /* tell fvwm we're running */
  SendFinishedStartupNotification(fd);

  /* garbage collect */
  alloca(0);

  /* just in case any previous FvwmTheme has died and left pixmaps dangling.
   * This might be overkill but any other method must ensure that fvwm doesn't
   * get killed (it can be the owner of the pixels in colorset 0)
   */
  XKillClient(dpy, AllTemporary);

  /* sit around waiting for something to do */
  main_loop();
}

static void main_loop(void)
{
  FvwmPacket *packet;
  fd_set_size_t fd_width;
  fd_set in_fdset;
  struct timeval delay;

  fd_width = fd[1] + 1;
  FD_ZERO(&in_fdset);
  delay.tv_sec = 5;
  delay.tv_usec = 0;

  while (True) {
    /* garbage collect */
    alloca(0);

    FD_SET(fd[1], &in_fdset);
    /* wait for an instruction from fvwm or a timeout */
    if (fvwmSelect(fd_width, &in_fdset, NULL, NULL, junk ? &delay : NULL) < 0)
    {
      fprintf(stderr, "%s: select error!\n", name);
      exit(-1);
    }

    if (!FD_ISSET(fd[1], &in_fdset))
    { /* have a timeout, tidy up */
      feng_shui();
      continue;
    }

    packet = ReadFvwmPacket(fd[1]);
    if (!packet)
      exit(0);

    /* handle dynamic config lines and text messages */
    switch (packet->type) {
    case M_CONFIG_INFO:
      parse_config_line((char *)&packet->body[3]);
      SendUnlockNotification(fd);
      break;
    case M_STRING:
      parse_message_line((char *)&packet->body[3]);
      SendUnlockNotification(fd);
      break;
    }
  }
}

/* config options, the first NULL is replaced with *FvwmThemeColorset */
/* the second NULL is replaced with *FvwmThemeReadWriteColors */
/* FvwmTheme ignores any "colorset" lines since it causes them */
static char *config_options[] =
{
  "ImagePath",
  "ColorLimit",
  NULL,
  NULL,
  NULL
};

static void parse_config_line(char *line)
{
  char *rest;

  switch(GetTokenIndex(line, config_options, -1, &rest))
  {
  case 0: /* ImagePath */
    SetImagePath(rest);
    break;
  case 1: /* ColorLimit */
    sscanf(rest, "%d", &color_limit);
    break;
  case 2: /* *FvwmThemColorset */
    parse_colorset(rest);
    break;
  case 3: /* *FvwmThemeReadWriteColors */
    if (sharedCells)
      fprintf(stderr, "%s: must come first, already allocated shared pixels\n",
	      config_options[3]);
    else if (Pvisual->class != PseudoColor)
      fprintf(stderr, "%s: only works with PseudoColor visuals\n",
	      config_options[3]);
    else
      privateCells = True;
  }
}

static char *get_simple_color(
  char *string, char **color, colorset_struct *cs, int supplied_color,
  int special_flag, char *special_string)
{
  char *rest;

  if (*color)
  {
    free(*color);
    *color = NULL;
  }
  rest = GetNextToken(string, color);
  if (*color)
  {
    if (special_string && StrEquals(*color, special_string))
    {
      free(*color);
      *color = NULL;
      cs->color_flags |= special_flag;
      cs->color_flags &= ~supplied_color;
    }
    else
    {
      cs->color_flags |= supplied_color;
      cs->color_flags &= ~special_flag;
    }
  }
  else
    cs->color_flags &= ~(supplied_color | special_flag);

  return rest;
}

static void SafeDestroyPicture(Display *dpy, Picture *picture)
{
    /* have to subvert destroy picture so that it doesn't free pixmaps
     * these are added to the junk list to be cleaned up after a timeout */
    if (picture->count < 2)
    {
      if (picture->picture)
      {
        add_to_junk(picture->picture);
        picture->picture = None;
      }
      if (picture->mask)
      {
        add_to_junk(picture->mask);
        picture->mask = None;
      }
    }
    /* all that this will now do is free the colors and the name */
    DestroyPicture(dpy, picture);
}

static void free_colorset_background(colorset_struct *cs)
{
  if (cs->picture) {
    if (cs->picture->picture != cs->pixmap)
      fprintf(stderr, "FvwmTheme warning: cs->picture != cs->pixmap\n");
    SafeDestroyPicture(dpy, cs->picture);
    cs->picture = None;
    cs->pixmap = None;
  }
  if (cs->pixmap && cs->pixmap != ParentRelative)
  {
    add_to_junk(cs->pixmap);
  }
  cs->pixmap = None;
  if (cs->mask)
  {
    add_to_junk(cs->mask);
    cs->mask = None;
  }
  if (cs->pixels && cs->nalloc_pixels)
  {
    XFreeColors(dpy, Pcmap, cs->pixels, cs->nalloc_pixels, 0);
    free(cs->pixels);
    cs->pixels = NULL;
    cs->nalloc_pixels = 0;
  }
}

static char *csetopts[] =
{
  "Foreground",
  "Fore",
  "fg",

  "Background",
  "Back",
  "bg",

  "Hilight",
  "Hilite",
  "hi",

  "Shadow",
  "Shade",
  "sh",

  /* these strings are used inside the cases in the switch below! */
  "Pixmap",
  "TiledPixmap",
  "AspectPixmap",

  /* these strings are used inside the cases in the switch below! */
  "Shape",
  "TiledShape",
  "AspectShape",

  /* switch off pixmaps and gradients */
  "Plain",
  /* switch off shape */
  "NoShape",

  /* Make the background transparent, it copies the root window background */
  "Transparent",

  NULL
};

/* translate a colorset spec into a colorset structure */
static void parse_colorset(char *line)
{
  int n;
  int i;
  int w;
  int h;
  colorset_struct *cs;
  char *token;
  char *optstring;
  char *args;
  char *option;
  char type;
  char *fg = NULL;
  char *bg = NULL;
  char *hi = NULL;
  char *sh = NULL;
  Bool do_remove_shape;
  Bool have_pixels_changed = False;
  Bool has_fg_changed = False;
  Bool has_bg_changed = False;
  Bool has_sh_changed = False;
  Bool has_hi_changed = False;
  Bool has_pixmap_changed = False;
  Bool has_shape_changed = False;
  XColor color;
  XGCValues xgcv;
  static GC mono_gc = None;
  static int nColorsets = 0;

  /* find out which colorset */
  if (GetIntegerArguments(line, &line, &n, 1) != 1)
    return;
  if (n < 0)
  {
    fprintf(stderr, "%s: colorset number must be zero or positive\n", name);
    return;
  }

  /* make sure it exists and has sensible contents */
  if (nColorsets <= n) {
    Colorset =
      (colorset_struct *)saferealloc((char *)Colorset,
				     (n + 1) * sizeof(colorset_struct));
    memset(&Colorset[nColorsets], 0,
	   (n + 1 - nColorsets) * sizeof(colorset_struct));
  }

  /* initialize new colorsets to black on gray */
  while (nColorsets <= n)
  {
    colorset_struct *ncs = &Colorset[nColorsets];
    have_pixels_changed = True;
    if (privateCells &&
	/* grab four writeable cells */
	XAllocColorCells(dpy, Pcmap, False, NULL, 0, &(ncs->fg), 4)) {
      XColor *colorp;

      /* set the fg color */
      MyXParseColor(dpy, Pcmap, black, &color);
      color.pixel = ncs->fg;
      XStoreColor(dpy, Pcmap, &color);
      /* set the bg */
      MyXParseColor(dpy, Pcmap, gray, &color);
      color.pixel = ncs->bg;
      XStoreColor(dpy, Pcmap, &color);
      /* calculate and set the hilite */
      colorp = GetHiliteColor(ncs->bg);
      colorp->pixel = ncs->hilite;
      XStoreColor(dpy, Pcmap, colorp);
      /* calculate and set the shadow */
      colorp = GetShadowColor(ncs->bg);
      colorp->pixel = ncs->shadow;
      XStoreColor(dpy, Pcmap, colorp);
    } else {
      /* grab four shareable colors */
      Colorset[nColorsets].fg = GetColor(black);
      Colorset[nColorsets].bg = GetColor(gray);
      Colorset[nColorsets].hilite = GetHilite(Colorset[nColorsets].bg);
      Colorset[nColorsets].shadow = GetShadow(Colorset[nColorsets].bg);
      /* set flags for fg contrast, bg average in case just a pixmap is given */
      Colorset[nColorsets].color_flags = FG_CONTRAST | BG_AVERAGE;
    }
    nColorsets++;
  }

  cs = &Colorset[n];

  /* ---------- Parse the options ---------- */
  while (line && *line)
  {
    /* Read next option specification (delimited by a comma or \0). */
    line = GetQuotedString(line, &optstring, ",", NULL, NULL, NULL);
    if (!optstring)
      break;
    args = GetNextToken(optstring, &option);
    if (!option)
    {
      free(optstring);
      break;
    }

    do_remove_shape = False;
    switch((i = GetTokenIndex(option, csetopts, 0, NULL)))
    {
    case 0: /* Foreground */
    case 1: /* Fore */
    case 2: /* fg */
      get_simple_color(args, &fg, cs, FG_SUPPLIED, FG_CONTRAST, "contrast");
      has_fg_changed = True;
      break;
    case 3: /* Background */
    case 4: /* Back */
    case 5: /* bg */
      get_simple_color(args, &bg, cs, BG_SUPPLIED, BG_AVERAGE, "average");
      has_bg_changed = True;
      break;
    case 6: /* Hilight */
    case 7: /* Hilite */
    case 8: /* hi */
      get_simple_color(args, &hi, cs, HI_SUPPLIED, 0, NULL);
      has_hi_changed = True;
      break;
    case 9: /* Shadow */
    case 10: /* Shade */
    case 11: /* sh */
      get_simple_color(args, &sh, cs, SH_SUPPLIED, 0, NULL);
      has_sh_changed = True;
      break;
    case 12: /* TiledPixmap */
    case 13: /* Pixmap */
    case 14: /* AspectPixmap */
      has_pixmap_changed = True;
      free_colorset_background(cs);
      /* set the flags */
      if (csetopts[i][0] == 'T')
	cs->pixmap_type = PIXMAP_TILED;
      else if (csetopts[i][0] == 'A')
	cs->pixmap_type = PIXMAP_STRETCH_ASPECT;
      else
	cs->pixmap_type = PIXMAP_STRETCH;

      /* read filename */
      token = PeekToken(args, &args);
      if (!token)
	break;
      /* load the file using the color reduction routines in Picture.c */
      cs->picture = CachePicture(dpy, win, NULL, token, color_limit);
      if (!cs->picture)
      {
	fprintf(stderr, "%s: can't load picture %s\n", name, token);
	break;
      }
      /* don't try to be smart with bitmaps */
      if (cs->picture->depth != Pdepth)
      {
	fprintf(stderr, "%s: bitmaps not supported\n", name);
	SafeDestroyPicture(dpy, cs->picture);
	cs->picture = None;
	break;
      }
      /* copy the picture pixmap into the public colorset structure */
      cs->width = cs->picture->width;
      cs->height = cs->picture->height;
      cs->pixmap = cs->picture->picture;

      if (cs->pixmap)
      {
	if (cs->picture->mask != None)
	{
	  /* make an inverted copy of the mask */
	  cs->mask = XCreatePixmap(dpy, win, cs->width, cs->height, 1);
	  if (cs->mask)
	  {
	    if (mono_gc == None)
	    {
	      xgcv.foreground = 1;
	      xgcv.background = 0;
	      /* create a gc for 1 bit depth */
	      mono_gc = fvwmlib_XCreateGC(
		dpy, cs->mask, GCForeground | GCBackground, &xgcv);
	    }
	    XCopyArea(dpy, cs->picture->mask, cs->mask, mono_gc, 0, 0,
		      cs->width, cs->height, 0, 0);
	    /* Invert the mask. We use it to draw the background. */
	    XSetFunction(dpy, mono_gc, GXinvert);
	    XFillRectangle(dpy, cs->mask, mono_gc, 0, 0, cs->width, cs->height);
	    XSetFunction(dpy, mono_gc, GXcopy);
	  }
	}
      }
      break;
    case 15: /* Shape */
    case 16: /* TiledShape */
    case 17: /* AspectShape */
      if (FHaveShapeExtension)
      {
	/* read filename */
	token = PeekToken(args, &args);
	has_shape_changed = True;
	if (cs->shape_mask)
	{
	  add_to_junk(cs->shape_mask);
	  cs->shape_mask = None;
	}
	if (do_remove_shape == True)
	  break;
	/* set the flags */
	if (csetopts[i][0] == 'T')
	  cs->shape_type = SHAPE_TILED;
	else if (csetopts[i][0] == 'A')
	  cs->shape_type = SHAPE_STRETCH_ASPECT;
	else
	  cs->shape_type = SHAPE_STRETCH;

	/* try to load the shape mask */
	if (token)
	{
	  Picture *picture;

	  /* load the shape mask */
	  picture = CachePicture(dpy, win, NULL, token, color_limit);
	  if (!picture)
	    fprintf(stderr, "%s: can't load picture %s\n", name, token);
	  else if (picture->depth != 1 && picture->mask == None)
	  {
	    fprintf(stderr, "%s: shape pixmap must be of depth 1\n", name);
	    SafeDestroyPicture(dpy, picture);
	  }
	  else
	  {
	    Pixmap mask;

	    /* okay, we have what we want */
	    if (picture->mask != None)
	      mask = picture->mask;
	    else
	      mask = picture->picture;
	    cs->shape_width = picture->width;
	    cs->shape_height = picture->height;

	    if (mask != None)
	    {
	      cs->shape_mask = XCreatePixmap(
		dpy, mask, picture->width, picture->height, 1);
	      if (cs->shape_mask != None)
	      {
		if (mono_gc == None)
		{
		  xgcv.foreground = 1;
		  xgcv.background = 0;
		  /* create a gc for 1 bit depth */
		  mono_gc = fvwmlib_XCreateGC(
		    dpy, picture->mask, GCForeground|GCBackground, &xgcv);
		}
		XCopyPlane(dpy, mask, cs->shape_mask, mono_gc, 0, 0,
			   picture->width, picture->height, 0, 0, 1);
	      }
	    }
	  }
	  if (picture)
	  {
	    SafeDestroyPicture(dpy, picture);
	    picture = None;
	  }
	}
      }
      else
      {
	cs->shape_mask = None;
      }
      break;
    case 18: /* Plain */
      has_pixmap_changed = True;
      free_colorset_background(cs);
      break;
    case 19: /* NoShape */
      has_shape_changed = True;
      if (cs->shape_mask)
      {
	add_to_junk(cs->shape_mask);
	cs->shape_mask = None;
      }
      break;
    case 20: /* Transparent */
      /* this is only allowable when the root depth == fvwm visual depth
       * otherwise bad match errors happen, it may be even more restrictive
       * but my tests (on exceed 6.2) show that only == depth is necessary */
      if (Pdepth != DefaultDepth(dpy, (DefaultScreen(dpy))))
      {
	fprintf(stderr,
		"%s: can't do Transparent when root_depth != fvwm_depth\n",
		name);
	break;
      }
      has_pixmap_changed = True;
      free_colorset_background(cs);
      cs->pixmap = ParentRelative;
      cs->pixmap_type = PIXMAP_STRETCH;
      break;
    default:
      /* test for ?Gradient */
      if (option[0] && StrEquals(&option[1], "Gradient"))
      {
	type = toupper(option[0]);
	if (!IsGradientTypeSupported(type))
	  break;
	has_pixmap_changed = True;
	free_colorset_background(cs);
	/* create a pixmap of the gradient type */
	cs->pixmap = CreateGradientPixmapFromString(dpy, win, gc, type, args,
						    &w, &h, &cs->pixels,
						    &cs->nalloc_pixels);
	cs->width = w;
	cs->height = h;
	if (type == V_GRADIENT)
	  cs->pixmap_type = PIXMAP_STRETCH_Y;
	else if (type == H_GRADIENT)
	  cs->pixmap_type = PIXMAP_STRETCH_X;
	else
	  cs->pixmap_type = PIXMAP_STRETCH;
      }
      else
      {
	fprintf(stderr, "%s: bad colorset pixmap specifier %s %s\n", name,
		option, line);
      }
      break;
    } /* switch */

    if (option)
    {
      free(option);
      option = NULL;
    }
    free(optstring);
    optstring = NULL;
  } /* while (line && *line) */

  /*
   * ---------- change the background colour ----------
   */
  if (has_bg_changed || has_pixmap_changed)
  {
    Bool do_set_default_background = False;

    if ((cs->color_flags & BG_AVERAGE) && cs->pixmap != None &&
        cs->pixmap != ParentRelative)
    {
      /* calculate average background color */
      XColor *colors;
      XImage *image;
      XImage *mask_image = None;
      unsigned int i, j, k = 0;
      unsigned long red = 0, blue = 0, green = 0;
      unsigned long tred, tblue, tgreen;
      double dred = 0.0, dblue = 0.0, dgreen = 0.0;

      /* create an array to store all the pixmap colors in */
      colors = (XColor *)safemalloc(cs->width * cs->height * sizeof(XColor));
      /* get the pixmap and mask into an image */
      image = XGetImage(dpy, cs->pixmap, 0, 0, cs->width, cs->height,
			AllPlanes, ZPixmap);
      if (cs->mask != None)
	mask_image = XGetImage(dpy, cs->mask, 0, 0, cs->width, cs->height,
			       AllPlanes, ZPixmap);
      /* only fetch the pixels that are not masked out */
      for (i = 0; i < cs->width; i++)
	for (j = 0; j < cs->height; j++)
	  if ((cs->mask == None) || (XGetPixel(mask_image, i, j) == 0))
	    colors[k++].pixel = XGetPixel(image, i, j);

      XDestroyImage(image);
      if (mask_image != None)
	XDestroyImage(mask_image);
      if (k == 0)
      {
	do_set_default_background = True;
      }
      else
      {
	/* look them all up, XQueryColors() can't handle more than 256 */
	for (i = 0; i < k; i += 256)
	  XQueryColors(dpy, Pcmap, &colors[i], min(k - i, 256));
	/* calculate average, ignore overflow: .red is short, red is long */

        for (i = 0; i < k; i++)
        {
          tred = red;
          red += colors[i].red;
          if (red < tred)
          {
            dred += (double)tred;
            red = colors[i].red;
          }
          tgreen = green;
          green += colors[i].green;
          if (green < tgreen)
          {
            dgreen += (double)tgreen;
            green = colors[i].green;
          }
          tblue = blue;
          blue += colors[i].blue;
          if (blue < tblue)
          {
            dblue += (double)tblue;
            blue = colors[i].blue;
          }
        }
        dred += red;
        dgreen += green;
        dblue += blue;
        free(colors);
        /* get it */
        color.red = dred / k;
        color.green = dgreen / k;
        color.blue = dblue / k;
	if (privateCells) {
	  color.pixel = cs->bg;
	  XStoreColor(dpy, Pcmap, &color);
	} else {
	  Pixel old_bg = cs->bg;

	  XFreeColors(dpy, Pcmap, &cs->bg, 1, 0);
	  XAllocColor(dpy, Pcmap, &color);
	  cs->bg = color.pixel;
	  if (old_bg != cs->bg)
	    have_pixels_changed = True;
	}
      }
    } /* average */
    else if ((cs->color_flags & BG_SUPPLIED) && bg != NULL)
    {
      /* user specified colour */
      if (privateCells) {
	unsigned short red, green, blue;

	MyXParseColor(dpy, Pcmap, bg, &color);
	red = color.red;
	green = color.green;
	blue = color.blue;
	color.pixel = cs->bg;
	XStoreColor(dpy, Pcmap, &color);
      } else {
	Pixel old_bg = cs->bg;

	XFreeColors(dpy, Pcmap, &cs->bg, 1, 0);
	cs->bg = GetColor(bg);
	if (old_bg != cs->bg)
	  have_pixels_changed = True;
      }
    } /* user specified */
    else if ((bg == NULL && has_bg_changed) ||
             ((cs->color_flags & BG_AVERAGE) && cs->pixmap == ParentRelative))
    {
      /* default */
      do_set_default_background = True;
    } /* default */
    if (do_set_default_background)
    {
      if (privateCells) {
        MyXParseColor(dpy, Pcmap, gray, &color);
        color.pixel = cs->bg;
        XStoreColor(dpy, Pcmap, &color);
      } else {
	Pixel old_bg = cs->bg;

	XFreeColors(dpy, Pcmap, &cs->bg, 1, 0);
	cs->bg = GetColor(gray);
	if (old_bg != cs->bg)
	  have_pixels_changed = True;
      }
    }
    has_bg_changed = True;
  } /* has_bg_changed */

  /*
   * ---------- change the foreground colour ----------
   */
  if (has_fg_changed || (has_bg_changed && (cs->color_flags & FG_CONTRAST)))
  {
    has_fg_changed = 1;
    if (cs->color_flags & FG_CONTRAST)
    {
      /* calculate contrasting foreground color */
      color.pixel = cs->bg;
      XQueryColor(dpy, Pcmap, &color);
      color.red = (color.red > 32767) ? 0 : 65535;
      color.green = (color.green > 32767) ? 0 : 65535;
      color.blue = (color.blue > 32767) ? 0 : 65535;
      if (privateCells) {
	color.pixel = cs->fg;
	XStoreColor(dpy, Pcmap, &color);
      } else {
	Pixel old_fg = cs->fg;

	XFreeColors(dpy, Pcmap, &cs->fg, 1, 0);
	XAllocColor(dpy, Pcmap, &color);
	cs->fg = color.pixel;
	if (old_fg != cs->fg)
	  have_pixels_changed = True;
      }
    } /* contrast */
    else if ((cs->color_flags & FG_SUPPLIED) && fg != NULL)
    {
      /* user specified colour */
      if (privateCells) {
	MyXParseColor(dpy, Pcmap, fg, &color);
	color.pixel = cs->fg;
	XStoreColor(dpy, Pcmap, &color);
      } else {
	Pixel old_fg = cs->fg;

	XFreeColors(dpy, Pcmap, &cs->fg, 1, 0);
	cs->fg = GetColor(fg);
	if (old_fg != cs->fg)
	  have_pixels_changed = True;
      }
    } /* user specified */
    else if (fg == NULL)
    {
      /* default */
      if (privateCells) {
        /* query it */
        MyXParseColor(dpy, Pcmap, black, &color);
        color.pixel = cs->fg;
        XStoreColor(dpy, Pcmap, &color);
      } else {
	Pixel old_fg = cs->fg;

	XFreeColors(dpy, Pcmap, &cs->fg, 1, 0);
	cs->fg = GetColor(black);
	if (old_fg != cs->fg)
	  have_pixels_changed = True;
      }
    }
  } /* has_fg_changed */

  /*
   * ---------- change the hilight colour ----------
   */
  if (has_hi_changed || has_bg_changed)
  {
    has_hi_changed = 1;
    if ((cs->color_flags & HI_SUPPLIED) && hi != NULL)
    {
      /* user specified colour */
      if (privateCells) {
	MyXParseColor(dpy, Pcmap, hi, &color);
	color.pixel = cs->hilite;
	XStoreColor(dpy, Pcmap, &color);
      } else {
	Pixel old_hilite = cs->hilite;

	XFreeColors(dpy, Pcmap, &cs->hilite, 1, 0);
	cs->hilite = GetColor(hi);
	if (old_hilite != cs->hilite)
	  have_pixels_changed = True;
      }
    } /* user specified */
    else if (hi == NULL)
    {
      if (privateCells) {
	XColor *colorp;

	colorp = GetHiliteColor(cs->bg);
	colorp->pixel = cs->hilite;
	XStoreColor(dpy, Pcmap, colorp);
      } else {
	Pixel old_hilite = cs->hilite;

	XFreeColors(dpy, Pcmap, &cs->hilite, 1, 0);
	cs->hilite = GetHilite(cs->bg);
	if (old_hilite != cs->hilite)
	  have_pixels_changed = True;
      }
    }
  } /* has_hi_changed */

  /*
   * ---------- change the shadow colour ----------
   */
  if (has_sh_changed || has_bg_changed)
  {
    has_sh_changed = 1;
    if ((cs->color_flags & SH_SUPPLIED) && sh != NULL)
    {
      /* user specified colour */
      if (privateCells) {
	MyXParseColor(dpy, Pcmap, sh, &color);
	color.pixel = cs->shadow;
	XStoreColor(dpy, Pcmap, &color);
      } else {
	Pixel old_shadow = cs->shadow;

	XFreeColors(dpy, Pcmap, &cs->shadow, 1, 0);
	cs->shadow = GetColor(sh);
	if (old_shadow != cs->shadow)
	  have_pixels_changed = True;
      }
    } /* user specified */
    else if (sh == NULL)
    {
      if (privateCells) {
	XColor *colorp;

	colorp = GetShadowColor(cs->bg);
	colorp->pixel = cs->shadow;
	XStoreColor(dpy, Pcmap, colorp);
      } else {
	Pixel old_shadow = cs->shadow;

	XFreeColors(dpy, Pcmap, &cs->shadow, 1, 0);
	cs->shadow = GetShadow(cs->bg);
	if (old_shadow != cs->shadow)
	  have_pixels_changed = True;
      }
    }
  } /* has_sh_changed */

  /*
   * ---------- change the masked out parts of the background pixmap ----------
   */
  if ((cs->mask != None) && (has_pixmap_changed || has_bg_changed))
  {
    /* Now that we know the background colour we can update the pixmap
     * background. */
    XSetForeground(dpy, gc, cs->bg);
    XSetClipMask(dpy, gc, cs->mask);
    XFillRectangle(dpy, cs->pixmap, gc, 0, 0, cs->width, cs->height);
    XSetClipMask(dpy, gc, None);
    has_pixmap_changed = True;
  } /* has_pixmap_changed */

  /*
   * ---------- send new colorset to fvwm and clean up ----------
   */
  /* make sure the server has this to avoid races */
  XSync(dpy, False);

  /* inform fvwm of the change */
  if (have_pixels_changed || has_pixmap_changed || has_shape_changed)
    SendText(fd, DumpColorset(n, &Colorset[n]), 0);

  if (fg)
    free(fg);
  if (bg)
    free(bg);
  if (hi)
    free(hi);
  if (sh)
    free(sh);

  /* if privateCells are not being used and XAllocColor has been used
   * we are stuck in sharedCells behaviour forever */
  /* have_pixels_changed will be set if a new colorset has been made */
  if (!privateCells && have_pixels_changed)
    sharedCells = True;
}

/* SendToModule options */
static char *message_options[] = {"Colorset", NULL};

static void parse_message_line(char *line)
{
  char *rest;

  switch(GetTokenIndex(line, message_options, -1, &rest)) {
  case 0:
    parse_colorset(rest);
    break;
  }
}

static void parse_config(void)
{
  char *line;

  /* prepare the tokenizer array, [0,1] are ImagePath and ColorLimit */
  config_options[2] = safemalloc(namelen + 10);
  sprintf(config_options[2], "*%sColorset", name);
  config_options[3] = safemalloc(namelen + 17);
  sprintf(config_options[3], "*%sReadWriteColors", name);

  /* set a filter on the config lines sent */
  line = safemalloc(namelen + 2);
  sprintf(line, "*%s", name);
  InitGetConfigLine(fd, line);
  free(line);

  /* tell fvwm what we want to receive */
  SetMessageMask(fd, M_CONFIG_INFO | M_END_CONFIG_INFO | M_SENDCONFIG
		 | M_STRING);

  /* loop on config lines, a bit strange looking because GetConfigLine
   * is a void(), have to test $line */
  while (GetConfigLine(fd, &line), line)
    parse_config_line(line);

  SetSyncMask(fd, M_CONFIG_INFO | M_STRING);
}

static int error_handler(Display *d, XErrorEvent *e)
{
  /* Since GetColor() doesn't return a status it is possible that FvwmTheme
   * will try to free a color that it hasn't XAlloc()'d. Allow this error */
  if (e->error_code == BadAccess && e->request_code == X_FreeColors)
  {
    fprintf(stderr, "%s: couldn't free a pixel, may have run out of colors\n",
	    name);
    return 0;
  }
  /* All other errors cause diagnostic output and stop execution */
  PrintXErrorAndCoredump(d, e, name);
  return 0;
}

static void set_signals(void) {
#ifdef HAVE_SIGACTION
  struct sigaction  sigact;

  sigemptyset(&sigact.sa_mask);
  sigaddset(&sigact.sa_mask, SIGPIPE);
  sigaddset(&sigact.sa_mask, SIGINT);
  sigaddset(&sigact.sa_mask, SIGHUP);
  sigaddset(&sigact.sa_mask, SIGTERM);
# ifdef SA_INTERRUPT
  sigact.sa_flags = SA_INTERRUPT;
# else
  sigact.sa_flags = 0;
# endif
  sigact.sa_handler = signal_handler;
  sigaction(SIGPIPE, &sigact, NULL);
  sigaction(SIGINT, &sigact, NULL);
  sigaction(SIGHUP, &sigact, NULL);
  sigaction(SIGTERM, &sigact, NULL);
#else
  /* We don't have sigaction(), so fall back to less robust methods.  */
#ifdef USE_BSD_SIGNALS
  fvwmSetSignalMask(sigmask(SIGPIPE)
		    | sigmask(SIGINT)
		    | sigmask(SIGHUP)
		    | sigmask(SIGTERM));
#endif
  signal(SIGPIPE, signal_handler);
  signal(SIGINT, signal_handler);
  signal(SIGHUP, signal_handler);
  signal(SIGTERM, signal_handler);
#ifdef HAVE_SIGINTERRUPT
  siginterrupt(SIGPIPE, 1);
  siginterrupt(SIGINT, 1);
  siginterrupt(SIGHUP, 1);
  siginterrupt(SIGTERM, 1);
#endif
#endif
}

static RETSIGTYPE signal_handler(int signal) {
  fprintf(stderr, "%s quiting on signal %d\n", name, signal);
  exit(signal);
}

static void add_to_junk(Pixmap pixmap)
{
  struct junklist *oldjunk = junk;

  junk = (struct junklist *)safemalloc(sizeof(struct junklist));
  junk->prev = oldjunk;
  junk->pixmap = pixmap;
}

static void feng_shui()
{
  struct junklist *oldjunk = junk;

  while (junk)
  {
    XFreePixmap(dpy, junk->pixmap);
    oldjunk = junk;
    junk = junk->prev;
    free(oldjunk);
  }
  XFlush(dpy);
}
