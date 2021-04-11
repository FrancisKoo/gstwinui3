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
  pipeline_ = gst_parse_launch(
    "videotestsrc ! queue ! winui3videosink name=sink", nullptr);

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
