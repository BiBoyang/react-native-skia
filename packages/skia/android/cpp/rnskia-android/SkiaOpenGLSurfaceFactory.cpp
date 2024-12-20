#include "SkiaOpenGLSurfaceFactory.h"
#include "GrAHardwareBufferUtils.h"
#include "SkiaOpenGLHelper.h"

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdocumentation"

#include "include/gpu/ganesh/SkImageGanesh.h"
#include "include/gpu/ganesh/gl/GrGLBackendSurface.h"
#include "src/gpu/ganesh/gl/GrGLDefines.h"

#pragma clang diagnostic pop

namespace RNSkia {

sk_sp<SkImage> SkiaOpenGLSurfaceFactory::makeImageFromHardwareBuffer(
    SkiaOpenGLContext *context, void *buffer, bool requireKnownFormat) {
#if __ANDROID_API__ >= 26
  const AHardwareBuffer *hardwareBuffer =
      static_cast<AHardwareBuffer *>(buffer);
  DeleteImageProc deleteImageProc = nullptr;
  UpdateImageProc updateImageProc = nullptr;
  TexImageCtx deleteImageCtx = nullptr;

  AHardwareBuffer_Desc description;
  AHardwareBuffer_describe(hardwareBuffer, &description);
  GrBackendFormat format;
  switch (description.format) {
  // TODO: find out if we can detect, which graphic buffers support
  // GR_GL_TEXTURE_2D
  case AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM:
    format = GrBackendFormats::MakeGL(GR_GL_RGBA8, GR_GL_TEXTURE_EXTERNAL);
  case AHARDWAREBUFFER_FORMAT_R16G16B16A16_FLOAT:
    format = GrBackendFormats::MakeGL(GR_GL_RGBA16F, GR_GL_TEXTURE_EXTERNAL);
  case AHARDWAREBUFFER_FORMAT_R5G6B5_UNORM:
    format = GrBackendFormats::MakeGL(GR_GL_RGB565, GR_GL_TEXTURE_EXTERNAL);
  case AHARDWAREBUFFER_FORMAT_R10G10B10A2_UNORM:
    format = GrBackendFormats::MakeGL(GR_GL_RGB10_A2, GR_GL_TEXTURE_EXTERNAL);
  case AHARDWAREBUFFER_FORMAT_R8G8B8_UNORM:
    format = GrBackendFormats::MakeGL(GR_GL_RGB8, GR_GL_TEXTURE_EXTERNAL);
#if __ANDROID_API__ >= 33
  case AHARDWAREBUFFER_FORMAT_R8_UNORM:
    format = GrBackendFormats::MakeGL(GR_GL_R8, GR_GL_TEXTURE_EXTERNAL);
#endif
  default:
    if (requireKnownFormat) {
      format = GrBackendFormat();
    } else {
      format = GrBackendFormats::MakeGL(GR_GL_RGBA8, GR_GL_TEXTURE_EXTERNAL);
    }
  }

  auto backendTex = MakeGLBackendTexture(
      context->directContext.get(),
      const_cast<AHardwareBuffer *>(hardwareBuffer), description.width,
      description.height, &deleteImageProc, &updateImageProc, &deleteImageCtx,
      false, format, false);
  if (!backendTex.isValid()) {
    RNSkLogger::logToConsole(
        "Failed to convert HardwareBuffer to OpenGL Texture!");
    return nullptr;
  }
  sk_sp<SkImage> image = SkImages::BorrowTextureFrom(
      context->directContext.get(), backendTex, kTopLeft_GrSurfaceOrigin,
      kRGBA_8888_SkColorType, kOpaque_SkAlphaType, nullptr, deleteImageProc,
      deleteImageCtx);
  return image;
#else
  throw std::runtime_error(
      "HardwareBuffers are only supported on Android API 26 or higher! Set "
      "your minSdk to 26 (or higher) and try again.");
#endif
}

sk_sp<SkSurface>
SkiaOpenGLSurfaceFactory::makeOffscreenSurface(SkiaOpenGLContext *context,
                                               int width, int height) {

  auto colorType = kN32_SkColorType;

  SkSurfaceProps props(0, kUnknown_SkPixelGeometry);

  if (!SkiaOpenGLHelper::makeCurrent(context, context->gl1x1Surface)) {
    RNSkLogger::logToConsole(
        "Could not create EGL Surface from native window / surface. Could "
        "not set new surface as current surface.");
    return nullptr;
  }

  // Create texture
  auto texture = context->directContext->createBackendTexture(
      width, height, colorType, skgpu::Mipmapped::kNo, GrRenderable::kYes);

  if (!texture.isValid()) {
    RNSkLogger::logToConsole("couldn't create offscreen texture %dx%d", width,
                             height);
  }

  struct ReleaseContext {
    SkiaOpenGLContext *context;
    GrBackendTexture texture;
  };

  auto releaseCtx = new ReleaseContext({context, texture});

  // Create a SkSurface from the GrBackendTexture
  return SkSurfaces::WrapBackendTexture(
      context->directContext.get(), texture, kTopLeft_GrSurfaceOrigin, 0,
      colorType, nullptr, &props,
      [](void *addr) {
        auto releaseCtx = reinterpret_cast<ReleaseContext *>(addr);

        releaseCtx->context->directContext->deleteBackendTexture(
            releaseCtx->texture);
      },
      releaseCtx);
}

sk_sp<SkSurface> AndroidSkiaContext::getSurface() {
  if (_skSurface == nullptr) {

    // Now we can create a surface
    _glSurface = SkiaOpenGLHelper::createWindowedSurface(_window);
    if (_glSurface == EGL_NO_SURFACE) {
      RNSkLogger::logToConsole(
          "Could not create EGL Surface from native window / surface.");
      return nullptr;
    }

    // Now make this one current
    if (!SkiaOpenGLHelper::makeCurrent(_context, _glSurface)) {
      RNSkLogger::logToConsole(
          "Could not create EGL Surface from native window / surface. Could "
          "not set new surface as current surface.");
      return nullptr;
    }

    // Set up parameters for the render target so that it
    // matches the underlying OpenGL context.
    GrGLFramebufferInfo fboInfo;

    // We pass 0 as the framebuffer id, since the
    // underlying Skia GrGlGpu will read this when wrapping the context in the
    // render target and the GrGlGpu object.
    fboInfo.fFBOID = 0;
    fboInfo.fFormat = 0x8058; // GL_RGBA8

    GLint stencil;
    glGetIntegerv(GL_STENCIL_BITS, &stencil);

    GLint samples;
    glGetIntegerv(GL_SAMPLES, &samples);

    auto colorType = kN32_SkColorType;

    auto maxSamples =
        _context->directContext->maxSurfaceSampleCountForColorType(colorType);

    if (samples > maxSamples) {
      samples = maxSamples;
    }

    auto renderTarget = GrBackendRenderTargets::MakeGL(_width, _height, samples,
                                                       stencil, fboInfo);

    SkSurfaceProps props(0, kUnknown_SkPixelGeometry);

    struct ReleaseContext {
      EGLSurface glSurface;
    };

    auto releaseCtx = new ReleaseContext({_glSurface});

    // Create surface object
    _skSurface = SkSurfaces::WrapBackendRenderTarget(
        _context->directContext.get(), renderTarget,
        kBottomLeft_GrSurfaceOrigin, colorType, nullptr, &props,
        [](void *addr) {
          auto releaseCtx = reinterpret_cast<ReleaseContext *>(addr);
          SkiaOpenGLHelper::destroySurface(releaseCtx->glSurface);
          delete releaseCtx;
        },
        reinterpret_cast<void *>(releaseCtx));
  }
  return _skSurface;
}

} // namespace RNSkia
