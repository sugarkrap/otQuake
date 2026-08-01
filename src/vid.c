/*
Copyright (C) 1996-1997 Id Software, Inc.

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/
// vid_x.c -- general x video driver -> ported to use Qt instead now!
// Modified by John Ryland, Copyright 2001 Trolltech.

extern void CreateQtWindow(void);
extern void KillQtApp(void);
extern void RepaintQtWindow(void);
extern void RepaintQtWindowRect(int x, int y, int w, int h);
extern void DoQtEventLoop(void);
extern void ProcessOneQtEvent(void);
extern void SetQtPalette(unsigned char *palette);

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "quakedef.h"
#include "d_local.h"

long long FPM_TMPVAR_INT64;

cvar_t		_windowed_mouse = {"_windowed_mouse","0", true};
cvar_t		m_filter = {"m_filter","0", true};
//float old_windowed_mouse;
qboolean        mouse_avail;
int             mouse_buttons=3;
int             mouse_oldbuttonstate;
int             mouse_buttonstate;
float   mouse_x, mouse_y;
float   old_mouse_x, old_mouse_y;
int p_mouse_x;
int p_mouse_y;
int bits_per_pixel;

typedef struct
{
	int input;
	int output;
} keymap_t;


void S_StartSoundFPM (int entnum, int entchannel, sfx_t *sfx, vec3_FPM_t origin, float fvol,  float attenuation)
{
}

extern viddef_t vid; // global video state
unsigned short d_8to16table[256]; // some global used else where
int vid_buffersize;
static long X11_highhunkmark;
static long X11_buffersize;
int vid_surfcachesize;
void *vid_surfcache;
int min_vid_width = 320;
float start_yaw = 0, yaw_modifier = 0;


// Called at startup to set up translation tables, takes 256 8 bit RGB values
// the palette data will go away after the call, so it must be copied off if
// the video driver will need it again

void VID_Init (unsigned char *palette)
{
   int pnum, i;

CreateQtWindow();
SetQtPalette(palette);
  
   vid.width = 320;
   vid.height = 240;
//   vid.width = 238;
//   vid.height = 180;
   vid.maxwarpwidth = WARP_WIDTH;
   vid.maxwarpheight = WARP_HEIGHT;
   vid.numpages = 2;
//   vid.numpages = 1;
   vid.colormap = host_colormap;
   vid.fullbright = 256 - LittleLong (*((int *)vid.colormap + 2048));
   
	srandom(getpid());

// check for command-line window size
	if ((pnum=COM_CheckParm("-winsize")))
	{
		if (pnum >= com_argc-2)
			Sys_Error("VID: -winsize <width> <height>\n");
		vid.width = Q_atoi(com_argv[pnum+1]);
		vid.height = Q_atoi(com_argv[pnum+2]);
		if (!vid.width || !vid.height)
			Sys_Error("VID: Bad window width/height\n");
	}
	if ((pnum=COM_CheckParm("-width"))) {
		if (pnum >= com_argc-1)
			Sys_Error("VID: -width <width>\n");
		vid.width = Q_atoi(com_argv[pnum+1]);
		if (!vid.width)
			Sys_Error("VID: Bad window width\n");
	}
	if ((pnum=COM_CheckParm("-height"))) {
		if (pnum >= com_argc-1)
			Sys_Error("VID: -height <height>\n");
		vid.height = Q_atoi(com_argv[pnum+1]);
		if (!vid.height)
			Sys_Error("VID: Bad window height\n");
	}

	if (d_pzbuffer)
	{
		D_FlushCaches ();
		Hunk_FreeToHighMark (X11_highhunkmark);
		d_pzbuffer = NULL;
	}

	X11_highhunkmark = Hunk_HighMark();

// alloc an extra line in case we want to wrap, and allocate the z-buffer
	X11_buffersize = vid.width * vid.height * sizeof (*d_pzbuffer);

	vid_surfcachesize = D_SurfaceCacheForRes(vid.width, vid.height);

	X11_buffersize += vid_surfcachesize;

	d_pzbuffer = (short int *)Hunk_HighAllocName(X11_buffersize, "video");
	if (d_pzbuffer == NULL)
		Sys_Error ("Not enough memory for video mode\n");

	vid_surfcache = (byte *) d_pzbuffer
		+ vid.width * vid.height * sizeof (*d_pzbuffer);

#ifdef USEFPM
	D_InitCachesFPM(vid_surfcache, vid_surfcachesize);
#else
	D_InitCaches(vid_surfcache, vid_surfcachesize);
#endif

	vid.buffer = (pixel_t *)malloc( ((vid.width*2+7)&~7) * vid.height );

	if (!vid.buffer)
		Sys_Error("VID: XCreateImage failed\n");

	vid.rowbytes = vid.width;
	vid.direct = 0;
	vid.conbuffer = vid.buffer;
	vid.conrowbytes = vid.rowbytes;
	vid.conwidth = vid.width;
	vid.conheight = vid.height;
	vid.aspect = ((float)vid.height / (float)vid.width) * (320.0 / 240.0);

	if (vid.width<320) min_vid_width=vid.width;
	else min_vid_width=320;
}


/* 
void GetEvent(void)
{
	case KeyPress:
		keyq[keyq_head].key = XLateKey(&x_event.xkey);
		keyq[keyq_head].down = true;
		keyq_head = (keyq_head + 1) & 63;
		break;
	case KeyRelease:
		keyq[keyq_head].key = XLateKey(&x_event.xkey);
		keyq[keyq_head].down = false;
		keyq_head = (keyq_head + 1) & 63;
		break;
}
*/



void VID_ShiftPalette(unsigned char *p) { SetQtPalette(p); }
void VID_SetPalette(unsigned char *p) { SetQtPalette(p); }
// Called at shutdown
void VID_Shutdown(void) { KillQtApp(); }
// flushes the given rectangles from the view buffer to the screen
/* Quake hands us the rectangle that actually changed; vid_fb uses it to skip
 * repainting untouched scanlines (the blit is ~60% of the frame on the
 * SL-C860). A NULL/empty list means "everything". */
void VID_Update(vrect_t *rects)
{
    if (rects)
        RepaintQtWindowRect (rects->x, rects->y, rects->width, rects->height);
    else
        RepaintQtWindow();
}
void VID_DitherOn(void) { }
void VID_DitherOff(void) { }
int Sys_OpenWindow(void) { return 0; }
void Sys_EraseWindow(int window) { }
void Sys_DrawCircle(int window, int x, int y, int r) { }
void Sys_DisplayWindow(int window) { }
void D_BeginDirectRect (int x, int y, byte *pbitmap, int width, int height) { }
void D_EndDirectRect (int x, int y, int width, int height) { }

void Sys_SendKeyEvents(void)
{
    ProcessOneQtEvent();

/*
// get events from x server
	if (x_disp)
	{
		while (XPending(x_disp))
		    GetEvent();

		while (keyq_head != keyq_tail)
		{
			Key_Event(keyq[keyq_tail].key, keyq[keyq_tail].down);
			keyq_tail = (keyq_tail + 1) & 63;
		}
	}
*/
}


void IN_Init (void)
{
	Cvar_RegisterVariable (&_windowed_mouse);
	Cvar_RegisterVariable (&m_filter);
   if ( COM_CheckParm ("-nomouse") )
     return;
   mouse_x = mouse_y = 0.0;
   mouse_avail = 1;
}

void IN_Shutdown (void)
{
   mouse_avail = 0;
}

void IN_Commands (void)
{
	int i;
   
	if (!mouse_avail) return;
   
	for (i=0 ; i<mouse_buttons ; i++) {
		if ( (mouse_buttonstate & (1<<i)) && !(mouse_oldbuttonstate & (1<<i)) )
			Key_Event (K_MOUSE1 + i, true);

		if ( !(mouse_buttonstate & (1<<i)) && (mouse_oldbuttonstate & (1<<i)) )
			Key_Event (K_MOUSE1 + i, false);
	}
	mouse_oldbuttonstate = mouse_buttonstate;
}

void IN_Move (usercmd_t *cmd)
{
	if (!mouse_avail)
		return;
   
	if (m_filter.value) {
		mouse_x = (mouse_x + old_mouse_x) * 0.5;
		mouse_y = (mouse_y + old_mouse_y) * 0.5;
	}

	old_mouse_x = mouse_x;
	old_mouse_y = mouse_y;
   
	mouse_x *= sensitivity.value;
	mouse_y *= sensitivity.value;
   
	if ( (in_strafe.state & 1) || (lookstrafe.value && (in_mlook.state & 1) ))
		cmd->sidemove += m_side.value * mouse_x;
	else
		cl.viewangles[YAW] -= m_yaw.value * mouse_x;
	if (in_mlook.state & 1)
		V_StopPitchDrift ();
   
	if ( (in_mlook.state & 1) && !(in_strafe.state & 1)) {
		cl.viewangles[PITCH] += m_pitch.value * mouse_y;
		if (cl.viewangles[PITCH] > 80)
			cl.viewangles[PITCH] = 80;
		if (cl.viewangles[PITCH] < -70)
			cl.viewangles[PITCH] = -70;
	} else {
		if ((in_strafe.state & 1) && noclip_anglehack)
			cmd->upmove -= m_forward.value * mouse_y;
		else
			cmd->forwardmove -= m_forward.value * mouse_y;
	}
	mouse_x = mouse_y = 0.0;
}

