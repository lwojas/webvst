#include "SessionState.h"
#include "Trace.h"

namespace {
constexpr int kFormatVersion = 1;
constexpr int kEndMarker = 0x54535657;  // closes the body, so a truncated blob is never mistaken for a whole one
constexpr int64_t kMaxBlobBytes = int64_t{1} << 31;
constexpr int kTextChunkChars = 256 * 1024;
constexpr size_t kBlobChunkBytes = 96 * 1024;

juce::var object(std::initializer_list<std::pair<const char*, juce::var>> props) {
  auto* o = new juce::DynamicObject();
  for (auto& p : props) o->setProperty(p.first, p.second);
  return juce::var(o);
}
}  // namespace

SessionState::SessionState(juce::String requiredApp) : requiredApp_(std::move(requiredApp)) {}

void SessionState::setEventSink(std::function<void(const juce::var&)> sink) {
  sink_ = std::move(sink);
  if (!sink_) pageListening_ = false;  // the page went away; the next one asks again
}

void SessionState::send(const juce::var& event) {
  if (sink_) sink_(event);
}

void SessionState::fail(const juce::String& message) {
  send(object({{"type", "error"}, {"message", "session state: " + message}}));
}

bool SessionState::hasState() const {
  const juce::ScopedLock lock(lock_);
  return current_ != nullptr;
}

juce::String SessionState::app() const {
  const juce::ScopedLock lock(lock_);
  return current_ ? current_->app : juce::String();
}

bool SessionState::handleMessage(const juce::var& msg) {
  const juce::String type = msg["type"].toString();
  if (!isStateMessage(type)) return false;

  if (type == "state-begin") {
    pending_ = std::make_unique<Pending>();
    pending_->app = msg["app"].toString();
    pending_->format = static_cast<int>(msg["format"]);
    pendingBlobs_.clear();
  } else if (type == "state-text") {
    if (pending_) pending_->text += msg["data"].toString();
  } else if (type == "state-blob-begin") {
    const double bytes = static_cast<double>(msg["bytes"]);
    if (bytes >= 0 && bytes <= static_cast<double>(kMaxBlobBytes)) pendingBlobs_[msg["key"].toString()].bytes.assign(static_cast<size_t>(bytes), 0);
  } else if (type == "state-blob-chunk") {
    auto it = pendingBlobs_.find(msg["key"].toString());
    if (it == pendingBlobs_.end()) return true;
    juce::MemoryOutputStream out;
    if (!juce::Base64::convertFromBase64(out, msg["data"].toString())) return true;
    const double offset = static_cast<double>(msg["offsetBytes"]);
    auto& bytes = it->second.bytes;
    if (offset < 0 || offset >= static_cast<double>(bytes.size())) return true;
    const size_t at = static_cast<size_t>(offset);
    const size_t n = std::min(out.getDataSize(), bytes.size() - at);
    std::memcpy(bytes.data() + at, out.getData(), n);
  } else if (type == "state-commit") {
    if (!pending_) return true;
    auto data = std::make_shared<Data>();
    data->app = pending_->app;
    data->format = pending_->format;
    data->text = std::move(pending_->text);
    std::map<juce::String, Bytes> store;
    if (auto* keys = msg["keys"].getArray()) {
      for (const auto& k : *keys) {
        const juce::String key = k.toString();
        if (auto it = pendingBlobs_.find(key); it != pendingBlobs_.end()) {
          store[key] = std::make_shared<const std::vector<uint8_t>>(std::move(it->second.bytes));
        } else if (auto old = blobStore_.find(key); old != blobStore_.end()) {
          store[key] = old->second;
        } else {
          pending_.reset();
          pendingBlobs_.clear();
          fail("commit refers to blob '" + key + "' that was never sent; the snapshot was not saved");
          return true;
        }
      }
    }
    data->blobs = store;
    blobStore_ = std::move(store);
    pending_.reset();
    pendingBlobs_.clear();
    const juce::ScopedLock lock(lock_);
    current_ = std::move(data);
  } else if (type == "state-clear") {
    pending_.reset();
    pendingBlobs_.clear();
    blobStore_.clear();
    const juce::ScopedLock lock(lock_);
    current_.reset();
  } else if (type == "state-ready") {
    pageListening_ = true;
    sendRestore();
  } else {
    fail("unknown message '" + type + "'");
  }
  return true;
}

// Hands the page the committed snapshot (if any) in pieces small enough to ride the event channel.
void SessionState::sendRestore() {
  std::shared_ptr<const Data> d;
  {
    const juce::ScopedLock lock(lock_);
    d = current_;
  }
  if (!sink_) return;
  if (!d) {  // a definite "nothing saved", so the page can tell a new session from a slow plugin
    send(object({{"type", "state-restore-none"}}));
    return;
  }

  juce::Array<juce::var> blobList;
  for (const auto& [key, bytes] : d->blobs) blobList.add(object({{"key", key}, {"bytes", static_cast<double>(bytes->size())}}));
  Trace::log("native: sending the saved project to the page (" + juce::String(d->text.length()) + " chars, " + juce::String(static_cast<int>(d->blobs.size())) + " blobs)");
  send(object({{"type", "state-restore-begin"}, {"app", d->app}, {"format", d->format}, {"blobs", blobList}}));
  for (int at = 0; at < d->text.length(); at += kTextChunkChars)
    send(object({{"type", "state-restore-text"}, {"data", d->text.substring(at, at + kTextChunkChars)}}));
  for (const auto& [key, bytes] : d->blobs) {
    for (size_t at = 0; at < bytes->size(); at += kBlobChunkBytes) {
      const size_t n = std::min(kBlobChunkBytes, bytes->size() - at);
      send(object({{"type", "state-restore-blob"},
                   {"key", key},
                   {"offsetBytes", static_cast<double>(at)},
                   {"data", juce::Base64::toBase64(bytes->data() + at, n)}}));
    }
  }
  send(object({{"type", "state-restore-end"}}));
  Trace::log("native: saved project sent");
}

// "WVST" | int version | gzip( app, format, text, blobCount, { key, size, bytes }* )
void SessionState::serialize(juce::MemoryBlock& destination) const {
  std::shared_ptr<const Data> d;
  {
    const juce::ScopedLock lock(lock_);
    d = current_;
  }
  if (!d) return;

  juce::MemoryOutputStream compressed;
  {
    juce::GZIPCompressorOutputStream gz(compressed, 6);
    gz.writeString(d->app);
    gz.writeInt(d->format);
    gz.writeString(d->text);
    gz.writeInt(static_cast<int>(d->blobs.size()));
    for (const auto& [key, bytes] : d->blobs) {
      gz.writeString(key);
      gz.writeInt64(static_cast<juce::int64>(bytes->size()));
      gz.write(bytes->data(), bytes->size());
    }
    gz.writeInt(kEndMarker);
    gz.flush();
  }
  destination.append("WVST", 4);
  const int version = juce::ByteOrder::swapIfBigEndian(kFormatVersion);
  destination.append(&version, sizeof(version));
  destination.append(compressed.getData(), compressed.getDataSize());
}

bool SessionState::restore(const void* data, size_t size) {
  if (size == 0) return false;
  if (size < 8 || std::memcmp(data, "WVST", 4) != 0) return false;
  int version = 0;
  std::memcpy(&version, static_cast<const char*>(data) + 4, sizeof(version));
  if (juce::ByteOrder::swapIfBigEndian(version) != kFormatVersion) return false;

  juce::MemoryInputStream raw(static_cast<const char*>(data) + 8, size - 8, false);
  juce::GZIPDecompressorInputStream in(&raw, false, juce::GZIPDecompressorInputStream::zlibFormat);

  auto parsed = std::make_shared<Data>();
  parsed->app = in.readString();
  if (parsed->app.isEmpty()) return false;  // a stamped snapshot always has an app: anything else is not ours
  parsed->format = in.readInt();
  parsed->text = in.readString();
  const int count = in.readInt();
  if (count < 0 || count > 1 << 20) return false;
  std::map<juce::String, Bytes> store;
  for (int i = 0; i < count; i++) {
    const juce::String key = in.readString();
    const juce::int64 n = in.readInt64();
    if (n < 0 || n > kMaxBlobBytes) return false;
    auto bytes = std::make_shared<std::vector<uint8_t>>(static_cast<size_t>(n));
    if (n > 0 && in.read(bytes->data(), static_cast<int>(std::min<juce::int64>(n, 0x7fffffff))) != static_cast<int>(n)) return false;
    store[key] = bytes;
  }
  if (in.readInt() != kEndMarker) return false;
  if (requiredApp_.isNotEmpty() && parsed->app != requiredApp_) return false;
  parsed->blobs = store;
  blobStore_ = std::move(store);
  {
    const juce::ScopedLock lock(lock_);
    current_ = std::move(parsed);
  }
  if (pageListening_) sendRestore();
  return true;
}
