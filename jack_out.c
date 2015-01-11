#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <pthread.h>
#include <signal.h>
#include <math.h>

#include <jack/jack.h>
#include <jack/ringbuffer.h>
#include <memops.h>

#include <alsa/asoundlib.h>
#include "alsa.h"

typedef struct _thread_info {
	pthread_t thread_id;
	jack_nframes_t duration;
	jack_nframes_t rb_size;
	jack_client_t *client;
	unsigned int channels;
	int bitdepth;
	char *path;
	volatile int can_capture;
	volatile int can_process;
	volatile int status;
} jack_thread_info_t;

struct audio au;

jack_nframes_t nframes;
const size_t sample_size = sizeof(jack_default_audio_sample_t);

/* Synchronization between process thread and disk thread. */
#define DEFAULT_RB_SIZE 96000		/* ringbuffer size in frames */
jack_ringbuffer_t *rb;
pthread_mutex_t alsa_thread_lock = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t  data_ready = PTHREAD_COND_INITIALIZER;
long overruns = 0;
jack_client_t *client;
jack_port_t *output_port0;
jack_port_t *output_port1;
jack_default_audio_sample_t **in;
unsigned int nports = 2;
jack_port_t **ports;

static void signal_handler (int sig)
{
	jack_client_close(client);
	fprintf(stderr, "signal received, exiting ...\n");
	exit(0);
}

static void jack_shutdown (void *arg)
{
	fprintf(stderr, "JACK shut down, exiting ...\n");
	exit(1);
}

static void * alsa_thread (void *arg)
{
	nframes = 512;
	jack_thread_info_t *info = (jack_thread_info_t *) arg;
	jack_nframes_t samples_per_frame = info->channels * nframes;
	size_t bytes_per_frame = samples_per_frame * sample_size;
	void *framebuf = malloc (bytes_per_frame);
	void *resamplebuf = malloc (samples_per_frame);

	pthread_setcanceltype (PTHREAD_CANCEL_ASYNCHRONOUS, NULL);
	pthread_mutex_lock (&alsa_thread_lock);

	info->status = 0;
	while (1) {
		while (info->can_capture &&
				(jack_ringbuffer_read_space (rb) >= bytes_per_frame)) {

			jack_ringbuffer_read (rb, framebuf, bytes_per_frame);
			sample_move_d16_sS((char*)resamplebuf, framebuf, bytes_per_frame, 2, NULL);
			audio_alsa_play_write(resamplebuf, nframes);
		}
	//		printf("buffer2: %d \n",jack_ringbuffer_read_space (rb));

		/* wait until process() signals more data */
		pthread_cond_wait (&data_ready, &alsa_thread_lock);
	}
done:
	pthread_mutex_unlock (&alsa_thread_lock);
	free (framebuf);
	return 0;
}

static void setup_alsa_thread (jack_thread_info_t *info)
{
	au.device = "hw:1,0";
	au.sample_rate = 48000;
	au.channels = 2;
	au.buffer = 500000;
	audio_alsa_play_open(&au);
	info->can_capture = 0;
	pthread_create (&info->thread_id, NULL, alsa_thread, info);
}

static int process (jack_nframes_t nframes, void *arg)
{
	int chn;
	size_t i;
	jack_thread_info_t *info = (jack_thread_info_t *) arg;

	/* Do nothing until we're ready to begin. */
	if ((!info->can_process) || (!info->can_capture))
		return 0;

	for (chn = 0; chn < nports; chn++)
		in[chn] = jack_port_get_buffer (ports[chn], nframes);

	for (i = 0; i < nframes; i++) {
		for (chn = 0; chn < nports; chn++) {

			if (jack_ringbuffer_write (rb, (char *) (in[chn]+i),
						sample_size)
					< sample_size)
				overruns++;
		}
	}


	/* Tell the disk thread there is work to do.  If it is already
	 * 	 * running, the lock will not be available.  We can't wait
	 * 	 	 * here in the process() thread, but we don't need to signal
	 * 	 	 	 * in that case, because the disk thread will read all the
	 * 	 	 	 	 * data queued before waiting again. */
	if (pthread_mutex_trylock (&alsa_thread_lock) == 0) {
		pthread_cond_signal (&data_ready);
		pthread_mutex_unlock (&alsa_thread_lock);
	}
	return 0;
}

static void run_alsa_thread (jack_thread_info_t *info)
{
	info->can_capture = 1;
	pthread_join (info->thread_id, NULL);
	audio_alsa_play_close();
	if (overruns > 0) {
		fprintf (stderr,
				"jack_gaudio_out failed with %ld overruns.\n", overruns);
		fprintf (stderr, " try a bigger buffer than -B %"
				PRIu32 ".\n", info->rb_size);
		info->status = EPIPE;
	}
}

static void setup_ports (jack_thread_info_t *info)
{
	unsigned int i;
	size_t in_size;

	/* Allocate data structures that depend on the number of ports. */
	ports = (jack_port_t **) malloc (sizeof (jack_port_t *) * nports);
	in_size =  nports * sizeof (jack_default_audio_sample_t *);
	in = (jack_default_audio_sample_t **) malloc (in_size);
	rb = jack_ringbuffer_create (nports * sample_size * info->rb_size);


	/* When JACK is running realtime, jack_activate() will have
	 * called mlockall() to lock our pages into memory.  But, we
	 * still need to touch any newly allocated pages before
	 * process() starts using them.  Otherwise, a page fault could
	 * create a delay that would force JACK to shut us down. */
	memset(in, 0, in_size);
	memset(rb->buf, 0, rb->size);

	for (i = 0; i < nports; i++) {
		char name[64];

		sprintf (name, "input%d", i+1);

		if ((ports[i] = jack_port_register (info->client, name, JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput, 0)) == 0) {
			fprintf (stderr, "cannot register input port \"%s\"!\n", name);
			jack_client_close (info->client);
			exit (1);
		}
	}

	info->can_process = 1;		/* process() can start, now */
}



int main (int argc, char *argv[])
{

	jack_thread_info_t thread_info;
	memset (&thread_info, 0, sizeof (thread_info));
	thread_info.rb_size = DEFAULT_RB_SIZE;

	if ((client = jack_client_open ("gaudio", JackNullOption, NULL)) == 0) {
		fprintf (stderr, "JACK server not running?\n");
		exit (1);
	}

	thread_info.client = client;
	thread_info.channels = 2;
	thread_info.can_process = 0;
	
	setup_alsa_thread (&thread_info);

	jack_set_process_callback (client, process, &thread_info);
	jack_on_shutdown (client, jack_shutdown, &thread_info);

	if (jack_activate (client)) {
		fprintf (stderr, "cannot activate client");
	}

	setup_ports(&thread_info);

	signal(SIGTERM, signal_handler);
	signal(SIGINT, signal_handler);
	run_alsa_thread (&thread_info);

	jack_client_close (client);

	jack_ringbuffer_free (rb);

	exit (0);
}
