#include "TextureUploader.h"
#include "externals/DirectXTex/d3dx12.h"
using namespace Microsoft::WRL;


//void TextureUploader::UploadMatToTexture(const cv::Mat& image,
//    ComPtr<ID3D12Resource> texture,
//    ComPtr<ID3D12Resource> uploadHeap,
//    ComPtr<ID3D12GraphicsCommandList> cmdList,
//    UINT texWidth,
//    UINT texHeight) {
//    if (image.empty())
//    {
//        OutputDebugStringA("image is empty\n");
//        return;
//    }
//
//    // 必要ならフォーマット変換（OpenCVはBGR、D3D12はRGBA推奨）
//    cv::Mat converted;
//    cv::cvtColor(image, converted, cv::COLOR_BGR2RGBA);
//
//    if (converted.data == nullptr) {
//        OutputDebugStringA("converted.data is nullptr\n");
//        return;
//    }
//    
//
//    D3D12_SUBRESOURCE_DATA subresource = {};
//    subresource.pData = converted.data;
//    subresource.RowPitch = converted.cols*converted.elemSize();// RGBA = 4バイト
//    subresource.SlicePitch = subresource.RowPitch *converted.rows;
//
//    UpdateSubresources(cmdList.Get(), texture.Get(), uploadHeap.Get(), 0, 0, 1, &subresource);
//}

//void TextureUploader::UploadRawToTexture(
//    const uint8_t* rawRGBA,
//    ComPtr<ID3D12Resource> texture,
//    ComPtr<ID3D12Resource> uploadHeap,
//    ComPtr<ID3D12GraphicsCommandList> cmdList,
//    UINT texWidth,
//    UINT texHeight){
//
//    if (rawRGBA == nullptr) {
//        OutputDebugStringA("rawRGBA is nullptr\n");
//        return;
//    }
//
//    D3D12_SUBRESOURCE_DATA subresource = {};
//    subresource.pData = rawRGBA;
//    subresource.RowPitch = texWidth * 4;  // RGBA = 4バイト
//    subresource.SlicePitch = subresource.RowPitch * texHeight;
//
//    UpdateSubresources(cmdList.Get(), texture.Get(), uploadHeap.Get(), 0, 0, 1, &subresource);
//}
//
//void TextureUploader::UploadNV12ToTexture(
//    const uint8_t* nv12Data,
//    size_t ySize,
//    size_t uvSize,
//    ComPtr<ID3D12Resource> texture,
//    ComPtr<ID3D12Resource> uploadHeap,
//    ComPtr<ID3D12GraphicsCommandList> cmdList,
//    UINT width,
//    UINT height)
//{
//    if (nv12Data == nullptr) {
//        OutputDebugStringA("nv12Data is nullptr\n");
//        return;
//    }
//
//    D3D12_SUBRESOURCE_DATA subresource = {};
//    subresource.pData = nv12Data;
//    subresource.RowPitch = width;                 // 1byte per pixel (Y, UV)
//    subresource.SlicePitch = ySize + uvSize;      // 全体のバイト数
//
//    UpdateSubresources(cmdList.Get(), texture.Get(), uploadHeap.Get(), 0, 0, 1, &subresource);
//}
