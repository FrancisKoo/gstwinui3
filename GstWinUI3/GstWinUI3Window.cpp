#include "pch.h"

#include "GstWinUI3Window.h"
#include <cmath>
#include <cstring>
#include <inspectable.h>
#include <mutex>

#include <winrt/Microsoft.System.h>

using namespace winrt;
using namespace winrt::Microsoft::UI::Xaml;
using namespace winrt::Microsoft::UI::Xaml::Controls;
using namespace winrt::Microsoft::System;

GST_DEBUG_CATEGORY_STATIC(winui3_window);
#define GST_CAT_DEFAULT winui3_window

std::once_flag debug_init_once;

namespace winrt::GstWinUI3::implementation {
GstWinUI3Window::GstWinUI3Window(SwapChainPanel const& panel,
                                 GstD3D11Device* device)
  : panel_(panel)
  , actual_width_(0)
  , actual_height_(0)
  , scale_x_(1.0f)
  , scale_y_(1.0f)
  , output_width_(1)
  , output_height_(1)
  , input_colorspace_(DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709)
  , output_colorspace_(DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709)
  , input_dxgi_format_(DXGI_FORMAT_UNKNOWN)
  , output_dxgi_format_(DXGI_FORMAT_B8G8R8A8_UNORM)
  , device_(nullptr)
  , converter_(nullptr)
  , compositor_(nullptr)
  , dispatcher_is_active_(true)
{
  std::call_once(debug_init_once, []() {
    GST_DEBUG_CATEGORY_INIT(winui3_window, "winui3window", 0, "winui3window");
  });

  device_ = (GstD3D11Device*)gst_object_ref(device);
  shoutdown_handle_ = ::CreateEvent(nullptr, true, false, nullptr);
}

GstWinUI3Window::~GstWinUI3Window()
{
  if (panel_ && swapchain_) {
    std::function<HRESULT(void)> func =
      std::bind(&GstWinUI3Window::deinitSwapChain, this);

    runSync(panel_.DispatcherQueue(), func);
  }

  g_clear_pointer(&converter_, gst_d3d11_converter_free);
  g_clear_pointer(&compositor_, gst_d3d11_overlay_compositor_free);
  gst_clear_object(&device_);
  gst_clear_buffer(&last_buf_);

  ::CloseHandle(shoutdown_handle_);
}

void
GstWinUI3Window::onSwapChainSizeChanged(
  const winrt::Windows::Foundation::IInspectable sender,
  SizeChangedEventArgs args)
{
  actual_width_ = args.NewSize().Width;
  actual_height_ = args.NewSize().Height;

  GST_INFO("Size changed to %f, %f", actual_width_, actual_height_);

  gst_d3d11_device_lock(device_);
  updateSwapChain();
  gst_d3d11_device_unlock(device_);
}

void
GstWinUI3Window::onCompositionScaleChanged(
  SwapChainPanel sender,
  Windows::Foundation::IInspectable insp)
{
  scale_x_ = sender.CompositionScaleX();
  scale_y_ = sender.CompositionScaleY();

  GST_INFO("Scale changed to %f, %f", scale_x_, scale_y_);

  gst_d3d11_device_lock(device_);
  updateSwapChain();
  gst_d3d11_device_unlock(device_);
}

void
GstWinUI3Window::onShutdownStarting(
  Microsoft::System::DispatcherQueue queue,
  Microsoft::System::DispatcherQueueShutdownStartingEventArgs args)
{
  GST_INFO("Shutdown Starting");
}

void
GstWinUI3Window::onShutdownCompleted(Microsoft::System::DispatcherQueue queue,
                                     Windows::Foundation::IInspectable insp)
{
  GST_INFO("Shutdown Completed");
  ::SetEvent(shoutdown_handle_);
}

HRESULT
GstWinUI3Window::runSync(DispatcherQueue const& queue,
                         std::function<HRESULT(void)> func)
{
  if (queue.HasThreadAccess()) {
    return func();
  }

  HRESULT hr = S_OK;
  winrt::handle h{ ::CreateEvent(nullptr, false, false, nullptr) };
  if (!queue.TryEnqueue(DispatcherQueueHandler([&]() {
        hr = func();
        winrt::check_bool(::SetEvent(h.get()));
      }))) {
    return E_FAIL;
  }

  HANDLE handles[2];
  handles[0] = h.get();
  handles[1] = shoutdown_handle_;
  // set timeout??
  DWORD ret = ::WaitForMultipleObjects(2, handles, FALSE, INFINITE);
  if (ret != WAIT_OBJECT_0 && ret != WAIT_OBJECT_0 + 1) {
    GST_ERROR("Error waiting for event handle: 0x%x", (guint)ret);
    return E_FAIL;
  }

  return hr;
}

HRESULT
GstWinUI3Window::initSwapChain(void)
{
  IDXGIFactory1* factory = gst_d3d11_device_get_dxgi_factory_handle(device_);
  ID3D11Device* device = gst_d3d11_device_get_device_handle(device_);
  HRESULT hr;
  DXGI_SWAP_CHAIN_DESC1 desc = {
    0,
  };

  gst_d3d11_device_lock(device_);
  if (!swapchain_) {
    com_ptr<ISwapChainPanelNative> native;
    winrt::com_ptr<IDXGIFactory2> factory2;
    winrt::com_ptr<IDXGISwapChain1> swapchain;
    if (!panel_.try_as(native)) {
      GST_ERROR("Failed to get ISwapChainPanelNative");
      gst_d3d11_device_unlock(device_);
      return E_FAIL;
    }

    winrt::check_hresult(
      factory->QueryInterface(__uuidof(factory2), factory2.put_void()));

    actual_width_ = panel_.ActualWidth();
    actual_height_ = panel_.ActualHeight();

    scale_x_ = panel_.CompositionScaleX();
    scale_y_ = panel_.CompositionScaleY();

    output_width_ = (UINT)MAX(round(actual_width_ * scale_x_), 1.0);
    output_height_ = (UINT)MAX(round(actual_height_ * scale_y_), 1.0);

    desc.Width = output_width_;
    desc.Height = output_height_;
    // FIXME: should be able to render other format
    desc.Format = output_dxgi_format_;
    desc.SampleDesc.Count = 1;
    desc.BufferCount = 2;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.Scaling = DXGI_SCALING_STRETCH;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;

    hr = factory2->CreateSwapChainForComposition(
      device, &desc, nullptr, swapchain.put());
    winrt::check_hresult(hr);

    // We need swapchain2 interface for ::SetMatrixTransform
    if (!swapchain.try_as(swapchain_)) {
      gst_d3d11_device_unlock(device_);
      return E_FAIL;
    }

    winrt::check_hresult(native->SetSwapChain(swapchain_.get()));

    // TODO: update format depending on selected DXGI format
    gst_video_info_set_format(&output_info_,
                              gst_d3d11_dxgi_format_to_gst(output_dxgi_format_),
                              output_width_,
                              output_height_);

    // Set colorspace we use
    output_info_.colorimetry.primaries = GST_VIDEO_COLOR_PRIMARIES_BT709;
    output_info_.colorimetry.transfer = GST_VIDEO_TRANSFER_BT709;
    output_info_.colorimetry.range = GST_VIDEO_COLOR_RANGE_0_255;

    // Register events
    size_changed_token_ = panel_.SizeChanged(SizeChangedEventHandler{
      this, &GstWinUI3Window::onSwapChainSizeChanged });

    scale_changed_token_ = panel_.CompositionScaleChanged(
      winrt::Windows::Foundation::TypedEventHandler<
        Microsoft::UI::Xaml::Controls::SwapChainPanel,
        Windows::Foundation::IInspectable>(
        { this, &GstWinUI3Window::onCompositionScaleChanged }));

    // Register shutdown events
    shutdown_starting_token_ = panel_.DispatcherQueue().ShutdownStarting(
      winrt::Windows::Foundation::TypedEventHandler<
        Microsoft::System::DispatcherQueue,
        Microsoft::System::DispatcherQueueShutdownStartingEventArgs>(
        { this, &GstWinUI3Window::onShutdownStarting }));

    shutdown_completed_token_ = panel_.DispatcherQueue().ShutdownCompleted(
      winrt::Windows::Foundation::TypedEventHandler<
        Microsoft::System::DispatcherQueue,
        Windows::Foundation::IInspectable>(
        { this, &GstWinUI3Window::onShutdownCompleted }));
  }

  hr = initConverter();
  if (FAILED(hr)) {
    gst_d3d11_device_unlock(device_);
    return hr;
  }

  hr = updateSwapChain();
  gst_d3d11_device_unlock(device_);

  return hr;
}

// Must be called with d3d11 lock
HRESULT
GstWinUI3Window::initConverter(void)
{
  g_clear_pointer(&converter_, gst_d3d11_converter_free);
  g_clear_pointer(&compositor_, gst_d3d11_overlay_compositor_free);
  proc_enum_ = nullptr;
  proc_ = nullptr;

  // Setup converter for colorspace conversion via shader
  converter_ = gst_d3d11_converter_new(device_, &info_, &output_info_);
  if (!converter_)
    winrt::throw_hresult(E_FAIL);

  // Setup overlay compositor to handle videooverlay meta
  compositor_ = gst_d3d11_overlay_compositor_new(device_, &output_info_);
  if (!compositor_)
    winrt::throw_hresult(E_FAIL);

  // Try ID3D11VideoProcessor, but any error below will be ignored since we can
  // use converter at least
  if (input_dxgi_format_ == DXGI_FORMAT_UNKNOWN) {
    // Non-native DXGI formats (e.g., I420) are no supported by video processor
    return S_OK;
  }

  ID3D11VideoDevice* video_device =
    gst_d3d11_device_get_video_device_handle(device_);
  ID3D11VideoContext* video_context =
    gst_d3d11_device_get_video_context_handle(device_);

  // Device doesn't support hardware video processor
  if (!video_device || !video_context) {
    GST_INFO("video device is unavailable");
    return S_OK;
  }

  HRESULT hr;
  winrt::com_ptr<ID3D11VideoContext1> video_context1;
  hr = video_context->QueryInterface(__uuidof(video_context1),
                                     video_context1.put_void());
  if (FAILED(hr)) {
    GST_INFO("ID3D11VideoContext1 is unavailable");
    return S_OK;
  }

  D3D11_VIDEO_PROCESSOR_CONTENT_DESC desc;
  winrt::com_ptr<ID3D11VideoProcessorEnumerator> proc_enum;

  memset(&desc, 0, sizeof(D3D11_VIDEO_PROCESSOR_CONTENT_DESC));
  desc.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
  desc.InputWidth = GST_VIDEO_INFO_WIDTH(&info_);
  desc.InputHeight = GST_VIDEO_INFO_HEIGHT(&info_);
  desc.OutputWidth = GST_VIDEO_INFO_WIDTH(&output_info_);
  desc.OutputHeight = GST_VIDEO_INFO_HEIGHT(&output_info_);
  desc.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;

  hr = video_device->CreateVideoProcessorEnumerator(&desc, proc_enum.put());
  if (FAILED(hr)) {
    GST_INFO("ID3D11VideoProcessorEnumerator is unavailable");
    return S_OK;
  }

  winrt::com_ptr<ID3D11VideoProcessorEnumerator1> proc_enum1;
  if (!proc_enum.try_as(proc_enum1)) {
    GST_INFO("ID3D11VideoProcessorEnumerator1 is unavailable");
    return S_OK;
  }

  BOOL supported = 0;
  hr = proc_enum1->CheckVideoProcessorFormatConversion(input_dxgi_format_,
                                                       input_colorspace_,
                                                       output_dxgi_format_,
                                                       output_colorspace_,
                                                       &supported);
  if (FAILED(hr) || !supported)
    return S_OK;

  // Ok, we can use video processor
  winrt::com_ptr<ID3D11VideoProcessor> proc;
  hr = video_device->CreateVideoProcessor(proc_enum.get(), 0, proc.put());
  if (FAILED(hr))
    return S_OK;

  // Set initial rect here, then it will be updated upon resize event
  RECT rect;
  rect.left = 0;
  rect.top = 0;
  rect.right = GST_VIDEO_INFO_WIDTH(&info_);
  rect.bottom = GST_VIDEO_INFO_HEIGHT(&info_);

  video_context->VideoProcessorSetStreamSourceRect(proc.get(), 0, TRUE, &rect);

  // We don't want arbitrary auto processing done by GPU
  video_context->VideoProcessorSetStreamAutoProcessingMode(
    proc.get(), 0, FALSE);

  video_context1->VideoProcessorSetStreamColorSpace1(
    proc.get(), 0, input_colorspace_);
  video_context1->VideoProcessorSetStreamColorSpace1(
    proc.get(), 0, output_colorspace_);

  proc_enum_ = proc_enum1;
  proc_ = proc;

  return S_OK;
}

// Must be called with d3d11 lock
HRESULT
GstWinUI3Window::updateSwapChain(void)
{
  // Clear existing resources if any
  rtv_ = nullptr;
  pov_ = nullptr;

  output_width_ = (UINT)MAX(round(actual_width_ * scale_x_), 1.0);
  output_height_ = (UINT)MAX(round(actual_height_ * scale_y_), 1.0);

  winrt::check_hresult(swapchain_->ResizeBuffers(
    0, output_width_, output_height_, output_dxgi_format_, 0));

  // apply inverse scale
  DXGI_MATRIX_3X2_F inverse_scale = {
    0,
  };
  inverse_scale._11 = 1.0f / scale_x_;
  inverse_scale._22 = 1.0f / scale_y_;
  winrt::check_hresult(swapchain_->SetMatrixTransform(&inverse_scale));

  winrt::com_ptr<ID3D11Texture2D> backbuffer;
  winrt::check_hresult(
    swapchain_->GetBuffer(0, __uuidof(backbuffer), backbuffer.put_void()));

  ID3D11Device* device_handle = gst_d3d11_device_get_device_handle(device_);
  winrt::check_hresult(device_handle->CreateRenderTargetView(
    backbuffer.get(), nullptr, rtv_.put()));

  GstVideoRectangle src_rect, dst_rect, rst_rect;
  // backbuffer size
  dst_rect.x = 0;
  dst_rect.y = 0;
  dst_rect.w = output_width_;
  dst_rect.h = output_height_;

  // PAR compensated video size
  src_rect.x = 0;
  src_rect.y = 0;
  src_rect.w = video_width_;
  src_rect.h = video_height_;

  // calculate center rect
  gst_video_sink_center_rect(src_rect, dst_rect, &rst_rect, TRUE);

  ID3D11VideoDevice* video_device =
    gst_d3d11_device_get_video_device_handle(device_);
  ID3D11VideoContext* video_context =
    gst_d3d11_device_get_video_context_handle(device_);

  // If we have video processor, prepare processor output view as well
  if (proc_ && proc_enum_ && video_device && video_context) {
    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC desc;

    desc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
    desc.Texture2D.MipSlice = 0;

    winrt::check_hresult(video_device->CreateVideoProcessorOutputView(
      backbuffer.get(), proc_enum_.get(), &desc, pov_.put()));

    RECT out_rect;
    out_rect.left = rst_rect.x;
    out_rect.top = rst_rect.y;
    out_rect.right = rst_rect.x + rst_rect.w;
    out_rect.bottom = rst_rect.y + rst_rect.h;

    video_context->VideoProcessorSetOutputTargetRect(
      proc_.get(), TRUE, &out_rect);
    video_context->VideoProcessorSetStreamDestRect(
      proc_.get(), 0, TRUE, &out_rect);
  }

  D3D11_VIEWPORT vp;
  vp.TopLeftX = (FLOAT)rst_rect.x;
  vp.TopLeftY = (FLOAT)rst_rect.y;
  vp.Width = (FLOAT)rst_rect.w;
  vp.Height = (FLOAT)rst_rect.h;
  vp.MinDepth = 0.0f;
  vp.MaxDepth = 1.0f;

  gst_d3d11_converter_update_viewport(converter_, &vp);
  gst_d3d11_overlay_compositor_update_viewport(compositor_, &vp);

  // If we have cached buffer, redraw it here. It will reduce glitch during
  return presentSync();
}

// Must be called from dispatcher queue
HRESULT
GstWinUI3Window::deinitSwapChain(void)
{
  if (size_changed_token_)
    panel_.SizeChanged(size_changed_token_);
  size_changed_token_.value = 0;

  if (scale_changed_token_)
    panel_.CompositionScaleChanged(scale_changed_token_);
  scale_changed_token_.value = 0;

  swapchain_ = nullptr;

  return S_OK;
}

bool
GstWinUI3Window::updateInputFormat(GstCaps* caps)
{
  gint width, height;
  gint par_n, par_d;
  guint num, den;

  if (!gst_video_info_from_caps(&info_, caps)) {
    GST_ERROR("Invalid caps %" GST_PTR_FORMAT, caps);

    return false;
  }

  width = GST_VIDEO_INFO_WIDTH(&info_);
  height = GST_VIDEO_INFO_HEIGHT(&info_);
  par_n = GST_VIDEO_INFO_PAR_N(&info_);
  par_d = GST_VIDEO_INFO_PAR_D(&info_);

  if (!gst_video_calculate_display_ratio(
        &num, &den, width, height, par_n, par_d, 1, 1)) {
    GST_ERROR("Couldn't calculate display ratio");
    return false;
  }

  if (width % num == 0) {
    // Keep video width
    video_width_ = width;
    video_height_ = width * den / num;
  } else {
    // Keep video height
    video_width_ = height * num / den;
    video_height_ = height;
  }

  const GstD3D11Format* in_format =
    gst_d3d11_device_format_from_gst(device_, GST_VIDEO_INFO_FORMAT(&info_));
  if (!in_format) {
    GST_ERROR("Failed to get d3d11 format from caps %" GST_PTR_FORMAT, caps);
    return false;
  }

  // NOTE: for some formats, this can be DXGI_FORMAT_UNKNOWN (I420 for example).
  // In that case, we use resource format for each plain
  input_dxgi_format_ = in_format->dxgi_format;

  // TODO: implement mapping
  if (GST_VIDEO_INFO_IS_RGB(&info_)) {
    // Our default format
    input_colorspace_ = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
  } else {
    input_colorspace_ = DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709;
  }

  return true;
}

gboolean
GstWinUI3Window::Prepare(GstCaps* caps, gboolean* have_video_processor)
{
  // reset this, and it will be updated on Present method later.
  processor_in_use_ = false;
  *have_video_processor = FALSE;

  if (!updateInputFormat(caps))
    return FALSE;

  std::function<HRESULT(void)> func =
    std::bind(&GstWinUI3Window::initSwapChain, this);
  winrt::check_hresult(runSync(panel_.DispatcherQueue(), func));

  if (proc_)
    *have_video_processor = TRUE;

  return TRUE;
}

gboolean
GstWinUI3Window::Unprepare(void)
{
  ::SetEvent(shoutdown_handle_);

  return TRUE;
}

bool
GstWinUI3Window::getPIVFromBuffer(GstBuffer* buf,
                                  ID3D11VideoProcessorInputView** piv)
{
  if (!proc_ || !pov_)
    return false;

  // Should be single texture buffer
  if (gst_buffer_n_memory(buf) != 1)
    return false;

  GstD3D11Memory* mem = (GstD3D11Memory*)gst_buffer_peek_memory(buf, 0);
  ID3D11VideoDevice* video_device =
    gst_d3d11_device_get_video_device_handle(device_);

  ID3D11VideoProcessorInputView* view =
    gst_d3d11_memory_get_processor_input_view(
      mem, video_device, proc_enum_.get());

  if (!view)
    return false;

  *piv = view;

  return true;
}

bool
GstWinUI3Window::getSRVFromBuffer(
  GstBuffer* buf,
  ID3D11ShaderResourceView* srv[GST_VIDEO_MAX_PLANES])
{
  guint num_views = 0;

  for (guint i = 0; i < gst_buffer_n_memory(buf); i++) {
    GstD3D11Memory* mem = (GstD3D11Memory*)gst_buffer_peek_memory(buf, i);
    guint view_size;

    view_size = gst_d3d11_memory_get_shader_resource_view_size(mem);
    if (!view_size)
      return false;

    for (guint j = 0; j < view_size; j++) {
      g_assert(num_views < GST_VIDEO_MAX_PLANES);

      srv[num_views++] = gst_d3d11_memory_get_shader_resource_view(mem, j);
    }
  }

  return true;
}

static bool
upload_buffer(GstBuffer* buf)
{
  guint num_mapped = 0;
  GstMapFlags map_flags = (GstMapFlags)(GST_MAP_READ | GST_MAP_D3D11);
  GstMapInfo info;

  // There might be pending data in staging texture, and gst_memory_map()
  // will trigger upload if needed
  for (num_mapped = 0; num_mapped < gst_buffer_n_memory(buf); num_mapped++) {
    GstMemory* mem = gst_buffer_peek_memory(buf, num_mapped);

    if (!gst_memory_map(mem, &info, map_flags)) {
      GST_ERROR("Couldn't map memory %d", num_mapped);
      return false;
    }
    gst_memory_unmap(mem, &info);
  }

  return true;
}

// Must be called with d3d11 lock
HRESULT
GstWinUI3Window::processorBlt(ID3D11VideoProcessorInputView* piv)
{
  ID3D11VideoContext* video_context =
    gst_d3d11_device_get_video_context_handle(device_);
  HRESULT hr;
  D3D11_VIDEO_PROCESSOR_STREAM stream = {
    0,
  };

  stream.Enable = TRUE;
  stream.pInputSurface = piv;

  hr = video_context->VideoProcessorBlt(proc_.get(), pov_.get(), 0, 1, &stream);

  if (gst_d3d11_result(hr, device_))
    processor_in_use_ = true;

  return hr;
}

// Must be called with d3d11 lock
HRESULT
GstWinUI3Window::convert(ID3D11ShaderResourceView* srv[GST_VIDEO_MAX_PLANES])
{
  ID3D11RenderTargetView* rtv[GST_VIDEO_MAX_PLANES] = {
    nullptr,
  };

  rtv[0] = rtv_.get();

  if (!gst_d3d11_converter_convert_unlocked(
        converter_, srv, rtv, nullptr, nullptr)) {
    GST_ERROR("Conversion failed");
    return E_FAIL;
  }

  return S_OK;
}

// Must be called with lock
HRESULT
GstWinUI3Window::presentSync(void)
{
  if (!last_buf_)
    return S_OK;

  ID3D11ShaderResourceView* srv[GST_VIDEO_MAX_PLANES] = {
    nullptr,
  };
  ID3D11VideoProcessorInputView* piv = nullptr;
  HRESULT hr = E_FAIL;

  bool have_srv = false;
  bool have_piv = false;

  have_srv = getSRVFromBuffer(last_buf_, srv);
  have_piv = getPIVFromBuffer(last_buf_, &piv);

  if (!have_srv && !have_piv) {
    GST_ERROR("Not a d3d11 buffer");
    return E_FAIL;
  }

  if (have_piv && processor_in_use_)
    hr = processorBlt(piv);
  else if (have_srv)
    hr = convert(srv);
  else if (have_piv)
    hr = processorBlt(piv);

  if (!gst_d3d11_result(hr, device_))
    return hr;

  ID3D11RenderTargetView* rtv = rtv_.get();
  gst_d3d11_overlay_compositor_draw_unlocked(compositor_, &rtv);
  hr = swapchain_->Present(0, 0);

  return hr;
}

GstFlowReturn
GstWinUI3Window::Present(GstBuffer* buf)
{
  // Do swapchain independent things here
  gst_d3d11_overlay_compositor_upload(compositor_, buf);
  if (!upload_buffer(buf)) {
    GST_ERROR("Failed to upload buffer");
    return GST_FLOW_ERROR;
  }

  gst_d3d11_device_lock(device_);
  gst_buffer_replace(&last_buf_, buf);
  HRESULT hr = presentSync();
  gst_d3d11_device_unlock(device_);

  if (!gst_d3d11_result(hr, device_)) {
    GST_ERROR("Failed to present");
    return GST_FLOW_ERROR;
  }

  return GST_FLOW_OK;
}

}
