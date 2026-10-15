# TASK: Port Graphics Backend from Mesa D3D12 to ANGLE (D3D11 / GLES 3.0) with GitHub Actions Cloud Build & Xbox Device Portal Deployment

## Role & System Context
You are an expert Systems and Graphics Engineer specializing in Xbox Dev Mode (UWP/WinRT), Direct3D 11/12, EGL, and automated CI/CD deployment pipelines.
We are maintaining `OpenCE-UWP-XB1`, a UWP fork of OpenCE (Halo: Combat Evolved decompilation) targeting original Xbox One (Durango) hardware.

Local device hardware cannot handle compilation workloads. All compiling, linking, asset bundling, and packaging MUST execute entirely in the cloud via **GitHub Actions** (`windows-latest`). Once built, the packaged `.appx` package and dependencies must be deployed to the target Xbox One Dev Mode console over the local network at `https://192.168.1.67:11443`.

---

## Technical Architecture & Constraints

1. **Target Hardware & Environment**: 
   - Platform: Xbox One (Durango) in Developer Mode (`WINAPI_FAMILY_APP`, x64).
   - Console Endpoint: `https://192.168.1.67:11443` (Xbox Device Portal REST API).
2. **Graphics Architecture Migration**:
   - **Remove**: Desktop OpenGL 4.5 Core over Mesa D3D12 (stripping SM6.0 DXIL compiler hacks, VEH exception handlers at RVA `0x57f68a`, and Mesa stdout/stderr redirection).
   - **Implement**: Microsoft ANGLE over **Direct3D 11 Feature Level 11_0**, exposing **EGL 1.5** and **OpenGL ES 3.0**.
   - **Shader Target**: GLSL ES 3.00 (`#version 300 es`, `precision mediump float;`).
   - **Engine Layer**: Route calls through OpenCE's existing GLES/Android path in `port/linux/src/d3d8_gl.c`.
3. **Cloud Build Execution**:
   - Zero local toolchain dependencies (no MSVC/Clang/Ninja required on the local client).
   - GitHub Actions manages dependency retrieval (`nuget` for ANGLE UWP binaries), compilation, signing, AppX packaging, and artifact release.

---

## Required Deliverables & Implementation Details

### 1. Host Context & Swapchain Lifecycle (`UWP/xbox/host/host_main.cpp`)
- Revert all Mesa hacks:
  - Strip `AddVectoredExceptionHandler` and RVA `0x57f68a` crash recovery.
  - Remove stdout/stderr redirection hooks and experimental `D3D12_DEBUG` flags.
- Implement ANGLE EGL display initialization targeting Direct3D 11:
  ```cpp
  #include <EGL/egl.h>
  #include <EGL/eglext.h>
  #include <EGL/eglext_angle.h>
  #include <GLES3/gl3.h>

  const EGLint displayAttributes[] = {
      EGL_PLATFORM_ANGLE_TYPE_ANGLE,                  EGL_PLATFORM_ANGLE_TYPE_D3D11_ANGLE,
      EGL_PLATFORM_ANGLE_DEVICE_TYPE_ANGLE,            EGL_PLATFORM_ANGLE_DEVICE_TYPE_HARDWARE_ANGLE,
      EGL_PLATFORM_ANGLE_ENABLE_AUTOMATIC_TRIM_ANGLE,      EGL_TRUE,
      EGL_NONE,
  };
  EGLDisplay egl_display = eglGetPlatformDisplayEXT(EGL_PLATFORM_ANGLE_ANGLE, EGL_DEFAULT_DISPLAY, displayAttributes);
  ```
- Select RGBA8888, 24-bit depth, 8-bit stencil with `EGL_OPENGL_ES3_BIT`.
- Create context with `EGL_CONTEXT_CLIENT_VERSION = 3`.
- Bind WinRT `CoreWindow` to EGL surface:
  ```cpp
  IInspectable* window = reinterpret_cast<IInspectable*>(
      winrt::Windows::UI::Core::CoreWindow::GetForCurrentThread().get()
  );
  EGLSurface egl_surface = eglCreateWindowSurface(egl_display, egl_config, static_cast<EGLNativeWindowType>(window), nullptr);
  eglMakeCurrent(egl_display, egl_surface, egl_surface, egl_context);
  ```
- Map swapchain flip to `eglSwapBuffers(egl_display, egl_surface)` and extension queries to `eglGetProcAddress`.

### 2. Setup UI & Shader Conformance (`UWP/xbox/host/setup_ui.cpp`)
- Update ImGui backend initialization:
  ```cpp
  ImGui_ImplOpenGL3_Init("#version 300 es");
  ```
- Ensure all host GLSL shaders declare `#version 300 es` with default precision definitions (`precision mediump float;`).
- Remove any desktop GL state calls (e.g., `glPolygonMode`).

### 3. GL Thunk Stub Generator (`tools/xbox_host_gl.py`)
- Retarget symbol generation from desktop `opengl32.dll` to `libGLESv2.dll` and GLES 3.0 headers.
- Enforce `GL_APIENTRY` calling conventions for x64 UWP.
- Prune desktop OpenGL 4.5 extensions unsupported by ANGLE GLES 3.0.

### 4. GitHub Actions CI/CD Pipeline (`.github/workflows/uwp-auto.yml`)
- Update the workflow to execute the entire build on `windows-latest`:
  1. **Fetch Dependencies**: Download and unpack the official `ANGLE.WindowsStore` NuGet package to extract prebuilt UWP `x64` binaries (`libEGL.dll`, `libGLESv2.dll`, `libEGL.lib`, `libGLESv2.lib`) and include headers (`EGL`, `GLES2`, `GLES3`, `KHR`) into `UWP/xbox/third_party/angle/`.
  2. **Purge Mesa Hooks**: Strip all downloads, patches, and build scripts related to Mesa D3D12 and the SM6.0 triple patch.
  3. **Build Target**: Run CMake and MSVC inside the runner (`cmake -B build -S . -DCMAKE_SYSTEM_NAME=WindowsStore -DCMAKE_SYSTEM_VERSION="10.0" ...`).
  4. **Package & Sign**: Create test cert (`tools/sign_xbox.ps1`), stage dependencies (`tools/package_xbox.ps1`), and generate `.appx` / `.msixbundle`.
  5. **Upload Artifacts**: Upload the signed AppX bundle, certificate (`.cer`), and dependency package folder as GitHub workflow artifacts.

### 5. Deployment Automation to Xbox (`192.168.1.67:11443`)
Because the build compiles in the GitHub cloud and the Xbox is located on a private LAN (`192.168.1.67`), provide deployment handling through:
- **Option A (Local Bridge Script - `tools/deploy_xbox.ps1` / `tools/deploy_xbox.sh`)**:
  A lightweight script that can be executed from any terminal or local device on the network. It:
  - Fetches the latest successful AppX artifact using the GitHub REST API (`gh run download` or `curl`).
  - Calls the Xbox Device Portal REST API to upload and install the package dependencies and application:
    ```bash
    POST https://192.168.1.67:11443/api/app/packagemanager/package?package=OpenCE_1.5.7.0_x64.appx
    ```
  - Handles authentication and self-signed SSL verification (`--insecure` / `-SkipCertificateCheck`).
- **Option B (Tailscale / Self-Hosted Action Runner Configuration)**:
  Provide the workflow additions required if using a self-hosted runner or network tunnel to push directly from GitHub Actions to `https://192.168.1.67:11443` at the end of the build job.

---

## Output Instructions
1. Provide the exact file diffs/rewrites for:
   - `UWP/xbox/host/host_main.cpp`
   - `UWP/xbox/host/setup_ui.cpp`
   - `tools/xbox_host_gl.py`
   - `UWP/xbox/CMakeLists.txt`
   - `.github/workflows/uwp-auto.yml`
   - `tools/deploy_xbox.ps1` (or `tools/deploy_xbox.sh`)
2. Ensure all WinRT COM pointer handling (`winrt::com_ptr`, `IInspectable`) is fully valid C++/WinRT.
3. Keep changes minimal, targeted, and directly functional without placeholder comments.

---

### Immediate Execution Verification Checklist

1. **GitHub Secrets Check**:
   - Ensure a GitHub Personal Access Token (`GH_TOKEN` or `GITHUB_TOKEN`) with `repo` and `actions:read` permissions is available for fetching workflow run artifacts.
   - If using authenticated Xbox Device Portal, note the console admin username and password for the deployment script.

2. **Console State Verification**:
   - Confirm Xbox One is running in **Dev Mode**.
   - Verify Xbox Device Portal is enabled: `Settings -> System -> Developer settings -> Enable Xbox Device Portal`.
   - Test connectivity from your network by querying `curl -k https://192.168.1.67:11443/api/os/info`.

3. **Trigger CI**:
   - Push the updated files and workflow to your repository branch.
   - Monitor the GitHub Actions tab to confirm CMake links against `libGLESv2.lib` and outputs the signed `.appx` artifact.
