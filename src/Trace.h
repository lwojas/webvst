#pragma once
#include <juce_core/juce_core.h>

// Opt-in timing log for diagnosing a slow window open, written to ~/Library/Logs/WebVST/trace.log (macOS) with a
// millisecond clock shared by native and page events (the page sends marks, see shim.js).
// Turn it on by setting WEBVST_TRACE=1 in the host's environment, or by creating the file ~/.webvst-trace. Off, it costs
// nothing: the log lines are not even built into the file.
namespace Trace {

inline bool enabled() {
  static const bool on = juce::SystemStats::getEnvironmentVariable("WEBVST_TRACE", {}).isNotEmpty() ||
                         juce::File::getSpecialLocation(juce::File::userHomeDirectory).getChildFile(".webvst-trace").exists();
  return on;
}

inline void log(const juce::String& message) {
  if (!enabled()) return;
  static const juce::File file = juce::File::getSpecialLocation(juce::File::userHomeDirectory).getChildFile("Library/Logs/WebVST/trace.log");
  static const double start = juce::Time::getMillisecondCounterHiRes();
  static juce::CriticalSection lock;
  const juce::ScopedLock scoped(lock);
  file.getParentDirectory().createDirectory();
  const auto line = juce::String(juce::Time::getMillisecondCounterHiRes() - start, 0).paddedLeft(' ', 7) + " ms  " + message + "\n";
  file.appendText(line);
}

}  // namespace Trace
