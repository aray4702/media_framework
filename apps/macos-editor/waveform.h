#pragma once

// A file's audio as peaks for drawing: the loudest sample of each 1/kPeaksPerSecond of media
// time, scaled so the file's loudest is 1 (quiet files still show their shape).

#import <Foundation/Foundation.h>

#include <vector>

namespace editor {

constexpr int kPeaksPerSecond = 50;

// Reads the file's first audio track off the main thread; `done` runs on the main queue with
// the peaks, empty when the file has no readable audio.
void loadPeaks(NSString* path, void (^done)(std::vector<float> peaks));

}  // namespace editor
