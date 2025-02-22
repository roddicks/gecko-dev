/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef PROFILER_FUNCS_H
#define PROFILER_FUNCS_H

#include "js/TypeDecls.h"
#include "js/ProfilingStack.h"
#include "mozilla/UniquePtr.h"
#include "mozilla/Vector.h"
#include "GeckoProfilerTypes.h"
#include <stdint.h>

class nsISupports;

namespace mozilla {
class TimeStamp;

namespace dom {
class Promise;
} // namespace dom

} // namespace mozilla

class ProfilerBacktrace;
class ProfilerMarkerPayload;

// Returns a handle to pass on exit. This can check that we are popping the
// correct callstack.
inline void* mozilla_sampler_call_enter(const char *aInfo, js::ProfileEntry::Category aCategory,
                                        void *aFrameAddress = nullptr, bool aCopy = false,
                                        uint32_t line = 0);

inline void  mozilla_sampler_call_exit(void* handle);

void  mozilla_sampler_add_marker(const char *aInfo,
                                 ProfilerMarkerPayload *aPayload = nullptr);

void mozilla_sampler_start(int aEntries, double aInterval,
                           const char** aFeatures, uint32_t aFeatureCount,
                           const char** aThreadNameFilters, uint32_t aFilterCount);

void mozilla_sampler_stop();

bool mozilla_sampler_is_paused();
void mozilla_sampler_pause();
void mozilla_sampler_resume();

UniqueProfilerBacktrace mozilla_sampler_get_backtrace();
void mozilla_sampler_get_backtrace_noalloc(char *output, size_t outputSize);

bool mozilla_sampler_is_active();

bool mozilla_sampler_feature_active(const char* aName);

void mozilla_sampler_responsiveness(const mozilla::TimeStamp& time);

void mozilla_sampler_frame_number(int frameNumber);

mozilla::UniquePtr<char[]> mozilla_sampler_get_profile(double aSinceTime);

JSObject *mozilla_sampler_get_profile_data(JSContext* aCx, double aSinceTime);
void mozilla_sampler_get_profile_data_async(double aSinceTime,
                                            mozilla::dom::Promise* aPromise);
MOZ_EXPORT
void mozilla_sampler_save_profile_to_file_async(double aSinceTime,
                                                const char* aFileName);
void mozilla_sampler_get_profiler_start_params(int* aEntrySize,
                                               double* aInterval,
                                               mozilla::Vector<const char*>* aFilters,
                                               mozilla::Vector<const char*>* aFeatures);
void mozilla_sampler_get_gatherer(nsISupports** aRetVal);

// Make this function easily callable from a debugger in a build without
// debugging information (work around http://llvm.org/bugs/show_bug.cgi?id=22211)
extern "C" {
  void mozilla_sampler_save_profile_to_file(const char* aFilename);
}

const char** mozilla_sampler_get_features();

void mozilla_sampler_get_buffer_info(uint32_t *aCurrentPosition, uint32_t *aTotalSize,
                                     uint32_t *aGeneration);

void mozilla_sampler_init(void* stackTop);

void mozilla_sampler_shutdown();

// Lock the profiler. When locked the profiler is (1) stopped,
// (2) profile data is cleared, (3) profiler-locked is fired.
// This is used to lock down the profiler during private browsing
void mozilla_sampler_lock();

// Unlock the profiler, leaving it stopped and fires profiler-unlocked.
void mozilla_sampler_unlock();

// Register/unregister threads with the profiler
bool mozilla_sampler_register_thread(const char* name, void* stackTop);
void mozilla_sampler_unregister_thread();

void mozilla_sampler_sleep_start();
void mozilla_sampler_sleep_end();
bool mozilla_sampler_is_sleeping();

double mozilla_sampler_time();
double mozilla_sampler_time(const mozilla::TimeStamp& aTime);

void mozilla_sampler_tracing(const char* aCategory, const char* aInfo,
                             TracingMetadata aMetaData);

void mozilla_sampler_tracing(const char* aCategory, const char* aInfo,
                             UniqueProfilerBacktrace aCause,
                             TracingMetadata aMetaData);

void mozilla_sampler_log(const char *fmt, va_list args);

#endif

