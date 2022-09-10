/*********************************************************
 * Copyright (C) 2020, Val Doroshchuk <valbok@gmail.com> *
 *                                                       *
 * This file is part of QtAVPlayer.                      *
 * Free Qt Media Player based on FFmpeg.                 *
 *********************************************************/

#include "qavhwdevice_d3d11_p.h"
#include "qavvideobuffer_gpu_p.h"
#include <d3d11.h>

extern "C" {
#include <libavcodec/d3d11va.h>
}

typedef HANDLE (*wglDXOpenDeviceNV_)(void *dxDevice);
static wglDXOpenDeviceNV_ s_wglDXOpenDeviceNV = nullptr;

typedef BOOL (*wglDXCloseDeviceNV_)(HANDLE hDevice);
static wglDXCloseDeviceNV_ s_wglDXCloseDeviceNV = nullptr;

typedef HANDLE (*wglDXRegisterObjectNV_)(HANDLE hDevice, void *dxObject,GLuint name, GLenum type, GLenum access);
static wglDXRegisterObjectNV_ s_wglDXRegisterObjectNV = nullptr;

typedef BOOL (*wglDXUnregisterObjectNV_)(HANDLE hDevice, HANDLE hObject);
static wglDXUnregisterObjectNV_ s_wglDXUnregisterObjectNV = nullptr;

typedef HGLRC (WINAPI * PFNWGLCREATECONTEXTATTRIBSARBPROC) (HDC hDC, HGLRC hShareContext, const int *attribList);
#define WGL_CONTEXT_DEBUG_BIT_ARB         0x00000001
#define WGL_CONTEXT_FLAGS_ARB             0x2094
#define WGL_CONTEXT_PROFILE_MASK_ARB      0x9126
#define WGL_CONTEXT_COMPATIBILITY_PROFILE_BIT_ARB 0x00000002
#define WGL_ACCESS_READ_WRITE_NV          0x00000001

static const struct {
    DXGI_FORMAT fmt;
    DXGI_FORMAT plane_fmt[2];
} plane_formats[] = {
{ DXGI_FORMAT_R8G8B8A8_UNORM, {DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_UNKNOWN}},
{ DXGI_FORMAT_NV12, {DXGI_FORMAT_R8_UNORM, DXGI_FORMAT_R8G8_UNORM}},
{ DXGI_FORMAT_P010, {DXGI_FORMAT_R16_UNORM, DXGI_FORMAT_R16G16_UNORM}},
{ DXGI_FORMAT_P016, {DXGI_FORMAT_R16_UNORM, DXGI_FORMAT_R16G16_UNORM}},
};

static DXGI_FORMAT planeFormat(DXGI_FORMAT fmt, int plane)
{
    for (size_t i = 0; i < sizeof(plane_formats) / sizeof(plane_formats[0]); ++i) {
        if (plane_formats[i].fmt == fmt)
            return plane_formats[i].plane_fmt[plane];
    }
    return DXGI_FORMAT_UNKNOWN;
}
static HWND temp = 0;
static HDC tempdc = 0;
static HGLRC temprc = 0;

QT_BEGIN_NAMESPACE

QAVHWDevice_D3D11::QAVHWDevice_D3D11(QObject *parent)
    : QObject(parent)
{
}

AVPixelFormat QAVHWDevice_D3D11::format() const
{
    return AV_PIX_FMT_D3D11;
}

AVHWDeviceType QAVHWDevice_D3D11::type() const
{
    return AV_HWDEVICE_TYPE_D3D11VA;
}

class VideoBuffer_D3D11_GL : public QAVVideoBuffer_GPU
{
public:
    VideoBuffer_D3D11_GL(const QAVVideoFrame &frame)
        : QAVVideoBuffer_GPU(frame)
    {
        if (!s_wglDXOpenDeviceNV) {
            createDummyWindow();
            s_wglDXOpenDeviceNV = (wglDXOpenDeviceNV_)wglGetProcAddress("wglDXOpenDeviceNV");
            s_wglDXCloseDeviceNV = (wglDXCloseDeviceNV_)wglGetProcAddress("wglDXCloseDeviceNV");
            s_wglDXRegisterObjectNV = (wglDXRegisterObjectNV_)wglGetProcAddress("wglDXRegisterObjectNV");
            s_wglDXUnregisterObjectNV = (wglDXUnregisterObjectNV_)wglGetProcAddress("wglDXUnregisterObjectNV");
        }
    }

    ~VideoBuffer_D3D11_GL()
    {
        if (dxDevice) {
            for (int i = 0; i < textures_handles.size(); ++i)
                s_wglDXUnregisterObjectNV(dxDevice, textures_handles[i]);
            s_wglDXCloseDeviceNV(dxDevice);
            //wglMakeCurrent(tempdc, NULL);
            //wglDeleteContext(temprc);
            //ReleaseDC(temp, tempdc);
        }
        for (int i = 0; i < textures_gl.size(); ++i) {
            if (textures_gl[i])
                glDeleteTextures(1, &textures_gl[i]); 
        }
    }

    void createDummyWindow()
    {
        temp = CreateWindowA("STATIC", "temp", WS_OVERLAPPED,
                             CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
                             NULL, NULL, NULL, NULL);
        tempdc = GetDC(temp);
        PIXELFORMATDESCRIPTOR pfd;
        pfd.nSize = sizeof(pfd);
        pfd.nVersion = 1;
        pfd.dwFlags = PFD_SUPPORT_OPENGL;
        pfd.iPixelType = PFD_TYPE_RGBA;
        pfd.iLayerType = PFD_MAIN_PLANE;
        int format = ChoosePixelFormat(tempdc, &pfd);
        DescribePixelFormat(tempdc, format, sizeof(pfd), &pfd);
        SetPixelFormat(tempdc, format, &pfd);
        temprc = wglCreateContext(tempdc);
        wglMakeCurrent(tempdc, temprc);
        PFNWGLCREATECONTEXTATTRIBSARBPROC wglCreateContextAttribsARB = (PFNWGLCREATECONTEXTATTRIBSARBPROC)wglGetProcAddress("wglCreateContextAttribsARB");
        int attrib[] =
        {
            WGL_CONTEXT_FLAGS_ARB, WGL_CONTEXT_DEBUG_BIT_ARB,
            WGL_CONTEXT_PROFILE_MASK_ARB, WGL_CONTEXT_COMPATIBILITY_PROFILE_BIT_ARB,
            0,
        };

        HGLRC newrc = wglCreateContextAttribsARB(tempdc, NULL, attrib);
        wglMakeCurrent(tempdc, newrc);
        wglDeleteContext(temprc);
    }

    QAVVideoFrame::HandleType handleType() const override
    {
        return QAVVideoFrame::GLTextureHandle;
    }

    QList<GLuint> texture()
    {
        auto av_frame = frame().frame();
        auto texture = (ID3D11Texture2D *)(uintptr_t)av_frame->data[0];
        auto texture_index = (intptr_t)av_frame->data[1];
        if (!texture)
            return {};

        ID3D11Device *device = nullptr;
        texture->GetDevice(&device);
        if (!device) {
            qWarning() << "Could not GetDevice";
            return {};
        }

        ID3D11DeviceContext *deviceCtx = nullptr;
        device->GetImmediateContext(&deviceCtx);
        if (!deviceCtx) {
            qWarning() << "Could not GetImmediateContext";
            return {};
        }

        if (textures_gl.isEmpty())
        {
            dxDevice = s_wglDXOpenDeviceNV(device);

            CD3D11_TEXTURE2D_DESC desc0(planeFormat(DXGI_FORMAT_NV12, 0), av_frame->width, av_frame->height, 1, 1);
            desc0.BindFlags |= D3D11_BIND_RENDER_TARGET;
            desc0.MiscFlags = D3D11_RESOURCE_MISC_SHARED;

            CD3D11_TEXTURE2D_DESC desc1(planeFormat(DXGI_FORMAT_NV12, 1), av_frame->width, av_frame->height, 1, 1);
            desc1.BindFlags |= D3D11_BIND_RENDER_TARGET;
            desc1.MiscFlags = D3D11_RESOURCE_MISC_SHARED;

            textures_gl.resize(2);
            glGenTextures(1, &textures_gl[0]);
            glGenTextures(1, &textures_gl[1]);

            textures_d3d11.resize(2);
            device->CreateTexture2D(&desc0, NULL, &textures_d3d11[0]);
            device->CreateTexture2D(&desc1, NULL, &textures_d3d11[1]);

            textures_handles.resize(2);
            for (int i = 0; i < textures_d3d11.size(); ++i)
                textures_handles[i] = s_wglDXRegisterObjectNV(dxDevice, textures_d3d11[i], textures_gl[i], GL_TEXTURE_2D, WGL_ACCESS_READ_WRITE_NV);
        }

        for (int i = 0; i < textures_d3d11.size(); ++i)
            deviceCtx->CopySubresourceRegion(textures_d3d11[i], 0, 0, 0, 0,
                                             texture, texture_index, NULL);
        return textures_gl;
    }

    QVariant handle() const override
    {
        QList<GLuint> v = const_cast<VideoBuffer_D3D11_GL *>(this)->texture();
        return QVariant::fromValue(v);
    }

    HANDLE dxDevice = 0;
    QList<ID3D11Texture2D *> textures_d3d11;
    QList<GLuint> textures_gl;
    QList<HANDLE> textures_handles;
};

QAVVideoBuffer *QAVHWDevice_D3D11::videoBuffer(const QAVVideoFrame &frame) const
{
    return new VideoBuffer_D3D11_GL(frame);
}

QT_END_NAMESPACE
