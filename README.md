# GStreamer Video Sink for WinUI3

This project contains a new GStreamer video sink element and examples to show a basic usage.

## 1. Requirements

### 1.1 gst-build
- Setup [gst-build](https://gitlab.freedesktop.org/gstreamer/gst-build) as documented, see also [Windows Prerequisites Setup](https://gitlab.freedesktop.org/gstreamer/gst-build#windows-prerequisites-setup)

- Apply 3 patches to GStreamer source code
  - gstreamer: https://gitlab.freedesktop.org/gstreamer/gstreamer/-/merge_requests/787
  - gst-plugins-bad: https://gitlab.freedesktop.org/gstreamer/gst-plugins-bad/-/merge_requests/2153 and [local patch](Patches/0001-d3d11-Install-library-headers.patch)

- Installation prefix should be specified, for example
```shell
PS > meson _builddir --prefix=C:/WHERE/TO/INSTALL
... meson will configure gst-build project...
PS > ninja -C _builddir install
```

### 1.2 Visual Studio 2019
- Visual Studio 2019 16.9.3 or newer is required so that can use [WinUI3 development package](https://docs.microsoft.com/en-us/windows/apps/winui/winui3/)

- Modify `GST_INSTALL_PATH` user macro in [GStreamer.props](GstWinUI3/GStreamer.props) with your `prefix` path via Visual Studio -> `Property Manager` or manually.


## 2. GstWinUI3VideoSink
This element is very similar to upstream `d3d11videosink` element but WinUI3 SwapChainPanel specific implementation. The usage is not much different from other elements.

### 2.1 Element create

User can create the element via `gst_element_factory_make()`
```c
GstElement* mySink =
  gst_element_factory_make (`winui3videosink`, nullptr);
```

and `gst_parse_launch()` will work as well.

### 2.2 SwapChainPanel Setup

`winui3videosink` element doesn't support `GstVideoOverlay` interface. So, `gst_video_overlay_set_window_handle()` method must not be used in this case. Instead, user need to use `gst_win_ui3_video_sink_set_panel()` method. See also [header file](GstWinUI3/GstWinUI3VideoSink.h)