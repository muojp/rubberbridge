#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <signal.h>
#ifndef WIN32
#include <unistd.h>
#endif
#include <jack/jack.h>
#include <rubberband/RubberBandStretcher.h>
#include "imtui/imtui.h"

#include "imtui/imtui-impl-ncurses.h"

jack_port_t *input_port, *output_port;
jack_client_t *client;

using RB = RubberBand::RubberBandStretcher;
typedef struct
{
	RB *rb;
	int delay_frames = 0;
	int pending_samples = 0;
	float pitch_val = 0.0f;
	int delay_selection = 0;
	int formant_preference = -1;
} paTestData;

void cleanup_imtui() {
	ImTui_ImplText_Shutdown();
	ImTui_ImplNcurses_Shutdown();
}

static void signal_handler(int sig)
{
	jack_client_close(client);
	cleanup_imtui();
	fprintf(stderr, "signal received, exiting ...\n");
	exit(0);
}

/**
 * The process callback for this JACK application is called in a
 * special realtime thread once for each audio cycle.
 *
 * This client follows a simple rule: when the JACK transport is
 * running, copy the input port to the output.  When it stops, exit.
 */

int process(jack_nframes_t nframes, void *arg)
{
	jack_default_audio_sample_t *in, *out;
	paTestData *data = (paTestData *)arg;
	int i;

	in = (jack_default_audio_sample_t *)jack_port_get_buffer(input_port, nframes);
	out = (jack_default_audio_sample_t *)jack_port_get_buffer(output_port, nframes);

	data->rb->process((const float *const *)&in, nframes, false);
	data->pending_samples += nframes;
	auto avail = data->rb->available();
	if (avail == -1)
	{
		// this should never happen.
		cleanup_imtui();
		exit(2);
	}
	if ((nframes + data->delay_frames) <= avail)
	{
		// only retrieve results when output buffer will be fulfilled.
		data->rb->retrieve((float *const *)&out, nframes);
		data->pending_samples -= nframes;
	}

	return 0;
}

/**
 * JACK calls this shutdown_callback if the server ever shuts down or
 * decides to disconnect the client.
 */
void jack_shutdown(void *arg)
{
	cleanup_imtui();
	exit(1);
}

int main(int argc, char *argv[])
{
	const char **ports;
	const char *client_name;
	const char *server_name = NULL;
	jack_options_t options = JackNullOption;
	jack_status_t status;
	paTestData data;
	int i;

	const int opts = (RB::OptionProcessRealTime | RB::OptionPitchHighQuality);

	RB rb(44100, 1, opts);
	data.rb = &rb;
	// rb.setPitchScale(pow(2.0, 4 / 12.0));

	client_name = strrchr(argv[0], '/');
	if (client_name == 0)
	{
		client_name = argv[0];
	}
	else
	{
		client_name++;
	}

	/* open a client connection to the JACK server */

	client = jack_client_open(client_name, options, &status, server_name);
	if (client == NULL)
	{
		fprintf(stderr, "jack_client_open() failed, "
						"status = 0x%2.0x\n",
				status);
		if (status & JackServerFailed)
		{
			fprintf(stderr, "Unable to connect to JACK server\n");
		}
		exit(1);
	}
	if (status & JackServerStarted)
	{
		fprintf(stderr, "JACK server started\n");
	}
	if (status & JackNameNotUnique)
	{
		client_name = jack_get_client_name(client);
		fprintf(stderr, "unique name `%s' assigned\n", client_name);
	}

	/* tell the JACK server to call `process()' whenever
	   there is work to be done.
	*/

	jack_set_process_callback(client, process, &data);

	/* tell the JACK server to call `jack_shutdown()' if
	   it ever shuts down, either entirely, or if it
	   just decides to stop calling us.
	*/

	jack_on_shutdown(client, jack_shutdown, 0);

	/* create two ports */

	input_port = jack_port_register(client, "mic",
									JACK_DEFAULT_AUDIO_TYPE,
									JackPortIsInput, 0);

	output_port = jack_port_register(client, "output",
									  JACK_DEFAULT_AUDIO_TYPE,
									  JackPortIsOutput, 0);

	if ((input_port == NULL) || (output_port == NULL))
	{
		fprintf(stderr, "no more JACK ports available\n");
		exit(1);
	}

	/* Tell the JACK server that we are ready to roll.  Our
	 * process() callback will start running now. */

	if (jack_activate(client))
	{
		fprintf(stderr, "cannot activate client");
		exit(1);
	}

	/* Connect the ports.  You can't do this before the client is
	 * activated, because we can't make connections to clients
	 * that aren't running.  Note the confusing (but necessary)
	 * orientation of the driver backend ports: playback ports are
	 * "input" to the backend, and capture ports are "output" from
	 * it.
	 */

	ports = jack_get_ports(client, NULL, NULL,
						   JackPortIsPhysical | JackPortIsInput);
	if (ports == NULL)
	{
		fprintf(stderr, "no physical playback ports\n");
		exit(1);
	}

	if (jack_connect(client, jack_port_name(output_port), ports[0]))
	{
		fprintf(stderr, "cannot connect output ports\n");
	}

	jack_free(ports);

	ports = jack_get_ports(client, "capture_?", NULL, 0);
	if (ports == NULL)
	{
		fprintf(stderr, "no physical capture ports\n");
		exit(1);
	}

	if (jack_connect(client, ports[0], jack_port_name(input_port)))
	{
		fprintf(stderr, "cannot connect input ports\n");
		exit(1);
	}

	jack_free(ports);

	/* install a signal handler to properly quits jack client */
#ifdef WIN32
	signal(SIGINT, signal_handler);
	signal(SIGABRT, signal_handler);
	signal(SIGTERM, signal_handler);
#else
	signal(SIGQUIT, signal_handler);
	signal(SIGTERM, signal_handler);
	signal(SIGHUP, signal_handler);
	signal(SIGINT, signal_handler);
#endif

	IMGUI_CHECKVERSION();
	ImGui::CreateContext();

	auto screen = ImTui_ImplNcurses_Init(true);
	ImTui_ImplText_Init();

	float pitch_val = 1.25f;
	int delay_val = data.delay_selection;
	int formant_val = 0;

	/* keep running until the Ctrl+C */
	while (1)
	{
#ifdef WIN32
		Sleep(1000);
#else
		ImTui_ImplNcurses_NewFrame();
		ImTui_ImplText_NewFrame();

		ImGui::NewFrame();

		ImGui::SetNextWindowPos(ImVec2(4, 2), ImGuiCond_Once);
		ImGui::SetNextWindowSize(ImVec2(64.0, 10.0), ImGuiCond_Once);

		ImGui::Begin("dynamic pitch controller for Linux (RubberBand + Jack)");

		ImGui::Text("Pitch:");
		ImGui::SameLine();
		ImGui::SliderFloat("##float", &pitch_val, 0.75f, 2.0f);
		if (data.pitch_val != pitch_val)
		{
			// update pitch
			data.pitch_val = pitch_val;
			rb.setPitchScale(pow(2.0, data.pitch_val - 1.0f));
		}

		ImGui::Text("Formant:");
		ImGui::SameLine();

		ImGui::RadioButton("preserve", &formant_val, 0);
		ImGui::SameLine();
		ImGui::RadioButton("shift", &formant_val, 1);
		if (data.formant_preference != formant_val)
		{
			// formant preference changed. this can be applied on the fly.
			data.formant_preference = formant_val;
			switch (data.formant_preference)
			{
			case 0:
				rb.setFormantOption(RB::OptionFormantPreserved);
				break;
			case 1:
				rb.setFormantOption(RB::OptionFormantShifted);
				break;
			}
		}

		ImGui::Text("Delay(ms):");
		ImGui::SameLine();

		ImGui::RadioButton("0", &delay_val, 0);
		ImGui::SameLine();
		ImGui::RadioButton("500", &delay_val, 1);
		ImGui::SameLine();
		ImGui::RadioButton("1,000", &delay_val, 2);
		ImGui::SameLine();
		ImGui::RadioButton("1,500", &delay_val, 3);
		ImGui::SameLine();
		ImGui::RadioButton("2,000", &delay_val, 4);
		if (data.delay_selection != delay_val)
		{
			// delay preference changed. reset internals
			data.delay_selection = delay_val;
			data.delay_frames = data.delay_selection * (44100 / 2);
			data.pending_samples = 0;
			rb.reset();
		}

		ImGui::Text(" ");
		ImGui::Text("Actual Delay: %.1f ms", data.pending_samples * 1000.0 / 44100);

		ImGui::End();

		ImGui::Render();

		ImTui_ImplText_RenderDrawData(ImGui::GetDrawData(), screen);
		ImTui_ImplNcurses_DrawScreen();
		usleep(50000);
#endif
	}

	cleanup_imtui();

	jack_client_close(client);
	exit(0);
}
