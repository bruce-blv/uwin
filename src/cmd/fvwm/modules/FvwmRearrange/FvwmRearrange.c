/*
 * FvwmRearrange.c -- fvwm module to arrange windows
 *
 * Copyright (C) 1996, 1997, 1998, 1999 Andrew T. Veliath
 *
 * Version 1.0
 *
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
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 * Combined FvwmTile and FvwmCascade to FvwmRearrange module.
 * 9-Nov-1998 Dominik Vogt
 */
#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#include <signal.h>

#ifdef HAVE_SYS_BSDTYPES_H
#include <sys/bsdtypes.h>
#endif

#if HAVE_SYS_SELECT_H
#include <sys/select.h>
#endif

#include <X11/Xlib.h>

#include "libs/fvwmlib.h"
#include "libs/FScreen.h"
#include "libs/Module.h"
#include "fvwm/fvwm.h"
#include "libs/vpacket.h"

typedef struct window_item {
	Window frame;
	int th, bw;
	unsigned long width, height;
	struct window_item *prev, *next;
} window_item, *window_list;

/* vars */
Display *dpy;
int dx, dy;
int dwidth, dheight;
char *argv0;
int fd[2];
fd_set_size_t fd_width;
window_list wins = NULL, wins_tail = NULL;
int wins_count = 0;
FILE *console;

/* switches */
int ofsx = 0, ofsy = 0;
int maxw = 0, maxh = 0;
int maxx, maxy;
int untitled = 0, transients = 0;
int maximized = 0;
int all = 0;
int desk = 0;
int reversed = 0, raise_window = 1;
int resize = 0;
int nostretch = 0;
int sticky = 0;
int flatx = 0, flaty = 0;
int incx = 0, incy = 0;
int horizontal = 0;
int maxnum = 0;

char FvwmTile;
char FvwmCascade;


void DeadPipe(int sig)
{
  exit(0);
}

void insert_window_list(window_list *wl, window_item *i)
{
  if (*wl) {
    if ((i->prev = (*wl)->prev))
      i->prev->next = i;
    i->next = *wl;
    (*wl)->prev = i;
  } else
    i->next = i->prev = NULL;
  *wl = i;
}

void free_window_list(window_list wl)
{
  window_item *q;

  while (wl)
  {
    q = wl;
    wl = wl->next;
    free(q);
  }
}

int is_suitable_window(unsigned long *body)
{
  XWindowAttributes xwa;
  struct ConfigWinPacket  *cfgpacket = (void *) body;

  if ((DO_SKIP_WINDOW_LIST(cfgpacket)) && !all)
    return 0;

  if ((IS_MAXIMIZED(cfgpacket)) && !maximized)
    return 0;

  if ((IS_STICKY(cfgpacket)) && !sticky)
    return 0;

  if (!XGetWindowAttributes(dpy, cfgpacket->w, &xwa))
    return 0;

  if (xwa.map_state != IsViewable)
    return 0;

  if (!(IS_MAPPED(cfgpacket)))
    return 0;

  if (IS_ICONIFIED(cfgpacket))
    return 0;

  if (!desk)
  {
    int x = (int)cfgpacket->frame_x, y = (int)cfgpacket->frame_y;
    int w = (int)cfgpacket->frame_width, h = (int)cfgpacket->frame_height;
    if (x >= dx + dwidth || y >= dy + dheight || x + w <= dx || y + h <= dy)
      return 0;
  }

  if (!(HAS_TITLE(cfgpacket)) && !untitled)
    return 0;

  if ((IS_TRANSIENT(cfgpacket)) && !transients)
    return 0;

  return 1;
}

int get_window(void)
{
  FvwmPacket* packet;
  struct ConfigWinPacket  *cfgpacket;
  int last = 0;
  fd_set infds;

  FD_ZERO(&infds);
  FD_SET(fd[1], &infds);
  select(fd_width, SELECT_FD_SET_CAST &infds, 0, 0, NULL);

  if ( (packet = ReadFvwmPacket(fd[1])) == NULL )
    DeadPipe(0);
  else {
    cfgpacket = (struct ConfigWinPacket*) packet->body;
    switch (packet->type) {
    case M_CONFIGURE_WINDOW:
      if (is_suitable_window(packet->body)) {
	window_item *wi =
	  (window_item*)safemalloc(sizeof( window_item ));
	wi->frame = cfgpacket->frame;
	wi->th = cfgpacket->title_height;
	wi->bw = cfgpacket->border_width;
	wi->width = cfgpacket->frame_width;
	wi->height = cfgpacket->frame_height;
	if (!wins_tail) wins_tail = wi;
	insert_window_list(&wins, wi);
	++wins_count;
      }
      last = 1;
      break;

    case M_END_WINDOWLIST:
      break;

    default:
      fprintf(console,
	      "%s: internal inconsistency: unknown message\n",
	      argv0);
      break;
    }
  }
  return last;
}

void wait_configure(window_item *wi)
{
  int found = 0;

  /** Uh, what's the point of the select() here?? **/
  fd_set infds;
  FD_ZERO(&infds);
  FD_SET(fd[1], &infds);
  select(fd_width, SELECT_FD_SET_CAST &infds, 0, 0, NULL);

  while (!found) {
    FvwmPacket* packet = ReadFvwmPacket(fd[1]);
    if ( packet == NULL )
      DeadPipe(0);
    if ( packet->type == M_CONFIGURE_WINDOW
	 && (Window)(packet->body[1]) == wi->frame )
      found = 1;
  }
}

int atopixel(char *s, unsigned long f)
{
  int l = strlen(s);
  if (l < 1) return 0;
  if (isalpha(s[l - 1])) {
    char s2[24];
    strcpy(s2,s);
    s2[strlen(s2) - 1] = 0;
    return atoi(s2);
  }
  return (atoi(s) * f) / 100;
}

void tile_windows(void)
{
  char msg[128];
  int cur_x = ofsx, cur_y = ofsy;
  int wdiv, hdiv, i, j, count = 1;
  window_item *w = reversed ? wins_tail : wins;

  if (horizontal) {
    if ((maxnum > 0) && (maxnum < wins_count)) {
      count = wins_count / maxnum;
      if (wins_count % maxnum) ++count;
      hdiv = (maxy - ofsy + 1) / maxnum;
    } else {
      maxnum = wins_count;
      hdiv = (maxy - ofsy + 1) / wins_count;
    }
    wdiv = (maxx - ofsx + 1) / count;

    for (i = 0; w && (i < count); ++i)  {
      for (j = 0; w && (j < maxnum); ++j) {
	int nw = wdiv - w->bw * 2;
	int nh = hdiv - w->bw * 2 - w->th;

	if (resize) {
	  if (nostretch) {
	    if (nw > w->width)
	      nw = w->width;
	    if (nh > w->height)
	      nh = w->height;
	  }
	  sprintf(msg, "Resize %lup %lup",
		  (nw > 0) ? nw : w->width,
		  (nh > 0) ? nh : w->height);
	  SendInfo(fd,msg,w->frame);
	}
	sprintf(msg, "Move %up %up", cur_x, cur_y);
	SendInfo(fd,msg,w->frame);
	if (raise_window)
	  SendInfo(fd,"Raise",w->frame);
	cur_y += hdiv;
	wait_configure(w);
	w = reversed ? w->prev : w->next;
      }
      cur_x += wdiv;
      cur_y = ofsy;
    }
  } else  {
    if ((maxnum > 0) && (maxnum < wins_count)) {
      count = wins_count / maxnum;
      if (wins_count % maxnum) ++count;
      wdiv = (maxx - ofsx + 1) / maxnum;
    } else {
      maxnum = wins_count;
      wdiv = (maxx - ofsx + 1) / wins_count;
    }
    hdiv = (maxy - ofsy + 1) / count;

    for (i = 0; w && (i < count); ++i)  {
      for (j = 0; w && (j < maxnum); ++j) {
	int nw = wdiv - w->bw * 2;
	int nh = hdiv - w->bw * 2 - w->th;

	if (resize) {
	  if (nostretch) {
	    if (nw > w->width)
	      nw = w->width;
	    if (nh > w->height)
	      nh = w->height;
	  }
	  sprintf(msg, "Resize %lup %lup",
		  (nw > 0) ? nw : w->width,
		  (nh > 0) ? nh : w->height);
	  SendInfo(fd,msg,w->frame);
	}
	sprintf(msg, "Move %up %up", cur_x, cur_y);
	SendInfo(fd,msg,w->frame);
	if (raise_window)
	  SendInfo(fd,"Raise",w->frame);
	cur_x += wdiv;
	wait_configure(w);
	w = reversed ? w->prev : w->next;
      }
      cur_x = ofsx;
      cur_y += hdiv;
    }
  }
}

void cascade_windows(void)
{
  char msg[128];
  int cur_x = ofsx, cur_y = ofsy;
  window_item *w = reversed ? wins_tail : wins;
  while (w)
  {
    unsigned long nw = 0, nh = 0;
    if (raise_window)
      SendInfo(fd,"Raise",w->frame);
    sprintf(msg, "Move %up %up", cur_x, cur_y);
    SendInfo(fd,msg,w->frame);
    if (resize) {
      if (nostretch) {
	if (maxw
	    && (w->width > maxw))
	  nw = maxw;
	if (maxh
	    && (w->height > maxh))
	  nh = maxh;
      } else {
	nw = maxw;
	nh = maxh;
      }
      if (nw || nh) {
	sprintf(msg, "Resize %lup %lup",
		nw ? nw : w->width,
		nh ? nh : w->height);
	SendInfo(fd,msg,w->frame);
      }
    }
    wait_configure(w);
    if (!flatx)
      cur_x += w->bw;
    cur_x += incx;
    if (!flaty)
      cur_y += w->bw + w->th;
    cur_y += incy;
    w = reversed ? w->prev : w->next;
  }
}

void parse_args(char *s, int argc, char *argv[], int argi)
{
  int nsargc = 0;
  /* parse args */
  for (; argi < argc; ++argi)
  {
    if (!strcmp(argv[argi],"-tile") || !strcmp(argv[argi],"-cascade")) {
      /* ignore */
    }
    else if (!strcmp(argv[argi],"-u")) {
      untitled = 1;
    }
    else if (!strcmp(argv[argi],"-t")) {
      transients = 1;
    }
    else if (!strcmp(argv[argi], "-a")) {
      all = untitled = transients = maximized = 1;
      if (FvwmCascade)
	sticky = 1;
    }
    else if (!strcmp(argv[argi], "-r")) {
      reversed = 1;
    }
    else if (!strcmp(argv[argi], "-noraise")) {
      raise_window = 0;
    }
    else if (!strcmp(argv[argi], "-noresize")) {
      resize = 0;
    }
    else if (!strcmp(argv[argi], "-nostretch")) {
      nostretch = 1;
    }
    else if (!strcmp(argv[argi], "-desk")) {
      desk = 1;
    }
    else if (!strcmp(argv[argi], "-flatx")) {
      flatx = 1;
    }
    else if (!strcmp(argv[argi], "-flaty")) {
      flaty = 1;
    }
    else if (!strcmp(argv[argi], "-r")) {
      reversed = 1;
    }
    else if (!strcmp(argv[argi], "-h")) {
      horizontal = 1;
    }
    else if (!strcmp(argv[argi], "-m")) {
      maximized = 1;
    }
    else if (!strcmp(argv[argi], "-s")) {
      sticky = 1;
    }
    else if (!strcmp(argv[argi], "-mn") && ((argi + 1) < argc)) {
      maxnum = atoi(argv[++argi]);
    }
    else if (!strcmp(argv[argi], "-resize")) {
      resize = 1;
    }
    else if (!strcmp(argv[argi], "-nostretch")) {
      nostretch = 1;
    }
    else if (!strcmp(argv[argi], "-incx") && ((argi + 1) < argc)) {
      incx = atopixel(argv[++argi], dwidth);
    }
    else if (!strcmp(argv[argi], "-incy") && ((argi + 1) < argc)) {
      incy = atopixel(argv[++argi], dheight);
    }
    else {
      if (++nsargc > 4) {
	fprintf(console,
		"%s: %s: ignoring unknown arg %s\n",
		argv0, s, argv[argi]);
	continue;
      }
      if (nsargc == 1) {
	ofsx = atopixel(argv[argi], dwidth);
      } else if (nsargc == 2) {
	ofsy = atopixel(argv[argi], dheight);
      } else if (nsargc == 3) {
	if (FvwmCascade)
	  maxw = atopixel(argv[argi], dwidth);
	else /* FvwmTile */
	  maxx = atopixel(argv[argi], dwidth);
      } else if (nsargc == 4) {
	if (FvwmCascade)
	  maxh = atopixel(argv[argi], dheight);
	else /* FvwmTile */
	  maxy = atopixel(argv[argi], dheight);
      }
    }
  }
  ofsx += dx;
  ofsy += dy;
  maxx += dx;
  maxy += dy;
}

int main(int argc, char *argv[])
{
  char match[128];
  int len;
  char *config_line;
  int scr;

  console = fopen("/dev/console","w");
  if (!console) console = stderr;

  if (!(argv0 = strrchr(argv[0],'/')))
    argv0 = argv[0];
  else
    ++argv0;

  if (argc < 6) {
    fprintf(stderr,
	    "%s: module should be executed by fvwm only\n",
	    argv0);
    exit(-1);
  }

  fd[0] = atoi(argv[1]);
  fd[1] = atoi(argv[2]);

  if (!(dpy = XOpenDisplay(NULL))) {
    fprintf(console, "%s: couldn't open display %s\n",
	    argv0,
	    XDisplayName(NULL));
    exit(-1);
  }
  signal (SIGPIPE, DeadPipe);

  FScreenInit(dpy);
  scr = DefaultScreen(dpy);
  fd_width = GetFdWidth();

  strcpy(match, "*");
  strcat(match, argv0);
  len = strlen(match);
  InitGetConfigLine(fd,match);
  GetConfigLine(fd, &config_line);
  while (config_line != NULL)
  {
    if (strncasecmp(config_line, XINERAMA_CONFIG_STRING,
		    sizeof(XINERAMA_CONFIG_STRING) - 1) == 0)
    {
      FScreenConfigureModule(
	config_line + sizeof(XINERAMA_CONFIG_STRING) - 1);
    }
    GetConfigLine(fd, &config_line);
  }
  FScreenGetScrRect(NULL, FSCREEN_CURRENT, &dx, &dy, &dwidth, &dheight);

  if (strcmp(argv0, "FvwmCascade") &&
      (!strcmp(argv0, "FvwmTile") ||
       (argc >= 7 && !strcmp(argv[6], "-tile")))) {
    FvwmTile = 1;
    FvwmCascade = 0;
    resize = 1;
  } else {
    FvwmCascade = 1;
    FvwmTile = 0;
    resize = 0;
  }
  parse_args("module args", argc, argv, 6);

  SetMessageMask(fd,
		 M_CONFIGURE_WINDOW |
		 M_END_WINDOWLIST);

  if (FvwmTile) {
    if (maxx == dx)
      maxx = dx + dwidth;
    if (maxy == dy)
      maxy = dy + dheight;
  }

  SendInfo(fd,"Send_WindowList",0);

  /* tell fvwm we're running */
  SendFinishedStartupNotification(fd);

  while (get_window()) /* */;
  if (wins_count) {
    if (FvwmCascade)
      cascade_windows();
    else /* FvwmTile */
      tile_windows();
  }
  free_window_list(wins);

  if (console != stderr)
    fclose(console);

  return 0;
}
