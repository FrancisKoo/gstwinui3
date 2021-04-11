#pragma once
#include "pch.h"

#include <gst/gst.h>

#include <condition_variable>
#include <mutex>

namespace winrt::GstWinUI3::implementation {
class GstHelper
{
public:
  static GstHelper* get(void)
  {
    static GstHelper* instance = new GstHelper();
    return instance;
  }

  bool initialized(void);

  ~GstHelper(void);

private:
  GstHelper(void);
  static gpointer threadFunc(gpointer userData);
  static gboolean threadRunningCb(gpointer userData);

  GThread* thread_;
  GMainLoop* loop_;
  GMainContext* ctx_;
  std::mutex lock_;
  std::condition_variable cond_;

  bool init_done_;
};
}
