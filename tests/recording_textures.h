// A headless TextureOps for the text tests: every upload's pixels are kept in memory instead of GL.
#pragma once

#include <cstddef>
#include <map>
#include <vector>

#include "imgui_accesskit/run_cache.h"

namespace imgui_accesskit::text::test_support {

struct Texture {
  int width = 0, height = 0, channels = 1;
  std::vector<unsigned char> pixels;  // tightly packed rows
};

// Must outlive the RunCache it is given to.
class RecordingTextures {
 public:
  TextureOps ops() {
    TextureOps o;
    o.upload_alpha = [this](const unsigned char* data, int width, int height, int stride, unsigned reuse) {
      const unsigned id = reuse != 0 ? reuse : next_++;
      Texture& t = live_[id];
      t = {width, height, 1, {}};
      for (int y = 0; y < height; ++y) {
        const unsigned char* row = data + static_cast<size_t>(y) * static_cast<size_t>(stride);
        t.pixels.insert(t.pixels.end(), row, row + width);
      }
      ++uploads_;
      last_ = id;
      return id;
    };
    o.upload_rgba = [this](const unsigned char* data, int width, int height) {
      const unsigned id = next_++;
      live_[id] = {width, height, 4, {data, data + static_cast<size_t>(width) * static_cast<size_t>(height) * 4}};
      ++uploads_;
      last_ = id;
      return id;
    };
    o.destroy = [this](unsigned id) {
      destroyed_.push_back(id);
      live_.erase(id);
    };
    return o;
  }

  const Texture* find(unsigned id) const {
    const auto it = live_.find(id);
    return it == live_.end() ? nullptr : &it->second;
  }
  int uploads() const { return uploads_; }
  unsigned last_upload() const { return last_; }
  size_t live() const { return live_.size(); }
  const std::vector<unsigned>& destroyed() const { return destroyed_; }

 private:
  std::map<unsigned, Texture> live_;
  std::vector<unsigned> destroyed_;
  unsigned next_ = 1, last_ = 0;
  int uploads_ = 0;
};

}  // namespace imgui_accesskit::text::test_support
