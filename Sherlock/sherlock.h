/*******************************************************************************
The MIT License (MIT)

Copyright (c) 2015 Dmitry "Dima" Korolev <dmitry.korolev@gmail.com>

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*******************************************************************************/

#ifndef CURRENT_SHERLOCK_SHERLOCK_H
#define CURRENT_SHERLOCK_SHERLOCK_H

#include "../port.h"

#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <thread>

#include "exceptions.h"
#include "stream_data.h"
#include "pubsub.h"

#include "../TypeSystem/struct.h"
#include "../TypeSystem/Schema/schema.h"

#include "../Blocks/HTTP/api.h"
#include "../Blocks/Persistence/persistence.h"
#include "../Blocks/SS/ss.h"
#include "../Blocks/SS/signature.h"

#include "../Bricks/sync/locks.h"
#include "../Bricks/sync/scope_owned.h"
#include "../Bricks/time/chrono.h"
#include "../Bricks/util/waitable_terminate_signal.h"

// Sherlock is the overlord of streamed data storage and processing in Current.
// Sherlock's streams are persistent, immutable, append-only typed sequences of records ("entries").
// Each record is annotated with its the 1-based index and its epoch microsecond timestamp.
// Within the stream, timestamps are strictly increasing.
//
// A stream is constructed as `auto my_stream = sherlock::Stream<ENTRY>()`. This creates an in-memory stream.

// To create a persisted one, pass in the type of persister and its construction parameters, such as:
// `auto my_stream = sherlock::Stream<ENTRY, current::persistence::File>("data.json");`.
//
// Sherlock streams can be published into and subscribed to.
//
// Publishing is done via `my_stream.Publish(ENTRY{...});`.
//
// Subscription is done via `auto scope = my_stream.Subscribe(my_subscriber);`, where `my_subscriber`
// is an instance of the class doing the subscription. Sherlock runs each subscriber in a dedicated thread.
//
// Stack ownership of `my_subscriber` is respected, and `SubscriberScope` is returned for the user to store.
// As the returned `scope` object leaves the scope, the subscriber is sent a signal to terminate,
// and the destructor of `scope` waits for the subscriber to do so. The `scope` objects can be `std::move()`-d.
//
// The `my_subscriber` object should be an instance of `StreamSubscriber<IMPL, ENTRY>`,

namespace current {
namespace sherlock {

namespace constants {

constexpr const char* kDefaultNamespaceName = "SherlockSchema";
constexpr const char* kDefaultTopLevelName = "TopLevelTransaction";

}  // namespace constants

CURRENT_STRUCT(SherlockSchema) {
  CURRENT_FIELD(language, (std::map<std::string, std::string>));
  CURRENT_FIELD(type_name, std::string);
  CURRENT_FIELD(type_id, current::reflection::TypeID);
  CURRENT_FIELD(type_schema, reflection::SchemaInfo);
  CURRENT_DEFAULT_CONSTRUCTOR(SherlockSchema) {}
};

CURRENT_STRUCT(SubscribableSherlockSchema) {
  CURRENT_FIELD(type_id, current::reflection::TypeID, current::reflection::TypeID::UninitializedType);
  CURRENT_FIELD(entry_name, std::string);
  CURRENT_FIELD(namespace_name, std::string);
  CURRENT_DEFAULT_CONSTRUCTOR(SubscribableSherlockSchema) {}
  CURRENT_CONSTRUCTOR(SubscribableSherlockSchema)(
      current::reflection::TypeID type_id, const std::string& entry_name, const std::string& namespace_name)
      : type_id(type_id), entry_name(entry_name), namespace_name(namespace_name) {}
  bool operator==(const SubscribableSherlockSchema& rhs) const {
    return type_id == rhs.type_id && namespace_name == rhs.namespace_name && entry_name == rhs.entry_name;
  }
  bool operator!=(const SubscribableSherlockSchema& rhs) const { return !operator==(rhs); }
};

CURRENT_STRUCT(SherlockSchemaFormatNotFound) {
  CURRENT_FIELD(error, std::string, "Unsupported schema format requested.");
  CURRENT_FIELD(unsupported_format_requested, Optional<std::string>);
};

enum class StreamDataAuthority : bool { Own = true, External = false };

template <typename ENTRY>
using DEFAULT_PERSISTENCE_LAYER = current::persistence::Memory<ENTRY>;

template <typename ENTRY, template <typename> class PERSISTENCE_LAYER = DEFAULT_PERSISTENCE_LAYER>
class StreamImpl {
 public:
  using entry_t = ENTRY;
  using persistence_layer_t = PERSISTENCE_LAYER<entry_t>;
  using stream_data_t = StreamData<entry_t, PERSISTENCE_LAYER>;

  class StreamPublisher {
   public:
    explicit StreamPublisher(ScopeOwned<stream_data_t>& data) : data_(data, []() {}) {}

    template <current::locks::MutexLockStatus MLS>
    idxts_t DoPublish(const entry_t& entry, const current::time::DefaultTimeArgument) {
      return PublishImpl<MLS>(entry);
    }

    template <current::locks::MutexLockStatus MLS>
    idxts_t DoPublish(const entry_t& entry, const std::chrono::microseconds us) {
      return PublishImpl<MLS>(entry, us);
    }

    template <current::locks::MutexLockStatus MLS>
    idxts_t DoPublish(entry_t&& entry, const current::time::DefaultTimeArgument) {
      return PublishImpl<MLS>(std::move(entry));
    }

    template <current::locks::MutexLockStatus MLS>
    idxts_t DoPublish(entry_t&& entry, const std::chrono::microseconds us) {
      return PublishImpl<MLS>(std::move(entry), us);
    }

    template <current::locks::MutexLockStatus MLS>
    void DoUpdateHead(const current::time::DefaultTimeArgument) {
      UpdateHeadImpl<MLS>();
    }

    template <current::locks::MutexLockStatus MLS>
    void DoUpdateHead(const std::chrono::microseconds us) {
      UpdateHeadImpl<MLS>(us);
    }

    operator bool() const { return data_; }

   private:
    template <current::locks::MutexLockStatus MLS, typename... ARGS>
    idxts_t PublishImpl(ARGS&&... args) {
      try {
        auto& data = *data_;
        current::locks::SmartMutexLockGuard<MLS> lock(data.publish_mutex);
        const auto result = data.persistence.template Publish<current::locks::MutexLockStatus::AlreadyLocked>(
            std::forward<ARGS>(args)...);
        data.notifier.NotifyAllOfExternalWaitableEvent();
        return result;
      } catch (const current::sync::InDestructingModeException&) {
        CURRENT_THROW(StreamInGracefulShutdownException());
      }
    }

    template <current::locks::MutexLockStatus MLS, typename... ARGS>
    void UpdateHeadImpl(ARGS&&... args) {
      try {
        auto& data = *data_;
        current::locks::SmartMutexLockGuard<MLS> lock(data.publish_mutex);
        data.persistence.template UpdateHead<current::locks::MutexLockStatus::AlreadyLocked>(
            std::forward<ARGS>(args)...);
        data.notifier.NotifyAllOfExternalWaitableEvent();
      } catch (const current::sync::InDestructingModeException&) {
        CURRENT_THROW(StreamInGracefulShutdownException());
      }
    }

    ScopeOwnedBySomeoneElse<stream_data_t> data_;
  };
  using publisher_t = ss::StreamPublisher<StreamPublisher, entry_t>;

  StreamImpl()
      : schema_as_object_(StaticConstructSchemaAsObject(schema_namespace_name_)),
        own_data_(schema_namespace_name_),
        publisher_(std::make_unique<publisher_t>(own_data_)),
        authority_(StreamDataAuthority::Own) {}

  StreamImpl(const ss::StreamNamespaceName& namespace_name)
      : schema_namespace_name_(namespace_name),
        schema_as_object_(StaticConstructSchemaAsObject(schema_namespace_name_)),
        own_data_(schema_namespace_name_),
        publisher_(std::make_unique<publisher_t>(own_data_)),
        authority_(StreamDataAuthority::Own) {}

  template <typename X, typename... XS, class = std::enable_if_t<!std::is_same<X, ss::StreamNamespaceName>::value>>
  StreamImpl(X&& x, XS&&... xs)
      : schema_as_object_(StaticConstructSchemaAsObject(schema_namespace_name_)),
        own_data_(schema_namespace_name_, std::forward<X>(x), std::forward<XS>(xs)...),
        publisher_(std::make_unique<publisher_t>(own_data_)),
        authority_(StreamDataAuthority::Own) {}

  template <typename X, typename... XS>
  StreamImpl(const ss::StreamNamespaceName& namespace_name, X&& x, XS&&... xs)
      : schema_namespace_name_(namespace_name),
        schema_as_object_(StaticConstructSchemaAsObject(schema_namespace_name_)),
        own_data_(schema_namespace_name_, std::forward<X>(x), std::forward<XS>(xs)...),
        publisher_(std::make_unique<publisher_t>(own_data_)),
        authority_(StreamDataAuthority::Own) {}

  StreamImpl(StreamImpl&& rhs)
      : schema_namespace_name_(rhs.schema_namespace_name_),
        schema_as_object_(StaticConstructSchemaAsObject(schema_namespace_name_)),
        own_data_(std::move(rhs.own_data_)),
        publisher_(std::move(rhs.publisher_)),
        authority_(rhs.authority_) {
    rhs.authority_ = StreamDataAuthority::External;
  }

  ~StreamImpl() {
    auto& own_data = own_data_.ObjectAccessorDespitePossiblyDestructing();
    auto& http_subscriptions = own_data.http_subscriptions;
    // Ask all the HTTP subscibers to terminate asynchronously.
    {
      std::lock_guard<std::mutex> lock(http_subscriptions->mutex);
      for (auto& it : http_subscriptions->subscribers_map) {
        auto& subsciber_scope = *(it.second.first);
        subsciber_scope.AsyncTerminate();
      }
    }
    // Waiting for all the `subscribers_map` entries to be wiped by asynchronous tasks.
    while (true) {
      {
        std::lock_guard<std::mutex> lock(http_subscriptions->mutex);
        if (http_subscriptions->subscribers_map.empty()) {
          break;
        }
      }
      std::this_thread::yield();
    }
  }

  void operator=(StreamImpl&& rhs) {
    own_data_ = std::move(rhs.own_data_);
    publisher_ = std::move(rhs.publisher_);
    authority_ = rhs.authority_;
    rhs.authority_ = StreamDataAuthority::External;
  }

  idxts_t Publish(const entry_t& entry) { return PublishImpl(entry); }

  idxts_t Publish(const entry_t& entry, const std::chrono::microseconds us) { return PublishImpl(entry, us); }

  idxts_t Publish(entry_t&& entry) { return PublishImpl(std::move(entry)); }

  idxts_t Publish(entry_t&& entry, const std::chrono::microseconds us) { return PublishImpl(std::move(entry), us); }

  void UpdateHead() { UpdateHeadImpl(); }

  void UpdateHead(const std::chrono::microseconds us) { UpdateHeadImpl(us); }

  template <typename ACQUIRER>
  void MovePublisherTo(ACQUIRER&& acquirer) {
    std::lock_guard<std::mutex> lock(publisher_mutex_);
    if (publisher_) {
      acquirer.AcceptPublisher(std::move(publisher_));
      authority_ = StreamDataAuthority::External;
    } else {
      CURRENT_THROW(PublisherAlreadyReleasedException());
    }
  }

  void AcquirePublisher(std::unique_ptr<publisher_t> publisher) {
    std::lock_guard<std::mutex> lock(publisher_mutex_);
    if (!publisher_) {
      publisher_ = std::move(publisher);
      authority_ = StreamDataAuthority::Own;
    } else {
      CURRENT_THROW(PublisherAlreadyOwnedException());
    }
  }

  StreamDataAuthority DataAuthority() const {
    std::lock_guard<std::mutex> lock(publisher_mutex_);
    return authority_;
  }

  template <typename TYPE_SUBSCRIBED_TO, typename F>
  class SubscriberThreadInstance final : public current::sherlock::SubscriberScope::SubscriberThread {
   private:
    bool this_is_valid_;
    std::function<void()> done_callback_;
    current::WaitableTerminateSignal terminate_signal_;
    ScopeOwnedBySomeoneElse<stream_data_t> data_;
    F& subscriber_;
    const uint64_t begin_idx_;
    std::thread thread_;
    std::atomic_bool termination_requested_;

    SubscriberThreadInstance() = delete;
    SubscriberThreadInstance(const SubscriberThreadInstance&) = delete;
    SubscriberThreadInstance(SubscriberThreadInstance&&) = delete;
    void operator=(const SubscriberThreadInstance&) = delete;
    void operator=(SubscriberThreadInstance&&) = delete;

   public:
    SubscriberThreadInstance(ScopeOwned<stream_data_t>& data,
                             F& subscriber,
                             uint64_t begin_idx,
                             std::function<void()> done_callback)
        : this_is_valid_(false),
          done_callback_(done_callback),
          terminate_signal_(),
          data_(data,
                [this]() {
                  std::lock_guard<std::mutex> lock(data_.ObjectAccessorDespitePossiblyDestructing().publish_mutex);
                  terminate_signal_.SignalExternalTermination();
                }),
          subscriber_(subscriber),
          begin_idx_(begin_idx),
          thread_(&SubscriberThreadInstance::Thread, this),
          termination_requested_(false) {
      // Must guard against the constructor of `ScopeOwnedBySomeoneElse<stream_data_t> data_` throwing.
      this_is_valid_ = true;
    }

    ~SubscriberThreadInstance() {
      if (this_is_valid_) {
        // The constructor has completed successfully. The thread has started, and `data_` is valid.
        if (!termination_requested_) {
          TerminateSubscription();
        }
        CURRENT_ASSERT(thread_.joinable());
        thread_.join();
      } else {
        // The constructor has not completed successfully. The thread was not started, and `data_` is garbage.
        if (done_callback_) {
          // TODO(dkorolev): Fix this ownership issue.
          done_callback_();
        }
      }
    }

    void Thread() {
      // Keep the subscriber thread exception-safe. By construction, it's guaranteed to live
      // strictly within the scope of existence of `stream_data_t` contained in `data_`.
      stream_data_t& bare_data = data_.ObjectAccessorDespitePossiblyDestructing();
      ThreadImpl(bare_data, begin_idx_);
      subscriber_thread_done_ = true;
      std::lock_guard<std::mutex> lock(bare_data.http_subscriptions->mutex);
      if (done_callback_) {
        done_callback_();
      }
    }

    void ThreadImpl(stream_data_t& bare_data, uint64_t begin_idx) {
      auto head = std::chrono::microseconds(-1);
      uint64_t index = begin_idx;
      uint64_t size = 0;
      bool terminate_sent = false;
      while (true) {
        // TODO(dkorolev): This `EXCL` section can and should be tested by subscribing to an empty stream.
        // TODO(dkorolev): This is actually more a case of `EndReached()` first, right?
        if (!terminate_sent && terminate_signal_) {
          terminate_sent = true;
          if (subscriber_.Terminate() != ss::TerminationResponse::Wait) {
            return;
          }
        }
        auto head_idx = bare_data.persistence.HeadAndLastPublishedIndexAndTimestamp();
        size = Exists(head_idx.idxts) ? Value(head_idx.idxts).index + 1 : 0;
        if (head_idx.head > head) {
          if (size > index) {
            for (const auto& e : bare_data.persistence.Iterate(index, size)) {
              if (!terminate_sent && terminate_signal_) {
                terminate_sent = true;
                if (subscriber_.Terminate() != ss::TerminationResponse::Wait) {
                  return;
                }
              }
              if (current::ss::PassEntryToSubscriberIfTypeMatches<TYPE_SUBSCRIBED_TO, entry_t>(
                      subscriber_,
                      [this]() -> ss::EntryResponse { return subscriber_.EntryResponseIfNoMorePassTypeFilter(); },
                      e.entry,
                      e.idx_ts,
                      bare_data.persistence.LastPublishedIndexAndTimestamp()) == ss::EntryResponse::Done) {
                return;
              }
            }
            index = size;
            head = Value(head_idx.idxts).us;
          }
          if (size > begin_idx && head_idx.head > head && subscriber_(head_idx.head) == ss::EntryResponse::Done) {
            return;
          }
          head = head_idx.head;
        } else {
          std::unique_lock<std::mutex> lock(bare_data.publish_mutex);
          current::WaitableTerminateSignalBulkNotifier::Scope scope(bare_data.notifier, terminate_signal_);
          terminate_signal_.WaitUntil(
              lock,
              [this, &bare_data, &index, &begin_idx, &head]() {
                return terminate_signal_ ||
                       bare_data.persistence.template Size<current::locks::MutexLockStatus::AlreadyLocked>() > index ||
                       (index > begin_idx &&
                        bare_data.persistence.template CurrentHead<current::locks::MutexLockStatus::AlreadyLocked>() >
                            head);
              });
        }
      }
    }

    void TerminateSubscription() {
      if (!termination_requested_) {
        termination_requested_ = true;
        if (!subscriber_thread_done_) {
          std::lock_guard<std::mutex> lock(data_.ObjectAccessorDespitePossiblyDestructing().publish_mutex);
          terminate_signal_.SignalExternalTermination();
        }
      }
    }
  };

  // Expose the means to control the scope of the subscriber.
  template <typename F, typename TYPE_SUBSCRIBED_TO = entry_t>
  class SubscriberScope final : public current::sherlock::SubscriberScope {
   private:
    static_assert(current::ss::IsStreamSubscriber<F, TYPE_SUBSCRIBED_TO>::value, "");
    using base_t = current::sherlock::SubscriberScope;

   public:
    using subscriber_thread_t = SubscriberThreadInstance<TYPE_SUBSCRIBED_TO, F>;

    SubscriberScope(ScopeOwned<stream_data_t>& data,
                    F& subscriber,
                    uint64_t begin_idx,
                    std::function<void()> done_callback)
        : base_t(std::move(std::make_unique<subscriber_thread_t>(data, subscriber, begin_idx, done_callback))) {}

    virtual void AsyncTerminate() override {
      dynamic_cast<subscriber_thread_t&>(*thread_.get()).TerminateSubscription();
    }
  };

  template <typename TYPE_SUBSCRIBED_TO = entry_t, typename F>
  SubscriberScope<F, TYPE_SUBSCRIBED_TO> Subscribe(F& subscriber,
                                                   uint64_t begin_idx = 0u,
                                                   std::function<void()> done_callback = nullptr) {
    static_assert(current::ss::IsStreamSubscriber<F, TYPE_SUBSCRIBED_TO>::value, "");
    try {
      return SubscriberScope<F, TYPE_SUBSCRIBED_TO>(own_data_, subscriber, begin_idx, done_callback);
    } catch (const current::sync::InDestructingModeException&) {
      CURRENT_THROW(StreamInGracefulShutdownException());
    }
  }

  // Sherlock handler for serving stream data via HTTP (see `pubsub.h` for details).
  template <class J>
  void ServeDataViaHTTP(Request r) {
    try {
      // Prevent `own_data_` from being destroyed between the entry into this function
      // and the call to the construction of `PubSubHTTPEndpoint`.
      //
      // Granted, an overkill, as whoever could call `ServeDataViaHTTP` from another thread should have
      // its own `ScopeOwnedBySomeoneElse<>` copy of the stream object. But err on the safe side. -- D.K.
      ScopeOwnedBySomeoneElse<stream_data_t> scoped_data(own_data_, []() {});
      stream_data_t& data = *scoped_data;

      auto request_params = ParsePubSubHTTPRequest(r);  // Mutable as `tail` may change. -- D.K.

      if (request_params.terminate_requested) {
        {
          auto& http_subscriptions = *(data.http_subscriptions);
          std::lock_guard<std::mutex> lock(http_subscriptions.mutex);
          auto it = http_subscriptions.subscribers_map.find(request_params.terminate_id);
          if (it != http_subscriptions.subscribers_map.end()) {
            // Subscription found.
            auto& subscriber_scope = *(it->second.first);
            // Subscription will be terminated asynchronously.
            subscriber_scope.AsyncTerminate();
            r("", HTTPResponseCode.OK);
          } else {
            // Subscription not found.
            r("", HTTPResponseCode.NotFound);
          }
        }
        return;
      }

      // Unsupported HTTP method.
      if (r.method != "GET" && r.method != "HEAD") {
        r(current::net::DefaultMethodNotAllowedMessage(), HTTPResponseCode.MethodNotAllowed);
        return;
      }

      const auto stream_size = data.persistence.Size();

      if (request_params.size_only) {
        // Return the number of entries in the stream in `X-Current-Stream-Size` header and in the body in
        // case of `GET` method.
        const std::string size_str = current::ToString(stream_size);
        const std::string body = (r.method == "GET") ? size_str + '\n' : "";
        r(body,
          HTTPResponseCode.OK,
          current::net::constants::kDefaultContentType,
          current::net::http::Headers({{kSherlockHeaderCurrentStreamSize, size_str}}));
        return;
      }

      if (request_params.schema_requested) {
        const std::string& schema_format = request_params.schema_format;
        // Return the schema the user is requesting, in a top-level, or more fine-grained format.
        if (schema_format.empty()) {
          r(schema_as_http_response_);
        } else if (schema_format == "simple") {
          r(SubscribableSherlockSchema(
              schema_as_object_.type_id, schema_namespace_name_.entry_name, schema_namespace_name_.namespace_name));
        } else {
          const auto cit = schema_as_object_.language.find(schema_format);
          if (cit != schema_as_object_.language.end()) {
            r(cit->second);
          } else {
            SherlockSchemaFormatNotFound four_oh_four;
            four_oh_four.unsupported_format_requested = schema_format;
            r(four_oh_four, HTTPResponseCode.NotFound);
          }
        }
      } else {
        uint64_t begin_idx = 0u;
        std::chrono::microseconds from_timestamp(0);
        if (request_params.tail) {
          if (request_params.tail == static_cast<uint64_t>(-1)) {
            begin_idx = stream_size;
            request_params.tail = stream_size;
          } else {
            const uint64_t idx_by_tail = request_params.tail < stream_size ? (stream_size - request_params.tail) : 0u;
            begin_idx = std::max(request_params.i, idx_by_tail);
          }
        } else if (request_params.recent.count() > 0) {
          from_timestamp = r.timestamp - request_params.recent;
        } else if (request_params.since.count() > 0) {
          from_timestamp = request_params.since;
        } else {
          begin_idx = request_params.i;
        }

        if (from_timestamp.count() > 0) {
          const auto idx_by_timestamp =
              std::min(data.persistence.IndexRangeByTimestampRange(from_timestamp, std::chrono::microseconds(0)).first,
                       stream_size);
          begin_idx = std::max(begin_idx, idx_by_timestamp);
        }

        if (request_params.no_wait && begin_idx >= stream_size) {
          // Return "200 OK" if there is nothing to return now and we were asked to not wait for new entries.
          r("", HTTPResponseCode.OK);
          return;
        }

        const std::string subscription_id = data.GenerateRandomHTTPSubscriptionID();

        auto http_chunked_subscriber = std::make_unique<PubSubHTTPEndpoint<entry_t, PERSISTENCE_LAYER, J>>(
            subscription_id, scoped_data, std::move(r), std::move(request_params));

        // Acquire mutex before subscribing to the stream. Subscriber's thread will start with trying to lock
        // this mutex, thus ensuring that the corresponding `http_subscriptions` map entry will be initialized
        // prior to subscriber's thread starts its job.
        {
          std::lock_guard<std::mutex> lock(data.http_subscriptions->mutex);
          auto http_chunked_subscriber_scope =
              Subscribe(*http_chunked_subscriber,
                        begin_idx,
                        [&data, subscription_id]() {
                          // Launch the asynchronous task to destroy subscriber.
                          // This can not be done synchronously, since our lambda is called from within the
                          // subscriber's thread.
                          auto& http_subscriptions = data.http_subscriptions;
                          // `http_subscriptions` is captured by value, so the thread owns `shared_ptr`.
                          std::thread([http_subscriptions, subscription_id] {
                            // `done_callback()` is invoked by subscriber's thread in the locked section.
                            // This asynchronous thread will be able to acquire lock only after subscriber's
                            // thread is done and lock is released.
                            std::lock_guard<std::mutex> lock(http_subscriptions->mutex);
                            auto it = http_subscriptions->subscribers_map.find(subscription_id);
                            if (it != http_subscriptions->subscribers_map.end()) {
                              it->second.first = nullptr;
                              it->second.second = nullptr;
                              http_subscriptions->subscribers_map.erase(subscription_id);
                            }
                          }).detach();
                        });

          // TODO(dkorolev): This condition is to be rewritten correctly.
          if (!data.http_subscriptions->subscribers_map.count(subscription_id)) {
            data.http_subscriptions->subscribers_map.emplace(
              subscription_id,
              std::make_pair(std::make_unique<decltype(http_chunked_subscriber_scope)>(std::move(http_chunked_subscriber_scope)),
                             std::move(http_chunked_subscriber))
            );
          }
        }
      }
    } catch (const current::sync::InDestructingModeException&) {
      r("", HTTPResponseCode.ServiceUnavailable);
    }
  }

  void operator()(Request r) {
    if (r.url.query.has("json")) {
      const auto& json = r.url.query["json"];
      if (json == "js") {
        ServeDataViaHTTP<JSONFormat::Minimalistic>(std::move(r));
      } else if (json == "fs") {
        ServeDataViaHTTP<JSONFormat::NewtonsoftFSharp>(std::move(r));
      } else {
        r("The `?json` parameter is invalid, legal values are `js`, `fs`, or omit the parameter.\n",
          HTTPResponseCode.NotFound);
      }
    } else {
      ServeDataViaHTTP<JSONFormat::Current>(std::move(r));
    }
  }

  persistence_layer_t& Persister() { return own_data_.ObjectAccessorDespitePossiblyDestructing().persistence; }

 private:
  struct FillPerLanguageSchema {
    SherlockSchema& schema_ref;
    const ss::StreamNamespaceName& namespace_name;
    explicit FillPerLanguageSchema(SherlockSchema& schema, const ss::StreamNamespaceName& namespace_name)
        : schema_ref(schema), namespace_name(namespace_name) {}
    template <current::reflection::Language language>
    void PerLanguage() {
      schema_ref.language[current::ToString(language)] = schema_ref.type_schema.Describe<language>(
          current::reflection::NamespaceToExpose(namespace_name.namespace_name)
              .template AddType<entry_t>(namespace_name.entry_name));
    }
  };

  static SherlockSchema StaticConstructSchemaAsObject(const ss::StreamNamespaceName& namespace_name) {
    SherlockSchema schema;

    // TODO(dkorolev): `AsIdentifier` here?
    schema.type_name = current::reflection::CurrentTypeName<entry_t, current::reflection::NameFormat::Z>();
    schema.type_id =
        Value<current::reflection::ReflectedTypeBase>(current::reflection::Reflector().ReflectType<entry_t>()).type_id;

    reflection::StructSchema underlying_type_schema;
    underlying_type_schema.AddType<entry_t>();
    schema.type_schema = underlying_type_schema.GetSchemaInfo();

    current::reflection::ForEachLanguage(FillPerLanguageSchema(schema, namespace_name));

    return schema;
  }

  template <typename... ARGS>
  idxts_t PublishImpl(ARGS&&... args) {
    std::lock_guard<std::mutex> lock(publisher_mutex_);
    if (publisher_) {
      return publisher_->template Publish<current::locks::MutexLockStatus::AlreadyLocked>(std::forward<ARGS>(args)...);
    } else {
      CURRENT_THROW(PublishToStreamWithReleasedPublisherException());
    }
  }

  template <typename... ARGS>
  void UpdateHeadImpl(ARGS&&... args) {
    std::lock_guard<std::mutex> lock(publisher_mutex_);
    if (publisher_) {
      return publisher_->template UpdateHead<current::locks::MutexLockStatus::AlreadyLocked>(
          std::forward<ARGS>(args)...);
    } else {
      CURRENT_THROW(PublishToStreamWithReleasedPublisherException());
    }
  }

 private:
  const ss::StreamNamespaceName schema_namespace_name_ =
      ss::StreamNamespaceName(constants::kDefaultNamespaceName, constants::kDefaultTopLevelName);
  const SherlockSchema schema_as_object_;
  const Response schema_as_http_response_ = Response(JSON<JSONFormat::Minimalistic>(schema_as_object_),
                                                     HTTPResponseCode.OK,
                                                     current::net::constants::kDefaultJSONContentType);
  ScopeOwnedByMe<stream_data_t> own_data_;
  mutable std::mutex publisher_mutex_;
  std::unique_ptr<publisher_t> publisher_;
  StreamDataAuthority authority_;

  StreamImpl(const StreamImpl&) = delete;
  void operator=(const StreamImpl&) = delete;
};

template <typename ENTRY, template <typename> class PERSISTENCE_LAYER = DEFAULT_PERSISTENCE_LAYER>
using Stream = StreamImpl<ENTRY, PERSISTENCE_LAYER>;

// TODO(dkorolev) + TODO(mzhurovich): Shouldn't this be:
// using Stream = ss::StreamPublisher<StreamImpl<ENTRY, PERSISTENCE_LAYER>, ENTRY>;

}  // namespace sherlock
}  // namespace current

#endif  // CURRENT_SHERLOCK_SHERLOCK_H
