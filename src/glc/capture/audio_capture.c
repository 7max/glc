/**
 * \file src/capture/audio_capture.c
 * \brief audio capture
 * \author Pyry Haulos <pyry.haulos@gmail.com>
 * \date 2007
 * For conditions of distribution and use, see copyright notice in glc.h
 */

/**
 * \addtogroup capture
 *  \{
 * \defgroup audio_capture audio capture
 *  \{
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sched.h>
#include <packetstream.h>
#include <alsa/asoundlib.h>
#include <pthread.h>

#include <glc/common/glc.h>
#include <glc/common/core.h>
#include <glc/common/log.h>
#include <glc/common/state.h>
#include <glc/common/util.h>

#include "audio_capture.h"

struct audio_capture_s {
	glc_t *glc;
	ps_buffer_t *to;

	glc_state_audio_t state_audio;
	glc_audio_i id;

	snd_pcm_t *pcm;
	snd_pcm_uframes_t period_size;

	glc_flags_t flags;
	const char *device;
	unsigned int channels;
	unsigned int rate;
	unsigned int min_periods;
	snd_pcm_format_t format;
	ssize_t bytes_per_frame;
	int rate_usec;
	size_t period_size_in_bytes;

	snd_async_handler_t *async_handler;

	pthread_t capture_thread;
	sem_t capture;
	int skip_data;
	int stop_capture;
};

int audio_capture_open(audio_capture_t audio_capture);
int audio_capture_init_hw(audio_capture_t audio_capture, snd_pcm_hw_params_t *hw_params);
int audio_capture_init_sw(audio_capture_t audio_capture, snd_pcm_sw_params_t *sw_params);

void audio_capture_async_callback(snd_async_handler_t *async_handler);
void *audio_capture_thread(void *argptr);

glc_flags_t audio_capture_fmt_flags(snd_pcm_format_t pcm_fmt);

int audio_capture_xrun(audio_capture_t audio_capture, int err);
int audio_capture_stop(audio_capture_t audio_capture);

int audio_capture_init(audio_capture_t *audio_capture, glc_t *glc)
{
	*audio_capture = (audio_capture_t) malloc(sizeof(struct audio_capture_s));
	memset(*audio_capture, 0, sizeof(struct audio_capture_s));
	pthread_attr_t attr;

	(*audio_capture)->glc = glc;
	(*audio_capture)->device = "default";
	(*audio_capture)->channels = 2;
	(*audio_capture)->rate = 44100;
	(*audio_capture)->min_periods = 2;
	glc_state_audio_new((*audio_capture)->glc, &(*audio_capture)->id,
			    &(*audio_capture)->state_audio);
	(*audio_capture)->skip_data = 1;

	sem_init(&(*audio_capture)->capture, 0, 0);

	pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);
	pthread_create(&(*audio_capture)->capture_thread, &attr, audio_capture_thread, (void *) *audio_capture);
	pthread_attr_destroy(&attr);

	return 0;
}

int audio_capture_destroy(audio_capture_t audio_capture)
{
	if (audio_capture == NULL)
		return EINVAL;

	/** \todo snd_pcm_drain() ? */
	if (audio_capture->pcm)
		snd_pcm_close(audio_capture->pcm);

	audio_capture->stop_capture = 1;
	sem_post(&audio_capture->capture);
	pthread_join(audio_capture->capture_thread, NULL);

	free(audio_capture);
	return 0;
}

int audio_capture_set_device(audio_capture_t audio_capture, const char *device)
{
	if (audio_capture->pcm)
		return EALREADY;

	audio_capture->device = device;
	return 0;
}

int audio_capture_set_rate(audio_capture_t audio_capture, unsigned int rate)
{
	if (audio_capture->pcm)
		return EALREADY;

	audio_capture->rate = rate;
	return 0;
}

int audio_capture_set_channels(audio_capture_t audio_capture, unsigned int channels)
{
	if (audio_capture->pcm)
		return EALREADY;

	audio_capture->channels = channels;
	return 0;
}

int audio_capture_start(audio_capture_t audio_capture)
{
	int ret;
	if (audio_capture == NULL)
		return EINVAL;

	if (audio_capture->skip_data)
		glc_log(audio_capture->glc, GLC_WARNING, "audio_capture",
			 "device %s already started", audio_capture->device);
	else
		glc_log(audio_capture->glc, GLC_INFORMATION, "audio_capture",
			 "starting device %s", audio_capture->device);

	if (!audio_capture->pcm) {
		if ((ret = audio_capture_open(audio_capture)))
			return ret;
	}

	audio_capture->skip_data = 0;
	return 0;
}

int audio_capture_stop(audio_capture_t audio_capture)
{
	if (audio_capture == NULL)
		return EINVAL;

	if (audio_capture->skip_data)
		glc_log(audio_capture->glc, GLC_INFORMATION, "audio_capture",
			 "stopping device %s", audio_capture->device);
	else
		glc_log(audio_capture->glc, GLC_WARNING, "audio_capture",
			 "device %s already stopped", audio_capture->device);

	audio_capture->skip_data = 1;
	return 0;
}

int audio_capture_open(audio_capture_t audio_capture)
{
	snd_pcm_hw_params_t *hw_params = NULL;
	snd_pcm_sw_params_t *sw_params = NULL;
	ps_packet_t packet;
	int dir, ret = 0;
	glc_message_header_t msg_hdr;
	glc_audio_format_message_t fmt_msg;

	glc_log(audio_capture->glc, GLC_DEBUG, "audio_capture", "opening device %s",
		 audio_capture->device);

	/* open pcm */
	if ((ret = snd_pcm_open(&audio_capture->pcm, audio_capture->device, SND_PCM_STREAM_CAPTURE, 0)) < 0)
		goto err;

	/* init hw */
	if ((ret = snd_pcm_hw_params_malloc(&hw_params)) < 0)
		goto err;
	if ((ret = -audio_capture_init_hw(audio_capture, hw_params)))
		goto err;

	/* set software params */
	if ((ret = snd_pcm_sw_params_malloc(&sw_params)) < 0)
		goto err;
	if ((ret = -audio_capture_init_sw(audio_capture, sw_params)))
		goto err;

	/* we need period size */
	if ((ret = snd_pcm_hw_params_get_period_size(hw_params, &audio_capture->period_size, NULL)))
		goto err;
	audio_capture->bytes_per_frame = snd_pcm_frames_to_bytes(audio_capture->pcm, 1);
	audio_capture->period_size_in_bytes = audio_capture->period_size * audio_capture->bytes_per_frame;

	/* read actual settings */
	if ((ret = snd_pcm_hw_params_get_format(hw_params, &audio_capture->format)) < 0)
		goto err;
	if ((ret = snd_pcm_hw_params_get_rate(hw_params, &audio_capture->rate, &dir)) < 0)
		goto err;
	if ((ret = snd_pcm_hw_params_get_channels(hw_params, &audio_capture->channels)) < 0)
		goto err;

	audio_capture->rate_usec = 1000000 / audio_capture->rate;

	audio_capture->flags = GLC_AUDIO_INTERLEAVED;
	audio_capture->flags |= audio_capture_fmt_flags(audio_capture->format);
	if (audio_capture->flags & GLC_AUDIO_FORMAT_UNKNOWN) {
		glc_log(audio_capture->glc, GLC_ERROR, "audio_capture",
			 "unsupported audio format 0x%02x", audio_capture->format);
		return ENOTSUP;
	}

	/* prepare packet */
	fmt_msg.audio = audio_capture->id;
	fmt_msg.rate = audio_capture->rate;
	fmt_msg.channels = audio_capture->channels;
	fmt_msg.flags = audio_capture->flags;

	msg_hdr.type = GLC_MESSAGE_AUDIO_FORMAT;
	ps_packet_init(&packet, audio_capture->to);
	ps_packet_open(&packet, PS_PACKET_WRITE);
	ps_packet_write(&packet, &msg_hdr, GLC_MESSAGE_HEADER_SIZE);
	ps_packet_write(&packet, &fmt_msg, GLC_AUDIO_FORMAT_MESSAGE_SIZE);
	ps_packet_close(&packet);
	ps_packet_destroy(&packet);

	snd_pcm_hw_params_free(hw_params);
	snd_pcm_sw_params_free(sw_params);

	/* init callback */
	if ((ret = snd_async_add_pcm_handler(&audio_capture->async_handler, audio_capture->pcm,
					     audio_capture_async_callback, audio_capture)) < 0)
		goto err;
	if ((ret = snd_pcm_start(audio_capture->pcm)) < 0)
		goto err;

	glc_log(audio_capture->glc, GLC_DEBUG, "audio_capture",
		 "success (stream=%d, device=%s, rate=%u, channels=%u)", audio_capture->id,
		 audio_capture->device, audio_capture->rate, audio_capture->channels);

	return 0;
err:
	if (hw_params)
		snd_pcm_hw_params_free(hw_params);
	if (sw_params)
		snd_pcm_sw_params_free(sw_params);

	glc_log(audio_capture->glc, GLC_ERROR, "audio_capture",
		 "initialization failed: %s", snd_strerror(ret));
	return -ret;
}

glc_flags_t audio_capture_fmt_flags(snd_pcm_format_t pcm_fmt)
{
	switch (pcm_fmt) {
	case SND_PCM_FORMAT_S16_LE:
		return GLC_AUDIO_S16_LE;
	case SND_PCM_FORMAT_S24_LE:
		return GLC_AUDIO_S24_LE;
	case SND_PCM_FORMAT_S32_LE:
		return GLC_AUDIO_S32_LE;
	default:
		return GLC_AUDIO_FORMAT_UNKNOWN;
	}
}

int audio_capture_init_hw(audio_capture_t audio_capture, snd_pcm_hw_params_t *hw_params)
{
	snd_pcm_format_mask_t *formats = NULL;
	snd_pcm_uframes_t max_buffer_size;
	unsigned int min_periods;
	int dir, ret = 0;

	if ((ret = snd_pcm_hw_params_any(audio_capture->pcm, hw_params)) < 0)
		goto err;

	if ((ret = snd_pcm_hw_params_set_access(audio_capture->pcm, hw_params, SND_PCM_ACCESS_RW_INTERLEAVED)) < 0)
		goto err;

	formats = (snd_pcm_format_mask_t *) malloc(snd_pcm_format_mask_sizeof());
	snd_pcm_format_mask_none(formats);
	snd_pcm_format_mask_set(formats, SND_PCM_FORMAT_S16_LE);
	snd_pcm_format_mask_set(formats, SND_PCM_FORMAT_S24_LE);
	snd_pcm_format_mask_set(formats, SND_PCM_FORMAT_S32_LE);

	if ((ret = snd_pcm_hw_params_set_format_mask(audio_capture->pcm, hw_params, formats)) < 0)
		goto err;
	if ((ret = snd_pcm_hw_params_set_channels(audio_capture->pcm, hw_params, audio_capture->channels)) < 0)
		goto err;
	if ((ret = snd_pcm_hw_params_set_rate(audio_capture->pcm, hw_params, audio_capture->rate, 0)) < 0)
		goto err;

	if ((ret = snd_pcm_hw_params_get_buffer_size_max(hw_params, &max_buffer_size)) < 0)
		goto err;
	if ((ret = snd_pcm_hw_params_set_buffer_size(audio_capture->pcm, hw_params, max_buffer_size)) < 0)
		goto err;

	if ((ret = snd_pcm_hw_params_get_periods_min(hw_params, &min_periods, &dir)) < 0)
		goto err;
	if ((ret = snd_pcm_hw_params_set_periods(audio_capture->pcm, hw_params,
						 min_periods < audio_capture->min_periods ?
						 audio_capture->min_periods : min_periods, dir)) < 0)
		goto err;

	if ((ret = snd_pcm_hw_params(audio_capture->pcm, hw_params)) < 0)
		goto err;

	free(formats);
	return 0;
err:
	if (formats)
		free(formats);
	return -ret;
}

int audio_capture_init_sw(audio_capture_t audio_capture, snd_pcm_sw_params_t *sw_params)
{
	int ret = 0;

	if ((ret = snd_pcm_sw_params_current(audio_capture->pcm, sw_params)) < 0)
		goto err;
	if ((ret = snd_pcm_sw_params(audio_capture->pcm, sw_params)))
		goto err;

	return 0;
err:
	return -ret;
}

void audio_capture_async_callback(snd_async_handler_t *async_handler)
{
	/*
	 Async handler is called from signal handler so mixing this
	 with mutexes seems to be a retared idea.

	 http://www.kaourantin.net/2006/08/pthreads-and-signals.html
	 */
	audio_capture_t audio_capture =
		snd_async_handler_get_callback_private(async_handler);
	sem_post(&audio_capture->capture);
}

void *audio_capture_thread(void *argptr)
{
	audio_capture_t audio_capture = argptr;
	snd_pcm_sframes_t avail, read;
	glc_utime_t time, delay_usec = 0;
	glc_audio_header_t hdr;
	glc_message_header_t msg_hdr;
	ps_packet_t packet;
	int ret;
	char *dma;

	ps_packet_init(&packet, audio_capture->to);
	msg_hdr.type = GLC_MESSAGE_AUDIO;

	while (!sem_wait(&audio_capture->capture)) {
		if (audio_capture->stop_capture)
			break;

		avail = 0;
		if ((ret = snd_pcm_delay(audio_capture->pcm, &avail)) < 0)
			audio_capture_xrun(audio_capture, ret);

		while (avail >= audio_capture->period_size) {
			/* loop till we have one period available */
			avail = snd_pcm_avail_update(audio_capture->pcm);
			if (avail < audio_capture->period_size)
				continue;

			/* and discard it if glc is paused */
			if (audio_capture->skip_data) {
				fprintf(stderr, "snd_pcm_reset()\n");
				snd_pcm_reset(audio_capture->pcm);
				continue;
			}

			time = glc_state_time(audio_capture->glc);
			delay_usec = avail * audio_capture->rate_usec;

			if (delay_usec < time)
				time -= delay_usec;
			hdr.timestamp = time;
			hdr.size = audio_capture->period_size_in_bytes;
			hdr.audio = audio_capture->id;

			if ((ret = ps_packet_open(&packet, PS_PACKET_WRITE)))
				goto cancel;
			if ((ret = ps_packet_write(&packet, &msg_hdr, GLC_MESSAGE_HEADER_SIZE)))
				goto cancel;
			if ((ret = ps_packet_write(&packet, &hdr, GLC_AUDIO_HEADER_SIZE)))
				goto cancel;
			if ((ret = ps_packet_dma(&packet, (void *) &dma, hdr.size, PS_ACCEPT_FAKE_DMA)))
				goto cancel;

			if ((read = snd_pcm_readi(audio_capture->pcm, dma, audio_capture->period_size)) < 0)
				read = -audio_capture_xrun(audio_capture, read);

			if (read != audio_capture->period_size)
				glc_log(audio_capture->glc, GLC_WARNING, "audio_capture",
					 "read %ld, expected %ld", read * audio_capture->bytes_per_frame,
					 audio_capture->period_size_in_bytes);
			else if (read < 0)
				glc_log(audio_capture->glc, GLC_ERROR, "audio_capture",
					 "xrun recovery failed: %s", snd_strerror(read));

			hdr.size = read * audio_capture->bytes_per_frame;
			if ((ret = ps_packet_setsize(&packet, GLC_MESSAGE_HEADER_SIZE +
							     GLC_AUDIO_HEADER_SIZE +
							     hdr.size)))
				goto cancel;
			if ((ret = ps_packet_close(&packet)))
				goto cancel;

			/* just check for xrun */
			if ((ret = snd_pcm_delay(audio_capture->pcm, &avail)) < 0) {
				audio_capture_xrun(audio_capture, ret);
				break;
			}
			continue;

cancel:
			glc_log(audio_capture->glc, GLC_ERROR, "audio_capture", "%s (%d)", strerror(ret), ret);
			if (ret == EINTR)
				break;
			if (ps_packet_cancel(&packet))
				break;
		}
	}

	ps_packet_destroy(&packet);
	return NULL;
}

int audio_capture_xrun(audio_capture_t audio_capture, int err)
{
	glc_log(audio_capture->glc, GLC_DEBUG, "audio_capture", "xrun");

	if (err == -EPIPE) {
		if ((err = snd_pcm_prepare(audio_capture->pcm)) < 0)
			return -err;
		if ((err = snd_pcm_start(audio_capture->pcm)) < 0)
			return -err;
		return 0;
	} else if (err == -ESTRPIPE) {
		while ((err = snd_pcm_resume(audio_capture->pcm)) == -EAGAIN)
			sched_yield();
		if (err < 0) {
			if ((err = snd_pcm_prepare(audio_capture->pcm)) < 0)
				return -err;
			if ((err = snd_pcm_start(audio_capture->pcm)) < 0)
				return -err;
			return 0;
		}
	}

	return -err;
}

/**  \} */
/**  \} */