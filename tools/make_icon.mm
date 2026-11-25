// Build-time tool: renders the VortexClean application icon with Core Graphics
// and writes it as a multi-resolution .icns through ImageIO.
//
//   vortex_make_icon <output.icns>
//
// The icon is generated instead of being checked in, so the repository holds no
// binary assets and the artwork stays reviewable as code.
#import <CoreGraphics/CoreGraphics.h>
#import <Foundation/Foundation.h>
#import <ImageIO/ImageIO.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

namespace {

struct Variant {
    size_t pixels;
    double dpi;  // 144 marks an @2x representation
};

// The representations a macOS icon is expected to carry (16...512 pt, 1x and 2x).
constexpr Variant kVariants[] = {
    {16, 72.0},  {32, 144.0},  {32, 72.0},  {64, 144.0},   {128, 72.0},
    {256, 144.0}, {256, 72.0}, {512, 144.0}, {512, 72.0},  {1024, 144.0},
};

CGColorRef CreateColor(CGColorSpaceRef space, CGFloat r, CGFloat g, CGFloat b, CGFloat a) {
    const CGFloat components[] = {r, g, b, a};
    return CGColorCreate(space, components);
}

/// A continuous-corner ("squircle"-like) rounded rectangle.
CGPathRef CreateBodyPath(CGRect rect, CGFloat radius) {
    return CGPathCreateWithRoundedRect(rect, radius, radius, nullptr);
}

struct Arm {
    double radius;      // radius at the head of the arm
    double headWidth;   // stroke width at the head
    double startAngle;  // where the head sits (radians, counter-clockwise from +x)
};

/// Renders the vortex arms into an image by evaluating their coverage per pixel.
/// Each arm is an arc that thins, fades and pulls slightly inward along its
/// length; computing it analytically gives smooth, seam-free gradients that a
/// stroked path cannot express.
CGImageRef CreateArmsImage(size_t pixels, const std::vector<Arm> &arms, double sweep, CGColorSpaceRef space) {
    const double size = static_cast<double>(pixels);
    const double center = size / 2.0;
    const double twoPi = 2.0 * M_PI;
    std::vector<uint8_t> buffer(pixels * pixels * 4, 0);

    for (size_t y = 0; y < pixels; ++y) {
        for (size_t x = 0; x < pixels; ++x) {
            const double dx = (static_cast<double>(x) + 0.5) - center;
            // Bitmap rows run top-down, Core Graphics angles bottom-up.
            const double dy = center - (static_cast<double>(y) + 0.5);
            const double r = std::hypot(dx, dy);
            const double theta = std::atan2(dy, dx);

            double alpha = 0.0;
            for (const Arm &arm : arms) {
                // Position along the arm: 0 at the head, 1 at the tail (clockwise).
                double delta = std::fmod(arm.startAngle - theta, twoPi);
                if (delta < 0.0) delta += twoPi;
                const double t = delta / sweep;
                double coverage = 0.0;
                double strength = 1.0;
                if (t <= 1.0) {
                    const double bandRadius = arm.radius * (1.0 - 0.10 * t);
                    const double halfWidth = arm.headWidth * (1.0 - 0.72 * t) / 2.0;
                    coverage = std::clamp(halfWidth - std::fabs(r - bandRadius) + 0.5, 0.0, 1.0);
                    // Anti-alias the very end of the tail.
                    coverage *= std::clamp((1.0 - t) * sweep * bandRadius + 0.5, 0.0, 1.0);
                    strength = 1.0 - 0.80 * t;
                } else {
                    // Round cap at the head: a disc centred on the start of the arc.
                    const double headX = arm.radius * std::cos(arm.startAngle);
                    const double headY = arm.radius * std::sin(arm.startAngle);
                    const double distance = std::hypot(dx - headX, dy - headY);
                    coverage = std::clamp(arm.headWidth / 2.0 - distance + 0.5, 0.0, 1.0);
                }
                alpha = std::max(alpha, coverage * strength);
            }
            if (alpha <= 0.0) continue;
            const auto value = static_cast<uint8_t>(std::lround(alpha * 255.0));
            uint8_t *pixel = &buffer[(y * pixels + x) * 4];
            pixel[0] = pixel[1] = pixel[2] = pixel[3] = value;  // premultiplied white
        }
    }

    CFDataRef data = CFDataCreate(kCFAllocatorDefault, buffer.data(), static_cast<CFIndex>(buffer.size()));
    CGDataProviderRef provider = CGDataProviderCreateWithCFData(data);
    CGImageRef image = CGImageCreate(pixels, pixels, 8, 32, pixels * 4, space,
                                     static_cast<CGBitmapInfo>(kCGImageAlphaPremultipliedLast), provider, nullptr,
                                     true, kCGRenderingIntentDefault);
    CGDataProviderRelease(provider);
    CFRelease(data);
    return image;
}

CGImageRef CreateIcon(size_t pixels) {
    CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGContextRef context = CGBitmapContextCreate(nullptr, pixels, pixels, 8, 0, space,
                                                 static_cast<uint32_t>(kCGImageAlphaPremultipliedLast));
    if (context == nullptr) {
        CGColorSpaceRelease(space);
        return nullptr;
    }
    const CGFloat size = static_cast<CGFloat>(pixels);
    CGContextSetInterpolationQuality(context, kCGInterpolationHigh);
    CGContextSetShouldAntialias(context, true);

    // ---- Body: Apple's icon grid leaves ~10% margin around the rounded square.
    const CGFloat inset = size * 0.098;
    const CGRect body = CGRectMake(inset, inset, size - 2.0 * inset, size - 2.0 * inset);
    const CGFloat radius = body.size.width * 0.2237;
    CGPathRef bodyPath = CreateBodyPath(body, radius);

    // Soft drop shadow beneath the body.
    CGContextSaveGState(context);
    CGColorRef shadow = CreateColor(space, 0.0, 0.0, 0.0, 0.32);
    CGContextSetShadowWithColor(context, CGSizeMake(0.0, -size * 0.012), size * 0.028, shadow);
    CGColorRef base = CreateColor(space, 0.17, 0.13, 0.55, 1.0);
    CGContextSetFillColorWithColor(context, base);
    CGContextAddPath(context, bodyPath);
    CGContextFillPath(context);
    CGColorRelease(base);
    CGColorRelease(shadow);
    CGContextRestoreGState(context);

    // Background gradient, clipped to the body.
    CGContextSaveGState(context);
    CGContextAddPath(context, bodyPath);
    CGContextClip(context);
    const CGFloat backgroundColors[] = {
        0.38, 0.50, 1.00, 1.0,  // top: bright azure
        0.27, 0.24, 0.86, 1.0,  // middle: indigo
        0.13, 0.08, 0.44, 1.0,  // bottom: deep violet
    };
    const CGFloat backgroundStops[] = {0.0, 0.48, 1.0};
    CGGradientRef background = CGGradientCreateWithColorComponents(space, backgroundColors, backgroundStops, 3);
    CGContextDrawLinearGradient(context, background, CGPointMake(0.0, CGRectGetMaxY(body)),
                                CGPointMake(0.0, CGRectGetMinY(body)), 0);
    CGGradientRelease(background);

    // A soft glow behind the vortex.
    const CGPoint center = CGPointMake(size / 2.0, size / 2.0);
    const CGFloat glowColors[] = {1.0, 1.0, 1.0, 0.30, 1.0, 1.0, 1.0, 0.0};
    const CGFloat glowStops[] = {0.0, 1.0};
    CGGradientRef glow = CGGradientCreateWithColorComponents(space, glowColors, glowStops, 2);
    CGContextDrawRadialGradient(context, glow, center, 0.0, center, size * 0.36, 0);
    CGGradientRelease(glow);

    // ---- The vortex: three arms at decreasing radii, each rotated by a third of a turn.
    const std::vector<Arm> arms{
        {size * 0.285, size * 0.066, M_PI * 0.50},
        {size * 0.200, size * 0.058, M_PI * (0.50 + 2.0 / 3.0)},
        {size * 0.118, size * 0.050, M_PI * (0.50 + 4.0 / 3.0)},
    };
    if (CGImageRef vortex = CreateArmsImage(pixels, arms, M_PI * 1.22, space)) {
        CGContextDrawImage(context, CGRectMake(0.0, 0.0, size, size), vortex);
        CGImageRelease(vortex);
    }

    // The eye of the vortex.
    CGColorRef eye = CreateColor(space, 1.0, 1.0, 1.0, 1.0);
    CGContextSetFillColorWithColor(context, eye);
    const CGFloat eyeRadius = size * 0.034;
    CGContextFillEllipseInRect(context, CGRectMake(center.x - eyeRadius, center.y - eyeRadius, 2.0 * eyeRadius,
                                                   2.0 * eyeRadius));
    CGColorRelease(eye);

    // A gentle highlight across the top half of the body.
    const CGFloat glossColors[] = {1.0, 1.0, 1.0, 0.16, 1.0, 1.0, 1.0, 0.0};
    CGGradientRef gloss = CGGradientCreateWithColorComponents(space, glossColors, glowStops, 2);
    CGContextDrawLinearGradient(context, gloss, CGPointMake(0.0, CGRectGetMaxY(body)),
                                CGPointMake(0.0, CGRectGetMidY(body)), 0);
    CGGradientRelease(gloss);
    CGContextRestoreGState(context);

    // A hairline inner border keeps the edge crisp on dark backgrounds.
    if (pixels >= 64) {
        CGColorRef border = CreateColor(space, 1.0, 1.0, 1.0, 0.16);
        CGContextSetStrokeColorWithColor(context, border);
        CGContextSetLineWidth(context, MAX(1.0, size / 512.0));
        CGPathRef borderPath = CreateBodyPath(CGRectInset(body, size / 1024.0, size / 1024.0), radius);
        CGContextAddPath(context, borderPath);
        CGContextStrokePath(context);
        CGPathRelease(borderPath);
        CGColorRelease(border);
    }

    CGPathRelease(bodyPath);
    CGImageRef image = CGBitmapContextCreateImage(context);
    CGContextRelease(context);
    CGColorSpaceRelease(space);
    return image;
}

}  // namespace

int main(int argc, const char *argv[]) {
    @autoreleasepool {
        if (argc != 2) {
            std::fprintf(stderr, "usage: %s <output.icns>\n", argv[0]);
            return 2;
        }
        NSURL *url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:argv[1]]];
        const size_t count = sizeof(kVariants) / sizeof(kVariants[0]);
        CGImageDestinationRef destination =
            CGImageDestinationCreateWithURL((__bridge CFURLRef)url, CFSTR("com.apple.icns"), count, nullptr);
        if (destination == nullptr) {
            std::fprintf(stderr, "cannot create %s\n", argv[1]);
            return 1;
        }
        for (const Variant &variant : kVariants) {
            CGImageRef image = CreateIcon(variant.pixels);
            if (image == nullptr) {
                std::fprintf(stderr, "cannot render the %zu px icon\n", variant.pixels);
                CFRelease(destination);
                return 1;
            }
            NSDictionary *properties = @{
                (__bridge NSString *)kCGImagePropertyDPIWidth: @(variant.dpi),
                (__bridge NSString *)kCGImagePropertyDPIHeight: @(variant.dpi),
            };
            CGImageDestinationAddImage(destination, image, (__bridge CFDictionaryRef)properties);
            CGImageRelease(image);
        }
        const bool written = CGImageDestinationFinalize(destination);
        CFRelease(destination);
        if (!written) {
            std::fprintf(stderr, "cannot write %s\n", argv[1]);
            return 1;
        }
    }
    return 0;
}
