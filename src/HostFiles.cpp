#include "HostFiles.h"

namespace {
constexpr int64_t kMaxFileBytes = int64_t{512} * 1024 * 1024;
constexpr size_t kChunkBytes = 96 * 1024;

juce::var object(std::initializer_list<std::pair<const char*, juce::var>> props) {
  auto* o = new juce::DynamicObject();
  for (auto& p : props) o->setProperty(p.first, p.second);
  return juce::var(o);
}

juce::String wildcardFor(const juce::StringArray& extensions) {
  juce::StringArray patterns;
  for (const auto& e : extensions) {
    const auto clean = e.trimCharactersAtStart(".*").trim();
    if (clean.isNotEmpty()) patterns.add("*." + clean);
  }
  return patterns.isEmpty() ? juce::String("*") : patterns.joinIntoString(";");
}
}  // namespace

HostFiles::HostFiles(Chooser chooser)
    : chooser_(std::move(chooser)), lastDirectory_(juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)) {}

HostFiles::~HostFiles() { *alive_ = false; }

void HostFiles::setEventSink(std::function<void(const juce::var&)> sink) { sink_ = std::move(sink); }

void HostFiles::send(const juce::var& event) {
  if (sink_) sink_(event);
}

bool HostFiles::handleMessage(const juce::var& msg) {
  const juce::String type = msg["type"].toString();
  if (!isFilesMessage(type)) return false;
  const int id = static_cast<int>(msg["id"]);

  if (type == "files-save-begin") {
    const double bytes = static_cast<double>(msg["bytes"]);
    if (bytes < 0 || bytes > static_cast<double>(kMaxFileBytes)) {
      send(object({{"type", "files-save-result"}, {"id", id}, {"name", juce::var()}, {"error", "the file is too large to save"}}));
      return true;
    }
    Upload u;
    u.name = msg["name"].toString();
    u.bytes.assign(static_cast<size_t>(bytes), 0);
    uploads_[id] = std::move(u);
  } else if (type == "files-save-chunk") {
    auto it = uploads_.find(id);
    if (it == uploads_.end()) return true;
    juce::MemoryOutputStream out;
    if (!juce::Base64::convertFromBase64(out, msg["data"].toString())) return true;
    const double offset = static_cast<double>(msg["offsetBytes"]);
    auto& bytes = it->second.bytes;
    if (offset < 0 || offset >= static_cast<double>(bytes.size())) return true;
    const size_t at = static_cast<size_t>(offset);
    std::memcpy(bytes.data() + at, out.getData(), std::min(out.getDataSize(), bytes.size() - at));
  } else if (type == "files-save-end") {
    auto it = uploads_.find(id);
    if (it == uploads_.end()) return true;
    Upload u = std::move(it->second);
    uploads_.erase(it);
    finishSave(id, std::move(u));
  } else if (type == "files-open") {
    juce::StringArray extensions;
    if (auto* list = msg["extensions"].getArray())
      for (const auto& e : *list) extensions.add(e.toString());
    startOpen(id, extensions);
  } else {
    send(object({{"type", "error"}, {"message", "host files: unknown message '" + type + "'"}}));
  }
  return true;
}

void HostFiles::finishSave(int id, Upload upload) {
  if (dialogOpen_) {
    send(object({{"type", "files-save-result"}, {"id", id}, {"name", juce::var()}, {"error", "another file dialog is already open"}}));
    return;
  }
  dialogOpen_ = true;
  ChooserRequest request;
  request.save = true;
  request.title = "Save file";
  request.startDirectory = lastDirectory_;
  request.suggestedName = upload.name;
  request.wildcard = "*" + upload.name.fromLastOccurrenceOf(".", true, false);  // "*.json", or "*" for a name with no extension
  auto alive = alive_;
  auto data = std::make_shared<Upload>(std::move(upload));
  chooser_(request, [this, alive, id, data](juce::File chosen) {
    if (!*alive) return;
    dialogOpen_ = false;
    if (chosen == juce::File()) {
      send(object({{"type", "files-save-result"}, {"id", id}, {"name", juce::var()}}));
      return;
    }
    lastDirectory_ = chosen.getParentDirectory();
    if (chosen.replaceWithData(data->bytes.data(), data->bytes.size()))
      send(object({{"type", "files-save-result"}, {"id", id}, {"name", chosen.getFileName()}}));
    else
      send(object({{"type", "files-save-result"}, {"id", id}, {"name", juce::var()}, {"error", "could not write " + chosen.getFullPathName()}}));
  });
}

void HostFiles::startOpen(int id, const juce::StringArray& extensions) {
  if (dialogOpen_) {
    send(object({{"type", "files-open-result"}, {"id", id}, {"error", "another file dialog is already open"}}));
    return;
  }
  dialogOpen_ = true;
  ChooserRequest request;
  request.save = false;
  request.title = "Open file";
  request.startDirectory = lastDirectory_;
  request.wildcard = wildcardFor(extensions);
  auto alive = alive_;
  chooser_(request, [this, alive, id](juce::File chosen) {
    if (!*alive) return;
    dialogOpen_ = false;
    if (chosen == juce::File()) {
      send(object({{"type", "files-open-result"}, {"id", id}, {"cancelled", true}}));
      return;
    }
    lastDirectory_ = chosen.getParentDirectory();
    if (chosen.getSize() > kMaxFileBytes) {
      send(object({{"type", "files-open-result"}, {"id", id}, {"error", chosen.getFileName() + " is too large to open"}}));
      return;
    }
    juce::MemoryBlock data;
    if (!chosen.loadFileAsData(data)) {
      send(object({{"type", "files-open-result"}, {"id", id}, {"error", "could not read " + chosen.getFullPathName()}}));
      return;
    }
    send(object({{"type", "files-open-begin"}, {"id", id}, {"name", chosen.getFileName()}, {"bytes", static_cast<double>(data.getSize())}}));
    for (size_t at = 0; at < data.getSize(); at += kChunkBytes) {
      const size_t n = std::min(kChunkBytes, data.getSize() - at);
      send(object({{"type", "files-open-chunk"},
                   {"id", id},
                   {"offsetBytes", static_cast<double>(at)},
                   {"data", juce::Base64::toBase64(static_cast<const char*>(data.getData()) + at, n)}}));
    }
    send(object({{"type", "files-open-end"}, {"id", id}}));
  });
}
