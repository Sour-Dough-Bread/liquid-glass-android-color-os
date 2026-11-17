// Every Skia layout fact the engine depends on, as compile-time constants.
// See layout_check.sh. `private`/`protected` are redefined so offsetof can see
// the members; nothing here is ever linked or run.
#define private public
#define protected public
#include "include/core/SkCanvas.h"
#include "include/core/SkData.h"
#include "include/core/SkImage.h"
#include "include/core/SkM44.h"
#include "include/core/SkMatrix.h"
#include "include/core/SkPaint.h"
#include "include/core/SkRRect.h"
#include "include/core/SkSamplingOptions.h"
#include "include/core/SkString.h"
#include "include/effects/SkRuntimeEffect.h"
#include <cstddef>
#define E(name, expr) extern "C" const long name = (long)(expr);
E(SkPaint_size, sizeof(SkPaint))
E(SkPaint_align, alignof(SkPaint))
E(SkPaint_fShader, offsetof(SkPaint, fShader))
E(SkPaint_fColor4f, offsetof(SkPaint, fColor4f))
E(SkPaint_fWidth, offsetof(SkPaint, fWidth))
E(SkPaint_fBitfields, offsetof(SkPaint, fBitfields))
E(SkCanvas_fMCRec, offsetof(SkCanvas, fMCRec))
E(MCRec_fMatrix, offsetof(SkCanvas::MCRec, fMatrix))
E(SkM44_size, sizeof(SkM44))
E(SkMatrix_size, sizeof(SkMatrix))
E(SkRRect_size, sizeof(SkRRect))
E(SkRRect_fRadii, offsetof(SkRRect, fRadii))
E(SkData_size, sizeof(SkData))
#ifdef SKDATA_SPAN  // android17-release: {fPtr, fSize} became SkSpan fSpan
E(SkData_fPtr, offsetof(SkData, fSpan))
#else
E(SkData_fPtr, offsetof(SkData, fPtr))
#endif
E(SkString_size, sizeof(SkString))
E(Result_size, sizeof(SkRuntimeEffect::Result))
E(Result_errorText, offsetof(SkRuntimeEffect::Result, errorText))
E(Uniform_size, sizeof(SkRuntimeEffect::Uniform))
E(Uniform_offset, offsetof(SkRuntimeEffect::Uniform, offset))
E(Child_size, sizeof(SkRuntimeEffect::Child))
E(Child_type, offsetof(SkRuntimeEffect::Child, type))
E(Child_index, offsetof(SkRuntimeEffect::Child, index))
E(ChildPtr_size, sizeof(SkRuntimeEffect::ChildPtr))
E(Sampling_size, sizeof(SkSamplingOptions))
// Read inline by the engine to classify a region against the display and to
// recognise "the same layer" across calls: width()/height()/uniqueID().
E(SkImage_fInfo, offsetof(SkImage, fInfo))
E(SkImageInfo_fDimensions, offsetof(SkImageInfo, fDimensions))
E(SkImage_fUniqueID, offsetof(SkImage, fUniqueID))
E(SkRefCntBase_fRefCnt, offsetof(SkRefCntBase, fRefCnt))
