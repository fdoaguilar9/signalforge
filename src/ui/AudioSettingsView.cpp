#include "AudioSettingsView.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "imgui.h"

#include "Fonts.h"
#include "Widgets.h"

namespace sf
{

namespace
{

constexpr double kSampleRates[] = {44100.0, 48000.0, 88200.0, 96000.0};
constexpr uint32_t kBufferSizes[] = {64, 128, 256, 512, 1024, 2048};

// The design's 520px. Every select inside stacks full width rather than
// sharing a row: the design's Select has a 260px minimum, and two of them side
// by side overflow this -- the same arithmetic that decided the splitter
// card's layout.
constexpr float kModalWidth = 520.0f;

// The popup's ImGui id, in one place: opening a popup and beginning one have
// to agree on it exactly, and two spellings of the same title is a modal that
// never appears.
constexpr const char* kTitle = "Audio settings";

enum class PortDirectionWanted
{
	Capture,  // hardware sources -- what feeds the board
	Playback  // hardware sinks -- what the board feeds
};

// Records the widget just drawn, while it is still "the last item" as far as
// Dear ImGui is concerned. Mirrors BoardView's helper of the same shape.
void RecordWidget(AudioSettingsViewState& state, const std::string& id)
{
	if (!state.record_widgets)
	{
		return;
	}

	const ImVec2 low = ImGui::GetItemRectMin();
	const ImVec2 high = ImGui::GetItemRectMax();

	state.widgets.push_back({id, (low.x + high.x) * 0.5f, (low.y + high.y) * 0.5f, ImGui::GetItemID(), true});
}

// One picker: a dropdown of what JACK currently reports in that direction.
// No free-text field -- the dropdown can express everything worth choosing,
// and a second way to say the same thing is just a second thing to get out
// of sync.
//
// Rows come at **two granularities**, because the two directions genuinely
// want different ones and a single choice would break one of them:
//   - a whole device ("all N ports"), which is what an output wants, since
//     the board feeds a stereo pair and splitting it would put the sound in
//     one ear;
//   - an individual port, which is what a mono input wants, since which jack
//     the guitar is in is exactly the question.
// Both are offered for both directions rather than guessing, so a mono
// monitor or a stereo-summed input is still expressible. A device with only
// one port in this direction gets one row, not two identical ones.
//
// The case a plain list can't hold is a saved value whose hardware isn't
// present right now (the interface is unplugged today). That is preserved
// rather than silently dropped: shown as the selection, marked unavailable,
// so unplugging a device for an evening doesn't quietly rewrite the setting.
void DrawPortSelector(AudioSettingsViewState& state, const char* probe_id, const char* label, std::string& filter,
	const std::vector<AudioSettingsJackPort>* ports, PortDirectionWanted wanted)
{
	ImGui::PushID(label);

	const std::string prefix = probe_id;
	const bool want_capture = wanted == PortDirectionWanted::Capture;

	// Already sorted by client then port, so a single pass walks each
	// device's ports contiguously.
	std::vector<const AudioSettingsJackPort*> relevant;

	if (ports)
	{
		for (const AudioSettingsJackPort& port : *ports)
		{
			if (port.is_capture == want_capture)
			{
				relevant.push_back(&port);
			}
		}
	}

	const auto ports_in_device = [&relevant](const std::string& client)
	{
		unsigned count = 0;

		for (const AudioSettingsJackPort* port : relevant)
		{
			if (port->client == client)
			{
				++count;
			}
		}

		return count;
	};

	// What the closed combo reads, and whether the saved value still refers
	// to something that exists.
	bool present = filter.empty();
	std::string preview = "(any available port)";
	char scratch[320];

	if (!filter.empty())
	{
		preview = filter;

		for (const AudioSettingsJackPort* port : relevant)
		{
			if (port->name == filter)
			{
				std::snprintf(scratch, sizeof(scratch), "%s   (%s)", port->port.c_str(), port->client.c_str());
				preview = scratch;
				present = true;
				break;
			}
		}

		if (!present)
		{
			if (const unsigned count = ports_in_device(filter); count > 0)
			{
				std::snprintf(scratch, sizeof(scratch), "%s   (all %u port%s)", filter.c_str(), count,
					count == 1 ? "" : "s");
				preview = scratch;
				present = true;
			}
		}
	}

	// Full width and on its own row: see kModalWidth.
	ImGui::SetNextItemWidth(-1.0f);

	const bool open = ImGui::BeginCombo(label, preview.c_str());

	// Recorded whether or not it is open -- this is the widget a test has to
	// click *to* open it.
	RecordWidget(state, prefix + ":combo");

	if (open)
	{
		if (ImGui::Selectable("(any available port)", filter.empty()))
		{
			filter.clear();
		}

		RecordWidget(state, prefix + ":any");

		for (size_t i = 0; i < relevant.size();)
		{
			const std::string& client = relevant[i]->client;
			const unsigned count = ports_in_device(client);

			// Only worth a device row when it would mean something different
			// from the single port row below it.
			if (count > 1)
			{
				std::snprintf(scratch, sizeof(scratch), "%s   (all %u ports)", client.c_str(), count);

				if (ImGui::Selectable(scratch, filter == client))
				{
					filter = client;
				}

				RecordWidget(state, prefix + ":device:" + client);
			}

			for (; i < relevant.size() && relevant[i]->client == client; ++i)
			{
				std::snprintf(scratch, sizeof(scratch), count > 1 ? "      %s" : "%s   (%s)",
					relevant[i]->port.c_str(), client.c_str());

				if (ImGui::Selectable(scratch, filter == relevant[i]->name))
				{
					filter = relevant[i]->name;
				}

				RecordWidget(state, prefix + ":port:" + relevant[i]->name);
			}
		}

		if (!present)
		{
			std::snprintf(scratch, sizeof(scratch), "%s   (not connected right now)", filter.c_str());

			// Selectable rather than disabled text: it *is* the current
			// selection, and it has to stay selectable so that opening the
			// dropdown to look and closing it again doesn't clear it.
			ImGui::Selectable(scratch, true);

			RecordWidget(state, prefix + ":unavailable");
		}

		if (relevant.empty())
		{
			ImGui::BeginDisabled();
			ImGui::TextUnformatted("(nothing detected -- no JACK server to ask)");
			ImGui::EndDisabled();
		}

		ImGui::EndCombo();
	}

	if (!present)
	{
		ImGui::SameLine();
		ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.4f, 1.0f), "(not connected)");
	}

	ImGui::PopID();
}

}  // namespace

void DrawAudioSettings(AudioSettingsViewState& state)
{
	// A modal in the shared frame like every other one (SF-80), rather than
	// the floating window with its own shape this used to be. Opened here
	// rather than by the caller because the caller has nothing else to say
	// about it: `DrawAudioSettings` being called *is* the request.
	if (!ImGui::IsPopupOpen(kTitle))
	{
		ImGui::OpenPopup(kTitle);
	}

	if (!widgets::BeginModal(kTitle, kModalWidth))
	{
		return;
	}

	if (state.first_run)
	{
		ImGui::TextUnformatted("First run: choose an audio device before the board starts.");
		ImGui::Separator();
	}

	if (state.server_running)
	{
		ImGui::TextWrapped(
			"A JACK server is already running. SignalForge will connect to it as-is -- its sample rate is that "
			"server's to set. The devices and the buffer size are still yours to change below.");
	}
	else
	{
		ImGui::TextWrapped("No JACK server is running. SignalForge will start its own.");
	}

	ImGui::Spacing();
	ImGui::SeparatorText("Devices");

	// Input and output are chosen independently. The guitar comes in one
	// interface and the sound very often goes out another (headphones on the
	// motherboard codec while the guitar is in a Scarlett is the ordinary
	// case, not an exotic one), so a single shared device picker would be
	// wrong on most desks.
	DrawPortSelector(state, "audio-input", "Input (guitar in)", state.input_filter, state.jack_ports,
		PortDirectionWanted::Capture);
	DrawPortSelector(state, "audio-output", "Output (sound out)", state.output_filter, state.jack_ports,
		PortDirectionWanted::Playback);

	// How wide the board's input is (SF-76). It sits with the input selector
	// because it is a fact about the input, and it is the setting the
	// splitter's stereo mode depends on -- with one channel there are no sides
	// to send to two amps.
	{
		static const char* const kWidths[] = {"Mono (1 in)", "Stereo (2 in)"};

		const int selected = state.board_inputs >= 2 ? 1 : 0;

		ImGui::SetNextItemWidth(-1.0f);

		if (ImGui::BeginCombo("Board input", kWidths[selected]))
		{
			for (int i = 0; i < 2; ++i)
			{
				if (ImGui::Selectable(kWidths[i], i == selected))
				{
					state.board_inputs = static_cast<unsigned>(i + 1);
				}

				RecordWidget(state, i == 0 ? "audio-inputs:mono" : "audio-inputs:stereo");
			}

			ImGui::EndCombo();
		}

		RecordWidget(state, "audio-inputs:combo");

		if (ImGui::IsItemHovered())
		{
			ImGui::SetTooltip("How many channels the board takes in.\nStereo is what the splitter's \"Stereo input\" "
							  "mode divides between the two channels.");
		}
	}

	if (!state.server_running)
	{
		ImGui::Spacing();
		ImGui::SeparatorText("Server (SignalForge will start this one)");

		// Only meaningful when *we* spawn the server: it's the ALSA card
		// jackd is told to open. When attaching to a server someone else is
		// running, that decision was already made and the port pickers above
		// are what actually matter.
		const char* current_device_name = "(ALSA default)";

		if (state.devices && state.selected_device >= 0 &&
			static_cast<size_t>(state.selected_device) < state.devices->size())
		{
			current_device_name = (*state.devices)[static_cast<size_t>(state.selected_device)].name.c_str();
		}

		ImGui::SetNextItemWidth(-1.0f);

		if (ImGui::BeginCombo("Card", current_device_name))
		{
			if (ImGui::Selectable("(ALSA default)", state.selected_device == -1))
			{
				state.selected_device = -1;
			}

			if (state.devices)
			{
				for (size_t i = 0; i < state.devices->size(); ++i)
				{
					const bool selected = state.selected_device == static_cast<int>(i);

					if (ImGui::Selectable((*state.devices)[i].name.c_str(), selected))
					{
						state.selected_device = static_cast<int>(i);

						// No live graph to pick ports from yet (there's no
						// server), so seed both filters from the card name as
						// a starting guess. Both remain editable, and once a
						// server is up the dropdowns above supersede this.
						if (state.input_filter.empty())
						{
							state.input_filter = (*state.devices)[i].filter;
						}

						if (state.output_filter.empty())
						{
							state.output_filter = (*state.devices)[i].filter;
						}
					}
				}
			}

			ImGui::EndCombo();
		}

		char rate_label[32];
		std::snprintf(rate_label, sizeof(rate_label), "%.0f Hz", state.sample_rate);

		ImGui::SetNextItemWidth(-1.0f);

		if (ImGui::BeginCombo("Sample rate", rate_label))
		{
			for (double rate : kSampleRates)
			{
				char label[32];
				std::snprintf(label, sizeof(label), "%.0f Hz", rate);

				if (ImGui::Selectable(label, state.sample_rate == rate))
				{
					state.sample_rate = rate;
				}
			}

			ImGui::EndCombo();
		}
	}

	// Always offered, unlike sample rate: JACK can change its period live on
	// a running server (jack_set_buffer_size), so this is adjustable whether
	// SignalForge started the server or merely attached to one. Sample rate
	// is not -- the driver is opened at a rate and keeps it -- which is why
	// only one of the two escapes the block above.
	ImGui::Spacing();
	ImGui::SeparatorText("Latency");

	char buffer_label[64];
	std::snprintf(buffer_label, sizeof(buffer_label), "%u samples", state.buffer_size);

	ImGui::SetNextItemWidth(-1.0f);

	if (ImGui::BeginCombo("Buffer size", buffer_label))
	{
		for (uint32_t size : kBufferSizes)
		{
			char label[64];

			// The millisecond figure is the point of the setting; the sample
			// count is just how it's spelled. One period at the current rate,
			// so it tracks whatever the server is actually running at.
			if (state.sample_rate > 0.0)
			{
				std::snprintf(label, sizeof(label), "%u samples  (%.1f ms)", size,
					1000.0 * static_cast<double>(size) / state.sample_rate);
			}
			else
			{
				std::snprintf(label, sizeof(label), "%u samples", size);
			}

			if (ImGui::Selectable(label, state.buffer_size == size))
			{
				state.buffer_size = size;
			}
		}

		ImGui::EndCombo();
	}

	if (ImGui::IsItemHovered())
	{
		ImGui::SetTooltip("Smaller is less delay but more work per second, so too small gives dropouts. This is a "
						  "server-wide setting: it changes for every JACK application, not just this one.");
	}

	// What the buffer size actually costs, in the unit the player cares about
	// (SF-80). Two periods: one in, one out -- which is what "round trip"
	// means and what the sidebar's Latency row reports from the server. Shown
	// in mono because it is a number to compare against another number.
	if (state.sample_rate > 0.0)
	{
		ScopedFont mono(Face::Mono, text::kXs);
		ImGui::TextDisabled(
			"%.1f ms round trip", 2000.0 * static_cast<double>(state.buffer_size) / state.sample_rate);
	}

	if (!state.status.empty())
	{
		ImGui::Spacing();
		ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.4f, 1.0f), "%s", state.status.c_str());
	}

	ImGui::Spacing();
	ImGui::Separator();

	// **The first run has no Cancel**: there is nothing yet to cancel back to,
	// and a button that closed the only way of configuring audio would leave a
	// board that cannot start.
	if (!state.first_run)
	{
		if (widgets::SecondaryButton("Cancel") || widgets::ModalDismissed())
		{
			state.cancel_requested = true;
			ImGui::CloseCurrentPopup();
		}

		RecordWidget(state, "audio:cancel");
		ImGui::SameLine();
	}

	// Apply is the primary, right-aligned -- one loud button per modal, at the
	// end of the row a reader finishes on.
	const char* apply = state.first_run ? "Start" : "Apply";
	ImGui::SetCursorPosX(ImGui::GetContentRegionMax().x - widgets::ButtonWidth(apply));

	if (widgets::PrimaryButton(apply))
	{
		state.apply_requested = true;
	}

	RecordWidget(state, "audio:apply");

	widgets::EndModal();
}

}  // namespace sf
