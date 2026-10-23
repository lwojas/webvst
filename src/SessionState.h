#pragma once
#include <functional>
#include <map>
#include <memory>
#include <vector>
#include <juce_core/juce_core.h>

// The application's project, kept in the plugin so the DAW session saves it (webdsp docs/host-state.md).
//
// The page publishes an opaque snapshot: the application's own JSON text plus the binary blobs (sample files) it
// refers to by key. We hold the latest committed snapshot in memory, so getStateInformation never waits on the
// page, and a session saves correctly with the editor closed. setStateInformation loads a saved one, and the page
// is handed it when it asks (or at once, if it is already listening).
//
// Page -> plugin messages (all on the "webvst" event channel, all on the message thread):
//   state-begin {app, format}, state-text {data}*, state-blob-begin {key, bytes}, state-blob-chunk {key,
//   offsetBytes, data (base64)}*, state-commit {keys}, state-clear, state-ready.
// Plugin -> page: state-restore-begin {app, format, blobs:[{key,bytes}]}, state-restore-text {data}*,
//   state-restore-blob {key, offsetBytes, data}*, state-restore-end.
// A commit replaces the snapshot atomically; a blob the page does not re-send keeps the copy we hold.
class SessionState {
 public:
  // `requiredApp`: when non-empty, only state stamped with this app is accepted from the host (setStateInformation).
  explicit SessionState(juce::String requiredApp = {});

  void setEventSink(std::function<void(const juce::var&)> sink);

  // Message thread: a "state-*" message from the page. Returns true if the type is one of ours.
  bool handleMessage(const juce::var& message);

  // The DAW session blob. serialize() may be called from any thread; restore() from the message thread.
  void serialize(juce::MemoryBlock& destination) const;
  bool restore(const void* data, size_t size);

  bool hasState() const;
  juce::String app() const;

  static bool isStateMessage(const juce::String& type) { return type.startsWith("state-"); }

 private:
  using Bytes = std::shared_ptr<const std::vector<uint8_t>>;
  struct Data {
    juce::String app;
    int format = 0;
    juce::String text;
    std::map<juce::String, Bytes> blobs;
  };

  void sendRestore();
  void send(const juce::var& event);
  void fail(const juce::String& message);

  const juce::String requiredApp_;
  std::function<void(const juce::var&)> sink_;

  mutable juce::CriticalSection lock_;
  std::shared_ptr<const Data> current_;  // the committed snapshot

  // message thread only
  struct Pending {
    juce::String app;
    int format = 0;
    juce::String text;
  };
  std::unique_ptr<Pending> pending_;
  struct PendingBlob {
    std::vector<uint8_t> bytes;
  };
  std::map<juce::String, PendingBlob> pendingBlobs_;
  std::map<juce::String, Bytes> blobStore_;  // blobs of the current snapshot, so a re-publish need not resend them
  bool pageListening_ = false;
};
