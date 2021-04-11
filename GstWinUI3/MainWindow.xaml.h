#pragma once

#pragma push_macro("GetCurrentTime")
#undef GetCurrentTime

#include "MainWindow.g.h"

#pragma pop_macro("GetCurrentTime")

#include "GstHelper.h"

namespace winrt::GstWinUI3::implementation {
struct MainWindow : MainWindowT<MainWindow>
{
  MainWindow();

  int32_t MyProperty();
  void MyProperty(int32_t value);

  void btnStart_Click(Windows::Foundation::IInspectable const& sender,
                      Microsoft::UI::Xaml::RoutedEventArgs const& args);
  void btnStop_Click(Windows::Foundation::IInspectable const& sender,
                     Microsoft::UI::Xaml::RoutedEventArgs const& args);

  void runPipeline(void);
  void stopPipeline(void);

  GstElement* pipeline_;
};
}

namespace winrt::GstWinUI3::factory_implementation {
struct MainWindow : MainWindowT<MainWindow, implementation::MainWindow>
{};
}
