/**
 * \file src/play/audio_play.c
 * \brief audio playback
 * \author Pyry Haulos <pyry.haulos@gmail.com>
 * \date 2007
 * For conditions of distribution and use, see copyright notice in glc.h
 */

/**
 * \addtogroup play
 *  \{
 * \defgroup audio_play audio playback
 *  \{
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <packetstream.h>
#include <alsa/asoundlib.h>
#include <errno.h>
#include <sched.h>

#include <glc/common/glc.h>
#include <glc/common/core.h>
#include <glc/common/log.h>
#include <glc/common/util.h>
#include <glc/common/state.h>
#include <glc/common/thread.h>

#include "audio_play.h"

struct audio_play_s {
	glc_t *glc;
	glc_thread_t thread;
	int running;

	glc_utime_t silence_threshold;

	glc_audio_i audio_i;
	snd_pcm_t *pcm;
	const char *device;
	
	unsigned int channels;
	unsigned int rate;
	glc_flags_t flags;

	int fmt;

	void **bufs;
};

int audio_play_read_callback(glc_thread_state_t *state);
void audio_play_finish_callback(void *priv, int err);

int audio_play_hw(audio_play_t audio_play, glc_audio_format_message_t *fmt_msg);
int audio_play_play(audio_play_t audio_play, glc_audio_header_t *audio_msg, char *data);

snd_pcm_format_t glc_fmt_to_pcm_fmt(glc_flags_t flags);

int audio_play_xrun(audio_play_t audio_play, int err);

snd_pcm_format_t glc_fmt_to_pcm_fmt(glc_flags_t flags)
{
	if (flags & GLC_AUDIO_S16_LE)
		return SND_PCM_FORMAT_S16_LE;
	else if (flags & GLC_AUDIO_S24_LE)
		return SND_PCM_FORMAT_S24_LE;
	else if (flags & GLC_AUDIO_S32_LE)
		return SND_PCM_FORMAT_S32_LE;
	return 0;
}

int audio_play_init(audio_play_t *audio_play, glc_t *glc)
{
	*audio_play = (audio_play_t) malloc(sizeof(struct audio_play_s));
	memset(*audio_play, 0, sizeof(struct audio_play_s));

	(*audio_play)->glc = glc;
	(*audio_play)->device = "default";
	(*audio_play)->audio_i = 1;
	(*audio_play)->silence_threshold = 200000; /** \todo make configurable? */

	(*audio_play)->thread.flags = GLC_THREAD_READ;
	(*audio_play)->thread.ptr = *audio_play;
	(*audio_play)->thread.read_callback = &audio_play_read_callback;
	(*audio_play)->thread.finish_callback = &audio_play_finish_callback;
	(*audio_play)->thread.threads = 1;

	return 0;
}

int audio_play_destroy(audio_play_t audio_play)
{
	free(audio_play);
	return 0;
}

int audio_play_set_alsa_playback_device(audio_play_t audio_play, const char *device)
{
	audio_play->device = device;
	return 0;
}

int audio_play_set_stream_number(audio_play_t audio_play, glc_audio_i audio)
{
	audio_play->audio_i = audio;
	return 0;
}

int audio_play_process_start(audio_play_t audio_play, ps_buffer_t *from)
{
	int ret;
	if (audio_play->running)
		return EAGAIN;

	if ((ret = glc_thread_create(audio_play->glc, &audio_play->thread, from, NULL)))
		return ret;
	audio_play->running = 1;

	return 0;
}

int audio_play_process_wait(audio_play_t audio_play)
{
	if (!audio_play->running)
		return EAGAIN;

	glc_thread_wait(&audio_play->thread);
	audio_play->running = 0;

	return 0;
}

void audio_play_finish_callback(void *priv, int err)
{
	audio_play_t audio_play = (audio_play_t) priv;

	if (err)
		glc_log(audio_play->glc, GLC_ERROR, "audio_play", "%s (%d)",
			 strerror(err), err);
	
	if (audio_play->pcm) {
		snd_pcm_close(audio_play->pcm);
		audio_play->pcm = NULL;
	}

	if (audio_play->bufs) {
		free(audio_play->bufs);
		audio_play->bufs = NULL;
	}
}

int audio_play_read_callback(glc_thread_state_t *state)
{
	audio_play_t audio_play = (audio_play_t ) state->ptr;

	if (state->header.type == GLC_MESSAGE_AUDIO_FORMAT)
		return audio_play_hw(audio_play, (glc_audio_format_message_t *) state->read_data);
	else if (state->header.type == GLC_MESSAGE_AUDIO)
		return audio_play_play(audio_play, (glc_audio_header_t *) state->read_data,
				       &state->read_data[GLC_AUDIO_HEADER_SIZE]);
	
	return 0;
}

int audio_play_hw(audio_play_t audio_play, glc_audio_format_message_t *fmt_msg)
{
	snd_pcm_hw_params_t *hw_params = NULL;
	snd_pcm_access_t access;
	snd_pcm_uframes_t max_buffer_size;
	unsigned int min_periods;
	int dir, ret = 0;

	if (fmt_msg->audio != audio_play->audio_i)
		return 0;

	audio_play->flags = fmt_msg->flags;
	audio_play->rate = fmt_msg->rate;
	audio_play->channels = fmt_msg->channels;

	if (audio_play->pcm) /* re-open */
		snd_pcm_close(audio_play->pcm);

	if (audio_play->flags & GLC_AUDIO_INTERLEAVED)
		access = SND_PCM_ACCESS_RW_INTERLEAVED;
	else
		access = SND_PCM_ACCESS_RW_NONINTERLEAVED;

	if ((ret = snd_pcm_open(&audio_play->pcm, audio_play->device,
				SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK)) < 0)
		goto err;
	if ((ret = snd_pcm_hw_params_malloc(&hw_params)) < 0)
		goto err;
	if ((ret = snd_pcm_hw_params_any(audio_play->pcm, hw_params)) < 0)
		goto err;
	if ((ret = snd_pcm_hw_params_set_access(audio_play->pcm,
						hw_params, access)) < 0)
		goto err;
	if ((ret = snd_pcm_hw_params_set_format(audio_play->pcm, hw_params,
						glc_fmt_to_pcm_fmt(audio_play->flags))) < 0)
		goto err;
	if ((ret = snd_pcm_hw_params_set_channels(audio_play->pcm, hw_params,
						  audio_play->channels)) < 0)
		goto err;
	if ((ret = snd_pcm_hw_params_set_rate(audio_play->pcm, hw_params,
					      audio_play->rate, 0)) < 0)
		goto err;
	if ((ret = snd_pcm_hw_params_get_buffer_size_max(hw_params,
							 &max_buffer_size)) < 0)
		goto err;
	if ((ret = snd_pcm_hw_params_set_buffer_size(audio_play->pcm,
						     hw_params, max_buffer_size)) < 0)
		goto err;
	if ((ret = snd_pcm_hw_params_get_periods_min(hw_params, &min_periods, &dir)) < 0)
		goto err;
	if ((ret = snd_pcm_hw_params_set_periods(audio_play->pcm, hw_params,
						 min_periods < 2 ? 2 : min_periods, dir)) < 0)
		goto err;
	if ((ret = snd_pcm_hw_params(audio_play->pcm, hw_params)) < 0)
		goto err;

	audio_play->bufs = (void **) malloc(sizeof(void *) * audio_play->channels);

	snd_pcm_hw_params_free(hw_params);
	return 0;
err:
	glc_log(audio_play->glc, GLC_ERROR, "audio_play", "can't initialize pcm: %s (%d)",
		 snd_strerror(ret), ret);
	if (hw_params)
		snd_pcm_hw_params_free(hw_params);
	return -ret;
}

int audio_play_play(audio_play_t audio_play, glc_audio_header_t *audio_hdr, char *data)
{
	snd_pcm_uframes_t frames, rem;
	snd_pcm_sframes_t ret = 0;
	unsigned int c;

	if (audio_hdr->audio != audio_play->audio_i)
		return 0;

	if (!audio_play->pcm) {
		glc_log(audio_play->glc, GLC_ERROR, "audio_play", "broken stream %d",
			 audio_play->audio_i);
		return EINVAL;
	}
	
	frames = snd_pcm_bytes_to_frames(audio_play->pcm, audio_hdr->size);
	glc_utime_t time = glc_state_time(audio_play->glc);
	glc_utime_t duration = (1000000 * frames) / audio_play->rate;
	
	if (time + audio_play->silence_threshold + duration < audio_hdr->timestamp)
		usleep(audio_hdr->timestamp - time - duration);
	else if (time > audio_hdr->timestamp) {
		glc_log(audio_play->glc, GLC_WARNING, "audio_play", "dropped packet");
		return 0;
	}

	rem = frames;

	while (rem > 0) {
		/* alsa is horrible... */
		snd_pcm_wait(audio_play->pcm, duration);
		
		if (audio_play->flags & GLC_AUDIO_INTERLEAVED)
			ret = snd_pcm_writei(audio_play->pcm,
					    &data[snd_pcm_frames_to_bytes(audio_play->pcm, frames - rem)],
					    rem);
		else {
			for (c = 0; c < audio_play->channels; c++)
				audio_play->bufs[c] =
					&data[snd_pcm_samples_to_bytes(audio_play->pcm, frames)
					      * c + snd_pcm_samples_to_bytes(audio_play->pcm, frames - rem)];
			ret = snd_pcm_writen(audio_play->pcm, audio_play->bufs, rem);
		}

		if (ret == 0)
			break;

		if ((ret == -EBUSY) | (ret == -EAGAIN))
			break;
		else if (ret < 0) {
			if ((ret = audio_play_xrun(audio_play, ret))) {
				glc_log(audio_play->glc, GLC_ERROR, "audio_play",
					 "xrun recovery failed: %s", snd_strerror(-ret));
				return ret;
			}
		} else
			rem -= ret;
	}

	return 0;
}

int audio_play_xrun(audio_play_t audio_play, int err)
{
	glc_log(audio_play->glc, GLC_DEBUG, "audio_play", "xrun");
	if (err == -EPIPE) {
		if ((err = snd_pcm_prepare(audio_play->pcm)) < 0)
			return -err;
		return 0;
	} else if (err == -ESTRPIPE) {
		while ((err = snd_pcm_resume(audio_play->pcm)) == -EAGAIN)
			sched_yield();
		if (err < 0) {
			if ((err = snd_pcm_prepare(audio_play->pcm)) < 0)
				return -err;
			return 0;
		}
	}
	return -err;
}

/**  \} */
/**  \} */