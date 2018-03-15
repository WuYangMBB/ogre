/*
-----------------------------------------------------------------------------
This source file is part of OGRE
(Object-oriented Graphics Rendering Engine)
For the latest info, see http://www.ogre3d.org/

Copyright (c) 2000-2014 Torus Knot Software Ltd

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
-----------------------------------------------------------------------------
*/
#include "OgreD3D11RenderSystem.h"
#include "OgreD3D11Prerequisites.h"
#include "OgreD3D11DriverList.h"
#include "OgreD3D11Driver.h"
#include "OgreD3D11VideoModeList.h"
#include "OgreD3D11VideoMode.h"
#include "OgreD3D11RenderWindow.h"
#include "OgreD3D11TextureManager.h"
#include "OgreD3D11Texture.h"
#include "OgreViewport.h"
#include "OgreLogManager.h"
#include "OgreMeshManager.h"
#include "OgreSceneManagerEnumerator.h"
#include "OgreD3D11HardwareBufferManager.h"
#include "OgreD3D11HardwareIndexBuffer.h"
#include "OgreD3D11HardwareVertexBuffer.h"
#include "OgreD3D11VertexDeclaration.h"
#include "OgreD3D11GpuProgramManager.h"
#include "OgreD3D11HLSLProgramFactory.h"

#include "OgreD3D11HardwareOcclusionQuery.h"
#include "OgreFrustum.h"
#include "OgreD3D11MultiRenderTarget.h"
#include "OgreD3D11HLSLProgram.h"

#include "OgreD3D11DepthBuffer.h"
#include "OgreD3D11HardwarePixelBuffer.h"
#include "OgreException.h"

#if OGRE_NO_QUAD_BUFFER_STEREO == 0
#include "OgreD3D11StereoDriverBridge.h"
#endif


#ifndef D3D_FL9_3_SIMULTANEOUS_RENDER_TARGET_COUNT
#   define D3D_FL9_3_SIMULTANEOUS_RENDER_TARGET_COUNT 4
#endif

#ifndef D3D_FL9_1_SIMULTANEOUS_RENDER_TARGET_COUNT
#   define D3D_FL9_1_SIMULTANEOUS_RENDER_TARGET_COUNT 1
#endif
//---------------------------------------------------------------------
#include <d3d10.h>
#include <OgreNsightChecker.h>

#if OGRE_PLATFORM == OGRE_PLATFORM_WINRT &&  defined(_WIN32_WINNT_WINBLUE) && _WIN32_WINNT >= _WIN32_WINNT_WINBLUE
#include <dxgi1_3.h> // for IDXGIDevice3::Trim
#endif

namespace Ogre 
{
    HRESULT WINAPI D3D11CreateDeviceN(
        _In_opt_ IDXGIAdapter* pAdapter,
        D3D_DRIVER_TYPE DriverType,
        HMODULE Software,
        UINT Flags,
        const D3D_FEATURE_LEVEL* pFeatureLevels,
        UINT FeatureLevels,
        UINT SDKVersion,
        _Out_ ID3D11DeviceN** ppDevice,
        _Out_ D3D_FEATURE_LEVEL* pFeatureLevel,
        _Out_ ID3D11DeviceContextN** ppImmediateContext )
    {
#if OGRE_PLATFORM == OGRE_PLATFORM_WIN32
        return D3D11CreateDevice(pAdapter, DriverType, Software, Flags, pFeatureLevels, FeatureLevels, SDKVersion, ppDevice, pFeatureLevel, ppImmediateContext);

#elif OGRE_PLATFORM == OGRE_PLATFORM_WINRT
        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;
        ComPtr<ID3D11DeviceN> deviceN;
        ComPtr<ID3D11DeviceContextN> contextN;
        D3D_FEATURE_LEVEL featureLevel;
        HRESULT mainHr, hr;

        mainHr = hr = D3D11CreateDevice(pAdapter, DriverType, Software, Flags, pFeatureLevels, FeatureLevels, SDKVersion,
                                        (ppDevice ? device.GetAddressOf() : NULL), &featureLevel, (ppImmediateContext ? context.GetAddressOf() : NULL));
        if(FAILED(hr)) return hr;

        hr = device ? device.As(&deviceN) : S_OK;
        if(FAILED(hr)) return hr;

        hr = context ? context.As(&contextN) : S_OK;
        if(FAILED(hr)) return hr;

        if(ppDevice)            *ppDevice = deviceN.Detach();
        if(pFeatureLevel)       *pFeatureLevel = featureLevel;
        if(ppImmediateContext)  *ppImmediateContext = contextN.Detach();

        return mainHr;
#endif
    }

    //---------------------------------------------------------------------
    D3D11RenderSystem::D3D11RenderSystem()
		: mDevice()
#if OGRE_NO_QUAD_BUFFER_STEREO == 0
		, mStereoDriver(NULL)
#endif	
#if OGRE_PLATFORM == OGRE_PLATFORM_WINRT
		, suspendingToken()
		, surfaceContentLostToken()
#endif
    {
        LogManager::getSingleton().logMessage( "D3D11: " + getName() + " created." );

        mRenderSystemWasInited = false;
        mSwitchingFullscreenCounter = 0;
        mDriverType = D3D_DRIVER_TYPE_HARDWARE;

        initRenderSystem();

        // set config options defaults
        initConfigOptions();

        // Clear class instance storage
        memset(mClassInstances, 0, sizeof(mClassInstances));
        memset(mNumClassInstances, 0, sizeof(mNumClassInstances));

        mEventNames.push_back("DeviceLost");
        mEventNames.push_back("DeviceRestored");

#if OGRE_PLATFORM == OGRE_PLATFORM_WINRT
#if defined(_WIN32_WINNT_WINBLUE) && _WIN32_WINNT >= _WIN32_WINNT_WINBLUE
		suspendingToken = (Windows::ApplicationModel::Core::CoreApplication::Suspending +=
			ref new Windows::Foundation::EventHandler<Windows::ApplicationModel::SuspendingEventArgs^>([this](Platform::Object ^sender, Windows::ApplicationModel::SuspendingEventArgs ^e)
		{
			// Hints to the driver that the app is entering an idle state and that its memory can be used temporarily for other apps.
			ComPtr<IDXGIDevice3> pDXGIDevice;
			if(mDevice.get() && SUCCEEDED(mDevice->QueryInterface(pDXGIDevice.GetAddressOf())))
				pDXGIDevice->Trim();
		}));

		surfaceContentLostToken = (Windows::Graphics::Display::DisplayInformation::DisplayContentsInvalidated +=
			ref new Windows::Foundation::TypedEventHandler<Windows::Graphics::Display::DisplayInformation^, Platform::Object^>(
				[this](Windows::Graphics::Display::DisplayInformation^ sender, Platform::Object^ arg)
		{
			LogManager::getSingleton().logMessage("D3D11: DisplayContentsInvalidated.");
			validateDevice(true);
		}));
#else // Win 8.0
		surfaceContentLostToken = (Windows::Graphics::Display::DisplayProperties::DisplayContentsInvalidated +=
			ref new Windows::Graphics::Display::DisplayPropertiesEventHandler([this](Platform::Object ^sender)
		{
			LogManager::getSingleton().logMessage("D3D11: DisplayContentsInvalidated.");
			validateDevice(true);
		}));
#endif
#endif
    }
    //---------------------------------------------------------------------
    D3D11RenderSystem::~D3D11RenderSystem()
    {
#if OGRE_PLATFORM == OGRE_PLATFORM_WINRT
#if defined(_WIN32_WINNT_WINBLUE) && _WIN32_WINNT >= _WIN32_WINNT_WINBLUE
		Windows::ApplicationModel::Core::CoreApplication::Suspending -= suspendingToken;
		Windows::Graphics::Display::DisplayInformation::DisplayContentsInvalidated -= surfaceContentLostToken;
#else // Win 8.0
		Windows::Graphics::Display::DisplayProperties::DisplayContentsInvalidated -= surfaceContentLostToken;
#endif
#endif

        shutdown();

        // Deleting the HLSL program factory
        if (mHLSLProgramFactory)
        {
            // Remove from manager safely
            if (HighLevelGpuProgramManager::getSingletonPtr())
                HighLevelGpuProgramManager::getSingleton().removeFactory(mHLSLProgramFactory);
            delete mHLSLProgramFactory;
            mHLSLProgramFactory = 0;
        }

        LogManager::getSingleton().logMessage( "D3D11: " + getName() + " destroyed." );
    }
    //---------------------------------------------------------------------
    const String& D3D11RenderSystem::getName() const
    {
        static String strName( "Direct3D11 Rendering Subsystem");
        return strName;
    }

	//---------------------------------------------------------------------
    D3D11DriverList* D3D11RenderSystem::getDirect3DDrivers(bool refreshList /* = false*/)
    {
        if(!mDriverList)
            mDriverList = new D3D11DriverList();

        if(refreshList || mDriverList->count() == 0)
            mDriverList->refresh();

        return mDriverList;
    }
    //---------------------------------------------------------------------
	ID3D11DeviceN* D3D11RenderSystem::createD3D11Device(D3D11Driver* d3dDriver, D3D_DRIVER_TYPE driverType,
		D3D_FEATURE_LEVEL minFL, D3D_FEATURE_LEVEL maxFL, D3D_FEATURE_LEVEL* pFeatureLevel)
	{
		IDXGIAdapterN* pAdapter = (d3dDriver && driverType == D3D_DRIVER_TYPE_HARDWARE) ? d3dDriver->getDeviceAdapter() : NULL;

		assert(driverType == D3D_DRIVER_TYPE_HARDWARE || driverType == D3D_DRIVER_TYPE_SOFTWARE || driverType == D3D_DRIVER_TYPE_WARP);
		if(d3dDriver != NULL)
		{
			if(0 == wcscmp(d3dDriver->getAdapterIdentifier().Description, L"NVIDIA PerfHUD"))
				driverType = D3D_DRIVER_TYPE_REFERENCE;
			else
				driverType = D3D_DRIVER_TYPE_UNKNOWN;
		}

		// determine deviceFlags
		UINT deviceFlags = 0;
#if OGRE_PLATFORM == OGRE_PLATFORM_WINRT
		// This flag is required in order to enable compatibility with Direct2D.
		deviceFlags |= D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#endif
		if(OGRE_DEBUG_MODE && !IsWorkingUnderNsight() && D3D11Device::D3D_NO_EXCEPTION != D3D11Device::getExceptionsErrorLevel())
		{
			deviceFlags |= D3D11_CREATE_DEVICE_DEBUG;
		}
		if(!OGRE_THREAD_SUPPORT)
		{
			deviceFlags |= D3D11_CREATE_DEVICE_SINGLETHREADED;
		}

		// determine feature levels
		D3D_FEATURE_LEVEL requestedLevels[] = {
#if !__OGRE_WINRT_PHONE // Windows Phone support only FL 9.3, but simulator can create much more capable device, so restrict it artificially here
#if defined(_WIN32_WINNT_WIN8) && _WIN32_WINNT >= _WIN32_WINNT_WIN8
			D3D_FEATURE_LEVEL_11_1,
#endif
			D3D_FEATURE_LEVEL_11_0,
			D3D_FEATURE_LEVEL_10_1,
			D3D_FEATURE_LEVEL_10_0,
#endif // !__OGRE_WINRT_PHONE
			D3D_FEATURE_LEVEL_9_3,
			D3D_FEATURE_LEVEL_9_2,
			D3D_FEATURE_LEVEL_9_1
		};

		D3D_FEATURE_LEVEL *pFirstFL = requestedLevels, *pLastFL = pFirstFL + ARRAYSIZE(requestedLevels) - 1;
		for(unsigned int i = 0; i < ARRAYSIZE(requestedLevels); i++)
		{
			if(minFL == requestedLevels[i])
				pLastFL = &requestedLevels[i];
			if(maxFL == requestedLevels[i])
				pFirstFL = &requestedLevels[i];
		}
		if(pLastFL < pFirstFL)
		{
			OGRE_EXCEPT(Exception::ERR_INTERNAL_ERROR,
				"Requested min level feature is bigger the requested max level feature.",
				"D3D11RenderSystem::initialise");
		}

		// create device
		ID3D11DeviceN* device = NULL;
		HRESULT hr = D3D11CreateDeviceN(pAdapter, driverType, NULL, deviceFlags, pFirstFL, pLastFL - pFirstFL + 1, D3D11_SDK_VERSION, &device, pFeatureLevel, 0);

		if(FAILED(hr) && 0 != (deviceFlags & D3D11_CREATE_DEVICE_DEBUG))
		{
			StringStream error;
			error << "Failed to create Direct3D11 device with debug layer (" << hr << ")\nRetrying without debug layer.";
			Ogre::LogManager::getSingleton().logMessage(error.str());

			// create device - second attempt, without debug layer
			deviceFlags &= ~D3D11_CREATE_DEVICE_DEBUG;
			hr = D3D11CreateDeviceN(pAdapter, driverType, NULL, deviceFlags, pFirstFL, pLastFL - pFirstFL + 1, D3D11_SDK_VERSION, &device, pFeatureLevel, 0);
		}
		if(FAILED(hr))
		{
			OGRE_EXCEPT_EX(Exception::ERR_RENDERINGAPI_ERROR, hr, "Failed to create Direct3D11 device", "D3D11RenderSystem::D3D11RenderSystem");
		}
		return device;
	}
    //---------------------------------------------------------------------
    void D3D11RenderSystem::initConfigOptions()
    {
        ConfigOption optDevice;
        ConfigOption optVideoMode;
        ConfigOption optFullScreen;
        ConfigOption optVSync;
        ConfigOption optVSyncInterval;
		ConfigOption optBackBufferCount;
        ConfigOption optAA;
        ConfigOption optFPUMode;
        ConfigOption optNVPerfHUD;
        ConfigOption optSRGB;
        ConfigOption optMinFeatureLevels;
        ConfigOption optMaxFeatureLevels;
        ConfigOption optExceptionsErrorLevel;
        ConfigOption optDriverType;
#if OGRE_NO_QUAD_BUFFER_STEREO == 0
		ConfigOption optStereoMode;
#endif

        optDevice.name = "Rendering Device";
        optDevice.currentValue = "(default)";
        optDevice.possibleValues.push_back("(default)");
        D3D11DriverList* driverList = getDirect3DDrivers();
        for( unsigned j=0; j < driverList->count(); j++ )
        {
            D3D11Driver* driver = driverList->item(j);
            optDevice.possibleValues.push_back( driver->DriverDescription() );
        }
        optDevice.immutable = false;

        optVideoMode.name = "Video Mode";
        optVideoMode.currentValue = "800 x 600 @ 32-bit colour";
        optVideoMode.immutable = false;

        optFullScreen.name = "Full Screen";
        optFullScreen.possibleValues.push_back( "Yes" );
        optFullScreen.possibleValues.push_back( "No" );
        optFullScreen.currentValue = "Yes";
        optFullScreen.immutable = false;

        optVSync.name = "VSync";
        optVSync.immutable = false;
        optVSync.possibleValues.push_back( "Yes" );
        optVSync.possibleValues.push_back( "No" );
        optVSync.currentValue = "No";

        optVSyncInterval.name = "VSync Interval";
        optVSyncInterval.immutable = false;
        optVSyncInterval.possibleValues.push_back( "1" );
        optVSyncInterval.possibleValues.push_back( "2" );
        optVSyncInterval.possibleValues.push_back( "3" );
        optVSyncInterval.possibleValues.push_back( "4" );
        optVSyncInterval.currentValue = "1";

		optBackBufferCount.name = "Backbuffer Count";
		optBackBufferCount.immutable = false;
		optBackBufferCount.possibleValues.push_back( "Auto" );
		optBackBufferCount.possibleValues.push_back( "1" );
		optBackBufferCount.possibleValues.push_back( "2" );
		optBackBufferCount.currentValue = "Auto";


        optAA.name = "FSAA";
        optAA.immutable = false;
        optAA.possibleValues.push_back( "None" );
        optAA.currentValue = "None";

        optFPUMode.name = "Floating-point mode";
#if OGRE_DOUBLE_PRECISION
        optFPUMode.currentValue = "Consistent";
#else
        optFPUMode.currentValue = "Fastest";
#endif
        optFPUMode.possibleValues.clear();
        optFPUMode.possibleValues.push_back("Fastest");
        optFPUMode.possibleValues.push_back("Consistent");
        optFPUMode.immutable = false;

        optNVPerfHUD.currentValue = "No";
        optNVPerfHUD.immutable = false;
        optNVPerfHUD.name = "Allow NVPerfHUD";
        optNVPerfHUD.possibleValues.push_back( "Yes" );
        optNVPerfHUD.possibleValues.push_back( "No" );

        // SRGB on auto window
        optSRGB.name = "sRGB Gamma Conversion";
        optSRGB.possibleValues.push_back("Yes");
        optSRGB.possibleValues.push_back("No");
        optSRGB.currentValue = "No";
        optSRGB.immutable = false;      

        // min feature level
        optMinFeatureLevels;
        optMinFeatureLevels.name = "Min Requested Feature Levels";
        optMinFeatureLevels.possibleValues.push_back("9.1");
        optMinFeatureLevels.possibleValues.push_back("9.3");
        optMinFeatureLevels.possibleValues.push_back("10.0");
        optMinFeatureLevels.possibleValues.push_back("10.1");
        optMinFeatureLevels.possibleValues.push_back("11.0");

        optMinFeatureLevels.currentValue = "9.1";
        optMinFeatureLevels.immutable = false;      


        // max feature level
        optMaxFeatureLevels;
        optMaxFeatureLevels.name = "Max Requested Feature Levels";
        optMaxFeatureLevels.possibleValues.push_back("9.1");

#if __OGRE_WINRT_PHONE_80
        optMaxFeatureLevels.possibleValues.push_back("9.2");
        optMaxFeatureLevels.possibleValues.push_back("9.3");
        optMaxFeatureLevels.currentValue = "9.3";
#elif __OGRE_WINRT_PHONE || __OGRE_WINRT_STORE
        optMaxFeatureLevels.possibleValues.push_back("9.3");
        optMaxFeatureLevels.possibleValues.push_back("10.0");
        optMaxFeatureLevels.possibleValues.push_back("10.1");
        optMaxFeatureLevels.possibleValues.push_back("11.0");
        optMaxFeatureLevels.possibleValues.push_back("11.1");
        optMaxFeatureLevels.currentValue = "11.1";
#else
        optMaxFeatureLevels.possibleValues.push_back("9.3");
        optMaxFeatureLevels.possibleValues.push_back("10.0");
        optMaxFeatureLevels.possibleValues.push_back("10.1");
        optMaxFeatureLevels.possibleValues.push_back("11.0");
        optMaxFeatureLevels.currentValue = "11.0";
#endif

        optMaxFeatureLevels.immutable = false;      

        // Exceptions Error Level
        optExceptionsErrorLevel.name = "Information Queue Exceptions Bottom Level";
        optExceptionsErrorLevel.possibleValues.push_back("No information queue exceptions");
        optExceptionsErrorLevel.possibleValues.push_back("Corruption");
        optExceptionsErrorLevel.possibleValues.push_back("Error");
        optExceptionsErrorLevel.possibleValues.push_back("Warning");
        optExceptionsErrorLevel.possibleValues.push_back("Info (exception on any message)");
#if OGRE_DEBUG_MODE
        optExceptionsErrorLevel.currentValue = "Info (exception on any message)";
#else
        optExceptionsErrorLevel.currentValue = "No information queue exceptions";
#endif
        optExceptionsErrorLevel.immutable = false;
        

        // Driver type
        optDriverType.name = "Driver type";
        optDriverType.possibleValues.push_back("Hardware");
        optDriverType.possibleValues.push_back("Software");
        optDriverType.possibleValues.push_back("Warp");
        optDriverType.currentValue = "Hardware";
        optDriverType.immutable = false;

#if OGRE_NO_QUAD_BUFFER_STEREO == 0
		optStereoMode.name = "Stereo Mode";
		optStereoMode.possibleValues.push_back(StringConverter::toString(SMT_NONE));
		optStereoMode.possibleValues.push_back(StringConverter::toString(SMT_FRAME_SEQUENTIAL));
		optStereoMode.currentValue = optStereoMode.possibleValues[0];
		optStereoMode.immutable = false;
		
		mOptions[optStereoMode.name] = optStereoMode;
#endif

        mOptions[optDevice.name] = optDevice;
        mOptions[optVideoMode.name] = optVideoMode;
        mOptions[optFullScreen.name] = optFullScreen;
        mOptions[optVSync.name] = optVSync;
        mOptions[optVSyncInterval.name] = optVSyncInterval;
        mOptions[optAA.name] = optAA;
        mOptions[optFPUMode.name] = optFPUMode;
        mOptions[optNVPerfHUD.name] = optNVPerfHUD;
        mOptions[optSRGB.name] = optSRGB;
        mOptions[optMinFeatureLevels.name] = optMinFeatureLevels;
        mOptions[optMaxFeatureLevels.name] = optMaxFeatureLevels;
        mOptions[optExceptionsErrorLevel.name] = optExceptionsErrorLevel;
        mOptions[optDriverType.name] = optDriverType;

		mOptions[optBackBufferCount.name] = optBackBufferCount;

        
        refreshD3DSettings();

    }
    //---------------------------------------------------------------------
    void D3D11RenderSystem::refreshD3DSettings()
    {
        ConfigOption* optVideoMode;
        D3D11VideoMode* videoMode;

        ConfigOptionMap::iterator opt = mOptions.find( "Rendering Device" );
        if( opt != mOptions.end() )
        {
            D3D11Driver *driver = getDirect3DDrivers()->findByName(opt->second.currentValue);
            if (driver)
            {
                opt = mOptions.find( "Video Mode" );
                optVideoMode = &opt->second;
                optVideoMode->possibleValues.clear();
                // get vide modes for this device
                for( unsigned k=0; k < driver->getVideoModeList()->count(); k++ )
                {
                    videoMode = driver->getVideoModeList()->item( k );
                    optVideoMode->possibleValues.push_back( videoMode->getDescription() );
                }

                // Reset video mode to default if previous doesn't avail in new possible values
                StringVector::const_iterator itValue =
                    std::find(optVideoMode->possibleValues.begin(),
                              optVideoMode->possibleValues.end(),
                              optVideoMode->currentValue);
                if (itValue == optVideoMode->possibleValues.end())
                {
                    optVideoMode->currentValue = "800 x 600 @ 32-bit colour";
                }

                // Also refresh FSAA options
                refreshFSAAOptions();
            }
        }

    }
    //---------------------------------------------------------------------
    void D3D11RenderSystem::setConfigOption( const String &name, const String &value )
    {
        initRenderSystem();

        LogManager::getSingleton().stream()
            << "D3D11: RenderSystem Option: " << name << " = " << value;

        bool viewModeChanged = false;

        // Find option
        ConfigOptionMap::iterator it = mOptions.find( name );

        // Update
        if( it != mOptions.end() )
            it->second.currentValue = value;
        else
        {
            StringStream str;
            str << "Option named '" << name << "' does not exist.";
            OGRE_EXCEPT( Exception::ERR_INVALIDPARAMS, str.str(), "D3D11RenderSystem::setConfigOption" );
        }

        // Refresh other options if D3DDriver changed
        if( name == "Rendering Device" )
            refreshD3DSettings();

        if( name == "Full Screen" )
        {
            // Video mode is applicable
            it = mOptions.find( "Video Mode" );
            if (it->second.currentValue.empty())
            {
                it->second.currentValue = "800 x 600 @ 32-bit colour";
                viewModeChanged = true;
            }
        }

        if( name == "Min Requested Feature Levels" )
        {
            mMinRequestedFeatureLevel = D3D11Device::parseFeatureLevel(value, D3D_FEATURE_LEVEL_9_1);
        }

        if( name == "Max Requested Feature Levels" )
        {
#if defined(_WIN32_WINNT_WIN8) && _WIN32_WINNT >= _WIN32_WINNT_WIN8
            mMaxRequestedFeatureLevel = D3D11Device::parseFeatureLevel(value, D3D_FEATURE_LEVEL_11_1);
#else
            mMaxRequestedFeatureLevel = D3D11Device::parseFeatureLevel(value, D3D_FEATURE_LEVEL_11_0);
#endif
        }

        if( name == "Allow NVPerfHUD" )
        {
            if (value == "Yes")
                mUseNVPerfHUD = true;
            else
                mUseNVPerfHUD = false;
        }

        if (viewModeChanged || name == "Video Mode")
        {
            refreshFSAAOptions();
        }

    }
    //---------------------------------------------------------------------
    void D3D11RenderSystem::refreshFSAAOptions(void)
    {

        ConfigOptionMap::iterator it = mOptions.find( "FSAA" );
        ConfigOption* optFSAA = &it->second;
        optFSAA->possibleValues.clear();

        it = mOptions.find("Rendering Device");
        D3D11Driver *driver = getDirect3DDrivers()->findByName(it->second.currentValue);
        if (driver)
        {
            it = mOptions.find("Video Mode");
            ComPtr<ID3D11DeviceN> device;
            device.Attach(createD3D11Device(driver, mDriverType, mMinRequestedFeatureLevel, mMaxRequestedFeatureLevel, NULL));
            D3D11VideoMode* videoMode = driver->getVideoModeList()->item(it->second.currentValue); // Could be NULL if working over RDP/Simulator
            DXGI_FORMAT format = videoMode ? videoMode->getFormat() : DXGI_FORMAT_R8G8B8A8_UNORM;
            UINT numLevels = 0;
            // set maskable levels supported
            for (unsigned int n = 1; n <= D3D11_MAX_MULTISAMPLE_SAMPLE_COUNT; n++)
            {
                HRESULT hr = device->CheckMultisampleQualityLevels(format, n, &numLevels);
                if (SUCCEEDED(hr) && numLevels > 0)
                {
                    optFSAA->possibleValues.push_back(StringConverter::toString(n));

                    // 8x could mean 8xCSAA, and we need other designation for 8xMSAA
                    if(n == 8 && SUCCEEDED(device->CheckMultisampleQualityLevels(format, 4, &numLevels)) && numLevels > 8    // 8x CSAA
                    || n == 16 && SUCCEEDED(device->CheckMultisampleQualityLevels(format, 4, &numLevels)) && numLevels > 16  // 16x CSAA
                    || n == 16 && SUCCEEDED(device->CheckMultisampleQualityLevels(format, 8, &numLevels)) && numLevels > 16) // 16xQ CSAA
                    {
                        optFSAA->possibleValues.push_back(StringConverter::toString(n) + " [Quality]");
                    }
                }
                else if(n == 16) // there could be case when 16xMSAA is not supported but 16xCSAA and may be 16xQ CSAA are supported
                {
                    bool csaa16x = SUCCEEDED(device->CheckMultisampleQualityLevels(format, 4, &numLevels)) && numLevels > 16;
                    bool csaa16xQ = SUCCEEDED(device->CheckMultisampleQualityLevels(format, 8, &numLevels)) && numLevels > 16;
                    if(csaa16x || csaa16xQ)
                        optFSAA->possibleValues.push_back("16");
                    if(csaa16x && csaa16xQ)
                        optFSAA->possibleValues.push_back("16 [Quality]");
                }
            }
        }

        if(optFSAA->possibleValues.empty())
        {
            optFSAA->possibleValues.push_back("1"); // D3D11 does not distinguish between noMSAA and 1xMSAA
        }

        // Reset FSAA to none if previous doesn't avail in new possible values
        StringVector::const_iterator itValue =
            std::find(optFSAA->possibleValues.begin(),
                      optFSAA->possibleValues.end(),
                      optFSAA->currentValue);
        if (itValue == optFSAA->possibleValues.end())
        {
            optFSAA->currentValue = optFSAA->possibleValues[0];
        }

    }
    //---------------------------------------------------------------------
    String D3D11RenderSystem::validateConfigOptions()
    {
        ConfigOptionMap::iterator it;
        
        // check if video mode is selected
        it = mOptions.find( "Video Mode" );
        if (it->second.currentValue.empty())
            return "A video mode must be selected.";

        it = mOptions.find( "Rendering Device" );
        String driverName = it->second.currentValue;
        if(driverName != "(default)" && getDirect3DDrivers()->findByName(driverName)->DriverDescription() != driverName)
        {
            // Just pick default driver
            setConfigOption("Rendering Device", "(default)");
            return "Requested rendering device could not be found, default would be used instead.";
        }

        return BLANKSTRING;
    }
    //---------------------------------------------------------------------
    ConfigOptionMap& D3D11RenderSystem::getConfigOptions()
    {
        // return a COPY of the current config options
        return mOptions;
    }
    //---------------------------------------------------------------------
    RenderWindow* D3D11RenderSystem::_initialise( bool autoCreateWindow, const String& windowTitle )
    {
        RenderWindow* autoWindow = NULL;
        LogManager::getSingleton().logMessage( "D3D11: Subsystem Initialising" );

		if(IsWorkingUnderNsight())
			LogManager::getSingleton().logMessage( "D3D11: Nvidia Nsight found");

        // Init using current settings
        ConfigOptionMap::iterator opt = mOptions.find( "Rendering Device" );
        if( opt == mOptions.end() )
            OGRE_EXCEPT( Exception::ERR_INVALIDPARAMS, "Can`t find requested Direct3D driver name!", "D3D11RenderSystem::initialise" );
        mDriverName = opt->second.currentValue;

        // Driver type
        opt = mOptions.find( "Driver type" );
        if( opt == mOptions.end() )
            OGRE_EXCEPT( Exception::ERR_INTERNAL_ERROR, "Can't find driver type!", "D3D11RenderSystem::initialise" );
        mDriverType = D3D11Device::parseDriverType(opt->second.currentValue);

        opt = mOptions.find( "Information Queue Exceptions Bottom Level" );
        if( opt == mOptions.end() )
            OGRE_EXCEPT( Exception::ERR_INTERNAL_ERROR, "Can't find Information Queue Exceptions Bottom Level option!", "D3D11RenderSystem::initialise" );
        D3D11Device::setExceptionsErrorLevel(opt->second.currentValue);

#if OGRE_NO_QUAD_BUFFER_STEREO == 0
        // Stereo driver must be created before device is created
        StereoModeType stereoMode = StringConverter::parseStereoMode(mOptions["Stereo Mode"].currentValue);
        D3D11StereoDriverBridge* stereoBridge = OGRE_NEW D3D11StereoDriverBridge(stereoMode);
#endif

        // create the device for the selected adapter
        createDevice();

        if( autoCreateWindow )
        {
            bool fullScreen;
            opt = mOptions.find( "Full Screen" );
            if( opt == mOptions.end() )
                OGRE_EXCEPT( Exception::ERR_INTERNAL_ERROR, "Can't find full screen option!", "D3D11RenderSystem::initialise" );
            fullScreen = opt->second.currentValue == "Yes";

            D3D11VideoMode* videoMode = NULL;
            unsigned int width, height;
            String temp;

            opt = mOptions.find( "Video Mode" );
            if( opt == mOptions.end() )
                OGRE_EXCEPT( Exception::ERR_INTERNAL_ERROR, "Can't find Video Mode option!", "D3D11RenderSystem::initialise" );

            // The string we are manipulating looks like this :width x height @ colourDepth
            // Pull out the colour depth by getting what comes after the @ and a space
            String colourDepth = opt->second.currentValue.substr(opt->second.currentValue.rfind('@')+1);
            // Now we know that the width starts a 0, so if we can find the end we can parse that out
            String::size_type widthEnd = opt->second.currentValue.find(' ');
            // we know that the height starts 3 characters after the width and goes until the next space
            String::size_type heightEnd = opt->second.currentValue.find(' ', widthEnd+3);
            // Now we can parse out the values
            width = StringConverter::parseInt(opt->second.currentValue.substr(0, widthEnd));
            height = StringConverter::parseInt(opt->second.currentValue.substr(widthEnd+3, heightEnd));

            D3D11VideoModeList* videoModeList = mActiveD3DDriver.getVideoModeList();
            for( unsigned j=0; j < videoModeList->count(); j++ )
            {
                temp = videoModeList->item(j)->getDescription();

                // In full screen we only want to allow supported resolutions, so temp and opt->second.currentValue need to 
                // match exactly, but in windowed mode we can allow for arbitrary window sized, so we only need
                // to match the colour values
                if(fullScreen && (temp == opt->second.currentValue) ||
                  !fullScreen && (temp.substr(temp.rfind('@')+1) == colourDepth))
                {
                    videoMode = videoModeList->item(j);
                    break;
                }
            }

            // sRGB window option
            bool hwGamma = false;
            opt = mOptions.find( "sRGB Gamma Conversion" );
            if( opt == mOptions.end() )
                OGRE_EXCEPT( Exception::ERR_INTERNAL_ERROR, "Can't find sRGB option!", "D3D11RenderSystem::initialise" );
            hwGamma = opt->second.currentValue == "Yes";
            uint fsaa = 0;
            String fsaaHint;
            if( (opt = mOptions.find("FSAA")) != mOptions.end() )
            {
                StringVector values = StringUtil::split(opt->second.currentValue, " ", 1);
                fsaa = StringConverter::parseUnsignedInt(values[0]);
                if (values.size() > 1)
                    fsaaHint = values[1];
            }

            if( !videoMode )
            {
                LogManager::getSingleton().logWarning(
                            "D3D11: Couldn't find requested video mode. Forcing 32bpp. "
                            "If you have two GPUs and you're rendering to the GPU that is not "
                            "plugged to the monitor you can then ignore this message.");
            }

            NameValuePairList miscParams;
            miscParams["colourDepth"] = StringConverter::toString(videoMode ? videoMode->getColourDepth() : 32);
            miscParams["FSAA"] = StringConverter::toString(fsaa);
            miscParams["FSAAHint"] = fsaaHint;
            miscParams["useNVPerfHUD"] = StringConverter::toString(mUseNVPerfHUD);
            miscParams["gamma"] = StringConverter::toString(hwGamma);
            //miscParams["useFlipMode"] = StringConverter::toString(true);

            opt = mOptions.find("VSync");
            if (opt == mOptions.end())
                OGRE_EXCEPT(Exception::ERR_INVALIDPARAMS, "Can't find VSync options!", "D3D11RenderSystem::initialise");
            bool vsync = (opt->second.currentValue == "Yes");
            miscParams["vsync"] = StringConverter::toString(vsync);

            opt = mOptions.find("VSync Interval");
            if (opt == mOptions.end())
                OGRE_EXCEPT(Exception::ERR_INVALIDPARAMS, "Can't find VSync Interval options!", "D3D11RenderSystem::initialise");
            miscParams["vsyncInterval"] = opt->second.currentValue;

            autoWindow = this->_createRenderWindow( windowTitle, width, height, 
                fullScreen, &miscParams );

            // If we have 16bit depth buffer enable w-buffering.
            assert( autoWindow );
            if ( autoWindow->getColourDepth() == 16 ) 
            { 
                mWBuffer = true;
            } 
            else 
            {
                mWBuffer = false;
            }           
        }

        LogManager::getSingleton().logMessage("***************************************");
        LogManager::getSingleton().logMessage("*** D3D11: Subsystem Initialized OK ***");
        LogManager::getSingleton().logMessage("***************************************");

        // call superclass method
        RenderSystem::_initialise( autoCreateWindow );
        this->fireDeviceEvent(&mDevice, "DeviceCreated");
        return autoWindow;
    }
    //---------------------------------------------------------------------
    void D3D11RenderSystem::reinitialise()
    {
        LogManager::getSingleton().logMessage( "D3D11: Reinitializing" );
        this->shutdown();
    //  this->initialise( true );
    }
    //---------------------------------------------------------------------
    void D3D11RenderSystem::shutdown()
    {
        RenderSystem::shutdown();

        mRenderSystemWasInited = false;

        mPrimaryWindow = NULL; // primary window deleted by base class.
        freeDevice();
        SAFE_DELETE( mDriverList );
        mActiveD3DDriver = D3D11Driver();
        mDevice.ReleaseAll();
        LogManager::getSingleton().logMessage("D3D11: Shutting down cleanly.");
        SAFE_DELETE( mTextureManager );
        SAFE_DELETE( mHardwareBufferManager );
        SAFE_DELETE( mGpuProgramManager );

    }
    //---------------------------------------------------------------------
	RenderWindow* D3D11RenderSystem::_createRenderWindow(const String &name,
		unsigned int width, unsigned int height, bool fullScreen,
		const NameValuePairList *miscParams)
	{

		// Check we're not creating a secondary window when the primary
		// was fullscreen
		if (mPrimaryWindow && mPrimaryWindow->isFullScreen() && fullScreen == false)
		{
			OGRE_EXCEPT(Exception::ERR_INVALID_STATE,
				"Cannot create secondary windows not in full screen when the primary is full screen",
				"D3D11RenderSystem::_createRenderWindow");
		}

		// Log a message
		StringStream ss;
		ss << "D3D11RenderSystem::_createRenderWindow \"" << name << "\", " <<
			width << "x" << height << " ";
		if (fullScreen)
			ss << "fullscreen ";
		else
			ss << "windowed ";
		if (miscParams)
		{
			ss << " miscParams: ";
			NameValuePairList::const_iterator it;
			for (it = miscParams->begin(); it != miscParams->end(); ++it)
			{
				ss << it->first << "=" << it->second << " ";
			}
			LogManager::getSingleton().logMessage(ss.str());
		}

		String msg;

		// Make sure we don't already have a render target of the 
		// sam name as the one supplied
		if (mRenderTargets.find(name) != mRenderTargets.end())
		{
			msg = "A render target of the same name '" + name + "' already "
				"exists.  You cannot create a new window with this name.";
			OGRE_EXCEPT(Exception::ERR_INTERNAL_ERROR, msg, "D3D11RenderSystem::_createRenderWindow");
		}

#if OGRE_PLATFORM == OGRE_PLATFORM_WIN32
		D3D11RenderWindowBase* win = new D3D11RenderWindowHwnd(mDevice);
#elif OGRE_PLATFORM == OGRE_PLATFORM_WINRT
		String windowType;
		if(miscParams)
		{
			// Get variable-length params
			NameValuePairList::const_iterator opt = miscParams->find("windowType");
			if(opt != miscParams->end())
				windowType = opt->second;
		}

		D3D11RenderWindowBase* win = NULL;
#if !__OGRE_WINRT_PHONE_80
		if(win == NULL && windowType == "SurfaceImageSource")
			win = new D3D11RenderWindowImageSource(mDevice);
		if(win == NULL && windowType == "SwapChainPanel")
			win = new D3D11RenderWindowSwapChainPanel(mDevice);
#endif // !__OGRE_WINRT_PHONE_80
		if(win == NULL)
			win = new D3D11RenderWindowCoreWindow(mDevice);
#endif
		win->create(name, width, height, fullScreen, miscParams);

		attachRenderTarget(*win);

#if OGRE_NO_QUAD_BUFFER_STEREO == 0
		// Must be called after device has been linked to window
		D3D11StereoDriverBridge::getSingleton().addRenderWindow(win);
		win->_validateStereo();
#endif

		// If this is the first window, get the D3D device and create the texture manager
		if (!mPrimaryWindow)
		{
			mPrimaryWindow = win;
			win->getCustomAttribute("D3DDEVICE", &mDevice);

			// Create the texture manager for use by others
			mTextureManager = new D3D11TextureManager(mDevice);
			// Also create hardware buffer manager
			mHardwareBufferManager = new D3D11HardwareBufferManager(mDevice);

			// Create the GPU program manager
			mGpuProgramManager = new D3D11GpuProgramManager();
			// create & register HLSL factory
			if (mHLSLProgramFactory == NULL)
				mHLSLProgramFactory = new D3D11HLSLProgramFactory(mDevice);
			mRealCapabilities = createRenderSystemCapabilities();

			// if we are using custom capabilities, then 
			// mCurrentCapabilities has already been loaded
			if (!mUseCustomCapabilities)
				mCurrentCapabilities = mRealCapabilities;

			fireEvent("RenderSystemCapabilitiesCreated");

			initialiseFromRenderSystemCapabilities(mCurrentCapabilities, mPrimaryWindow);

		}
		else
		{
			mSecondaryWindows.push_back(win);
		}

		return win;
	}

    //---------------------------------------------------------------------
    void D3D11RenderSystem::fireDeviceEvent(D3D11Device* device, const String & name, D3D11RenderWindowBase* sendingWindow /* = NULL */)
    {
        NameValuePairList params;
        params["D3DDEVICE"] =  StringConverter::toString((size_t)device->get());
        if(sendingWindow)
            params["RenderWindow"] = StringConverter::toString((size_t)sendingWindow);
        fireEvent(name, &params);
    }
    //---------------------------------------------------------------------
    RenderSystemCapabilities* D3D11RenderSystem::createRenderSystemCapabilities() const
    {
        RenderSystemCapabilities* rsc = new RenderSystemCapabilities();
        rsc->setDriverVersion(mDriverVersion);
        rsc->setDeviceName(mActiveD3DDriver.DriverDescription());
        rsc->setRenderSystemName(getName());

		rsc->setCapability(RSC_ADVANCED_BLEND_OPERATIONS);
		
        // Does NOT support fixed-function!
        //rsc->setCapability(RSC_FIXED_FUNCTION);

        rsc->setCapability(RSC_HWSTENCIL);
        rsc->setStencilBufferBitDepth(8);

        UINT formatSupport;
        if(mFeatureLevel >= D3D_FEATURE_LEVEL_9_2
        || SUCCEEDED(mDevice->CheckFormatSupport(DXGI_FORMAT_R32_UINT, &formatSupport)) && 0 != (formatSupport & D3D11_FORMAT_SUPPORT_IA_INDEX_BUFFER))
            rsc->setCapability(RSC_32BIT_INDEX);

        // Set number of texture units, cap at OGRE_MAX_TEXTURE_LAYERS
        rsc->setNumTextureUnits(OGRE_MAX_TEXTURE_LAYERS);
        rsc->setNumVertexAttributes(D3D11_STANDARD_VERTEX_ELEMENT_COUNT);
        rsc->setCapability(RSC_ANISOTROPY);
        rsc->setCapability(RSC_AUTOMIPMAP);
        rsc->setCapability(RSC_AUTOMIPMAP_COMPRESSED);
        rsc->setCapability(RSC_DOT3);
        // Cube map
        if (mFeatureLevel >= D3D_FEATURE_LEVEL_10_0)
        {
            rsc->setCapability(RSC_CUBEMAPPING);
            rsc->setCapability(RSC_READ_BACK_AS_TEXTURE);
        }

        // We always support compression, D3DX will decompress if device does not support
        rsc->setCapability(RSC_TEXTURE_COMPRESSION);
        rsc->setCapability(RSC_TEXTURE_COMPRESSION_DXT);
        rsc->setCapability(RSC_SCISSOR_TEST);

		if(mFeatureLevel >= D3D_FEATURE_LEVEL_10_0)
			rsc->setCapability(RSC_TWO_SIDED_STENCIL);

        rsc->setCapability(RSC_STENCIL_WRAP);
        rsc->setCapability(RSC_HWOCCLUSION);
        rsc->setCapability(RSC_HWOCCLUSION_ASYNCHRONOUS);

        convertVertexShaderCaps(rsc);
        convertPixelShaderCaps(rsc);
        convertGeometryShaderCaps(rsc);
        convertHullShaderCaps(rsc);
        convertDomainShaderCaps(rsc);
        convertComputeShaderCaps(rsc);
        rsc->addShaderProfile("hlsl");

        // Check support for dynamic linkage
        if (mFeatureLevel >= D3D_FEATURE_LEVEL_11_0)
        {
            rsc->setCapability(RSC_SHADER_SUBROUTINE);
        }

        rsc->setCapability(RSC_USER_CLIP_PLANES);
        rsc->setCapability(RSC_VERTEX_FORMAT_UBYTE4);

        rsc->setCapability(RSC_RTT_SEPARATE_DEPTHBUFFER);
        rsc->setCapability(RSC_RTT_MAIN_DEPTHBUFFER_ATTACHABLE);


        // Adapter details
        const DXGI_ADAPTER_DESC1& adapterID = mActiveD3DDriver.getAdapterIdentifier();

        switch(mDriverType) {
        case D3D_DRIVER_TYPE_HARDWARE:
            // determine vendor
            // Full list of vendors here: http://www.pcidatabase.com/vendors.php?sort=id
            switch(adapterID.VendorId)
            {
            case 0x10DE:
                rsc->setVendor(GPU_NVIDIA);
                break;
            case 0x1002:
                rsc->setVendor(GPU_AMD);
                break;
            case 0x163C:
            case 0x8086:
                rsc->setVendor(GPU_INTEL);
                break;
            default:
                rsc->setVendor(GPU_UNKNOWN);
                break;
            };
            break;
        case D3D_DRIVER_TYPE_SOFTWARE:
            rsc->setVendor(GPU_MS_SOFTWARE);
            break;
        case D3D_DRIVER_TYPE_WARP:
            rsc->setVendor(GPU_MS_WARP);
            break;
        default:
            rsc->setVendor(GPU_UNKNOWN);
            break;
        }

        rsc->setCapability(RSC_INFINITE_FAR_PLANE);

        rsc->setCapability(RSC_TEXTURE_3D);
        if (mFeatureLevel >= D3D_FEATURE_LEVEL_10_0)
        {
            rsc->setCapability(RSC_NON_POWER_OF_2_TEXTURES);
            rsc->setCapability(RSC_HWRENDER_TO_TEXTURE_3D);
            rsc->setCapability(RSC_TEXTURE_1D);
            rsc->setCapability(RSC_TEXTURE_COMPRESSION_BC6H_BC7);
            rsc->setCapability(RSC_COMPLETE_TEXTURE_BINDING);
        }

        rsc->setCapability(RSC_HWRENDER_TO_TEXTURE);
        rsc->setCapability(RSC_TEXTURE_FLOAT);

#ifdef D3D_FEATURE_LEVEL_9_3
        int numMultiRenderTargets = (mFeatureLevel > D3D_FEATURE_LEVEL_9_3) ? D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT :      // 8
                                    (mFeatureLevel == D3D_FEATURE_LEVEL_9_3) ? 4/*D3D_FL9_3_SIMULTANEOUS_RENDER_TARGET_COUNT*/ :    // 4
                                    1/*D3D_FL9_1_SIMULTANEOUS_RENDER_TARGET_COUNT*/;                                                // 1
#else
        int numMultiRenderTargets = D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT;     // 8
#endif

        rsc->setNumMultiRenderTargets(std::min(numMultiRenderTargets, (int)OGRE_MAX_MULTIPLE_RENDER_TARGETS));
        rsc->setCapability(RSC_MRT_DIFFERENT_BIT_DEPTHS);

        rsc->setCapability(RSC_POINT_SPRITES);
        rsc->setCapability(RSC_POINT_EXTENDED_PARAMETERS);
        rsc->setMaxPointSize(256); // TODO: guess!
    
        rsc->setCapability(RSC_VERTEX_TEXTURE_FETCH);
        rsc->setNumVertexTextureUnits(4);
        rsc->setVertexTextureUnitsShared(false);

        rsc->setCapability(RSC_MIPMAP_LOD_BIAS);

        // actually irrelevant, but set
        rsc->setCapability(RSC_PERSTAGECONSTANT);

        rsc->setCapability(RSC_VERTEX_BUFFER_INSTANCE_DATA);
        rsc->setCapability(RSC_CAN_GET_COMPILED_SHADER_BUFFER);

        return rsc;

    }
    //-----------------------------------------------------------------------
    void D3D11RenderSystem::initialiseFromRenderSystemCapabilities(
        RenderSystemCapabilities* caps, RenderTarget* primary)
    {
        if(caps->getRenderSystemName() != getName())
        {
            OGRE_EXCEPT(Exception::ERR_INVALIDPARAMS, 
                "Trying to initialize D3D11RenderSystem from RenderSystemCapabilities that do not support Direct3D11",
                "D3D11RenderSystem::initialiseFromRenderSystemCapabilities");
        }
        
        // add hlsl
        HighLevelGpuProgramManager::getSingleton().addFactory(mHLSLProgramFactory);

        Log* defaultLog = LogManager::getSingleton().getDefaultLog();
        if (defaultLog)
        {
            caps->log(defaultLog);
        }
    }
    //---------------------------------------------------------------------
    void D3D11RenderSystem::convertVertexShaderCaps(RenderSystemCapabilities* rsc) const
    {
        if (mFeatureLevel >= D3D_FEATURE_LEVEL_9_1)
        {
            rsc->addShaderProfile("vs_4_0_level_9_1");
#if SUPPORT_SM2_0_HLSL_SHADERS == 1
            rsc->addShaderProfile("vs_2_0");
#endif
        }
        if (mFeatureLevel >= D3D_FEATURE_LEVEL_9_3)
        {
            rsc->addShaderProfile("vs_4_0_level_9_3");
#if SUPPORT_SM2_0_HLSL_SHADERS == 1
            rsc->addShaderProfile("vs_2_a");
            rsc->addShaderProfile("vs_2_x");
#endif
        }
        if (mFeatureLevel >= D3D_FEATURE_LEVEL_10_0)
        {
            rsc->addShaderProfile("vs_4_0");
#if SUPPORT_SM2_0_HLSL_SHADERS == 1
            rsc->addShaderProfile("vs_3_0");
#endif
        }
        if (mFeatureLevel >= D3D_FEATURE_LEVEL_10_1)
        {
            rsc->addShaderProfile("vs_4_1");
        }
        if (mFeatureLevel >= D3D_FEATURE_LEVEL_11_0)
        {
            rsc->addShaderProfile("vs_5_0");
        }

        rsc->setCapability(RSC_VERTEX_PROGRAM);

        // TODO: constant buffers have no limits but lower models do
        // 16 boolean params allowed
        rsc->setVertexProgramConstantBoolCount(16);
        // 16 integer params allowed, 4D
        rsc->setVertexProgramConstantIntCount(16);
        // float params, always 4D
        rsc->setVertexProgramConstantFloatCount(512);

    }
    //---------------------------------------------------------------------
    void D3D11RenderSystem::convertPixelShaderCaps(RenderSystemCapabilities* rsc) const
    {
        if (mFeatureLevel >= D3D_FEATURE_LEVEL_9_1)
        {
            rsc->addShaderProfile("ps_4_0_level_9_1");
#if SUPPORT_SM2_0_HLSL_SHADERS == 1
            rsc->addShaderProfile("ps_2_0");
#endif
        }
        if (mFeatureLevel >= D3D_FEATURE_LEVEL_9_3)
        {
            rsc->addShaderProfile("ps_4_0_level_9_3");
#if SUPPORT_SM2_0_HLSL_SHADERS == 1
            rsc->addShaderProfile("ps_2_a");
            rsc->addShaderProfile("ps_2_b");
            rsc->addShaderProfile("ps_2_x");
#endif
        }
        if (mFeatureLevel >= D3D_FEATURE_LEVEL_10_0)
        {
            rsc->addShaderProfile("ps_4_0");
#if SUPPORT_SM2_0_HLSL_SHADERS == 1
            rsc->addShaderProfile("ps_3_0");
            rsc->addShaderProfile("ps_3_x");
#endif
        }
        if (mFeatureLevel >= D3D_FEATURE_LEVEL_10_1)
        {
            rsc->addShaderProfile("ps_4_1");
        }
        if (mFeatureLevel >= D3D_FEATURE_LEVEL_11_0)
        {
            rsc->addShaderProfile("ps_5_0");
        }


        rsc->setCapability(RSC_FRAGMENT_PROGRAM);


        // TODO: constant buffers have no limits but lower models do
        // 16 boolean params allowed
        rsc->setFragmentProgramConstantBoolCount(16);
        // 16 integer params allowed, 4D
        rsc->setFragmentProgramConstantIntCount(16);
        // float params, always 4D
        rsc->setFragmentProgramConstantFloatCount(512);

    }
    //---------------------------------------------------------------------
    void D3D11RenderSystem::convertHullShaderCaps(RenderSystemCapabilities* rsc) const
    {
        // Only for shader model 5.0
        if (mFeatureLevel >= D3D_FEATURE_LEVEL_11_0)
        {
            rsc->addShaderProfile("hs_5_0");
            
            rsc->setCapability(RSC_TESSELLATION_HULL_PROGRAM);

            // TODO: constant buffers have no limits but lower models do
            // 16 boolean params allowed
            rsc->setTessellationHullProgramConstantBoolCount(16);
            // 16 integer params allowed, 4D
            rsc->setTessellationHullProgramConstantIntCount(16);
            // float params, always 4D
            rsc->setTessellationHullProgramConstantFloatCount(512);
        }

    }
    //---------------------------------------------------------------------
    void D3D11RenderSystem::convertDomainShaderCaps(RenderSystemCapabilities* rsc) const
    {
        // Only for shader model 5.0
        if (mFeatureLevel >= D3D_FEATURE_LEVEL_11_0)
        {
            rsc->addShaderProfile("ds_5_0");

            rsc->setCapability(RSC_TESSELLATION_DOMAIN_PROGRAM);


            // TODO: constant buffers have no limits but lower models do
            // 16 boolean params allowed
            rsc->setTessellationDomainProgramConstantBoolCount(16);
            // 16 integer params allowed, 4D
            rsc->setTessellationDomainProgramConstantIntCount(16);
            // float params, always 4D
            rsc->setTessellationDomainProgramConstantFloatCount(512);
        }

    }
    //---------------------------------------------------------------------
    void D3D11RenderSystem::convertComputeShaderCaps(RenderSystemCapabilities* rsc) const
    {

        if (mFeatureLevel >= D3D_FEATURE_LEVEL_10_0)
        {
            rsc->addShaderProfile("cs_4_0");
            rsc->setCapability(RSC_COMPUTE_PROGRAM);
        }
        if (mFeatureLevel >= D3D_FEATURE_LEVEL_10_1)
        {
            rsc->addShaderProfile("cs_4_1");
        }
        if (mFeatureLevel >= D3D_FEATURE_LEVEL_11_0)
        {
            rsc->addShaderProfile("cs_5_0");
        }



        // TODO: constant buffers have no limits but lower models do
        // 16 boolean params allowed
        rsc->setComputeProgramConstantBoolCount(16);
        // 16 integer params allowed, 4D
        rsc->setComputeProgramConstantIntCount(16);
        // float params, always 4D
        rsc->setComputeProgramConstantFloatCount(512);

    }
    //---------------------------------------------------------------------
    void D3D11RenderSystem::convertGeometryShaderCaps(RenderSystemCapabilities* rsc) const
    {
        if (mFeatureLevel >= D3D_FEATURE_LEVEL_10_0)
        {
            rsc->addShaderProfile("gs_4_0");
            rsc->setCapability(RSC_GEOMETRY_PROGRAM);
            rsc->setCapability(RSC_HWRENDER_TO_VERTEX_BUFFER);
        }
        if (mFeatureLevel >= D3D_FEATURE_LEVEL_10_1)
        {
            rsc->addShaderProfile("gs_4_1");
        }
        if (mFeatureLevel >= D3D_FEATURE_LEVEL_11_0)
        {
            rsc->addShaderProfile("gs_5_0");
        }

        rsc->setGeometryProgramConstantFloatCount(512);
        rsc->setGeometryProgramConstantIntCount(16);
        rsc->setGeometryProgramConstantBoolCount(16);
        rsc->setGeometryProgramNumOutputVertices(1024);
    }
    //-----------------------------------------------------------------------
    bool D3D11RenderSystem::checkVertexTextureFormats(void)
    {
        return true;
    }
    //-----------------------------------------------------------------------
    bool D3D11RenderSystem::_checkTextureFilteringSupported(TextureType ttype, PixelFormat format, int usage)
    {
        return true;
    }
    //-----------------------------------------------------------------------
    MultiRenderTarget * D3D11RenderSystem::createMultiRenderTarget(const String & name)
    {
        MultiRenderTarget *retval;
        retval = new D3D11MultiRenderTarget(name);
        attachRenderTarget(*retval);

        return retval;
    }
    //-----------------------------------------------------------------------
    DepthBuffer* D3D11RenderSystem::_createDepthBufferFor( RenderTarget *renderTarget )
    {
        //Get surface data (mainly to get MSAA data)
        D3D11HardwarePixelBuffer *pBuffer;
        renderTarget->getCustomAttribute( "BUFFER", &pBuffer );
        D3D11_TEXTURE2D_DESC BBDesc;
        static_cast<ID3D11Texture2D*>(pBuffer->getParentTexture()->getTextureResource())->GetDesc( &BBDesc );

        // Create depth stencil texture
        ComPtr<ID3D11Texture2D> pDepthStencil;
        D3D11_TEXTURE2D_DESC descDepth;

        descDepth.Width                 = renderTarget->getWidth();
        descDepth.Height                = renderTarget->getHeight();
        descDepth.MipLevels             = 1;
        descDepth.ArraySize             = BBDesc.ArraySize;

        if ( mFeatureLevel < D3D_FEATURE_LEVEL_10_0)
            descDepth.Format            = DXGI_FORMAT_D24_UNORM_S8_UINT;
        else
            descDepth.Format            = DXGI_FORMAT_R32_TYPELESS;

        descDepth.SampleDesc.Count      = BBDesc.SampleDesc.Count;
        descDepth.SampleDesc.Quality    = BBDesc.SampleDesc.Quality;
        descDepth.Usage                 = D3D11_USAGE_DEFAULT;
        descDepth.BindFlags             = D3D11_BIND_DEPTH_STENCIL;

        // If we tell we want to use it as a Shader Resource when in MSAA, we will fail
        // This is a recomandation from NVidia.
        if(!mReadBackAsTexture && mFeatureLevel >= D3D_FEATURE_LEVEL_10_0 && BBDesc.SampleDesc.Count == 1)
            descDepth.BindFlags |= D3D11_BIND_SHADER_RESOURCE;

        descDepth.CPUAccessFlags        = 0;
        descDepth.MiscFlags             = 0;

        if (descDepth.ArraySize == 6)
        {
            descDepth.MiscFlags     |= D3D11_RESOURCE_MISC_TEXTURECUBE;
        }


        HRESULT hr = mDevice->CreateTexture2D( &descDepth, NULL, pDepthStencil.ReleaseAndGetAddressOf() );
        if( FAILED(hr) || mDevice.isError())
        {
            String errorDescription = mDevice.getErrorDescription(hr);
			OGRE_EXCEPT_EX(Exception::ERR_RENDERINGAPI_ERROR, hr,
                "Unable to create depth texture\nError Description:" + errorDescription,
                "D3D11RenderSystem::_createDepthBufferFor");
        }

        //
        // Create the View of the texture
        // If MSAA is used, we cannot do this
        //
        if(!mReadBackAsTexture && mFeatureLevel >= D3D_FEATURE_LEVEL_10_0 && BBDesc.SampleDesc.Count == 1)
        {
            D3D11_SHADER_RESOURCE_VIEW_DESC viewDesc;
            viewDesc.Format = DXGI_FORMAT_R32_FLOAT;
            viewDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            viewDesc.Texture2D.MostDetailedMip = 0;
            viewDesc.Texture2D.MipLevels = 1;
            HRESULT hr = mDevice->CreateShaderResourceView( pDepthStencil.Get(), &viewDesc, mDSTResView.ReleaseAndGetAddressOf());
            if( FAILED(hr) || mDevice.isError())
            {
                String errorDescription = mDevice.getErrorDescription(hr);
                OGRE_EXCEPT_EX(Exception::ERR_RENDERINGAPI_ERROR, hr,
                    "Unable to create the view of the depth texture \nError Description:" + errorDescription,
                    "D3D11RenderSystem::_createDepthBufferFor");
            }
        }

        // Create the depth stencil view
        ID3D11DepthStencilView      *depthStencilView;
        D3D11_DEPTH_STENCIL_VIEW_DESC descDSV;
        ZeroMemory( &descDSV, sizeof(D3D11_DEPTH_STENCIL_VIEW_DESC) );

        if (mFeatureLevel < D3D_FEATURE_LEVEL_10_0)
            descDSV.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
        else
            descDSV.Format = DXGI_FORMAT_D32_FLOAT;

        descDSV.ViewDimension = (BBDesc.SampleDesc.Count > 1) ? D3D11_DSV_DIMENSION_TEXTURE2DMS : D3D11_DSV_DIMENSION_TEXTURE2D;
        descDSV.Flags = 0 /* D3D11_DSV_READ_ONLY_DEPTH | D3D11_DSV_READ_ONLY_STENCIL */;    // TODO: Allows bind depth buffer as depth view AND texture simultaneously.
                                                                                            // TODO: Decide how to expose this feature
        descDSV.Texture2D.MipSlice = 0;
        hr = mDevice->CreateDepthStencilView( pDepthStencil.Get(), &descDSV, &depthStencilView );
        if( FAILED(hr) )
        {
			String errorDescription = mDevice.getErrorDescription(hr);
			OGRE_EXCEPT_EX(Exception::ERR_RENDERINGAPI_ERROR, hr,
                "Unable to create depth stencil view\nError Description:" + errorDescription,
                "D3D11RenderSystem::_createDepthBufferFor");
        }

        //Create the abstract container
        D3D11DepthBuffer *newDepthBuffer = new D3D11DepthBuffer( DepthBuffer::POOL_DEFAULT, this, depthStencilView,
                                                descDepth.Width, descDepth.Height,
                                                descDepth.SampleDesc.Count, descDepth.SampleDesc.Quality,
                                                false );

        return newDepthBuffer;
    }
    //---------------------------------------------------------------------
    void D3D11RenderSystem::_removeManualDepthBuffer(DepthBuffer *depthBuffer)
    {
        if(depthBuffer != NULL)
        {
            DepthBufferVec& pool = mDepthBufferPool[depthBuffer->getPoolId()];
            pool.erase(std::remove(pool.begin(), pool.end(), depthBuffer), pool.end());
        }
    }
    //---------------------------------------------------------------------
    DepthBuffer* D3D11RenderSystem::_addManualDepthBuffer( ID3D11DepthStencilView *depthSurface,
                                                            uint32 width, uint32 height,
                                                            uint32 fsaa, uint32 fsaaQuality )
    {
        //If this depth buffer was already added, return that one
        DepthBufferVec::const_iterator itor = mDepthBufferPool[DepthBuffer::POOL_DEFAULT].begin();
        DepthBufferVec::const_iterator end  = mDepthBufferPool[DepthBuffer::POOL_DEFAULT].end();

        while( itor != end )
        {
            if( static_cast<D3D11DepthBuffer*>(*itor)->getDepthStencilView() == depthSurface )
                return *itor;

            ++itor;
        }

        //Create a new container for it
        D3D11DepthBuffer *newDepthBuffer = new D3D11DepthBuffer( DepthBuffer::POOL_DEFAULT, this, depthSurface,
                                                                    width, height, fsaa, fsaaQuality, true );

        //Add the 'main' depth buffer to the pool
        mDepthBufferPool[newDepthBuffer->getPoolId()].push_back( newDepthBuffer );

        return newDepthBuffer;
    }
    //---------------------------------------------------------------------
    RenderTarget* D3D11RenderSystem::detachRenderTarget(const String &name)
    {
        RenderTarget* target = RenderSystem::detachRenderTarget(name);
        detachRenderTargetImpl(name);
        return target;
    }
    //---------------------------------------------------------------------
    void D3D11RenderSystem::detachRenderTargetImpl(const String& name)
    {
        // Check in specialized lists
		if (mPrimaryWindow != NULL && mPrimaryWindow->getName() == name)
        {
            // We're destroying the primary window, so reset device and window
			mPrimaryWindow = NULL;
        }
        else
        {
            // Check secondary windows
            SecondaryWindowList::iterator sw;
            for (sw = mSecondaryWindows.begin(); sw != mSecondaryWindows.end(); ++sw)
            {
                if ((*sw)->getName() == name)
                {
                    mSecondaryWindows.erase(sw);
                    break;
                }
            }
        }
    }
    //---------------------------------------------------------------------
    void D3D11RenderSystem::destroyRenderTarget(const String& name)
    {
#if OGRE_NO_QUAD_BUFFER_STEREO == 0
		D3D11StereoDriverBridge::getSingleton().removeRenderWindow(name);
#endif

        detachRenderTargetImpl(name);

        // Do the real removal
        RenderSystem::destroyRenderTarget(name);

        // Did we destroy the primary?
        if (!mPrimaryWindow)
        {
            // device is no longer valid, so free it all up
            freeDevice();
        }

    }
    //-----------------------------------------------------------------------
    void D3D11RenderSystem::freeDevice(void)
    {
        if (!mDevice.isNull() && mCurrentCapabilities)
        {
            // Set all texture units to nothing to release texture surfaces
            _disableTextureUnitsFrom(0);
            // Unbind any vertex streams to avoid memory leaks
            /*for (unsigned int i = 0; i < mLastVertexSourceCount; ++i)
            {
                HRESULT hr = mDevice->SetStreamSource(i, NULL, 0, 0);
            }
            */
            // Clean up depth stencil surfaces
            mDevice.ReleaseAll();
        }
    }
    //---------------------------------------------------------------------
    void D3D11RenderSystem::createDevice()
    {
        mDevice.ReleaseAll();

        D3D11Driver* d3dDriver = getDirect3DDrivers(true)->findByName(mDriverName);
        mActiveD3DDriver = *d3dDriver; // store copy of selected driver, so that it is not lost when drivers would be re-enumerated
        LogManager::getSingleton().stream() << "D3D11: Requested \"" << mDriverName << "\", selected \"" << d3dDriver->DriverDescription() << "\"";

        if(D3D11Driver* nvPerfHudDriver = (mDriverType == D3D_DRIVER_TYPE_HARDWARE && mUseNVPerfHUD) ? getDirect3DDrivers()->item("NVIDIA PerfHUD") : NULL)
        {
            d3dDriver = nvPerfHudDriver;
            LogManager::getSingleton().logMessage("D3D11: Actually \"NVIDIA PerfHUD\" is used");
        }

        ID3D11DeviceN * device = createD3D11Device(d3dDriver, mDriverType, mMinRequestedFeatureLevel, mMaxRequestedFeatureLevel, &mFeatureLevel);
        mDevice.TransferOwnership(device);

        LARGE_INTEGER driverVersion = mDevice.GetDriverVersion();
        mDriverVersion.major = HIWORD(driverVersion.HighPart);
        mDriverVersion.minor = LOWORD(driverVersion.HighPart);
        mDriverVersion.release = HIWORD(driverVersion.LowPart);
        mDriverVersion.build = LOWORD(driverVersion.LowPart);
    }
    //-----------------------------------------------------------------------
    void D3D11RenderSystem::handleDeviceLost()
    {
        LogManager::getSingleton().logMessage("D3D11: Device was lost, recreating.");

        // release device depended resources
        fireDeviceEvent(&mDevice, "DeviceLost");

        SceneManagerEnumerator::SceneManagerIterator scnIt = SceneManagerEnumerator::getSingleton().getSceneManagerIterator();
        while(scnIt.hasMoreElements())
            scnIt.getNext()->_releaseManualHardwareResources();

        notifyDeviceLost(&mDevice);

        // Release all automatic temporary buffers and free unused
        // temporary buffers, so we doesn't need to recreate them,
        // and they will reallocate on demand.
        HardwareBufferManager::getSingleton()._releaseBufferCopies(true);

        // Cleanup depth stencils surfaces.
        _cleanupDepthBuffers();

        // recreate device
        createDevice();

        // recreate device depended resources
        notifyDeviceRestored(&mDevice);

        MeshManager::getSingleton().reloadAll(Resource::LF_PRESERVE_STATE);

        scnIt = SceneManagerEnumerator::getSingleton().getSceneManagerIterator();
        while(scnIt.hasMoreElements())
            scnIt.getNext()->_restoreManualHardwareResources();

        // Invalidate active view port.
        mActiveViewport = NULL;

        fireDeviceEvent(&mDevice, "DeviceRestored");

        LogManager::getSingleton().logMessage("D3D11: Device was restored.");
    }
    //---------------------------------------------------------------------
    void D3D11RenderSystem::validateDevice(bool forceDeviceElection)
    {
        if(mDevice.isNull())
            return;

        // The D3D Device is no longer valid if the elected adapter changes or if
        // the device has been removed.

        bool anotherIsElected = false;
        if(forceDeviceElection)
        {
            // elect new device
            D3D11Driver* newDriver = getDirect3DDrivers(true)->findByName(mDriverName);

            // check by LUID
            LUID newLUID = newDriver->getAdapterIdentifier().AdapterLuid;
            LUID prevLUID = mActiveD3DDriver.getAdapterIdentifier().AdapterLuid;
            anotherIsElected = (newLUID.LowPart != prevLUID.LowPart) || (newLUID.HighPart != prevLUID.HighPart);
        }

        if(anotherIsElected || mDevice.IsDeviceLost())
        {
            handleDeviceLost();
        }
    }
    //-----------------------------------------------------------------------
    void D3D11RenderSystem::_updateAllRenderTargets(bool swapBuffers)
    {
        try
        {
            RenderSystem::_updateAllRenderTargets(swapBuffers);
        }
        catch(const D3D11RenderingAPIException& e)
        {
            if(e.getHResult() == DXGI_ERROR_DEVICE_REMOVED || e.getHResult() == DXGI_ERROR_DEVICE_RESET)
                LogManager::getSingleton().logMessage("D3D11: Device was lost while rendering.");
            else
                throw;
        }
    }
    //-----------------------------------------------------------------------
    void D3D11RenderSystem::_swapAllRenderTargetBuffers()
    {
        try
        {
            RenderSystem::_swapAllRenderTargetBuffers();
        }
        catch(const D3D11RenderingAPIException& e)
        {
            if(e.getHResult() == DXGI_ERROR_DEVICE_REMOVED || e.getHResult() == DXGI_ERROR_DEVICE_RESET)
                LogManager::getSingleton().logMessage("D3D11: Device was lost while rendering.");
            else
                throw;
        }
    }
    //---------------------------------------------------------------------
    VertexElementType D3D11RenderSystem::getColourVertexElementType(void) const
    {
        return VET_COLOUR_ABGR;
    }
    //---------------------------------------------------------------------
    void D3D11RenderSystem::_convertProjectionMatrix(const Matrix4& matrix,
        Matrix4& dest, bool forGpuProgram)
    {
        dest = matrix;

        // Convert depth range from [-1,+1] to [0,1]
        dest[2][0] = (dest[2][0] + dest[3][0]) / 2;
        dest[2][1] = (dest[2][1] + dest[3][1]) / 2;
        dest[2][2] = (dest[2][2] + dest[3][2]) / 2;
        dest[2][3] = (dest[2][3] + dest[3][3]) / 2;

        if (!forGpuProgram)
        {
            // Convert right-handed to left-handed
            dest[0][2] = -dest[0][2];
            dest[1][2] = -dest[1][2];
            dest[2][2] = -dest[2][2];
            dest[3][2] = -dest[3][2];
        }
    }
    //---------------------------------------------------------------------
    void D3D11RenderSystem::_makeProjectionMatrix(const Radian& fovy, Real aspect, Real nearPlane, 
        Real farPlane, Matrix4& dest, bool forGpuProgram)
    {
        Radian theta ( fovy * 0.5 );
        Real h = 1 / Math::Tan(theta);
        Real w = h / aspect;
        Real q, qn;
        if (farPlane == 0)
        {
            q = 1 - Frustum::INFINITE_FAR_PLANE_ADJUST;
            qn = nearPlane * (Frustum::INFINITE_FAR_PLANE_ADJUST - 1);
        }
        else
        {
            q = farPlane / ( farPlane - nearPlane );
            qn = -q * nearPlane;
        }

        dest = Matrix4::ZERO;
        dest[0][0] = w;
        dest[1][1] = h;

        if (forGpuProgram)
        {
            dest[2][2] = -q;
            dest[3][2] = -1.0f;
        }
        else
        {
            dest[2][2] = q;
            dest[3][2] = 1.0f;
        }

        dest[2][3] = qn;
    }
    //---------------------------------------------------------------------
    void D3D11RenderSystem::_makeOrthoMatrix(const Radian& fovy, Real aspect, Real nearPlane, Real farPlane, 
        Matrix4& dest, bool forGpuProgram )
    {
        Radian thetaY (fovy / 2.0f);
        Real tanThetaY = Math::Tan(thetaY);

        //Real thetaX = thetaY * aspect;
        Real tanThetaX = tanThetaY * aspect; //Math::Tan(thetaX);
        Real half_w = tanThetaX * nearPlane;
        Real half_h = tanThetaY * nearPlane;
        Real iw = 1.0f / half_w;
        Real ih = 1.0f / half_h;
        Real q;
        if (farPlane == 0)
        {
            q = 0;
        }
        else
        {
            q = 1.0f / (farPlane - nearPlane);
        }

        dest = Matrix4::ZERO;
        dest[0][0] = iw;
        dest[1][1] = ih;
        dest[2][2] = q;
        dest[2][3] = -nearPlane / (farPlane - nearPlane);
        dest[3][3] = 1;

        if (forGpuProgram)
        {
            dest[2][2] = -dest[2][2];
        }
    }
    //---------------------------------------------------------------------
    void D3D11RenderSystem::_setTexture( size_t stage, bool enabled, const TexturePtr& tex )
    {
        static D3D11TexturePtr dt;
        dt = static_pointer_cast<D3D11Texture>(tex);
        if (enabled && dt && dt->getSize() > 0)
        {
            // note used
            dt->touch();
            ID3D11ShaderResourceView * pTex = dt->getTexture();
            mTexStageDesc[stage].pTex = pTex;
            mTexStageDesc[stage].used = true;
            mTexStageDesc[stage].type = dt->getTextureType();

            mLastTextureUnitState = stage+1;
        }
        else
        {
            mTexStageDesc[stage].used = false;
            // now we now what's the last texture unit set
			mLastTextureUnitState = std::min(mLastTextureUnitState,stage);
        }
        mSamplerStatesChanged = true;
    }
    //---------------------------------------------------------------------
    void D3D11RenderSystem::_setBindingType(TextureUnitState::BindingType bindingType)
    {
        mBindingType = bindingType;
    }
    //---------------------------------------------------------------------
    void D3D11RenderSystem::_setVertexTexture(size_t stage, const TexturePtr& tex)
    {
        if (!tex)
            _setTexture(stage, false, tex);
        else
            _setTexture(stage, true, tex);  
    }
    //---------------------------------------------------------------------
    void D3D11RenderSystem::_setGeometryTexture(size_t stage, const TexturePtr& tex)
    {
        if (!tex)
            _setTexture(stage, false, tex);
        else
            _setTexture(stage, true, tex);  
    }
    //---------------------------------------------------------------------
    void D3D11RenderSystem::_setComputeTexture(size_t stage, const TexturePtr& tex)
    {
        if (!tex)
            _setTexture(stage, false, tex);
        else
            _setTexture(stage, true, tex);  
    }
    //---------------------------------------------------------------------
    void D3D11RenderSystem::_setTesselationHullTexture(size_t stage, const TexturePtr& tex)
    {
        if (!tex)
            _setTexture(stage, false, tex);
        else
            _setTexture(stage, true, tex);  
    }
    //---------------------------------------------------------------------
    void D3D11RenderSystem::_setTesselationDomainTexture(size_t stage, const TexturePtr& tex)
    {
        if (!tex)
            _setTexture(stage, false, tex);
        else
            _setTexture(stage, true, tex);  
    }
    //---------------------------------------------------------------------
    void D3D11RenderSystem::_disableTextureUnit(size_t texUnit)
    {
        RenderSystem::_disableTextureUnit(texUnit);
        // also disable vertex texture unit
        static TexturePtr nullPtr;
        _setVertexTexture(texUnit, nullPtr);
    }
    //---------------------------------------------------------------------
    void D3D11RenderSystem::_setTextureCoordSet( size_t stage, size_t index )
    {
        mTexStageDesc[stage].coordIndex = index;
    }
    //---------------------------------------------------------------------
    void D3D11RenderSystem::_setTextureMipmapBias(size_t unit, float bias)
    {
        mTexStageDesc[unit].samplerDesc.MipLODBias = bias;
        mSamplerStatesChanged = true;
    }
    //---------------------------------------------------------------------
    void D3D11RenderSystem::_setTextureAddressingMode( size_t stage, 
        const TextureUnitState::UVWAddressingMode& uvw )
    {
        // record the stage state
        mTexStageDesc[stage].samplerDesc.AddressU = D3D11Mappings::get(uvw.u);
        mTexStageDesc[stage].samplerDesc.AddressV = D3D11Mappings::get(uvw.v);
        mTexStageDesc[stage].samplerDesc.AddressW = D3D11Mappings::get(uvw.w);
        mSamplerStatesChanged = true;
    }
    //-----------------------------------------------------------------------------
    void D3D11RenderSystem::_setTextureBorderColour(size_t stage,
        const ColourValue& colour)
    {
        D3D11Mappings::get(colour, mTexStageDesc[stage].samplerDesc.BorderColor);
        mSamplerStatesChanged = true;
    }
    //---------------------------------------------------------------------
    void D3D11RenderSystem::_setSceneBlending( SceneBlendFactor sourceFactor, SceneBlendFactor destFactor, SceneBlendOperation op /*= SBO_ADD*/ )
    {
        if( sourceFactor == SBF_ONE && destFactor == SBF_ZERO)
        {
            mBlendDesc.RenderTarget[0].BlendEnable = FALSE;
        }
        else
        {
            mBlendDesc.RenderTarget[0].BlendEnable = TRUE;
            mBlendDesc.RenderTarget[0].SrcBlend = D3D11Mappings::get(sourceFactor, false);
            mBlendDesc.RenderTarget[0].DestBlend = D3D11Mappings::get(destFactor, false);
            mBlendDesc.RenderTarget[0].SrcBlendAlpha = D3D11Mappings::get(sourceFactor, true);
            mBlendDesc.RenderTarget[0].DestBlendAlpha = D3D11Mappings::get(destFactor, true);
            mBlendDesc.RenderTarget[0].BlendOp = mBlendDesc.RenderTarget[0].BlendOpAlpha = D3D11Mappings::get(op);
            
            // feature level 9 and below does not support alpha to coverage.
            if (mFeatureLevel < D3D_FEATURE_LEVEL_10_0)
                mBlendDesc.AlphaToCoverageEnable = false;
            else
                mBlendDesc.AlphaToCoverageEnable = mSceneAlphaToCoverage;

            mBlendDesc.RenderTarget[0].RenderTargetWriteMask = 0x0F;
        }
        mBlendDescChanged = true;
    }
    //---------------------------------------------------------------------
    void D3D11RenderSystem::_setSeparateSceneBlending( SceneBlendFactor sourceFactor, SceneBlendFactor destFactor, SceneBlendFactor sourceFactorAlpha, SceneBlendFactor destFactorAlpha, SceneBlendOperation op /*= SBO_ADD*/, SceneBlendOperation alphaOp /*= SBO_ADD*/ )
    {
        if( sourceFactor == SBF_ONE && destFactor == SBF_ZERO)
        {
            mBlendDesc.RenderTarget[0].BlendEnable = FALSE;
        }
        else
        {
            mBlendDesc.RenderTarget[0].BlendEnable = TRUE;
            mBlendDesc.RenderTarget[0].SrcBlend = D3D11Mappings::get(sourceFactor, false);
            mBlendDesc.RenderTarget[0].DestBlend = D3D11Mappings::get(destFactor, false);
            mBlendDesc.RenderTarget[0].BlendOp = D3D11Mappings::get(op) ;
            mBlendDesc.RenderTarget[0].SrcBlendAlpha = D3D11Mappings::get(sourceFactorAlpha, true);
            mBlendDesc.RenderTarget[0].DestBlendAlpha = D3D11Mappings::get(destFactorAlpha, true);
            mBlendDesc.RenderTarget[0].BlendOpAlpha = D3D11Mappings::get(alphaOp) ;
            mBlendDesc.AlphaToCoverageEnable = false;

            mBlendDesc.RenderTarget[0].RenderTargetWriteMask = 0x0F;
        }
        mBlendDescChanged = true;
    }
    //---------------------------------------------------------------------
    void D3D11RenderSystem::_setAlphaRejectSettings( CompareFunction func, unsigned char value, bool alphaToCoverage )
    {
        mSceneAlphaRejectFunc   = func;
        mSceneAlphaRejectValue  = value;
        mSceneAlphaToCoverage   = alphaToCoverage;
        mBlendDesc.AlphaToCoverageEnable = alphaToCoverage;
        mBlendDescChanged = true;
    }
    //---------------------------------------------------------------------
    void D3D11RenderSystem::_setCullingMode( CullingMode mode )
    {
        mCullingMode = mode;

		bool flip = (mInvertVertexWinding && !mActiveRenderTarget->requiresTextureFlipping() ||
					!mInvertVertexWinding && mActiveRenderTarget->requiresTextureFlipping());

		mRasterizerDesc.CullMode = D3D11Mappings::get(mode, flip);
        mRasterizerDescChanged = true;
    }
    //---------------------------------------------------------------------
    void D3D11RenderSystem::_setDepthBufferParams( bool depthTest, bool depthWrite, CompareFunction depthFunction )
    {
        _setDepthBufferCheckEnabled( depthTest );
        _setDepthBufferWriteEnabled( depthWrite );
        _setDepthBufferFunction( depthFunction );
    }
    //---------------------------------------------------------------------
    void D3D11RenderSystem::_setDepthBufferCheckEnabled( bool enabled )
    {
        mDepthStencilDesc.DepthEnable = enabled;
        mDepthStencilDescChanged = true;
    }
    //---------------------------------------------------------------------
    void D3D11RenderSystem::_setDepthBufferWriteEnabled( bool enabled )
    {
        if (enabled)
        {
            mDepthStencilDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
        }
        else
        {
            mDepthStencilDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
        }
        mDepthStencilDescChanged = true;
    }
    //---------------------------------------------------------------------
    void D3D11RenderSystem::_setDepthBufferFunction( CompareFunction func )
    {
        mDepthStencilDesc.DepthFunc = D3D11Mappings::get(func);
        mDepthStencilDescChanged = true;
    }
    //---------------------------------------------------------------------
    void D3D11RenderSystem::_setDepthBias(float constantBias, float slopeScaleBias)
    {
		const float nearFarFactor = 10.0; 
		mRasterizerDesc.DepthBias = static_cast<int>(-constantBias * nearFarFactor);
		mRasterizerDesc.SlopeScaledDepthBias = -slopeScaleBias;
        mRasterizerDescChanged = true;
    }
    //---------------------------------------------------------------------
    void D3D11RenderSystem::_setColourBufferWriteEnabled(bool red, bool green, 
        bool blue, bool alpha)
    {
        UINT8 val = 0;
        if (red) 
            val |= D3D11_COLOR_WRITE_ENABLE_RED;
        if (green)
            val |= D3D11_COLOR_WRITE_ENABLE_GREEN;
        if (blue)
            val |= D3D11_COLOR_WRITE_ENABLE_BLUE;
        if (alpha)
            val |= D3D11_COLOR_WRITE_ENABLE_ALPHA;

        mBlendDesc.RenderTarget[0].RenderTargetWriteMask = val; 
        mBlendDescChanged = true;
    }
    //---------------------------------------------------------------------
    void D3D11RenderSystem::_setPolygonMode(PolygonMode level)
    {
        if(mPolygonMode != level)
        {
            mPolygonMode = level;
            mRasterizerDesc.FillMode = D3D11Mappings::get(mPolygonMode);
            mRasterizerDescChanged = true;
        }
    }
    //---------------------------------------------------------------------
    void D3D11RenderSystem::setStencilCheckEnabled(bool enabled)
    {
        mDepthStencilDesc.StencilEnable = enabled;
        mDepthStencilDescChanged = true;
    }
    //---------------------------------------------------------------------
    void D3D11RenderSystem::setStencilBufferParams(CompareFunction func, 
        uint32 refValue, uint32 compareMask, uint32 writeMask, StencilOperation stencilFailOp, 
        StencilOperation depthFailOp, StencilOperation passOp, 
        bool twoSidedOperation, bool readBackAsTexture)
    {
		// We honor user intent in case of one sided operation, and carefully tweak it in case of two sided operations.
		bool flipFront = twoSidedOperation &&
						(mInvertVertexWinding && !mActiveRenderTarget->requiresTextureFlipping() ||
						!mInvertVertexWinding && mActiveRenderTarget->requiresTextureFlipping());
		bool flipBack = twoSidedOperation && !flipFront;

        mStencilRef = refValue;
        mDepthStencilDesc.StencilReadMask = compareMask;
        mDepthStencilDesc.StencilWriteMask = writeMask;

		mDepthStencilDesc.FrontFace.StencilFailOp = D3D11Mappings::get(stencilFailOp, flipFront);
		mDepthStencilDesc.BackFace.StencilFailOp = D3D11Mappings::get(stencilFailOp, flipBack);
        
		mDepthStencilDesc.FrontFace.StencilDepthFailOp = D3D11Mappings::get(depthFailOp, flipFront);
		mDepthStencilDesc.BackFace.StencilDepthFailOp = D3D11Mappings::get(depthFailOp, flipBack);
        
		mDepthStencilDesc.FrontFace.StencilPassOp = D3D11Mappings::get(passOp, flipFront);
		mDepthStencilDesc.BackFace.StencilPassOp = D3D11Mappings::get(passOp, flipBack);

		mDepthStencilDesc.FrontFace.StencilFunc = D3D11Mappings::get(func);
		mDepthStencilDesc.BackFace.StencilFunc = D3D11Mappings::get(func);
        mReadBackAsTexture = readBackAsTexture;
        mDepthStencilDescChanged = true;
    }
    //---------------------------------------------------------------------
    void D3D11RenderSystem::_setTextureUnitFiltering(size_t unit, FilterType ftype, 
        FilterOptions filter)
    {
        switch(ftype) {
        case FT_MIN:
            FilterMinification[unit] = filter;
            break;
        case FT_MAG:
            FilterMagnification[unit] = filter;
            break;
        case FT_MIP:
            FilterMips[unit] = filter;
            break;
        }

        mTexStageDesc[unit].samplerDesc.Filter = D3D11Mappings::get(FilterMinification[unit], FilterMagnification[unit], FilterMips[unit],CompareEnabled);
        mSamplerStatesChanged = true;
    }
    //---------------------------------------------------------------------
    void D3D11RenderSystem::_setTextureUnitCompareEnabled(size_t unit, bool compare)
    {
        CompareEnabled = compare;
        mSamplerStatesChanged = true;
    }
    //---------------------------------------------------------------------
    void D3D11RenderSystem::_setTextureUnitCompareFunction(size_t unit, CompareFunction function)
    {
        mTexStageDesc[unit].samplerDesc.ComparisonFunc = D3D11Mappings::get(function);
        mSamplerStatesChanged = true;
    }
    //---------------------------------------------------------------------
    DWORD D3D11RenderSystem::_getCurrentAnisotropy(size_t unit)
    {
        return mTexStageDesc[unit].samplerDesc.MaxAnisotropy;;
    }
    //---------------------------------------------------------------------
    void D3D11RenderSystem::_setTextureLayerAnisotropy(size_t unit, unsigned int maxAnisotropy)
    {
        mTexStageDesc[unit].samplerDesc.MaxAnisotropy = maxAnisotropy;
        mSamplerStatesChanged = true;
    }
    //---------------------------------------------------------------------
    void D3D11RenderSystem::_setRenderTarget(RenderTarget *target)
    {
        mActiveRenderTarget = target;
        if (mActiveRenderTarget)
        {
            // we need to clear the state 
            mDevice.GetImmediateContext()->ClearState();

            if (mDevice.isError())
            {
                String errorDescription = mDevice.getErrorDescription();
                OGRE_EXCEPT(Exception::ERR_RENDERINGAPI_ERROR, 
                    "D3D11 device cannot Clear State\nError Description:" + errorDescription,
                    "D3D11RenderSystem::_setRenderTarget");
            }

            _setRenderTargetViews();
        }
    }

    //---------------------------------------------------------------------
    void D3D11RenderSystem::_setRenderTargetViews()
    {
        RenderTarget *target = mActiveRenderTarget;

        if (target)
        {
            ID3D11RenderTargetView * pRTView[OGRE_MAX_MULTIPLE_RENDER_TARGETS];
            memset(pRTView, 0, sizeof(pRTView));

            target->getCustomAttribute( "ID3D11RenderTargetView", &pRTView );

            uint numberOfViews;
            target->getCustomAttribute( "numberOfViews", &numberOfViews );

            //Retrieve depth buffer
            D3D11DepthBuffer *depthBuffer = static_cast<D3D11DepthBuffer*>(target->getDepthBuffer());

            if( target->getDepthBufferPool() != DepthBuffer::POOL_NO_DEPTH && !depthBuffer )
            {
                //Depth is automatically managed and there is no depth buffer attached to this RT
                //or the Current D3D device doesn't match the one this Depth buffer was created
                setDepthBufferFor( target );
            }

            //Retrieve depth buffer again (it may have changed)
            depthBuffer = static_cast<D3D11DepthBuffer*>(target->getDepthBuffer());

            // now switch to the new render target
            mDevice.GetImmediateContext()->OMSetRenderTargets(
                numberOfViews,
                pRTView,
                depthBuffer ? depthBuffer->getDepthStencilView() : 0 );

            if (mDevice.isError())
            {
                String errorDescription = mDevice.getErrorDescription();
                OGRE_EXCEPT(Exception::ERR_RENDERINGAPI_ERROR, 
                    "D3D11 device cannot set render target\nError Description:" + errorDescription,
                    "D3D11RenderSystem::_setRenderTargetViews");
            }
        }
    }
    //---------------------------------------------------------------------
    void D3D11RenderSystem::_setViewport( Viewport *vp )
    {
        if (!vp)
        {
            mActiveViewport = NULL;
            _setRenderTarget(NULL);
        }
        else if( vp != mActiveViewport || vp->_isUpdated() )
        {
            mActiveViewport = vp;

            // ok, it's different, time to set render target and viewport params
            D3D11_VIEWPORT d3dvp;

            // Set render target
            RenderTarget* target;
            target = vp->getTarget();

            _setRenderTarget(target);
            _setCullingMode( mCullingMode );

            // set viewport dimensions
            d3dvp.TopLeftX = static_cast<FLOAT>(vp->getActualLeft());
            d3dvp.TopLeftY = static_cast<FLOAT>(vp->getActualTop());
            d3dvp.Width = static_cast<FLOAT>(vp->getActualWidth());
            d3dvp.Height = static_cast<FLOAT>(vp->getActualHeight());
            if (target->requiresTextureFlipping())
            {
                // Convert "top-left" to "bottom-left"
                d3dvp.TopLeftY = target->getHeight() - d3dvp.Height - d3dvp.TopLeftY;
            }

            // Z-values from 0.0 to 1.0 (TODO: standardise with OpenGL)
            d3dvp.MinDepth = 0.0f;
            d3dvp.MaxDepth = 1.0f;

            mDevice.GetImmediateContext()->RSSetViewports(1, &d3dvp);
            if (mDevice.isError())
            {
                String errorDescription = mDevice.getErrorDescription();
                OGRE_EXCEPT(Exception::ERR_RENDERINGAPI_ERROR, 
                    "D3D11 device cannot set viewports\nError Description:" + errorDescription,
                    "D3D11RenderSystem::_setViewport");
            }

#if OGRE_NO_QUAD_BUFFER_STEREO == 0
			D3D11RenderWindowBase* d3d11Window = dynamic_cast<D3D11RenderWindowBase*>(target);
			if(d3d11Window)
				d3d11Window->_validateStereo();
#endif

            vp->_clearUpdatedFlag();
        }
        else
        {
            // if swapchain was created with DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL we need to reestablish render target views
            D3D11RenderWindowBase* d3d11Window = dynamic_cast<D3D11RenderWindowBase*>(vp->getTarget());
            if(d3d11Window && d3d11Window->_shouldRebindBackBuffer())
                _setRenderTargetViews();
        }
    }
    //---------------------------------------------------------------------
    void D3D11RenderSystem::_beginFrame()
    {
    
        if( !mActiveViewport )
            OGRE_EXCEPT( Exception::ERR_INTERNAL_ERROR, "Cannot begin frame - no viewport selected.", "D3D11RenderSystem::_beginFrame" );
    }
    //---------------------------------------------------------------------
    void D3D11RenderSystem::_endFrame()
    {
    }
    //---------------------------------------------------------------------
    void D3D11RenderSystem::setVertexDeclaration(VertexDeclaration* decl)
    {
            OGRE_EXCEPT( Exception::ERR_INTERNAL_ERROR, 
                    "Cannot directly call setVertexDeclaration in the d3d11 render system - cast then use 'setVertexDeclaration(VertexDeclaration* decl, VertexBufferBinding* binding)' .", 
                    "D3D11RenderSystem::setVertexDeclaration" );
    }
    //---------------------------------------------------------------------
    void D3D11RenderSystem::setVertexDeclaration(VertexDeclaration* decl, VertexBufferBinding* binding)
    {
        D3D11VertexDeclaration* d3ddecl = 
            static_cast<D3D11VertexDeclaration*>(decl);

        d3ddecl->bindToShader(mBoundVertexProgram, binding);
    }
    //---------------------------------------------------------------------
    void D3D11RenderSystem::setVertexBufferBinding(VertexBufferBinding* binding)
    {
        // TODO: attempt to detect duplicates
        const VertexBufferBinding::VertexBufferBindingMap& binds = binding->getBindings();
        VertexBufferBinding::VertexBufferBindingMap::const_iterator i, iend;
        iend = binds.end();
        for (i = binds.begin(); i != iend; ++i)
        {
            const D3D11HardwareVertexBuffer* d3d11buf = 
                static_cast<const D3D11HardwareVertexBuffer*>(i->second.get());

            UINT stride = static_cast<UINT>(d3d11buf->getVertexSize());
            UINT offset = 0; // no stream offset, this is handled in _render instead
            UINT slot = static_cast<UINT>(i->first);
            ID3D11Buffer * pVertexBuffers = d3d11buf->getD3DVertexBuffer();
            mDevice.GetImmediateContext()->IASetVertexBuffers(
                slot, // The first input slot for binding.
                1, // The number of vertex buffers in the array.
                &pVertexBuffers,
                &stride,
                &offset 
                );

            if (mDevice.isError())
            {
                String errorDescription = mDevice.getErrorDescription();
                OGRE_EXCEPT(Exception::ERR_RENDERINGAPI_ERROR, 
                    "D3D11 device cannot set vertex buffers\nError Description:" + errorDescription,
                    "D3D11RenderSystem::setVertexBufferBinding");
            }
        }

        mLastVertexSourceCount = binds.size();      
    }

    //---------------------------------------------------------------------
    // TODO: Move this class to the right place.
    class D3D11RenderOperationState : public Renderable::RenderSystemData
    {
    public:
        ComPtr<ID3D11BlendState> mBlendState;
        ComPtr<ID3D11RasterizerState> mRasterizer;
        ComPtr<ID3D11DepthStencilState> mDepthStencilState;

        ComPtr<ID3D11SamplerState> mSamplerStates[OGRE_MAX_TEXTURE_LAYERS];
        size_t mSamplerStatesCount;

        ID3D11ShaderResourceView * mTextures[OGRE_MAX_TEXTURE_LAYERS]; // note - not owning
        size_t mTexturesCount;

        D3D11RenderOperationState() : mSamplerStatesCount(0), mTexturesCount(0) {}
        ~D3D11RenderOperationState() {}
    };

    //---------------------------------------------------------------------
    void D3D11RenderSystem::_render(const RenderOperation& op)
    {

        // Exit immediately if there is nothing to render
        if (op.vertexData==0 || op.vertexData->vertexCount == 0)
        {
            return;
        }

        HardwareVertexBufferSharedPtr globalInstanceVertexBuffer = getGlobalInstanceVertexBuffer();
        VertexDeclaration* globalVertexDeclaration = getGlobalInstanceVertexBufferVertexDeclaration();

        bool hasInstanceData = op.useGlobalInstancingVertexBufferIsAvailable &&
                    globalInstanceVertexBuffer && globalVertexDeclaration != NULL 
                || op.vertexData->vertexBufferBinding->getHasInstanceData();

        size_t numberOfInstances = op.numberOfInstances;

        if (op.useGlobalInstancingVertexBufferIsAvailable)
        {
            numberOfInstances *= getGlobalNumberOfInstances();
        }

        // Call super class
        RenderSystem::_render(op);
        
        D3D11RenderOperationState stackOpState;
        D3D11RenderOperationState * opState = &stackOpState;

        if(mBlendDescChanged)
        {
            mBlendDescChanged = false;
            mBoundBlendState = 0;

            HRESULT hr = mDevice->CreateBlendState(&mBlendDesc, opState->mBlendState.ReleaseAndGetAddressOf()) ;
            if (FAILED(hr))
            {
				String errorDescription = mDevice.getErrorDescription(hr);
				OGRE_EXCEPT_EX(Exception::ERR_RENDERINGAPI_ERROR, hr,
                    "Failed to create blend state\nError Description:" + errorDescription, 
                    "D3D11RenderSystem::_render" );
            }
        }
        else
        {
            opState->mBlendState = mBoundBlendState;
        }

        if(mRasterizerDescChanged)
		{
			mRasterizerDescChanged=false;
			mBoundRasterizer = 0;

            HRESULT hr = mDevice->CreateRasterizerState(&mRasterizerDesc, opState->mRasterizer.ReleaseAndGetAddressOf()) ;
            if (FAILED(hr))
            {
				String errorDescription = mDevice.getErrorDescription(hr);
				OGRE_EXCEPT_EX(Exception::ERR_RENDERINGAPI_ERROR, hr,
                    "Failed to create rasterizer state\nError Description:" + errorDescription, 
                    "D3D11RenderSystem::_render" );
            }
        }
        else
        {
            opState->mRasterizer = mBoundRasterizer;
        }

        if(mDepthStencilDescChanged)
		{
			mBoundDepthStencilState = 0;
			mDepthStencilDescChanged=false;

            HRESULT hr = mDevice->CreateDepthStencilState(&mDepthStencilDesc, opState->mDepthStencilState.ReleaseAndGetAddressOf()) ;
            if (FAILED(hr))
            {
				String errorDescription = mDevice.getErrorDescription(hr);
				OGRE_EXCEPT_EX(Exception::ERR_RENDERINGAPI_ERROR, hr,
                    "Failed to create depth stencil state\nError Description:" + errorDescription, 
                    "D3D11RenderSystem::_render" );
            }
        }
        else
		{
			opState->mDepthStencilState = mBoundDepthStencilState;
		}

        if(mSamplerStatesChanged)
		{
            // samplers mapping
            size_t numberOfSamplers = std::min(mLastTextureUnitState,(size_t)(OGRE_MAX_TEXTURE_LAYERS + 1));
            
            opState->mSamplerStatesCount = numberOfSamplers;
            opState->mTexturesCount = numberOfSamplers;
                            
            for (size_t n = 0; n < numberOfSamplers; n++)
            {
                ComPtr<ID3D11SamplerState> samplerState;
                ID3D11ShaderResourceView *texture = NULL;
                sD3DTextureStageDesc & stage = mTexStageDesc[n];
                if(stage.used)
                {
                    texture = stage.pTex;

                    stage.samplerDesc.Filter = D3D11Mappings::get(FilterMinification[n], FilterMagnification[n], FilterMips[n], false);
                    stage.samplerDesc.ComparisonFunc = D3D11Mappings::get(mSceneAlphaRejectFunc);
                    stage.samplerDesc.MipLODBias = static_cast<float>(Math::Clamp(stage.samplerDesc.MipLODBias - 0.5, -16.00, 15.99));
                    stage.samplerDesc.MinLOD = -D3D11_FLOAT32_MAX;
                    stage.samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;

                    HRESULT hr = mDevice->CreateSamplerState(&stage.samplerDesc, samplerState.ReleaseAndGetAddressOf());
                    if (FAILED(hr))
                    {
                        String errorDescription = mDevice.getErrorDescription(hr);
                        OGRE_EXCEPT_EX(Exception::ERR_RENDERINGAPI_ERROR, hr,
                            "Failed to create sampler state\nError Description:" + errorDescription,
                            "D3D11RenderSystem::_render" );
                    }
                
                }
                opState->mSamplerStates[n].Swap(samplerState);
                opState->mTextures[n]       = texture;
            }
            for (size_t n = opState->mTexturesCount; n < OGRE_MAX_TEXTURE_LAYERS; n++)
			{
				opState->mTextures[n] = NULL;
			}
        }

        if (opState->mBlendState != mBoundBlendState)
        {
            mBoundBlendState = opState->mBlendState ;
            mDevice.GetImmediateContext()->OMSetBlendState(opState->mBlendState.Get(), 0, 0xffffffff); // TODO - find out where to get the parameters
            if (mDevice.isError())
            {
                String errorDescription = mDevice.getErrorDescription();
                OGRE_EXCEPT(Exception::ERR_RENDERINGAPI_ERROR, 
                    "D3D11 device cannot set blend state\nError Description:" + errorDescription,
                    "D3D11RenderSystem::_render");
            }
            if (mSamplerStatesChanged && mBoundGeometryProgram && mBindingType == TextureUnitState::BT_GEOMETRY)
            {
                {
                    mDevice.GetImmediateContext()->GSSetSamplers(static_cast<UINT>(0), static_cast<UINT>(opState->mSamplerStatesCount), opState->mSamplerStates[0].GetAddressOf());
                    if (mDevice.isError())
                    {
                        String errorDescription = mDevice.getErrorDescription();
                        OGRE_EXCEPT(Exception::ERR_RENDERINGAPI_ERROR, 
                            "D3D11 device cannot set pixel shader samplers\nError Description:" + errorDescription,
                            "D3D11RenderSystem::_render");
                    }
                }
                mDevice.GetImmediateContext()->GSSetShaderResources(static_cast<UINT>(0), static_cast<UINT>(opState->mTexturesCount), &opState->mTextures[0]);
                if (mDevice.isError())
                {
                    String errorDescription = mDevice.getErrorDescription();
                    OGRE_EXCEPT(Exception::ERR_RENDERINGAPI_ERROR, 
                        "D3D11 device cannot set pixel shader resources\nError Description:" + errorDescription,
                        "D3D11RenderSystem::_render");
                }
            }
        }

        if (opState->mRasterizer != mBoundRasterizer)
        {
            mBoundRasterizer = opState->mRasterizer ;

            mDevice.GetImmediateContext()->RSSetState(opState->mRasterizer.Get());
            if (mDevice.isError())
            {
                String errorDescription = mDevice.getErrorDescription();
                OGRE_EXCEPT(Exception::ERR_RENDERINGAPI_ERROR, 
                    "D3D11 device cannot set rasterizer state\nError Description:" + errorDescription,
                    "D3D11RenderSystem::_render");
            }
        }
        

        if (opState->mDepthStencilState != mBoundDepthStencilState)
        {
            mBoundDepthStencilState = opState->mDepthStencilState ;

            mDevice.GetImmediateContext()->OMSetDepthStencilState(opState->mDepthStencilState.Get(), mStencilRef);
            if (mDevice.isError())
            {
                String errorDescription = mDevice.getErrorDescription();
                OGRE_EXCEPT(Exception::ERR_RENDERINGAPI_ERROR, 
                    "D3D11 device cannot set depth stencil state\nError Description:" + errorDescription,
                    "D3D11RenderSystem::_render");
            }
        }

        if (mSamplerStatesChanged && opState->mSamplerStatesCount > 0 ) //  if the NumSamplers is 0, the operation effectively does nothing.
        {
            mSamplerStatesChanged = false; // now it's time to set it to false
            /// Pixel Shader binding
            {
                {
                    mDevice.GetImmediateContext()->PSSetSamplers(static_cast<UINT>(0), static_cast<UINT>(opState->mSamplerStatesCount), opState->mSamplerStates[0].GetAddressOf());
                    if (mDevice.isError())
                    {
                        String errorDescription = mDevice.getErrorDescription();
                        OGRE_EXCEPT(Exception::ERR_RENDERINGAPI_ERROR, 
                            "D3D11 device cannot set pixel shader samplers\nError Description:" + errorDescription,
                            "D3D11RenderSystem::_render");
                    }
                }

                mDevice.GetImmediateContext()->PSSetShaderResources(static_cast<UINT>(0), static_cast<UINT>(opState->mTexturesCount), &opState->mTextures[0]);
                if (mDevice.isError())
                {
                    String errorDescription = mDevice.getErrorDescription();
                    OGRE_EXCEPT(Exception::ERR_RENDERINGAPI_ERROR, 
                        "D3D11 device cannot set pixel shader resources\nError Description:" + errorDescription,
                        "D3D11RenderSystem::_render");
                }
            }
            
            /// Vertex Shader binding
            /*if (mBindingType == TextureUnitState::BindingType::BT_VERTEX)*/
            {
                if (mFeatureLevel >= D3D_FEATURE_LEVEL_10_0)
                {
                    mDevice.GetImmediateContext()->VSSetSamplers(static_cast<UINT>(0), static_cast<UINT>(opState->mSamplerStatesCount), opState->mSamplerStates[0].GetAddressOf());
                    if (mDevice.isError())
                    {
                        String errorDescription = mDevice.getErrorDescription();
                        OGRE_EXCEPT(Exception::ERR_RENDERINGAPI_ERROR, 
                            "D3D11 device cannot set pixel shader samplers\nError Description:" + errorDescription,
                            "D3D11RenderSystem::_render");
                    }
                }

                if (mFeatureLevel >= D3D_FEATURE_LEVEL_10_0)
                {
                    mDevice.GetImmediateContext()->VSSetShaderResources(static_cast<UINT>(0), static_cast<UINT>(opState->mTexturesCount), &opState->mTextures[0]);
                    if (mDevice.isError())
                    {
                        String errorDescription = mDevice.getErrorDescription();
                        OGRE_EXCEPT(Exception::ERR_RENDERINGAPI_ERROR, 
                            "D3D11 device cannot set pixel shader resources\nError Description:" + errorDescription,
                            "D3D11RenderSystem::_render");
                    }
                }
            }

            /// Compute Shader binding
            if (mBoundComputeProgram && mBindingType == TextureUnitState::BT_COMPUTE)
            {
                if (mFeatureLevel >= D3D_FEATURE_LEVEL_10_0)
                {
                    mDevice.GetImmediateContext()->CSSetSamplers(static_cast<UINT>(0), static_cast<UINT>(opState->mSamplerStatesCount), opState->mSamplerStates[0].GetAddressOf());
                    if (mDevice.isError())
                    {
                        String errorDescription = mDevice.getErrorDescription();
                        OGRE_EXCEPT(Exception::ERR_RENDERINGAPI_ERROR, 
                            "D3D11 device cannot set compute shader samplers\nError Description:" + errorDescription,
                            "D3D11RenderSystem::_render");
                    }
                }
                
                if (mFeatureLevel >= D3D_FEATURE_LEVEL_10_0)
                {
                    mDevice.GetImmediateContext()->CSSetShaderResources(static_cast<UINT>(0), static_cast<UINT>(opState->mTexturesCount), &opState->mTextures[0]);
                    if (mDevice.isError())
                    {
                        String errorDescription = mDevice.getErrorDescription();
                        OGRE_EXCEPT(Exception::ERR_RENDERINGAPI_ERROR, 
                            "D3D11 device cannot set compute shader resources\nError Description:" + errorDescription,
                            "D3D11RenderSystem::_render");
                    }
                }
            }

            /// Hull Shader binding
            if (mBoundTessellationHullProgram && mBindingType == TextureUnitState::BT_TESSELLATION_HULL)
            {
                if (mFeatureLevel >= D3D_FEATURE_LEVEL_10_0)
                {
                    mDevice.GetImmediateContext()->HSSetSamplers(static_cast<UINT>(0), static_cast<UINT>(opState->mSamplerStatesCount), opState->mSamplerStates[0].GetAddressOf());
                    if (mDevice.isError())
                    {
                        String errorDescription = mDevice.getErrorDescription();
                        OGRE_EXCEPT(Exception::ERR_RENDERINGAPI_ERROR, 
                            "D3D11 device cannot set hull shader samplers\nError Description:" + errorDescription,
                            "D3D11RenderSystem::_render");
                    }
                }
                
                if (mFeatureLevel >= D3D_FEATURE_LEVEL_10_0)
                {
                    mDevice.GetImmediateContext()->HSSetShaderResources(static_cast<UINT>(0), static_cast<UINT>(opState->mTexturesCount), &opState->mTextures[0]);
                    if (mDevice.isError())
                    {
                        String errorDescription = mDevice.getErrorDescription();
                        OGRE_EXCEPT(Exception::ERR_RENDERINGAPI_ERROR, 
                            "D3D11 device cannot set hull shader resources\nError Description:" + errorDescription,
                            "D3D11RenderSystem::_render");
                    }
                }
            }
            
            /// Domain Shader binding
            if (mBoundTessellationDomainProgram && mBindingType == TextureUnitState::BT_TESSELLATION_DOMAIN)
            {
                if (mFeatureLevel >= D3D_FEATURE_LEVEL_10_0)
                {
                    mDevice.GetImmediateContext()->DSSetSamplers(static_cast<UINT>(0), static_cast<UINT>(opState->mSamplerStatesCount), opState->mSamplerStates[0].GetAddressOf());
                    if (mDevice.isError())
                    {
                        String errorDescription = mDevice.getErrorDescription();
                        OGRE_EXCEPT(Exception::ERR_RENDERINGAPI_ERROR, 
                            "D3D11 device cannot set domain shader samplers\nError Description:" + errorDescription,
                            "D3D11RenderSystem::_render");
                    }
                }

                if (mFeatureLevel >= D3D_FEATURE_LEVEL_10_0)
                {
                    mDevice.GetImmediateContext()->DSSetShaderResources(static_cast<UINT>(0), static_cast<UINT>(opState->mTexturesCount), &opState->mTextures[0]);
                    if (mDevice.isError())
                    {
                        String errorDescription = mDevice.getErrorDescription();
                        OGRE_EXCEPT(Exception::ERR_RENDERINGAPI_ERROR, 
                            "D3D11 device cannot set domain shader resources\nError Description:" + errorDescription,
                            "D3D11RenderSystem::_render");
                    }
                }
            }
        }

        ComPtr<ID3D11Buffer> pSOTarget;
        // Mustn't bind a emulated vertex, pixel shader (see below), if we are rendering to a stream out buffer
        mDevice.GetImmediateContext()->SOGetTargets(1, pSOTarget.GetAddressOf());

        //check consistency of vertex-fragment shaders
        if (!mBoundVertexProgram ||
             (!mBoundFragmentProgram && op.operationType != RenderOperation::OT_POINT_LIST && !pSOTarget ) 
           ) 
        {
            
            OGRE_EXCEPT(Exception::ERR_RENDERINGAPI_ERROR, 
                "Attempted to render to a D3D11 device without both vertex and fragment shaders there is no fixed pipeline in d3d11 - use the RTSS or write custom shaders.",
                "D3D11RenderSystem::_render");
        }

        // Check consistency of tessellation shaders
        if( (mBoundTessellationHullProgram && !mBoundTessellationDomainProgram) ||
            (!mBoundTessellationHullProgram && mBoundTessellationDomainProgram) )
        {
            if (mBoundTessellationHullProgram && !mBoundTessellationDomainProgram) {
            OGRE_EXCEPT(Exception::ERR_RENDERINGAPI_ERROR, 
                "Attempted to use tessellation, but domain shader is missing",
                "D3D11RenderSystem::_render");
            }
            else {
                OGRE_EXCEPT(Exception::ERR_RENDERINGAPI_ERROR, 
                "Attempted to use tessellation, but hull shader is missing",
                "D3D11RenderSystem::_render"); }
        }

        if (mDevice.isError())
        {
            // this will never happen but we want to be consistent with the error checks... 
            String errorDescription = mDevice.getErrorDescription();
            OGRE_EXCEPT(Exception::ERR_RENDERINGAPI_ERROR, 
                "D3D11 device cannot set geometry shader to null\nError Description:" + errorDescription,
                "D3D11RenderSystem::_render");
        }

        // Defer program bind to here because we must bind shader class instances,
        // and this can only be made in SetShader calls.
        // Also, bind shader resources
        if (mBoundVertexProgram)
        {
            mDevice.GetImmediateContext()->VSSetShader(mBoundVertexProgram->getVertexShader(), 
                                                       mClassInstances[GPT_VERTEX_PROGRAM], 
                                                       mNumClassInstances[GPT_VERTEX_PROGRAM]);
            if (mDevice.isError())
            {
                String errorDescription = mDevice.getErrorDescription();
                OGRE_EXCEPT(Exception::ERR_RENDERINGAPI_ERROR, 
                    "D3D11 device cannot set vertex shader\nError Description:" + errorDescription,
                    "D3D11RenderSystem::_render");
            }
        }
        if (mBoundFragmentProgram)
        {
            mDevice.GetImmediateContext()->PSSetShader(mBoundFragmentProgram->getPixelShader(),
                                                       mClassInstances[GPT_FRAGMENT_PROGRAM], 
                                                       mNumClassInstances[GPT_FRAGMENT_PROGRAM]);
            if (mDevice.isError())
            {
                String errorDescription = mDevice.getErrorDescription();
                OGRE_EXCEPT(Exception::ERR_RENDERINGAPI_ERROR, 
                    "D3D11 device cannot set pixel shader\nError Description:" + errorDescription,
                    "D3D11RenderSystem::_render");
            }
        }
        if (mBoundGeometryProgram)
        {
            mDevice.GetImmediateContext()->GSSetShader(mBoundGeometryProgram->getGeometryShader(),
                                                       mClassInstances[GPT_GEOMETRY_PROGRAM], 
                                                       mNumClassInstances[GPT_GEOMETRY_PROGRAM]);
            if (mDevice.isError())
            {
                String errorDescription = mDevice.getErrorDescription();
                OGRE_EXCEPT(Exception::ERR_RENDERINGAPI_ERROR, 
                    "D3D11 device cannot set geometry shader\nError Description:" + errorDescription,
                    "D3D11RenderSystem::_render");
            }
        }
        if (mBoundTessellationHullProgram)
        {
            mDevice.GetImmediateContext()->HSSetShader(mBoundTessellationHullProgram->getHullShader(),
                                                       mClassInstances[GPT_HULL_PROGRAM], 
                                                       mNumClassInstances[GPT_HULL_PROGRAM]);
            if (mDevice.isError())
            {
                String errorDescription = mDevice.getErrorDescription();
                OGRE_EXCEPT(Exception::ERR_RENDERINGAPI_ERROR, 
                    "D3D11 device cannot set hull shader\nError Description:" + errorDescription,
                    "D3D11RenderSystem::_render");
            }
        }
        if (mBoundTessellationDomainProgram)
        {
            mDevice.GetImmediateContext()->DSSetShader(mBoundTessellationDomainProgram->getDomainShader(),
                                                       mClassInstances[GPT_DOMAIN_PROGRAM], 
                                                       mNumClassInstances[GPT_DOMAIN_PROGRAM]);
            if (mDevice.isError())
            {
                String errorDescription = mDevice.getErrorDescription();
                OGRE_EXCEPT(Exception::ERR_RENDERINGAPI_ERROR, 
                    "D3D11 device cannot set domain shader\nError Description:" + errorDescription,
                    "D3D11RenderSystem::_render");
            }
        }
        if (mBoundComputeProgram)
        {
            mDevice.GetImmediateContext()->CSSetShader(mBoundComputeProgram->getComputeShader(),
                                                       mClassInstances[GPT_COMPUTE_PROGRAM], 
                                                       mNumClassInstances[GPT_COMPUTE_PROGRAM]);
            if (mDevice.isError())
            {
                String errorDescription = mDevice.getErrorDescription();
                OGRE_EXCEPT(Exception::ERR_RENDERINGAPI_ERROR, 
                    "D3D11 device cannot set compute shader\nError Description:" + errorDescription,
                    "D3D11RenderSystem::_render");
            }
        }


        setVertexDeclaration(op.vertexData->vertexDeclaration, op.vertexData->vertexBufferBinding);
        setVertexBufferBinding(op.vertexData->vertexBufferBinding);


        // Determine rendering operation
        D3D11_PRIMITIVE_TOPOLOGY primType = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
        DWORD primCount = 0;

        // Handle computing
        if(mBoundComputeProgram)
        {
            // Bound unordered access views
            mDevice.GetImmediateContext()->Dispatch(1, 1, 1);

            ID3D11UnorderedAccessView* views[] = { 0 };
            ID3D11ShaderResourceView* srvs[] = { 0 };
            mDevice.GetImmediateContext()->CSSetShaderResources( 0, 1, srvs );
            mDevice.GetImmediateContext()->CSSetUnorderedAccessViews( 0, 1, views, NULL );
            mDevice.GetImmediateContext()->CSSetShader( NULL, NULL, 0 );

            return;
        }
        else if(mBoundTessellationHullProgram && mBoundTessellationDomainProgram)
        {
            // useful primitives for tessellation
            switch( op.operationType )
            {
            case RenderOperation::OT_LINE_LIST:
                primType = D3D11_PRIMITIVE_TOPOLOGY_2_CONTROL_POINT_PATCHLIST;
                primCount = (DWORD)(op.useIndexes ? op.indexData->indexCount : op.vertexData->vertexCount) / 2;
                break;

            case RenderOperation::OT_LINE_STRIP:
                primType = D3D11_PRIMITIVE_TOPOLOGY_2_CONTROL_POINT_PATCHLIST;
                primCount = (DWORD)(op.useIndexes ? op.indexData->indexCount : op.vertexData->vertexCount) - 1;
                break;

            case RenderOperation::OT_TRIANGLE_LIST:
                primType = D3D11_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST;
                primCount = (DWORD)(op.useIndexes ? op.indexData->indexCount : op.vertexData->vertexCount) / 3;
                break;

            case RenderOperation::OT_TRIANGLE_STRIP:
                primType = D3D11_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST;
                primCount = (DWORD)(op.useIndexes ? op.indexData->indexCount : op.vertexData->vertexCount) - 2;
                break;
            }
        }
        else
        {
            //rendering without tessellation.
            bool useAdjacency = (mGeometryProgramBound && mBoundGeometryProgram && mBoundGeometryProgram->isAdjacencyInfoRequired());
            switch( op.operationType )
            {
            case RenderOperation::OT_POINT_LIST:
                primType = D3D11_PRIMITIVE_TOPOLOGY_POINTLIST;
                primCount = (DWORD)(op.useIndexes ? op.indexData->indexCount : op.vertexData->vertexCount);
                break;

            case RenderOperation::OT_LINE_LIST:
                primType = useAdjacency ? D3D11_PRIMITIVE_TOPOLOGY_LINELIST_ADJ : D3D11_PRIMITIVE_TOPOLOGY_LINELIST;
                primCount = (DWORD)(op.useIndexes ? op.indexData->indexCount : op.vertexData->vertexCount) / 2;
                break;

            case RenderOperation::OT_LINE_STRIP:
                primType = useAdjacency ? D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP_ADJ : D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP;
                primCount = (DWORD)(op.useIndexes ? op.indexData->indexCount : op.vertexData->vertexCount) - 1;
                break;

            case RenderOperation::OT_TRIANGLE_LIST:
                primType = useAdjacency ? D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST_ADJ : D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
                primCount = (DWORD)(op.useIndexes ? op.indexData->indexCount : op.vertexData->vertexCount) / 3;
                break;

            case RenderOperation::OT_TRIANGLE_STRIP:
                primType = useAdjacency ? D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP_ADJ : D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
                primCount = (DWORD)(op.useIndexes ? op.indexData->indexCount : op.vertexData->vertexCount) - 2;
                break;

            case RenderOperation::OT_TRIANGLE_FAN:
                OGRE_EXCEPT(Exception::ERR_RENDERINGAPI_ERROR, "Error - DX11 render - no support for triangle fan (OT_TRIANGLE_FAN)", "D3D11RenderSystem::_render");
                primType = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED; // todo - no TRIANGLE_FAN in DX 11
                primCount = (DWORD)(op.useIndexes ? op.indexData->indexCount : op.vertexData->vertexCount) - 2;
                break;
            }
        }
        
        if (primCount)
        {
            // Issue the op
            //HRESULT hr;
            if( op.useIndexes  )
            {
                D3D11HardwareIndexBuffer* d3dIdxBuf = 
                    static_cast<D3D11HardwareIndexBuffer*>(op.indexData->indexBuffer.get());
                mDevice.GetImmediateContext()->IASetIndexBuffer( d3dIdxBuf->getD3DIndexBuffer(), D3D11Mappings::getFormat(d3dIdxBuf->getType()), 0 );
                if (mDevice.isError())
                {
                    String errorDescription = mDevice.getErrorDescription();
                    OGRE_EXCEPT(Exception::ERR_RENDERINGAPI_ERROR, 
                        "D3D11 device cannot set index buffer\nError Description:" + errorDescription,
                        "D3D11RenderSystem::_render");
                }
            }

            mDevice.GetImmediateContext()->IASetPrimitiveTopology( primType );
            if (mDevice.isError())
            {
                String errorDescription = mDevice.getErrorDescription();
                OGRE_EXCEPT(Exception::ERR_RENDERINGAPI_ERROR, 
                    "D3D11 device cannot set primitive topology\nError Description:" + errorDescription,
                    "D3D11RenderSystem::_render");
            }

            do
            {
                if(op.useIndexes)
                {
                    if(hasInstanceData)
                    {
                        mDevice.GetImmediateContext()->DrawIndexedInstanced(
                            static_cast<UINT>(op.indexData->indexCount), 
                            static_cast<UINT>(numberOfInstances), 
                            static_cast<UINT>(op.indexData->indexStart), 
                            static_cast<INT>(op.vertexData->vertexStart),
                            0);
                    }
                    else
                    {
                        mDevice.GetImmediateContext()->DrawIndexed(
                            static_cast<UINT>(op.indexData->indexCount),
                            static_cast<UINT>(op.indexData->indexStart),
                            static_cast<INT>(op.vertexData->vertexStart));
                    }
                }
                else // non indexed
                {
                    if(op.vertexData->vertexCount == -1) // -1 is a sign to use DrawAuto
                    {
                        mDevice.GetImmediateContext()->DrawAuto();
                    }
                    else if(hasInstanceData)
                    {
                        mDevice.GetImmediateContext()->DrawInstanced(
                            static_cast<UINT>(op.vertexData->vertexCount),
                            static_cast<UINT>(numberOfInstances),
                            static_cast<UINT>(op.vertexData->vertexStart),
                            0);
                    }
                    else
                    {
                        mDevice.GetImmediateContext()->Draw(
                            static_cast<UINT>(op.vertexData->vertexCount),
                            static_cast<UINT>(op.vertexData->vertexStart));
                    }
                }

                if(mDevice.isError())
                {
                    String errorDescription = "D3D11 device cannot draw";
                    if(!op.useIndexes && op.vertexData->vertexCount == -1) // -1 is a sign to use DrawAuto
                        errorDescription.append(" auto");
                    else
                        errorDescription.append(op.useIndexes ? " indexed" : "").append(hasInstanceData ? " instanced" : "");
                    errorDescription.append("\nError Description:").append(mDevice.getErrorDescription());
                    errorDescription.append("\nActive OGRE shaders:")
                        .append(mBoundVertexProgram ? ("\nVS = " + mBoundVertexProgram->getName()).c_str() : "")
                        .append(mBoundTessellationHullProgram ? ("\nHS = " + mBoundTessellationHullProgram->getName()).c_str() : "")
                        .append(mBoundTessellationDomainProgram ? ("\nDS = " + mBoundTessellationDomainProgram->getName()).c_str() : "")
                        .append(mBoundGeometryProgram ? ("\nGS = " + mBoundGeometryProgram->getName()).c_str() : "")
                        .append(mBoundFragmentProgram ? ("\nFS = " + mBoundFragmentProgram->getName()).c_str() : "")
                        .append(mBoundComputeProgram ? ("\nCS = " + mBoundComputeProgram->getName()).c_str() : "");

                    OGRE_EXCEPT(Exception::ERR_RENDERINGAPI_ERROR, errorDescription, "D3D11RenderSystem::_render");
                }

            }while(updatePassIterationRenderState());
        }


        // Crashy : commented this, 99% sure it's useless but really time consuming
        /*if (true) // for now - clear the render state
        {
            mDevice.GetImmediateContext()->OMSetBlendState(0, 0, 0xffffffff); 
            mDevice.GetImmediateContext()->RSSetState(0);
            mDevice.GetImmediateContext()->OMSetDepthStencilState(0, 0); 
//          mDevice->PSSetSamplers(static_cast<UINT>(0), static_cast<UINT>(0), 0);
            
            // Clear class instance storage
            memset(mClassInstances, 0, sizeof(mClassInstances));
            memset(mNumClassInstances, 0, sizeof(mNumClassInstances));      
        }*/

    }
    //---------------------------------------------------------------------
    void D3D11RenderSystem::_renderUsingReadBackAsTexture(unsigned int passNr, Ogre::String variableName, unsigned int StartSlot)
    {
        RenderTarget *target = mActiveRenderTarget;
        switch (passNr)
        {
        case 1:
            if (target)
            {
                ID3D11RenderTargetView * pRTView[OGRE_MAX_MULTIPLE_RENDER_TARGETS];
                memset(pRTView, 0, sizeof(pRTView));

                target->getCustomAttribute( "ID3D11RenderTargetView", &pRTView );

                uint numberOfViews;
                target->getCustomAttribute( "numberOfViews", &numberOfViews );

                //Retrieve depth buffer
                D3D11DepthBuffer *depthBuffer = static_cast<D3D11DepthBuffer*>(target->getDepthBuffer());

                // now switch to the new render target
                mDevice.GetImmediateContext()->OMSetRenderTargets(
                    numberOfViews,
                    pRTView,
                    depthBuffer->getDepthStencilView());

                if (mDevice.isError())
                {
                    String errorDescription = mDevice.getErrorDescription();
                    OGRE_EXCEPT(Exception::ERR_RENDERINGAPI_ERROR, 
                        "D3D11 device cannot set render target\nError Description:" + errorDescription,
                        "D3D11RenderSystem::_renderUsingReadBackAsTexture");
                }
                
                mDevice.GetImmediateContext()->ClearDepthStencilView(depthBuffer->getDepthStencilView(), D3D11_CLEAR_DEPTH, 1.0f, 0);

                float ClearColor[4];
                //D3D11Mappings::get(colour, ClearColor);
                // Clear all views
                mActiveRenderTarget->getCustomAttribute( "numberOfViews", &numberOfViews );
                if( numberOfViews == 1 )
                    mDevice.GetImmediateContext()->ClearRenderTargetView( pRTView[0], ClearColor );
                else
                {
                    for( uint i = 0; i < numberOfViews; ++i )
                        mDevice.GetImmediateContext()->ClearRenderTargetView( pRTView[i], ClearColor );
                }

            }
            break;
        case 2:
            if (target)
            {
                //
                // We need to remove the the DST from the Render Targets if we want to use it as a texture :
                //
                ID3D11RenderTargetView * pRTView[OGRE_MAX_MULTIPLE_RENDER_TARGETS];
                memset(pRTView, 0, sizeof(pRTView));

                target->getCustomAttribute( "ID3D11RenderTargetView", &pRTView );

                uint numberOfViews;
                target->getCustomAttribute( "numberOfViews", &numberOfViews );

                //Retrieve depth buffer
                D3D11DepthBuffer *depthBuffer = static_cast<D3D11DepthBuffer*>(target->getDepthBuffer());

                // now switch to the new render target
                mDevice.GetImmediateContext()->OMSetRenderTargets(
                    numberOfViews,
                    pRTView,
                    NULL);

                mDevice.GetImmediateContext()->PSSetShaderResources(static_cast<UINT>(StartSlot), 1, mDSTResView.GetAddressOf());
                if (mDevice.isError())
                {
                    String errorDescription = mDevice.getErrorDescription();
                    OGRE_EXCEPT(Exception::ERR_RENDERINGAPI_ERROR, 
                        "D3D11 device cannot set pixel shader resources\nError Description:" + errorDescription,
                        "D3D11RenderSystem::_renderUsingReadBackAsTexture");
                }

            }
            break;
        case 3:
            //
            // We need to unbind mDSTResView from the given variable because this buffer
            // will be used later as the typical depth buffer, again
            // must call Apply(0) here : to flush SetResource(NULL)
            //
            
            if (target)
            {
                uint numberOfViews;
                target->getCustomAttribute( "numberOfViews", &numberOfViews );

                mDevice.GetImmediateContext()->PSSetShaderResources(static_cast<UINT>(StartSlot), static_cast<UINT>(numberOfViews), NULL);
                    if (mDevice.isError())
                    {
                        String errorDescription = mDevice.getErrorDescription();
                        OGRE_EXCEPT(Exception::ERR_RENDERINGAPI_ERROR, 
                            "D3D11 device cannot set pixel shader resources\nError Description:" + errorDescription,
                            "D3D11RenderSystem::_renderUsingReadBackAsTexture");
                    }           
            }

            break;
        }
    }
    //---------------------------------------------------------------------
    void D3D11RenderSystem::bindGpuProgram(GpuProgram* prg)
    {
        if (!prg)
        {
            OGRE_EXCEPT(Exception::ERR_RENDERINGAPI_ERROR, 
                "Null program bound.",
                "D3D11RenderSystem::bindGpuProgram");
        }

        switch (prg->getType())
        {
        case GPT_VERTEX_PROGRAM:
            {
                // get the shader
                mBoundVertexProgram = static_cast<D3D11HLSLProgram*>(prg);
/*              ID3D11VertexShader * vsShaderToSet = mBoundVertexProgram->getVertexShader();

                // set the shader
                mDevice.GetImmediateContext()->VSSetShader(vsShaderToSet, NULL, 0);
                if (mDevice.isError())
                {
                    String errorDescription = mDevice.getErrorDescription();
                    OGRE_EXCEPT(Exception::ERR_RENDERINGAPI_ERROR, 
                        "D3D11 device cannot set vertex shader\nError Description:" + errorDescription,
                        "D3D11RenderSystem::bindGpuProgram");
                }*/     
            }
            break;
        case GPT_FRAGMENT_PROGRAM:
            {
                mBoundFragmentProgram = static_cast<D3D11HLSLProgram*>(prg);
/*              ID3D11PixelShader* psShaderToSet = mBoundFragmentProgram->getPixelShader();

                mDevice.GetImmediateContext()->PSSetShader(psShaderToSet, NULL, 0);
                if (mDevice.isError())
                {
                    String errorDescription = mDevice.getErrorDescription();
                    OGRE_EXCEPT(Exception::ERR_RENDERINGAPI_ERROR, 
                        "D3D11 device cannot set fragment shader\nError Description:" + errorDescription,
                        "D3D11RenderSystem::bindGpuProgram");
                }*/     
            }
            break;
        case GPT_GEOMETRY_PROGRAM:
            {
                mBoundGeometryProgram = static_cast<D3D11HLSLProgram*>(prg);
/*              ID3D11GeometryShader* gsShaderToSet = mBoundGeometryProgram->getGeometryShader();

                mDevice.GetImmediateContext()->GSSetShader(gsShaderToSet, NULL, 0);
                if (mDevice.isError())
                {
                    String errorDescription = mDevice.getErrorDescription();
                    OGRE_EXCEPT(Exception::ERR_RENDERINGAPI_ERROR, 
                        "D3D11 device cannot set geometry shader\nError Description:" + errorDescription,
                        "D3D11RenderSystem::bindGpuProgram");
                }*/     

            }
            break;
        case GPT_HULL_PROGRAM:
            {
                mBoundTessellationHullProgram = static_cast<D3D11HLSLProgram*>(prg);
/*              ID3D11HullShader* gsShaderToSet = mBoundTessellationHullProgram->getHullShader();

                mDevice.GetImmediateContext()->HSSetShader(gsShaderToSet, NULL, 0);
                if (mDevice.isError())
                {
                    String errorDescription = mDevice.getErrorDescription();
                    OGRE_EXCEPT(Exception::ERR_RENDERINGAPI_ERROR, 
                        "D3D11 device cannot set hull shader\nError Description:" + errorDescription,
                        "D3D11RenderSystem::bindGpuProgram");
                }       */

            }
            break;
        case GPT_DOMAIN_PROGRAM:
            {
                mBoundTessellationDomainProgram = static_cast<D3D11HLSLProgram*>(prg);
/*              ID3D11DomainShader* gsShaderToSet = mBoundTessellationDomainProgram->getDomainShader();

                mDevice.GetImmediateContext()->DSSetShader(gsShaderToSet, NULL, 0);
                if (mDevice.isError())
                {
                    String errorDescription = mDevice.getErrorDescription();
                    OGRE_EXCEPT(Exception::ERR_RENDERINGAPI_ERROR, 
                        "D3D11 device cannot set domain shader\nError Description:" + errorDescription,
                        "D3D11RenderSystem::bindGpuProgram");
                }*/     

            }
            break;
        case GPT_COMPUTE_PROGRAM:
            {
                mBoundComputeProgram = static_cast<D3D11HLSLProgram*>(prg);
/*              ID3D11ComputeShader* gsShaderToSet = mBoundComputeProgram->getComputeShader();

                mDevice.GetImmediateContext()->CSSetShader(gsShaderToSet, NULL, 0);
                if (mDevice.isError())
                {
                    String errorDescription = mDevice.getErrorDescription();
                    OGRE_EXCEPT(Exception::ERR_RENDERINGAPI_ERROR, 
                        "D3D11 device cannot set compute shader\nError Description:" + errorDescription,
                        "D3D11RenderSystem::bindGpuProgram");
                }*/     

            }
            break;
        };

        RenderSystem::bindGpuProgram(prg);
   }
    //---------------------------------------------------------------------
    void D3D11RenderSystem::unbindGpuProgram(GpuProgramType gptype)
    {

        switch(gptype)
        {
        case GPT_VERTEX_PROGRAM:
            {
                mActiveVertexGpuProgramParameters.reset();
                mBoundVertexProgram = NULL;
                //mDevice->VSSetShader(NULL);
                mDevice.GetImmediateContext()->VSSetShader(NULL, NULL, 0);
            }
            break;
        case GPT_FRAGMENT_PROGRAM:
            {
                mActiveFragmentGpuProgramParameters.reset();
                mBoundFragmentProgram = NULL;
                //mDevice->PSSetShader(NULL);
                mDevice.GetImmediateContext()->PSSetShader(NULL, NULL, 0);
            }

            break;
        case GPT_GEOMETRY_PROGRAM:
            {
                mActiveGeometryGpuProgramParameters.reset();
                mBoundGeometryProgram = NULL;
                mDevice.GetImmediateContext()->GSSetShader( NULL, NULL, 0 );
            }
            break;
        case GPT_HULL_PROGRAM:
            {
                mActiveTessellationHullGpuProgramParameters.reset();
                mBoundTessellationHullProgram = NULL;
                mDevice.GetImmediateContext()->HSSetShader( NULL, NULL, 0 );
            }
            break;
        case GPT_DOMAIN_PROGRAM:
            {
                mActiveTessellationDomainGpuProgramParameters.reset();
                mBoundTessellationDomainProgram = NULL;
                mDevice.GetImmediateContext()->DSSetShader( NULL, NULL, 0 );
            }
            break;
        case GPT_COMPUTE_PROGRAM:
            {
                mActiveComputeGpuProgramParameters.reset();
                mBoundComputeProgram = NULL;
                mDevice.GetImmediateContext()->CSSetShader( NULL, NULL, 0 );
            }
            break;
        default:
            assert(false && "Undefined Program Type!");
        };
        RenderSystem::unbindGpuProgram(gptype);
    }
    //---------------------------------------------------------------------
    void D3D11RenderSystem::bindGpuProgramParameters(GpuProgramType gptype, GpuProgramParametersSharedPtr params, uint16 mask)
    {
        if (mask & (uint16)GPV_GLOBAL)
        {
            // TODO: Dx11 supports shared constant buffers, so use them
            // check the match to constant buffers & use rendersystem data hooks to store
            // for now, just copy
            params->_copySharedParams();
        }

        // Do everything here in Dx11, since deal with via buffers anyway so number of calls
        // is actually the same whether we categorise the updates or not
        ID3D11Buffer* pBuffers[1] ;
        switch(gptype)
        {
        case GPT_VERTEX_PROGRAM:
            {
                //  if (params->getAutoConstantCount() > 0)
                //{
                if (mBoundVertexProgram)
                {
                    pBuffers[0] = mBoundVertexProgram->getConstantBuffer(params, mask);
                    mDevice.GetImmediateContext()->VSSetConstantBuffers( 0, 1, pBuffers );
                    if (mDevice.isError())
                    {
                        String errorDescription = mDevice.getErrorDescription();
                        OGRE_EXCEPT(Exception::ERR_RENDERINGAPI_ERROR, 
                            "D3D11 device cannot set vertex shader constant buffers\nError Description:" + errorDescription,
                            "D3D11RenderSystem::bindGpuProgramParameters");
                    }       

                }
            }
            break;
        case GPT_FRAGMENT_PROGRAM:
            {
                //if (params->getAutoConstantCount() > 0)
                //{
                if (mBoundFragmentProgram)
                {
                    pBuffers[0] = mBoundFragmentProgram->getConstantBuffer(params, mask);
                    mDevice.GetImmediateContext()->PSSetConstantBuffers( 0, 1, pBuffers );
                    if (mDevice.isError())
                    {
                        String errorDescription = mDevice.getErrorDescription();
                        OGRE_EXCEPT(Exception::ERR_RENDERINGAPI_ERROR, 
                            "D3D11 device cannot set fragment shader constant buffers\nError Description:" + errorDescription,
                            "D3D11RenderSystem::bindGpuProgramParameters");
                    }       

                }
            }
            break;
        case GPT_GEOMETRY_PROGRAM:
            {
                if (mBoundGeometryProgram)
                {
                    pBuffers[0] = mBoundGeometryProgram->getConstantBuffer(params, mask);
                    mDevice.GetImmediateContext()->GSSetConstantBuffers( 0, 1, pBuffers );
                    if (mDevice.isError())
                    {
                        String errorDescription = mDevice.getErrorDescription();
                        OGRE_EXCEPT(Exception::ERR_RENDERINGAPI_ERROR, 
                            "D3D11 device cannot set Geometry shader constant buffers\nError Description:" + errorDescription,
                            "D3D11RenderSystem::bindGpuProgramParameters");
                    }       

                }
            }
            break;
        case GPT_HULL_PROGRAM:
            {
                if (mBoundTessellationHullProgram)
                {
                    pBuffers[0] = mBoundTessellationHullProgram->getConstantBuffer(params, mask);
                    mDevice.GetImmediateContext()->HSSetConstantBuffers( 0, 1, pBuffers );
                    if (mDevice.isError())
                    {
                        String errorDescription = mDevice.getErrorDescription();
                        OGRE_EXCEPT(Exception::ERR_RENDERINGAPI_ERROR, 
                            "D3D11 device cannot set Hull shader constant buffers\nError Description:" + errorDescription,
                            "D3D11RenderSystem::bindGpuProgramParameters");
                    }       

                }
            }
            break;
        case GPT_DOMAIN_PROGRAM:
            {
                if (mBoundTessellationDomainProgram)
                {
                    pBuffers[0] = mBoundTessellationDomainProgram->getConstantBuffer(params, mask);
                    mDevice.GetImmediateContext()->DSSetConstantBuffers( 0, 1, pBuffers );
                    if (mDevice.isError())
                    {
                        String errorDescription = mDevice.getErrorDescription();
                        OGRE_EXCEPT(Exception::ERR_RENDERINGAPI_ERROR, 
                            "D3D11 device cannot set Domain shader constant buffers\nError Description:" + errorDescription,
                            "D3D11RenderSystem::bindGpuProgramParameters");
                    }       

                }
            }
            break;
        case GPT_COMPUTE_PROGRAM:
            {
                if (mBoundComputeProgram)
                {
                    pBuffers[0] = mBoundComputeProgram->getConstantBuffer(params, mask);
                    mDevice.GetImmediateContext()->CSSetConstantBuffers( 0, 1, pBuffers );
                    if (mDevice.isError())
                    {
                        String errorDescription = mDevice.getErrorDescription();
                        OGRE_EXCEPT(Exception::ERR_RENDERINGAPI_ERROR, 
                            "D3D11 device cannot set Compute shader constant buffers\nError Description:" + errorDescription,
                            "D3D11RenderSystem::bindGpuProgramParameters");
                    }       

                }
            }
            break;
        };

        // Now, set class instances
        const GpuProgramParameters::SubroutineMap& subroutineMap = params->getSubroutineMap();
        if (subroutineMap.empty())
            return;

        GpuProgramParameters::SubroutineIterator it;
        GpuProgramParameters::SubroutineIterator end = subroutineMap.end();
        for(it = subroutineMap.begin(); it != end; ++it)
        {
            setSubroutine(gptype, it->first, it->second);
        }
    }
    //---------------------------------------------------------------------
    void D3D11RenderSystem::bindGpuProgramPassIterationParameters(GpuProgramType gptype)
    {

        switch(gptype)
        {
        case GPT_VERTEX_PROGRAM:
            bindGpuProgramParameters(gptype, mActiveVertexGpuProgramParameters, (uint16)GPV_PASS_ITERATION_NUMBER);
            break;

        case GPT_FRAGMENT_PROGRAM:
            bindGpuProgramParameters(gptype, mActiveFragmentGpuProgramParameters, (uint16)GPV_PASS_ITERATION_NUMBER);
            break;
        case GPT_GEOMETRY_PROGRAM:
            bindGpuProgramParameters(gptype, mActiveGeometryGpuProgramParameters, (uint16)GPV_PASS_ITERATION_NUMBER);
            break;
        case GPT_HULL_PROGRAM:
            bindGpuProgramParameters(gptype, mActiveTessellationHullGpuProgramParameters, (uint16)GPV_PASS_ITERATION_NUMBER);
            break;
        case GPT_DOMAIN_PROGRAM:
            bindGpuProgramParameters(gptype, mActiveTessellationDomainGpuProgramParameters, (uint16)GPV_PASS_ITERATION_NUMBER);
            break;
        case GPT_COMPUTE_PROGRAM:
            bindGpuProgramParameters(gptype, mActiveComputeGpuProgramParameters, (uint16)GPV_PASS_ITERATION_NUMBER);
            break;
        }
    }
    //---------------------------------------------------------------------
    void D3D11RenderSystem::setSubroutine(GpuProgramType gptype, unsigned int slotIndex, const String& subroutineName)
    {
        ID3D11ClassInstance* instance = 0;
        
        ClassInstanceIterator it = mInstanceMap.find(subroutineName);
        if (it == mInstanceMap.end())
        {
            // try to get instance already created (must have at least one field)
            HRESULT hr = mDevice.GetClassLinkage()->GetClassInstance(subroutineName.c_str(), 0, &instance);
            if (FAILED(hr) || instance == 0)
            {
                // probably class don't have a field, try create a new
                hr = mDevice.GetClassLinkage()->CreateClassInstance(subroutineName.c_str(), 0, 0, 0, 0, &instance);
                if (FAILED(hr) || instance == 0)
                {
					OGRE_EXCEPT_EX(Exception::ERR_RENDERINGAPI_ERROR, hr,
						"Shader subroutine with name " + subroutineName + " doesn't exist.",
						"D3D11RenderSystem::setSubroutineName");
                }
            }

            // Store class instance
            mInstanceMap.insert(std::make_pair(subroutineName, instance));
        }
        else
        {
            instance = it->second;
        }
        
        // If already created, store class instance
        mClassInstances[gptype][slotIndex] = instance;
        mNumClassInstances[gptype] = mNumClassInstances[gptype] + 1;
    }
    //---------------------------------------------------------------------
    void D3D11RenderSystem::setSubroutine(GpuProgramType gptype, const String& slotName, const String& subroutineName)
    {
        unsigned int slotIdx = 0;
        switch(gptype)
        {
        case GPT_VERTEX_PROGRAM:
            {
                if (mBoundVertexProgram)
                {
                    slotIdx = mBoundVertexProgram->getSubroutineSlot(slotName);
                }
            }
            break;
        case GPT_FRAGMENT_PROGRAM:
            {
                if (mBoundFragmentProgram)
                {
                    slotIdx = mBoundFragmentProgram->getSubroutineSlot(slotName);
                }
            }
            break;
        case GPT_GEOMETRY_PROGRAM:
            {
                if (mBoundGeometryProgram)
                {
                    slotIdx = mBoundGeometryProgram->getSubroutineSlot(slotName);
                }
            }
            break;
        case GPT_HULL_PROGRAM:
            {
                if (mBoundTessellationHullProgram)
                {
                    slotIdx = mBoundTessellationHullProgram->getSubroutineSlot(slotName);
                }
            }
            break;
        case GPT_DOMAIN_PROGRAM:
            {
                if (mBoundTessellationDomainProgram)
                {
                    slotIdx = mBoundTessellationDomainProgram->getSubroutineSlot(slotName);
                }
            }
            break;
        case GPT_COMPUTE_PROGRAM:
            {
                if (mBoundComputeProgram)
                {
                    slotIdx = mBoundComputeProgram->getSubroutineSlot(slotName);
                }
            }
            break;
        };
        
        // Set subroutine for slot
        setSubroutine(gptype, slotIdx, subroutineName);
    }
    //---------------------------------------------------------------------
    void D3D11RenderSystem::setClipPlanesImpl(const PlaneList& clipPlanes)
    {
    }
    //---------------------------------------------------------------------
    void D3D11RenderSystem::setScissorTest(bool enabled, size_t left, size_t top, size_t right,
        size_t bottom)
    {
        mRasterizerDesc.ScissorEnable = enabled;
        mScissorRect.left = static_cast<LONG>(left);
        mScissorRect.top = static_cast<LONG>(top);
        mScissorRect.right = static_cast<LONG>(right);
        mScissorRect.bottom =static_cast<LONG>( bottom);

        mDevice.GetImmediateContext()->RSSetScissorRects(1, &mScissorRect);
        if (mDevice.isError())
        {
            String errorDescription = mDevice.getErrorDescription();
            OGRE_EXCEPT(Exception::ERR_RENDERINGAPI_ERROR, 
                "D3D11 device cannot set scissor rects\nError Description:" + errorDescription,
                "D3D11RenderSystem::setScissorTest");
        }   
        mRasterizerDescChanged=true;
    }
    //---------------------------------------------------------------------
    void D3D11RenderSystem::clearFrameBuffer(unsigned int buffers, 
        const ColourValue& colour, Real depth, unsigned short stencil)
    {
        if (mActiveRenderTarget)
        {
            ID3D11RenderTargetView * pRTView[OGRE_MAX_MULTIPLE_RENDER_TARGETS];
            memset(pRTView, 0, sizeof(pRTView));

            mActiveRenderTarget->getCustomAttribute( "ID3D11RenderTargetView", &pRTView );
            
            if (buffers & FBT_COLOUR)
            {
                float ClearColor[4];
                D3D11Mappings::get(colour, ClearColor);

                // Clear all views
                uint numberOfViews;
                mActiveRenderTarget->getCustomAttribute( "numberOfViews", &numberOfViews );
                if( numberOfViews == 1 )
                    mDevice.GetImmediateContext()->ClearRenderTargetView( pRTView[0], ClearColor );
                else
                {
                    for( uint i = 0; i < numberOfViews; ++i )
                        mDevice.GetImmediateContext()->ClearRenderTargetView( pRTView[i], ClearColor );
                }

            }
            UINT ClearFlags = 0;
            if (buffers & FBT_DEPTH)
            {
                ClearFlags |= D3D11_CLEAR_DEPTH;
            }
            if (buffers & FBT_STENCIL)
            {
                ClearFlags |= D3D11_CLEAR_STENCIL;
            }

            if (ClearFlags)
            {
                D3D11DepthBuffer *depthBuffer = static_cast<D3D11DepthBuffer*>(mActiveRenderTarget->
                                                                                        getDepthBuffer());
                if( depthBuffer )
                {
                    mDevice.GetImmediateContext()->ClearDepthStencilView(
                                                        depthBuffer->getDepthStencilView(),
                                                        ClearFlags, depth, static_cast<UINT8>(stencil) );
                }
            }
        }
    }
    //---------------------------------------------------------------------
    void D3D11RenderSystem::_makeProjectionMatrix(Real left, Real right, 
        Real bottom, Real top, Real nearPlane, Real farPlane, Matrix4& dest,
        bool forGpuProgram)
    {
        // Correct position for off-axis projection matrix
        if (!forGpuProgram)
        {
            Real offsetX = left + right;
            Real offsetY = top + bottom;

            left -= offsetX;
            right -= offsetX;
            top -= offsetY;
            bottom -= offsetY;
        }

        Real width = right - left;
        Real height = top - bottom;
        Real q, qn;
        if (farPlane == 0)
        {
            q = 1 - Frustum::INFINITE_FAR_PLANE_ADJUST;
            qn = nearPlane * (Frustum::INFINITE_FAR_PLANE_ADJUST - 1);
        }
        else
        {
            q = farPlane / ( farPlane - nearPlane );
            qn = -q * nearPlane;
        }
        dest = Matrix4::ZERO;
        dest[0][0] = 2 * nearPlane / width;
        dest[0][2] = (right+left) / width;
        dest[1][1] = 2 * nearPlane / height;
        dest[1][2] = (top+bottom) / height;
        if (forGpuProgram)
        {
            dest[2][2] = -q;
            dest[3][2] = -1.0f;
        }
        else
        {
            dest[2][2] = q;
            dest[3][2] = 1.0f;
        }
        dest[2][3] = qn;
    }

    //---------------------------------------------------------------------
    HardwareOcclusionQuery* D3D11RenderSystem::createHardwareOcclusionQuery(void)
    {
        D3D11HardwareOcclusionQuery* ret = new D3D11HardwareOcclusionQuery (mDevice); 
        mHwOcclusionQueries.push_back(ret);
        return ret;
    }
    //---------------------------------------------------------------------
    Real D3D11RenderSystem::getHorizontalTexelOffset(void)
    {
        // D3D11 is now like GL
        return 0.0f;
    }
    //---------------------------------------------------------------------
    Real D3D11RenderSystem::getVerticalTexelOffset(void)
    {
        // D3D11 is now like GL
        return 0.0f;
    }
    //---------------------------------------------------------------------
    void D3D11RenderSystem::_applyObliqueDepthProjection(Matrix4& matrix, const Plane& plane, 
        bool forGpuProgram)
    {
        // Thanks to Eric Lenyel for posting this calculation at www.terathon.com

        // Calculate the clip-space corner point opposite the clipping plane
        // as (sgn(clipPlane.x), sgn(clipPlane.y), 1, 1) and
        // transform it into camera space by multiplying it
        // by the inverse of the projection matrix

        /* generalised version
        Vector4 q = matrix.inverse() * 
            Vector4(Math::Sign(plane.normal.x), Math::Sign(plane.normal.y), 1.0f, 1.0f);
        */
        Vector4 q;
        q.x = Math::Sign(plane.normal.x) / matrix[0][0];
        q.y = Math::Sign(plane.normal.y) / matrix[1][1];
        q.z = 1.0F; 
        // flip the next bit from Lengyel since we're right-handed
        if (forGpuProgram)
        {
            q.w = (1.0F - matrix[2][2]) / matrix[2][3];
        }
        else
        {
            q.w = (1.0F + matrix[2][2]) / matrix[2][3];
        }

        // Calculate the scaled plane vector
        Vector4 clipPlane4d(plane.normal.x, plane.normal.y, plane.normal.z, plane.d);
        Vector4 c = clipPlane4d * (1.0F / (clipPlane4d.dotProduct(q)));

        // Replace the third row of the projection matrix
        matrix[2][0] = c.x;
        matrix[2][1] = c.y;
        // flip the next bit from Lengyel since we're right-handed
        if (forGpuProgram)
        {
            matrix[2][2] = c.z; 
        }
        else
        {
            matrix[2][2] = -c.z; 
        }
        matrix[2][3] = c.w;        
    }
    //---------------------------------------------------------------------
    Real D3D11RenderSystem::getMinimumDepthInputValue(void)
    {
        // Range [0.0f, 1.0f]
        return 0.0f;
    }
    //---------------------------------------------------------------------
    Real D3D11RenderSystem::getMaximumDepthInputValue(void)
    {
        // Range [0.0f, 1.0f]
        // D3D inverts even identity view matrices, so maximum INPUT is -1.0
        return -1.0f;
    }
    //---------------------------------------------------------------------
    void D3D11RenderSystem::registerThread()
    {
        // nothing to do - D3D11 shares rendering context already
    }
    //---------------------------------------------------------------------
    void D3D11RenderSystem::unregisterThread()
    {
        // nothing to do - D3D11 shares rendering context already
    }
    //---------------------------------------------------------------------
    void D3D11RenderSystem::preExtraThreadsStarted()
    {
        // nothing to do - D3D11 shares rendering context already
    }
    //---------------------------------------------------------------------
    void D3D11RenderSystem::postExtraThreadsStarted()
    {
        // nothing to do - D3D11 shares rendering context already
    }
    //---------------------------------------------------------------------
    void D3D11RenderSystem::determineFSAASettings(uint fsaa, const String& fsaaHint, 
        DXGI_FORMAT format, DXGI_SAMPLE_DESC* outFSAASettings)
    {
        bool qualityHint = fsaa >= 8 && fsaaHint.find("Quality") != String::npos;
        
        // NVIDIA, AMD - prefer CSAA aka EQAA if available.
        // see http://developer.nvidia.com/object/coverage-sampled-aa.html
        // see http://developer.amd.com/wordpress/media/2012/10/EQAA%20Modes%20for%20AMD%20HD%206900%20Series%20Cards.pdf

        // Modes are sorted from high quality to low quality, CSAA aka EQAA are listed first
        // Note, that max(Count, Quality) == FSAA level and (Count >= 8 && Quality != 0) == quality hint
        DXGI_SAMPLE_DESC presets[] = {
                { 8, 16 }, // CSAA 16xQ, EQAA 8f16x
                { 4, 16 }, // CSAA 16x,  EQAA 4f16x
                { 16, 0 }, // MSAA 16x

                { 12, 0 }, // MSAA 12x

                { 8, 8 },  // CSAA 8xQ
                { 4, 8 },  // CSAA 8x,  EQAA 4f8x
                { 8, 0 },  // MSAA 8x

                { 6, 0 },  // MSAA 6x
                { 4, 0 },  // MSAA 4x
                { 2, 0 },  // MSAA 2x
                { 1, 0 },  // MSAA 1x
                { NULL },
        };

        // Skip too HQ modes
        DXGI_SAMPLE_DESC* mode = presets;
        for(; mode->Count != 0; ++mode)
        {
            unsigned modeFSAA = std::max(mode->Count, mode->Quality);
            bool modeQuality = mode->Count >= 8 && mode->Quality != 0;
            bool tooHQ = (modeFSAA > fsaa || modeFSAA == fsaa && modeQuality && !qualityHint);
            if(!tooHQ)
                break;
        }

        // Use first supported mode
        for(; mode->Count != 0; ++mode)
        {
            UINT outQuality;
            HRESULT hr = mDevice->CheckMultisampleQualityLevels(format, mode->Count, &outQuality);

            if(SUCCEEDED(hr) && outQuality > mode->Quality)
            {
                *outFSAASettings = *mode;
                return;
            }
        }

        outFSAASettings->Count = 1;
        outFSAASettings->Quality = 0;
    }
    //---------------------------------------------------------------------
    void D3D11RenderSystem::initRenderSystem()
    {
        if (mRenderSystemWasInited)
        {
            return;
        }

        mRenderSystemWasInited = true;
        // set pointers to NULL
        mDriverList = NULL;
        mTextureManager = NULL;
        mHardwareBufferManager = NULL;
        mGpuProgramManager = NULL;
        mPrimaryWindow = NULL;
        mMinRequestedFeatureLevel = D3D_FEATURE_LEVEL_9_1;
#if __OGRE_WINRT_PHONE // Windows Phone support only FL 9.3, but simulator can create much more capable device, so restrict it artificially here
        mMaxRequestedFeatureLevel = D3D_FEATURE_LEVEL_9_3;
#elif defined(_WIN32_WINNT_WIN8) && _WIN32_WINNT >= _WIN32_WINNT_WIN8
        mMaxRequestedFeatureLevel = D3D_FEATURE_LEVEL_11_1;
#else
        mMaxRequestedFeatureLevel = D3D_FEATURE_LEVEL_11_0;
#endif
        mUseNVPerfHUD = false;
        mHLSLProgramFactory = NULL;

#if OGRE_NO_QUAD_BUFFER_STEREO == 0
		OGRE_DELETE mStereoDriver;
		mStereoDriver = NULL;
#endif

        mBoundVertexProgram = NULL;
        mBoundFragmentProgram = NULL;
        mBoundGeometryProgram = NULL;
        mBoundTessellationHullProgram = NULL;
        mBoundTessellationDomainProgram = NULL;
        mBoundComputeProgram = NULL;

        mBindingType = TextureUnitState::BT_FRAGMENT;

        ZeroMemory( &mBlendDesc, sizeof(mBlendDesc));

        ZeroMemory( &mRasterizerDesc, sizeof(mRasterizerDesc));
        mRasterizerDesc.FrontCounterClockwise = true;
		mRasterizerDesc.DepthClipEnable = true;
        mRasterizerDesc.MultisampleEnable = true;


        ZeroMemory( &mDepthStencilDesc, sizeof(mDepthStencilDesc));

        ZeroMemory( &mDepthStencilDesc, sizeof(mDepthStencilDesc));
        ZeroMemory( &mScissorRect, sizeof(mScissorRect));

        // set filters to defaults
        for (size_t n = 0; n < OGRE_MAX_TEXTURE_LAYERS; n++)
        {
            FilterMinification[n] = FO_NONE;
            FilterMagnification[n] = FO_NONE;
            FilterMips[n] = FO_NONE;
        }

        mPolygonMode = PM_SOLID;
        mRasterizerDesc.FillMode = D3D11Mappings::get(mPolygonMode);

        //sets the modification trackers to true
        mBlendDescChanged = true;
		mRasterizerDescChanged = true;
		mDepthStencilDescChanged = true;
		mSamplerStatesChanged = true;
		mLastTextureUnitState = 0;

        ZeroMemory(mTexStageDesc, OGRE_MAX_TEXTURE_LAYERS * sizeof(sD3DTextureStageDesc));

        mLastVertexSourceCount = 0;
        mReadBackAsTexture = false;

        ID3D11DeviceN * device = createD3D11Device(NULL, D3D_DRIVER_TYPE_HARDWARE, mMinRequestedFeatureLevel, mMaxRequestedFeatureLevel, 0);
        mDevice.TransferOwnership(device);
    }
    //---------------------------------------------------------------------
    void D3D11RenderSystem::getCustomAttribute(const String& name, void* pData)
    {
        if( name == "D3DDEVICE" )
        {
            *(ID3D11DeviceN**)pData = mDevice.get();
        }
        else
        {
            OGRE_EXCEPT(Exception::ERR_INVALIDPARAMS, "Attribute not found: " + name, "RenderSystem::getCustomAttribute");
        }
    }
    //---------------------------------------------------------------------
    bool D3D11RenderSystem::_getDepthBufferCheckEnabled( void )
    {
        return mDepthStencilDesc.DepthEnable == TRUE;
    }
    //---------------------------------------------------------------------
    D3D11HLSLProgram* D3D11RenderSystem::_getBoundVertexProgram() const
    {
        return mBoundVertexProgram;
    }
    //---------------------------------------------------------------------
    D3D11HLSLProgram* D3D11RenderSystem::_getBoundFragmentProgram() const
    {
        return mBoundFragmentProgram;
    }
    //---------------------------------------------------------------------
    D3D11HLSLProgram* D3D11RenderSystem::_getBoundGeometryProgram() const
    {
        return mBoundGeometryProgram;
    }
    //---------------------------------------------------------------------
    D3D11HLSLProgram* D3D11RenderSystem::_getBoundTessellationHullProgram() const
    {
        return mBoundTessellationHullProgram;
    }
    //---------------------------------------------------------------------
    D3D11HLSLProgram* D3D11RenderSystem::_getBoundTessellationDomainProgram() const
    {
        return mBoundTessellationDomainProgram;
    }
    //---------------------------------------------------------------------
    D3D11HLSLProgram* D3D11RenderSystem::_getBoundComputeProgram() const
    {
        return mBoundComputeProgram;
    }
	//---------------------------------------------------------------------
	bool D3D11RenderSystem::setDrawBuffer(ColourBufferType colourBuffer)
	{
#if OGRE_NO_QUAD_BUFFER_STEREO == 0
		return D3D11StereoDriverBridge::getSingleton().setDrawBuffer(colourBuffer);
#else
		return false;
#endif
	}
    //---------------------------------------------------------------------
    void D3D11RenderSystem::beginProfileEvent( const String &eventName )
    {
#if OGRE_D3D11_PROFILING
        if(mDevice.GetProfiler())
        {			
            wchar_t wideName[256]; // Let avoid heap memory allocation if we are in profiling code.
            bool wideNameOk = !eventName.empty() && 0 != MultiByteToWideChar(CP_ACP, 0, eventName.data(), eventName.length() + 1, wideName, ARRAYSIZE(wideName));
            mDevice.GetProfiler()->BeginEvent(wideNameOk ? wideName : L"<too long or empty event name>");
        }
#endif
    }
    //---------------------------------------------------------------------
    void D3D11RenderSystem::endProfileEvent( void )
    {
#if OGRE_D3D11_PROFILING
        if(mDevice.GetProfiler())
            mDevice.GetProfiler()->EndEvent();
#endif
    }
    //---------------------------------------------------------------------
    void D3D11RenderSystem::markProfileEvent( const String &eventName )
    {
#if OGRE_D3D11_PROFILING
        if(mDevice.GetProfiler())
        {
            wchar_t wideName[256]; // Let avoid heap memory allocation if we are in profiling code.
            bool wideNameOk = !eventName.empty() && 0 != MultiByteToWideChar(CP_ACP, 0, eventName.data(), eventName.length() + 1, wideName, ARRAYSIZE(wideName));
            mDevice.GetProfiler()->SetMarker(wideNameOk ? wideName : L"<too long or empty event name>");
        }
#endif
    }
}
