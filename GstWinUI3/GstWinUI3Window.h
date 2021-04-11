#pragma once

#include "pch.h"

#include <functional>
#include <microsoft.ui.xaml.media.dxinterop.h>

#ifndef GST_USE_UNSTABLE_API
#define GST_USE_UNSTABLE_API
#endif
#include <gst/d3d11/gstd3d11.h>

namespace winrt::GstWinUI3::implementation {
class GstWinUI3Window
{
public:
  GstWinUI3Window(
    winrt::Microsoft::UI::Xaml::Controls::SwapChainPanel const& panel,
    GstD3D11Device* device);
  ~GstWinUI3Window(void);

  gboolean Prepare(GstCaps* caps, gboolean* have_video_processor);
  gboolean Unprepare(void);
  GstFlowReturn Present(GstBuffer* buf);

private:
  // SwapChainPanel event handlers and tokens
  void onSwapChainSizeChanged(
    const winrt::Windows::Foundation::IInspectable sender,
    winrt::Microsoft::UI::Xaml::SizeChangedEventArgs args);
  void onCompositionScaleChanged(
    Microsoft::UI::Xaml::Controls::SwapChainPanel sender,
    Windows::Foundation::IInspectable insp);
  void onShutdownStarting(
    Microsoft::System::DispatcherQueue queue,
    Microsoft::System::DispatcherQueueShutdownStartingEventArgs args);
  void onShutdownCompleted(Microsoft::System::DispatcherQueue queue,
                           Windows::Foundation::IInspectable insp);

  winrt::event_token size_changed_token_;
  winrt::event_token scale_changed_token_;
  winrt::event_token shutdown_starting_token_;
  winrt::event_token shutdown_completed_token_;

private:
  HRESULT initSwapChain(void);
  HRESULT deinitSwapChain(void);
  HRESULT initConverter(void);
  HRESULT updateSwapChain(void);
  HRESULT presentSync(void);
  HRESULT runSync(winrt::Microsoft::System::DispatcherQueue const& queue,
                  std::function<HRESULT(void)> func);
  bool updateInputFormat(GstCaps* caps);
  bool getPIVFromBuffer(GstBuffer* buf, ID3D11VideoProcessorInputView** piv);
  bool getSRVFromBuffer(GstBuffer* buf,
                        ID3D11ShaderResourceView* srv[GST_VIDEO_MAX_PLANES]);
  HRESULT processorBlt(ID3D11VideoProcessorInputView* piv);
  HRESULT convert(ID3D11ShaderResourceView* srv[GST_VIDEO_MAX_PLANES]);

private:
  winrt::Microsoft::UI::Xaml::Controls::SwapChainPanel panel_;
  winrt::com_ptr<IDXGISwapChain2> swapchain_;
  winrt::com_ptr<ID3D11RenderTargetView> rtv_;
  winrt::com_ptr<ID3D11VideoProcessorOutputView> pov_;
  winrt::com_ptr<ID3D11VideoProcessorEnumerator1> proc_enum_;
  winrt::com_ptr<ID3D11VideoProcessor> proc_;

  // Panel::ActualSize
  double actual_width_;
  double actual_height_;

  // Panel::CompositionScaleXY
  float scale_x_;
  float scale_y_;

  // Panel::ActualSize * Panel::CompositionScaleXY
  UINT output_width_;
  UINT output_height_;

  DXGI_COLOR_SPACE_TYPE input_colorspace_;
  DXGI_COLOR_SPACE_TYPE output_colorspace_;

  DXGI_FORMAT input_dxgi_format_;
  DXGI_FORMAT output_dxgi_format_;

  GstD3D11Device* device_;
  GstD3D11Converter* converter_;
  GstD3D11OverlayCompositor* compositor_;

  // PAR compensated resolution
  UINT video_width_;
  UINT video_height_;

  // represent shader input format
  GstVideoInfo info_;

  // represent shader output format (i.e., swapchain's backbuffer)
  GstVideoInfo output_info_;

  GstBuffer* last_buf_;

  bool processor_in_use_;
  bool dispatcher_is_active_;
  HANDLE shoutdown_handle_;
};
}
