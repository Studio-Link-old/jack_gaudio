default: jack_gaudio_out

jack_gaudio_out: alsa.c jack_out.c
	gcc alsa.c jack_out.c -o jack_gaudio_out -l jack -l asound

clean:
	rm -f jack_gaudio_out
