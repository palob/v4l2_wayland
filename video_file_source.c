#include "video_file_source.h"

static int open_codec_context(int *stream_idx, AVCodecContext **dec_ctx,
 AVFormatContext *fmt_ctx, enum AVMediaType type) {
  int ret, stream_index;
  AVStream *st;
  AVCodec *dec = NULL;
  AVDictionary *opts = NULL;
  ret = av_find_best_stream(fmt_ctx, type, -1, -1, NULL, 0);
  if (ret < 0) {
    fprintf(stderr, "Could not find %s stream\n",
     av_get_media_type_string(type));
    return ret;
  } else {
    stream_index = ret;
    st = fmt_ctx->streams[stream_index];

    /* find decoder for the stream */
    dec = avcodec_find_decoder(st->codecpar->codec_id);
    if (!dec) {
      fprintf(stderr, "Failed to find %s codec\n",
       av_get_media_type_string(type));
     return AVERROR(EINVAL);
  }

    /* Allocate a codec context for the decoder */
    *dec_ctx = avcodec_alloc_context3(dec);
    if (!*dec_ctx) {
      fprintf(stderr, "Failed to allocate the %s codec context\n",
       av_get_media_type_string(type));
      return AVERROR(ENOMEM);
    }

    /* Copy codec parameters from input stream to output codec context */
    if ((ret = avcodec_parameters_to_context(*dec_ctx, st->codecpar)) < 0) {
      fprintf(stderr, "Failed to copy %s codec parameters to decoder context\n",
       av_get_media_type_string(type));
      return ret;
    }

    if ((ret = avcodec_open2(*dec_ctx, dec, &opts)) < 0) {
      fprintf(stderr, "Failed to open %s codec\n",
       av_get_media_type_string(type));
      return ret;
    }
    *stream_idx = stream_index;
  }
  return 0;
}

void video_file_create(video_file_t *vf, char *name, uint64_t z) {
	memset(vf, 0, sizeof(*vf));
	printf("video_file_create %d\n", z);
	strncpy(vf->name, name, 254);
	((draggable *)vf)->z = z;
	pthread_create(&vf->thread_id, NULL, video_file_thread, vf);
}

int video_file_destroy(video_file_t *vf) {
	avcodec_close(vf->video_dec_ctx);
	avformat_close_input(&vf->fmt_ctx);
	av_frame_free(&vf->frame);
	av_free(vf->video_dst_data[0]);
	av_freep(&vf->decoded_frame->data[0]);
	av_frame_free(&vf->decoded_frame);
	jack_ringbuffer_free(vf->vbuf);
	pthread_cancel(vf->thread_id);
	memset(vf, 0, sizeof(*vf));
	return 0;
}

int video_file_play(video_file_t *vf) {
	printf("play\n");
	vf->current_playtime = 0;
	clock_gettime(CLOCK_MONOTONIC, &vf->play_start_ts);
	return 0;
}

int video_file_in(video_file_t *v, double x, double y) {
		if ((x >= ((draggable *)v)->pos.x && x <= v->dr.pos.x + v->width) &&
			(y >= ((draggable *)v)->pos.y && y <= ((draggable *)v)->pos.y + v->height)) {
		return 1;
	} else {
		return 0;
	}
}

void *video_file_thread(void *arg) {
  int ret;
	int got_frame;
	got_frame = 0;
  video_file_t *vf = (video_file_t *)arg;
	av_register_all();
	vf->fmt_ctx = NULL;
  if (avformat_open_input(&vf->fmt_ctx, vf->name, NULL, NULL) < 0) {
		printf("cannot open file: %s\n", vf->name);
		return NULL;
	}
	if (avformat_find_stream_info(vf->fmt_ctx, NULL) < 0) {
		printf("could not find stream information\n");
		return NULL;
	}
	av_dump_format(vf->fmt_ctx, 0, vf->name, 0);
	if (open_codec_context(&vf->video_stream_idx, &vf->video_dec_ctx, vf->fmt_ctx,
	 AVMEDIA_TYPE_VIDEO) >= 0) {
		vf->video_stream = vf->fmt_ctx->streams[vf->video_stream_idx];
		vf->width = vf->video_dec_ctx->width;
		vf->height = vf->video_dec_ctx->height;
		vf->pix_fmt = vf->video_dec_ctx->pix_fmt;
		ret = av_image_alloc(vf->video_dst_data, vf->video_dst_linesize,
		 vf->width, vf->height, AV_PIX_FMT_ARGB, 1);
		if (ret < 0) {
			printf("could not allocate raw video buffer\n");
			video_file_destroy(vf);
			return NULL;
		}
		vf->video_dst_bufsize = ret;
	}
	vf->resample = sws_getContext(vf->width, vf->height, vf->pix_fmt,
	 vf->width, vf->height, AV_PIX_FMT_BGRA, SWS_BICUBIC, NULL, NULL, NULL);
	vf->frame = av_frame_alloc();
	av_init_packet(&vf->pkt);
	vf->pkt.data = NULL;
	vf->pkt.size = 0;
  pthread_mutex_init(&vf->lock, NULL);
  pthread_cond_init(&vf->data_ready, NULL);
	vf->vbuf = jack_ringbuffer_create(5*vf->video_dst_bufsize);
	memset(vf->vbuf->buf, 0, vf->vbuf->size);
  vf->decoded_frame = av_frame_alloc();
  vf->decoded_frame->format = AV_PIX_FMT_ARGB;
  vf->decoded_frame->width = vf->width;
  vf->decoded_frame->height = vf->height;
  av_image_alloc(vf->decoded_frame->data, vf->decoded_frame->linesize,
	 vf->decoded_frame->width, vf->decoded_frame->height, vf->decoded_frame->format, 1);
  vf->allocated = 1;
	pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, NULL);
  pthread_mutex_lock(&vf->lock);
	double pts;
	video_file_play(vf);
	vf->playing = 1;
	vf->active = 1;
	while(av_read_frame(vf->fmt_ctx, &vf->pkt) >= 0) {
		vf->decoding_started = 1;
		if (vf->pkt.stream_index == vf->video_stream_idx) {
			vf->frame = av_frame_alloc();
			ret = avcodec_decode_video2(vf->video_dec_ctx, vf->frame,
		   &got_frame, &vf->pkt);
			if (ret < 0) {
				printf("Error decoding video frame (%s)\n", av_err2str(ret));
			}
			if (vf->pkt.dts != AV_NOPTS_VALUE) {
				pts = av_frame_get_best_effort_timestamp(vf->frame);
			} else {
				pts = 0;
			}
			pts *= av_q2d(vf->video_stream->time_base);
			if (got_frame) {
	printf("z: %d\n", ((draggable *)vf)->z);
				sws_scale(vf->resample, (uint8_t const * const *)vf->frame->data,
	 			 vf->frame->linesize, 0, vf->frame->height, vf->video_dst_data,
	 			 vf->video_dst_linesize);
				int space = vf->video_dst_bufsize + sizeof(double);
				int buf_space = jack_ringbuffer_write_space(vf->vbuf);
				while (buf_space < space) {
					printf("video_file wait\n");
					pthread_cond_wait(&vf->data_ready, &vf->lock);
					buf_space = jack_ringbuffer_write_space(vf->vbuf);
  			}
				jack_ringbuffer_write(vf->vbuf, (void *)&pts, sizeof(double));
				jack_ringbuffer_write(vf->vbuf, (void *)vf->video_dst_data[0],
				 vf->video_dst_bufsize);
			}
			av_frame_unref(vf->frame);
		}
	}
	av_free_packet(&vf->pkt);
	vf->decoding_finished = 1;
  pthread_mutex_unlock(&vf->lock);
	return 0;
}


