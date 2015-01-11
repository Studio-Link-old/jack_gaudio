default: jack_gaudio_out

jack_gaudio_out: alsa.c jack_out.c
	gcc jack2/memops.c alsa.c jack_out.c -o jack_gaudio_out -l jack -l asound -l pthread -lm -I jack2 -I jack2/jack

clean:
	rm -f jack_gaudio_out
