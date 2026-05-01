#pragma once
#include <d3d12.h>
#include <wrl/client.h>
using namespace Microsoft::WRL;

class TextureUploader {
public:
   /* void UploadMatToTexture(const cv::Mat& image,
        ComPtr<ID3D12Resource> texture,
        ComPtr<ID3D12Resource> uploadHeap,
        ComPtr<ID3D12GraphicsCommandList> cmdList,
        UINT texWidth,
        UINT texHeight);*/

   /* void UploadRawToTexture(
        const uint8_t* rawRGBA,
        ComPtr<ID3D12Resource> texture,
        ComPtr<ID3D12Resource> uploadHeap,
        ComPtr<ID3D12GraphicsCommandList> cmdList,
        UINT texWidth,
        UINT texHeight);

    static void UploadNV12ToTexture(
        const uint8_t* nv12Data,
        size_t ySize,
        size_t uvSize,
        ComPtr<ID3D12Resource> texture,
        ComPtr<ID3D12Resource> uploadHeap,
        ComPtr<ID3D12GraphicsCommandList> cmdList,
        UINT width,
        UINT height);*/
};