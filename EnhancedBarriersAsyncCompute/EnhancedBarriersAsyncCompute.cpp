#include <Windows.h>
#include <tchar.h>
#include <wrl/client.h>
#include <stdexcept>
#include <dxgi1_4.h>
#include <d3d12.h>
#include "d3dx12.h"
#include <d3dcompiler.h>

#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dxguid.lib")
#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "d3dcompiler.lib")

extern "C" { __declspec(dllexport) extern const UINT D3D12SDKVersion = 700; }
extern "C" { __declspec(dllexport) extern const auto* D3D12SDKPath = u8".\\..\\..\\dll\\1.700\\"; }

// Enhanced barries does not have implicit transitions
//#define USE_DX12_IMPLICIT_STATE_TRANSITIONS 0

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
	uint32_t mResourceStride;
	ComPtr<ID3D12CommandAllocator> mCmdAlloc[BUFFER_COUNT];
	ComPtr<ID3D12CommandQueue> mCmdQueue;
	ComPtr<IDXGISwapChain3> mSwapChain;
	ComPtr<ID3D12GraphicsCommandList> mCmdList;
	uint64_t mFrameCount = 0;
	ComPtr<ID3D12Fence> mFence;
	ComPtr<ID3D12Resource> mSwapChainTex[BUFFER_COUNT];
	ComPtr<ID3D12DescriptorHeap> mSwapChainRTVs;

	ComPtr<ID3D12CommandAllocator> mAsyncCmdAlloc[BUFFER_COUNT];
	ComPtr<ID3D12CommandQueue> mAsyncCmdQueue;
	ComPtr<ID3D12GraphicsCommandList> mAsyncCmdList;
	ComPtr<ID3D12Fence> mAsyncFence;
	ComPtr<ID3D12Resource> mOffscreenTex;
	ComPtr<ID3D12DescriptorHeap> mOffscreenUAV;

	ComPtr<ID3D12RootSignature> mRootSig;
	ComPtr<ID3D12PipelineState> mPSO;

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

		// Currently enhanced barriers only suppors Microsoft Basic Render Driver
		ComPtr<IDXGIAdapter1> adapter;
		for (UINT i = 0; mDxgiFactory->EnumAdapters1(i, &adapter) == S_OK; ++i)
		{
			DXGI_ADAPTER_DESC1 desc;
			CHK(adapter->GetDesc1(&desc));
			if (desc.Flags == DXGI_ADAPTER_FLAG_SOFTWARE)
				break;
		}

#if _DEBUG
		ComPtr<ID3D12Debug> debug = nullptr;
		D3D12GetDebugInterface(IID_PPV_ARGS(&debug));
		if (debug)
		{
			debug->EnableDebugLayer();
			ComPtr<ID3D12Debug3> debug3;
			CHK(debug.As(&debug3));
			// GBV does not support enhanced barriers?
			//debug3->SetEnableGPUBasedValidation(TRUE);
			debug3->SetEnableSynchronizedCommandQueueValidation(TRUE);
			ComPtr<ID3D12Debug6> debug6;
			CHK(debug.As(&debug6));
			debug6->SetEnableAutoName(TRUE);
			debug6->SetForceLegacyBarrierValidation(FALSE);
			debug.Reset();
		}
#endif
		CHK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&mDevice)));
		D3D12_FEATURE_DATA_D3D12_OPTIONS12 options12 = {};
		CHK(mDevice->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS12, &options12, sizeof options12));
		if (!options12.EnhancedBarriersSupported)
			throw runtime_error("Enhanced barriers not supported.");

		mResourceStride = mDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
		mRTVStride = mDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

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

		// Asnyc Compute

		for (int i = 0; i < BUFFER_COUNT; i++)
		{
			CHK(mDevice->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COMPUTE, IID_PPV_ARGS(&mAsyncCmdAlloc[i])));
			mAsyncCmdAlloc[i]->SetName(L"AsyncCmdAlloc");
		}

		queueDesc.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
		CHK(mDevice->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&mAsyncCmdQueue)));
		mAsyncCmdQueue->SetName(L"AsyncCmdQueue");

		CHK(mDevice->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COMPUTE, mAsyncCmdAlloc[0].Get(), nullptr, IID_PPV_ARGS(&mAsyncCmdList)));
		mAsyncCmdList->SetName(L"AsyncCmdList");
		mAsyncCmdList->Close();

		CHK(mDevice->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&mAsyncFence)));

		// Shader

		CD3DX12_DESCRIPTOR_RANGE descRange[1];
		descRange[0].Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0);
		CD3DX12_ROOT_PARAMETER rootParam[1];
		rootParam[0].InitAsDescriptorTable(1, descRange);
		CD3DX12_ROOT_SIGNATURE_DESC rootSigDesc;
		rootSigDesc.Init(_countof(rootParam), rootParam);

		ComPtr<ID3DBlob> rootSigBlob, rootSigError;
		CHK(D3D12SerializeRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1, &rootSigBlob, &rootSigError));
		CHK(mDevice->CreateRootSignature(0, rootSigBlob->GetBufferPointer(), rootSigBlob->GetBufferSize(), IID_PPV_ARGS(&mRootSig)));

		static const char* shaderCode = R"#(
RWTexture2D<unorm float4> tex;
[numthreads(8, 8, 1)]
void main(uint2 id : SV_DispatchThreadID) {
	tex[id] = float4((float2)(id % 128) / 127.0, 0.0, 1.0);
}
)#";

		ComPtr<ID3DBlob> shaderBlob, shaderError;
		D3DCompile(shaderCode, strlen(shaderCode) - 1, nullptr, nullptr, nullptr, "main", "cs_5_1",
			D3DCOMPILE_ALL_RESOURCES_BOUND, 0, &shaderBlob, &shaderError);
		if (shaderError) {
			OutputDebugStringA(reinterpret_cast<char*>(shaderError->GetBufferPointer()));
			throw runtime_error("Shader compile error.");
		}

		D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc = {};
		psoDesc.pRootSignature = mRootSig.Get();
		psoDesc.CS = CD3DX12_SHADER_BYTECODE(shaderBlob.Get());
		CHK(mDevice->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&mPSO)));

		// Resource

		ComPtr<ID3D12Device10> device10;
		CHK(mDevice.As(&device10));

		auto heapProp = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
		auto resDesc = CD3DX12_RESOURCE_DESC1::Tex2D(DXGI_FORMAT_R8G8B8A8_UNORM, WINDOW_WIDTH, WINDOW_HEIGHT, 1, 1);
		resDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
		CHK(device10->CreateCommittedResource3(
			&heapProp, D3D12_HEAP_FLAG_NONE, &resDesc,
			D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr,
			0, nullptr, IID_PPV_ARGS(&mOffscreenTex)));

		descHeapDesc = {};
		descHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
		descHeapDesc.NumDescriptors = 10;
		descHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
		CHK(mDevice->CreateDescriptorHeap(&descHeapDesc, IID_PPV_ARGS(&mOffscreenUAV)));

		CD3DX12_CPU_DESCRIPTOR_HANDLE offscreenUAVHandle(mOffscreenUAV->GetCPUDescriptorHandleForHeapStart());
		mDevice->CreateUnorderedAccessView(mOffscreenTex.Get(), nullptr, nullptr, offscreenUAVHandle);
	}

	void Draw()
	{
		mFrameCount++;
		auto frameIndex = mSwapChain->GetCurrentBackBufferIndex();

		ComPtr<ID3D12GraphicsCommandList7> cmdList7, asyncCmdList7;
		CHK(mCmdList.As(&cmdList7));
		CHK(mAsyncCmdList.As(&asyncCmdList7));

		//-------------------------------

		// Start recording commands
		CHK(mCmdAlloc[mFrameCount % BUFFER_COUNT]->Reset());
		CHK(mCmdList->Reset(mCmdAlloc[mFrameCount % BUFFER_COUNT].Get(), nullptr));

		CHK(mAsyncCmdAlloc[mFrameCount % BUFFER_COUNT]->Reset());
		CHK(mAsyncCmdList->Reset(mAsyncCmdAlloc[mFrameCount % BUFFER_COUNT].Get(), nullptr));

		// Draw an image to offscreen
		{
			CD3DX12_TEXTURE_BARRIER barrierTex[1] = {
				// I want to change the resource layout D3D12_BARRIER_LAYOUT_COMPUTE_QUEUE_UNORDERED_ACCESS
				// but is disallowed because direct queue cannot use that layout.
				CD3DX12_TEXTURE_BARRIER(
					D3D12_BARRIER_SYNC_NONE, D3D12_BARRIER_SYNC_COMPUTE_SHADING,
					D3D12_BARRIER_ACCESS_NO_ACCESS, D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,
					D3D12_BARRIER_LAYOUT_UNDEFINED, D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS,
					mOffscreenTex.Get(), CD3DX12_BARRIER_SUBRESOURCE_RANGE(0),
					D3D12_TEXTURE_BARRIER_FLAG_NONE)
			};
			CD3DX12_BARRIER_GROUP barriers[] = {
				CD3DX12_BARRIER_GROUP(_countof(barrierTex), barrierTex)
			};
			asyncCmdList7->Barrier(_countof(barriers), barriers);
		}

		mAsyncCmdList->SetComputeRootSignature(mRootSig.Get());
		mAsyncCmdList->SetPipelineState(mPSO.Get());
		mAsyncCmdList->SetDescriptorHeaps(1, mOffscreenUAV.GetAddressOf());
		mAsyncCmdList->SetComputeRootDescriptorTable(0, mOffscreenUAV->GetGPUDescriptorHandleForHeapStart());
		mAsyncCmdList->Dispatch(Align(WINDOW_WIDTH, 8) / 8, Align(WINDOW_HEIGHT, 8) / 8, 1);

		// Copy from offscrren image to swap chain
		{
			CD3DX12_TEXTURE_BARRIER barrierTex[1] = {
				CD3DX12_TEXTURE_BARRIER(
					D3D12_BARRIER_SYNC_NONE, D3D12_BARRIER_SYNC_COPY,
					D3D12_BARRIER_ACCESS_NO_ACCESS, D3D12_BARRIER_ACCESS_COPY_SOURCE,
					D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS, D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_COPY_SOURCE,
					mOffscreenTex.Get(), CD3DX12_BARRIER_SUBRESOURCE_RANGE(0),
					D3D12_TEXTURE_BARRIER_FLAG_NONE)
			};
			CD3DX12_BARRIER_GROUP barriers[] = {
				CD3DX12_BARRIER_GROUP(_countof(barrierTex), barrierTex)
			};
			cmdList7->Barrier(_countof(barriers), barriers);
		}
		D3D12_RESOURCE_BARRIER transitions[1];
		transitions[0] = CD3DX12_RESOURCE_BARRIER::Transition(mSwapChainTex[frameIndex].Get(),
			D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
		mCmdList->ResourceBarrier(1, transitions);

		mCmdList->CopyResource(mSwapChainTex[frameIndex].Get(), mOffscreenTex.Get());

		{
			CD3DX12_TEXTURE_BARRIER barrierTex[1] = {
				CD3DX12_TEXTURE_BARRIER(
					D3D12_BARRIER_SYNC_COPY, D3D12_BARRIER_SYNC_NONE,
					D3D12_BARRIER_ACCESS_COPY_SOURCE, D3D12_BARRIER_ACCESS_NO_ACCESS,
					D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_COPY_SOURCE, D3D12_BARRIER_LAYOUT_UNDEFINED,
					mOffscreenTex.Get(), CD3DX12_BARRIER_SUBRESOURCE_RANGE(0),
					D3D12_TEXTURE_BARRIER_FLAG_DISCARD)
			};
			CD3DX12_BARRIER_GROUP barriers[] = {
				CD3DX12_BARRIER_GROUP(_countof(barrierTex), barrierTex)
			};
			cmdList7->Barrier(_countof(barriers), barriers);
		}
		transitions[0] = CD3DX12_RESOURCE_BARRIER::Transition(mSwapChainTex[frameIndex].Get(),
			D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
		mCmdList->ResourceBarrier(1, transitions);

		// Finish recording commands
		CHK(mAsyncCmdList->Close());
		CHK(mCmdList->Close());

		//-------------------------------

		// Execute recorded commands
		CHK(mAsyncCmdQueue->Wait(mFence.Get(), mFrameCount - 1)); // Wait completion to copy on GFX
		mAsyncCmdQueue->ExecuteCommandLists(1, CommandListCast(mAsyncCmdList.GetAddressOf()));
		CHK(mAsyncCmdQueue->Signal(mAsyncFence.Get(), mFrameCount));

		CHK(mCmdQueue->Wait(mAsyncFence.Get(), mFrameCount)); // Wait completion to draw on Async
		mCmdQueue->ExecuteCommandLists(1, CommandListCast(mCmdList.GetAddressOf()));
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
