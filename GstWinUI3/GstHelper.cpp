#include "pch.h"

#include "GstHelper.h"

using namespace winrt;

G_BEGIN_DECLS

GST_PLUGIN_STATIC_DECLARE(winui3);

G_END_DECLS

namespace winrt::GstWinUI3::implementation {
GstHelper::GstHelper()
  : init_done_(false)
{
  GstRegistry* reg;
  int num_loaded = 0;

  reg = gst_registry_get();

  // Disable registry fork
  gst_registry_fork_set_enabled(FALSE);

  gst_init(nullptr, nullptr);
  static char* plugin_list[] = { { "gstcoreelements.dll" },
                                 { "gstd3d11.dll" },
                                 { "gstvideotestsrc.dll" } };

  for (int i = 0; i < G_N_ELEMENTS(plugin_list); i++) {
    GstPlugin* plugin = gst_plugin_load_file(plugin_list[i], nullptr);

    if (!plugin)
      continue;

    gst_registry_add_plugin(reg, plugin);
    gst_object_unref(plugin);

    num_loaded++;
  }

  init_done_ = num_loaded == G_N_ELEMENTS(plugin_list);

  GST_PLUGIN_STATIC_REGISTER(winui3);

  ctx_ = g_main_context_default();
  loop_ = g_main_loop_new(ctx_, FALSE);

  // This thread will be our loop for default main context
  thread_ = g_thread_new(
    "GstHelperThread", (GThreadFunc)GstHelper::threadFunc, (gpointer)this);

  std::unique_lock<std::mutex> Lock(lock_);
  while (!g_main_loop_is_running(loop_))
    cond_.wait(Lock);
};

GstHelper::~GstHelper()
{
  g_main_loop_quit(loop_);
  g_thread_join(thread_);
  g_main_loop_unref(loop_);
};

bool
GstHelper::initialized(void)
{
  return init_done_;
}

gboolean
GstHelper::threadRunningCb(gpointer userData)
{
  GstHelper* self = static_cast<GstHelper*>(userData);

  std::lock_guard<std::mutex> Lock(self->lock_);
  self->cond_.notify_one();

  return G_SOURCE_REMOVE;
}

gpointer
GstHelper::threadFunc(gpointer userData)
{
  GstHelper* self = static_cast<GstHelper*>(userData);
  GSource* source;

  g_main_context_push_thread_default(self->ctx_);
  source = g_idle_source_new();
  g_source_set_callback(
    source, (GSourceFunc)GstHelper::threadRunningCb, (gpointer)self, nullptr);
  g_source_attach(source, self->ctx_);
  g_source_unref(source);

  g_main_loop_run(self->loop_);

  g_main_context_pop_thread_default(self->ctx_);

  return nullptr;
}
}
