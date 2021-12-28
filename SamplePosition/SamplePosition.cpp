#include <Windows.h>
#include <tchar.h>
#include <wrl/client.h>
#include <stdexcept>
#include <dxgi1_4.h>
#include <d3d12.h>
#include "d3dx12.h"
#include <d3dcompiler.h>
#include <vector>

#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "d3dcompiler.lib")

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
	ComPtr<ID3D12GraphicsCommandList1> mCmdList1;
	uint64_t mFrameCount = 0;
	ComPtr<ID3D12Fence> mFence;
	ComPtr<ID3D12Resource> mSwapChainTex[BUFFER_COUNT];
	ComPtr<ID3D12DescriptorHeap> mSwapChainRTVs;

	ComPtr<ID3D12Resource> mOffscreenTex;
	ComPtr<ID3D12DescriptorHeap> mOffscreenRTV;

	ComPtr<ID3D12Resource> mResolvedTex;
	ComPtr<ID3D12DescriptorHeap> mResolveDescHeap;

	ComPtr<ID3D12RootSignature> mRootSig;
	ComPtr<ID3D12PipelineState> mPSO;
	ComPtr<ID3D12Resource> mVB;
	ComPtr<ID3D12Resource> mIB;
	uint32_t mVBSize;
	uint32_t mIBSize;
	const int SphereSlices = 8;
	const int SphereStacks = 8;
	struct VertexElement
	{
		float position[3];
		float normal[3];
	};

	ComPtr<ID3D12RootSignature> mResolveRootSig;
	ComPtr<ID3D12PipelineState> mResolvePSO;

	const float kDefaultRTClearColor[4] = { 0.1f, 0.2f, 0.4f, 0.0f };
	static const int kMsaaSample = 2;

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
		mResourceStride = mDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
		mRTVStride = mDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

		D3D12_FEATURE_DATA_D3D12_OPTIONS2 feature = {};
		CHK(mDevice->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS2, &feature, sizeof(feature)));
		if (feature.ProgrammableSamplePositionsTier == D3D12_PROGRAMMABLE_SAMPLE_POSITIONS_TIER_NOT_SUPPORTED) {
			throw std::exception("Sample position is unsupported.");
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
		CHK(mCmdList.As(&mCmdList1));

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

		// Shader

		CD3DX12_ROOT_SIGNATURE_DESC rootSigDesc;
		rootSigDesc.Init(0, nullptr, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

		ComPtr<ID3DBlob> rootSigBlob, rootSigError;
		CHK(D3D12SerializeRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1, &rootSigBlob, &rootSigError));
		CHK(mDevice->CreateRootSignature(0, rootSigBlob->GetBufferPointer(), rootSigBlob->GetBufferSize(), IID_PPV_ARGS(&mRootSig)));

		CD3DX12_DESCRIPTOR_RANGE descRange[2];
		descRange[0].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);
		descRange[1].Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0);
		CD3DX12_ROOT_PARAMETER rootParam[2];
		rootParam[0].InitAsConstants(1, 0);
		rootParam[1].InitAsDescriptorTable(2, descRange); // CBV_SRV_UAV
		rootSigDesc.Init(_countof(rootParam), rootParam);

		CHK(D3D12SerializeRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1, &rootSigBlob, &rootSigError));
		CHK(mDevice->CreateRootSignature(0, rootSigBlob->GetBufferPointer(), rootSigBlob->GetBufferSize(), IID_PPV_ARGS(&mResolveRootSig)));

		static const char* shaderCodeVS = R"#(
struct Output {
	float4 position : SV_Position;
	float3 normal : Normal;
};
Output main(float3 position : POSITION, float3 normal : NORMAL) {
	Output output; 
	output.position =  float4(position, 1);
	output.normal = normal;
	return output;
}
)#";
		static const char* shaderCodePS = R"#(
struct Input {
	float4 position : SV_Position;
	float3 normal : Normal;
};
float4 main(Input input) : SV_Target {
	return float4(input.normal, 1);
}
)#";
		static const char* shaderCodeCS = R"#(
cbuffer Constant {
	bool isOdd;
};
Texture2DMS<float4, 2> checkeredTex;
RWTexture2D<float4> resolvedTex;

[numthreads(8,8,1)]
void main(uint2 id : SV_DispatchThreadID) {
	uint2 resolvedPos = id * 2 + (isOdd ? uint2(1, 0) : uint2(0, 0));
	resolvedTex[resolvedPos] = checkeredTex.Load(id, 0);

	resolvedPos = id * 2 + (isOdd ? uint2(0, 1) : uint2(1, 1));
	resolvedTex[resolvedPos] = checkeredTex.Load(id, 1);
};
)#";

		ComPtr<ID3DBlob> shaderBlobVS, shaderErrorVS;
		D3DCompile(shaderCodeVS, strlen(shaderCodeVS) - 1, nullptr, nullptr, nullptr, "main", "vs_5_1",
			D3DCOMPILE_ALL_RESOURCES_BOUND, 0, &shaderBlobVS, &shaderErrorVS);
		if (shaderErrorVS) {
			OutputDebugStringA(reinterpret_cast<char*>(shaderErrorVS->GetBufferPointer()));
			throw runtime_error("Shader compile error.");
		}
		ComPtr<ID3DBlob> shaderBlobPS, shaderErrorPS;
		D3DCompile(shaderCodePS, strlen(shaderCodePS) - 1, nullptr, nullptr, nullptr, "main", "ps_5_1",
			D3DCOMPILE_ALL_RESOURCES_BOUND, 0, &shaderBlobPS, &shaderErrorPS);
		if (shaderErrorPS) {
			OutputDebugStringA(reinterpret_cast<char*>(shaderErrorPS->GetBufferPointer()));
			throw runtime_error("Shader compile error.");
		}
		ComPtr<ID3DBlob> shaderBlobCS, shaderErrorCS;
		D3DCompile(shaderCodeCS, strlen(shaderCodeCS) - 1, nullptr, nullptr, nullptr, "main", "cs_5_1",
			D3DCOMPILE_ALL_RESOURCES_BOUND, 0, &shaderBlobCS, &shaderErrorCS);
		if (shaderErrorCS) {
			OutputDebugStringA(reinterpret_cast<char*>(shaderErrorCS->GetBufferPointer()));
			throw runtime_error("Shader compile error.");
		}

		D3D12_INPUT_ELEMENT_DESC ieDesc[] = {
			{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
			{ "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
		};

		D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
		psoDesc.pRootSignature = mRootSig.Get();
		psoDesc.VS = CD3DX12_SHADER_BYTECODE(shaderBlobVS.Get());
		psoDesc.PS = CD3DX12_SHADER_BYTECODE(shaderBlobPS.Get());
		psoDesc.InputLayout = { ieDesc, _countof(ieDesc) };
		psoDesc.IBStripCutValue = D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED;
		psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
		psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(CD3DX12_DEFAULT());
		psoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(CD3DX12_DEFAULT());
		psoDesc.DepthStencilState.DepthEnable = FALSE;
		psoDesc.BlendState = CD3DX12_BLEND_DESC(CD3DX12_DEFAULT());
		psoDesc.SampleMask = UINT_MAX;
		psoDesc.NumRenderTargets = 1;
		psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
		psoDesc.SampleDesc.Count = kMsaaSample;
		CHK(mDevice->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&mPSO)));

		D3D12_COMPUTE_PIPELINE_STATE_DESC psoDescCS = {};
		psoDescCS.pRootSignature = mResolveRootSig.Get();
		psoDescCS.CS = CD3DX12_SHADER_BYTECODE(shaderBlobCS.Get());
		CHK(mDevice->CreateComputePipelineState(&psoDescCS, IID_PPV_ARGS(&mResolvePSO)));

		// Offscreen resource

		auto heapProp = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
		auto resDesc = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R8G8B8A8_UNORM, WINDOW_WIDTH / 2, WINDOW_HEIGHT / 2, 1, 1, kMsaaSample);
		resDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
		auto clearValue = CD3DX12_CLEAR_VALUE(DXGI_FORMAT_R8G8B8A8_UNORM, kDefaultRTClearColor);
		CHK(mDevice->CreateCommittedResource(
			&heapProp, D3D12_HEAP_FLAG_NONE, &resDesc,
			D3D12_RESOURCE_STATE_GENERIC_READ, &clearValue, IID_PPV_ARGS(&mOffscreenTex)));

		descHeapDesc = {};
		descHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
		descHeapDesc.NumDescriptors = 1;
		CHK(mDevice->CreateDescriptorHeap(&descHeapDesc, IID_PPV_ARGS(&mOffscreenRTV)));

		CD3DX12_CPU_DESCRIPTOR_HANDLE offscreenRTVHandle(mOffscreenRTV->GetCPUDescriptorHandleForHeapStart());
		mDevice->CreateRenderTargetView(mOffscreenTex.Get(), nullptr, offscreenRTVHandle);

		// Generate sphere triangles

		struct IndexList
		{
			short a[6];
		};
		vector<VertexElement> vertices;
		vector<IndexList> indices;
		vertices.reserve((SphereStacks + 1) * (SphereSlices + 1));
		for (int y = 0; y < SphereStacks + 1; ++y)
		{
			for (int x = 0; x < SphereSlices + 1; ++x)
			{
				// Generate a evenly tesselated plane
				float v[3] = { (float)x / (float)(SphereSlices), (float)y / (float)(SphereStacks), 0.0f };
				// Convert to spherical coordinate system
				float theta = 2 * 3.14159265f * v[0];
				float phi = 2 * 3.14159265f * v[1] / 2.0f;
				VertexElement ve = { sinf(phi) * sinf(theta), cosf(phi), sinf(phi) * cosf(theta), 0, 0, 0 };
				// Setup normal
				float r = 1.0f;
				ve.normal[0] = ve.position[0] / r;
				ve.normal[1] = ve.position[1] / r;
				ve.normal[2] = ve.position[2] / r;
				vertices.push_back(ve);
			}
		}
		indices.reserve(SphereStacks * SphereSlices);
		for (int y = 0; y < SphereStacks; ++y)
		{
			for (int x = 0; x < SphereSlices; ++x)
			{
				short b = static_cast<short>(y * (SphereSlices + 1) + x);
				short s = SphereSlices + 1;
				IndexList il = { b, b + 1, b + s, b + s, b + 1, b + s + 1 };
				indices.push_back(il);
			}
		}

		mVBSize = static_cast<uint32_t>(sizeof(vertices[0]) * vertices.size());
		heapProp = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
		resDesc = CD3DX12_RESOURCE_DESC::Buffer(mVBSize, D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE);
		CHK(mDevice->CreateCommittedResource(
			&heapProp, D3D12_HEAP_FLAG_NONE, &resDesc,
			D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&mVB)));
		void* gpuMem;
		CHK(mVB->Map(0, nullptr, &gpuMem));
		memcpy(gpuMem, vertices.data(), mVBSize);

		mIBSize = static_cast<uint32_t>(sizeof(indices[0]) * indices.size());
		resDesc = CD3DX12_RESOURCE_DESC::Buffer(mIBSize, D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE);
		CHK(mDevice->CreateCommittedResource(
			&heapProp, D3D12_HEAP_FLAG_NONE, &resDesc,
			D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&mIB)));
		CHK(mIB->Map(0, nullptr, &gpuMem));
		memcpy(gpuMem, indices.data(), mIBSize);

		// Resolved resource

		heapProp = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
		resDesc = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R8G8B8A8_UNORM, WINDOW_WIDTH, WINDOW_HEIGHT, 1, 1);
		resDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
		CHK(mDevice->CreateCommittedResource(
			&heapProp, D3D12_HEAP_FLAG_NONE, &resDesc,
			D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&mResolvedTex)));

		descHeapDesc = {};
		descHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
		descHeapDesc.NumDescriptors = 2;
		descHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
		CHK(mDevice->CreateDescriptorHeap(&descHeapDesc, IID_PPV_ARGS(&mResolveDescHeap)));

		CD3DX12_CPU_DESCRIPTOR_HANDLE resolveDescHeapHandle(mResolveDescHeap->GetCPUDescriptorHandleForHeapStart());
		mDevice->CreateShaderResourceView(mOffscreenTex.Get(), nullptr, resolveDescHeapHandle);

		resolveDescHeapHandle.Offset(mResourceStride);
		mDevice->CreateUnorderedAccessView(mResolvedTex.Get(), nullptr, nullptr, resolveDescHeapHandle);
	}

	void Draw()
	{
		mFrameCount++;
		auto frameIndex = mSwapChain->GetCurrentBackBufferIndex();

		//-------------------------------

		// Start recording commands
		CHK(mCmdAlloc[mFrameCount % BUFFER_COUNT]->Reset());
		CHK(mCmdList->Reset(mCmdAlloc[mFrameCount % BUFFER_COUNT].Get(), nullptr));

		// Draw an image to offscreen

		CD3DX12_RESOURCE_BARRIER transitions[2];
		transitions[0] = CD3DX12_RESOURCE_BARRIER::Transition(mOffscreenTex.Get(),
			D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_RENDER_TARGET);
		mCmdList->ResourceBarrier(1, transitions);

		mCmdList->ClearRenderTargetView(mOffscreenRTV->GetCPUDescriptorHandleForHeapStart(), kDefaultRTClearColor, 0, nullptr);

		// NumSamplePerPixel must match the PSO's sample count.
		// Tier1 hardware must use NumSamplePerPixel != 1 and NumPixels == 1.
		// It's difficult to implement '4K Geometry', but '4K Checkerboard' is easy with 2x MSAA.
		D3D12_SAMPLE_POSITION spEven[kMsaaSample] = { {-4, -4}, {4, 4} };
		D3D12_SAMPLE_POSITION spOdd[kMsaaSample] = { {4, -4}, {-4, 4} };
		mCmdList1->SetSamplePositions(kMsaaSample, 1, (mFrameCount & 1) ? spOdd : spEven);

		mCmdList->SetGraphicsRootSignature(mRootSig.Get());
		mCmdList->SetPipelineState(mPSO.Get());
		mCmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		D3D12_VERTEX_BUFFER_VIEW vb[1] = {};
		vb[0].BufferLocation = mVB->GetGPUVirtualAddress();
		vb[0].StrideInBytes = sizeof(VertexElement);
		vb[0].SizeInBytes = mVBSize;
		mCmdList->IASetVertexBuffers(0, 1, vb);
		D3D12_INDEX_BUFFER_VIEW ib = {};
		ib.BufferLocation = mIB->GetGPUVirtualAddress();
		ib.Format = DXGI_FORMAT_R16_UINT;
		ib.SizeInBytes = mIBSize;
		mCmdList->IASetIndexBuffer(&ib);
		auto viewport = CD3DX12_VIEWPORT(0.0f, 0.0f, WINDOW_WIDTH / 2, WINDOW_HEIGHT / 2);
		mCmdList->RSSetViewports(1, &viewport);
		auto scissor = CD3DX12_RECT(0, 0, WINDOW_WIDTH / 2, WINDOW_HEIGHT / 2);
		mCmdList->RSSetScissorRects(1, &scissor);
		auto descRTVCpuHandle = mOffscreenRTV->GetCPUDescriptorHandleForHeapStart();
		mCmdList->OMSetRenderTargets(1, &descRTVCpuHandle, TRUE, nullptr);
		mCmdList->DrawIndexedInstanced(6 * SphereStacks * SphereSlices, 1, 0, 0, 0);

		mCmdList1->SetSamplePositions(0, 0, nullptr);

		// Resolve

		transitions[0] = CD3DX12_RESOURCE_BARRIER::Transition(mOffscreenTex.Get(),
			D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_GENERIC_READ);
		transitions[1] = CD3DX12_RESOURCE_BARRIER::Transition(mResolvedTex.Get(),
			D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		mCmdList->ResourceBarrier(2, transitions);

		mCmdList->SetComputeRootSignature(mResolveRootSig.Get());
		mCmdList->SetPipelineState(mResolvePSO.Get());
		mCmdList->SetDescriptorHeaps(1, mResolveDescHeap.GetAddressOf());
		mCmdList->SetComputeRoot32BitConstant(0, (mFrameCount & 1), 0);
		CD3DX12_GPU_DESCRIPTOR_HANDLE resolveDescHandle(mResolveDescHeap->GetGPUDescriptorHandleForHeapStart());
		mCmdList->SetComputeRootDescriptorTable(1, resolveDescHandle);
		mCmdList->Dispatch(Align(WINDOW_WIDTH, 16) / 16, Align(WINDOW_HEIGHT, 16) / 16, 1);

		// Copy from offscrren image to swap chain

		transitions[0] = CD3DX12_RESOURCE_BARRIER::Transition(mResolvedTex.Get(),
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_GENERIC_READ);
		transitions[1] = CD3DX12_RESOURCE_BARRIER::Transition(mSwapChainTex[frameIndex].Get(),
			D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);
		mCmdList->ResourceBarrier(2, transitions);

		mCmdList->CopyResource(mSwapChainTex[frameIndex].Get(), mResolvedTex.Get());
		//D3D12_TEXTURE_COPY_LOCATION copySrc, copyDest;
		//copySrc.pResource = mResolvedTex.Get();
		//copySrc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
		//copySrc.SubresourceIndex = 0;
		//copyDest.pResource = mSwapChainTex[frameIndex].Get();
		//copyDest.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
		//copyDest.SubresourceIndex = 0;
		//D3D12_BOX box = { 0, 0, 0, WINDOW_WIDTH / 2, WINDOW_HEIGHT / 2, 1 };
		//mCmdList->CopyTextureRegion(&copyDest, 0, 0, 0, &copySrc, &box);

		transitions[0] = CD3DX12_RESOURCE_BARRIER::Transition(mSwapChainTex[frameIndex].Get(),
			D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
		mCmdList->ResourceBarrier(1, transitions);

		// Finish recording commands
		CHK(mCmdList->Close());

		//-------------------------------

		// Execute recorded commands
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
