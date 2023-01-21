#include <fstream>
#include <iostream>
#include <vector>
#include <stdexcept>
#define INITGUID
#include <wsl/wrladapter.h>
#include <directx/dxcore.h>
#include <directx/d3d12.h>
#include <dxguids/dxguids.h>

using namespace std;
using namespace Microsoft::WRL;

#define STRINGIFY(n) #n
#define TOSTRING(n) STRINGIFY(n)
#define CHK(hr) { auto e = ( hr ); if (FAILED(e)) { cout << hex << e << endl; throw runtime_error( __FILE__ "@" TOSTRING( __LINE__ ) ); } }

constexpr uint32_t WIDTH = 256;
constexpr uint32_t HEIGHT = 256;

auto Align(auto val, auto align)
{
	return ((val + align - 1) & ~(align - 1));
}

int main(int argc, char** argv)
{
	cout << "Start" << endl;
	// Get an adapter
	ComPtr<IDXCoreAdapterFactory> adapterFactory;
	CHK(DXCoreCreateAdapterFactory(IID_PPV_ARGS(&adapterFactory)));
	ComPtr<IDXCoreAdapterList> adapterList;
	GUID attributes[]{ DXCORE_ADAPTER_ATTRIBUTE_D3D12_GRAPHICS };
	CHK(adapterFactory->CreateAdapterList(1, attributes, IID_PPV_ARGS(&adapterList)));
	DXCoreAdapterPreference sortPreferences[]{ DXCoreAdapterPreference::Hardware, DXCoreAdapterPreference::HighPerformance };
	CHK(adapterList->Sort(2, sortPreferences));
	ComPtr<IDXCoreAdapter> adapter;
	CHK(adapterList->GetAdapter(0, IID_PPV_ARGS(&adapter)));
	DXCoreHardwareID hwid = {};
	CHK(adapter->GetProperty(DXCoreAdapterProperty::HardwareID, &hwid));
	cout << "HWID: " << hex << hwid.vendorID << "," << hwid.deviceID << "," << hwid.subSysID << "," << hwid.revision << endl;	
	bool isHW = {};
	CHK(adapter->GetProperty(DXCoreAdapterProperty::IsHardware, &isHW));
	cout << "IsHW: " << (isHW ? "Yes" : "No") << endl;
	uint64_t vram = {};
	CHK(adapter->GetProperty(DXCoreAdapterProperty::DedicatedAdapterMemory, &vram));
	cout << "VRAM: " << vram << " byte" << endl;
	// Create a device
	ComPtr<ID3D12Device> device;
	CHK(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_1, IID_PPV_ARGS(&device)));
	// Create resourcews
	ComPtr<ID3D12CommandAllocator> cmdAlloc;
	CHK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&cmdAlloc)));
	const D3D12_COMMAND_QUEUE_DESC queueDesc = { D3D12_COMMAND_LIST_TYPE_DIRECT };
	ComPtr<ID3D12CommandQueue> cmdQueue;
	CHK(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&cmdQueue)));
	ComPtr<ID3D12GraphicsCommandList> cmdList;
	CHK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, cmdAlloc.Get(), nullptr, IID_PPV_ARGS(&cmdList)));
	ComPtr<ID3D12Fence> fence;
	CHK(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
	// Create a rendering texture
	const D3D12_RESOURCE_DESC targetTexDesc = {
		.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D,
		.Width = WIDTH,
		.Height = HEIGHT,
		.DepthOrArraySize = 1,
		.MipLevels = 1,
		.Format = DXGI_FORMAT_R8G8B8A8_UNORM,
		.SampleDesc = {1, 0},
		.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
	};
	const D3D12_HEAP_PROPERTIES targetHeapProp = { D3D12_HEAP_TYPE_DEFAULT };
	ComPtr<ID3D12Resource> targetTex;
	CHK(device->CreateCommittedResource(&targetHeapProp, D3D12_HEAP_FLAG_NONE, &targetTexDesc, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&targetTex)));
	const D3D12_DESCRIPTOR_HEAP_DESC descHeapRtvDesc = { D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 10 };
	ComPtr<ID3D12DescriptorHeap> descHeapRtv;
	CHK(device->CreateDescriptorHeap(&descHeapRtvDesc, IID_PPV_ARGS(&descHeapRtv)));
	device->CreateRenderTargetView(targetTex.Get(), nullptr, descHeapRtv->GetCPUDescriptorHandleForHeapStart());
	// Create a readback buffer
	const D3D12_RESOURCE_DESC readbackBufDesc = {
		.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER,
		.Width = Align(4 * WIDTH, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT) * HEIGHT,
		.Height = 1,
		.DepthOrArraySize = 1,
		.MipLevels = 1,
		.Format = DXGI_FORMAT_UNKNOWN,
		.SampleDesc = {1, 0},
		.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
	};
	const D3D12_HEAP_PROPERTIES readbackHeapProp = { D3D12_HEAP_TYPE_READBACK };
	ComPtr<ID3D12Resource> readbackBuf;
	CHK(device->CreateCommittedResource(&readbackHeapProp, D3D12_HEAP_FLAG_NONE, &readbackBufDesc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readbackBuf)));
	// Clear that texture
	const auto h_rtv = descHeapRtv->GetCPUDescriptorHandleForHeapStart();
	const float clearColor[4] = { 0.1f, 0.2f, 0.4f, 1.0f };
	cmdList->ClearRenderTargetView(h_rtv, clearColor, 0, nullptr);
	const D3D12_RESOURCE_BARRIER barrier = {
		.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,
		.Transition = { targetTex.Get(), 0, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE },
	};
	cmdList->ResourceBarrier(1, &barrier);
	const D3D12_TEXTURE_COPY_LOCATION locSrc = {
		.pResource = targetTex.Get(),
		.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,
		.SubresourceIndex = 0,
	};
	const D3D12_TEXTURE_COPY_LOCATION locDst = {
		.pResource = readbackBuf.Get(),
		.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT,
		.PlacedFootprint = { .Offset = 0, .Footprint = { DXGI_FORMAT_R8G8B8A8_UNORM, WIDTH, HEIGHT, 1, Align(4 * WIDTH, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT) } },
	};
	cmdList->CopyTextureRegion(&locDst, 0, 0, 0, &locSrc, nullptr);
	// Execute GPU commands and wait
	CHK(cmdList->Close());
	ID3D12CommandList* cmdListP = cmdList.Get();
	cmdQueue->ExecuteCommandLists(1, &cmdListP);
	CHK(cmdQueue->Signal(fence.Get(), 1));
	fence->SetEventOnCompletion(1, (HANDLE)NULL);
	// Readback rendered data
	vector<uint8_t> pix;
	pix.resize(3 * WIDTH * HEIGHT);
	uint32_t *srcPix;
	CHK(readbackBuf->Map(0, nullptr, reinterpret_cast<void**>(&srcPix)));
	cout << "Pixel[0, 0]: " << hex << *srcPix << endl;
	for (int y = 0; y < HEIGHT; ++y)
	{
		auto pitch = Align(4 * WIDTH, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
		auto *dst = pix.data() + 3 * y * WIDTH;
		auto *src = reinterpret_cast<uint8_t*>(srcPix) + y * pitch;
		for (int x = 0; x < WIDTH; ++x)
		{
			dst[3 * x + 0] = src[4 * x + 0];
			dst[3 * x + 1] = src[4 * x + 1];
			dst[3 * x + 2] = src[4 * x + 2];
		}
	}
	readbackBuf->Unmap(0, nullptr);
	// Write PPM
	ofstream ofs("image.ppm", ios::binary | ios::trunc);
	if (!ofs)
	{
		cout << "Cannot open image file." << endl;
		return 1;
	}
	ofs << "P6" << endl << "# test" << endl << WIDTH << " " << HEIGHT << endl << "255" << endl;
	ofs.write(reinterpret_cast<char*>(pix.data()), pix.size());
	ofs << endl;
	ofs.close();
	cout << "End" << endl;
	return 0;
}

