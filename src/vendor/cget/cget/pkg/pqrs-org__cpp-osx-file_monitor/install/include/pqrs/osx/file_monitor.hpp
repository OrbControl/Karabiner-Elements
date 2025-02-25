#pragma once

// pqrs::osx::file_monitor v1.6

// (C) Copyright Takayama Fumihiko 2018.
// Distributed under the Boost Software License, Version 1.0.
// (See http://www.boost.org/LICENSE_1_0.txt)

// `pqrs::osx::file_monitor` can be used safely in a multi-threaded environment.

// Limitation:
//
// pqrs::osx::file_monitor signals `file_changed` slot just after the file is closed.
// Thus, we cannot use file_monitor to observe /var/log/xxx.log since
// these files are not closed while the owner process is running.

#include <CoreServices/CoreServices.h>
#include <nod/nod.hpp>
#include <pqrs/cf/array.hpp>
#include <pqrs/cf/string.hpp>
#include <pqrs/dispatcher.hpp>
#include <pqrs/filesystem.hpp>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace pqrs {
namespace osx {
class file_monitor final : public dispatcher::extra::dispatcher_client {
public:
  // Signals (invoked from the dispatcher thread)

  nod::signal<void(const std::string& changed_file_path, std::shared_ptr<std::vector<uint8_t>> changed_file_body)> file_changed;
  nod::signal<void(const std::string&)> error_occurred;

  // Methods

  file_monitor(std::weak_ptr<dispatcher::dispatcher> weak_dispatcher,
               const std::vector<std::string>& files) : dispatcher_client(weak_dispatcher),
                                                        files_(files),
                                                        directories_(cf::make_cf_mutable_array()),
                                                        stream_(nullptr) {
    queue_ = dispatch_queue_create("org.pqrs.osx.file_monitor", DISPATCH_QUEUE_SERIAL);

    std::unordered_set<std::string> directories;
    for (const auto& f : files) {
      directories.insert(filesystem::dirname(f));
    }

    if (directories_) {
      for (const auto& d : directories) {
        if (auto directory = cf::make_cf_string(d)) {
          CFArrayAppendValue(*directories_, *directory);
        }
      }
    }

    dispatch_sync(queue_, ^{
      impl::file_monitors_manager::insert(this);
    });
  }

  virtual ~file_monitor(void) {
    // Wrap with dispatch_sync to prevent file_monitor destruction while static_stream_callback is running.
    dispatch_sync(queue_, ^{
      impl::file_monitors_manager::erase(this);
    });

    dispatch_release(queue_);

    detach_from_dispatcher([this] {
      unregister_stream();
    });
  }

  void async_start(void) {
    enqueue_to_dispatcher([this] {
      register_stream();
    });
  }

  void enqueue_file_changed(const std::string& file_path) {
    enqueue_to_dispatcher([this, file_path] {
      auto it = file_bodies_.find(file_path);
      if (it != std::end(file_bodies_)) {
        auto changed_file_body = it->second;
        enqueue_to_dispatcher([this, file_path, changed_file_body] {
          file_changed(file_path,
                       changed_file_body);
        });
      }
    });
  }

  static std::shared_ptr<std::vector<uint8_t>> read_file(const std::string& path) {
    std::ifstream ifstream(path);
    if (ifstream) {
      ifstream.seekg(0, std::fstream::end);
      auto size = ifstream.tellg();
      ifstream.seekg(0, std::fstream::beg);

      auto buffer = std::make_shared<std::vector<uint8_t>>(size);
      ifstream.read(reinterpret_cast<char*>(&((*buffer)[0])), size);

      return buffer;
    }
    return nullptr;
  }

private:
  // This method is executed in the dispatcher thread.
  void register_stream(void) {
    // Skip if already started.

    if (stream_) {
      return;
    }

    if (!directories_) {
      return;
    }

    // ----------------------------------------
    // File System Events API does not call the callback if the root directory and files are moved at the same time.
    //
    // Example:
    //
    //   FSEventStreamCreate(... ,{"target/file1", "target/file2"})
    //
    //   $ mkdir target.new
    //   $ echo  target.new/file1
    //   $ mv    target.new target
    //
    //   In this case, the callback will not be called.
    //
    // Thus, we should signal manually once.

    for (const auto& file_path : files_) {
      auto [updated, file_body] = update_file_bodies(file_path);
      if (updated) {
        auto changed_file_body = file_body;
        enqueue_to_dispatcher([this, file_path, changed_file_body] {
          file_changed(file_path,
                       changed_file_body);
        });
      }
    }

    // ----------------------------------------

    FSEventStreamContext context{0};
    context.info = this;

    // kFSEventStreamCreateFlagWatchRoot and kFSEventStreamCreateFlagFileEvents are required in the following case.
    // (When directory == ~/.karabiner.d/configuration, file == ~/.karabiner.d/configuration/xxx.json)
    //
    // $ mkdir ~/.karabiner.d/configuration
    // $ touch ~/.karabiner.d/configuration/xxx.json
    // $ mv ~/.karabiner.d/configuration ~/.karabiner.d/configuration.back
    // $ ln -s ~/file-synchronisation-service/karabiner.d/configuration ~/.karabiner.d/
    // $ touch ~/.karabiner.d/configuration/xxx.json

    auto flags = FSEventStreamCreateFlags(0);
    flags |= kFSEventStreamCreateFlagWatchRoot;
    flags |= kFSEventStreamCreateFlagMarkSelf;
    flags |= kFSEventStreamCreateFlagFileEvents;

    stream_ = FSEventStreamCreate(kCFAllocatorDefault,
                                  static_stream_callback,
                                  &context,
                                  *directories_,
                                  kFSEventStreamEventIdSinceNow,
                                  0.1, // 100 ms
                                  flags);
    if (!stream_) {
      enqueue_to_dispatcher([this] {
        error_occurred("FSEventStreamCreate is failed.");
      });
    } else {
      FSEventStreamSetDispatchQueue(stream_,
                                    queue_);
      if (!FSEventStreamStart(stream_)) {
        enqueue_to_dispatcher([this] {
          error_occurred("FSEventStreamStart is failed.");
        });
      }
    }
  }

  // This method is executed in the dispatcher thread.
  void unregister_stream(void) {
    if (stream_) {
      FSEventStreamStop(stream_);
      FSEventStreamInvalidate(stream_);
      FSEventStreamRelease(stream_);
      stream_ = nullptr;
    }
  }

  struct fs_event {
    std::string file_path;
    FSEventStreamEventFlags flags;
  };

  static void static_stream_callback(ConstFSEventStreamRef stream,
                                     void* client_callback_info,
                                     size_t num_events,
                                     void* event_paths,
                                     const FSEventStreamEventFlags event_flags[],
                                     const FSEventStreamEventId event_ids[]) {
    auto self = reinterpret_cast<file_monitor*>(client_callback_info);
    if (!self) {
      return;
    }

    if (!impl::file_monitors_manager::alive(self)) {
      return;
    }

    auto fs_events = std::make_shared<std::vector<fs_event>>();

    auto paths = static_cast<const char**>(event_paths);
    for (size_t i = 0; i < num_events; ++i) {
      if (paths[i]) {
        fs_events->push_back({paths[i],
                              event_flags[i]});
      }
    }

    self->enqueue_to_dispatcher([self, fs_events] {
      self->stream_callback(fs_events);
    });
  }

  // This method is executed in the dispatcher thread.
  void stream_callback(std::shared_ptr<std::vector<fs_event>> fs_events) {
    for (auto e : *fs_events) {
      if (e.flags & (kFSEventStreamEventFlagRootChanged |
                     kFSEventStreamEventFlagKernelDropped |
                     kFSEventStreamEventFlagUserDropped)) {
        // re-register stream
        unregister_stream();
        register_stream();

      } else {
        // FSEvents passes realpathed file path to callback.
        // Thus, we should to convert it to file path in `files_`.

        std::optional<std::string> changed_file_path;

        if (auto realpath = filesystem::realpath(e.file_path)) {
          auto it = std::find_if(std::begin(files_),
                                 std::end(files_),
                                 [&](auto&& p) {
                                   return *realpath == filesystem::realpath(p);
                                 });
          if (it != std::end(files_)) {
            stream_file_paths_[e.file_path] = *it;
            changed_file_path = *it;
          }
        } else {
          // file_path might be removed.
          // (`realpath` fails if file does not exist.)

          auto it = stream_file_paths_.find(e.file_path);
          if (it != std::end(stream_file_paths_)) {
            changed_file_path = it->second;
            stream_file_paths_.erase(it);
          }
        }

        if (changed_file_path) {
          auto file_path = *changed_file_path;
          auto [updated, file_body] = update_file_bodies(file_path);
          if (updated) {
            bool own_event = e.flags & kFSEventStreamEventFlagOwnEvent;
            if (!own_event) {
              auto changed_file_body = file_body;
              enqueue_to_dispatcher([this, file_path, changed_file_body] {
                file_changed(file_path,
                             changed_file_body);
              });
            }
          }
        }
      }
    }
  }

  // This method is executed in the dispatcher thread.
  std::pair<bool, std::shared_ptr<std::vector<uint8_t>>> update_file_bodies(const std::string& file_path) {
    if (std::any_of(std::begin(files_),
                    std::end(files_),
                    [&](auto&& p) {
                      return file_path == p;
                    })) {
      auto file_body = read_file(file_path);
      auto it = file_bodies_.find(file_path);
      if (it != std::end(file_bodies_)) {
        if (it->second && file_body) {
          if (*(it->second) == *(file_body)) {
            // file_body is not changed
            return std::make_pair(false, nullptr);
          }
        } else if (!it->second && !file_body) {
          // file_body is not changed
          return std::make_pair(false, nullptr);
        }
      }
      file_bodies_[file_path] = file_body;

      return std::make_pair(true, file_body);
    }

    return std::make_pair(false, nullptr);
  }

  std::vector<std::string> files_;
  dispatch_queue_t queue_;
  cf::cf_ptr<CFMutableArrayRef> directories_;
  FSEventStreamRef stream_;
  // FSEventStreamEventPath -> file in files_
  // {
  //   "/Users/.../target/sub1/file1_1": "target/sub1/file1_1",
  //   "/Users/.../target/sub1/file1_2": "target/sub1/file1_2",
  // }
  std::unordered_map<std::string, std::string> stream_file_paths_;
  std::unordered_map<std::string, std::shared_ptr<std::vector<uint8_t>>> file_bodies_;

  class impl {
  public:
#include "impl/file_monitors_manager.hpp"
  };
};
} // namespace osx
} // namespace pqrs
