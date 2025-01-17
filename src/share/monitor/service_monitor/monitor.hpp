#pragma once

#include "boost_defs.hpp"

#include "dispatcher.hpp"
#include "logger.hpp"
#include "services.hpp"
#include <boost/signals2.hpp>

namespace krbn {
namespace monitor {
namespace service_monitor {
class monitor final : pqrs::dispatcher::extra::dispatcher_client {
public:
  // Signals (invoked from the shared dispatcher thread)

  boost::signals2::signal<void(std::shared_ptr<services>)> service_detected;
  boost::signals2::signal<void(std::shared_ptr<services>)> service_removed;

  // Methods

  monitor(const monitor&) = delete;

  monitor(CFDictionaryRef _Nonnull matching_dictionary) : matching_dictionary_(matching_dictionary),
                                                          notification_port_(nullptr),
                                                          matched_notification_(IO_OBJECT_NULL),
                                                          terminated_notification_(IO_OBJECT_NULL) {
    if (matching_dictionary_) {
      CFRetain(matching_dictionary_);
    }
  }

  virtual ~monitor(void) {
    detach_from_dispatcher([this] {
      stop();
    });

    if (matching_dictionary_) {
      CFRelease(matching_dictionary_);
    }
  }

  void async_start(void) {
    enqueue_to_dispatcher([this] {
      start();
    });
  }

  void async_stop(void) {
    enqueue_to_dispatcher([this] {
      stop();
    });
  }

  void async_invoke_service_detected(void) {
    enqueue_to_dispatcher([this] {
      if (matching_dictionary_) {
        CFRetain(matching_dictionary_);
        io_iterator_t it;
        auto kr = IOServiceGetMatchingServices(kIOMasterPortDefault, matching_dictionary_, &it);
        if (kr != KERN_SUCCESS) {
          logger::get_logger().error("IOServiceGetMatchingServices error: {1} @ {0}", __PRETTY_FUNCTION__, kr);
        } else {
          matched_callback(it);
          IOObjectRelease(it);
        }
      }
    });
  }

private:
  // This method is executed in the dispatcher thread.
  void start(void) {
    if (!notification_port_) {
      notification_port_ = IONotificationPortCreate(kIOMasterPortDefault);
      if (!notification_port_) {
        logger::get_logger().error("IONotificationPortCreate is failed @ {0}", __PRETTY_FUNCTION__);
        return;
      }

      if (auto loop_source = IONotificationPortGetRunLoopSource(notification_port_)) {
        CFRunLoopAddSource(CFRunLoopGetMain(), loop_source, kCFRunLoopCommonModes);
      } else {
        logger::get_logger().error("IONotificationPortGetRunLoopSource is failed @ {0}", __PRETTY_FUNCTION__);
      }
    }

    // kIOMatchedNotification

    if (!matched_notification_) {
      if (matching_dictionary_) {
        CFRetain(matching_dictionary_);
        auto kr = IOServiceAddMatchingNotification(notification_port_,
                                                   kIOMatchedNotification,
                                                   matching_dictionary_,
                                                   &(monitor::static_matched_callback),
                                                   static_cast<void*>(this),
                                                   &matched_notification_);
        if (kr != kIOReturnSuccess) {
          logger::get_logger().error("IOServiceAddMatchingNotification error: {1} @ {0}", __PRETTY_FUNCTION__, kr);
          CFRelease(matching_dictionary_);
        } else {
          matched_callback(matched_notification_);
        }
      }
    }

    // kIOTerminatedNotification

    if (!terminated_notification_) {
      if (matching_dictionary_) {
        CFRetain(matching_dictionary_);
        auto kr = IOServiceAddMatchingNotification(notification_port_,
                                                   kIOTerminatedNotification,
                                                   matching_dictionary_,
                                                   &(monitor::static_terminated_callback),
                                                   static_cast<void*>(this),
                                                   &terminated_notification_);
        if (kr != kIOReturnSuccess) {
          logger::get_logger().error("IOServiceAddMatchingNotification error: {1} @ {0}", __PRETTY_FUNCTION__, kr);
          CFRelease(matching_dictionary_);
        } else {
          terminated_callback(terminated_notification_);
        }
      }
    }
  }

  // This method is executed in the dispatcher thread.
  void stop(void) {
    if (matched_notification_) {
      IOObjectRelease(matched_notification_);
      matched_notification_ = IO_OBJECT_NULL;
    }

    if (terminated_notification_) {
      IOObjectRelease(terminated_notification_);
      terminated_notification_ = IO_OBJECT_NULL;
    }

    if (notification_port_) {
      if (auto loop_source = IONotificationPortGetRunLoopSource(notification_port_)) {
        CFRunLoopRemoveSource(CFRunLoopGetMain(), loop_source, kCFRunLoopCommonModes);
      }

      IONotificationPortDestroy(notification_port_);
      notification_port_ = nullptr;
    }
  }

  static void static_matched_callback(void* _Nonnull refcon, io_iterator_t iterator) {
    auto self = static_cast<monitor*>(refcon);
    if (!self) {
      return;
    }

    self->matched_callback(iterator);
  }

  void matched_callback(io_iterator_t iterator) {
    auto s = std::make_shared<services>(iterator);
    enqueue_to_dispatcher([this, s] {
      service_detected(s);
    });
  }

  static void static_terminated_callback(void* _Nonnull refcon, io_iterator_t iterator) {
    auto self = static_cast<monitor*>(refcon);
    if (!self) {
      return;
    }

    self->terminated_callback(iterator);
  }

  void terminated_callback(io_iterator_t iterator) {
    auto s = std::make_shared<services>(iterator);
    enqueue_to_dispatcher([this, s] {
      service_removed(s);
    });
  }

  CFDictionaryRef _Nonnull matching_dictionary_;

  IONotificationPortRef _Nullable notification_port_;
  io_iterator_t matched_notification_;
  io_iterator_t terminated_notification_;
};
} // namespace service_monitor
} // namespace monitor
} // namespace krbn
