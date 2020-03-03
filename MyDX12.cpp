#include <windows.h>

#include <wrl.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <DirectXMath.h>
#include "d3dx12.h"

// STL Headers
#include <algorithm>
#include <cassert>
#include <chrono>
#include <DirectXMath.h>

#include "Win32Application.h"
#include "DXSample.h"

struct FCommandListData
{
	ComPtr<ID3D12Resource> mBackBuffers;
	ComPtr<ID3D12CommandAllocator> mCommandAllocators;
	uint64_t mFrameFenceValues;
};

struct Vertex
{
	float position[3];
	float color[4];
};

const uint8_t g_NumFrames = 3;

std::vector<UINT8> GenerateTextureData(UINT TextureWidth, UINT TextureHeight, UINT TexturePixelSize)
{
	const UINT rowPitch = TextureWidth * TexturePixelSize;
	const UINT cellPitch = rowPitch >> 3;        // The width of a cell in the checkboard texture.
	const UINT cellHeight = TextureWidth >> 3;    // The height of a cell in the checkerboard texture.
	const UINT textureSize = rowPitch * TextureHeight;

	std::vector<UINT8> data(textureSize);
	UINT8* pData = &data[0];

	for (UINT n = 0; n < textureSize; n += TexturePixelSize)
	{
		UINT x = n % rowPitch;
		UINT y = n / rowPitch;
		UINT i = x / cellPitch;
		UINT j = y / cellHeight;

		if (i % 2 == j % 2)
		{
			pData[n] = 0x00;        // R
			pData[n + 1] = 0x00;    // G
			pData[n + 2] = 0x00;    // B
			pData[n + 3] = 0xff;    // A
		}
		else
		{
			pData[n] = 0xff;        // R
			pData[n + 1] = 0xff;    // G
			pData[n + 2] = 0xff;    // B
			pData[n + 3] = 0xff;    // A
		}
	}

	return data;
}

class CHelloDX12 : public DXSample
{
	FCommandListData mCommandQueueEntry[g_NumFrames];

	// DirectX 12 Objects
	ComPtr<ID3D12Device2> mDevice;
	ComPtr<ID3D12CommandQueue> mCommandQueue;
	ComPtr<IDXGISwapChain4> mSwapChain;

	ComPtr<ID3D12GraphicsCommandList> mCommandList;

	ComPtr<ID3D12DescriptorHeap> mRTVDescriptorHeap;
	UINT mRTVDescriptorSize;
	UINT mCurrentBackBufferIndex;

	// Synchronization objects
	ComPtr<ID3D12Fence> mFence;
	uint64_t mFenceValue;
	HANDLE mFenceEvent;

	// By default, enable V-Sync.
	// Can be toggled with the V key.
	bool mVSync;
	bool mTearingSupported;

	ComPtr<ID3D12RootSignature> mRootSignature;
	ComPtr<ID3D12PipelineState> mPipelineState;

	ComPtr<ID3D12Resource> mVertexBuffer;
	ComPtr<ID3D12Resource> mIndexBuffer;
	ComPtr<ID3D12Resource> mTexture;

	D3D12_VERTEX_BUFFER_VIEW mVertexBufferView;
	D3D12_INDEX_BUFFER_VIEW mIndiceBufferView;

	bool CheckTearingSupport()
	{
		BOOL allowTearing = FALSE;

		// Rather than create the DXGI 1.5 factory interface directly, we create the
		// DXGI 1.4 interface and query for the 1.5 interface. This is to enable the 
		// graphics debugging tools which will not support the 1.5 factory interface 
		// until a future update.
		ComPtr<IDXGIFactory4> factory4;
		if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory4))))
		{
			ComPtr<IDXGIFactory5> factory5;
			if (SUCCEEDED(factory4.As(&factory5)))
			{
				if (FAILED(factory5->CheckFeatureSupport(
					DXGI_FEATURE_PRESENT_ALLOW_TEARING,
					&allowTearing, sizeof(allowTearing))))
				{
					allowTearing = FALSE;
				}
			}
		}

		return allowTearing == TRUE;
	}

	ComPtr<IDXGIAdapter4> GetAdapter(bool useWarp)
	{
		ComPtr<IDXGIFactory4> dxgiFactory;
		UINT createFactoryFlags = 0;
#if defined(_DEBUG)
		createFactoryFlags = DXGI_CREATE_FACTORY_DEBUG;
#endif

		ThrowIfFailed(CreateDXGIFactory2(createFactoryFlags, IID_PPV_ARGS(&dxgiFactory)));

		ComPtr<IDXGIAdapter1> dxgiAdapter1;
		ComPtr<IDXGIAdapter4> dxgiAdapter4;

		if (useWarp)
		{
			ThrowIfFailed(dxgiFactory->EnumWarpAdapter(IID_PPV_ARGS(&dxgiAdapter1)));
			ThrowIfFailed(dxgiAdapter1.As(&dxgiAdapter4));
		}
		else
		{
			SIZE_T maxDedicatedVideoMemory = 0;
			for (UINT i = 0; dxgiFactory->EnumAdapters1(i, &dxgiAdapter1) != DXGI_ERROR_NOT_FOUND; ++i)
			{
				DXGI_ADAPTER_DESC1 dxgiAdapterDesc1;
				dxgiAdapter1->GetDesc1(&dxgiAdapterDesc1);

				// Check to see if the adapter can create a D3D12 device without actually 
				// creating it. The adapter with the largest dedicated video memory
				// is favored.
				if ((dxgiAdapterDesc1.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0 &&
					SUCCEEDED(D3D12CreateDevice(dxgiAdapter1.Get(),
						D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), nullptr)) &&
					dxgiAdapterDesc1.DedicatedVideoMemory > maxDedicatedVideoMemory)
				{
					maxDedicatedVideoMemory = dxgiAdapterDesc1.DedicatedVideoMemory;
					ThrowIfFailed(dxgiAdapter1.As(&dxgiAdapter4));

					TCHAR szDebug[512];

					::wsprintfW(szDebug, L"显卡[%d]-\"%s\":独占显存[%dMB]、独占内存[%dMB]、共享内存[%dMB]\n"
						, i
						, dxgiAdapterDesc1.Description
						, dxgiAdapterDesc1.DedicatedVideoMemory / (1024 * 1024)
						, dxgiAdapterDesc1.DedicatedSystemMemory / (1024 * 1024)
						, dxgiAdapterDesc1.SharedSystemMemory / (1024 * 1024));

					D3D12_FEATURE_DATA_ARCHITECTURE stArchitecture = {};


					::OutputDebugStringW(szDebug);
				}
			}
		}

		return dxgiAdapter4;
	}

	ComPtr<ID3D12Device2> CreateDevice(ComPtr<IDXGIAdapter4> adapter)
	{
		ComPtr<ID3D12Device2> d3d12Device2;
		ThrowIfFailed(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&d3d12Device2)));

#if defined(_DEBUG)
		ComPtr<ID3D12InfoQueue> pInfoQueue;
		if (SUCCEEDED(d3d12Device2.As(&pInfoQueue)))
		{
			pInfoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, TRUE);
			pInfoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, TRUE);
			pInfoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_WARNING, TRUE);

			// Suppress whole categories of messages
	 //D3D12_MESSAGE_CATEGORY Categories[] = {};

	 // Suppress messages based on their severity level
			D3D12_MESSAGE_SEVERITY Severities[] =
			{
				D3D12_MESSAGE_SEVERITY_INFO
			};

			// Suppress individual messages by their ID
			D3D12_MESSAGE_ID DenyIds[] = {
				D3D12_MESSAGE_ID_CLEARRENDERTARGETVIEW_MISMATCHINGCLEARVALUE,   // I'm really not sure how to avoid this message.
				D3D12_MESSAGE_ID_MAP_INVALID_NULLRANGE,                         // This warning occurs when using capture frame while graphics debugging.
				D3D12_MESSAGE_ID_UNMAP_INVALID_NULLRANGE,                       // This warning occurs when using capture frame while graphics debugging.
			};

			D3D12_INFO_QUEUE_FILTER NewFilter = {};
			//NewFilter.DenyList.NumCategories = _countof(Categories);
			//NewFilter.DenyList.pCategoryList = Categories;
			NewFilter.DenyList.NumSeverities = _countof(Severities);
			NewFilter.DenyList.pSeverityList = Severities;
			NewFilter.DenyList.NumIDs = _countof(DenyIds);
			NewFilter.DenyList.pIDList = DenyIds;

			ThrowIfFailed(pInfoQueue->PushStorageFilter(&NewFilter));
		}
#endif

		return d3d12Device2;
	}

	ComPtr<ID3D12CommandQueue> CreateCommandQueue(ComPtr<ID3D12Device2> device, D3D12_COMMAND_LIST_TYPE type)
	{
		ComPtr<ID3D12CommandQueue> d3d12CommandQueue;

		D3D12_COMMAND_QUEUE_DESC desc = {};
		desc.Type = type;
		desc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
		desc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
		desc.NodeMask = 0;

		ThrowIfFailed(device->CreateCommandQueue(&desc, IID_PPV_ARGS(&d3d12CommandQueue)));

		return d3d12CommandQueue;
	}

	ComPtr<IDXGISwapChain4> CreateSwapChain(HWND hWnd,
		ComPtr<ID3D12CommandQueue> commandQueue,
		uint32_t width, uint32_t height, uint32_t bufferCount)
	{
		ComPtr<IDXGISwapChain4> dxgiSwapChain4;
		ComPtr<IDXGIFactory4> dxgiFactory4;
		UINT createFactoryFlags = 0;
#if defined(_DEBUG)
		createFactoryFlags = DXGI_CREATE_FACTORY_DEBUG;
#endif

		ThrowIfFailed(CreateDXGIFactory2(createFactoryFlags, IID_PPV_ARGS(&dxgiFactory4)));

		DXGI_SWAP_CHAIN_DESC1 swapChainDesc = {};
		swapChainDesc.Width = width;
		swapChainDesc.Height = height;
		swapChainDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		swapChainDesc.Stereo = FALSE;
		swapChainDesc.SampleDesc = { 1, 0 };
		swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
		swapChainDesc.BufferCount = bufferCount;
		swapChainDesc.Scaling = DXGI_SCALING_STRETCH;
		swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
		swapChainDesc.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
		// It is recommended to always allow tearing if tearing support is available.
		swapChainDesc.Flags = CheckTearingSupport() ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;

		ComPtr<IDXGISwapChain1> swapChain1;
		ThrowIfFailed(dxgiFactory4->CreateSwapChainForHwnd(
			commandQueue.Get(),
			hWnd,
			&swapChainDesc,
			nullptr,
			nullptr,
			&swapChain1));

		// Disable the Alt+Enter fullscreen toggle feature. Switching to fullscreen
		// will be handled manually.
		ThrowIfFailed(dxgiFactory4->MakeWindowAssociation(hWnd, DXGI_MWA_NO_ALT_ENTER));

		ThrowIfFailed(swapChain1.As(&dxgiSwapChain4));

		return dxgiSwapChain4;
	}

	ComPtr<ID3D12DescriptorHeap> CreateDescriptorHeap(ComPtr<ID3D12Device2> device,
		D3D12_DESCRIPTOR_HEAP_TYPE type, uint32_t numDescriptors)
	{
		ComPtr<ID3D12DescriptorHeap> descriptorHeap;

		D3D12_DESCRIPTOR_HEAP_DESC desc = {};
		desc.NumDescriptors = numDescriptors;
		desc.Type = type;

		ThrowIfFailed(device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&descriptorHeap)));

		return descriptorHeap;
	}

	void UpdateRenderTargetViews(ComPtr<ID3D12Device2> device,
		ComPtr<IDXGISwapChain4> swapChain, ComPtr<ID3D12DescriptorHeap> descriptorHeap, UINT rtvDescriptorSize)
	{
		CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle(descriptorHeap->GetCPUDescriptorHandleForHeapStart());

		for (int i = 0; i < g_NumFrames; ++i)
		{
			ComPtr<ID3D12Resource> backBuffer;
			ThrowIfFailed(swapChain->GetBuffer(i, IID_PPV_ARGS(&backBuffer)));

			device->CreateRenderTargetView(backBuffer.Get(), nullptr, rtvHandle);

			mCommandQueueEntry[i].mBackBuffers = backBuffer;

			rtvHandle.Offset(rtvDescriptorSize);
		}
	}

	ComPtr<ID3D12CommandAllocator> CreateCommandAllocator(ComPtr<ID3D12Device2> device, D3D12_COMMAND_LIST_TYPE type)
	{
		ComPtr<ID3D12CommandAllocator> commandAllocator;
		ThrowIfFailed(device->CreateCommandAllocator(type, IID_PPV_ARGS(&commandAllocator)));

		return commandAllocator;
	}

	ComPtr<ID3D12GraphicsCommandList> CreateCommandList(ComPtr<ID3D12Device2> device,
		ComPtr<ID3D12CommandAllocator> commandAllocator,ComPtr<ID3D12PipelineState> pso, 
		D3D12_COMMAND_LIST_TYPE type)
	{
		ComPtr<ID3D12GraphicsCommandList> commandList;
		ThrowIfFailed(device->CreateCommandList(0, type, commandAllocator.Get(), pso.Get(), IID_PPV_ARGS(&commandList)));

		ThrowIfFailed(commandList->Close());

		return commandList;
	}

	ComPtr<ID3D12Fence> CreateFence(ComPtr<ID3D12Device2> device)
	{
		ComPtr<ID3D12Fence> fence;

		ThrowIfFailed(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));

		return fence;
	}

	HANDLE CreateEventHandle()
	{
		HANDLE fenceEvent;

		fenceEvent = ::CreateEvent(NULL, FALSE, FALSE, NULL);
		assert(fenceEvent && "Failed to create fence event.");

		return fenceEvent;
	}

	uint64_t Signal(ComPtr<ID3D12CommandQueue> commandQueue, ComPtr<ID3D12Fence> fence,
		uint64_t& fenceValue)
	{
		uint64_t fenceValueForSignal = ++fenceValue;
		ThrowIfFailed(commandQueue->Signal(fence.Get(), fenceValueForSignal));

		return fenceValueForSignal;
	}

	void WaitForFenceValue(ComPtr<ID3D12Fence> fence, uint64_t fenceValue, HANDLE fenceEvent)
	{
		if (fence->GetCompletedValue() < fenceValue)
		{
			ThrowIfFailed(fence->SetEventOnCompletion(fenceValue, fenceEvent));
			::WaitForSingleObject(fenceEvent, 5000);
		}
	}

	void CreateRootSignature()
	{
		CD3DX12_DESCRIPTOR_RANGE1 ranges[1];
		ranges[0].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, D3D12_DESCRIPTOR_RANGE_FLAG_DATA_STATIC);

		CD3DX12_ROOT_PARAMETER1 rootParameters[1];
		rootParameters[0].InitAsDescriptorTable(1, &ranges[0], D3D12_SHADER_VISIBILITY_PIXEL);

		D3D12_STATIC_SAMPLER_DESC sampler = {};
		sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
		sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
		sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
		sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
		sampler.MipLODBias = 0;
		sampler.MaxAnisotropy = 0;
		sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
		sampler.BorderColor = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
		sampler.MinLOD = 0.0f;
		sampler.MaxLOD = D3D12_FLOAT32_MAX;
		sampler.ShaderRegister = 0;
		sampler.RegisterSpace = 0;
		sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

		CD3DX12_ROOT_SIGNATURE_DESC rootSignatureDesc;
		rootSignatureDesc.Init(1, (const D3D12_ROOT_PARAMETER *)&rootParameters[0], 1, &sampler, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

		ComPtr<ID3DBlob> signature;
		ComPtr<ID3DBlob> error;
		ThrowIfFailed(D3D12SerializeRootSignature(&rootSignatureDesc, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &error));
		ThrowIfFailed(mDevice->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(), IID_PPV_ARGS(&mRootSignature)));
	}

	const TCHAR *GetAssetPath(const TCHAR *localPath)
	{
		static TCHAR szPath[256], szAssetPath[256];
		static bool bInit = false;
		if (!bInit)
		{
			::GetCurrentDirectoryW(256, szPath);
		}
		
		::wsprintfW(szAssetPath, L"%s\\%s", szPath, localPath);
		return szAssetPath;
	}

	void CreateShader(ComPtr<ID3DBlob> &vertexShader, ComPtr<ID3DBlob> &pixelShader)
	{
#if defined(_DEBUG)
		// Enable better shader debugging with the graphics debugging tools.
		UINT compileFlags = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#else
		UINT compileFlags = 0;
#endif

		const TCHAR *pFilePath = GetAssetPath(L"vs.shader");
		ThrowIfFailed(D3DCompileFromFile(GetAssetPath(L"vs.shader"), nullptr, nullptr, "main", "vs_5_0", compileFlags, 0, &vertexShader, nullptr));
		ThrowIfFailed(D3DCompileFromFile(GetAssetPath(L"ps.shader"), nullptr, nullptr, "main", "ps_5_0", compileFlags, 0, &pixelShader, nullptr));
	}

	ComPtr<ID3D12PipelineState> CreatePSO(ID3DBlob *vertexShader, ID3DBlob *pixelShader,
		D3D12_INPUT_ELEMENT_DESC *inputElementDescs, UINT eleSize)
	{
		ComPtr<ID3D12PipelineState> pipelineState;

		// Describe and create the graphics pipeline state object (PSO).
		D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
		psoDesc.InputLayout = { inputElementDescs, eleSize };
		psoDesc.pRootSignature = mRootSignature.Get();
		psoDesc.VS = CD3DX12_SHADER_BYTECODE(vertexShader);
		psoDesc.PS = CD3DX12_SHADER_BYTECODE(pixelShader);
		psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
		psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
		psoDesc.DepthStencilState.DepthEnable = FALSE;
		psoDesc.DepthStencilState.StencilEnable = FALSE;
		psoDesc.SampleMask = UINT_MAX;
		psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
		psoDesc.NumRenderTargets = 1;
		psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
		psoDesc.SampleDesc.Count = 1;

		ThrowIfFailed(mDevice->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&pipelineState)));
		return pipelineState;
	}

	void CreateIndice()
	{
		UINT16 Indice[] = { 1,0,2,2,0,3 };

		ThrowIfFailed(mDevice->CreateCommittedResource(&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD),
			D3D12_HEAP_FLAG_NONE,
			&CD3DX12_RESOURCE_DESC::Buffer(sizeof(Indice)),
			D3D12_RESOURCE_STATE_GENERIC_READ,
			nullptr,
			IID_PPV_ARGS(&mIndexBuffer)
		));

		UINT8* pIndexDataBegin;
		CD3DX12_RANGE readRange(0, 0);        // We do not intend to read from this resource on the CPU.
		ThrowIfFailed(mIndexBuffer->Map(0, &readRange, reinterpret_cast<void**>(&pIndexDataBegin)));
		memcpy(pIndexDataBegin, Indice, sizeof(Indice));
		mVertexBuffer->Unmap(0, nullptr);

		mIndiceBufferView.BufferLocation = mIndexBuffer->GetGPUVirtualAddress();
		mIndiceBufferView.Format = DXGI_FORMAT_R16_UINT;
		mIndiceBufferView.SizeInBytes = sizeof(Indice);
	}

	void CreateVertex()
	{
		Vertex triangleVertices[] =
		{
			{ { -0.5f, 0.5f * m_aspectRatio, 0.0f }, { 1.0f, 0.0f, 0.0f, 1.0f } },
			{ { -0.5f, -0.5f * m_aspectRatio, 0.0f }, { 0.0f, 1.0f, 0.0f, 1.0f } },
			{ { 0.5f, -0.5f * m_aspectRatio, 0.0f }, { 0.0f, 0.0f, 1.0f, 1.0f } },
			{ { 0.5f, 0.5f * m_aspectRatio, 0.0f }, { 1.0f, 0.0f, 1.0f, 1.0f } }
		};

		const UINT vertexBufferSize = sizeof(triangleVertices);

		// Note: using upload heaps to transfer static data like vert buffers is not 
		// recommended. Every time the GPU needs it, the upload heap will be marshalled 
		// over. Please read up on Default Heap usage. An upload heap is used here for 
		// code simplicity and because there are very few verts to actually transfer.
		CD3DX12_RESOURCE_DESC *pResourceDesc = &CD3DX12_RESOURCE_DESC::Buffer(vertexBufferSize);
		ThrowIfFailed(mDevice->CreateCommittedResource(
			&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD),
			D3D12_HEAP_FLAG_NONE,
			pResourceDesc,
			D3D12_RESOURCE_STATE_GENERIC_READ,
			nullptr,
			IID_PPV_ARGS(&mVertexBuffer)));

		UINT8* pVertexDataBegin;
		CD3DX12_RANGE readRange(0, 0);        // We do not intend to read from this resource on the CPU.
		ThrowIfFailed(mVertexBuffer->Map(0, &readRange, reinterpret_cast<void**>(&pVertexDataBegin)));
		memcpy(pVertexDataBegin, triangleVertices, sizeof(triangleVertices));
		mVertexBuffer->Unmap(0, nullptr);

		mVertexBufferView.BufferLocation = mVertexBuffer->GetGPUVirtualAddress();
		mVertexBufferView.StrideInBytes = sizeof(Vertex);
		mVertexBufferView.SizeInBytes = vertexBufferSize;
	}

	//创建Texture,尚待完成
	void CreateTexture(UINT TextureWidth, UINT TextureHeight)
	{
		// Describe and create a Texture2D.
		D3D12_RESOURCE_DESC textureDesc = {};
		textureDesc.MipLevels = 1;
		textureDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		textureDesc.Width = TextureWidth;
		textureDesc.Height = TextureHeight;
		textureDesc.Flags = D3D12_RESOURCE_FLAG_NONE;
		textureDesc.DepthOrArraySize = 1;
		textureDesc.SampleDesc.Count = 1;
		textureDesc.SampleDesc.Quality = 0;
		textureDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;

		ThrowIfFailed(mDevice->CreateCommittedResource(
			&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
			D3D12_HEAP_FLAG_NONE,
			&textureDesc,
			D3D12_RESOURCE_STATE_COPY_DEST,
			nullptr,
			IID_PPV_ARGS(&mTexture)));

		const UINT64 uploadBufferSize = GetRequiredIntermediateSize(mTexture.Get(), 0, 1);

		D3D12_RESOURCE_DESC *pResourceDesc = &CD3DX12_RESOURCE_DESC::Buffer(uploadBufferSize);
		ComPtr<ID3D12Resource> textureUploadHeap;
		// Create the GPU upload buffer.
		ThrowIfFailed(mDevice->CreateCommittedResource(
			&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD),
			D3D12_HEAP_FLAG_NONE,
			pResourceDesc,
			D3D12_RESOURCE_STATE_GENERIC_READ,
			nullptr,
			IID_PPV_ARGS(&textureUploadHeap)));

		// Copy data to the intermediate upload heap and then schedule a copy 
// from the upload heap to the Texture2D.
		std::vector<UINT8> texture = GenerateTextureData(256, 256, 4);
		D3D12_SUBRESOURCE_DATA textureData = {};
		textureData.pData = &texture[0];
		textureData.RowPitch = TextureWidth * 4;
		textureData.SlicePitch = textureData.RowPitch * TextureHeight;

		UpdateSubresources(mCommandList.Get(), mTexture.Get(), 
			textureUploadHeap.Get(), 0, 0, 1, &textureData);

		mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mTexture.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE));
	}

	// Load the sample assets.
	void LoadAssets()
	{
		CreateRootSignature();

		ComPtr<ID3DBlob> vertexShader;
		ComPtr<ID3DBlob> pixelShader;
		CreateShader(vertexShader, pixelShader);

		// Define the vertex input layout.
		D3D12_INPUT_ELEMENT_DESC inputElementDescs[] =
		{
			{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
			{ "COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
		};

		mPipelineState = CreatePSO(vertexShader.Get(), pixelShader.Get(), inputElementDescs, _countof(inputElementDescs));

		CreateVertex();
		CreateIndice();
		CreateTexture(256, 256);
	}

public:
	CHelloDX12(UINT width, UINT height, std::wstring name):
		DXSample(width,height,name),
		mVSync(true),
		mFenceValue(0)
	{
		mTearingSupported = false;// CheckTearingSupport();
		ComPtr<IDXGIAdapter4> adapter = GetAdapter(mTearingSupported);
		mDevice = CreateDevice(adapter);


		D3D12_FEATURE_DATA_ARCHITECTURE stArchitecture = {};
		mDevice->CheckFeatureSupport(D3D12_FEATURE_ARCHITECTURE, &stArchitecture, sizeof(stArchitecture));


		mCommandQueue = CreateCommandQueue(mDevice, 
			D3D12_COMMAND_LIST_TYPE::D3D12_COMMAND_LIST_TYPE_DIRECT);

	}

	virtual ~CHelloDX12()
	{

	}

	virtual void OnInit()
	{
		mSwapChain = CreateSwapChain(Win32Application::GetHwnd(), mCommandQueue,
			this->GetWidth(), this->GetHeight(), g_NumFrames);

		mCurrentBackBufferIndex = mSwapChain->GetCurrentBackBufferIndex(); //获取当前的backbuffer index

		mRTVDescriptorHeap = CreateDescriptorHeap(mDevice, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, g_NumFrames);
		mRTVDescriptorSize = mDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

		for (int i = 0; i < g_NumFrames; ++i)
		{
			mCommandQueueEntry[i].mCommandAllocators = CreateCommandAllocator(mDevice, D3D12_COMMAND_LIST_TYPE_DIRECT);
		}

		UpdateRenderTargetViews(mDevice, mSwapChain, mRTVDescriptorHeap, mRTVDescriptorSize);

		mFence = CreateFence(mDevice);
		mFenceEvent = CreateEventHandle();

		//---------------create resources
		LoadAssets();

		mCommandList = CreateCommandList(mDevice,
			mCommandQueueEntry[mCurrentBackBufferIndex].mCommandAllocators,
			mPipelineState,
			D3D12_COMMAND_LIST_TYPE_DIRECT);

		mCommandList->Close();
	}

	virtual void OnUpdate()
	{
		static uint64_t frameCounter = 0;
		static double elapsedSeconds = 0.0;
		static std::chrono::high_resolution_clock clock;
		static auto t0 = clock.now();

		frameCounter++;
		auto t1 = clock.now();
		auto deltaTime = t1 - t0;
		t0 = t1;

		elapsedSeconds += deltaTime.count() * 1e-9;
		if (elapsedSeconds > 1.0)
		{
			char buffer[500];
			auto fps = frameCounter / elapsedSeconds;
			sprintf_s(buffer, 500, "FPS: %f\n", fps);
			OutputDebugStringA(buffer);

			frameCounter = 0;
			elapsedSeconds = 0.0;
		}
	}

	virtual void OnRender()
	{
		static CD3DX12_VIEWPORT viewport(0.0f, 0.0f,
			static_cast<float>(GetWidth()), static_cast<float>(GetHeight()));

		static CD3DX12_RECT scissorRect(0, 0,
			static_cast<LONG>(GetWidth()), static_cast<LONG>(GetHeight()));

		if (viewport.Height != GetHeight() || viewport.Width != GetWidth())
		{
			viewport = CD3DX12_VIEWPORT(0.0f, 0.0f,
				static_cast<float>(GetWidth()), static_cast<float>(GetHeight()));

			scissorRect = CD3DX12_RECT(0, 0,
				static_cast<LONG>(GetWidth()), static_cast<LONG>(GetHeight()));
		}

		auto commandAllocator = mCommandQueueEntry[mCurrentBackBufferIndex].mCommandAllocators;
		auto backBuffer = mCommandQueueEntry[mCurrentBackBufferIndex].mBackBuffers;

		//reset command allocator and command list
		commandAllocator->Reset();
		mCommandList->Reset(commandAllocator.Get(), mPipelineState.Get());

		// Set necessary state.
		mCommandList->SetGraphicsRootSignature(mRootSignature.Get());
		mCommandList->RSSetViewports(1, &viewport);
		mCommandList->RSSetScissorRects(1, &scissorRect);

		// Clear the render target.
		{
			CD3DX12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(
				backBuffer.Get(),
				D3D12_RESOURCE_STATE_PRESENT,
				D3D12_RESOURCE_STATE_RENDER_TARGET);

			mCommandList->ResourceBarrier(1, &barrier);

			FLOAT clearColor[] = { 0.4f, 0.6f, 0.9f, 1.0f };

			//获取back buffer在descriptor heap的位置
			CD3DX12_CPU_DESCRIPTOR_HANDLE rtv(mRTVDescriptorHeap->GetCPUDescriptorHandleForHeapStart(),
				mCurrentBackBufferIndex, mRTVDescriptorSize);

			mCommandList->OMSetRenderTargets(1, &rtv, FALSE, nullptr);

			mCommandList->ClearRenderTargetView(rtv, clearColor, 0, nullptr);
			mCommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
			mCommandList->IASetVertexBuffers(0, 1, &mVertexBufferView);
			mCommandList->IASetIndexBuffer(&mIndiceBufferView);
			mCommandList->DrawIndexedInstanced(6, 1, 0, 0, 0);
		}

		// Present
		{
			CD3DX12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(
				backBuffer.Get(),
				D3D12_RESOURCE_STATE_RENDER_TARGET, 
				D3D12_RESOURCE_STATE_PRESENT);

			mCommandList->ResourceBarrier(1, &barrier);

			ThrowIfFailed(mCommandList->Close());

			ID3D12CommandList* const commandLists[] = { mCommandList.Get() };
			mCommandQueue->ExecuteCommandLists(_countof(commandLists), commandLists);

			UINT syncInterval = mVSync ? 1 : 0;
			UINT presentFlags = mTearingSupported && !mVSync ? DXGI_PRESENT_ALLOW_TEARING : 0;
			ThrowIfFailed(mSwapChain->Present(syncInterval, presentFlags));

			mCommandQueueEntry[mCurrentBackBufferIndex].mFrameFenceValues = Signal(mCommandQueue, mFence, mFenceValue);

			mCurrentBackBufferIndex = mSwapChain->GetCurrentBackBufferIndex();
			WaitForFenceValue(mFence, 
				mCommandQueueEntry[mCurrentBackBufferIndex].mFrameFenceValues, mFenceEvent);

		}
	}

	virtual void OnDestroy()
	{

	}
};

int CALLBACK wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, PWSTR lpCmdLine, int nCmdShow)
{
	CHelloDX12 sample(600, 600, L"D3D12 Hello Window");
	return Win32Application::Run(&sample, hInstance, nCmdShow);
}