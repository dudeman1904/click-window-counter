#pragma once

// .gdr2 loading + conversion to 240 Hz input events. No Geode dependency.

#include <gdr/gdr.hpp>

#include <cmath>
#include <filesystem>
#include <string>
#include <vector>

#include "Searcher.hpp"

namespace cw {

struct LoadedMacro {
	std::vector<InputEvent> events; // frames converted to 240 Hz
	double sourceFramerate = 240.0;
	size_t rawInputs = 0;
	std::string author;
	std::string levelName;
};

inline bool convertReplay(gdr::Replay<> const& replay, LoadedMacro& out, std::string& err) {
	double fps = replay.framerate;
	if (!(fps > 0.0)) fps = 240.0;
	out.sourceFramerate = fps;
	out.rawInputs = replay.inputs.size();
	out.author = replay.author;
	out.levelName = replay.levelInfo.name;
	out.events.clear();
	out.events.reserve(replay.inputs.size());
	for (auto const& in : replay.inputs) {
		InputEvent e;
		e.frame = static_cast<int64_t>(std::llround(static_cast<double>(in.frame) * 240.0 / fps));
		e.button = in.button;
		e.p2 = in.player2;
		e.down = in.down;
		out.events.push_back(e);
	}
	if (out.events.empty()) {
		err = "The macro has no inputs";
		return false;
	}
	return true;
}

inline bool loadMacro(std::filesystem::path const& path, LoadedMacro& out, std::string& err) {
	try {
		auto res = gdr::Replay<>::importData(path);
		if (res.isErr()) {
			err = "Couldn't read .gdr2 file: " + res.unwrapErr();
			return false;
		}
		return convertReplay(res.unwrap(), out, err);
	} catch (std::exception const& e) {
		err = std::string("Couldn't read .gdr2 file: ") + e.what();
		return false;
	}
}

} // namespace cw
