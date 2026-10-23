#pragma once
#include <functional>
#include <map>
#include <memory>
#include <vector>
#include <juce_core/juce_core.h>

// Open and save files through the host's own dialogs, for a page whose WebView cannot (webdsp docs/host-files.md:
// WKWebView has no file input and ignores downloads).
//
// Page -> plugin (the "webvst" event channel, message thread):
//   files-save-begin {id, name, bytes}, files-save-chunk {id, offsetBytes, data (base64)}*, files-save-end {id}
//   files-open {id, extensions}
// Plugin -> page:
//   files-save-result {id, name | null (cancelled), error?}
//   files-open-begin {id, name, bytes}, files-open-chunk {id, offsetBytes, data}*, files-open-end {id}
//   files-open-result {id, cancelled? , error?}   (sent instead of begin/chunk/end when nothing was read)
// One dialog at a time; a request that arrives while one is open is answered with an error.
class HostFiles {
 public:
  struct ChooserRequest {
    bool save = false;
    juce::String title;
    juce::File startDirectory;
    juce::String suggestedName;  // save only
    juce::String wildcard;       // e.g. "*.json"
  };
  // Shows a dialog for `request` and calls `done` with the chosen file, or juce::File() if cancelled. May call back later.
  using Chooser = std::function<void(const ChooserRequest&, std::function<void(juce::File)> done)>;

  explicit HostFiles(Chooser chooser);
  ~HostFiles();

  void setEventSink(std::function<void(const juce::var&)> sink);

  // Message thread. Returns true if the type is one of ours.
  bool handleMessage(const juce::var& message);
  static bool isFilesMessage(const juce::String& type) { return type.startsWith("files-"); }

 private:
  struct Upload {
    juce::String name;
    std::vector<uint8_t> bytes;
  };

  void send(const juce::var& event);
  void finishSave(int id, Upload upload);
  void startOpen(int id, const juce::StringArray& extensions);

  Chooser chooser_;
  std::function<void(const juce::var&)> sink_;
  std::map<int, Upload> uploads_;
  bool dialogOpen_ = false;
  juce::File lastDirectory_;
  std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
};
