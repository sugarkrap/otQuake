/*
 * cd_wav.c -- CD audio emulation for handheldquake
 *
 * The Zaurus has no CD drive, so "CD tracks" are read from plain PCM WAV
 * files (music/trackNN.wav, matching the CD track numbers Quake's maps
 * already reference) and streamed straight into the same mix buffer the
 * sound effects use (see the CDAudio_MixMusic() call from S_PaintChannels
 * in snd_mix.c).
 *
 * There is no decoder and no resampler here on purpose: this hardware has
 * no FPU, so tracks must already be stored at the exact rate/channels/bits
 * the mixer is running (16-bit PCM, matching -sndspeed/-sndmono/-sndstereo).
 * A mismatched file is refused with a console message rather than played
 * back at the wrong pitch or garbled.
 */
#include "quakedef.h"

#define CD_MUSIC_DIR	"music"
#define CD_READ_FRAMES	256		// frames per fread() batch while mixing

typedef struct
{
	FILE	*file;
	long	data_start;	// absolute offset of the first PCM byte
	long	data_end;	// absolute offset one past the last PCM byte
	int	channels;
	int	bits;
	int	rate;
} cdtrack_t;

static cdtrack_t	cd_track;
static qboolean		cd_enabled = true;
static qboolean		cd_playing = false;
static qboolean		cd_paused = false;
static qboolean		cd_looping = false;
static byte		cd_curtrack = 0;

static qboolean ReadLE16 (FILE *f, unsigned short *out)
{
	unsigned char b[2];
	if (fread (b, 1, 2, f) != 2)
		return false;
	*out = (unsigned short)(b[0] | (b[1] << 8));
	return true;
}

static qboolean ReadLE32 (FILE *f, unsigned int *out)
{
	unsigned char b[4];
	if (fread (b, 1, 4, f) != 4)
		return false;
	*out = (unsigned int)b[0] | ((unsigned int)b[1] << 8) |
		((unsigned int)b[2] << 16) | ((unsigned int)b[3] << 24);
	return true;
}

static qboolean ReadTag (FILE *f, char out[4])
{
	return fread (out, 1, 4, f) == 4;
}

static void CDAudio_CloseTrack (void)
{
	if (cd_track.file)
		fclose (cd_track.file);
	memset (&cd_track, 0, sizeof(cd_track));
	cd_playing = false;
	cd_paused = false;
	cd_curtrack = 0;
}

/*
 * Walks the RIFF/WAVE chunks of an already-open file (positioned at its
 * start, which may be an offset into a shared pak file) looking for a
 * "fmt " chunk followed by a "data" chunk. On success, fills in
 * cd_track.data_start/data_end/channels/bits/rate and leaves the file
 * open; on failure the caller is responsible for closing it.
 */
static qboolean CDAudio_ParseWav (char *path, FILE *f, int filelen)
{
	long		start = ftell (f);
	long		limit = start + filelen;
	char		tag[4];
	unsigned int	chunklen;
	qboolean	have_fmt = false;
	unsigned short	format, channels, bits;
	unsigned int	rate;

	if (!ReadTag (f, tag) || strncmp (tag, "RIFF", 4) ||
		fseek (f, 4, SEEK_CUR) ||
		!ReadTag (f, tag) || strncmp (tag, "WAVE", 4))
	{
		Con_Printf ("CDAudio: %s is not a RIFF/WAVE file\n", path);
		return false;
	}

	while (ftell (f) < limit)
	{
		if (!ReadTag (f, tag) || !ReadLE32 (f, &chunklen))
			break;

		if (!strncmp (tag, "fmt ", 4))
		{
			long chunkstart = ftell (f);

			if (chunklen < 16 ||
				!ReadLE16 (f, &format) || !ReadLE16 (f, &channels) ||
				!ReadLE32 (f, &rate) ||
				fseek (f, 6, SEEK_CUR) ||	// byte rate + block align
				!ReadLE16 (f, &bits))
			{
				Con_Printf ("CDAudio: %s has a malformed fmt chunk\n", path);
				return false;
			}
			if (format != 1)
			{
				Con_Printf ("CDAudio: %s is not uncompressed PCM\n", path);
				return false;
			}
			have_fmt = true;
			fseek (f, chunkstart + (long)((chunklen + 1) & ~1u), SEEK_SET);
		}
		else if (!strncmp (tag, "data", 4))
		{
			if (!have_fmt)
			{
				Con_Printf ("CDAudio: %s has a data chunk before its fmt chunk\n", path);
				return false;
			}
			cd_track.data_start = ftell (f);
			cd_track.data_end = cd_track.data_start + (long)chunklen;
			if (cd_track.data_end > limit)
				cd_track.data_end = limit;	// truncated file, salvage what's there
			cd_track.channels = channels;
			cd_track.bits = bits;
			cd_track.rate = rate;
			return true;
		}
		else
		{
			fseek (f, ftell (f) + (long)((chunklen + 1) & ~1u), SEEK_SET);
		}
	}

	Con_Printf ("CDAudio: %s has no data chunk\n", path);
	return false;
}

static qboolean CDAudio_OpenTrack (byte track)
{
	char	path[MAX_QPATH];
	FILE	*f;
	int	filelen;

	sprintf (path, "%s/track%02d.wav", CD_MUSIC_DIR, track);

	filelen = COM_FOpenFile (path, &f);
	if (filelen == -1)
	{
		Con_DPrintf ("CDAudio: %s not found\n", path);
		return false;
	}

	memset (&cd_track, 0, sizeof(cd_track));
	cd_track.file = f;

	if (!CDAudio_ParseWav (path, f, filelen))
	{
		fclose (f);
		cd_track.file = NULL;
		return false;
	}

	if (!shm)
	{
		fclose (f);
		cd_track.file = NULL;
		return false;
	}

	if (cd_track.bits != 16 || shm->samplebits != 16)
	{
		Con_Printf ("CDAudio: %s is %d-bit but the mixer is running %d-bit -- "
			"only 16-bit PCM music is supported\n",
			path, cd_track.bits, shm->samplebits);
		fclose (f);
		cd_track.file = NULL;
		return false;
	}

	if (cd_track.channels != shm->channels)
	{
		Con_Printf ("CDAudio: %s is %s but the mixer is running %s -- "
			"convert the track, or restart with -sndmono/-sndstereo to match\n",
			path, cd_track.channels == 1 ? "mono" : "stereo",
			shm->channels == 1 ? "mono" : "stereo");
		fclose (f);
		cd_track.file = NULL;
		return false;
	}

	if (cd_track.rate != shm->speed)
	{
		Con_Printf ("CDAudio: %s is %dHz but the mixer is running at %dHz -- "
			"convert the track, or restart with -sndspeed %d\n",
			path, cd_track.rate, shm->speed, cd_track.rate);
		fclose (f);
		cd_track.file = NULL;
		return false;
	}

	return true;
}

static void CD_f (void)
{
	char	*command;

	if (Cmd_Argc () < 2)
	{
		Con_Printf ("commands: play <track>, stop, pause, resume, info\n");
		return;
	}

	command = Cmd_Argv (1);

	if (!Q_strcasecmp (command, "play") || !Q_strcasecmp (command, "loop"))
	{
		if (Cmd_Argc () < 3)
		{
			Con_Printf ("cd %s <track>\n", command);
			return;
		}
		CDAudio_Play ((byte)atoi (Cmd_Argv (2)), true);
	}
	else if (!Q_strcasecmp (command, "stop"))
		CDAudio_Stop ();
	else if (!Q_strcasecmp (command, "pause"))
		CDAudio_Pause ();
	else if (!Q_strcasecmp (command, "resume"))
		CDAudio_Resume ();
	else if (!Q_strcasecmp (command, "info"))
	{
		if (cd_playing)
			Con_Printf ("Playing track %d%s%s\n", cd_curtrack,
				cd_paused ? " (paused)" : "", cd_looping ? ", looping" : "");
		else
			Con_Printf ("Not playing a track.\n");
	}
	else
		Con_Printf ("commands: play <track>, stop, pause, resume, info\n");
}

int CDAudio_Init (void)
{
	memset (&cd_track, 0, sizeof(cd_track));

	if (COM_CheckParm ("-nocdaudio"))
		cd_enabled = false;

	Cmd_AddCommand ("cd", CD_f);

	return 0;
}

void CDAudio_Play (byte track, qboolean looping)
{
	if (!cd_enabled)
		return;

	if (cd_playing && cd_curtrack == track)
		return;		// already on this track; don't restart it

	CDAudio_CloseTrack ();

	if (track < 1)
		return;

	if (!CDAudio_OpenTrack (track))
		return;

	cd_curtrack = track;
	cd_looping = looping;
	cd_playing = true;
	cd_paused = false;
}

void CDAudio_Stop (void)
{
	CDAudio_CloseTrack ();
}

void CDAudio_Pause (void)
{
	if (cd_playing)
		cd_paused = true;
}

void CDAudio_Resume (void)
{
	if (cd_playing)
		cd_paused = false;
}

void CDAudio_Update (void)
{
}

void CDAudio_Shutdown (void)
{
	CDAudio_CloseTrack ();
}

/*
 * Called from S_PaintChannels() for every paintbuffer chunk, right
 * alongside the sound-effect channels. Adds up to `count` frames of the
 * currently playing track (scaled by bgmvolume) into `buffer`, looping or
 * stopping at the end of the data chunk as appropriate. Left untouched
 * means silence, so a track ending mid-buffer just leaves the remainder
 * as whatever the sfx channels already painted there.
 */
void CDAudio_MixMusic (portable_samplepair_t *buffer, int count)
{
	int		bgm_vol;
	int		frame_bytes;
	int		out_idx;
	unsigned char	sampbuf[CD_READ_FRAMES * 4];	// up to stereo 16-bit

	if (!cd_playing || cd_paused)
		return;

	bgm_vol = (int)(bgmvolume.value * 256);
	if (bgm_vol <= 0)
		return;

	frame_bytes = cd_track.channels * 2;
	out_idx = 0;

	while (out_idx < count)
	{
		long		cur, remain_bytes;
		int		remain_frames, batch, got, j;
		signed short	*sp;

		cur = ftell (cd_track.file);
		remain_bytes = cd_track.data_end - cur;
		remain_frames = remain_bytes > 0 ? (int)(remain_bytes / frame_bytes) : 0;

		if (remain_frames <= 0)
		{
			if (!cd_looping || cd_track.data_end <= cd_track.data_start)
			{
				CDAudio_CloseTrack ();
				break;
			}
			fseek (cd_track.file, cd_track.data_start, SEEK_SET);
			continue;
		}

		batch = count - out_idx;
		if (batch > remain_frames)
			batch = remain_frames;
		if (batch > CD_READ_FRAMES)
			batch = CD_READ_FRAMES;

		got = fread (sampbuf, frame_bytes, batch, cd_track.file);
		if (got <= 0)
		{
			if (!cd_looping)
			{
				CDAudio_CloseTrack ();
				break;
			}
			fseek (cd_track.file, cd_track.data_start, SEEK_SET);
			continue;
		}

		sp = (signed short *) sampbuf;
		if (cd_track.channels == 2)
		{
			for (j = 0; j < got; j++, out_idx++)
			{
				int l = LittleShort (sp[j*2]);
				int r = LittleShort (sp[j*2+1]);
				buffer[out_idx].left  += (l * bgm_vol) >> 8;
				buffer[out_idx].right += (r * bgm_vol) >> 8;
			}
		}
		else
		{
			for (j = 0; j < got; j++, out_idx++)
			{
				int s = (LittleShort (sp[j]) * bgm_vol) >> 8;
				buffer[out_idx].left  += s;
				buffer[out_idx].right += s;
			}
		}
	}
}
