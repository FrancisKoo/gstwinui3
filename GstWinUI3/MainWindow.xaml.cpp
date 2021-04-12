#include "pch.h"

#include "MainWindow.xaml.h"
#if __has_include("MainWindow.g.cpp")
#include "MainWindow.g.cpp"
#endif

#include "GstWinUI3VideoSink.h"

using namespace winrt;
using namespace Microsoft::UI::Xaml;

// To learn more about WinUI, the WinUI project structure,
// and more about our project templates, see: http://aka.ms/winui-project-info.

namespace winrt::GstWinUI3::implementation {
MainWindow::MainWindow()
  : pipeline_(nullptr)
{
  InitializeComponent();
}

int32_t
MainWindow::MyProperty()
{
  throw hresult_not_implemented();
}

void MainWindow::MyProperty(int32_t /* value */)
{
  throw hresult_not_implemented();
}

void
MainWindow::btnStart_Click(IInspectable const&, RoutedEventArgs const&)
{
  btnStart().IsEnabled(false);
  runPipeline();
  videoPanel().Visibility(winrt::Microsoft::UI::Xaml::Visibility::Visible);
  btnStop().IsEnabled(true);
}

void
MainWindow::btnStop_Click(IInspectable const&, RoutedEventArgs const&)
{
  btnStop().IsEnabled(false);
  stopPipeline();
  btnStart().IsEnabled(true);
  videoPanel().Visibility(winrt::Microsoft::UI::Xaml::Visibility::Collapsed);
}

void
MainWindow::runPipeline()
{
#define VIDEOTEST_STR "videotestsrc ! queue ! winui3videosink name=sink"
#define MFVIDESRC_STR "mfvideosrc ! queue ! winui3videosink name=sink"
#define SCREENCAP_STR "d3d11desktopdupsrc ! queue ! winui3videosink name=sink"

#define FILE_PATH "C:/Work/gst-build/trailer.mp4"

#define FILE_MP4_H264_AAC                                                      \
  "filesrc location=" FILE_PATH " ! qtdemux name=d ! queue ! h264parse ! "     \
  "d3d11h264dec ! winui3videosink name=sink"                                   \
  " d. ! queue ! aacparse ! avdec_aac ! audioconvert ! audioresample ! "       \
  "wasapi2sink"
#define PIPELINE_STR FILE_MP4_H264_AAC

  pipeline_ = gst_parse_launch(PIPELINE_STR, nullptr);
  g_assert(pipeline_);

  GstElement* sink = gst_bin_get_by_name(GST_BIN_CAST(pipeline_), "sink");
  if (sink) {
    gst_win_ui3_video_sink_set_panel(GST_WIN_UI3_VIDEO_SINK(sink),
                                     videoPanel());
    gst_object_unref(sink);
  }

  gst_element_set_state(pipeline_, GST_STATE_PLAYING);
}

void
MainWindow::stopPipeline()
{
  if (pipeline_) {
    gst_element_set_state(pipeline_, GST_STATE_NULL);
    gst_clear_object(&pipeline_);
  }
}
}
