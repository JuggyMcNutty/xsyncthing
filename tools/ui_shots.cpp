// xst-ui-shots: renders the console UI's screens to PNG on the host, with a software rasterizer for ImGui's draw
// lists (triangles with textures and clip rects), against a real App whose state the caller sets up. Also drives the
// UI with simulated gamepad input, to check that every screen is reachable and focusable with a controller alone.
//   xst-ui-shots <home-dir> <out-dir>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <thread>
#include <vector>

#include <zlib.h>

#include "app.hpp"
#include "core/fs.hpp"
#include "imgui.h"
#include "imgui_internal.h"
#include "ui/console_ui.hpp"

namespace {

constexpr int W = 1280, H = 720;

struct Canvas {
	std::vector<uint32_t> px = std::vector<uint32_t>(W * H, 0xFF000000);
};

// The font atlas (RGBA32), sampled with the texture's own size.
struct Tex {
	int w = 0, h = 0;
	std::vector<uint32_t> px;
};
std::vector<Tex *> g_textures;

inline uint32_t blend(uint32_t dst, uint32_t src)
{
	const uint32_t a = src >> 24;
	if (a == 255)
		return src;
	if (a == 0)
		return dst;
	auto ch = [&](int s) {
		const uint32_t d = (dst >> s) & 255, c = (src >> s) & 255;
		return ((c * a + d * (255 - a)) / 255) << s;
	};
	return 0xFF000000 | ch(16) | ch(8) | ch(0);
}

inline uint32_t mul(uint32_t a, uint32_t b)
{
	auto ch = [&](int s) { return ((((a >> s) & 255) * ((b >> s) & 255)) / 255) << s; };
	return ch(24) | ch(16) | ch(8) | ch(0);
}

void raster(Canvas &c, const ImDrawVert &v0, const ImDrawVert &v1, const ImDrawVert &v2, const ImVec4 &clip, const Tex *t)
{
	const float minx = std::max(clip.x, std::floor(std::min({ v0.pos.x, v1.pos.x, v2.pos.x })));
	const float maxx = std::min(clip.z, std::ceil(std::max({ v0.pos.x, v1.pos.x, v2.pos.x })));
	const float miny = std::max(clip.y, std::floor(std::min({ v0.pos.y, v1.pos.y, v2.pos.y })));
	const float maxy = std::min(clip.w, std::ceil(std::max({ v0.pos.y, v1.pos.y, v2.pos.y })));
	const float area = (v1.pos.x - v0.pos.x) * (v2.pos.y - v0.pos.y) - (v2.pos.x - v0.pos.x) * (v1.pos.y - v0.pos.y);
	if (std::fabs(area) < 1e-6f)
		return;
	for (int y = static_cast<int>(std::max(0.f, miny)); y < static_cast<int>(std::min<float>(H, maxy)); y++)
		for (int x = static_cast<int>(std::max(0.f, minx)); x < static_cast<int>(std::min<float>(W, maxx)); x++) {
			const float px = x + 0.5f, py = y + 0.5f;
			float w0 = ((v1.pos.x - px) * (v2.pos.y - py) - (v2.pos.x - px) * (v1.pos.y - py)) / area;
			float w1 = ((v2.pos.x - px) * (v0.pos.y - py) - (v0.pos.x - px) * (v2.pos.y - py)) / area;
			float w2 = 1 - w0 - w1;
			if (w0 < -1e-4f || w1 < -1e-4f || w2 < -1e-4f)
				continue;
			auto lerp_col = [&](int s) {
				return static_cast<uint32_t>(std::clamp(w0 * ((v0.col >> s) & 255) + w1 * ((v1.col >> s) & 255) + w2 * ((v2.col >> s) & 255), 0.f, 255.f));
			};
			// ImGui colours are ABGR; convert to ARGB.
			uint32_t col = lerp_col(24) << 24 | lerp_col(0) << 16 | lerp_col(8) << 8 | lerp_col(16);
			if (t && !t->px.empty()) {
				const float u = w0 * v0.uv.x + w1 * v1.uv.x + w2 * v2.uv.x;
				const float v = w0 * v0.uv.y + w1 * v1.uv.y + w2 * v2.uv.y;
				const int tx = std::clamp(static_cast<int>(u * t->w), 0, t->w - 1);
				const int ty = std::clamp(static_cast<int>(v * t->h), 0, t->h - 1);
				const uint32_t s = t->px[static_cast<size_t>(ty * t->w + tx)];
				const uint32_t argb = (s & 0xFF000000) | (s & 0xFF) << 16 | (s & 0xFF00) | (s >> 16 & 0xFF);
				col = mul(col, argb);
			}
			uint32_t &d = c.px[static_cast<size_t>(y * W + x)];
			d = blend(d, col);
		}
}

void update_textures(ImDrawData *dd)
{
	if (!dd->Textures)
		return;
	for (ImTextureData *tex : *dd->Textures) {
		if (tex->Status == ImTextureStatus_OK)
			continue;
		if (tex->Status == ImTextureStatus_WantDestroy) {
			tex->SetTexID(ImTextureID_Invalid);
			tex->SetStatus(ImTextureStatus_Destroyed);
			continue;
		}
		Tex *t = tex->BackendUserData ? static_cast<Tex *>(tex->BackendUserData) : new Tex();
		if (!tex->BackendUserData) {
			g_textures.push_back(t);
			tex->BackendUserData = t;
			tex->SetTexID(static_cast<ImTextureID>(g_textures.size()));
		}
		t->w = tex->Width;
		t->h = tex->Height;
		t->px.assign(static_cast<size_t>(t->w * t->h), 0);
		for (int y = 0; y < t->h; y++)
			std::memcpy(&t->px[static_cast<size_t>(y * t->w)], tex->GetPixelsAt(0, y), static_cast<size_t>(t->w) * 4);
		tex->SetStatus(ImTextureStatus_OK);
	}
}

void render(Canvas &c, ImDrawData *dd)
{
	update_textures(dd);
	std::fill(c.px.begin(), c.px.end(), 0xFF0B1620);
	for (const ImDrawList *list : dd->CmdLists)
		for (const ImDrawCmd &cmd : list->CmdBuffer) {
			if (cmd.UserCallback)
				continue;
			const ImTextureID id = cmd.GetTexID();
			const Tex *t = id && id <= g_textures.size() ? g_textures[id - 1] : nullptr;
			for (unsigned i = 0; i + 2 < cmd.ElemCount; i += 3) {
				const auto &idx = list->IdxBuffer;
				const auto &vtx = list->VtxBuffer;
				raster(c, vtx[cmd.VtxOffset + idx[cmd.IdxOffset + i]], vtx[cmd.VtxOffset + idx[cmd.IdxOffset + i + 1]],
					vtx[cmd.VtxOffset + idx[cmd.IdxOffset + i + 2]], cmd.ClipRect, t);
			}
		}
}

void png(const Canvas &c, const std::string &path)
{
	std::vector<uint8_t> raw;
	raw.reserve(static_cast<size_t>((W * 3 + 1) * H));
	for (int y = 0; y < H; y++) {
		raw.push_back(0);
		for (int x = 0; x < W; x++) {
			const uint32_t p = c.px[static_cast<size_t>(y * W + x)];
			raw.push_back(static_cast<uint8_t>(p >> 16));
			raw.push_back(static_cast<uint8_t>(p >> 8));
			raw.push_back(static_cast<uint8_t>(p));
		}
	}
	uLongf zlen = compressBound(static_cast<uLong>(raw.size()));
	std::vector<uint8_t> z(zlen);
	compress2(z.data(), &zlen, raw.data(), static_cast<uLong>(raw.size()), 6);
	z.resize(zlen);
	std::ofstream out(path, std::ios::binary);
	auto be = [&](uint32_t v) {
		const uint8_t b[4] = { static_cast<uint8_t>(v >> 24), static_cast<uint8_t>(v >> 16), static_cast<uint8_t>(v >> 8), static_cast<uint8_t>(v) };
		out.write(reinterpret_cast<const char *>(b), 4);
	};
	auto chunk = [&](const char *type, const std::vector<uint8_t> &data) {
		be(static_cast<uint32_t>(data.size()));
		std::vector<uint8_t> td(type, type + 4);
		td.insert(td.end(), data.begin(), data.end());
		out.write(reinterpret_cast<const char *>(td.data()), static_cast<std::streamsize>(td.size()));
		be(static_cast<uint32_t>(crc32(0, td.data(), static_cast<uInt>(td.size()))));
	};
	out.write("\x89PNG\r\n\x1a\n", 8);
	std::vector<uint8_t> ihdr = { 0, 0, W >> 8, W & 255, 0, 0, H >> 8, H & 255, 8, 2, 0, 0, 0 };
	chunk("IHDR", ihdr);
	chunk("IDAT", z);
	chunk("IEND", {});
}

// One frame with the given gamepad keys held.
void frame(xst::ui::ConsoleUi &ui, Canvas &c, std::initializer_list<ImGuiKey> held = {})
{
	ImGuiIO &io = ImGui::GetIO();
	io.DisplaySize = ImVec2(W, H);
	io.DeltaTime = 1.0f / 60;
	io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
	for (ImGuiKey k = ImGuiKey_GamepadStart; k <= ImGuiKey_GamepadRStickDown; k = static_cast<ImGuiKey>(k + 1))
		io.AddKeyEvent(k, std::find(held.begin(), held.end(), k) != held.end());
	ImGui::NewFrame();
	ui.frame(W, H);
	ImGui::Render();
	render(c, ImGui::GetDrawData());
}

// Presses a key: one frame down, one up.
void press(xst::ui::ConsoleUi &ui, Canvas &c, ImGuiKey k)
{
	frame(ui, c, { k });
	frame(ui, c);
}

} // namespace

int main(int argc, char **argv)
{
	if (argc < 3) {
		std::fprintf(stderr, "usage: %s <home> <out-dir>\n", argv[0]);
		return 2;
	}
	xst::App::Options o;
	o.data_dir = argv[1];
	o.device_name = "Xbox 360";
	o.open_fs = [](const std::string &p) { return xst::fs::open_posix(p); };
	o.enable_api = true;
	xst::App app(o);
	xst::register_services(app);
	std::string error;
	if (!app.start(&error)) {
		std::fprintf(stderr, "%s\n", error.c_str());
		return 1;
	}
	std::this_thread::sleep_for(std::chrono::seconds(3)); // let folders scan and peers connect

	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGui::GetIO().BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
	xst::ui::ConsoleUi::setup_style();
	xst::ui::ConsoleUi ui(app);
	Canvas c;
	const std::string out = argv[2];
	int failures = 0;
	auto check = [&](const char *name, bool ok) {
		std::printf("%s: %s\n", ok ? "PASS" : "FAIL", name);
		failures += !ok;
	};

	const char *names[] = { "overview", "this-device", "devices", "folders", "requests", "settings", "log" };
	for (int i = 0; i < 7; i++) {
		for (int f = 0; f < 30; f++) // settle (the snapshot refreshes every 0.5 s)
			frame(ui, c);
		std::this_thread::sleep_for(std::chrono::milliseconds(600));
		frame(ui, c);
		frame(ui, c);
		png(c, out + "/ui-" + std::to_string(i) + "-" + names[i] + ".png");
		check((std::string("screen reached with RB: ") + names[i]).c_str(), static_cast<int>(ui.screen()) == i);
		press(ui, c, ImGuiKey_GamepadR1); // next screen, with the controller only
	}
	check("RB from the last screen wraps to the first", ui.screen() == xst::ui::ConsoleUi::Screen::dashboard);
	press(ui, c, ImGuiKey_GamepadL1);
	check("LB goes back to the last screen", ui.screen() == xst::ui::ConsoleUi::Screen::log);

	// Devices: Y opens Add a device; the D-pad moves focus; B closes the dialog.
	ui.go(xst::ui::ConsoleUi::Screen::devices);
	for (int f = 0; f < 5; f++)
		frame(ui, c);
	press(ui, c, ImGuiKey_GamepadFaceUp);
	for (int f = 0; f < 5; f++)
		frame(ui, c);
	check("Y on Devices opens the Add a device dialog", ui.open_dialog() == "Add a device");
	png(c, out + "/ui-7-add-device.png");
	press(ui, c, ImGuiKey_GamepadFaceRight);
	for (int f = 0; f < 5; f++)
		frame(ui, c);
	check("B closes it", ui.open_dialog().empty());

	// Focus: the D-pad moves the navigation cursor to an item on the overview.
	ui.go(xst::ui::ConsoleUi::Screen::dashboard);
	for (int f = 0; f < 5; f++)
		frame(ui, c);
	press(ui, c, ImGuiKey_GamepadDpadDown);
	press(ui, c, ImGuiKey_GamepadDpadDown);
	check("the D-pad gives a focused item (navigation works with a controller)",
		ImGui::GetCurrentContext()->NavId != 0);

	// Back held for a second quits (frame() returns false).
	bool quit = false;
	ImGuiIO &io = ImGui::GetIO();
	for (int f = 0; f < 90 && !quit; f++) {
		io.AddKeyEvent(ImGuiKey_GamepadBack, true);
		ImGui::NewFrame();
		quit = !ui.frame(W, H);
		ImGui::Render();
		std::this_thread::sleep_for(std::chrono::milliseconds(16));
	}
	check("holding Back for a second quits", quit);

	ImGui::DestroyContext();
	app.stop();
	std::printf("ui: %d failed\n", failures);
	return failures ? 1 : 0;
}
