#pragma once

#include "pch.h"

#include <gst/video/gstvideosink.h>
#include <gst/video/video.h>

#include "GstWinUI3Window.h"

G_BEGIN_DECLS

#define GST_TYPE_WIN_UI3_VIDEO_SINK (gst_win_ui3_video_sink_get_type())
G_DECLARE_FINAL_TYPE(GstWinUI3VideoSink,
                     gst_win_ui3_video_sink,
                     GST,
                     WIN_UI3_VIDEO_SINK,
                     GstVideoSink);

G_END_DECLS

gboolean
gst_win_ui3_video_sink_set_panel(
  GstWinUI3VideoSink* sink,
  winrt::Microsoft::UI::Xaml::Controls::SwapChainPanel const& panel);
