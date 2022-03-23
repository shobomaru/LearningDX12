#include <Windows.h>
#include <tchar.h>
#include <wrl/client.h>
#include <stdexcept>
#include <dxgi1_4.h>
#include <d3d12.h>
#include "d3dx12.h"

#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dxguid.lib")
#pragma comment(lib, "d3d12.lib")

using namespace std;
using Microsoft::WRL::ComPtr;

namespace
{
	const int WINDOW_WIDTH = 640;
	const int WINDOW_HEIGHT = 360;
	const int BUFFER_COUNT = 3;
	HWND g_mainWindowHandle = 0;
};

void CHK(HRESULT hr)
{
	if (FAILED(hr))
		throw runtime_error("HRESULT is failed value.");
}

class D3D
{
	ComPtr<IDXGIFactory2> mDxgiFactory;
	ComPtr<ID3D12Device> mDevice;
	uint32_t mRTVStride;
	ComPtr<ID3D12CommandAllocator> mCmdAlloc[BUFFER_COUNT];
	ComPtr<ID3D12CommandQueue> mCmdQueue;
	ComPtr<IDXGISwapChain3> mSwapChain;
	ComPtr<ID3D12GraphicsCommandList> mCmdList;
	uint64_t mFrameCount = 0;
	ComPtr<ID3D12Fence> mFence;
	ComPtr<ID3D12Resource> mSwapChainTex[BUFFER_COUNT];
	ComPtr<ID3D12DescriptorHeap> mSwapChainRTVs;

	ComPtr<ID3D12Heap> mRedHeap;
	ComPtr<ID3D12Heap> mGreenHeap;
	ComPtr<ID3D12Resource> mRedTex;
	ComPtr<ID3D12Resource> mGreenTex;
	ComPtr<ID3D12DescriptorHeap> mRedRTV;
	ComPtr<ID3D12DescriptorHeap> mGreenRTV;

	int mTileCountX;
	int mTileCountY;
	ComPtr<ID3D12Resource> mTileTex;

	int Align(int val, int align)
	{
		return ((val + align - 1) & ~(align - 1));
	}

public:
	~D3D()
	{
		mFrameCount++;

		// Wait GPU command completion
		mCmdQueue->Signal(mFence.Get(), mFrameCount);
		while (mFence->GetCompletedValue() < mFrameCount)
		{
			SwitchToThread();
		}
	}

	D3D(int width, int height, HWND hWnd)
	{
		UINT factoryFlags = 0;
#ifdef _DEBUG
		factoryFlags = DXGI_CREATE_FACTORY_DEBUG;
#endif
		CHK(CreateDXGIFactory2(factoryFlags, IID_PPV_ARGS(&mDxgiFactory)));

#if _DEBUG
		ComPtr<ID3D12Debug> debug = nullptr;
		D3D12GetDebugInterface(IID_PPV_ARGS(&debug));
		if (debug)
		{
			debug->EnableDebugLayer();
			debug.Reset();
		}
#endif
		CHK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&mDevice)));
		mRTVStride = mDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

		D3D12_FEATURE_DATA_D3D12_OPTIONS feature = {};
		CHK(mDevice->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &feature, sizeof(feature)));
		if (feature.TiledResourcesTier < D3D12_TILED_RESOURCES_TIER_2) {
			throw std::exception("Tiled resource is unsupported.");
		}

		for (int i = 0; i < BUFFER_COUNT; i++)
		{
			CHK(mDevice->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&mCmdAlloc[i])));
		}

		D3D12_COMMAND_QUEUE_DESC queueDesc = {};
		queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
		CHK(mDevice->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&mCmdQueue)));

		DXGI_SWAP_CHAIN_DESC1 sc = {};
		sc.Width = WINDOW_WIDTH;
		sc.Height = WINDOW_HEIGHT;
		sc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		sc.SampleDesc.Count = 1;
		sc.BufferCount = BUFFER_COUNT;
		sc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
		sc.BufferUsage = DXGI_USAGE_BACK_BUFFER | DXGI_USAGE_RENDER_TARGET_OUTPUT;
		ComPtr<IDXGISwapChain1> tempSwapChain;
		CHK(mDxgiFactory->CreateSwapChainForHwnd(mCmdQueue.Get(), hWnd, &sc, nullptr, nullptr, &tempSwapChain));
		CHK(tempSwapChain.As(&mSwapChain));
		CHK(mDxgiFactory->MakeWindowAssociation(hWnd, DXGI_MWA_NO_ALT_ENTER));

		CHK(mDevice->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, mCmdAlloc[0].Get(), nullptr, IID_PPV_ARGS(&mCmdList)));
		mCmdList->Close();

		CHK(mDevice->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&mFence)));

		D3D12_DESCRIPTOR_HEAP_DESC descHeapDesc = {};
		descHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
		descHeapDesc.NumDescriptors = 10;
		CHK(mDevice->CreateDescriptorHeap(&descHeapDesc, IID_PPV_ARGS(&mSwapChainRTVs)));

		CD3DX12_CPU_DESCRIPTOR_HANDLE swapChainRTVHandle(mSwapChainRTVs->GetCPUDescriptorHandleForHeapStart());
		for (int i = 0; i < BUFFER_COUNT; i++)
		{
			CHK(mSwapChain->GetBuffer(i, IID_PPV_ARGS(&mSwapChainTex[i])));
			mDevice->CreateRenderTargetView(mSwapChainTex[i].Get(), nullptr, swapChainRTVHandle);
			swapChainRTVHandle.Offset(1, mRTVStride);
		}

		// Create tile data
		auto resDesc = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R8G8B8A8_UNORM, 128, 128, 1, 1);
		resDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
		resDesc.Layout = D3D12_TEXTURE_LAYOUT_64KB_UNDEFINED_SWIZZLE;
		auto allocInfo = mDevice->GetResourceAllocationInfo(0, 1, &resDesc);
		auto heapInfo = CD3DX12_HEAP_DESC(allocInfo, D3D12_HEAP_TYPE_DEFAULT);
		CHK(mDevice->CreateHeap(&heapInfo, IID_PPV_ARGS(&mRedHeap)));
		CHK(mDevice->CreateHeap(&heapInfo, IID_PPV_ARGS(&mGreenHeap)));

		float clearColorRed[4] = { 1.0f, 0.0f, 0.0f, 1.0f };
		float clearColorGreen[4] = { 0.0f, 1.0f, 0.0f, 1.0f };
		auto clearValueRed = CD3DX12_CLEAR_VALUE(DXGI_FORMAT_R8G8B8A8_UNORM, clearColorRed);
		auto clearValueGreen = CD3DX12_CLEAR_VALUE(DXGI_FORMAT_R8G8B8A8_UNORM, clearColorGreen);
		CHK(mDevice->CreatePlacedResource(mRedHeap.Get(), 0, &resDesc,
			D3D12_RESOURCE_STATE_GENERIC_READ, &clearValueRed, IID_PPV_ARGS(&mRedTex)));
		CHK(mDevice->CreatePlacedResource(mGreenHeap.Get(), 0, &resDesc,
			D3D12_RESOURCE_STATE_GENERIC_READ, &clearValueGreen, IID_PPV_ARGS(&mGreenTex)));

		descHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
		descHeapDesc.NumDescriptors = 1;
		CHK(mDevice->CreateDescriptorHeap(&descHeapDesc, IID_PPV_ARGS(&mRedRTV)));
		CHK(mDevice->CreateDescriptorHeap(&descHeapDesc, IID_PPV_ARGS(&mGreenRTV)));
		mDevice->CreateRenderTargetView(mRedTex.Get(), nullptr, mRedRTV->GetCPUDescriptorHandleForHeapStart());
		mDevice->CreateRenderTargetView(mGreenTex.Get(), nullptr, mGreenRTV->GetCPUDescriptorHandleForHeapStart());

		// Create tiled texture, not allocate a commited memory
		resDesc = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R8G8B8A8_UNORM, WINDOW_WIDTH, WINDOW_HEIGHT, 1, 1);
		resDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
		resDesc.Layout = D3D12_TEXTURE_LAYOUT_64KB_UNDEFINED_SWIZZLE;
		CHK(mDevice->CreateReservedResource(&resDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&mTileTex)));

		// Tile Pool
		// 32bpp * 128 * 128 = 64KB per tile
		mTileCountX = Align(width, 128) / 128;
		mTileCountY = Align(height, 128) / 128;
	}

	void Draw()
	{
		mFrameCount++;
		auto frameIndex = mSwapChain->GetCurrentBackBufferIndex();

		//-------------------------------

		// Start recording commands
		CHK(mCmdAlloc[mFrameCount % BUFFER_COUNT]->Reset());
		CHK(mCmdList->Reset(mCmdAlloc[mFrameCount % BUFFER_COUNT].Get(), nullptr));

		// Clear tile data
		auto transition = CD3DX12_RESOURCE_BARRIER::Transition(mRedTex.Get(),
			D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_RENDER_TARGET);
		mCmdList->ResourceBarrier(1, &transition);
		transition = CD3DX12_RESOURCE_BARRIER::Transition(mGreenTex.Get(),
			D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_RENDER_TARGET);
		mCmdList->ResourceBarrier(1, &transition);

		float clearColorRed[4] = { 1.0f, 0.0f, 0.0f, 1.0f };
		float clearColorGreen[4] = { 0.0f, 1.0f, 0.0f, 1.0f };
		mCmdList->ClearRenderTargetView(mRedRTV->GetCPUDescriptorHandleForHeapStart(), clearColorRed, 0, nullptr);
		mCmdList->ClearRenderTargetView(mGreenRTV->GetCPUDescriptorHandleForHeapStart(), clearColorGreen, 0, nullptr);

		transition = CD3DX12_RESOURCE_BARRIER::Transition(mRedTex.Get(),
			D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_GENERIC_READ);
		mCmdList->ResourceBarrier(1, &transition);
		transition = CD3DX12_RESOURCE_BARRIER::Transition(mGreenTex.Get(),
			D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_GENERIC_READ);
		mCmdList->ResourceBarrier(1, &transition);

		// Clear swap chain for debug
		transition = CD3DX12_RESOURCE_BARRIER::Transition(mSwapChainTex[frameIndex].Get(),
			D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
		mCmdList->ResourceBarrier(1, &transition);

		float clearColor[4] = { 0.1f, 0.2f, 0.4f, 1.0f };
		CD3DX12_CPU_DESCRIPTOR_HANDLE swapChainRTVHandle(mSwapChainRTVs->GetCPUDescriptorHandleForHeapStart());
		swapChainRTVHandle.Offset(frameIndex, mRTVStride);
		mCmdList->ClearRenderTargetView(swapChainRTVHandle, clearColor, 0, nullptr);

		transition = CD3DX12_RESOURCE_BARRIER::Transition(mSwapChainTex[frameIndex].Get(),
			D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
		mCmdList->ResourceBarrier(1, &transition);

		// Copy from tiled texture to swap chain
		transition = CD3DX12_RESOURCE_BARRIER::Transition(mSwapChainTex[frameIndex].Get(),
			D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);
		mCmdList->ResourceBarrier(1, &transition);

		mCmdList->CopyResource(mSwapChainTex[frameIndex].Get(), mTileTex.Get());

		transition = CD3DX12_RESOURCE_BARRIER::Transition(mSwapChainTex[frameIndex].Get(),
			D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
		mCmdList->ResourceBarrier(1, &transition);

		// Finish recording commands
		CHK(mCmdList->Close());

		//-------------------------------

		// Make checker pattern
		const int tileCount = mTileCountX * mTileCountY;
		auto *coord = (D3D12_TILED_RESOURCE_COORDINATE*)alloca(sizeof(D3D12_TILED_RESOURCE_COORDINATE) * mTileCountX * mTileCountY);
		auto *region = (D3D12_TILE_REGION_SIZE*)alloca(sizeof(D3D12_TILE_REGION_SIZE) * mTileCountX * mTileCountY);
		for (int y = 0; y < mTileCountY; y++)
		{
			for (int x = 0; x < mTileCountX; x++)
			{
				int id = y * mTileCountX + x;
				if (id % 2 == 0)
				{
					coord[id / 2] = CD3DX12_TILED_RESOURCE_COORDINATE(x, y, 0, 0);
					region[id / 2] = CD3DX12_TILE_REGION_SIZE(1, FALSE, 0, 0, 0);
				}
				else
				{
					coord[id / 2 + (tileCount + 1) / 2] = CD3DX12_TILED_RESOURCE_COORDINATE(x, y, 0, 0);
					region[id / 2 + (tileCount + 1) / 2] = CD3DX12_TILE_REGION_SIZE(1, FALSE, 0, 0, 0);
				}
			}
		}

		// Map tile before rendering
		// Red
#if 0
		// REUSE_SINGLE_TILE
		D3D12_TILE_RANGE_FLAGS tileMappingFlags = D3D12_TILE_RANGE_FLAG_REUSE_SINGLE_TILE;
		uint32_t tilePoolCount = ((tileCount + 1) / 2);
		uint32_t tilePoolOffset = 0;
		mCmdQueue->UpdateTileMappings(
			mTileTex.Get(), ((tileCount + 1) / 2),
			coord, region,
			mRedHeap.Get(), 1, &tileMappingFlags, &tilePoolOffset, &tilePoolCount, D3D12_TILE_MAPPING_FLAG_NONE);
#else
		// NONE
		D3D12_TILE_RANGE_FLAGS tileMappingFlags[8] = { D3D12_TILE_RANGE_FLAG_NONE };
		uint32_t tilePoolCount[8] = { 1,1,1,1,1,1,1,1 };
		uint32_t tilePoolOffset[8] = { 0,0,0,0,0,0,0,0 };
		mCmdQueue->UpdateTileMappings(
			mTileTex.Get(), ((tileCount + 1) / 2),
			coord, region,
			mRedHeap.Get(), 8, tileMappingFlags, tilePoolOffset, tilePoolCount, D3D12_TILE_MAPPING_FLAG_NONE);
#endif
		// Green
#if 0
		tileMappingFlags = D3D12_TILE_RANGE_FLAG_REUSE_SINGLE_TILE;
		tilePoolCount = (tileCount / 2);
		tilePoolOffset = 0;
		mCmdQueue->UpdateTileMappings(
			mTileTex.Get(), (tileCount / 2),
			coord + (tileCount + 1) / 2, region + (tileCount + 1) / 2,
			mGreenHeap.Get(), 1, &tileMappingFlags, &tilePoolOffset, &tilePoolCount, D3D12_TILE_MAPPING_FLAG_NONE);
#else
		mCmdQueue->UpdateTileMappings(
			mTileTex.Get(), (tileCount / 2),
			coord + (tileCount + 1) / 2, region + (tileCount + 1) / 2,
			mGreenHeap.Get(), 7, tileMappingFlags, tilePoolOffset, tilePoolCount, D3D12_TILE_MAPPING_FLAG_NONE);
#endif

		// Execute recorded commands
		mCmdQueue->ExecuteCommandLists(1, CommandListCast(mCmdList.GetAddressOf()));

		// Unmap tile after rendering
		auto tileMappingFlagsNull = D3D12_TILE_RANGE_FLAG_NULL;
		mCmdQueue->UpdateTileMappings(mTileTex.Get(), 1, nullptr, nullptr, nullptr,
			1, &tileMappingFlagsNull, nullptr, nullptr, D3D12_TILE_MAPPING_FLAG_NONE);

		CHK(mCmdQueue->Signal(mFence.Get(), mFrameCount));
	}

	void Present()
	{
		while ((mFence->GetCompletedValue() + 1) < mFrameCount)
		{
			SwitchToThread();
		}
		DXGI_PRESENT_PARAMETERS pp = {};
		CHK(mSwapChain->Present1(0, 0, &pp));

		return;
	}
};

LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
	switch (message) {
	case WM_KEYDOWN:
		if (wParam == VK_ESCAPE) {
			PostMessage(hWnd, WM_DESTROY, 0, 0);
			return 0;
		}
		break;
	case WM_DESTROY:
		PostQuitMessage(0);
		break;
	default:
		return DefWindowProc(hWnd, message, wParam, lParam);
	}
	return 0;
}

static HWND setupWindow(int width, int height)
{
	WNDCLASSEX wcex;
	wcex.cbSize = sizeof(WNDCLASSEX);
	wcex.style = CS_HREDRAW | CS_VREDRAW;
	wcex.lpfnWndProc = WndProc;
	wcex.cbClsExtra = 0;
	wcex.cbWndExtra = 0;
	wcex.hInstance = (HMODULE)GetModuleHandle(0);
	wcex.hIcon = nullptr;
	wcex.hCursor = LoadCursor(nullptr, IDC_ARROW);
	wcex.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
	wcex.lpszMenuName = nullptr;
	wcex.lpszClassName = _T("WindowClass");
	wcex.hIconSm = nullptr;
	if (!RegisterClassEx(&wcex)) {
		throw runtime_error("RegisterClassEx()");
	}

	RECT rect = { 0, 0, width, height };
	AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
	const int windowWidth = (rect.right - rect.left);
	const int windowHeight = (rect.bottom - rect.top);

	HWND hWnd = CreateWindow(_T("WindowClass"), _T("Window"),
		WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, 0, windowWidth, windowHeight,
		nullptr, nullptr, nullptr, nullptr);
	if (!hWnd) {
		throw runtime_error("CreateWindow()");
	}

	return hWnd;
}

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int)
{
	MSG msg;
	ZeroMemory(&msg, sizeof msg);

#ifdef NDEBUG
	try
#endif
	{
		g_mainWindowHandle = setupWindow(WINDOW_WIDTH, WINDOW_HEIGHT);
		ShowWindow(g_mainWindowHandle, SW_SHOW);
		UpdateWindow(g_mainWindowHandle);

		D3D d3d(WINDOW_WIDTH, WINDOW_HEIGHT, g_mainWindowHandle);

		while (msg.message != WM_QUIT) {
			BOOL r = PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE);
			if (r == 0) {
				d3d.Draw();
				d3d.Present();
			}
			else {
				DispatchMessage(&msg);
			}
		}
	}
#ifdef NDEBUG
	catch (std::exception &e) {
		MessageBoxA(g_mainWindowHandle, e.what(), "Exception occuured.", MB_ICONSTOP);
	}
#endif

	return static_cast<int>(msg.wParam);
}
