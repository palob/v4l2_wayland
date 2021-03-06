#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <poll.h>
#include <pthread.h>
#include <getopt.h>
#include <math.h>
#include <time.h>

#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <pwd.h>
#include <sys/time.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <jack/jack.h>
#include <jack/ringbuffer.h>
#include <jack/midiport.h>
#include <linux/input.h>
#include <cairo/cairo.h>
#include <fftw3.h>
#include <algorithm>
#include <vector>
#include <memory>
#include <cstring>
#include <boost/bind.hpp>
#include <gtk/gtk.h>

#include "dingle_dots.h"
#include "kmeter.h"
#include "muxing.h"
#include "sound_shape.h"
#include "midi.h"
#include "v4l2_wayland.h"
#include "v4l2.h"
#include "drawable.h"
#include "video_file_source.h"
#include "easable.h"

fftw_complex                   *fftw_in, *fftw_out;
fftw_plan                      p;
static ccv_dense_matrix_t      *cdm = 0, *cdm2 = 0;
static ccv_tld_t               *tld = 0;

jack_ringbuffer_t        *video_ring_buf, *audio_ring_buf;
const size_t              sample_size = sizeof(jack_default_audio_sample_t);
pthread_mutex_t           av_thread_lock = PTHREAD_MUTEX_INITIALIZER;

void errno_exit(const char *s) {
	fprintf(stderr, "%s error %d, %s\n", s, errno, strerror(errno));
	exit(EXIT_FAILURE);
}

void *audio_disk_thread(void *arg) {
	int ret;
	DingleDots *dd = (DingleDots *)arg;
	pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, NULL);
	pthread_mutex_lock(&dd->audio_thread_info.lock);
	while(1) {
		ret = write_audio_frame(dd, dd->video_output_context, &dd->audio_thread_info.stream);
		if (ret == 1) {
			dd->audio_done = 1;
			break;
		}
		if (ret == 0) continue;
		if (ret == -1) pthread_cond_wait(&dd->audio_thread_info.data_ready,
										 &dd->audio_thread_info.lock);
	}
	if (dd->audio_done && dd->video_done && !dd->trailer_written) {
		av_write_trailer(dd->video_output_context);
		dd->trailer_written = 1;
	}
	pthread_mutex_unlock(&dd->audio_thread_info.lock);
	return 0;
}

void *video_disk_thread (void *arg) {
	int ret;
	DingleDots *dd = (DingleDots *)arg;
	pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, NULL);
	pthread_mutex_lock(&dd->video_thread_info.lock);
	while(1) {
		ret = write_video_frame(dd, dd->video_output_context,
								&dd->video_thread_info.stream);
		if (ret == 1) {
			dd->video_done = 1;
			break;
		}
		if (ret == 0) continue;
		if (ret == -1) pthread_cond_wait(&dd->video_thread_info.data_ready,
										 &dd->video_thread_info.lock);
	}
	if (dd->audio_done && dd->video_done && !dd->trailer_written) {
		av_write_trailer(dd->video_output_context);
		dd->trailer_written = 1;
	}
	pthread_mutex_unlock(&dd->video_thread_info.lock);
	printf("vid thread gets here\n");
	return 0;
}

int timespec2file_name(char *buf, uint len, const char *dir,
					   const char *extension,
					   struct timespec *ts) {
	uint ret;
	struct tm t;
	const char *homedir;
	if (localtime_r(&(ts->tv_sec), &t) == NULL) {
		return 1;
	}
	if ((homedir = getenv("HOME")) == NULL) {
		homedir = getpwuid(getuid())->pw_dir;
	}
	ret = snprintf(buf, len, "%s/%s/v4l2_wayland-", homedir, dir);
	len -= ret - 1;
	ret = strftime(&buf[strlen(buf)], len, "%Y-%m-%d-%H:%M:%S", &t);
	if (ret == 0)
		return 2;
	len -= ret - 1;
	ret = snprintf(&buf[strlen(buf)], len, ".%03d.%s",
			(int)ts->tv_nsec/1000000, extension);
	if (ret >= len)
		return 3;
	return 0;
}

void *snapshot_disk_thread (void *arg) {
	int fsize;
	int tsize;
	int space;
	cairo_surface_t *csurf;
	AVFrame *frame;
	struct timespec ts;
	char timestr[STR_LEN+1];
	tzset();
	DingleDots *dd = (DingleDots *)arg;
	pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, NULL);
	pthread_mutex_lock(&dd->snapshot_thread_info.lock);
	frame = NULL;
	frame = av_frame_alloc();
	frame->width = dd->drawing_rect.width;
	frame->height = dd->drawing_rect.height;
	frame->format = AV_PIX_FMT_ARGB;
	av_image_alloc(frame->data, frame->linesize,
				   frame->width, frame->height, (AVPixelFormat)frame->format, 1);
	if (!frame) {
		fprintf(stderr, "Could not allocate temporary picture\n");
		exit(1);
	}

	fsize = 4 * frame->width * frame->height;
	tsize = fsize + sizeof(struct timespec);
	csurf = cairo_image_surface_create_for_data((unsigned char *)frame->data[0],
			CAIRO_FORMAT_ARGB32, frame->width, frame->height, 4*frame->width);
	while(1) {
		space = jack_ringbuffer_read_space(dd->snapshot_thread_info.ring_buf);
		while (space >= tsize) {
			jack_ringbuffer_read(dd->snapshot_thread_info.ring_buf, (char *)frame->data[0],
					fsize);
			jack_ringbuffer_read(dd->snapshot_thread_info.ring_buf, (char *)&ts,
								 sizeof(ts));
			timespec2file_name(timestr, STR_LEN, "Pictures", "png", &ts);
			cairo_surface_write_to_png(csurf, timestr);
			space = jack_ringbuffer_read_space(dd->snapshot_thread_info.ring_buf);
		}
		pthread_cond_wait(&dd->snapshot_thread_info.data_ready,
						  &dd->snapshot_thread_info.lock);
	}
	cairo_surface_destroy(csurf);
	av_freep(frame->data[0]);
	av_frame_free(&frame);
	pthread_mutex_unlock(&dd->snapshot_thread_info.lock);
	return 0;
}

ccv_tld_t *new_tld(int x, int y, int w, int h, DingleDots *dd) {
	ccv_tld_param_t p = ccv_tld_default_params;
	ccv_rect_t box = ccv_rect(x, y, w, h);
	ccv_read(dd->analysis_frame->data[0], &cdm, CCV_IO_ARGB_RAW | CCV_IO_GRAY,
			dd->analysis_rect.height, dd->analysis_rect.width, 4*dd->analysis_rect.width);
	return ccv_tld_new(cdm, box, p);
}

static void render_detection_box(cairo_t *cr, int initializing,
								 int x, int y, int w, int h) {
	double minimum = vw_min(w, h);
	double r = 0.05 * minimum;
	cairo_save(cr);
	//cairo_new_sub_path(cr);
	if (initializing) cairo_set_source_rgba(cr, 0.85, 0.85, 0., 0.75);
	else cairo_set_source_rgba(cr, 0.2, 0., 0.2, 0.75);
	cairo_arc(cr, x + r, y + r, r, M_PI, 1.5 * M_PI);
	cairo_stroke(cr);
	cairo_arc(cr, x + w - r, y + r, r, 1.5 * M_PI, 2. * M_PI);
	cairo_stroke(cr);
	cairo_arc(cr, x + w - r, y + h - r, r, 0, 0.5 * M_PI);
	cairo_stroke(cr);
	cairo_arc(cr, x + r, y + h - r, r, 0.5 * M_PI, M_PI);
	cairo_stroke(cr);
	/*cairo_close_path(cr);
  if (initializing) cairo_set_source_rgba(cr, 0.85, 0.85, 0., 0.25);
  else cairo_set_source_rgba(cr, 0.2, 0., 0.2, 0.25);
  cairo_fill_preserve(cr);*/
	cairo_move_to(cr, x, 0.5 * h + y);
	cairo_line_to(cr, x + w, 0.5 * h + y);
	cairo_move_to(cr, 0.5 * w + x, y);
	cairo_line_to(cr, 0.5 * w + x, h + y);
	cairo_stroke(cr);
	cairo_restore(cr);
}



static void render_pointer(cairo_t *cr, double x, double y) {
	double l = 10.;
	cairo_save(cr);
	cairo_set_source_rgba(cr, 1, 0, 0.1, 0.75);
	cairo_translate(cr, x, y);
	cairo_rotate(cr, M_PI/4.);
	cairo_translate(cr, -x, -y);
	cairo_move_to(cr, x - l, y);
	cairo_line_to(cr, x + l, y);
	cairo_move_to(cr, x, y - l);
	cairo_line_to(cr, x, y + l);
	cairo_stroke(cr);
	cairo_restore(cr);
}

void timespec_diff(struct timespec *start, struct timespec *stop,
				   struct timespec *result) {
	if ((stop->tv_nsec - start->tv_nsec) < 0) {
		result->tv_sec = stop->tv_sec - start->tv_sec - 1;
		result->tv_nsec = stop->tv_nsec - start->tv_nsec + 1000000000;
	} else {
		result->tv_sec = stop->tv_sec - start->tv_sec;
		result->tv_nsec = stop->tv_nsec - start->tv_nsec;
	}
	return;
}

double timespec_to_seconds(struct timespec *ts) {
	double ret;
	ret = ts->tv_sec + ts->tv_nsec / 1000000000.;
	return ret;
}

void clear(cairo_t *cr)
{
	cairo_save(cr);
	cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
	cairo_paint(cr);
	cairo_restore(cr);
}

void get_sources(DingleDots *dd, std::vector<Drawable *> &sources)
{
	for (int i = 0; i < MAX_NUM_V4L2; i++) {
		if (dd->v4l2[i].active) {
			sources.push_back(&dd->v4l2[i]);
		}
	}
	for (int j = 0; j < MAX_NUM_VIDEO_FILES; j++) {
		if (dd->vf[j].active) {
			sources.push_back(&dd->vf[j]);
		}
	}
	for (int i = 0; i < 2; i++) {
		if (dd->sprites[i].active) {
			sources.push_back(&dd->sprites[i]);
		}
	}
}

double calculate_motion(SoundShape *ss, AVFrame *sources_frame, uint32_t *save_buf_sources,
						double width, double height)
{
	int i;
	int j;
	double fb;
	int iend;
	int jend;
	double fg;
	double fr;
	int jstart;
	int istart;
	double bw;
	double bw2;
	double diff;
	uint32_t val;
	double sum;
	uint32_t npts;

	sum = 0;
	npts = 0;
	istart = vw_min(width, vw_max(0, round(ss->pos.x -
													ss->r * ss->scale)));
	jstart = vw_min(height, vw_max(0, round(ss->pos.y -
													   ss->r * ss->scale)));
	iend = vw_max(istart, vw_min(width, round(ss->pos.x +
													ss->r * ss->scale)));
	jend = vw_max(jstart, vw_min(height, round(ss->pos.y +
													 ss->r* ss->scale)));
	for (i = istart; i < iend; i++) {
		for (j = jstart; j < jend; j++) {
			if (ss->in(i, j)) {
				val = save_buf_sources[i + j * sources_frame->width];
				fr = ((val & 0x00ff0000) >> 16) / 256.;
				fg = ((val & 0x0000ff00) >> 8) / 256.;
				fb = ((val & 0x000000ff)) / 256.;
				bw = fr * 0.3 + fg * 0.59 + fb * 0.1;
				val = ((uint32_t *)sources_frame->data[0])[i + j * sources_frame->width];
				fr = ((val & 0x00ff0000) >> 16) / 256.;
				fg = ((val & 0x0000ff00) >> 8) / 256.;
				fb = ((val & 0x000000ff)) / 256.;
				bw2 = fr * 0.3 + fg * 0.59 + fb * 0.1;
				diff = bw - bw2;
				sum += diff * diff;
				npts++;
			}
		}
	}
	return sum / npts;
}

void set_to_on_or_off(SoundShape *ss, GtkWidget *da)
{
	if (ss->double_clicked_on || ss->motion_state
			|| ss->tld_state) {
		if (!ss->on) {
			ss->set_on();
			gtk_widget_queue_draw(da);
		}
	}
	if (!ss->double_clicked_on && !ss->motion_state
			&& !ss->tld_state) {
		if (ss->on) {
			ss->set_off();
			gtk_widget_queue_draw(da);
		}
	}
}

void process_image(cairo_t *screen_cr, void *arg) {
	DingleDots *dd = (DingleDots *)arg;
	static int first_call = 1;
	static int first_data = 1;
	static struct timespec ts, snapshot_ts;
	static ccv_comp_t newbox;
	static int made_first_tld = 0;
	std::vector<Drawable *> sources;
	int s, i;
	double diff;
	static uint32_t *save_buf_sources;
	int render_drawing_surf = 0;
	cairo_t *sources_cr;
	cairo_surface_t *sources_surf;
	cairo_t *drawing_cr;
	cairo_surface_t *drawing_surf;
	struct timespec start_ts, end_ts;
	clock_gettime(CLOCK_MONOTONIC, &start_ts);
	if (first_data) {
		save_buf_sources = (uint32_t *)malloc(dd->sources_frame->linesize[0] * dd->sources_frame->height);
	}
	sources_surf = cairo_image_surface_create_for_data((unsigned char *)dd->sources_frame->data[0],
			CAIRO_FORMAT_ARGB32, dd->sources_frame->width, dd->sources_frame->height,
			dd->sources_frame->linesize[0]);
	sources_cr = cairo_create(sources_surf);
	drawing_surf = cairo_image_surface_create_for_data((unsigned char *)dd->drawing_frame->data[0],
			CAIRO_FORMAT_ARGB32, dd->drawing_frame->width, dd->drawing_frame->height,
			dd->drawing_frame->linesize[0]);
	drawing_cr = cairo_create(drawing_surf);
	if (!first_data && (dd->doing_motion || dd->snapshot_shape.active)) {
		memcpy(save_buf_sources, dd->sources_frame->data[0], 4 * dd->sources_frame->width *
				dd->sources_frame->height);
	}
	clear(sources_cr);
	get_sources(dd, sources);
	std::sort(sources.begin(), sources.end(), [](Drawable *a, Drawable *b) { return a->z < b->z; } );
	std::vector<cairo_t *> contexts;
	cairo_save(screen_cr);
	cairo_scale(screen_cr, dd->scale, dd->scale);
	contexts.push_back(screen_cr);
	contexts.push_back(sources_cr);
	for (std::vector<Drawable *>::iterator it = sources.begin(); it != sources.end(); ++it) {
		(*it)->update_easers();
		(*it)->render(contexts);
	}
	if (dd->doing_motion) {
		std::vector<SoundShape *> sound_shapes;
		for (i = 0; i < MAX_NUM_SOUND_SHAPES; ++i) {
			SoundShape *s = &dd->sound_shapes[i];
			if (s->active) {
				sound_shapes.push_back(s);
			}
		}
		for (std::vector<SoundShape *>::iterator it = sound_shapes.begin(); it != sound_shapes.end(); ++it) {
			SoundShape *s = *it;
			diff = calculate_motion(s, dd->sources_frame, save_buf_sources,
										   dd->drawing_rect.width, dd->drawing_rect.height);
			if (diff > dd->motion_threshold) {
				s->set_motion_state(1);
			} else {
				s->set_motion_state(0);
			}
		}
	} else {
		for (s = 0; s < MAX_NUM_SOUND_SHAPES; s++) {
			dd->sound_shapes[s].set_motion_state(0);
		}
	}
	if (dd->snapshot_shape.active) {
		diff= calculate_motion(&dd->snapshot_shape, dd->sources_frame,
							   save_buf_sources, dd->drawing_rect.width,
							   dd->drawing_rect.height);
		if (diff >= dd->motion_threshold) {
			dd->snapshot_shape.set_motion_state(1);
		} else {
			dd->snapshot_shape.set_motion_state(0);
		}
		set_to_on_or_off(&dd->snapshot_shape, dd->drawing_area);
	}
	if (dd->do_snapshot || (dd->recording_started && !dd->recording_stopped)) {
		render_drawing_surf = 1;
	}
	if (render_drawing_surf) {
		cairo_save(drawing_cr);
		cairo_set_source_surface(drawing_cr, sources_surf, 0.0, 0.0);
		cairo_set_operator(drawing_cr, CAIRO_OPERATOR_SOURCE);
		cairo_paint(drawing_cr);
		cairo_restore(drawing_cr);
	}
	if (first_data) {
		first_data = 0;
	}
	if (dd->doing_tld) {
		sws_scale(dd->analysis_resize, (uint8_t const * const *)dd->sources_frame->data,
				  dd->sources_frame->linesize, 0, dd->sources_frame->height, dd->analysis_frame->data, dd->analysis_frame->linesize);
		if (dd->make_new_tld == 1) {
			if (dd->user_tld_rect.width > 0 && dd->user_tld_rect.height > 0) {
				tld = new_tld(dd->user_tld_rect.x/dd->ascale_factor_x, dd->user_tld_rect.y/dd->ascale_factor_y,
							  dd->user_tld_rect.width/dd->ascale_factor_x, dd->user_tld_rect.height/dd->ascale_factor_y, dd);
			} else {
				tld = NULL;
				dd->doing_tld = 0;
				made_first_tld = 0;
				newbox.rect.x = 0;
				newbox.rect.y = 0;
				newbox.rect.width = 0;
				newbox.rect.height = 0;
			}
			made_first_tld = 1;
			dd->make_new_tld = 0;
		} else {
			ccv_read(dd->analysis_frame->data[0], &cdm2, CCV_IO_ABGR_RAW |
					CCV_IO_GRAY, dd->analysis_rect.height, dd->analysis_rect.width, 4*dd->analysis_rect.width);
			ccv_tld_info_t info;
			newbox = ccv_tld_track_object(tld, cdm, cdm2, &info);
			cdm = cdm2;
			cdm2 = 0;
		}
		if (newbox.rect.width && newbox.rect.height &&
				made_first_tld && tld->found) {
			for (i = 0; i < MAX_NUM_SOUND_SHAPES; i++) {
				if (!dd->sound_shapes[i].active) continue;
				if (dd->sound_shapes[i].in(dd->ascale_factor_x*newbox.rect.x +
										   0.5*dd->ascale_factor_x*newbox.rect.width,
										   dd->ascale_factor_y*newbox.rect.y +
										   0.5*dd->ascale_factor_y*newbox.rect.height)) {
					dd->sound_shapes[i].tld_state = 1;
				} else {
					dd->sound_shapes[i].tld_state = 0;
				}
			}
		}
	} else {
		tld = NULL;
		made_first_tld = 0;
		newbox.rect.x = 0;
		newbox.rect.y = 0;
		newbox.rect.width = 0;
		newbox.rect.height = 0;
	}
	for (int i = 0; i < MAX_NUM_SOUND_SHAPES; i++) {
		if (!dd->sound_shapes[i].active) continue;
		set_to_on_or_off(&dd->sound_shapes[i], dd->drawing_area);
	}
	std::vector<Drawable *> sound_shapes;
	std::vector<cairo_t *> ss_contexts;
	ss_contexts.push_back(screen_cr);
	if (dd->snapshot_shape.active) {
		dd->snapshot_shape.update_easers();
		dd->snapshot_shape.render(ss_contexts);
	}
	if (render_drawing_surf) {
		ss_contexts.push_back(drawing_cr);
	}
	for (i = 0; i < MAX_NUM_SOUND_SHAPES; ++i) {
		SoundShape *s = &dd->sound_shapes[i];
		if (s->active) {
			sound_shapes.push_back(s);
		}
	}
	std::sort(sound_shapes.begin(), sound_shapes.end(), [](Drawable *a, Drawable *b) { return a->z < b->z; } );
	for (std::vector<Drawable *>::iterator it = sound_shapes.begin(); it != sound_shapes.end(); ++it) {
		Drawable *d = *it;
		d->update_easers();
		if (d->active) d->render(ss_contexts);
	}

#if defined(RENDER_KMETERS)
	for (i = 0; i < 2; i++) {
		kmeter_render(&dd->meters[i], drawing_cr, 1.);
		kmeter_render(&dd->meters[i], screen_cr, 1.);
	}
#endif
#if defined(RENDER_FFT)
	double space;
	double w;
	double h;
	double x;
	w = dd->drawing_rect.width / (FFT_SIZE + 1);
	space = w;
	cairo_save(drawing_cr);
	cairo_save(screen_cr);
	cairo_set_source_rgba(drawing_cr, 0, 0.25, 0, 0.5);
	cairo_set_source_rgba(screen_cr, 0, 0.25, 0, 0.5);
	for (i = 0; i < FFT_SIZE/2; i++) {
		h = ((20.0 * log(sqrt(fftw_out[i][0] * fftw_out[i][0] +
			  fftw_out[i][1] * fftw_out[i][1])) / M_LN10) + 50.) * (dd->drawing_rect.height / 50.);
		x = i * (w + space) + space;
		cairo_rectangle(drawing_cr, x, dd->drawing_rect.height - h, w, h);
		cairo_rectangle(screen_cr, x, dd->drawing_rect.height - h, w, h);
		cairo_fill(drawing_cr);
		cairo_fill(screen_cr);
	}
	cairo_restore(drawing_cr);
	cairo_restore(screen_cr);
#endif
	if (dd->smdown) {
		if (render_drawing_surf) {
			render_detection_box(drawing_cr, 1, dd->user_tld_rect.x, dd->user_tld_rect.y,
								 dd->user_tld_rect.width, dd->user_tld_rect.height);
		}
		render_detection_box(screen_cr, 1, dd->user_tld_rect.x, dd->user_tld_rect.y,
							 dd->user_tld_rect.width, dd->user_tld_rect.height);
	}
	if (dd->selection_in_progress) {
		dd->update_easers();
		if (render_drawing_surf) {
			dd->render_selection_box(drawing_cr);
		}
		dd->render_selection_box(screen_cr);
		for (i = 0; i < MAX_NUM_SOUND_SHAPES; i++) {
			SoundShape *ss = &dd->sound_shapes[i];
			if (!ss->active) continue;
			GdkRectangle ss_rect;
			ss_rect.x = ss->pos.x - ss->r * ss->scale;
			ss_rect.y = ss->pos.y - ss->r * ss->scale;
			ss_rect.width = 2 * ss->r * ss->scale;
			ss_rect.height = 2 * ss->r * ss->scale;
			if (gdk_rectangle_intersect(&ss_rect, &dd->selection_rect, NULL)) {
				ss->selected = 1;
				ss->selected_pos.x = ss->pos.x;
				ss->selected_pos.y = ss->pos.y;
			} else {
				ss->selected = 0;
			}
		}
	}
	//
	cairo_restore(screen_cr);
	if (render_drawing_surf) {
		render_pointer(drawing_cr, dd->mouse_pos.x, dd->mouse_pos.y);
		//gtk_widget_draw(GTK_WIDGET(dd->ctl_window), drawing_cr);
	}
	render_pointer(screen_cr, dd->scale * dd->mouse_pos.x, dd->scale * dd->mouse_pos.y);
	if (dd->doing_tld) {
		if (render_drawing_surf) {
			render_detection_box(drawing_cr, 0, dd->ascale_factor_x*newbox.rect.x,
								 dd->ascale_factor_y*newbox.rect.y, dd->ascale_factor_x*newbox.rect.width,
								 dd->ascale_factor_y*newbox.rect.height);
		}
		render_detection_box(screen_cr, 0, dd->ascale_factor_x*newbox.rect.x,
							 dd->ascale_factor_y*newbox.rect.y, dd->ascale_factor_x*newbox.rect.width,
							 dd->ascale_factor_y*newbox.rect.height);
	}

	clock_gettime(CLOCK_REALTIME, &snapshot_ts);
	int drawing_size = 4 * dd->drawing_frame->width * dd->drawing_frame->height;
	uint tsize = drawing_size + sizeof(struct timespec);
	if (dd->do_snapshot) {
		if (jack_ringbuffer_write_space(dd->snapshot_thread_info.ring_buf) >= (size_t)tsize) {
			jack_ringbuffer_write(dd->snapshot_thread_info.ring_buf, (const char *)dd->drawing_frame->data[0],
					drawing_size);
			jack_ringbuffer_write(dd->snapshot_thread_info.ring_buf, (const char *)&snapshot_ts,
								  sizeof(ts));
			if (pthread_mutex_trylock(&dd->snapshot_thread_info.lock) == 0) {
				pthread_cond_signal(&dd->snapshot_thread_info.data_ready);
				pthread_mutex_unlock(&dd->snapshot_thread_info.lock);
			}
		}
		dd->do_snapshot = 0;
	}
	if (dd->recording_stopped) {
		if (pthread_mutex_trylock(&dd->video_thread_info.lock) == 0) {
			pthread_cond_signal(&dd->video_thread_info.data_ready);
			pthread_mutex_unlock(&dd->video_thread_info.lock);
		}
	}
	if (dd->recording_started && !dd->recording_stopped) {
		clock_gettime(CLOCK_MONOTONIC, &ts);
		if (first_call) {
			first_call = 0;
			dd->video_thread_info.stream.first_time = ts;
			dd->can_capture = 1;
		}
		tsize = drawing_size + sizeof(struct timespec);
		if (jack_ringbuffer_write_space(video_ring_buf) >= tsize) {
			jack_ringbuffer_write(video_ring_buf, (const char *)dd->drawing_frame->data[0],
					drawing_size);
			jack_ringbuffer_write(video_ring_buf, (const char *)&ts,
								  sizeof(struct timespec));
			if (pthread_mutex_trylock(&dd->video_thread_info.lock) == 0) {
				pthread_cond_signal(&dd->video_thread_info.data_ready);
				pthread_mutex_unlock(&dd->video_thread_info.lock);
			}
		}
	}
	cairo_destroy(sources_cr);
	cairo_destroy(drawing_cr);
	cairo_surface_destroy(sources_surf);
	cairo_surface_destroy(drawing_surf);
	clock_gettime(CLOCK_MONOTONIC, &end_ts);
	struct timespec diff_ts;
	timespec_diff(&start_ts, &end_ts, &diff_ts);
	printf("process_image time: %f\n", timespec_to_seconds(&diff_ts)*1000);
}

double hanning_window(int i, int N) {
	return 0.5 * (1 - cos(2 * M_PI * i / (N - 1)));
}

int process(jack_nframes_t nframes, void *arg) {
	DingleDots *dd = (DingleDots *)arg;
	static int first_call = 1;
	if (!dd->can_process) return 0;
	midi_process_output(nframes, dd);
	for (int chn = 0; chn < dd->nports; chn++) {
		dd->in[chn] = (jack_default_audio_sample_t *)jack_port_get_buffer(dd->in_ports[chn], nframes);
		dd->out[chn] = (jack_default_audio_sample_t *)jack_port_get_buffer(dd->out_ports[chn], nframes);
		kmeter_process(&dd->meters[chn], dd->in[chn], nframes);
	}
	if (nframes >= FFT_SIZE) {
		for (int i = 0; i < FFT_SIZE; i++) {
			fftw_in[i][0] = dd->in[0][i] * hanning_window(i, FFT_SIZE);
			fftw_in[i][1] = 0.0;
		}
		fftw_execute(p);
	}
	if (first_call) {
		struct timespec *ats = &dd->audio_thread_info.stream.first_time;
		clock_gettime(CLOCK_MONOTONIC, ats);
		dd->audio_thread_info.stream.samples_count = 0;
		first_call = 0;
	}
	for (uint i = 0; i < nframes; i++) {
		for (int chn = 0; chn < dd->nports; chn++) {
			dd->out[chn][i] = dd->in[chn][i];
		}
	}
	for (int i = 0; i < MAX_NUM_VIDEO_FILES; ++i) {
		VideoFile *vf = &dd->vf[i];
		if (vf->active && vf->audio_playing && !vf->paused) {
			jack_default_audio_sample_t sample;
			if (vf->audio_decoding_started) {
				if (vf->audio_decoding_finished &&
						jack_ringbuffer_read_space(vf->abuf) == 0) {
					vf->audio_playing = 0;
					if (pthread_mutex_trylock(&vf->video_lock) == 0) {
						pthread_cond_signal(&vf->video_data_ready);
						pthread_mutex_unlock(&vf->video_lock);
					}
				} else{
					for (uint frm = 0; frm < nframes; ++frm) {
						for (int chn = 0; chn < 2; ++chn) {
							if (jack_ringbuffer_read_space(vf->abuf) >= sizeof(sample)) {
								jack_ringbuffer_read(vf->abuf, (char *)&sample, sizeof(sample));
								dd->out[chn][frm] += sample;
							}
						}
					}
					vf->nb_frames_played += nframes;
					if (pthread_mutex_trylock(&vf->audio_lock) == 0) {
						pthread_cond_signal(&vf->audio_data_ready);
						pthread_mutex_unlock(&vf->audio_lock);
					}
				}
			}
		}
	}
	if (dd->recording_started && !dd->audio_done) {
		for (uint i = 0; i < nframes; i++) {
			for (int chn = 0; chn < dd->nports; chn++) {
				if (jack_ringbuffer_write (audio_ring_buf, (const char *) (dd->out[chn]+i),
										   sample_size) < sample_size) {
					printf("jack overrun: %ld\n", ++dd->jack_overruns);
				}
			}
		}
		if (pthread_mutex_trylock (&dd->audio_thread_info.lock) == 0) {
			pthread_cond_signal (&dd->audio_thread_info.data_ready);
			pthread_mutex_unlock (&dd->audio_thread_info.lock);
		}
	}
	return 0;
}

void jack_shutdown (void *) {
	printf("JACK shutdown\n");
	abort();
}

void setup_jack(DingleDots *dd) {
	size_t in_size;
	dd->can_process = 0;
	dd->jack_overruns = 0;
	if ((dd->client = jack_client_open("v4l2_wayland",
									   JackNoStartServer, NULL)) == 0) {
		printf("jack server not running?\n");
		exit(1);
	}
	jack_set_process_callback(dd->client, process, dd);
	jack_on_shutdown(dd->client, jack_shutdown, NULL);
	if (jack_activate(dd->client)) {
		printf("cannot activate jack client\n");
	}
	dd->in_ports = (jack_port_t **) malloc(sizeof(jack_port_t *) * dd->nports);
	dd->out_ports = (jack_port_t **) malloc(sizeof(jack_port_t *) * dd->nports);
	in_size =  dd->nports * sizeof (jack_default_audio_sample_t *);
	dd->in = (jack_default_audio_sample_t **) malloc (in_size);
	dd->out = (jack_default_audio_sample_t **) malloc (in_size);
	audio_ring_buf = jack_ringbuffer_create (dd->nports * sample_size *
											 16384);
	memset(dd->in, 0, in_size);
	memset(dd->out, 0, in_size);
	memset(audio_ring_buf->buf, 0, audio_ring_buf->size);
	dd->midi_ring_buf = jack_ringbuffer_create(MIDI_RB_SIZE);
	fftw_in = (fftw_complex *)fftw_malloc(sizeof(fftw_complex) * FFT_SIZE);
	fftw_out = (fftw_complex *)fftw_malloc(sizeof(fftw_complex) * FFT_SIZE);
	p = fftw_plan_dft_1d(FFT_SIZE, fftw_in, fftw_out, FFTW_FORWARD, FFTW_ESTIMATE);
	for (int i = 0; i < dd->nports; i++) {
		char name[64];
		sprintf(name, "input%d", i + 1);
		if ((dd->in_ports[i] = jack_port_register (dd->client, name, JACK_DEFAULT_AUDIO_TYPE,
												   JackPortIsInput, 0)) == 0) {
			printf("cannot register input port \"%s\"!\n", name);
			jack_client_close(dd->client);
			exit(1);
		}
		sprintf(name, "output%d", i + 1);
		if ((dd->out_ports[i] = jack_port_register (dd->client, name, JACK_DEFAULT_AUDIO_TYPE,
													JackPortIsOutput, 0)) == 0) {
			printf("cannot register output port \"%s\"!\n", name);
			jack_client_close(dd->client);
			exit(1);
		}
	}
	dd->midi_port = jack_port_register(dd->client, "output_midi",
									   JACK_DEFAULT_MIDI_TYPE, JackPortIsOutput, 0);
	dd->can_process = 1;
}

void teardown_jack(DingleDots *dd) {
	while (jack_ringbuffer_read_space(dd->midi_ring_buf)) {
		struct timespec pause;
		pause.tv_sec = 0;
		pause.tv_nsec = 1000;
		nanosleep(&pause, NULL);
	}
	jack_client_close(dd->client);
}

void start_recording(DingleDots *dd) {
	struct timespec ts;
	if (strlen(dd->video_file_name) == 0) {
		clock_gettime(CLOCK_REALTIME, &ts);
		timespec2file_name(dd->video_file_name, STR_LEN, "Videos", "webm", &ts);
	}
	init_output(dd);
	dd->recording_started = 1;
	dd->recording_stopped = 0;
	pthread_create(&dd->audio_thread_info.thread_id, NULL, audio_disk_thread,
				   dd);
	pthread_create(&dd->video_thread_info.thread_id, NULL, video_disk_thread,
				   dd);
}

void stop_recording(DingleDots *dd) {
	dd->recording_stopped = 1;
	gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(dd->record_button), 0);
	gtk_widget_set_sensitive(dd->record_button, 0);
}

static gboolean configure_event_cb (GtkWidget *,
									GdkEventConfigure *event, gpointer data) {
	DingleDots *dd;
	dd = (DingleDots *)data;
	dd->scale = (double)(event->height) / dd->drawing_rect.height;
	return TRUE;
}

static gboolean window_state_event_cb (GtkWidget *,
									   GdkEventWindowState *event, gpointer   data) {
	DingleDots *dd;
	dd = (DingleDots *)data;
	if (event->new_window_state & GDK_WINDOW_STATE_FULLSCREEN) {
		dd->fullscreen = TRUE;
	} else {
		dd->fullscreen = FALSE;
	}
	return TRUE;
}

static gint queue_draw_timeout_cb(gpointer data) {
	DingleDots *dd;
	dd = (DingleDots *)data;
	if (dd->recording_started && !dd->recording_stopped) {
		gtk_widget_queue_draw(dd->drawing_area);
	}
	return TRUE;
}

static gboolean draw_cb (GtkWidget *, cairo_t *cr, gpointer   data) {
	DingleDots *dd;
	dd = (DingleDots *)data;
	process_image(cr, dd);
	return TRUE;
}

void mark_hovered(bool use_sources, DingleDots *dd) {
	int found = 0;
	std::vector<Drawable *> sources;
	get_sources(dd, sources);
	std::vector<Drawable *> sound_shapes;
	dd->get_sound_shapes(sound_shapes);
	if (use_sources) {
		for (std::vector<Drawable *>::iterator it = sound_shapes.begin(); it != sound_shapes.end(); ++it) {
			if ((*it)->hovered == 1) {
				(*it)->hovered = 0;
				gtk_widget_queue_draw(dd->drawing_area);
			}
		}
		std::sort(sources.begin(), sources.end(), [](Drawable *a, Drawable *b) { return a->z > b->z; } );
		for (std::vector<Drawable *>::iterator it = sources.begin(); it != sources.end(); ++it) {
			if (found) {
				if ((*it)->hovered == 1) {
					(*it)->hovered = 0;
					gtk_widget_queue_draw(dd->drawing_area);
				}
			} else if ((*it)->in(dd->mouse_pos.x, dd->mouse_pos.y)) {
				found = 1;
				if ((*it)->hovered == 0) {
					(*it)->hovered = 1;
					gtk_widget_queue_draw(dd->drawing_area);
				}
			} else {
				if ((*it)->hovered == 1) {
					(*it)->hovered = 0;
					gtk_widget_queue_draw(dd->drawing_area);
				}
			}
		}
	} else {
		for (std::vector<Drawable *>::iterator it = sources.begin(); it != sources.end(); ++it) {
			if ((*it)->hovered == 1) {
				(*it)->hovered = 0;
				gtk_widget_queue_draw(dd->drawing_area);
			}
		}
		found = 0;
		std::sort(sound_shapes.begin(), sound_shapes.end(), [](Drawable *a, Drawable *b) { return a->z > b->z; } );
		for (std::vector<Drawable *>::iterator it = sound_shapes.begin(); it != sound_shapes.end(); ++it) {
			Drawable *s = *it;
			if (!found && s->in(dd->mouse_pos.x, dd->mouse_pos.y)) {
				s->hovered = 1;
				found = 1;
				gtk_widget_queue_draw(dd->drawing_area);
			} else if (s->hovered == 1) {
				s->hovered = 0;
				gtk_widget_queue_draw(dd->drawing_area);
			}
		}
	}
}

static gboolean motion_notify_event_cb(GtkWidget *,
									   GdkEventMotion *event, gpointer data) {
	int i;
	DingleDots *dd = (DingleDots *)data;
	dd->mouse_pos.x = event->x / dd->scale;
	dd->mouse_pos.y = event->y / dd->scale;
	if (!dd->dragging && !dd->selection_in_progress)
		mark_hovered(event->state & GDK_SHIFT_MASK, dd);
	if (dd->smdown) {
		dd->user_tld_rect.width = dd->mouse_pos.x - dd->mdown_pos.x;
		dd->user_tld_rect.height = dd->mouse_pos.y - dd->mdown_pos.y;
		if (dd->user_tld_rect.width < 0) {
			if (dd->user_tld_rect.width > - 20 * dd->ascale_factor_x) {
				dd->user_tld_rect.width = 20 * dd->ascale_factor_x;
			} else {
				dd->user_tld_rect.width = -dd->user_tld_rect.width;
			}
			dd->user_tld_rect.x = dd->mdown_pos.x - dd->user_tld_rect.width;
		} else if (dd->user_tld_rect.width >= 0) {
			if (dd->user_tld_rect.width < 20 * dd->ascale_factor_x) {
				dd->user_tld_rect.width = 20 * dd->ascale_factor_x;
			}
			dd->user_tld_rect.x = dd->mdown_pos.x;
		}
		if (dd->user_tld_rect.height < 0) {
			if (dd->user_tld_rect.height > - 20 * dd->ascale_factor_y) {
				dd->user_tld_rect.height = 20 * dd->ascale_factor_y;
			} else {
				dd->user_tld_rect.height = -dd->user_tld_rect.height;
			}
			dd->user_tld_rect.y = dd->mdown_pos.y - dd->user_tld_rect.height;
		} else if (dd->user_tld_rect.width >= 0) {
			if (dd->user_tld_rect.height < 20 * dd->ascale_factor_y) {
				dd->user_tld_rect.height = 20 * dd->ascale_factor_y;
			}
			dd->user_tld_rect.y = dd->mdown_pos.y;
		}
	}
	if (dd->selection_in_progress) {
		if (dd->mdown) {
			dd->selection_rect.width = dd->mouse_pos.x - dd->mdown_pos.x;
			dd->selection_rect.height = dd->mouse_pos.y - dd->mdown_pos.y;
		} else {
			dd->selection_rect.width = dd->mup_pos.x - dd->mdown_pos.x;
			dd->selection_rect.height = dd->mup_pos.y - dd->mdown_pos.y;
		}
		if (dd->selection_rect.width < 0) {
			dd->selection_rect.width = -dd->selection_rect.width;
			dd->selection_rect.x = dd->mdown_pos.x - dd->selection_rect.width;
		} else if (dd->selection_rect.width >= 0) {
			dd->selection_rect.x = dd->mdown_pos.x;
		}
		if (dd->selection_rect.height < 0) {
			dd->selection_rect.height = -dd->selection_rect.height;
			dd->selection_rect.y = dd->mdown_pos.y - dd->selection_rect.height;
		} else if (dd->selection_rect.height >= 0) {
			dd->selection_rect.y = dd->mdown_pos.y;
		}
	} else if (dd->mdown) {
		dd->dragging = 1;
	}
	if (!(event->state & GDK_SHIFT_MASK)) {
		std::vector<Drawable *> sound_shapes;
		for (i = 0; i < MAX_NUM_SOUND_SHAPES; ++i) {
			SoundShape *s = &dd->sound_shapes[i];
			if (s->active) sound_shapes.push_back(s);
		}
		std::sort(sound_shapes.begin(), sound_shapes.end(), [](Drawable *a, Drawable *b) { return a->z > b->z; } );
		for (std::vector<Drawable *>::iterator it = sound_shapes.begin(); it != sound_shapes.end(); ++it) {
			Drawable *s = *it;
			if (s->mdown) {
				if (s->selected) {
					for (std::vector<Drawable *>::iterator ij = sound_shapes.begin(); ij != sound_shapes.end(); ++ij) {
						Drawable *sj = *ij;
						if (sj->selected) {
							sj->pos.x = dd->mouse_pos.x -
									s->mdown_pos.x +
									sj->selected_pos.x;
							sj->pos.y = dd->mouse_pos.y -
									s->mdown_pos.y +
									sj->selected_pos.y;
						}
					};
				} else {
					s->drag(dd->mouse_pos.x, dd->mouse_pos.y);
				}
				break;
			}
		}
	} else {
		std::vector<Drawable *> sources;
		get_sources(dd, sources);
		std::sort(sources.begin(), sources.end(), [](Drawable *a, Drawable *b) { return a->z > b->z; } );
		for (std::vector<Drawable *>::iterator it = sources.begin(); it != sources.end(); ++it) {
			if ((*it)->active) {
				if ((*it)->mdown) {
					(*it)->drag(dd->mouse_pos.x, dd->mouse_pos.y);
					break;
				}
			}
		}
	}
	gtk_widget_queue_draw(dd->drawing_area);
	return TRUE;
}

static gboolean double_press_event_cb(GtkWidget *widget,
									  GdkEventButton *event, gpointer data) {
	DingleDots * dd = (DingleDots *)data;
	if (event->type == GDK_2BUTTON_PRESS &&
			event->button == GDK_BUTTON_PRIMARY &&
			!dd->delete_active) {
		uint8_t found = 0;
		double x, y;
		x = event->x / dd->scale;;
		y = event->y / dd->scale;
		if (!(event->state & GDK_SHIFT_MASK)) {
			for (int i = 0; i < MAX_NUM_SOUND_SHAPES; ++i) {
				if (!dd->sound_shapes[i].active) continue;
				if (dd->sound_shapes[i].in(x, y)) {
					found = 1;
					if (dd->sound_shapes[i].double_clicked_on) {
						dd->sound_shapes[i].double_clicked_on = 0;
					} else {
						dd->sound_shapes[i].double_clicked_on = 1;
					}
				}
			}
		} else {
			std::vector<VideoFile *> video_files;

			for (int i = 0; i < MAX_NUM_VIDEO_FILES; ++i) {
				VideoFile *vf = &dd->vf[i];
				if (vf->active) {
					video_files.push_back(vf);
				}
			}
			std::sort(video_files.begin(), video_files.end(), [](VideoFile *a, VideoFile *b) { return a->z > b->z; });
			for (std::vector<VideoFile *>::iterator it = video_files.begin();
				 it != video_files.end(); ++it) {
				VideoFile *vf = *it;
				if (vf->in(x, y)) {
					vf->toggle_play_pause();
					found = 1;
					break;
				}
			}
		}
		if (found) return TRUE;
		GtkWidget *toplevel = gtk_widget_get_toplevel(widget);
		if (gtk_widget_is_toplevel(toplevel)) {
			if (!dd->fullscreen) gtk_window_fullscreen(GTK_WINDOW(toplevel));
			else gtk_window_unfullscreen(GTK_WINDOW(toplevel));
			return TRUE;
		}
	}
	return FALSE;
}

static gboolean button_press_event_cb(GtkWidget *,
									  GdkEventButton *event, gpointer data) {
	int i;
	DingleDots * dd = (DingleDots *)data;
	dd->mouse_pos.x = event->x / dd->scale;
	dd->mouse_pos.y = event->y / dd->scale;
	if (event->button == GDK_BUTTON_PRIMARY) {
		dd->mdown = 1;
		dd->mdown_pos.x = dd->mouse_pos.x;
		dd->mdown_pos.y = dd->mouse_pos.y;
		if (!(event->state & GDK_SHIFT_MASK)) {
			std::vector<Drawable *> sound_shapes;
			for (i = 0; i < MAX_NUM_SOUND_SHAPES; ++i) {
				SoundShape *s = &dd->sound_shapes[i];
				if (s->active) sound_shapes.push_back(s);
			}
			std::sort(sound_shapes.begin(), sound_shapes.end(), [](Drawable *a, Drawable *b) { return a->z > b->z; } );
			for (std::vector<Drawable *>::iterator it = sound_shapes.begin(); it != sound_shapes.end(); ++it) {
				Drawable *s = *it;
				if (s->in(dd->mouse_pos.x, dd->mouse_pos.y)) {
					if (dd->delete_active) {
						s->deactivate();
					} else {
						if (!s->selected) {
							for (std::vector<Drawable *>::iterator ij = sound_shapes.begin(); ij != sound_shapes.end(); ++ij) {
								Drawable *sj = *ij;
								if (sj->selected) {
									sj->selected = 0;
									gtk_widget_queue_draw(dd->drawing_area);
								}
							}
						} else {
							for (std::vector<Drawable *>::reverse_iterator ij = sound_shapes.rbegin(); ij != sound_shapes.rend(); ++ij) {
								Drawable *sj = *ij;
								if (sj->selected) {
									sj->z = dd->next_z++;
									sj->selected_pos.x = sj->pos.x;
									sj->selected_pos.y = sj->pos.y;
								}
							}
						}
						s->set_mdown(dd->mouse_pos.x, dd->mouse_pos.y, dd->next_z++);
						gtk_widget_queue_draw(dd->drawing_area);
					}
					return FALSE;
				}
			}
			for (std::vector<Drawable *>::iterator ij = sound_shapes.begin(); ij != sound_shapes.end(); ++ij) {
				Drawable *sj = *ij;
				sj->selected = 0;
			}
			gtk_widget_queue_draw(dd->drawing_area);
		} else {
			std::vector<Drawable *> sources;
			get_sources(dd, sources);
			std::sort(sources.begin(), sources.end(), [](Drawable *a, Drawable *b) { return a->z > b->z; } );
			for (std::vector<Drawable *>::iterator it = sources.begin(); it != sources.end(); ++it) {
				if ((*it)->in(dd->mouse_pos.x, dd->mouse_pos.y)) {
					(*it)->set_mdown(dd->mouse_pos.x, dd->mouse_pos.y,
									 event->state & GDK_CONTROL_MASK ?
										 (*it)->z : dd->next_z++);
					break;
				}
			}
			return false;
		}
		dd->set_selecting_on();
		dd->selection_rect.x = dd->mouse_pos.x;
		dd->selection_rect.y = dd->mouse_pos.y;
		dd->selection_rect.width = 0;
		dd->selection_rect.height = 0;
		return FALSE;
	}

//	if (!dd->shift_pressed && event->button == GDK_BUTTON_SECONDARY) {
//		dd->smdown = 1;
//		dd->mdown_pos.x = dd->mouse_pos.x;
//		dd->mdown_pos.y = dd->mouse_pos.y;
//		dd->user_tld_rect.x = dd->mdown_pos.x;
//		dd->user_tld_rect.y = dd->mdown_pos.y;
//		dd->user_tld_rect.width = 20 * dd->ascale_factor_x;
//		dd->user_tld_rect.height = 20 * dd->ascale_factor_y;
//		return TRUE;
//	}
//	if (dd->doing_tld && dd->shift_pressed) {
//		if (event->button == GDK_BUTTON_SECONDARY) {
//			dd->doing_tld = 0;
//			return TRUE;
//		}
//	}
	return FALSE;
}

static gboolean button_release_event_cb(GtkWidget *,
										GdkEventButton *event, gpointer data) {
	DingleDots * dd;
	int i;
	dd = (DingleDots *)data;
	dd->mouse_pos.x = event->x / dd->scale;
	dd->mouse_pos.y = event->y / dd->scale;
	dd->mup_pos.x = dd->mouse_pos.x;
	dd->mup_pos.y = dd->mouse_pos.y;
	mark_hovered(event->state & GDK_SHIFT_MASK, dd);
	if (event->button == GDK_BUTTON_PRIMARY) {
		if (!dd->dragging && !dd->selection_in_progress) {

			for (i = 0; i < MAX_NUM_SOUND_SHAPES; i++) {
				if (!dd->sound_shapes[i].active) continue;
				if (!dd->sound_shapes[i].mdown) {
					dd->sound_shapes[i].selected = 0;
				}
			}
		}
		for (i = 0; i < MAX_NUM_SOUND_SHAPES; i++) {
			if (!dd->sound_shapes[i].active) continue;
			dd->sound_shapes[i].mdown = 0;
		}
		std::vector<Drawable *> sources;
		get_sources(dd, sources);
		for (std::vector<Drawable *>::iterator it = sources.begin(); it != sources.end(); ++it) {
			if ((*it)->active) {
				(*it)->mdown = 0;
			}
		}
		dd->mdown = 0;
		dd->dragging = 0;
		if (dd->selection_in_progress) {
			Easer *e = new Easer();
			e->initialize(dd, EASER_LINEAR, boost::bind(&DingleDots::set_selection_box_alpha, dd, _1), 1.0, 0.0, 0.2);
			e->add_finish_action(boost::bind(&DingleDots::set_selecting_off, dd));
			dd->add_easer(e);
			e->start();
		}
		gtk_widget_queue_draw(dd->drawing_area);
		return TRUE;
	} /*else if (!(event->state & GDK_SHIFT_MASK) && event->button == GDK_BUTTON_SECONDARY) {
		dd->smdown = 0;
		dd->make_new_tld = 1;
		dd->doing_tld = 1;
		return TRUE;
	}*/
	return FALSE;
}

void apply_scrolling_operations_to_list(GdkEventScroll *event, DingleDots *dd,
										std::vector<Drawable *> drawables)
{
	gboolean up = FALSE;
	if (event->delta_y == -1.0) {
		up = TRUE;
	}
	for (std::vector<Drawable *>::iterator it = drawables.begin(); it != drawables.end(); ++it) {
		if ((*it)->in(dd->mouse_pos.x, dd->mouse_pos.y)) {
			if (event->state & GDK_MOD1_MASK) {
				double inc = 0.025;
				double o = (*it)->get_opacity();
				(*it)->set_opacity(o + (up ? inc: -inc));
			} else if (dd->s_pressed){
				double inc = 0.05;
				(*it)->set_scale((*it)->get_scale() + (up ? inc: -inc));
			} else if (event->state & GDK_CONTROL_MASK) {
				double inc = 2 * M_PI / 180;
				(*it)->rotate(up ? inc : -inc);
			}
			gtk_widget_queue_draw(dd->drawing_area);
			break;
		}
	}
}

static gboolean scroll_cb(GtkWidget *, GdkEventScroll *event,
						  gpointer data) {
	DingleDots *dd = (DingleDots *)data;

	if (!(event->state & GDK_SHIFT_MASK)) {
		std::vector<Drawable *> sound_shapes;
		dd->get_sound_shapes(sound_shapes);
		std::sort(sound_shapes.begin(), sound_shapes.end(), [](Drawable *a, Drawable *b) { return a->z > b->z; } );
		apply_scrolling_operations_to_list(event, dd, sound_shapes);
		return TRUE;
	} else {
		std::vector<Drawable *> sources;
		dd->get_sources(sources);
		std::sort(sources.begin(), sources.end(), [](Drawable *a, Drawable *b) { return a->z > b->z; } );
		apply_scrolling_operations_to_list(event, dd, sources);
		return TRUE;
	}

	return FALSE;
}

static gboolean on_key_press(GtkWidget *widget, GdkEventKey *event,
							 gpointer data) {
	DingleDots *dd = (DingleDots *)data;
	if (event->keyval == GDK_KEY_q && (!dd->recording_started ||
									   dd->recording_stopped)) {
		g_application_quit(dd->app);
	} else if (event->keyval == GDK_KEY_r && (!dd->recording_started)) {
		gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(dd->record_button), 1);
		return TRUE;
	} else if (event->keyval == GDK_KEY_r && (dd->recording_started)) {
		gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(dd->record_button), 0);
		return TRUE;
	} else if (event->keyval == GDK_KEY_d) {
		if (!dd->delete_active) {
			gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(dd->delete_button), 1);
		} else {
			gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(dd->delete_button), 0);
		}
	} else if (event->keyval == GDK_KEY_s ||
			   event->keyval == GDK_KEY_S) {
		dd->s_pressed = 1;
		return TRUE;
	} else if (event->keyval == GDK_KEY_Shift_L ||
			   event->keyval == GDK_KEY_Shift_R) {
		if (!dd->selection_in_progress)
			mark_hovered(1, dd);
		return TRUE;
	} else if (event->keyval == GDK_KEY_Escape && dd->fullscreen) {
		GtkWidget *toplevel = gtk_widget_get_toplevel(widget);
		if (gtk_widget_is_toplevel(toplevel)) {
			gtk_window_unfullscreen(GTK_WINDOW(toplevel));
			return TRUE;
		}
	}
	return FALSE;
}

static gboolean on_key_release(GtkWidget *, GdkEventKey *event,
							   gpointer data) {
	DingleDots *dd = (DingleDots *)data;
	if (event->keyval == GDK_KEY_s || event->keyval == GDK_KEY_S) {
		dd->s_pressed = 0;
	}
	if (event->keyval == GDK_KEY_Shift_L ||
			event->keyval == GDK_KEY_Shift_R) {
		if (!dd->dragging && !dd->selection_in_progress) mark_hovered(0, dd);
		return TRUE;
	}
	return FALSE;
}

static gboolean delete_cb(GtkWidget *, gpointer data) {
	DingleDots * dd;
	dd = (DingleDots *)data;
	if (!dd->delete_active) {
		dd->delete_active = 1;
	} else {
		dd->delete_active = 0;
	}
	return TRUE;
}

static gboolean record_cb(GtkWidget *, gpointer data) {
	DingleDots * dd;
	dd = (DingleDots *)data;
	if (!dd->recording_started && !dd->recording_stopped) {
		start_recording(dd);
		gtk_widget_queue_draw(dd->drawing_area);
	} else if (dd->recording_started && !dd->recording_stopped) {
		stop_recording(dd);
	}
	return TRUE;
}

static gboolean quit_cb(GtkWidget *, gpointer data) {
	DingleDots * dd;
	dd = (DingleDots *)data;
	g_application_quit(dd->app);
	return TRUE;
}
static gboolean show_sprite_cb(GtkWidget *, gpointer data) {
	DingleDots * dd;
	dd = (DingleDots *)data;
	GtkWidget *dialog;
	GtkFileChooserAction action = GTK_FILE_CHOOSER_ACTION_OPEN;
	gint res;
	dialog = gtk_file_chooser_dialog_new ("Open File",
										  GTK_WINDOW(dd->ctl_window),
										  action,
										  "Cancel",
										  GTK_RESPONSE_CANCEL,
										  "Open",
										  GTK_RESPONSE_ACCEPT,
										  NULL);
	res = gtk_dialog_run (GTK_DIALOG (dialog));
	if (res == GTK_RESPONSE_ACCEPT) {
		gchar *fname;
		GtkFileChooser *chooser = GTK_FILE_CHOOSER (dialog);
		fname = gtk_file_chooser_get_filename(chooser);
		std::string filename(fname);
		for (int index = 0; index < MAX_NUM_SPRITES; ++index) {
			Sprite *s = &dd->sprites[index];
			if (!s->active) {
				s->create(&filename, dd->next_z++, dd);
				gtk_widget_queue_draw(dd->drawing_area);
				break;
			}
		}
		g_free (fname);
	}
	gtk_widget_destroy (dialog);
	return TRUE;
}

static gboolean play_file_cb(GtkWidget *, gpointer data) {
	DingleDots * dd;
	dd = (DingleDots *)data;
	GtkWidget *dialog;
	GtkFileChooserAction action = GTK_FILE_CHOOSER_ACTION_OPEN;
	gint res;
	dialog = gtk_file_chooser_dialog_new ("Open File",
										  GTK_WINDOW(dd->ctl_window),
										  action,
										  "Cancel",
										  GTK_RESPONSE_CANCEL,
										  "Open",
										  GTK_RESPONSE_ACCEPT,
										  NULL);
	res = gtk_dialog_run (GTK_DIALOG (dialog));
	if (res == GTK_RESPONSE_ACCEPT) {
		char *filename;
		GtkFileChooser *chooser = GTK_FILE_CHOOSER (dialog);
		filename = gtk_file_chooser_get_filename (chooser);
		for (int i = 0; i < MAX_NUM_VIDEO_FILES; ++i) {
			if(!dd->vf[i].allocated) {
				dd->vf[i].dingle_dots = dd;
				dd->vf[i].create(filename, 0.0, 0.0, dd->next_z++);
				break;
			}
		}
		g_free (filename);
	}
	gtk_widget_destroy (dialog);
	return TRUE;
}

static gboolean motion_cb(GtkWidget *widget, gpointer data) {
	DingleDots *dd = (DingleDots*) data;
	if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget))) {
		dd->doing_motion = 1;
	} else {
		dd->doing_motion = 0;
	}
	return TRUE;
}

static gboolean snapshot_shape_cb(GtkWidget *widget, gpointer data) {
	DingleDots *dd = (DingleDots*) data;
	if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget))) {
		dd->snapshot_shape.activate();
		//dd->show_shapshot_shape = 1;
	} else {
		dd->snapshot_shape.deactivate();
		//dd->show_shapshot_shape = 0;
	}
	gtk_widget_queue_draw(dd->drawing_area);
	return TRUE;
}

static gboolean rand_color_cb(GtkWidget *widget, gpointer data) {
	DingleDots * dd;
	dd = (DingleDots *)data;
	if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget))) {
		dd->use_rand_color_for_scale = 1;
		gtk_widget_set_sensitive(dd->scale_color_button, 0);
	} else {
		dd->use_rand_color_for_scale = 0;
		gtk_widget_set_sensitive(dd->scale_color_button, 1);
	}
	return TRUE;
}

static gboolean set_modes_cb(GtkWidget *widget, gpointer data) {
	GtkComboBoxText *resolution_combo = (GtkComboBoxText *) data;
	gtk_combo_box_text_remove_all(resolution_combo);
	std::vector<std::pair<int, int>> width_height;
	gchar *name = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(widget));
	V4l2::get_dimensions(name, width_height);
	int index = 0;

	for (std::vector<std::pair<int,int>>::iterator it = width_height.begin();
		 it != width_height.end(); ++it) {
		char index_str[64];
		char mode_str[64];
		memset(index_str, '\0', sizeof(index_str));
		memset(mode_str, '\0', sizeof(index_str));
		snprintf(index_str, 63, "%d", index);
		snprintf(mode_str, 63, "%dx%d", width_height.at(index).first,
				 width_height.at(index).second);
		gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(resolution_combo), index_str, mode_str);
		++index;
	}
	gtk_combo_box_set_active(GTK_COMBO_BOX(resolution_combo), 0);
	return TRUE;
}

static gboolean camera_cb(GtkWidget *, gpointer data) {
	DingleDots * dd;
	dd = (DingleDots *)data;
	GtkWidget *dialog;
	GtkWidget *dialog_content;
	GtkWidget *combo;
	GtkWidget *resolution_combo;
	int res;
	int index;
	GtkDialogFlags flags = (GtkDialogFlags)(GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT);
	dialog = gtk_dialog_new_with_buttons("Open Camera", GTK_WINDOW(dd->ctl_window),
										 flags, "Open", GTK_RESPONSE_ACCEPT, "Cancel", GTK_RESPONSE_REJECT, NULL);
	dialog_content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
	combo = gtk_combo_box_text_new();

	std::vector<std::string> files;
	V4l2::list_devices(files);
	index = 0;
	for (std::vector<std::string>::iterator it = files.begin();
		 it != files.end(); ++it) {
		char index_str[64];
		memset(index_str, '\0', sizeof(index_str));
		snprintf(index_str, 63, "%d", index);
		gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(combo), index_str, (*it).c_str());
		++index;
	}
	resolution_combo = gtk_combo_box_text_new();
	gtk_container_add(GTK_CONTAINER(dialog_content), combo);

	g_signal_connect(combo, "changed", G_CALLBACK(set_modes_cb), resolution_combo);

	//gtk_combo_box_set_active(GTK_COMBO_BOX(resolution_combo), 1);
	gtk_container_add(GTK_CONTAINER(dialog_content), resolution_combo);
	gtk_widget_show_all(dialog);
	gtk_combo_box_set_active(GTK_COMBO_BOX(combo), 0);
	res = gtk_dialog_run(GTK_DIALOG(dialog));
	if (res == GTK_RESPONSE_ACCEPT) {
		gchar *name = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(combo));
		gchar *res_str = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(resolution_combo));
		gchar *w, *h;
		w = strsep(&res_str, "x");
		h = strsep(&res_str, "x");
		for(int i = 0; i < MAX_NUM_V4L2; i++) {
			if (!dd->v4l2[i].allocated) {
				dd->v4l2[i].create(dd, name, atof(w), atof(h), dd->next_z++);
				break;
			}
		}
	}
	gtk_widget_destroy (dialog);
	return TRUE;
}

static gboolean snapshot_cb(GtkWidget *, gpointer data) {
	DingleDots * dd;
	dd = (DingleDots *)data;
	dd->do_snapshot = 1;
	return TRUE;
}



static gboolean make_scale_cb(GtkWidget *, gpointer data) {
	DingleDots * dd;
	GdkRGBA gc;
	color c;
	dd = (DingleDots *)data;
	char *text_scale;
	char text_note[4];
	int channel;
	text_scale =
			gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(dd->scale_combo));
	sprintf(text_note, "%s",
			gtk_combo_box_get_active_id(GTK_COMBO_BOX(dd->note_combo)));
	midi_key_t key;
	midi_key_init_by_scale_id(&key, atoi(text_note),
							  midi_scale_text_to_id(text_scale));
	if (dd->use_rand_color_for_scale) {
		c = dd->random_color();
	} else {
		gtk_color_chooser_get_rgba(GTK_COLOR_CHOOSER(dd->scale_color_button), &gc);
		color_init(&c, gc.red, gc.green, gc.blue, gc.alpha);
	}
	channel = atoi(gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(dd->channel_combo)));
	dd->add_scale(&key, channel, &c);
	return TRUE;
}

static void activate(GtkApplication *app, gpointer user_data) {
	GtkWidget *window;
	GtkWidget *drawing_area;
	GtkWidget *note_hbox;
	GtkWidget *toggle_hbox;
	GtkWidget *vbox;
	GtkWidget *qbutton;
	GtkWidget *mbutton;
	GtkWidget *play_file_button;
	GtkWidget *show_sprite_button;
	GtkWidget *snapshot_button;
	GtkWidget *snapshot_shape_button;
	GtkWidget *camera_button;
	GtkWidget *make_scale_button;
	GtkWidget *aspect;
	GtkWidget *channel_hbox;
	GtkWidget *channel_label;
	DingleDots *dd;
	int ret;
	dd = (DingleDots *)user_data;
	ccv_enable_default_cache();
	dd->user_tld_rect.width = dd->drawing_rect.width/5.;
	dd->user_tld_rect.height = dd->drawing_rect.height/5.;
	dd->user_tld_rect.x = dd->drawing_rect.width/2.0;
	dd->user_tld_rect.y = dd->drawing_rect.height/2.0 - 0.5 * dd->user_tld_rect.height;
	dd->drawing_frame = av_frame_alloc();
	dd->drawing_frame->format = AV_PIX_FMT_ARGB;
	dd->drawing_frame->width = dd->drawing_rect.width;
	dd->drawing_frame->height = dd->drawing_rect.height;
	ret = av_image_alloc(dd->drawing_frame->data, dd->drawing_frame->linesize,
						 dd->drawing_frame->width, dd->drawing_frame->height, (AVPixelFormat)dd->drawing_frame->format, 1);
	if (ret < 0) {
		fprintf(stderr, "Could not allocate raw picture buffer\n");
		exit(1);
	}
	dd->sources_frame = av_frame_alloc();
	dd->sources_frame->format = AV_PIX_FMT_ARGB;
	dd->sources_frame->width = dd->drawing_rect.width;
	dd->sources_frame->height = dd->drawing_rect.height;
	ret = av_image_alloc(dd->sources_frame->data, dd->sources_frame->linesize,
						 dd->sources_frame->width, dd->sources_frame->height, (AVPixelFormat)dd->sources_frame->format, 1);
	if (ret < 0) {
		fprintf(stderr, "Could not allocate sources buffer\n");
		exit(1);
	}
	uint32_t rb_size = 5 * 4 * dd->drawing_frame->linesize[0] *
			dd->drawing_frame->height;
	video_ring_buf = jack_ringbuffer_create(rb_size);
	memset(video_ring_buf->buf, 0, video_ring_buf->size);
	dd->snapshot_thread_info.ring_buf = jack_ringbuffer_create(rb_size);
	memset(dd->snapshot_thread_info.ring_buf->buf, 0,
		   dd->snapshot_thread_info.ring_buf->size);
	window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
	gtk_window_set_resizable(GTK_WINDOW(window), TRUE);
	gtk_window_set_deletable(GTK_WINDOW (window), TRUE);
	gtk_window_set_default_size(GTK_WINDOW(window), dd->drawing_rect.width, dd->drawing_rect.height);
	dd->ctl_window = gtk_application_window_new(app);
	gtk_window_set_keep_above(GTK_WINDOW(dd->ctl_window), TRUE);
	gtk_window_set_title(GTK_WINDOW (dd->ctl_window), "Controls");
	g_signal_connect (window, "destroy", G_CALLBACK (quit_cb), dd);
	g_signal_connect (dd->ctl_window, "destroy", G_CALLBACK (quit_cb), dd);
	aspect = gtk_aspect_frame_new(NULL, 0.5, 0.5, ((float)dd->drawing_rect.width)/dd->drawing_rect.height, FALSE);
	gtk_frame_set_shadow_type(GTK_FRAME(aspect), GTK_SHADOW_NONE);
	drawing_area = gtk_drawing_area_new();
	dd->drawing_area = drawing_area;
	GdkGeometry size_hints;
	size_hints.min_aspect = ((double)dd->drawing_rect.width)/dd->drawing_rect.height;
	size_hints.max_aspect = ((double)dd->drawing_rect.width)/dd->drawing_rect.height;
	gtk_window_set_geometry_hints(GTK_WINDOW(window), NULL, &size_hints,
								  GDK_HINT_ASPECT);
	gtk_container_add(GTK_CONTAINER(aspect), drawing_area);
	note_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
	toggle_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
	vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
	dd->record_button = gtk_toggle_button_new_with_label("RECORD");
	dd->delete_button = gtk_toggle_button_new_with_label("DELETE");
	qbutton = gtk_button_new_with_label("QUIT");
	mbutton = gtk_check_button_new_with_label("MOTION DETECTION");
	snapshot_shape_button = gtk_check_button_new_with_label("MOTION SNAPSHOT CONTROLLER");
	play_file_button = gtk_button_new_with_label("PLAY VIDEO FILE");
	show_sprite_button = gtk_button_new_with_label("SHOW IMAGE");
	snapshot_button = gtk_button_new_with_label("TAKE SNAPSHOT");
	camera_button = gtk_button_new_with_label("OPEN CAMERA");
	gtk_box_pack_start(GTK_BOX(toggle_hbox), mbutton, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(toggle_hbox), snapshot_shape_button, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(vbox), dd->record_button, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(vbox), snapshot_button, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(vbox), toggle_hbox, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(vbox), play_file_button, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(vbox), show_sprite_button, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(vbox), camera_button, FALSE, FALSE, 0);
	dd->scale_combo = gtk_combo_box_text_new();
	int i = 0;
	const char *name = midi_scale_id_to_text(i);
	while (strcmp("None", name) != 0) {
		gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(dd->scale_combo), NULL, name);
		name = midi_scale_id_to_text(++i);
	}
	gtk_combo_box_set_active(GTK_COMBO_BOX(dd->scale_combo), 0);
	dd->note_combo = gtk_combo_box_text_new();
	for (int i = 0; i < 128; i++) {
		char id[4], text[NCHAR];
		sprintf(id, "%d", i);
		midi_note_to_octave_name(i, text);
		gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(dd->note_combo), id, text);
	}
	gtk_combo_box_set_active(GTK_COMBO_BOX(dd->note_combo), 60);
	dd->channel_combo = gtk_combo_box_text_new();
	for (int i = 0; i < 16; ++i) {
		char id[3];
		char text[NCHAR];
		snprintf(text, 3, "%d", i);
		gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(dd->channel_combo), id, text);

	}
	gtk_combo_box_set_active(GTK_COMBO_BOX(dd->channel_combo), 0);
	channel_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
	channel_label = gtk_label_new("CHANNEL");
	gtk_box_pack_start(GTK_BOX(channel_hbox), dd->channel_combo, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(channel_hbox), channel_label, FALSE, FALSE, 0);
	make_scale_button = gtk_button_new_with_label("MAKE SCALE");
	dd->rand_color_button = gtk_check_button_new_with_label("RANDOM COLOR");
	GdkRGBA gc;
	gc.red = 0;
	gc.green = 0.2;
	gc.blue = 0.3;
	gc.alpha = 0.5;
	dd->scale_color_button = gtk_color_button_new_with_rgba(&gc);
	gtk_color_chooser_set_use_alpha(GTK_COLOR_CHOOSER(dd->scale_color_button),
									TRUE);
	gtk_box_pack_start(GTK_BOX(vbox), note_hbox, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(note_hbox), dd->scale_combo, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(note_hbox), dd->note_combo, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(note_hbox), dd->scale_color_button, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(note_hbox), dd->rand_color_button, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(note_hbox), channel_hbox, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(note_hbox), dd->channel_combo, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(note_hbox), make_scale_button, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(vbox), dd->delete_button, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(vbox), qbutton, FALSE, FALSE, 0);
	gtk_container_add (GTK_CONTAINER(window), aspect);
	gtk_container_add (GTK_CONTAINER (dd->ctl_window), vbox);

	g_signal_connect(dd->record_button, "clicked", G_CALLBACK(record_cb), dd);
	g_signal_connect(dd->delete_button, "clicked", G_CALLBACK(delete_cb), dd);
	g_signal_connect(qbutton, "clicked", G_CALLBACK(quit_cb), dd);
	g_signal_connect(snapshot_button, "clicked", G_CALLBACK(snapshot_cb), dd);
	g_signal_connect(snapshot_shape_button, "clicked", G_CALLBACK(snapshot_shape_cb), dd);
	g_signal_connect(camera_button, "clicked", G_CALLBACK(camera_cb), dd);
	g_signal_connect(make_scale_button, "clicked", G_CALLBACK(make_scale_cb), dd);
	g_signal_connect(mbutton, "toggled", G_CALLBACK(motion_cb), dd);
	g_signal_connect(play_file_button, "clicked", G_CALLBACK(play_file_cb), dd);
	g_signal_connect(show_sprite_button, "clicked", G_CALLBACK(show_sprite_cb), dd);
	g_signal_connect(dd->rand_color_button, "toggled", G_CALLBACK(rand_color_cb), dd);
	g_signal_connect (drawing_area, "draw",
					  G_CALLBACK (draw_cb), dd);
	g_signal_connect (drawing_area,"configure-event",
					  G_CALLBACK (configure_event_cb), dd);
	g_signal_connect (window,"window-state-event",
					  G_CALLBACK (window_state_event_cb), dd);
	g_signal_connect (drawing_area, "motion-notify-event",
					  G_CALLBACK (motion_notify_event_cb), dd);
	g_signal_connect (drawing_area, "scroll-event",
					  G_CALLBACK (scroll_cb), dd);
	g_signal_connect (drawing_area, "button-press-event",
					  G_CALLBACK (button_press_event_cb), dd);
	g_signal_connect (drawing_area, "button-press-event",
					  G_CALLBACK (double_press_event_cb), dd);
	g_signal_connect (drawing_area, "button-release-event",
					  G_CALLBACK (button_release_event_cb), dd);
	g_signal_connect (window, "key-press-event",
					  G_CALLBACK (on_key_press), dd);
	g_signal_connect (window, "key-release-event",
					  G_CALLBACK (on_key_release), dd);
	gtk_widget_set_events(window, gtk_widget_get_events(window)
						  | GDK_WINDOW_STATE);
	gtk_widget_set_events (drawing_area, gtk_widget_get_events (drawing_area)
						   | GDK_BUTTON_PRESS_MASK | GDK_2BUTTON_PRESS
						   | GDK_POINTER_MOTION_MASK | GDK_BUTTON_RELEASE_MASK
						   | GDK_KEY_PRESS_MASK | GDK_KEY_RELEASE_MASK | GDK_SMOOTH_SCROLL_MASK);
	gtk_widget_show_all (window);
	gtk_widget_show_all (dd->ctl_window);
}

static void mainloop(DingleDots *dd) {
	dd->app = G_APPLICATION(gtk_application_new("org.dsheeler.v4l2_wayland",
												G_APPLICATION_NON_UNIQUE));
	g_signal_connect(dd->app, "activate", G_CALLBACK (activate), dd);
	g_application_run(G_APPLICATION(dd->app), 0, NULL);
}

static void signal_handler(int) {
	fprintf(stderr, "signal received, exiting ...\n");
	exit(0);
}

void setup_signal_handler() {
	signal(SIGQUIT, signal_handler);
	signal(SIGTERM, signal_handler);
	signal(SIGHUP, signal_handler);
	signal(SIGINT, signal_handler);
}

static void usage(DingleDots *, FILE *fp, int, char **argv)
{
	fprintf(fp,
			"Usage: %s [options]\n\n"
			"Options:\n"
			"-h | --help          Print this message\n"
			"-w	| --width         display width in pixels"
			"-g | --height        display height in pixels"
			"-b | --bitrate       bit rate of video file output\n"
			"",
			argv[0]);
}

static const char short_options[] = "d:ho:b:w:g:x:y:";

static const struct option
		long_options[] = {
{ "help",   no_argument,       NULL, 'h' },
{ "bitrate", required_argument, NULL, 'b' },
{ "width", required_argument, NULL, 'w' },
{ "height", required_argument, NULL, 'g' },
{ 0, 0, 0, 0 }
};

int main(int argc, char **argv) {
	DingleDots dingle_dots;
	int width = 1280;
	int height = 720;
	int video_bitrate = 1000000;
	srand(time(NULL));
	for (;;) {
		int idx;
		int c;
		c = getopt_long(argc, argv,
						short_options, long_options, &idx);
		if (-1 == c)
			break;
		switch (c) {
			case 0: /* getopt_long() flag */
				break;
			case 'b':
				video_bitrate = atoi(optarg);
				break;
			case 'w':
				width = atoi(optarg);
				break;
			case 'g':
				height = atoi(optarg);
				break;
			case 'h':
				usage(&dingle_dots, stdout, argc, argv);
				exit(EXIT_SUCCESS);
			default:
				usage(&dingle_dots, stderr, argc, argv);
				exit(EXIT_FAILURE);
		}
	}
	dingle_dots.init(width, height, video_bitrate);
	setup_jack(&dingle_dots);
	setup_signal_handler();
	g_timeout_add(40, queue_draw_timeout_cb, &dingle_dots);
	mainloop(&dingle_dots);
	dingle_dots.deactivate_sound_shapes();
	dingle_dots.free();
	teardown_jack(&dingle_dots);
	fprintf(stderr, "\n");
	return 0;
}
