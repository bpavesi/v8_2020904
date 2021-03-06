// Copyright 2012 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/execution/runtime-profiler.h"

#include "src/base/platform/platform.h"
#include "src/codegen/assembler.h"
#include "src/codegen/compilation-cache.h"
#include "src/codegen/compiler.h"
#include "src/codegen/pending-optimization-table.h"
#include "src/diagnostics/code-tracer.h"
#include "src/execution/execution.h"
#include "src/execution/frames-inl.h"
#include "src/handles/global-handles.h"
#include "src/init/bootstrapper.h"
#include "src/interpreter/interpreter.h"
#include "src/tracing/trace-event.h"

namespace v8 {
namespace internal {

// Number of times a function has to be seen on the stack before it is
// optimized.
static const int kProfilerTicksBeforeOptimization = 2;

// The number of ticks required for optimizing a function increases with
// the size of the bytecode. This is in addition to the
// kProfilerTicksBeforeOptimization required for any function.
static const int kBytecodeSizeAllowancePerTick = 1200;

// Maximum size in bytes of generate code for a function to allow OSR.
static const int kOSRBytecodeSizeAllowanceBase = 180;

static const int kOSRBytecodeSizeAllowancePerTick = 48;

// Maximum size in bytes of generated code for a function to be optimized
// the very first time it is seen on the stack.
static const int kMaxBytecodeSizeForEarlyOpt = 90;

// Number of times a function has to be seen on the stack before it is
// OSRed in TurboProp
// This value is chosen so TurboProp OSRs at similar time as TurboFan. The
// current interrupt budger of TurboFan is approximately 10 times that of
// TurboProp and we wait for 3 ticks (2 for marking for optimization and an
// additional tick to mark it for OSR) and hence this is set to 3 * 10.
static const int kProfilerTicksForTurboPropOSR = 3 * 10;

#define OPTIMIZATION_REASON_LIST(V)   \
  V(DoNotOptimize, "do not optimize") \
  V(HotAndStable, "hot and stable")   \
  V(SmallFunction, "small function")

enum class OptimizationReason : uint8_t {
#define OPTIMIZATION_REASON_CONSTANTS(Constant, message) k##Constant,
  OPTIMIZATION_REASON_LIST(OPTIMIZATION_REASON_CONSTANTS)
#undef OPTIMIZATION_REASON_CONSTANTS
};

char const* OptimizationReasonToString(OptimizationReason reason) {
  static char const* reasons[] = {
#define OPTIMIZATION_REASON_TEXTS(Constant, message) message,
      OPTIMIZATION_REASON_LIST(OPTIMIZATION_REASON_TEXTS)
#undef OPTIMIZATION_REASON_TEXTS
  };
  size_t const index = static_cast<size_t>(reason);
  DCHECK_LT(index, arraysize(reasons));
  return reasons[index];
}

#undef OPTIMIZATION_REASON_LIST

std::ostream& operator<<(std::ostream& os, OptimizationReason reason) {
  return os << OptimizationReasonToString(reason);
}

namespace {

void TraceInOptimizationQueue(JSFunction function) {
  if (FLAG_trace_opt_verbose) {
    PrintF("[function ");
    function.PrintName();
    PrintF(" is already in optimization queue]\n");
  }
}

void TraceHeuristicOptimizationDisallowed(JSFunction function) {
  if (FLAG_trace_opt_verbose) {
    PrintF("[function ");
    function.PrintName();
    PrintF(" has been marked manually for optimization]\n");
  }
}

void TraceRecompile(JSFunction function, OptimizationReason reason,
                    Isolate* isolate) {
  if (FLAG_trace_opt) {
    CodeTracer::Scope scope(isolate->GetCodeTracer());
    PrintF(scope.file(), "[marking ");
    function.ShortPrint(scope.file());
    PrintF(scope.file(), " for optimized recompilation, reason: %s",
           OptimizationReasonToString(reason));
    PrintF(scope.file(), "]\n");
  }
}

void TraceNCIRecompile(JSFunction function, OptimizationReason reason) {
  if (FLAG_trace_turbo_nci) {
    StdoutStream os;
    os << "NCI tierup mark: " << Brief(function) << ", "
       << OptimizationReasonToString(reason) << std::endl;
  }
}

}  // namespace

RuntimeProfiler::RuntimeProfiler(Isolate* isolate)
    : isolate_(isolate), any_ic_changed_(false) {}

void RuntimeProfiler::Optimize(JSFunction function, OptimizationReason reason) {
  DCHECK_NE(reason, OptimizationReason::kDoNotOptimize);
  TraceRecompile(function, reason, isolate_);
  function.MarkForOptimization(ConcurrencyMode::kConcurrent);
}

void RuntimeProfiler::AttemptOnStackReplacement(InterpretedFrame* frame,
                                                int loop_nesting_levels) {
  JSFunction function = frame->function();
  SharedFunctionInfo shared = function.shared();
  if (!FLAG_use_osr || !shared.IsUserJavaScript()) {
    return;
  }

  // If the code is not optimizable, don't try OSR.
  if (shared.optimization_disabled()) return;

  // We're using on-stack replacement: Store new loop nesting level in
  // BytecodeArray header so that certain back edges in any interpreter frame
  // for this bytecode will trigger on-stack replacement for that frame.
  if (FLAG_trace_osr) {
    CodeTracer::Scope scope(isolate_->GetCodeTracer());
    PrintF(scope.file(), "[OSR - arming back edges in ");
    function.PrintName(scope.file());
    PrintF(scope.file(), "]\n");
  }

  DCHECK_EQ(StackFrame::INTERPRETED, frame->type());
  int level = frame->GetBytecodeArray().osr_loop_nesting_level();
  frame->GetBytecodeArray().set_osr_loop_nesting_level(
      Min(level + loop_nesting_levels, AbstractCode::kMaxLoopNestingMarker));
}

void RuntimeProfiler::MaybeOptimizeInterpretedFrame(JSFunction function,
                                                    InterpretedFrame* frame) {
  if (function.IsInOptimizationQueue()) {
    TraceInOptimizationQueue(function);
    return;
  }
  if (FLAG_testing_d8_test_runner &&
      !PendingOptimizationTable::IsHeuristicOptimizationAllowed(isolate_,
                                                                function)) {
    TraceHeuristicOptimizationDisallowed(function);
    return;
  }

  if (function.shared().optimization_disabled()) return;

  if (FLAG_always_osr) {
    AttemptOnStackReplacement(frame, AbstractCode::kMaxLoopNestingMarker);
    // Fall through and do a normal optimized compile as well.
  } else if (MaybeOSR(function, frame)) {
    return;
  }

  OptimizationReason reason =
      ShouldOptimize(function, function.shared().GetBytecodeArray());

  if (reason != OptimizationReason::kDoNotOptimize) {
    Optimize(function, reason);
  }
}

void RuntimeProfiler::MaybeOptimizeNCIFrame(JSFunction function) {
  DCHECK_EQ(function.code().kind(), CodeKind::NATIVE_CONTEXT_INDEPENDENT);

  if (function.IsInOptimizationQueue()) {
    TraceInOptimizationQueue(function);
    return;
  }
  if (FLAG_testing_d8_test_runner &&
      !PendingOptimizationTable::IsHeuristicOptimizationAllowed(isolate_,
                                                                function)) {
    TraceHeuristicOptimizationDisallowed(function);
    return;
  }

  if (function.shared().optimization_disabled()) return;

  // Note: We currently do not trigger OSR compilation from NCI code.
  // TODO(jgruber,v8:8888): But we should.

  OptimizationReason reason =
      ShouldOptimize(function, function.shared().GetBytecodeArray());

  if (reason != OptimizationReason::kDoNotOptimize) {
    TraceNCIRecompile(function, reason);
    Optimize(function, reason);
  }
}

bool RuntimeProfiler::MaybeOSR(JSFunction function, InterpretedFrame* frame) {
  int ticks = function.feedback_vector().profiler_ticks();
  // TODO(rmcilroy): Also ensure we only OSR top-level code if it is smaller
  // than kMaxToplevelSourceSize.

  // Turboprop optimizes quite early. So don't attempt to OSR if the loop isn't
  // hot enough.
  if (FLAG_turboprop && ticks < kProfilerTicksForTurboPropOSR) {
    return false;
  }

  if (function.IsMarkedForOptimization() ||
      function.IsMarkedForConcurrentOptimization() ||
      function.HasAvailableOptimizedCode()) {
    // Attempt OSR if we are still running interpreted code even though the
    // the function has long been marked or even already been optimized.
    int64_t allowance =
        kOSRBytecodeSizeAllowanceBase +
        static_cast<int64_t>(ticks) * kOSRBytecodeSizeAllowancePerTick;
    if (function.shared().GetBytecodeArray().length() <= allowance) {
      AttemptOnStackReplacement(frame);
    }
    return true;
  }
  return false;
}

OptimizationReason RuntimeProfiler::ShouldOptimize(JSFunction function,
                                                   BytecodeArray bytecode) {
  if (function.ActiveTierIsTurbofan()) {
    return OptimizationReason::kDoNotOptimize;
  }
  int ticks = function.feedback_vector().profiler_ticks();
  int ticks_for_optimization =
      kProfilerTicksBeforeOptimization +
      (bytecode.length() / kBytecodeSizeAllowancePerTick);
  if (ticks >= ticks_for_optimization) {
    return OptimizationReason::kHotAndStable;
  } else if (!any_ic_changed_ &&
             bytecode.length() < kMaxBytecodeSizeForEarlyOpt) {
    // If no IC was patched since the last tick and this function is very
    // small, optimistically optimize it now.
    return OptimizationReason::kSmallFunction;
  } else if (FLAG_trace_opt_verbose) {
    PrintF("[not yet optimizing ");
    function.PrintName();
    PrintF(", not enough ticks: %d/%d and ", ticks,
           kProfilerTicksBeforeOptimization);
    if (any_ic_changed_) {
      PrintF("ICs changed]\n");
    } else {
      PrintF(" too large for small function optimization: %d/%d]\n",
             bytecode.length(), kMaxBytecodeSizeForEarlyOpt);
    }
  }
  return OptimizationReason::kDoNotOptimize;
}

RuntimeProfiler::MarkCandidatesForOptimizationScope::
    MarkCandidatesForOptimizationScope(RuntimeProfiler* profiler)
    : handle_scope_(profiler->isolate_), profiler_(profiler) {
  TRACE_EVENT0(TRACE_DISABLED_BY_DEFAULT("v8.compile"),
               "V8.MarkCandidatesForOptimization");
}

RuntimeProfiler::MarkCandidatesForOptimizationScope::
    ~MarkCandidatesForOptimizationScope() {
  profiler_->any_ic_changed_ = false;
}

void RuntimeProfiler::MarkCandidatesForOptimizationFromBytecode() {
  if (!isolate_->use_optimizer()) return;
  MarkCandidatesForOptimizationScope scope(this);
  int i = 0;
  for (JavaScriptFrameIterator it(isolate_); i < FLAG_frame_count && !it.done();
       i++, it.Advance()) {
    JavaScriptFrame* frame = it.frame();
    if (!frame->is_interpreted()) continue;

    JSFunction function = frame->function();
    DCHECK(function.shared().is_compiled());
    if (!function.shared().IsInterpreted()) continue;

    if (!function.has_feedback_vector()) continue;

    MaybeOptimizeInterpretedFrame(function, InterpretedFrame::cast(frame));

    // TODO(leszeks): Move this increment to before the maybe optimize checks,
    // and update the tests to assume the increment has already happened.
    function.feedback_vector().SaturatingIncrementProfilerTicks();
  }
}

void RuntimeProfiler::MarkCandidatesForOptimizationFromCode() {
  if (!isolate_->use_optimizer()) return;
  MarkCandidatesForOptimizationScope scope(this);
  int i = 0;
  for (JavaScriptFrameIterator it(isolate_); i < FLAG_frame_count && !it.done();
       i++, it.Advance()) {
    JavaScriptFrame* frame = it.frame();
    if (!frame->is_optimized()) continue;

    JSFunction function = frame->function();
    if (function.code().kind() != CodeKind::NATIVE_CONTEXT_INDEPENDENT) {
      continue;
    }

    DCHECK(function.shared().is_compiled());
    DCHECK(function.has_feedback_vector());

    function.feedback_vector().SaturatingIncrementProfilerTicks();

    MaybeOptimizeNCIFrame(function);
  }
}

}  // namespace internal
}  // namespace v8
