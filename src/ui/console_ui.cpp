#include "ui/console_ui.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "imgui.h"
#include "platform/platform.hpp"
#include "qrcodegen.hpp"

namespace xst::ui {

namespace {

// Colours: Syncthing's blue, and state colours as the Web GUI uses them.
const ImVec4 kAccent(0.0f, 0.53f, 0.82f, 1.0f);
const ImVec4 kGood(0.18f, 0.80f, 0.44f, 1.0f);
const ImVec4 kWarn(0.95f, 0.61f, 0.07f, 1.0f);
const ImVec4 kBad(0.91f, 0.30f, 0.24f, 1.0f);
const ImVec4 kDim(0.65f, 0.70f, 0.75f, 1.0f);

// The title-safe area: 10% in from each edge of 1280x720 (5% would be the action-safe one).
constexpr float kSafe = 0.075f;

// The colour for a folder state as state_text() names it, as the Web GUI colours them.
ImVec4 text_colour(const std::string &st)
{
	if (st == "Up to Date")
		return kGood;
	if (st == "Paused" || st == "Unshared")
		return kDim;
	if (st == "Error" || st == "Failed Items")
		return kBad;
	if (st == "Out of Sync")
		return kWarn;
	return kAccent; // Scanning (n files), Syncing n / m
}

std::string state_text(const FolderRow &f)
{
	if (f.paused)
		return "Paused";
	if (!f.error.empty() || f.state == "error")
		return "Error";
	if (f.state == "scanning")
		return "Scanning (" + std::to_string(f.scan_files) + " files)";
	if (f.state == "syncing")
		return f.pull_total > 0 ? "Syncing " + std::to_string(f.pull_done) + " / " + std::to_string(f.pull_total)
								: std::string("Syncing");
	if (f.shared_with.empty())
		return "Unshared";
	if (f.need_items > 0)
		return "Out of Sync";
	if (f.errors > 0)
		return "Failed Items";
	return "Up to Date";
}

bool big_button(const char *label, float width = 0)
{
	return ImGui::Button(label, ImVec2(width, 0));
}

void progress(double pct, float width)
{
	char text[32];
	std::snprintf(text, sizeof text, "%.0f%%", pct);
	ImGui::ProgressBar(static_cast<float>(pct / 100.0), ImVec2(width, 0), text);
}

// A text field: on the console the backend opens XAM's keyboard when it's activated (A), so typing works with a pad.
void text_field(const char *label, char *buf, size_t size, ImGuiInputTextFlags flags = 0)
{
	ImGui::TextUnformatted(label);
	ImGui::SetNextItemWidth(-1);
	ImGui::PushID(label);
	ImGui::InputText("##f", buf, size, flags);
	ImGui::PopID();
}

} // namespace

ConsoleUi::ConsoleUi(App &app) : app_(app) {}

void ConsoleUi::setup_style(float scale)
{
	ImGuiIO &io = ImGui::GetIO();
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad | ImGuiConfigFlags_NavEnableKeyboard;
	io.IniFilename = nullptr;
	io.LogFilename = nullptr;
	ImGui::StyleColorsDark();
	ImGuiStyle &s = ImGui::GetStyle();
#if IMGUI_VERSION_NUM >= 19200
	s.FontSizeBase = 22.0f * scale; // read across a room
#else
	io.FontGlobalScale = 1.4f * scale;
#endif
	s.WindowRounding = 6;
	s.FrameRounding = 4;
	s.GrabRounding = 4;
	s.ChildRounding = 6;
	s.FramePadding = ImVec2(12, 8);
	s.ItemSpacing = ImVec2(12, 10);
	s.WindowPadding = ImVec2(16, 14);
	s.ScrollbarSize = 18;
	auto *c = s.Colors;
	c[ImGuiCol_WindowBg] = ImVec4(0.06f, 0.09f, 0.12f, 1.0f);
	c[ImGuiCol_ChildBg] = ImVec4(0.10f, 0.14f, 0.18f, 1.0f);
	c[ImGuiCol_PopupBg] = ImVec4(0.09f, 0.12f, 0.16f, 0.98f);
	c[ImGuiCol_Button] = ImVec4(0.14f, 0.22f, 0.30f, 1.0f);
	c[ImGuiCol_ButtonHovered] = kAccent;
	c[ImGuiCol_ButtonActive] = ImVec4(0.0f, 0.42f, 0.66f, 1.0f);
	c[ImGuiCol_Header] = ImVec4(0.14f, 0.22f, 0.30f, 1.0f);
	c[ImGuiCol_HeaderHovered] = kAccent;
	c[ImGuiCol_HeaderActive] = ImVec4(0.0f, 0.42f, 0.66f, 1.0f);
	c[ImGuiCol_Tab] = ImVec4(0.10f, 0.14f, 0.18f, 1.0f);
	c[ImGuiCol_TabSelected] = kAccent;
	c[ImGuiCol_TabHovered] = ImVec4(0.0f, 0.42f, 0.66f, 1.0f);
	c[ImGuiCol_PlotHistogram] = kGood;
	c[ImGuiCol_NavCursor] = ImVec4(1.0f, 0.85f, 0.2f, 1.0f); // a bright focus ring, visible on a TV
	c[ImGuiCol_FrameBg] = ImVec4(0.12f, 0.17f, 0.22f, 1.0f);
}

std::vector<std::vector<bool>> ConsoleUi::qr(const std::string &text)
{
	try {
		const auto code = qrcodegen::QrCode::encodeText(text.c_str(), qrcodegen::QrCode::Ecc::MEDIUM);
		std::vector<std::vector<bool>> out(static_cast<size_t>(code.getSize()), std::vector<bool>(static_cast<size_t>(code.getSize())));
		for (int y = 0; y < code.getSize(); y++)
			for (int x = 0; x < code.getSize(); x++)
				out[static_cast<size_t>(y)][static_cast<size_t>(x)] = code.getModule(x, y);
		return out;
	} catch (...) {
		return {};
	}
}

void ConsoleUi::draw_qr(const std::string &text, float size)
{
	const auto m = qr(text);
	if (m.empty())
		return;
	const float quiet = 4; // modules of white border, as the spec asks
	const float n = static_cast<float>(m.size()) + 2 * quiet;
	const float cell = std::floor(size / n);
	const ImVec2 p = ImGui::GetCursorScreenPos();
	ImDrawList *dl = ImGui::GetWindowDrawList();
	dl->AddRectFilled(p, ImVec2(p.x + cell * n, p.y + cell * n), IM_COL32_WHITE);
	for (size_t y = 0; y < m.size(); y++)
		for (size_t x = 0; x < m.size(); x++)
			if (m[y][x]) {
				const ImVec2 a(p.x + (static_cast<float>(x) + quiet) * cell, p.y + (static_cast<float>(y) + quiet) * cell);
				dl->AddRectFilled(a, ImVec2(a.x + cell, a.y + cell), IM_COL32_BLACK);
			}
	ImGui::Dummy(ImVec2(cell * n, cell * n));
}

void ConsoleUi::toast(const std::string &text)
{
	toast_ = text;
	toast_until_ = std::chrono::steady_clock::now() + std::chrono::seconds(4);
}

bool ConsoleUi::frame(float width, float height)
{
	// Refresh the snapshot twice a second: it's a handful of database queries.
	const auto now = std::chrono::steady_clock::now();
	if (now - snap_at_ > std::chrono::milliseconds(500)) {
		snap_ = snapshot(app_);
		snap_at_ = now;
	}
	const float mx = width * kSafe, my = height * kSafe;
	ImGui::SetNextWindowPos(ImVec2(mx, my));
	ImGui::SetNextWindowSize(ImVec2(width - 2 * mx, height - 2 * my));
	ImGui::Begin("xsyncthing", nullptr,
		ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
			ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);
	header(width - 2 * mx);
	tabs();
	const float footer_h = ImGui::GetFrameHeightWithSpacing() + 4;
	ImGui::BeginChild("screen", ImVec2(0, -footer_h), ImGuiChildFlags_NavFlattened, 0);
	switch (screen_) {
	case Screen::dashboard:
		draw_dashboard();
		break;
	case Screen::device_id:
		draw_device_id();
		break;
	case Screen::devices:
		draw_devices();
		break;
	case Screen::folders:
		draw_folders();
		break;
	case Screen::pending:
		draw_pending();
		break;
	case Screen::settings:
		draw_settings();
		break;
	case Screen::log:
		draw_log();
		break;
	}
	ImGui::EndChild();
	footer(width - 2 * mx, height);
	modal_add_device();
	modal_share_folder();
	modal_confirm();
	open_dialog_.clear();
	for (const char *d : { "Add a device", "Folder", "Are you sure?" })
		if (ImGui::IsPopupOpen(d))
			open_dialog_ = d;
	ImGui::End();

	// LB / RB switch screens; Back held for a second quits.
	if (!ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId)) {
		const int n = 7;
		int i = static_cast<int>(screen_);
		if (ImGui::IsKeyPressed(ImGuiKey_GamepadL1, false))
			screen_ = static_cast<Screen>((i + n - 1) % n);
		if (ImGui::IsKeyPressed(ImGuiKey_GamepadR1, false))
			screen_ = static_cast<Screen>((i + 1) % n);
	}
	if (ImGui::IsKeyDown(ImGuiKey_GamepadBack)) {
		if (back_held_since_ == std::chrono::steady_clock::time_point{})
			back_held_since_ = now;
		else if (now - back_held_since_ > std::chrono::seconds(1))
			want_quit_ = true;
	} else {
		back_held_since_ = {};
	}
	return !want_quit_;
}

void ConsoleUi::header(float)
{
	ImGui::PushStyleColor(ImGuiCol_Text, kAccent);
	ImGui::TextUnformatted("xsyncthing");
	ImGui::PopStyleColor();
	ImGui::SameLine();
	ImGui::TextDisabled("%s  |  %s  |  %s", snap_.device_name.c_str(), snap_.address.empty() ? "no network" : snap_.address.c_str(),
		snap_.version.c_str());
	int connected = 0;
	for (const auto &d : snap_.devices)
		connected += d.connected;
	char conn[48];
	std::snprintf(conn, sizeof conn, "%d/%zu devices connected", connected, snap_.devices.size());
	ImGui::SameLine();
	ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize(conn).x);
	ImGui::TextColored(connected ? kGood : kDim, "%s", conn);
}

void ConsoleUi::tabs()
{
	struct Tab {
		Screen s;
		const char *name;
	};
	const size_t pending = snap_.pending_devices.size() + snap_.pending_folders.size();
	char pending_label[32];
	std::snprintf(pending_label, sizeof pending_label, pending ? "Requests (%zu)" : "Requests", pending);
	const Tab tabs[] = { { Screen::dashboard, "Overview" }, { Screen::device_id, "This Device" },
		{ Screen::devices, "Devices" }, { Screen::folders, "Folders" }, { Screen::pending, pending_label },
		{ Screen::settings, "Settings" }, { Screen::log, "Log" } };
	ImGui::TextDisabled("LB");
	ImGui::SameLine();
	for (const auto &t : tabs) {
		const bool active = screen_ == t.s;
		if (active)
			ImGui::PushStyleColor(ImGuiCol_Button, kAccent);
		if (ImGui::Button(t.name))
			screen_ = t.s;
		if (active)
			ImGui::PopStyleColor();
		ImGui::SameLine();
	}
	ImGui::TextDisabled("RB");
	ImGui::Separator();
}

void ConsoleUi::footer(float, float)
{
	ImGui::Separator();
	if (!toast_.empty() && std::chrono::steady_clock::now() < toast_until_) {
		ImGui::TextColored(kGood, "%s", toast_.c_str());
		return;
	}
	const char *hint = "A Select   B Back   LB/RB Screens   Hold BACK: quit";
	switch (screen_) {
	case Screen::devices:
		hint = "A Select   Y Add device   X Pause/Resume   LB/RB Screens";
		break;
	case Screen::folders:
		hint = "A Select   X Rescan   LB/RB Screens";
		break;
	case Screen::device_id:
		hint = "LB/RB Screens   Hold BACK: quit";
		break;
	default:
		break;
	}
	ImGui::TextDisabled("%s", hint);
	if (!snap_.gui_url.empty()) {
		const std::string url = "Web GUI: " + snap_.gui_url;
		const float hint_w = ImGui::CalcTextSize(hint).x;
		const float url_w = ImGui::CalcTextSize(url.c_str()).x;
		ImGui::SameLine();
		const float avail = ImGui::GetContentRegionAvail().x;
		if (url_w + 40 < avail) {
			ImGui::SetCursorPosX(ImGui::GetCursorPosX() + avail - url_w);
			ImGui::TextDisabled("%s", url.c_str());
		} else {
			ImGui::NewLine();
		}
		(void)hint_w;
	}
}

void ConsoleUi::draw_dashboard()
{
	const float w = ImGui::GetContentRegionAvail().x;
	ImGui::BeginChild("folders", ImVec2(w * 0.55f, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_NavFlattened, 0);
	ImGui::TextUnformatted("Folders");
	ImGui::Separator();
	if (snap_.folders.empty()) {
		ImGui::TextWrapped("No folders yet. Accept one a device offers (Requests), or add a device and share one.");
	}
	for (const auto &f : snap_.folders) {
		ImGui::PushID(f.id.c_str());
		const std::string st = state_text(f);
		if (ImGui::Selectable(("##row" + f.id).c_str(), false, 0, ImVec2(0, ImGui::GetTextLineHeight() * 2.4f)))
			screen_ = Screen::folders;
		ImGui::SameLine(10);
		ImGui::BeginGroup();
		ImGui::TextUnformatted(f.label.c_str());
		ImGui::SameLine(ImGui::GetContentRegionAvail().x * 0.62f);
		ImGui::TextColored(text_colour(st), "%s", st.c_str());
		if (f.state == "syncing" && f.pull_total > 0) {
			progress(100.0 * static_cast<double>(f.pull_done) / static_cast<double>(f.pull_total),
				ImGui::GetContentRegionAvail().x * 0.9f);
		} else if (f.need_items > 0 && !f.paused) {
			progress(f.completion, ImGui::GetContentRegionAvail().x * 0.9f);
		} else {
			ImGui::TextDisabled("%s  -  %lld files, %s", f.path.c_str(), static_cast<long long>(f.local_files),
				human_bytes(f.global_bytes).c_str());
		}
		ImGui::EndGroup();
		ImGui::PopID();
	}
	ImGui::EndChild();
	ImGui::SameLine();
	ImGui::BeginChild("devices", ImVec2(0, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_NavFlattened, 0);
	ImGui::TextUnformatted("Devices");
	ImGui::Separator();
	if (snap_.devices.empty())
		ImGui::TextWrapped("No devices yet. Press Y on Devices to add one, or open This Device to show your ID.");
	for (const auto &d : snap_.devices) {
		ImGui::PushID(d.id.c_str());
		if (ImGui::Selectable(("##d" + d.id).c_str(), false, 0, ImVec2(0, ImGui::GetTextLineHeight() * 2.4f)))
			screen_ = Screen::devices;
		ImGui::SameLine(10);
		ImGui::BeginGroup();
		ImGui::TextUnformatted(d.name.c_str());
		ImGui::SameLine(ImGui::GetContentRegionAvail().x * 0.55f);
		if (d.paused)
			ImGui::TextColored(kDim, "Paused");
		else if (!d.connected)
			ImGui::TextColored(kDim, "Disconnected");
		else if (d.completion < 100)
			ImGui::TextColored(kAccent, "Syncing %.0f%%", d.completion);
		else
			ImGui::TextColored(kGood, "Up to Date");
		ImGui::TextDisabled("%s", d.connected ? (d.address + "  " + d.type).c_str() : d.short_id.c_str());
		ImGui::EndGroup();
		ImGui::PopID();
	}
	const size_t pending = snap_.pending_devices.size() + snap_.pending_folders.size();
	if (pending) {
		ImGui::Spacing();
		ImGui::PushStyleColor(ImGuiCol_Button, kWarn);
		char b[64];
		std::snprintf(b, sizeof b, "%zu request%s waiting", pending, pending == 1 ? "" : "s");
		if (big_button(b, -1))
			screen_ = Screen::pending;
		ImGui::PopStyleColor();
	}
	ImGui::PushStyleColor(ImGuiCol_Text, kWarn);
	for (const auto &e : snap_.system_errors)
		ImGui::TextWrapped("%s", e.c_str());
	ImGui::PopStyleColor();
	ImGui::EndChild();
}

void ConsoleUi::draw_device_id()
{
	const float avail = ImGui::GetContentRegionAvail().y;
	const float qr_size = std::min(avail - 10, 400.0f);
	ImGui::BeginGroup();
	draw_qr(snap_.device_id, qr_size);
	ImGui::EndGroup();
	ImGui::SameLine(0, 30);
	ImGui::BeginGroup();
	ImGui::TextColored(kAccent, "This console's device ID");
	ImGui::Spacing();
	// The ID in its 8 groups, two lines, large: easy to read off the TV and type on a PC.
	const std::string &id = snap_.device_id;
	if (id.size() == 63) {
		ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.45f);
		ImGui::TextUnformatted(id.substr(0, 31).c_str());
		ImGui::TextUnformatted(id.substr(32).c_str());
		ImGui::PopFont();
	}
	ImGui::Spacing();
	ImGui::TextWrapped("To pair: in Syncthing on your PC or phone, choose Add Remote Device and scan this code or enter "
					   "the ID. This console will then show the request under Requests.");
	ImGui::Spacing();
	ImGui::TextDisabled("Name: %s", snap_.device_name.c_str());
	ImGui::TextDisabled("Address: %s, port %s", snap_.address.c_str(), snap_.listen_address.c_str());
	if (!snap_.relay.empty())
		ImGui::TextDisabled("Relay: %s", snap_.relay.substr(0, snap_.relay.find('?')).c_str());
	if (!snap_.gui_url.empty()) {
		ImGui::Spacing();
		ImGui::TextColored(kAccent, "Web GUI");
		ImGui::Text("%s", snap_.gui_url.c_str());
		if (snap_.gui_has_password)
			ImGui::TextDisabled("Log in as \"%s\" (password set in Settings)", snap_.gui_user.c_str());
		else
		{
			ImGui::PushStyleColor(ImGuiCol_Text, kWarn);
			ImGui::TextWrapped("No GUI password set: anyone on your network can open it. Set one in Settings.");
			ImGui::PopStyleColor();
		}
		ImGui::TextDisabled("API key: %s", snap_.api_key.c_str());
	}
	ImGui::EndGroup();
}

void ConsoleUi::draw_devices()
{
	if (big_button("Add a device (Y)") || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceUp, false)) {
		add_device_open_ = true;
		add_device_error_.clear();
		std::memset(add_device_id_, 0, sizeof add_device_id_);
		std::memset(add_device_name_, 0, sizeof add_device_name_);
	}
	ImGui::Spacing();
	if (ImGui::BeginTable("devices", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)) {
		const ImGuiStyle &st = ImGui::GetStyle();
		const float buttons = ImGui::CalcTextSize("Resume").x + ImGui::CalcTextSize("Share...").x +
			ImGui::CalcTextSize("Remove").x + 6 * st.FramePadding.x + 2 * st.ItemSpacing.x + 8;
		ImGui::TableSetupColumn("Device", ImGuiTableColumnFlags_WidthStretch, 1.3f);
		ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthStretch, 1.0f);
		ImGui::TableSetupColumn("Connection", ImGuiTableColumnFlags_WidthStretch, 1.2f);
		ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, buttons);
		ImGui::TableHeadersRow();
		for (const auto &d : snap_.devices) {
			ImGui::PushID(d.id.c_str());
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			ImGui::TextUnformatted(d.name.c_str());
			ImGui::TextDisabled("%s", d.short_id.c_str());
			ImGui::TableNextColumn();
			ImGui::TextColored(d.paused ? kDim : d.connected ? kGood : kDim, "%s",
				d.paused ? "Paused" : d.connected ? (d.completion < 100 ? "Syncing" : "Up to Date") : "Disconnected");
			ImGui::TableNextColumn();
			ImGui::TextDisabled("%s", d.connected ? (d.address + "\n" + d.client).c_str() : "-");
			ImGui::TableNextColumn();
			if (ImGui::Button(d.paused ? "Resume" : "Pause"))
				set_device_paused(app_, d.id, !d.paused);
			if (ImGui::IsItemFocused() && ImGui::IsKeyPressed(ImGuiKey_GamepadFaceLeft, false))
				set_device_paused(app_, d.id, !d.paused);
			ImGui::SameLine();
			if (ImGui::Button("Share...")) {
				share_open_ = true;
				share_from_pending_ = false;
				share_device_ = d.id;
				share_error_.clear();
				std::memset(share_id_, 0, sizeof share_id_);
				std::memset(share_label_, 0, sizeof share_label_);
				std::memset(share_path_, 0, sizeof share_path_);
			}
			ImGui::SameLine();
			if (ImGui::Button("Remove")) {
				confirm_open_ = true;
				confirm_text_ = "Remove " + d.name + "? Folders stop syncing with it.";
				const std::string id = d.id;
				confirm_action_ = [this, id] {
					remove_device(app_, id);
					toast("Device removed");
				};
			}
			ImGui::PopID();
		}
		ImGui::EndTable();
	}
}

void ConsoleUi::draw_folders()
{
	if (snap_.folders.empty())
		ImGui::TextWrapped("No folders yet. Accept a folder a device offers (Requests), or choose Share... on a device.");
	for (const auto &f : snap_.folders) {
		ImGui::PushID(f.id.c_str());
		ImGui::BeginChild(("f" + f.id).c_str(), ImVec2(0, 0),
			ImGuiChildFlags_Borders | ImGuiChildFlags_NavFlattened | ImGuiChildFlags_AutoResizeY, 0);
		ImGui::TextUnformatted(f.label.c_str());
		ImGui::SameLine();
		ImGui::TextDisabled("(%s)", f.id.c_str());
		ImGui::SameLine(ImGui::GetContentRegionAvail().x * 0.70f);
		const std::string st = state_text(f);
		ImGui::TextColored(text_colour(st), "%s", st.c_str());
		ImGui::TextDisabled("%s", f.path.c_str());
		std::string shared;
		for (const auto &n : f.shared_with)
			shared += (shared.empty() ? "" : ", ") + n;
		ImGui::Text("%lld files, %s   Shared with: %s", static_cast<long long>(f.local_files), human_bytes(f.global_bytes).c_str(),
			shared.empty() ? "no one yet" : shared.c_str());
		if (f.need_items > 0)
			progress(f.completion, 300);
		if (!f.error.empty())
			ImGui::TextColored(kBad, "%s", f.error.c_str());
		else if (f.errors > 0)
			ImGui::TextColored(kWarn, "%d item%s could not be synced (see the Web GUI's Failed Items)", f.errors,
				f.errors == 1 ? "" : "s");
		if (ImGui::Button("Rescan"))
			rescan(app_, f.id), toast("Rescanning " + f.label);
		if (ImGui::IsItemFocused() && ImGui::IsKeyPressed(ImGuiKey_GamepadFaceLeft, false))
			rescan(app_, f.id), toast("Rescanning " + f.label);
		ImGui::SameLine();
		if (ImGui::Button(f.paused ? "Resume" : "Pause"))
			set_folder_paused(app_, f.id, !f.paused);
		ImGui::SameLine();
		if (ImGui::Button("Remove")) {
			confirm_open_ = true;
			confirm_text_ = "Stop syncing " + f.label + "? Files on the drive stay where they are.";
			const std::string id = f.id;
			confirm_action_ = [this, id] {
				remove_folder(app_, id);
				toast("Folder removed");
			};
		}
		ImGui::EndChild();
		ImGui::PopID();
	}
}

void ConsoleUi::draw_pending()
{
	if (snap_.pending_devices.empty() && snap_.pending_folders.empty()) {
		ImGui::TextWrapped("No requests. When a device you haven't added connects, or offers a folder, it shows here.");
		return;
	}
	for (const auto &p : snap_.pending_devices) {
		ImGui::PushID(p.id.c_str());
		ImGui::BeginChild(("pd" + p.id).c_str(), ImVec2(0, 0),
			ImGuiChildFlags_Borders | ImGuiChildFlags_NavFlattened | ImGuiChildFlags_AutoResizeY, 0);
		ImGui::TextColored(kWarn, "Device \"%s\" wants to connect", p.name.c_str());
		ImGui::TextDisabled("%s  from %s", p.id.c_str(), p.address.c_str());
		if (ImGui::Button("Add device")) {
			accept_pending_device(app_, p.id, p.name);
			toast("Added " + p.name);
		}
		ImGui::SameLine();
		if (ImGui::Button("Ignore"))
			dismiss_pending_device(app_, p.id);
		ImGui::EndChild();
		ImGui::PopID();
	}
	for (const auto &p : snap_.pending_folders) {
		ImGui::PushID((p.folder_id + p.device_id).c_str());
		ImGui::BeginChild(("pf" + p.folder_id + p.device_id).c_str(), ImVec2(0, 0),
			ImGuiChildFlags_Borders | ImGuiChildFlags_NavFlattened | ImGuiChildFlags_AutoResizeY, 0);
		ImGui::TextColored(kWarn, "%s wants to share \"%s\"", p.device_name.c_str(), p.label.c_str());
		ImGui::TextDisabled("Folder ID %s", p.folder_id.c_str());
		if (ImGui::Button("Accept...")) {
			share_open_ = true;
			share_from_pending_ = true;
			share_pending_ = p;
			share_error_.clear();
			std::snprintf(share_id_, sizeof share_id_, "%s", p.folder_id.c_str());
			std::snprintf(share_label_, sizeof share_label_, "%s", p.label.c_str());
			std::snprintf(share_path_, sizeof share_path_, "%s", default_folder_path(app_, p.label).c_str());
		}
		ImGui::SameLine();
		if (ImGui::Button("Ignore"))
			dismiss_pending_folder(app_, p);
		ImGui::EndChild();
		ImGui::PopID();
	}
}

void ConsoleUi::draw_settings()
{
	ImGui::TextColored(kAccent, "Web GUI");
	bool gui_on = !snap_.gui_url.empty() || app_.config().gui().value("enabled", true);
	if (ImGui::Checkbox("Serve the Web GUI on the network (port 8384)", &gui_on))
		set_gui_enabled(app_, gui_on);
	if (!snap_.gui_url.empty())
		ImGui::TextDisabled("Open %s in a browser on your PC.", snap_.gui_url.c_str());
	ImGui::Spacing();
	ImGui::TextUnformatted(snap_.gui_has_password ? "GUI login (set; enter a new one to change it)" : "GUI login (not set)");
	ImGui::AlignTextToFramePadding();
	ImGui::TextUnformatted("User    ");
	ImGui::SameLine();
	ImGui::SetNextItemWidth(360);
	ImGui::InputText("##user", gui_user_, sizeof gui_user_);
	ImGui::AlignTextToFramePadding();
	ImGui::TextUnformatted("Password");
	ImGui::SameLine();
	ImGui::SetNextItemWidth(360);
	ImGui::InputText("##pass", gui_pass_, sizeof gui_pass_, ImGuiInputTextFlags_Password);
	if (ImGui::Button("Save login") && gui_user_[0] && gui_pass_[0]) {
		set_gui_password(app_, gui_user_, gui_pass_);
		std::memset(gui_pass_, 0, sizeof gui_pass_);
		toast("GUI login saved");
	}
	ImGui::Separator();
	ImGui::TextColored(kAccent, "Network");
	bool local = snap_.local_discovery, global = snap_.global_discovery, relays = snap_.relays;
	if (ImGui::Checkbox("Find devices on this network (local discovery)", &local))
		set_option(app_, "localAnnounceEnabled", local);
	if (ImGui::Checkbox("Find devices over the internet (global discovery)", &global))
		set_option(app_, "globalAnnounceEnabled", global);
	if (ImGui::Checkbox("Use relays when a direct connection isn't possible", &relays))
		set_option(app_, "relaysEnabled", relays);
	ImGui::TextDisabled("Listening on TCP port %s.%s", snap_.listen_address.c_str(),
		snap_.relay.empty() ? "" : "  Joined a relay.");
	for (const auto &e : snap_.discovery_errors)
		ImGui::TextColored(kWarn, "%s", e.c_str());
	ImGui::Separator();
	ImGui::TextColored(kAccent, "Storage");
	ImGui::TextWrapped("The internal hard disk (FATX) keeps names of up to 42 characters and files under 4 GB; files "
					   "that don't fit are listed as failed items and left alone on your other devices. USB drives with "
					   "exFAT have neither limit.");
}

void ConsoleUi::draw_log()
{
	const auto lines = platform::recent_log();
	ImGui::BeginChild("log", ImVec2(0, 0), ImGuiChildFlags_None);
	for (const auto &l : lines) {
		ImGui::TextDisabled("%s", l.when.substr(11, 8).c_str());
		ImGui::SameLine();
		ImGui::TextWrapped("%s", l.message.c_str());
	}
	if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 5)
		ImGui::SetScrollHereY(1.0f);
	ImGui::EndChild();
}

void ConsoleUi::modal_add_device()
{
	if (add_device_open_) {
		ImGui::OpenPopup("Add a device");
		add_device_open_ = false;
	}
	ImGui::SetNextWindowSize(ImVec2(820, 0));
	if (ImGui::BeginPopupModal("Add a device", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove)) {
		ImGui::TextWrapped("Enter the other device's ID: in Syncthing there, Actions > Show ID. You can leave out the "
						   "dashes; 0, 1 and 8 are read as O, I and B.");
		text_field("Device ID", add_device_id_, sizeof add_device_id_);
		text_field("Name (optional)", add_device_name_, sizeof add_device_name_);
		if (!add_device_error_.empty())
			ImGui::TextColored(kBad, "%s", add_device_error_.c_str());
		if (ImGui::Button("Add", ImVec2(160, 0))) {
			if (add_device(app_, add_device_id_, add_device_name_, &add_device_error_)) {
				toast("Device added");
				ImGui::CloseCurrentPopup();
			}
		}
		ImGui::SameLine();
		if (ImGui::Button("Cancel", ImVec2(160, 0)) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false))
			ImGui::CloseCurrentPopup();
		ImGui::EndPopup();
	}
}

void ConsoleUi::modal_share_folder()
{
	if (share_open_) {
		ImGui::OpenPopup("Folder");
		share_open_ = false;
	}
	ImGui::SetNextWindowSize(ImVec2(860, 0));
	if (ImGui::BeginPopupModal("Folder", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove)) {
		if (share_from_pending_) {
			ImGui::Text("Accept \"%s\" from %s", share_pending_.label.c_str(), share_pending_.device_name.c_str());
		} else {
			ImGui::TextWrapped("Share a folder with this device: pick an existing one, or make a new one.");
			for (const auto &f : snap_.folders) {
				if (std::find(f.shared_with.begin(), f.shared_with.end(), "") != f.shared_with.end())
					continue;
				if (ImGui::Button(("Share \"" + f.label + "\"").c_str())) {
					if (share_folder(app_, f.id, f.label, f.path, share_device_, &share_error_)) {
						toast("Shared " + f.label);
						ImGui::CloseCurrentPopup();
					}
				}
			}
			ImGui::Separator();
			text_field("New folder ID (the same on every device)", share_id_, sizeof share_id_);
			text_field("Label", share_label_, sizeof share_label_);
		}
		if (!share_path_[0] && share_label_[0])
			std::snprintf(share_path_, sizeof share_path_, "%s", default_folder_path(app_, share_label_).c_str());
		text_field("Where on the console (hdd:/..., usb0:/...)", share_path_, sizeof share_path_);
		if (!share_error_.empty())
			ImGui::TextColored(kBad, "%s", share_error_.c_str());
		if (ImGui::Button(share_from_pending_ ? "Accept" : "Create and share", ImVec2(220, 0))) {
			const bool ok = share_from_pending_
				? accept_pending_folder(app_, share_pending_, share_path_, &share_error_)
				: share_folder(app_, share_id_, share_label_, share_path_, share_device_, &share_error_);
			if (ok) {
				toast("Folder added");
				ImGui::CloseCurrentPopup();
			}
		}
		ImGui::SameLine();
		if (ImGui::Button("Cancel", ImVec2(160, 0)) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false))
			ImGui::CloseCurrentPopup();
		ImGui::EndPopup();
	}
}

void ConsoleUi::modal_confirm()
{
	if (confirm_open_) {
		ImGui::OpenPopup("Are you sure?");
		confirm_open_ = false;
	}
	if (ImGui::BeginPopupModal("Are you sure?", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove)) {
		ImGui::TextUnformatted(confirm_text_.c_str());
		if (ImGui::Button("Yes", ImVec2(160, 0))) {
			if (confirm_action_)
				confirm_action_();
			ImGui::CloseCurrentPopup();
		}
		ImGui::SameLine();
		ImGui::SetItemDefaultFocus();
		if (ImGui::Button("No", ImVec2(160, 0)) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false))
			ImGui::CloseCurrentPopup();
		ImGui::EndPopup();
	}
}

} // namespace xst::ui
