#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "app.hpp"
#include "ui/view.hpp"

struct ImFont;

namespace xst::ui {

// The controller-driven console UI, drawn with Dear ImGui at 1280x720 inside the title-safe area. It reads the daemon
// only through ui::snapshot and changes it only through ui::* actions (view.hpp): the Web GUI sees every change.
//
//   LB / RB      previous / next screen        D-pad / left stick   move focus
//   A            activate                       B                    back / cancel
//   X            context action (pause, rescan)  Y                    screen action (add, show ID)
//   Back         (held 1 s) quit                 Text fields open the console's on-screen keyboard.
class ConsoleUi {
public:
	enum class Screen { dashboard, device_id, devices, folders, pending, settings, log };

	explicit ConsoleUi(App &app);

	// Draws one frame's UI (between ImGui::NewFrame and ImGui::Render). Returns false when the user asked to quit.
	bool frame(float width, float height);
	// Style and fonts for a television: larger text, high contrast. Call once after ImGui::CreateContext.
	static void setup_style(float scale = 1.0f);

	Screen screen() const { return screen_; }
	// The dialog open at the end of the last frame ("" if none): "Add a device", "Folder", "Are you sure?".
	const std::string &open_dialog() const { return open_dialog_; }
	void go(Screen s) { screen_ = s; }

	// The QR code of a text as rows of modules (true = dark), from Nayuki's qrcodegen; empty on failure.
	static std::vector<std::vector<bool>> qr(const std::string &text);

private:
	void header(float width);
	void tabs();
	void footer(float width, float height);
	void draw_dashboard();
	void draw_device_id();
	void draw_devices();
	void draw_folders();
	void draw_pending();
	void draw_settings();
	void draw_log();
	void draw_qr(const std::string &text, float size);
	void modal_add_device();
	void modal_share_folder();
	void modal_confirm();
	void toast(const std::string &text);

	App &app_;
	Snapshot snap_;
	std::chrono::steady_clock::time_point snap_at_{};
	Screen screen_ = Screen::dashboard;
	bool want_quit_ = false;

	// Dialog state.
	bool add_device_open_ = false;
	char add_device_id_[80] = {};
	char add_device_name_[64] = {};
	std::string add_device_error_;

	bool share_open_ = false;
	char share_id_[64] = {};
	char share_label_[64] = {};
	char share_path_[256] = {};
	std::string share_device_; // device to share with
	PendingFolder share_pending_{};
	bool share_from_pending_ = false;
	std::string share_error_;

	bool confirm_open_ = false;
	std::string confirm_text_;
	std::function<void()> confirm_action_;

	char gui_user_[64] = {};
	char gui_pass_[64] = {};

	std::string open_dialog_;
	std::string toast_;
	std::chrono::steady_clock::time_point toast_until_{};
	std::chrono::steady_clock::time_point back_held_since_{};
};

} // namespace xst::ui
