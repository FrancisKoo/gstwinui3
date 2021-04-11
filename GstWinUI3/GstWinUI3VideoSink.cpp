#include "pch.h"

#include "GstWinUI3VideoSink.h"

enum
{
  PROP_0,
  PROP_ADAPTER,
};

using namespace winrt::GstWinUI3::implementation;
using namespace winrt::Microsoft::UI::Xaml::Controls;

#define DEFAULT_ADAPTER -1
#define DEFAULT_FORCE_ASPECT_RATIO TRUE

#define GST_WIN_UI_SINK_CAPS_SYSTEM_MEMORY                                     \
  GST_VIDEO_CAPS_MAKE(GST_D3D11_SINK_FORMATS)                                  \
  ";" GST_VIDEO_CAPS_MAKE_WITH_FEATURES(                                       \
    GST_CAPS_FEATURE_MEMORY_SYSTEM_MEMORY                                      \
    "," GST_CAPS_FEATURE_META_GST_VIDEO_OVERLAY_COMPOSITION,                   \
    GST_D3D11_SINK_FORMATS)

#define GST_WIN_UI_SINK_CAPS_D3D11                                             \
  GST_VIDEO_CAPS_MAKE_WITH_FEATURES(GST_CAPS_FEATURE_MEMORY_D3D11_MEMORY,      \
                                    GST_D3D11_SINK_FORMATS)                    \
  ";" GST_VIDEO_CAPS_MAKE_WITH_FEATURES(                                       \
    GST_CAPS_FEATURE_MEMORY_D3D11_MEMORY                                       \
    "," GST_CAPS_FEATURE_META_GST_VIDEO_OVERLAY_COMPOSITION,                   \
    GST_D3D11_SINK_FORMATS)

#define GST_WIN_UI_SINK_CAPS                                                   \
  GST_WIN_UI_SINK_CAPS_SYSTEM_MEMORY ";" GST_WIN_UI_SINK_CAPS_D3D11

static GstStaticPadTemplate sink_template =
  GST_STATIC_PAD_TEMPLATE("sink",
                          GST_PAD_SINK,
                          GST_PAD_ALWAYS,
                          GST_STATIC_CAPS(GST_WIN_UI_SINK_CAPS));

GST_DEBUG_CATEGORY_STATIC(win_ui_video_sink_debug);
#define GST_CAT_DEFAULT win_ui_video_sink_debug

typedef struct _GstWinUI3VideoSinkInner
{
  SwapChainPanel panel;
  GstWinUI3Window* window;
} GstWinUI3VideoSinkInner;

struct _GstWinUI3VideoSink
{
  GstVideoSink parent;
  GstWinUI3VideoSinkInner* inner;

  GstD3D11Device* device;
  GstVideoInfo info;

  gint adapter;
  gboolean force_aspect_ratio;

  GstBufferPool* fallback_pool;
  gboolean have_video_processor;
  gboolean processor_in_use;
};

static void
gst_win_ui3_video_sink_set_property(GObject* object,
                                    guint prop_id,
                                    const GValue* value,
                                    GParamSpec* pspec);

static void
gst_win_ui3_video_sink_get_property(GObject* object,
                                    guint prop_id,
                                    GValue* value,
                                    GParamSpec* pspec);

static void
gst_win_ui3_video_sink_dispose(GObject* object);

static void
gst_win_ui3_video_sink_finalize(GObject* object);

static void
gst_win_ui3_video_sink_set_context(GstElement* element, GstContext* context);

static gboolean
gst_win_ui3_video_sink_set_caps(GstBaseSink* sink, GstCaps* caps);

static gboolean
gst_win_ui3_video_sink_start(GstBaseSink* sink);

static gboolean
gst_win_ui3_video_sink_stop(GstBaseSink* sink);

static gboolean
gst_win_ui3_video_sink_propose_allocation(GstBaseSink* sink, GstQuery* query);

static gboolean
gst_win_ui3_video_sink_query(GstBaseSink* sink, GstQuery* query);

static GstFlowReturn
gst_win_ui3_video_sink_show_frame(GstVideoSink* sink, GstBuffer* buf);

#define gst_win_ui3_video_sink_parent_class parent_class
G_DEFINE_TYPE_WITH_CODE(GstWinUI3VideoSink,
                        gst_win_ui3_video_sink,
                        GST_TYPE_VIDEO_SINK,
                        GST_DEBUG_CATEGORY_INIT(win_ui_video_sink_debug,
                                                "winui3videosink",
                                                0,
                                                "winui3videosink"));

static void
gst_win_ui3_video_sink_class_init(GstWinUI3VideoSinkClass* klass)
{
  GObjectClass* gobject_class = G_OBJECT_CLASS(klass);
  GstElementClass* element_class = GST_ELEMENT_CLASS(klass);
  GstBaseSinkClass* basesink_class = GST_BASE_SINK_CLASS(klass);
  GstVideoSinkClass* videosink_class = GST_VIDEO_SINK_CLASS(klass);

  gobject_class->set_property = gst_win_ui3_video_sink_set_property;
  gobject_class->get_property = gst_win_ui3_video_sink_get_property;
  gobject_class->dispose = gst_win_ui3_video_sink_dispose;
  gobject_class->finalize = gst_win_ui3_video_sink_finalize;

  g_object_class_install_property(
    gobject_class,
    PROP_ADAPTER,
    g_param_spec_int("adapter",
                     "Adapter",
                     "Adapter index for creating device (-1 for default)",
                     -1,
                     G_MAXINT32,
                     DEFAULT_ADAPTER,
                     (GParamFlags)(G_PARAM_READWRITE | GST_PARAM_MUTABLE_READY |
                                   G_PARAM_STATIC_STRINGS)));

  element_class->set_context =
    GST_DEBUG_FUNCPTR(gst_win_ui3_video_sink_set_context);

  gst_element_class_set_static_metadata(
    element_class,
    "WinUI3 Video Sink",
    "Sink/Video",
    "Video Sink for WinUI3",
    "Seungha Yang <seungha@centricular.com>");

  gst_element_class_add_static_pad_template(element_class, &sink_template);

  basesink_class->set_caps = GST_DEBUG_FUNCPTR(gst_win_ui3_video_sink_set_caps);
  basesink_class->start = GST_DEBUG_FUNCPTR(gst_win_ui3_video_sink_start);
  basesink_class->stop = GST_DEBUG_FUNCPTR(gst_win_ui3_video_sink_stop);
  basesink_class->propose_allocation =
    GST_DEBUG_FUNCPTR(gst_win_ui3_video_sink_propose_allocation);
  basesink_class->query = GST_DEBUG_FUNCPTR(gst_win_ui3_video_sink_query);

  videosink_class->show_frame =
    GST_DEBUG_FUNCPTR(gst_win_ui3_video_sink_show_frame);
}

static void
gst_win_ui3_video_sink_init(GstWinUI3VideoSink* self)
{
  self->adapter = DEFAULT_ADAPTER;
  self->force_aspect_ratio = DEFAULT_FORCE_ASPECT_RATIO;
  self->inner = new GstWinUI3VideoSinkInner;
  self->inner->panel = nullptr;
  self->inner->window = nullptr;
}

static void
gst_win_ui3_video_sink_set_property(GObject* object,
                                    guint prop_id,
                                    const GValue* value,
                                    GParamSpec* pspec)
{
  GstWinUI3VideoSink* self = GST_WIN_UI3_VIDEO_SINK(object);

  GST_OBJECT_LOCK(self);
  switch (prop_id) {
    case PROP_ADAPTER:
      self->adapter = g_value_get_int(value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
      break;
  }
  GST_OBJECT_UNLOCK(self);
}

static void
gst_win_ui3_video_sink_get_property(GObject* object,
                                    guint prop_id,
                                    GValue* value,
                                    GParamSpec* pspec)
{
  GstWinUI3VideoSink* self = GST_WIN_UI3_VIDEO_SINK(object);

  switch (prop_id) {
    case PROP_ADAPTER:
      g_value_set_int(value, self->adapter);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
      break;
  }
}

static void
gst_win_ui3_video_sink_dispose(GObject* object)
{
  GstWinUI3VideoSink* self = GST_WIN_UI3_VIDEO_SINK(object);

  if (self->inner->window) {
    delete self->inner->window;
    self->inner->window = nullptr;
  }

  g_clear_object(&self->device);

  G_OBJECT_CLASS(parent_class)->dispose(object);
}

static void
gst_win_ui3_video_sink_finalize(GObject* object)
{
  GstWinUI3VideoSink* self = GST_WIN_UI3_VIDEO_SINK(object);

  delete self->inner;

  G_OBJECT_CLASS(parent_class)->finalize(object);
}

static void
gst_win_ui3_video_sink_set_context(GstElement* element, GstContext* context)
{
  GstWinUI3VideoSink* self = GST_WIN_UI3_VIDEO_SINK(element);

  gst_d3d11_handle_set_context(element, context, self->adapter, &self->device);

  GST_ELEMENT_CLASS(parent_class)->set_context(element, context);
}

static gboolean
gst_win_ui3_video_sink_prepare_window(GstWinUI3VideoSink* self)
{
  if (self->inner->window)
    return TRUE;

  if (!self->inner->panel)
    GST_ERROR_OBJECT(self, "Required SwapChainPanel handle is missing");

  self->inner->window = new GstWinUI3Window(self->inner->panel, self->device);

  return TRUE;
}

static gboolean
gst_win_ui3_video_sink_set_caps(GstBaseSink* sink, GstCaps* caps)
{
  GstWinUI3VideoSink* self = GST_WIN_UI3_VIDEO_SINK(sink);

  GST_DEBUG_OBJECT(self, "set caps %" GST_PTR_FORMAT, caps);

  if (!gst_win_ui3_video_sink_prepare_window(self))
    return FALSE;

  if (!gst_video_info_from_caps(&self->info, caps)) {
    GST_DEBUG_OBJECT(
      sink, "Could not locate image format from caps %" GST_PTR_FORMAT, caps);
    return FALSE;
  }

  self->have_video_processor = FALSE;
  if (!self->inner->window->Prepare(caps, &self->have_video_processor)) {
    GST_ELEMENT_ERROR(
      sink, RESOURCE, NOT_FOUND, (nullptr), ("Failed to prepare panel"));
    return FALSE;
  }

  if (self->fallback_pool) {
    gst_buffer_pool_set_active(self->fallback_pool, FALSE);
    gst_clear_object(&self->fallback_pool);
  }

  GstD3D11AllocationParams* d3d11_params;
  gint bind_flags = D3D11_BIND_SHADER_RESOURCE;

  if (self->have_video_processor) {
    /* To create video processor input view, one of following bind flags
     * is required
     * NOTE: Any texture arrays which were created with D3D11_BIND_DECODER
     * flag cannot be used for shader input.
     *
     * D3D11_BIND_DECODER
     * D3D11_BIND_VIDEO_ENCODER
     * D3D11_BIND_RENDER_TARGET
     * D3D11_BIND_UNORDERED_ACCESS_VIEW
     */
    bind_flags |= D3D11_BIND_RENDER_TARGET;
  }

  d3d11_params = gst_d3d11_allocation_params_new(
    self->device, &self->info, (GstD3D11AllocationFlags)0, bind_flags);

  self->fallback_pool = gst_d3d11_buffer_pool_new(self->device);

  GstStructure* config = gst_buffer_pool_get_config(self->fallback_pool);
  gst_buffer_pool_config_set_params(
    config, caps, (guint)GST_VIDEO_INFO_SIZE(&self->info), 2, 0);
  gst_buffer_pool_config_set_d3d11_allocation_params(config, d3d11_params);
  gst_d3d11_allocation_params_free(d3d11_params);
  gst_buffer_pool_config_add_option(config, GST_BUFFER_POOL_OPTION_VIDEO_META);
  gst_buffer_pool_set_config(self->fallback_pool, config);

  self->processor_in_use = FALSE;

  return TRUE;
}

static gboolean
gst_win_ui3_video_sink_start(GstBaseSink* sink)
{
  GstWinUI3VideoSink* self = GST_WIN_UI3_VIDEO_SINK(sink);

  if (!gst_d3d11_ensure_element_data(
        GST_ELEMENT_CAST(self), self->adapter, &self->device)) {
    GST_ERROR_OBJECT(sink, "Cannot create d3d11device");
    return FALSE;
  }

  return TRUE;
}

static gboolean
gst_win_ui3_video_sink_stop(GstBaseSink* sink)
{
  GstWinUI3VideoSink* self = GST_WIN_UI3_VIDEO_SINK(sink);

  if (self->fallback_pool) {
    gst_buffer_pool_set_active(self->fallback_pool, FALSE);
    gst_object_unref(self->fallback_pool);
    self->fallback_pool = nullptr;
  }

  if (self->inner->window) {
    self->inner->window->Unprepare();
    delete self->inner->window;
    self->inner->window = nullptr;
  }

  gst_clear_object(&self->device);

  return TRUE;
}

static gboolean
gst_win_ui3_video_sink_propose_allocation(GstBaseSink* sink, GstQuery* query)
{
  GstWinUI3VideoSink* self = GST_WIN_UI3_VIDEO_SINK(sink);
  GstCaps* caps = nullptr;
  GstBufferPool* pool = nullptr;
  GstVideoInfo info;
  gsize size;
  gboolean need_pool;

  if (!self->device || !self->inner->window)
    return FALSE;

  gst_query_parse_allocation(query, &caps, &need_pool);

  if (!caps) {
    GST_WARNING_OBJECT(self, "no caps specified");
    return FALSE;
  }

  if (!gst_video_info_from_caps(&info, caps)) {
    GST_WARNING_OBJECT(self, "invalid caps specified");
    return FALSE;
  }

  /* the normal size of a frame */
  size = info.size;

  if (need_pool) {
    GstCapsFeatures* features;
    GstStructure* config;
    bool is_d3d11 = false;

    features = gst_caps_get_features(caps, 0);
    if (features && gst_caps_features_contains(
                      features, GST_CAPS_FEATURE_MEMORY_D3D11_MEMORY)) {
      GST_DEBUG_OBJECT(self, "upstream support d3d11 memory");
      pool = gst_d3d11_buffer_pool_new(self->device);
      is_d3d11 = true;
    } else {
      pool = gst_video_buffer_pool_new();
    }

    config = gst_buffer_pool_get_config(pool);
    gst_buffer_pool_config_add_option(config,
                                      GST_BUFFER_POOL_OPTION_VIDEO_META);
    /* d3d11 pool does not support video alignment */
    if (!is_d3d11) {
      gst_buffer_pool_config_add_option(config,
                                        GST_BUFFER_POOL_OPTION_VIDEO_ALIGNMENT);
    }

    size = GST_VIDEO_INFO_SIZE(&info);
    if (is_d3d11) {
      GstD3D11AllocationParams* d3d11_params;

      d3d11_params =
        gst_d3d11_allocation_params_new(self->device,
                                        &info,
                                        (GstD3D11AllocationFlags)0,
                                        D3D11_BIND_SHADER_RESOURCE);

      gst_buffer_pool_config_set_d3d11_allocation_params(config, d3d11_params);
      gst_d3d11_allocation_params_free(d3d11_params);
    }

    gst_buffer_pool_config_set_params(config, caps, (guint)size, 2, 0);

    if (!gst_buffer_pool_set_config(pool, config)) {
      GST_ERROR_OBJECT(pool, "Couldn't set config");
      gst_object_unref(pool);

      return FALSE;
    }

    // gst_buffer_pool_set_config() will update size depending on staging
    // texture size
    if (is_d3d11)
      size = GST_D3D11_BUFFER_POOL(pool)->buffer_size;
  }

  gst_query_add_allocation_pool(query, pool, (guint)size, 2, 0);
  if (pool)
    g_object_unref(pool);

  gst_query_add_allocation_meta(query, GST_VIDEO_META_API_TYPE, nullptr);
  gst_query_add_allocation_meta(
    query, GST_VIDEO_OVERLAY_COMPOSITION_META_API_TYPE, nullptr);

  return TRUE;
}

static gboolean
gst_win_ui3_video_sink_query(GstBaseSink* sink, GstQuery* query)
{
  GstWinUI3VideoSink* self = GST_WIN_UI3_VIDEO_SINK(sink);

  switch (GST_QUERY_TYPE(query)) {
    case GST_QUERY_CONTEXT:
      if (gst_d3d11_handle_context_query(
            GST_ELEMENT(self), query, self->device)) {
        return TRUE;
      }
      break;
    default:
      break;
  }

  return GST_BASE_SINK_CLASS(parent_class)->query(sink, query);
}

static gboolean
gst_win_ui3_video_sink_upload_frame(GstWinUI3VideoSink* self,
                                    GstBuffer* inbuf,
                                    GstBuffer* outbuf)
{
  GstVideoFrame in_frame, out_frame;
  gboolean ret;

  GST_LOG_OBJECT(self, "Copy to fallback buffer");

  if (!gst_video_frame_map(
        &in_frame,
        &self->info,
        inbuf,
        (GstMapFlags)(GST_MAP_READ | GST_VIDEO_FRAME_MAP_FLAG_NO_REF)))
    goto invalid_buffer;

  if (!gst_video_frame_map(
        &out_frame,
        &self->info,
        outbuf,
        (GstMapFlags)(GST_MAP_WRITE | GST_VIDEO_FRAME_MAP_FLAG_NO_REF))) {
    gst_video_frame_unmap(&in_frame);
    goto invalid_buffer;
  }

  ret = gst_video_frame_copy(&out_frame, &in_frame);

  gst_video_frame_unmap(&in_frame);
  gst_video_frame_unmap(&out_frame);

  return ret;

  /* ERRORS */
invalid_buffer : {
  GST_ELEMENT_WARNING(
    self, CORE, NOT_IMPLEMENTED, (nullptr), ("invalid video buffer received"));
  return FALSE;
}
}

static gboolean
gst_win_ui3_video_sink_copy_d3d11_to_d3d11(GstWinUI3VideoSink* self,
                                           GstBuffer* inbuf,
                                           GstBuffer* outbuf)
{
  GST_LOG_OBJECT(self, "Copy to fallback buffer using device memory copy");

  // Shouldn't happen
  if (gst_buffer_n_memory(inbuf) != gst_buffer_n_memory(outbuf))
    return gst_win_ui3_video_sink_upload_frame(self, inbuf, outbuf);

  for (guint i = 0; i < gst_buffer_n_memory(inbuf); i++) {
    GstMemory *dst_mem, *src_mem;
    GstD3D11Memory *dst_dmem, *src_dmem;
    GstMapInfo dst_info;
    GstMapInfo src_info;
    ID3D11Resource *dst_texture, *src_texture;
    ID3D11DeviceContext* device_context;
    D3D11_BOX src_box = {
      0,
    };
    D3D11_TEXTURE2D_DESC dst_desc, src_desc;
    guint dst_subidx, src_subidx;

    dst_mem = gst_buffer_peek_memory(outbuf, i);
    src_mem = gst_buffer_peek_memory(inbuf, i);

    dst_dmem = GST_D3D11_MEMORY_CAST(dst_mem);
    src_dmem = GST_D3D11_MEMORY_CAST(src_mem);

    gst_d3d11_memory_get_texture_desc(dst_dmem, &dst_desc);
    gst_d3d11_memory_get_texture_desc(src_dmem, &src_desc);

    if (dst_desc.Format != src_desc.Format) {
      GST_WARNING("different dxgi format");
      return FALSE;
    }

    device_context = gst_d3d11_device_get_device_context_handle(self->device);

    if (!gst_memory_map(
          dst_mem, &dst_info, (GstMapFlags)(GST_MAP_WRITE | GST_MAP_D3D11))) {
      GST_ERROR("Cannot map dst d3d11 memory");
      return FALSE;
    }

    if (!gst_memory_map(
          src_mem, &src_info, (GstMapFlags)(GST_MAP_READ | GST_MAP_D3D11))) {
      GST_ERROR("Cannot map src d3d11 memory");
      gst_memory_unmap(dst_mem, &dst_info);
      return FALSE;
    }

    dst_texture = (ID3D11Resource*)dst_info.data;
    src_texture = (ID3D11Resource*)src_info.data;

    /* src/dst texture size might be different if padding was used.
     * select smaller size */
    src_box.left = 0;
    src_box.top = 0;
    src_box.front = 0;
    src_box.back = 1;
    src_box.right = MIN(src_desc.Width, dst_desc.Width);
    src_box.bottom = MIN(src_desc.Height, dst_desc.Height);

    dst_subidx = gst_d3d11_memory_get_subresource_index(dst_dmem);
    src_subidx = gst_d3d11_memory_get_subresource_index(src_dmem);

    gst_d3d11_device_lock(self->device);
    device_context->CopySubresourceRegion(
      dst_texture, dst_subidx, 0, 0, 0, src_texture, src_subidx, &src_box);
    gst_d3d11_device_unlock(self->device);

    gst_memory_unmap(src_mem, &src_info);
    gst_memory_unmap(dst_mem, &dst_info);
  }

  return TRUE;
}

static gboolean
gst_win_ui3_video_sink_get_fallback_buffer(GstWinUI3VideoSink* self,
                                           GstBuffer* inbuf,
                                           GstBuffer** fallback_buf,
                                           gboolean device_copy)
{
  GstBuffer* outbuf = nullptr;
  GstVideoOverlayCompositionMeta* compo_meta;

  if (!self->fallback_pool ||
      !gst_buffer_pool_set_active(self->fallback_pool, TRUE) ||
      gst_buffer_pool_acquire_buffer(self->fallback_pool, &outbuf, nullptr) !=
        GST_FLOW_OK) {
    GST_ERROR_OBJECT(self, "fallback pool is unavailable");
    return FALSE;
  }

  if (device_copy) {
    if (!gst_win_ui3_video_sink_copy_d3d11_to_d3d11(self, inbuf, outbuf)) {
      GST_ERROR_OBJECT(self, "cannot copy frame");
      goto error;
    }
  } else if (!gst_win_ui3_video_sink_upload_frame(self, inbuf, outbuf)) {
    GST_ERROR_OBJECT(self, "cannot upload frame");
    goto error;
  }

  /* Copy overlaycomposition meta if any */
  compo_meta = gst_buffer_get_video_overlay_composition_meta(inbuf);
  if (compo_meta)
    gst_buffer_add_video_overlay_composition_meta(outbuf, compo_meta->overlay);

  *fallback_buf = outbuf;

  return TRUE;

error:
  gst_buffer_unref(outbuf);
  return FALSE;
}

static gboolean
gst_win_ui3_video_sink_is_d3d11_buffer(GstWinUI3VideoSink* self, GstBuffer* buf)
{
  for (guint i = 0; i < gst_buffer_n_memory(buf); i++) {
    GstMemory* mem = gst_buffer_peek_memory(buf, i);

    if (!gst_is_d3d11_memory(mem))
      return FALSE;

    GstD3D11Memory* dmem = GST_D3D11_MEMORY_CAST(mem);
    // Different device, we need to copy them
    if (dmem->device != self->device)
      return FALSE;
  }

  return TRUE;
}

static gboolean
gst_win_ui3_video_sink_ensure_srv(GstWinUI3VideoSink* self, GstBuffer* buf)
{
  if (!gst_win_ui3_video_sink_is_d3d11_buffer(self, buf))
    return FALSE;

  for (guint i = 0; i < gst_buffer_n_memory(buf); i++) {
    GstD3D11Memory* mem = (GstD3D11Memory*)gst_buffer_peek_memory(buf, i);
    if (!gst_d3d11_memory_get_shader_resource_view_size(mem))
      return FALSE;
  }

  return TRUE;
}

static GstFlowReturn
gst_win_ui3_video_sink_show_frame(GstVideoSink* sink, GstBuffer* buf)
{
  GstWinUI3VideoSink* self = GST_WIN_UI3_VIDEO_SINK(sink);
  GstFlowReturn ret = GST_FLOW_OK;
  GstBuffer* fallback_buf = nullptr;

  if (!gst_win_ui3_video_sink_is_d3d11_buffer(self, buf)) {
    GST_LOG_OBJECT(self, "Need fallback buffer");

    if (!gst_win_ui3_video_sink_get_fallback_buffer(
          self, buf, &fallback_buf, FALSE)) {
      return GST_FLOW_ERROR;
    }
  } else {
    gboolean direct_rendering = FALSE;

    /* Check if we can use video processor for conversion */
    if (gst_buffer_n_memory(buf) == 1 && self->have_video_processor) {
      GstD3D11Memory* mem = (GstD3D11Memory*)gst_buffer_peek_memory(buf, 0);
      D3D11_TEXTURE2D_DESC desc;

      gst_d3d11_memory_get_texture_desc(mem, &desc);
      if ((desc.BindFlags & D3D11_BIND_DECODER) == D3D11_BIND_DECODER) {
        GST_TRACE_OBJECT(
          self, "Got VideoProcessor compatible texture, do direct rendering");
        direct_rendering = TRUE;
        self->processor_in_use = TRUE;
      } else if (self->processor_in_use &&
                 (desc.BindFlags & D3D11_BIND_RENDER_TARGET) ==
                   D3D11_BIND_RENDER_TARGET) {
        direct_rendering = TRUE;
      }
    }

    /* Or, SRV should be available */
    if (!direct_rendering) {
      if (gst_win_ui3_video_sink_ensure_srv(self, buf)) {
        GST_TRACE_OBJECT(self, "SRV is available, do direct rendering");
        direct_rendering = TRUE;
      }
    }

    if (!direct_rendering && !gst_win_ui3_video_sink_get_fallback_buffer(
                               self, buf, &fallback_buf, TRUE)) {
      return GST_FLOW_ERROR;
    }
  }

  ret = self->inner->window->Present(fallback_buf ? fallback_buf : buf);

  gst_clear_buffer(&fallback_buf);

  return ret;
}

gboolean
gst_win_ui3_video_sink_set_panel(GstWinUI3VideoSink* sink,
                                 SwapChainPanel const& panel)
{
  g_return_val_if_fail(GST_IS_WIN_UI3_VIDEO_SINK(sink), FALSE);

  sink->inner->panel = panel;

  return TRUE;
}

static gboolean
plugin_init(GstPlugin* plugin)
{
  return gst_element_register(plugin,
                              "winui3videosink",
                              GST_RANK_PRIMARY + 256,
                              GST_TYPE_WIN_UI3_VIDEO_SINK);
}

#define PACKAGE "GstWinUI3"

GST_PLUGIN_DEFINE(GST_VERSION_MAJOR,
                  GST_VERSION_MINOR,
                  winui3,
                  "WinUI3 plugin",
                  plugin_init,
                  "0.1",
                  "MIT/X11",
                  "gstwinui3",
                  "http://centricular.com")
