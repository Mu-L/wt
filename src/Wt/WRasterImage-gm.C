/*
 * Copyright (C) 2010 Emweb bv, Herent, Belgium.
 *
 * See the LICENSE file for terms of use.
 */

#include "Wt/WApplication.h"
#include "Wt/WBrush.h"
#include "Wt/WException.h"
#include "Wt/WFontMetrics.h"
#include "Wt/WLogger.h"
#include "Wt/WPainter.h"
#include "Wt/WPen.h"
#include "Wt/WRasterImage.h"
#include "Wt/WTransform.h"

#include "Wt/cpp17/filesystem.hpp"
#include "Wt/Http/Response.h"

// graphicsmagick headers seem to include windows.h, which is bad
#ifdef WT_WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#endif

#include "Wt/FontSupport.h"
#include "WebUtils.h"
#include "UriUtils.h"

#include <magick/api.h>

#include <boost/algorithm/string.hpp>

#include <cstdio>
#include <cmath>

#if MagickLibVersion < 0x020002
#error GraphicsMagick version must be at least 1.3.0
#error You should upgrade GraphicsMagick or disable WRasterImage with -DENABLE_GM=OFF
#endif

#define MAGICK_IMPLEMENTATION
#define MagickEpsilon 1E-5
#include "alpha_composite.h"
#undef MAGICK_IMPLEMENTATION
#undef MagickEpsilon

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#if defined(_MSC_VER) && (_MSC_VER < 1800)
namespace {
  double round(double x)
  {
    return floor(x + 0.5);
  }
}
#endif

#include <algorithm>
#include <utility>

namespace {
  static const double EPSILON = 1E-5;

  double adjust360(double d) {
    if (d > 360.0)
      return 360.0;
    else if (d < -360.0)
      return -360.0;
    else
      return d;
  }

  bool fequal(double d1, double d2) {
    return std::fabs(d1 - d2) < 1E-5;
  }

  void WColorToPixelPacket(const Wt::WColor& color, PixelPacket *pp)
  {
    pp->red = ScaleCharToQuantum(color.red());
    pp->green = ScaleCharToQuantum(color.green());
    pp->blue = ScaleCharToQuantum(color.blue());
    pp->opacity = ScaleCharToQuantum(255 - color.alpha());
  }

  bool isTranslation(const Wt::WTransform& t)
  {
    return std::fabs(t.m11() - 1.0) < EPSILON
      && std::fabs(t.m12() - 0.0) < EPSILON
      && std::fabs(t.m21() - 0.0) < EPSILON
      && std::fabs(t.m22() - 1.0) < EPSILON;
  }
}

namespace Wt {

LOGGER("WRasterImage");

class WRasterImage::Impl {
public:
  std::string type_;
  FontSupport *fontSupport_;
  bool renderText_;

  unsigned long w_, h_;
  unsigned char *pixels_;
  DrawContext context_;
  Image *image_;
  double fontSize_;

  typedef std::list< std::pair<WPainterPath, int> > PathCache;
  PathCache clipPathCache_;
  int currentClipPath_;
  int currentClipPathRendered_;
  WRasterImage *rasterImage_;

  // Required for WResource implementation
  std::size_t dataSize_;
  std::shared_ptr<char> data_;
#ifdef WT_THREADED
  std::mutex dataMutex_;
#endif // WT_THREADED

  void internalInit(bool applyChanges = true);
  void internalDone();
  void paintPath();
  void drawPlainPath(const WPainterPath& path);
  void setTransform(const WTransform& t);
  void applyTransform(const WTransform& f);
  std::string currentClipPathName() const;

};

WRasterImage::WRasterImage(const std::string& type,
                           const WLength& width, const WLength& height)
  : width_(width),
    height_(height),
    painter_(nullptr),
    impl_(new Impl)
{
  InitializeMagick(nullptr);

  impl_->rasterImage_ = this;
  impl_->type_ = type;
  impl_->currentClipPath_ = -1;
  impl_->currentClipPathRendered_ = -1;

  impl_->w_ = static_cast<unsigned long>(width.toPixels());
  impl_->h_ = static_cast<unsigned long>(height.toPixels());

  impl_->context_ = nullptr;
  impl_->fontSupport_ = nullptr;

  if (!impl_->w_ || !impl_->h_) {
    impl_->pixels_ = nullptr;
    impl_->image_ = nullptr;
    return;
  }

  unsigned long bufSize = 4 * impl_->w_ * impl_->h_;
  impl_->pixels_ = new unsigned char[bufSize];
  for (unsigned i = 0; i < impl_->w_ * impl_->h_; ++i) {
    impl_->pixels_[i*4] = impl_->pixels_[i*4 + 1] = impl_->pixels_[i*4 + 2] = 254;
    impl_->pixels_[i*4 + 3] = 0;
  }

  impl_->dataSize_ = 0;

  ExceptionInfo exception;
  GetExceptionInfo(&exception);
  impl_->image_ = ConstituteImage(impl_->w_, impl_->h_, "RGBA", CharPixel,
                                  impl_->pixels_, &exception);
  DestroyExceptionInfo(&exception);

  SetImageType(impl_->image_, TrueColorMatteType);
  SetImageOpacity(impl_->image_, TransparentOpacity);

  std::string magick = type;
  std::transform(magick.begin(), magick.end(), magick.begin(), toupper);
  strcpy(impl_->image_->magick, magick.c_str());
}

void WRasterImage::clear()
{
  if (!impl_->image_)
    return;

  PixelPacket *pixel = SetImagePixels(impl_->image_, 0, 0, impl_->w_, impl_->h_);
  for (unsigned i = 0; i < impl_->w_ * impl_->h_; ++i)
    WColorToPixelPacket(WColor(0, 0, 0, 1), pixel + i);
  SyncImagePixels(impl_->image_);
}

WRasterImage::~WRasterImage()
{
  beingDeleted();

  if (impl_->image_) {
    DestroyImage(impl_->image_);
    delete[] impl_->pixels_;
  }

  delete impl_->fontSupport_;
  // DestroyMagick(); apparently should be called only once ?
}

void WRasterImage::addFontCollection(const std::string& directory,
                                     bool recursive)
{
  impl_->fontSupport_->addFontCollection(directory, recursive);
}

WFlags<PaintDeviceFeatureFlag> WRasterImage::features() const
{
  if (impl_->fontSupport_->canRender())
    return PaintDeviceFeatureFlag::FontMetrics;
  else
    return None;
}

void WRasterImage::init()
{
  if (!impl_->w_ || !impl_->h_)
    throw WException("Raster image should have non-0 width and height");

  impl_->fontSupport_ = new FontSupport(this);

  /* If the font support can actually render fonts to bitmaps, we use that */
  if (impl_->fontSupport_->canRender())
    impl_->fontSupport_->setDevice(nullptr);

  impl_->internalInit(true);
}

void WRasterImage::Impl::internalInit(bool applyChanges)
{
  if (!context_) {
    currentClipPathRendered_ = -1;
    SetImageClipMask(image_, nullptr);

    context_ = DrawAllocateContext(nullptr, image_);

    DrawPushGraphicContext(context_);

    DrawSetFillRule(context_, NonZeroRule);
    DrawSetTextEncoding(context_, "UTF-8");

    DrawPushGraphicContext(context_); // for painter->clipping();
    DrawPushGraphicContext(context_); // for painter->combinedTransform()

    if (applyChanges)
      rasterImage_->setChanged(PainterChangeFlag::Clipping |
                               PainterChangeFlag::Transform |
                               PainterChangeFlag::Pen |
                               PainterChangeFlag::Brush |
                               PainterChangeFlag::Font |
                               PainterChangeFlag::Hints);
  }
}

void WRasterImage::done()
{
  impl_->internalDone();

  delete impl_->fontSupport_;
  impl_->fontSupport_ = nullptr;

  if (impl_->image_) {
    ImageInfo info;
    GetImageInfo(&info);

    ExceptionInfo exception;
    GetExceptionInfo(&exception);

    std::size_t size;
    char* data = static_cast<char*>(ImageToBlob(&info, impl_->image_, &size, &exception));

    if (!data) {
      DestroyExceptionInfo(&exception);
      throw WException("WRasterImage::done() image could not be "
                       "converted to blob - is your image type supported "
                       "by GraphicsMagick?");
    }

    {
#ifdef WT_THREADED
      std::lock_guard<std::mutex> lock(impl_->dataMutex_);
#endif // WT_THREADED
      impl_->data_.reset(data, &free);
      impl_->dataSize_ = size;
    }

    DestroyExceptionInfo(&exception);

    WResource::setChanged();
  }
}

void WRasterImage::Impl::internalDone()
{
  if (context_) {
    DrawPopGraphicContext(context_); // for painter->combinedTransform()
    DrawPopGraphicContext(context_); // for painter->clipping();
    DrawPopGraphicContext(context_);

    DrawRender(context_);

    DrawDestroyContext(context_);

    context_ = nullptr;

    SetImageClipMask(image_, nullptr);
    currentClipPathRendered_ = -1;
  }
}

void WRasterImage::handleRequest(WT_MAYBE_UNUSED const Http::Request& request,
                                 Http::Response &response)
{
  std::size_t size = 0;
  std::shared_ptr<char> data;
  {
#ifdef WT_THREADED
    std::lock_guard<std::mutex> lock(impl_->dataMutex_);
#endif // WT_THREADED

    data = impl_->data_;
    size = impl_->dataSize_;
  }

  if (data) {
    response.setMimeType("image/" + impl_->type_);
    response.out().write(data.get(), size);
  } else {
    response.setStatus(500);
  }
}

void WRasterImage::Impl::applyTransform(const WTransform& t)
{
  AffineMatrix matrix;
  matrix.sx = t.m11();
  matrix.rx = t.m12();
  matrix.ry = t.m21();
  matrix.sy = t.m22();
  matrix.tx = t.dx();
  matrix.ty = t.dy();

  DrawAffine(context_, &matrix);
}

void WRasterImage::Impl::setTransform(const WTransform& t)
{
  DrawPopGraphicContext(context_);
  DrawPushGraphicContext(context_);

  applyTransform(t);
}

void WRasterImage::setChanged(WFlags<PainterChangeFlag> flags)
{
  /*
   * If it is only Clipping that changes, we may not need to manipulate
   * the context_, since some clipping paths are cached.
   */
  if (flags != PainterChangeFlag::Clipping)
    impl_->internalInit();

  if (flags.test(PainterChangeFlag::Clipping)) {
    if (painter()->hasClipping()) {
      if (impl_->clipPathCache_.empty())
        impl_->clipPathCache_.resize(3); // keep 3

      int index = -1;
      int nextIndex = 0;

      for (Impl::PathCache::iterator i = impl_->clipPathCache_.begin();
           i != impl_->clipPathCache_.end(); ++i) {
        if (i->first == painter()->clipPath()) {
          index = i->second;
          impl_->clipPathCache_.splice(impl_->clipPathCache_.begin(),
                                       impl_->clipPathCache_, i);
          break;
        } else {
          nextIndex = std::max(i->second, nextIndex);
        }
      }

      if (index == -1) {
        impl_->internalInit(false);

        DrawPopGraphicContext(impl_->context_);
        DrawPopGraphicContext(impl_->context_);
        DrawPushGraphicContext(impl_->context_);

        index = nextIndex + 1;
        DrawPushClipPath
          (impl_->context_,("clip" + std::to_string(index)).c_str());
        drawPath(painter()->clipPath());
        DrawPopClipPath(impl_->context_);

        impl_->clipPathCache_.pop_back(); // implement LRU
        impl_->clipPathCache_.push_front(std::make_pair(painter()->clipPath(),
                                                        index));
      } else if (impl_->context_) {
        DrawPopGraphicContext(impl_->context_);
        DrawPopGraphicContext(impl_->context_);
        DrawPushGraphicContext(impl_->context_);
      }

      impl_->currentClipPath_ = index;

      if (impl_->context_) {
        const WTransform& t = painter()->clipPathTransform();

        impl_->applyTransform(t);

        DrawSetClipUnits(impl_->context_, UserSpaceOnUse);
        DrawSetClipPath(impl_->context_, impl_->currentClipPathName().c_str());

        impl_->applyTransform(t.inverted());
      }
    } else {
      impl_->currentClipPath_ = -1;

      if (impl_->context_) {
        DrawPopGraphicContext(impl_->context_);
        DrawPopGraphicContext(impl_->context_);
        DrawPushGraphicContext(impl_->context_);
      }
    }

    if (impl_->context_) {
      DrawPushGraphicContext(impl_->context_);
      flags = PainterChangeFlag::Transform;
    }
  }

  if (flags != PainterChangeFlag::Clipping)
    impl_->internalInit();

  if (flags.test(PainterChangeFlag::Transform)) {
    impl_->setTransform(painter()->combinedTransform());
    flags = PainterChangeFlag::Pen | PainterChangeFlag::Brush |
      PainterChangeFlag::Font | PainterChangeFlag::Hints;
  }

  if (flags.test(PainterChangeFlag::Hints)) {
    if (!(painter()->renderHints() & RenderHint::Antialiasing))
      DrawSetStrokeAntialias(impl_->context_, 0);
    else
      DrawSetStrokeAntialias(impl_->context_, 1);
  }

  if (flags.test(PainterChangeFlag::Pen)) {
    const WPen& pen = painter()->pen();

    if (pen.style() != PenStyle::None) {
      const WColor& color = pen.color();

      PixelPacket pp;
      WColorToPixelPacket(color, &pp);
      pp.opacity = 0;
      DrawSetStrokeColor(impl_->context_, &pp);
      DrawSetStrokeOpacity(impl_->context_, color.alpha() / 255.0);

      WLength w = pen.width();
      w = painter()->normalizedPenWidth(w, w.value() == 0);
      DrawSetStrokeWidth(impl_->context_, w.toPixels());

      switch (pen.capStyle()) {
      case PenCapStyle::Flat:
        DrawSetStrokeLineCap(impl_->context_, ::ButtCap);
        break;
      case PenCapStyle::Square:
        DrawSetStrokeLineCap(impl_->context_, ::SquareCap);
        break;
      case PenCapStyle::Round:
        DrawSetStrokeLineCap(impl_->context_, ::RoundCap);
        break;
      }

      /*
      switch (pen.joinStyle()) {
      case PenJoinStyle::Miter:
        DrawSetStrokeLineJoin(context_, ::PenJoinStyle::Miter);
        DrawSetStrokeMiterLimit(context_, 3);
        break;
      case PenJoinStyle::Bevel:
        DrawSetStrokeLineJoin(context_, ::PenJoinStyle::Bevel);
        break;
      case PenJoinStyle::Round:
        DrawSetStrokeLineJoin(context_, ::PenJoinStyle::Round);
        break;
      }
      */

      switch (pen.style()) {
      case PenStyle::None:
        break;
      case PenStyle::SolidLine:
        DrawSetStrokeDashArray(impl_->context_, 0, nullptr);
        break;
      case PenStyle::DashLine: {
        const double dasharray[] = { 4, 2 };
        DrawSetStrokeDashArray(impl_->context_, 2, dasharray);
        break;
      }
      case PenStyle::DotLine: {
        const double dasharray[] = { 1, 2 };
        DrawSetStrokeDashArray(impl_->context_, 2, dasharray);
        break;
      }
      case PenStyle::DashDotLine: {
        const double dasharray[] = { 4, 2, 1, 2 };
        DrawSetStrokeDashArray(impl_->context_, 4, dasharray);
        break;
      }
      case PenStyle::DashDotDotLine: {
        const double dasharray[] = { 4, 2, 1, 2, 1, 2 };
        DrawSetStrokeDashArray(impl_->context_, 6, dasharray);
        break;
      }
      }

    } else {
      DrawSetStrokeWidth(impl_->context_, 0);
      DrawSetStrokeOpacity(impl_->context_, 0);
    }
  }

  if (flags.test(PainterChangeFlag::Brush)) {
    const WBrush& brush = painter()->brush();

    if (brush.style() != BrushStyle::None) {
      const WColor& color = painter()->brush().color();
      PixelPacket pp;
      WColorToPixelPacket(color, &pp);
      pp.opacity = 0;
      DrawSetFillColor(impl_->context_, &pp);
      DrawSetFillOpacity(impl_->context_, color.alpha() / 255.0);
    } else
      DrawSetFillOpacity(impl_->context_, 0);
  }

  if (flags.test(PainterChangeFlag::Font)) {
    const WFont& font = painter()->font();

    std::string name;

    /*
     * First, consult the font support to locate a suitable font
     */
    if (impl_->fontSupport_->busy()) {
      /*
       * We have a resolved true type font.
       */
      name = impl_->fontSupport_->drawingFontPath();
    } else if (font.genericFamily() != FontFamily::Default) {
      FontSupport::FontMatch match = impl_->fontSupport_->matchFont(font);

      if (match.matched())
        name = match.fileName();
    }

    if (name.empty() || !impl_->fontSupport_->canRender()) {
      if (name.empty()) {
        /*
         * FIXME, does this actually make any sense ?
         */
        const char *base;
        const char *italic = nullptr;
        const char *bold = nullptr;

        switch (font.genericFamily()) {
        case FontFamily::Default:
        case FontFamily::Serif:
          base = "Times";
          italic = "Italic";
          bold = "Bold";
          break;
        case FontFamily::SansSerif:
          base = "Helvetica";
          italic = "Oblique";
          bold = "Bold";
          break;
        case FontFamily::Monospace:
          base = "Courier";
          italic = "Oblique";
          bold = "Bold";
          break;
        case FontFamily::Fantasy: // Not really !
          base = "Symbol";
          break;
        case FontFamily::Cursive: // Not really !
          base = "ZapfDingbats";
        }

        if (italic)
          switch (font.style()) {
          case FontStyle::Normal:
            italic = nullptr;
            break;
          default:
            break;
          }

        if (bold)
          switch (font.weight()) {
          case FontWeight::Normal:
            bold = nullptr;
            break;
          case FontWeight::Bold:
          case FontWeight::Bolder:
            break;
          default:
            bold = nullptr;
          }

        std::string name = base;
        if (bold) {
          name += std::string("-") + bold;
          if (italic)
            name += italic;
        } else if (italic)
          name += std::string("-") + italic;

        if (name == "Times")
          name = "Times-Roman";
      }

      DrawSetFont(impl_->context_, name.c_str());

      impl_->fontSize_ = font.sizeLength(12).toPixels();

      DrawSetFontSize(impl_->context_, impl_->fontSize_);

      impl_->renderText_ = true;
    } else
      impl_->renderText_ = false;
  }
}

std::string WRasterImage::Impl::currentClipPathName() const
{
  return "clip" + std::to_string(currentClipPath_);
}

void WRasterImage::drawArc(const WRectF& rect,
                           double startAngle, double spanAngle)
{
  impl_->internalInit();

  double start = - startAngle;
  double end = - startAngle - spanAngle;

  if (end < start)
    std::swap(start, end);

  DrawArc(impl_->context_, rect.left(), rect.top(), rect.right(), rect.bottom(),
          start, end);
}

void WRasterImage::drawImage(const WRectF& rect, const std::string& imgUri,
                             WT_MAYBE_UNUSED int imgWidth, WT_MAYBE_UNUSED int imgHeight,
                             const WRectF& srect)
{
  ImageInfo info;
  GetImageInfo(&info);

  ExceptionInfo exception;
  GetExceptionInfo(&exception);

  Image *cImage;
  if (DataUri::isDataUri(imgUri)) {
    DataUri uri(imgUri);

    if (boost::istarts_with(uri.mimeType, "image/png"))
      strcpy(info.magick, "PNG");
    else if (boost::istarts_with(uri.mimeType, "image/gif"))
      strcpy(info.magick, "GIF");
    else if (boost::istarts_with(uri.mimeType, "image/jpg")
             || boost::istarts_with(uri.mimeType, "image/jpeg"))
      strcpy(info.magick, "JPG");
    else
      throw WException("Unsupported image mimetype: " + uri.mimeType);

    cImage = ReadInlineImage(&info, imgUri.substr(imgUri.find(',') + 1).c_str(), &exception);
  } else {
    strncpy(info.filename, imgUri.c_str(), 2048);
    cImage = ReadImage(&info, &exception);

    // Temporary fix #13366, see also #13367.
    if (cImage == nullptr) {
      cpp17::filesystem::path imagePath = cpp17::filesystem::path(imgUri);
      // If this is a relative path, this can potentially be the fallback for the "normal" draw in JS, using
      // gfxutils, with the file being part of the docroot.
      if (imagePath.is_relative()) {
        ExceptionInfo docrootCatchException;
        GetExceptionInfo(&docrootCatchException);
        imagePath = cpp17::filesystem::path(Wt::WApplication::instance()->docRoot()) / imagePath;
        LOG_WARN("drawImage failed on " << imgUri
                  << " attempt to prefix docroot: " << imagePath.string());
        strncpy(info.filename, imagePath.c_str(), 2048);
        cImage = ReadImage(&info, &docrootCatchException);

        if (cImage == nullptr) {
          LOG_ERROR("drawImage (and its fallback) failed - original: "
                    << (exception.reason ? exception.reason :
                        "(unknown reason)") << ", "
                    << (exception.description ? exception.description :
                        "(unknown description)")
                    << " - fallback: "
                    << (docrootCatchException.reason ? docrootCatchException.reason :
                        "(unknown reason)") << ", "
                    << (docrootCatchException.description ? docrootCatchException.description :
                        "(unknown description)"));
          DestroyExceptionInfo(&exception);
          DestroyExceptionInfo(&docrootCatchException);
          return;
        }
        DestroyExceptionInfo(&docrootCatchException);
      }
    }
  }

  if (cImage == nullptr) {
    LOG_ERROR("drawImage failed: "
              << (exception.reason ? exception.reason :
                  "(unknown reason)") << ", "
              << (exception.description ? exception.description :
                  "(unknown description)") );
    DestroyExceptionInfo(&exception);
    return;
  }

  RectangleInfo tocrop;
  tocrop.width = srect.width();
  tocrop.height = srect.height();
  tocrop.x = srect.x();
  tocrop.y = srect.y();
  Image *croppedImage = CropImage(cImage, &tocrop, &exception);
  DestroyExceptionInfo(&exception);

  const WTransform& t = painter()->combinedTransform();

  bool directComposite = false;
  if (isTranslation(t)
      && rect.width() == srect.width()
      && rect.height() == srect.height())
    directComposite = true;

  if (!directComposite) {
    impl_->internalInit();
    DrawComposite(impl_->context_, OverCompositeOp, rect.x(), rect.y(),
                  rect.width(), rect.height(), croppedImage);
  } else {
    impl_->internalDone();

    CompositeImage(impl_->image_, OverCompositeOp, croppedImage,
                   rect.x() + t.dx(), rect.y() + t.dy());
  }

  DestroyImage(croppedImage);
  DestroyImage(cImage);
}

void WRasterImage::drawLine(double x1, double y1, double x2, double y2)
{
  impl_->internalInit();

  DrawLine(impl_->context_, x1-0.5, y1-0.5, x2-0.5, y2-0.5);
}

void WRasterImage::drawRect(const WRectF& rectangle)
{
  drawPath(rectangle.toPath());
}

void WRasterImage::drawPath(const WPainterPath& path)
{
  impl_->internalInit();

  if (!path.isEmpty()) {
    DrawPathStart(impl_->context_);
    impl_->drawPlainPath(path);
    DrawPathFinish(impl_->context_);
  }
}

void WRasterImage::setPixel(int x, int y, const WColor& c)
{
  if (painter_)
    throw WException("WRasterImage::setPixel(): cannot be used while a "
                     "painter is active");

  PixelPacket *pixel = SetImagePixels(impl_->image_, x, y, 1, 1);
  WColorToPixelPacket(c, pixel);
  SyncImagePixels(impl_->image_);
}

void WRasterImage::getPixels(void *data)
{
  int w = (int)width_.value();
  int h = (int)height_.value();
  unsigned char *d = (unsigned char *)data;
  ExceptionInfo ei;
  GetExceptionInfo(&ei);
  const PixelPacket *pixel = AcquireImagePixels(impl_->image_,
    0, 0, w, h, &ei);
  if (pixel) {
    int i = 0;
    for (int r = 0; r < h; r++)
      for (int c = 0; c < w; c++) {
        d[i++] = ScaleQuantumToChar(pixel->red);
        d[i++] = ScaleQuantumToChar(pixel->green);
        d[i++] = ScaleQuantumToChar(pixel->blue);
        d[i++] = ScaleQuantumToChar(TransparentOpacity - pixel->opacity);
        pixel++;
      }
  } else {
    std::string desc = std::string("WRasterImage::getPixels(): error: ") + ei.description;
    DestroyExceptionInfo(&ei);
    throw WException(desc);
  }
  DestroyExceptionInfo(&ei);
}

WColor WRasterImage::getPixel(int x, int y)
{
  ExceptionInfo exception;
  GetExceptionInfo(&exception);
  PixelPacket pixel = AcquireOnePixel(impl_->image_, x, y, &exception);

  if (exception.description == nullptr) {
    return WColor(ScaleQuantumToChar(pixel.red),
                  ScaleQuantumToChar(pixel.green),
                  ScaleQuantumToChar(pixel.blue),
                  ScaleQuantumToChar(TransparentOpacity - pixel.opacity));
  } else {
    std::string desc = std::string("WRasterImage::getPixel(): error: ") + exception.description;
    DestroyExceptionInfo(&exception);
    throw WException(desc);
  }
  DestroyExceptionInfo(&exception);
}

void WRasterImage::Impl::drawPlainPath(const WPainterPath& path)
{
  internalInit();

  const std::vector<WPainterPath::Segment>& segments = path.segments();

  if (segments.size() > 0
      && segments[0].type() != SegmentType::MoveTo)
    DrawPathMoveToAbsolute(context_, -0.5, -0.5);

  for (unsigned i = 0; i < segments.size(); ++i) {
    const WPainterPath::Segment s = segments[i];

    switch (s.type()) {
    case SegmentType::MoveTo:
      DrawPathMoveToAbsolute(context_, s.x() - 0.5, s.y() - 0.5);
      break;
    case SegmentType::LineTo:
      DrawPathLineToAbsolute(context_, s.x() - 0.5, s.y() - 0.5);
      break;
    case SegmentType::CubicC1: {
      const double x1 = s.x();
      const double y1 = s.y();
      const double x2 = segments[i+1].x();
      const double y2 = segments[i+1].y();
      const double x3 = segments[i+2].x();
      const double y3 = segments[i+2].y();

      DrawPathCurveToAbsolute(context_, x1 - 0.5, y1 - 0.5,
                              x2 - 0.5, y2 - 0.5,
                              x3 - 0.5, y3 - 0.5);

      i += 2;
      break;
    }
    case SegmentType::CubicC2:
    case SegmentType::CubicEnd:
      assert(false);
    case SegmentType::ArcC: {
      WPointF current = path.positionAtSegment(i);
      // See also WSvgImage arc drawing

      const double cx = s.x();
      const double cy = s.y();
      const double rx = segments[i+1].x();
      const double ry = segments[i+1].y();
      const double theta1 = -WTransform::degreesToRadians(segments[i+2].x());
      const double deltaTheta
        = -WTransform::degreesToRadians(adjust360(segments[i+2].y()));

      const double x1 = rx * std::cos(theta1) + cx;
      const double y1 = ry * std::sin(theta1) + cy;
      const double x2 = rx * std::cos(theta1 + deltaTheta / 2.0) + cx;
      const double y2 = ry * std::sin(theta1 + deltaTheta / 2.0) + cy;
      const double x3 = rx * std::cos(theta1 + deltaTheta) + cx;
      const double y3 = ry * std::sin(theta1 + deltaTheta) + cy;
      const int fa = 0;
      const unsigned int fs = (deltaTheta > 0 ? 1 : 0);

      if (!fequal(current.x(), x1) || !fequal(current.y(), y1))
        DrawPathLineToAbsolute(context_, x1 - 0.5, y1 - 0.5);

      DrawPathEllipticArcAbsolute(context_, rx, ry, 0, fa, fs,
                                  x2 - 0.5, y2 - 0.5);
      DrawPathEllipticArcAbsolute(context_, rx, ry, 0, fa, fs,
                                  x3 - 0.5, y3 - 0.5);

      i += 2;
      break;
    }
    case SegmentType::ArcR:
    case SegmentType::ArcAngleSweep:
      assert(false);
    case SegmentType::QuadC: {
      const double x1 = s.x();
      const double y1 = s.y();
      const double x2 = segments[i+1].x();
      const double y2 = segments[i+1].y();

      DrawPathCurveToQuadraticBezierAbsolute(context_, x1 - 0.5, y1 - 0.5,
                                             x2 - 0.5, y2 - 0.5);

      i += 1;

      break;
    }
    case SegmentType::QuadEnd:
      assert(false);
    }
  }
}

void WRasterImage::drawText(const WRectF& rect,
                            WFlags<AlignmentFlag> flags,
                            TextFlag textFlag,
                            const WString& text,
                            const WPointF *clipPoint)
{
  if (textFlag == TextFlag::WordWrap)
    throw WException("WRasterImage::drawText() TextFlag::WordWrap is not supported");

  if (clipPoint && painter() && !painter()->clipPath().isEmpty()) {
    if (!painter()->clipPathTransform().map(painter()->clipPath())
          .isPointInPath(painter()->worldTransform().map(*clipPoint)))
      return;
  }

  if (impl_->renderText_) {
    impl_->internalInit();

    AlignmentFlag horizontalAlign = flags & AlignHorizontalMask;
    AlignmentFlag verticalAlign = flags & AlignVerticalMask;

    /*
     * For centering horizontally, we use graphicsmagick totally insane
     * gravity system.
     */
    GravityType gravity = NorthWestGravity;

    const WTransform& t = painter()->combinedTransform();

    WPointF p;

    double ascent = 0.8 * impl_->fontSize_;
    double descent = impl_->fontSize_ - ascent;

    switch (verticalAlign) {
    case AlignmentFlag::Top:
      p = rect.topLeft();
      p.setY(p.y() + ascent);
      break;
    case AlignmentFlag::Middle:
      p = rect.center();
      p.setY(p.y() + ascent - impl_->fontSize_/2);
      break;
    case AlignmentFlag::Bottom:
      p = rect.bottomLeft();
      p.setY(p.y() - descent);
      break;
    default:
      break;
    }

    switch (horizontalAlign) {
    case AlignmentFlag::Left:
      gravity = NorthWestGravity;
      p.setX(rect.left());
      break;
    case AlignmentFlag::Center:
      gravity = NorthGravity;
      p.setX(rect.center().x());

      p = t.map(p);
      p.setX(p.x() - impl_->w_/2);
      p = t.inverted().map(p);
      break;
    case AlignmentFlag::Right:
      gravity = NorthEastGravity;
      p.setX(rect.right());

      p = t.map(p);
      p.setX(impl_->w_ - p.x());
      p = t.inverted().map(p);
      break;
    default:
      break;
    }

    DrawPushGraphicContext(impl_->context_);

    PixelPacket pp;
    WColorToPixelPacket(painter()->pen().color(), &pp);
    pp.opacity = 0;
    DrawSetFillColor(impl_->context_, &pp);
    DrawSetFillOpacity(impl_->context_, painter()->pen().color().alpha() / 255.0);
    DrawSetStrokeOpacity(impl_->context_, 0);

    DrawSetGravity(impl_->context_, gravity);

    std::string utf8 = text.toUTF8();
    Utils::replace(utf8, '%', "%%");

    DrawAnnotation(impl_->context_, p.x(), p.y(), (const unsigned char *)utf8.c_str());

    DrawPopGraphicContext(impl_->context_);

    setChanged(PainterChangeFlag::Transform);
  } else {
    WTransform t = painter()->combinedTransform();

    if (painter()->hasClipping())
      setChanged(PainterChangeFlag::Clipping);

    impl_->internalDone();

    if (impl_->currentClipPath_ != impl_->currentClipPathRendered_) {
      if (impl_->currentClipPath_ != -1) {
        ImageInfo info;
        GetImageInfo(&info);
        DrawInfo *drawInfo = (DrawInfo *)malloc(sizeof(DrawInfo));
        GetDrawInfo(&info, drawInfo);

        AffineMatrix& m = drawInfo->affine;
        m.sx = t.m11();
        m.rx = t.m12();
        m.ry = t.m21();
        m.sy = t.m22();
        m.tx = t.dx();
        m.ty = t.dy();

        drawInfo->clip_units = UserSpaceOnUse;
        DrawClipPath(impl_->image_, drawInfo, impl_->currentClipPathName().c_str());

        DestroyDrawInfo(drawInfo);
      } else {
        SetImageClipMask(impl_->image_, nullptr);
      }

      impl_->currentClipPathRendered_ = impl_->currentClipPath_;
    }

    WRectF renderRect;

    int w, h, x0, y0;
    if (isTranslation(t)) {
      // #8025: make bitmap bigger to avoid unexpected text clipping
      int dX = round(rect.width()/2);
      int dY = round(rect.height()/2);
      x0 = round(t.dx() + rect.x()) - dX;
      y0 = round(t.dy() + rect.y()) - dY;
      w = 2 * rect.width();
      h = 2 * rect.height();
      renderRect = WRectF(dX, dY, rect.width(), rect.height());
      t = WTransform();
    } else {
      x0 = 0;
      y0 = 0;
      w = impl_->w_;
      h = impl_->h_;
      renderRect = rect;
    }

    FontSupport::Bitmap bitmap(w, h);
    impl_->fontSupport_->drawText(painter_->font(), renderRect,
                                  t, bitmap, flags, text);

    PixelPacket *pixels = GetImagePixels(impl_->image_, 0, 0,
                                         impl_->w_, impl_->h_);

    WColor c = painter()->pen().color();
    PixelPacket pc;
    WColorToPixelPacket(c, &pc);

    for (int y = 0; y < h; ++y) {
      int iy = y0 + y;

      if (iy < 0 || iy >= (int)impl_->h_)
        continue;

      for (int x = 0; x < w; ++x) {
        int ix = x0 + x;

        if (ix < 0 || ix >= (int)impl_->w_)
          continue;

        PixelPacket *pixel = pixels + (iy * impl_->w_ + ix);

        unsigned char bit = bitmap.value(x, y);

        if (bit > 0) {
          double alpha = ScaleCharToQuantum(255 - bit) * (MaxRGBDouble - pc.opacity) / MaxRGBDouble;
          AlphaCompositePixel(pixel, &pc, alpha,
                              pixel, pixel->opacity);
        }
      }
    }

    SyncImagePixels(impl_->image_);
  }
}

WTextItem WRasterImage::measureText(const WString& text, double maxWidth,
                                    bool wordWrap)
{
  if (impl_->renderText_)
    throw WException("WRasterImage::measureText() not supported");
  else
    return impl_->fontSupport_->measureText(painter_->font(), text, maxWidth,
                                            wordWrap);
}

WFontMetrics WRasterImage::fontMetrics()
{
  if (impl_->renderText_)
    throw WException("WRasterImage::fontMetrics() not supported");
  else
    return impl_->fontSupport_->fontMetrics(painter_->font());
}

}
