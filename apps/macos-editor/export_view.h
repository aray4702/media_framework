#pragma once

// The Export tab: renders the scene into video files with mf::Exporter. It lists outputs, each
// with a format (MP4 or MOV, H.264 or HEVC), a resolution (the project's, or a standard height at
// the project's aspect ratio), a frame rate (the project's, or a standard one) and a quality
// (which sets the bitrate); Add Output adds one. Export… asks for a name once and writes one file
// per output, one after another, with progress and Cancel, then Show in Finder. The editor's own
// preview keeps working meanwhile.

#import <AppKit/AppKit.h>

#include "document.h"
#include "mf/adapters.h"

@interface ExportView : NSView
- (instancetype)initWithFrame:(NSRect)frame document:(editor::Document*)doc platform:(mf::PlatformFactory*)platform;
- (void)refresh;  // the project changed: its size and rate in the options, and whether there's anything to export
- (void)cancel;   // stops an export in progress (e.g. the app is quitting)
@end
