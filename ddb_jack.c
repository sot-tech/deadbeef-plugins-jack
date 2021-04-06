/*
 * JACK output plugin for DeaDBeeF
 * Copyright (C) 2010 Steven McDonald <steven.mcdonald@libremail.me>
 * CopyLeft  (c) 2014 -tclover <tokiclover@gmail.com>
 * CopyLeft  (c) 2021 sot-tech <service@sot-te.ch>
 * License: MIT (see COPYING file).
 */

#define _GNU_SOURCE
#ifdef HAVE_CONFIG_H
#  include <config.h>
#endif

#define DB_CLIENT_NAME "deadbeef"
#define DB_PLUG_NAME "ddb_jack"

#include <errno.h>
#include <unistd.h>
#include <jack/jack.h>
#include <deadbeef/deadbeef.h>
#include <signal.h>
#include <stdbool.h>

#ifdef DEBUG
#define trace(...) fprintf(stderr, __VA_ARGS__)
#else
#define trace(fmt, ...)
#endif
#define f_entry(...) trace("%s\n", __func__)

#if (DDB_API_LEVEL < 11)
#define ddb_playback_state_t int
#define DDB_PLAYBACK_STATE_STOPPED OUTPUT_STATE_STOPPED
#define DDB_PLAYBACK_STATE_PLAYING OUTPUT_STATE_PLAYING
#define DDB_PLAYBACK_STATE_PAUSED OUTPUT_STATE_PAUSED
#endif

typedef struct {
	DB_functions_t *db;
	jack_client_t *jack;
	bool connected, clean, fulfill;
	ddb_waveformat_t *fmt;
	ddb_playback_state_t state;
	jack_port_t *ports[];
} ddb_jack_connector;

static DB_output_t plugin;
static ddb_jack_connector con = {
	.db    = NULL,
	.jack        = NULL,
	.fmt         = &plugin.fmt,
	.connected   = false,
	.clean       = true,
	.fulfill     = true
};

DB_plugin_t *ddb_jack_load(DB_functions_t *api) {
	f_entry ();
	sigset_t set;
	sigemptyset(&set);
	sigaddset(&set, SIGPIPE);
	sigprocmask(SIG_BLOCK, &set, 0);
	con.db = api;

	return DB_PLUGIN (&plugin);
}

static ddb_playback_state_t ddb_playback_state(void) {
	return con.state;
}

static int jack_rate_cb(void *_) {
	f_entry ();
	int result = 0;
	if (con.connected) {
		con.fmt->samplerate = (int) jack_get_sample_rate(con.jack);
	} else {
		result = EIO;
	}

	return result;
}

static int ddb_playback_pause(void) {
	f_entry ();
	if (con.state != DDB_PLAYBACK_STATE_STOPPED)
		con.state = DDB_PLAYBACK_STATE_PAUSED;
	return 0;
}

static int ddb_playback_stop(void) {
	f_entry ();
	con.state = DDB_PLAYBACK_STATE_STOPPED;
	con.db->streamer_reset(true);

	return 0;
}

static int ddb_jack_close(bool disconnect) {
	f_entry ();
	int result = 0;
	if (con.connected) {
		con.connected = false;

		// stop playback if we didn't start jack
		// this prevents problems with not disconnecting gracefully
		ddb_playback_stop();
		usleep(100);

		if (con.jack) {
			if (disconnect) {
				if ((result = jack_client_close(con.jack))) {
					con.db->log("Could not disconnect from JACK server\n");
				}
			}
		}
	}
	return result;
}

static int ddb_jack_free() {
	return ddb_jack_close(true);
}

static int jack_shutdown_cb(void *_) {
	f_entry ();
	con.db->log("JACK server shut down unexpectedly, stopping playback\n");
	return ddb_jack_close(false);
}

static int jack_proc_cb(jack_nframes_t inframes, void *_) {
	int result = 0;
	if (con.connected) {
		// FIXME: This function copies from the streamer to a local buffer,
		//        and then to JACK's buffer. This is wasteful.

		//            Update 2011-01-01:
		//        The streamer can now use floating point samples, but there
		//        is still no easy solution to this because the streamer
		//        outputs both channels multiplexed, whereas JACK expects
		//        each channel to be written to a separate buffer.
		switch (con.state) {
			case DDB_PLAYBACK_STATE_PLAYING: {
				int bufsize = (int) inframes * con.fmt->channels * (con.fmt->bps / 8);
				char buf[bufsize];
				int inbytes = con.db->streamer_read(buf, bufsize);

				// this avoids a crash if we are playing and change to a plugin
				// with no valid output and then switch back
				if (inbytes == EOF) {
					con.state = DDB_PLAYBACK_STATE_STOPPED;
				} else {
					// this is intended to make playback less jittery in case of
					// inadequate read from streamer
					while (con.fulfill && inbytes < bufsize) {
						//usleep (100);
						trace("Streamer data not aligned: %d, but need %d. Mitigating\n", inbytes, bufsize);
						int buftail = con.db->streamer_read(buf + inbytes, bufsize - inbytes);
						if (buftail != EOF) inbytes += buftail;
					}

					jack_nframes_t outframes = inbytes * 8 / (con.fmt->channels * con.fmt->bps);
					float *jack_port_buffer[con.fmt->channels];
					for (int ch = 0; ch < con.fmt->channels; ++ch) {
						jack_port_buffer[ch] = jack_port_get_buffer(con.ports[ch], outframes);//inframes);
					}

					float vol = con.db->volume_get_amp();

					for (jack_nframes_t frame = 0; frame < outframes; ++frame) {
						for (int ch = 0; ch < con.fmt->channels; ++ch) {
							// JACK expects floating point samples, so we need to convert from integer
							*jack_port_buffer[ch]++ =
								((float *) buf)[(con.fmt->channels * frame) + ch] * vol; // / 32768;
						}
					}
				}
				con.clean = false;
				break;
			}

				// this is necessary to stop JACK going berserk when we pause/stop
			default: {
				if (!con.clean) {
					float *jack_port_buffer[con.fmt->channels];
					for (int ch = 0; ch < con.fmt->channels; ++ch) {
						jack_port_buffer[ch] = jack_port_get_buffer(con.ports[ch], inframes);
					}

					for (jack_nframes_t frame = 0; frame < inframes; ++frame) {
						for (int ch = 0; ch < con.fmt->channels; ++ch) {
							*jack_port_buffer[ch]++ = 0;
						}
					}
					con.clean = true;
				}
				break;
			}
		}
	} else {
		result = EIO;
	}
	return result;
}

static int ddb_jack_init(void) {
	f_entry ();
	con.clean = true;
	con.fulfill = con.db->conf_get_int("jack.fulfill", 1);
	// create new jack on JACK server
	jack_status_t status;
	con.jack = jack_client_open(DB_CLIENT_NAME, JackNullOption | JackNoStartServer, &status);
	if (status) {
		con.db->log("Could not connect to JACK server\n");
		ddb_jack_close(false);
		return ENXIO;
	}

	con.fmt->samplerate = (int) jack_get_sample_rate(con.jack);

	// set process callback
	if (jack_set_process_callback(con.jack, &jack_proc_cb, NULL)) {
		con.db->log("Could not set process callback\n");
		ddb_jack_close(true);
		return EFAULT;
	}

	// set sample rate callback
	if (jack_set_sample_rate_callback(con.jack, (JackSampleRateCallback) &jack_rate_cb, NULL)) {
		con.db->log("Could not set sample rate callback\n");
		ddb_jack_close(true);
		return EFAULT;
	}

	// set shutdown callback
	jack_on_shutdown(con.jack, (JackShutdownCallback) &jack_shutdown_cb, NULL);

	// register ports
	for (int i = 0; i < con.fmt->channels; i++) {
		char port_name[24];

		// i+1 used to adhere to JACK convention of counting ports from 1, not 0
		snprintf(port_name, 24, "ddb_playback_%d", i + 1);
		con.ports[i] = jack_port_register(con.jack, (const char *) &port_name,
		                                  JACK_DEFAULT_AUDIO_TYPE,
		                                  JackPortIsOutput | JackPortIsTerminal,
		                                  0);
		if (!con.ports[i]) {
			con.db->log("Could not register port number %d\n", i + 1);
			ddb_jack_close(true);
			return EIO;
		}
	}

	// tell JACK we are ready to roll
	if (jack_activate(con.jack)) {
		con.db->log("Could not activate JACK\n");
		ddb_jack_close(true);
		return EIO;
	}

	// connected ports to hardware output
	const char **playback_ports;
	int result = 0;
	if (!(playback_ports = jack_get_ports(con.jack, NULL, NULL,
	                                      JackPortIsPhysical | JackPortIsInput))) {
		con.db->log("Could not find any playback ports to connected to\n");
		result = ENXIO;
	} else {
		int ret;
		for (int i = 0; i < con.fmt->channels; i++) {
			ret = jack_connect(con.jack, jack_port_name(con.ports[i]), playback_ports[i]);
			if (ret != 0 && ret != EEXIST) {
				con.db->log("Could not create connection from %s to %s\n",
				            jack_port_name(con.ports[i]), playback_ports[i]);
				ddb_jack_close(true);
				result = EIO;
				break;
			}
		}
		jack_free(playback_ports);
	}
	if (!result) {
		con.connected = true;
	}
	return result;
}

static int ddb_jack_setformat(ddb_waveformat_t *fmt) {
	f_entry();
	int result = 0;
	if (!con.connected) {
		if (!(result = ddb_jack_init())) {
			if (con.fmt->samplerate != fmt->samplerate) {
				con.db->log_detailed(&plugin.plugin, DDB_LOG_LAYER_INFO,
				                     "DeaDBeeF's and JACK's sample rates differs, use resample DSP\n");
			}
		}
	}
	return result;
}

static int ddb_playback_play(void) {
	f_entry ();
	if (!con.connected) {
		if (ddb_jack_init()) {
			return EIO;
		}
	}
	con.state = DDB_PLAYBACK_STATE_PLAYING;

	return 0;
}

static const char settings_dlg[] =
	"property \"Fulfill JACK buffer\" checkbox jack.fulfill 1;\n";

// define plugin interface
static DB_output_t plugin = {
	DB_PLUGIN_SET_API_VERSION
	.plugin.version_major = 0,
	.plugin.version_minor = 4,
	.plugin.type = DB_PLUGIN_OUTPUT,
	.plugin.id = DB_PLUG_NAME,
	.plugin.name = "JACK output plugin",
	.plugin.descr = "plays sound via JACK API",
	.plugin.copyright = "CopyLeft (C) 2014 -tclover <tokiclover@gmail.com> (mod. sot-tech)",
	.plugin.website = "https://github.com/sot-tech/deadbeef-plugins-jack",
	.plugin.configdialog = settings_dlg,
	.plugin.stop = ddb_jack_free,
	.free = ddb_jack_free,
	.init = ddb_jack_init,
	.setformat = ddb_jack_setformat,
	.play = ddb_playback_play,
	.unpause = ddb_playback_play,
	.pause = ddb_playback_pause,
	.stop = ddb_playback_stop,
	.state = ddb_playback_state,
	.fmt = {
		.bps = 32,
		.is_float = true,
		.channels = 2,
		.channelmask = DDB_SPEAKER_FRONT_LEFT | DDB_SPEAKER_FRONT_RIGHT,
		.is_bigendian = false,
	},
	.has_volume = true,
};

