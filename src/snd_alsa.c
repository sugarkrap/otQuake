/*
 * snd_alsa.c -- host-only ALSA sound backend, for local testing
 *
 * The real target (the Zaurus) uses snd_sun.c's mmap'd /dev/dsp (OSS).
 * A modern Linux desktop has no /dev/dsp at all, and PulseAudio/PipeWire's
 * OSS emulation (padsp) doesn't support the mmap access snd_sun.c
 * requires, so that driver just refuses to start here.
 *
 * This is a plain blocking-write ALSA driver that lets otQuake run (and
 * be heard) directly on the build machine, wired in only by the
 * `make host` Makefile target -- it is not built for the real device.
 *
 * shm->buffer is our own ring buffer (not a real DMA-mapped one): the
 * mixer paints into it exactly as it would the mmap'd buffer, and
 * SNDDMA_Submit() drains newly painted frames out to ALSA each frame.
 * frames_written tracks how much of that ring has actually been handed
 * to ALSA, standing in for the hardware DMA position snd_sun.c would
 * otherwise read back from the kernel.
 */
#include <alsa/asoundlib.h>
#include "quakedef.h"

#define SND_ALSA_RING_SAMPLES	(1 << 15)	// power of 2: required by the mixer's ring math

static snd_pcm_t	*pcm;
static byte		alsa_ring[SND_ALSA_RING_SAMPLES * 2];	// *2 for 16-bit samples
static long		frames_written;				// total frames ever handed to ALSA

qboolean SNDDMA_Init (void)
{
	int	rc;
	int	channels, rate;
	char	*s;
	int	i;

	channels = 2;
	rate = 11025;

	s = getenv ("QUAKE_SOUND_CHANNELS");
	if (s)
		channels = atoi (s);
	else if (COM_CheckParm ("-sndmono"))
		channels = 1;
	else if (COM_CheckParm ("-sndstereo"))
		channels = 2;

	s = getenv ("QUAKE_SOUND_SPEED");
	if (s)
		rate = atoi (s);
	else if ((i = COM_CheckParm ("-sndspeed")) != 0)
		rate = atoi (com_argv[i+1]);

	rc = snd_pcm_open (&pcm, "default", SND_PCM_STREAM_PLAYBACK, 0);
	if (rc < 0)
	{
		Con_Printf ("snd_alsa: snd_pcm_open failed: %s\n", snd_strerror (rc));
		pcm = NULL;
		return false;
	}

	rc = snd_pcm_set_params (pcm, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED,
		channels, rate, 1, 100000 /* 100ms latency */);
	if (rc < 0)
	{
		Con_Printf ("snd_alsa: snd_pcm_set_params failed: %s\n", snd_strerror (rc));
		snd_pcm_close (pcm);
		pcm = NULL;
		return false;
	}

	frames_written = 0;

	shm = &sn;
	memset ((void *)shm, 0, sizeof(*shm));
	shm->splitbuffer = 0;
	shm->samplebits = 16;
	shm->speed = rate;
	shm->channels = channels;
	shm->samples = SND_ALSA_RING_SAMPLES;
	shm->samplepos = 0;
	shm->submission_chunk = 1;
	shm->buffer = alsa_ring;
	shm->soundalive = true;
	shm->gamealive = true;

	Con_Printf ("snd_alsa: %d Hz, %d channel(s), 16-bit via ALSA \"default\"\n", rate, channels);

	return true;
}

int SNDDMA_GetDMAPos (void)
{
	if (!pcm)
		return 0;

	shm->samplepos = (int)((frames_written * shm->channels) % shm->samples);
	return shm->samplepos;
}

void SNDDMA_Shutdown (void)
{
	if (pcm)
	{
		snd_pcm_close (pcm);
		pcm = NULL;
	}
}

/*
==============
SNDDMA_Submit

Drains everything S_PaintChannels has painted since the last call out to
ALSA, blocking as needed. Recovers from underruns (XRUN) since our timing
isn't hardware-precise like the mmap driver's.
===============
*/
void SNDDMA_Submit (void)
{
	int			ring_frames, frame_bytes;
	int			pending, start_frame, avail_to_end, chunk;
	snd_pcm_sframes_t	written;

	if (!pcm)
		return;

	pending = paintedtime - (int)frames_written;
	if (pending <= 0)
		return;

	ring_frames = shm->samples / shm->channels;
	frame_bytes = shm->channels * (shm->samplebits / 8);

	while (pending > 0)
	{
		start_frame = (int)(frames_written % ring_frames);
		avail_to_end = ring_frames - start_frame;
		chunk = pending < avail_to_end ? pending : avail_to_end;

		written = snd_pcm_writei (pcm, shm->buffer + (long)start_frame * frame_bytes, chunk);
		if (written < 0)
		{
			written = snd_pcm_recover (pcm, (int)written, 1);
			if (written < 0)
			{
				Con_Printf ("snd_alsa: write failed: %s\n", snd_strerror ((int)written));
				return;
			}
			continue;	// recovered; retry the same chunk
		}

		frames_written += written;
		pending -= (int)written;
	}
}
