// IImageLoader on ImageIO: a still image (PNG, JPEG, HEIF, ...) into an IOSurface-backed BGRA
// CVPixelBuffer (premultiplied), which the compositor samples directly. EXIF orientation is
// applied, and images are capped at 8192 pixels on their longer side.

#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>
#import <ImageIO/ImageIO.h>

#include "mf/macos.h"

namespace mf::macos {
namespace {

class ImageIoLoader : public IImageLoader {
 public:
  Result load(const MediaSource& source, VideoFrame* out, int* width, int* height) override {
    @autoreleasepool {
      NSURL* url = (__bridge NSURL*)source.native.get();
      if (!url) return Result::InvalidArgument;
      CGImageSourceRef src = CGImageSourceCreateWithURL((__bridge CFURLRef)url, nullptr);
      if (!src) return Result::FileOpenFailed;
      NSDictionary* options = @{
        (id)kCGImageSourceCreateThumbnailFromImageAlways : @YES,
        (id)kCGImageSourceCreateThumbnailWithTransform : @YES,  // EXIF orientation
        (id)kCGImageSourceThumbnailMaxPixelSize : @8192,
        (id)kCGImageSourceShouldCacheImmediately : @YES,
      };
      CGImageRef image = CGImageSourceCreateThumbnailAtIndex(src, 0, (__bridge CFDictionaryRef)options);
      CFRelease(src);
      if (!image) return Result::UnsupportedFormat;
      size_t w = CGImageGetWidth(image), h = CGImageGetHeight(image);

      NSDictionary* attrs = @{
        (id)kCVPixelBufferMetalCompatibilityKey : @YES,
        (id)kCVPixelBufferIOSurfacePropertiesKey : @{},
      };
      CVPixelBufferRef pixels = nullptr;
      if (CVPixelBufferCreate(nullptr, w, h, kCVPixelFormatType_32BGRA, (__bridge CFDictionaryRef)attrs, &pixels) != kCVReturnSuccess) {
        CGImageRelease(image);
        return Result::UnsupportedFormat;
      }
      CVPixelBufferLockBaseAddress(pixels, 0);
      CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
      CGContextRef ctx = CGBitmapContextCreate(CVPixelBufferGetBaseAddress(pixels), w, h, 8, CVPixelBufferGetBytesPerRow(pixels), space,
                                               kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little);
      if (ctx) {
        CGContextClearRect(ctx, CGRectMake(0, 0, w, h));
        CGContextDrawImage(ctx, CGRectMake(0, 0, w, h), image);
        CGContextRelease(ctx);
      }
      CGColorSpaceRelease(space);
      CVPixelBufferUnlockBaseAddress(pixels, 0);
      CGImageRelease(image);
      if (!ctx) {
        CVPixelBufferRelease(pixels);
        return Result::UnsupportedFormat;
      }
      out->image = std::shared_ptr<void>(pixels, [](void* p) { CVPixelBufferRelease(static_cast<CVPixelBufferRef>(p)); });
      *width = int(w);
      *height = int(h);
      return Result::Ok;
    }
  }
};

}  // namespace

std::unique_ptr<IImageLoader> createImageLoader() { return std::make_unique<ImageIoLoader>(); }

}  // namespace mf::macos
